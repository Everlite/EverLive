/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_rx_eq.c — HDMI 2.0 receiver equalisation for the GC573
 *
 * At 4K60 RGB (~594 MHz pixel clock, 5.94 Gbit/s per TMDS lane) the fixed
 * power-on equaliser setting (RS 0x1f, forced) is not always good enough:
 * the receiver still locks, but a few TMDS characters per frame decode
 * wrongly.  That shows up as sparse coloured speckles in the picture and as
 * crackle in the audio, which travels in the same data islands.
 *
 * Both GC573 receivers (IT6664 input, IT6805 capture) use the same ITE
 * receiver core, so one implementation drives either.  It provides:
 *   - per-channel TMDS character-error (CED) counters, blocking and
 *     non-blocking, so link quality can be measured and logged;
 *   - a tuning pass for HDMI 2.0 links: hardware successive-approximation
 *     auto-EQ, optional DFE tap derivation from the auto-EQ amplitude
 *     readback, a per-channel RS sweep scored by CED when errors remain,
 *     and a final RS-fixed skew training pass.
 *
 * Register usage is functional interoperability knowledge of the ITE
 * receiver core; the control flow is EverLive's own.
 *
 *   bank 0  0x07  EQ IRQ status (W1C): [4] auto-EQ done, [5] fail
 *   bank 0  0x0f  bank select [1:0]
 *   bank 0  0x14  [7:6] HDMI 2.0 clock ratio / scrambling detected (IT6805)
 *   bank 0  0x23  [4] EQ reset (pulse), other bits preserved
 *   bank 0  0x3b  CED options; [3] count from the EQ engine's counter
 *   bank 3  0x20/0x21  clock-select init (0x1b/0x03), 0x20[7] auto trigger
 *   bank 3  0x22  [6] DFE enable, [5:3] AMP/EQ/EQ-power, [2] EQ trigger
 *   bank 3  0x26-0x29  RS: 0x27 ch0(B) 0x28 ch1(G) 0x29 ch2(R), [7] force
 *   bank 3  0x2c/0x2d  skew control (0x00/0x07 forces skew 0)
 *   bank 3  0x30-0x39  EQ engine options / timing
 *   bank 3  0x36[3:0]  RS index for amplitude readback
 *   bank 3  0x37[7:6]  channel select for readback, [4] RS one-stage
 *   bank 3  0x4b-0x53  manual DFE taps (3 per channel), 0x4b[7] enable
 *   bank 3  0x54[7]  pre-EQ, 0x55 RSM threshold ([7] clear for CED read)
 *   bank 3  0x5d-0x60  amplitude readback A..D for the selected RS index
 *   bank 3  0x63/0x64  AMP timeout per channel (0x3fff = timed out)
 *   bank 3  0xd5-0xd7  auto-EQ resulting RS per channel
 *   bank 3  0xe9-0xeb  CED counter control / low / high ([7] valid)
 */

#include "gc573_user_rx_eq.h"
#include "gc573_user_it6805.h"
#include "gc573_user_i2c.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BANK0			0x00
#define BANK3			0x03
#define REG_BANK_SELECT		0x0f

#define EQ_DONE_TIMEOUT_MS	500
#define EQ_POLL_MS		10
#define CED_WINDOW_MS		100
#define CED_SATURATED		0x7fff

#define CH_COUNT		GC573_RX_TMDS_CHANNELS

struct rx_port {
	const char *name;
	u8 addr;
	/* DFE taps are only applied where the register block is confirmed. */
	bool dfe;
	int (*set_bank)(void *bar0, u8 bank);
};

/* The IT6664 RX has no shared bank cache; select explicitly every time. */
static int it6664_rx_set_bank(void *bar0, u8 bank)
{
	u8 val = bank & 0x03;

	return gc573_user_i2c_write(bar0, GC573_I2C_BUS_PRIMARY, 0x38,
				    REG_BANK_SELECT, 1, &val, 1);
}

static const struct rx_port rx_ports[GC573_RX_PORT_COUNT] = {
	[GC573_RX_INPUT] = {
		.name = "input (IT6664)",
		.addr = 0x38,
		.dfe = false,
		.set_bank = it6664_rx_set_bank,
	},
	[GC573_RX_CAPTURE] = {
		.name = "capture (IT6805)",
		.addr = 0x48,
		.dfe = true,
		/* Keeps the IT6805 driver's bank cache coherent. */
		.set_bank = gc573_user_it6805_set_bank,
	},
};

/* RS values the amplitude readback indexes (0x36[3:0] = position). */
static const u8 rs_ladder[] = {
	0x7f, 0x7e, 0x3f, 0x3e, 0x1f, 0x1e, 0x0f,
	0x0e, 0x07, 0x06, 0x03, 0x02, 0x01, 0x00,
};
#define RS_LADDER_LEN		(sizeof(rs_ladder) / sizeof(rs_ladder[0]))
#define RS_DEFAULT_INDEX	4	/* 0x1f */

/*
 * The manual sweep tries every RS step the receiver defines.  Short or
 * strong links can want the weakest settings (down to 0x00), so the ends
 * of the range must be covered.
 */
#define rs_sweep		rs_ladder
#define RS_SWEEP_LEN		RS_LADDER_LEN

struct dfe_taps {
	u8 tap[3];
};

static struct {
	bool armed;
	u64 armed_at_ms;
} g_ced_poll[GC573_RX_PORT_COUNT];

/* ---------- register helpers ---------- */

static int rd(void *bar0, const struct rx_port *p, u8 bank, u8 reg, u8 *val)
{
	int ret = p->set_bank(bar0, bank);

	if (ret < 0)
		return ret;
	return gc573_user_i2c_read(bar0, GC573_I2C_BUS_PRIMARY, p->addr,
				   reg, 1, val, 1);
}

static int wr(void *bar0, const struct rx_port *p, u8 bank, u8 reg, u8 val)
{
	int ret = p->set_bank(bar0, bank);

	if (ret < 0)
		return ret;
	return gc573_user_i2c_write(bar0, GC573_I2C_BUS_PRIMARY, p->addr,
				    reg, 1, &val, 1);
}

static int upd(void *bar0, const struct rx_port *p, u8 bank, u8 reg,
	       u8 mask, u8 val)
{
	u8 cur = 0;
	int ret = rd(bar0, p, bank, reg, &cur);

	if (ret < 0)
		return ret;
	return wr(bar0, p, bank, reg, (u8)((cur & ~mask) | (val & mask)));
}

#define TRY(expr)				\
	do {					\
		int ret_ = (expr);		\
		if (ret_ < 0)			\
			return ret_;		\
	} while (0)

static u64 mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000ULL + (u64)ts.tv_nsec / 1000000ULL;
}

const char *gc573_rx_port_name(enum gc573_rx_port port)
{
	return port < GC573_RX_PORT_COUNT ? rx_ports[port].name : "?";
}

/* ---------- link state / CED ---------- */

bool gc573_rx_link_is_hdmi2(void *bar0)
{
	const struct rx_port *p = &rx_ports[GC573_RX_CAPTURE];
	u8 status14 = 0;
	bool hdmi2;

	if (rd(bar0, p, BANK0, 0x14, &status14) < 0)
		return false;
	hdmi2 = (status14 & 0xc0) != 0;
	p->set_bank(bar0, BANK0);
	return hdmi2;
}

static int ced_arm(void *bar0, const struct rx_port *p)
{
	TRY(upd(bar0, p, BANK0, 0x3b, 0x08, 0x08));
	TRY(upd(bar0, p, BANK3, 0x55, 0x80, 0x00));
	TRY(wr(bar0, p, BANK3, 0xe9, 0x80));
	return p->set_bank(bar0, BANK0);
}

static int ced_latch(void *bar0, const struct rx_port *p,
		     struct gc573_rx_ced *out)
{
	/* 0xe9[6:5] selects the channel whose count shows in 0xea/0xeb. */
	static const u8 select[CH_COUNT] = { 0x00, 0x20, 0x40 };

	for (unsigned int ch = 0; ch < CH_COUNT; ch++) {
		u8 lo = 0, hi = 0;

		TRY(wr(bar0, p, BANK3, 0xe9, select[ch]));
		TRY(rd(bar0, p, BANK3, 0xea, &lo));
		TRY(rd(bar0, p, BANK3, 0xeb, &hi));
		out->valid[ch] = (hi & 0x80) != 0;
		out->count[ch] = (u16)(((hi & 0x7f) << 8) | lo);
	}
	return p->set_bank(bar0, BANK0);
}

static int ced_measure(void *bar0, const struct rx_port *p,
		       unsigned int window_ms, struct gc573_rx_ced *out)
{
	memset(out, 0, sizeof(*out));
	TRY(ced_arm(bar0, p));
	msleep(window_ms);
	out->window_ms = window_ms;
	return ced_latch(bar0, p, out);
}

int gc573_rx_ced_measure(void *bar0, enum gc573_rx_port port,
			 unsigned int window_ms, struct gc573_rx_ced *out)
{
	if (!bar0 || !out || port >= GC573_RX_PORT_COUNT)
		return -EINVAL;
	return ced_measure(bar0, &rx_ports[port], window_ms, out);
}

int gc573_rx_ced_poll(void *bar0, enum gc573_rx_port port, u64 now_ms,
		      struct gc573_rx_ced *out)
{
	const struct rx_port *p;
	int ret;

	if (!bar0 || !out || port >= GC573_RX_PORT_COUNT)
		return -EINVAL;
	p = &rx_ports[port];

	if (!g_ced_poll[port].armed) {
		ret = ced_arm(bar0, p);
		if (ret < 0)
			return ret;
		g_ced_poll[port].armed = true;
		g_ced_poll[port].armed_at_ms = now_ms;
		return -EAGAIN;
	}

	memset(out, 0, sizeof(*out));
	ret = ced_latch(bar0, p, out);
	if (ret == 0) {
		out->window_ms = (u32)(now_ms - g_ced_poll[port].armed_at_ms);
		ret = ced_arm(bar0, p);
	}
	if (ret < 0) {
		g_ced_poll[port].armed = false;
		return ret;
	}
	g_ced_poll[port].armed_at_ms = now_ms;
	return 0;
}

void gc573_rx_ced_reset(void)
{
	memset(g_ced_poll, 0, sizeof(g_ced_poll));
}

/* Invalid or saturated channels score as the worst possible value. */
static u32 ced_score(const struct gc573_rx_ced *c, unsigned int ch)
{
	return c->valid[ch] ? c->count[ch] : CED_SATURATED;
}

static bool ced_any_valid(const struct gc573_rx_ced *c)
{
	for (unsigned int ch = 0; ch < CH_COUNT; ch++)
		if (c->valid[ch])
			return true;
	return false;
}

static u32 ced_total(const struct gc573_rx_ced *c)
{
	u32 sum = 0;

	for (unsigned int ch = 0; ch < CH_COUNT; ch++)
		sum += ced_score(c, ch);
	return sum;
}

/* ---------- EQ engine ---------- */

/* Pulse the EQ reset bit; leave every other bit of 0x23 as it was. */
static int eq_reset_pulse(void *bar0, const struct rx_port *p)
{
	u8 reg23 = 0;

	TRY(rd(bar0, p, BANK0, 0x23, &reg23));
	TRY(wr(bar0, p, BANK0, 0x07, 0xff));
	TRY(wr(bar0, p, BANK0, 0x23, (u8)(reg23 | 0xb0)));
	msleep(10);
	TRY(wr(bar0, p, BANK0, 0x23, (u8)((reg23 | 0xa0) & ~0x10)));
	return 0;
}

/* Waits for the auto-EQ done/fail IRQ; returns 1 done, 0 failed/timeout. */
static int eq_wait(void *bar0, const struct rx_port *p)
{
	u64 deadline = mono_ms() + EQ_DONE_TIMEOUT_MS;
	int result = 0;

	for (;;) {
		u8 irq = 0;

		TRY(rd(bar0, p, BANK0, 0x07, &irq));
		if (irq & 0x30) {
			TRY(wr(bar0, p, BANK0, 0x07, irq & 0x30));
			result = (irq & 0x10) ? 1 : 0;
			break;
		}
		if (mono_ms() >= deadline)
			break;
		msleep(EQ_POLL_MS);
	}
	/* Drop the trigger again once the engine has finished. */
	TRY(upd(bar0, p, BANK3, 0x22, 0x04, 0x00));
	TRY(p->set_bank(bar0, BANK0));
	return result;
}

static int eq_run_auto(void *bar0, const struct rx_port *p)
{
	static const u8 setup[][2] = {
		{ 0x26, 0x00 },
		{ 0x27, 0x1f }, { 0x28, 0x1f }, { 0x29, 0x1f },
		{ 0x30, 0x8c },			/* skew option on */
		{ 0x31, 0xb0 },			/* AMP time */
		{ 0x32, 0x43 }, { 0x33, 0x47 }, { 0x34, 0x4b }, { 0x35, 0x53 },
		{ 0x37, 0x0b },
		{ 0x38, 0xf2 },			/* monitor time */
		{ 0x39, 0x0d },			/* CED valid threshold */
		{ 0x55, 0x40 },			/* RSM threshold */
	};

	/* The trigger must be low before the EQ reset. */
	TRY(wr(bar0, p, BANK3, 0x20, 0x1b));
	TRY(wr(bar0, p, BANK3, 0x21, 0x03));
	TRY(upd(bar0, p, BANK3, 0x20, 0x80, 0x00));
	TRY(wr(bar0, p, BANK3, 0x22, 0x00));

	TRY(eq_reset_pulse(bar0, p));
	TRY(wr(bar0, p, BANK0, 0x3b, 0x23));

	TRY(upd(bar0, p, BANK3, 0x2c, 0xc0, 0xc0));
	TRY(upd(bar0, p, BANK3, 0x2d, 0xf7, 0x20));
	for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++)
		TRY(wr(bar0, p, BANK3, setup[i][0], setup[i][1]));
	TRY(upd(bar0, p, BANK3, 0x36, 0xc0, 0x00));
	TRY(upd(bar0, p, BANK3, 0x4a, 0x80, 0x00));
	TRY(upd(bar0, p, BANK3, 0x4b, 0x80, 0x00));
	TRY(upd(bar0, p, BANK3, 0x54, 0x80, 0x80));
	TRY(upd(bar0, p, BANK3, 0x22, 0x04, 0x04));
	return eq_wait(bar0, p);
}

/*
 * Derive DFE taps for every RS step on one channel from the amplitude
 * samples the auto-EQ engine leaves behind.  Tap n is half the difference
 * between sample A and sample n+1, signed, clamped to 5/4/3 bits.
 */
static int eq_read_dfe(void *bar0, const struct rx_port *p, unsigned int ch,
		       struct dfe_taps taps[RS_LADDER_LEN])
{
	static const u8 tap_max[3] = { 0x1f, 0x0f, 0x07 };
	static const u8 tap_flag[3] = { 0x40, 0x20, 0x10 };

	TRY(upd(bar0, p, BANK3, 0x37, 0xc0, (u8)(ch << 6)));
	for (unsigned int i = 0; i < RS_LADDER_LEN; i++) {
		u8 amp[4];

		TRY(upd(bar0, p, BANK3, 0x36, 0x0f, (u8)i));
		for (unsigned int k = 0; k < 4; k++)
			TRY(rd(bar0, p, BANK3, (u8)(0x5d + k), &amp[k]));
		for (unsigned int t = 0; t < 3; t++) {
			bool negative = amp[0] > amp[t + 1];
			u8 mag = negative ? (u8)((amp[0] - amp[t + 1]) >> 1) :
					    (u8)((amp[t + 1] - amp[0]) >> 1);

			if (mag > tap_max[t])
				mag = tap_max[t];
			/* sign bit sits just above the magnitude field */
			taps[i].tap[t] = (u8)(tap_flag[t] |
					      (negative ? (tap_max[t] + 1) : 0) |
					      mag);
		}
	}
	return 0;
}

static unsigned int rs_index(u8 rs)
{
	for (unsigned int i = 0; i < RS_LADDER_LEN; i++)
		if (rs_ladder[i] == rs)
			return i;
	return RS_DEFAULT_INDEX;
}

/*
 * Force one RS value per channel and, when @dfe is non-NULL, load the
 * matching DFE taps and enable the manual DFE path.
 */
static int eq_apply(void *bar0, const struct rx_port *p, const u8 rs[CH_COUNT],
		    struct dfe_taps (*dfe)[RS_LADDER_LEN])
{
	for (unsigned int ch = 0; ch < CH_COUNT; ch++) {
		TRY(wr(bar0, p, BANK3, (u8)(0x27 + ch), (u8)(0x80 | rs[ch])));
		if (dfe) {
			const struct dfe_taps *t = &dfe[ch][rs_index(rs[ch])];

			for (unsigned int k = 0; k < 3; k++)
				TRY(wr(bar0, p, BANK3, (u8)(0x4b + ch * 3 + k),
				       t->tap[k]));
		}
	}
	if (p->dfe) {
		/* 0x4b[7] shares a register with ch0's first tap: set last. */
		TRY(upd(bar0, p, BANK3, 0x4b, 0x80, dfe ? 0x80 : 0x00));
		TRY(upd(bar0, p, BANK3, 0x22, 0x40, dfe ? 0x40 : 0x00));
	}
	return p->set_bank(bar0, BANK0);
}

/* Skew training with the RS values already forced. */
static int eq_run_skew(void *bar0, const struct rx_port *p)
{
	TRY(upd(bar0, p, BANK3, 0x22, 0x04, 0x00));
	TRY(wr(bar0, p, BANK3, 0x2d, 0x00));
	TRY(wr(bar0, p, BANK3, 0x30, 0x94));	/* manual RS + skew option */
	TRY(wr(bar0, p, BANK3, 0x31, 0xb0));
	TRY(upd(bar0, p, BANK3, 0x37, 0x10, 0x10));
	TRY(upd(bar0, p, BANK3, 0x54, 0x80, 0x00));
	TRY(upd(bar0, p, BANK3, 0x22, 0x04, 0x04));
	return eq_wait(bar0, p);
}

static int eq_force_zero_skew(void *bar0, const struct rx_port *p)
{
	TRY(wr(bar0, p, BANK3, 0x2c, 0x00));
	TRY(wr(bar0, p, BANK3, 0x2d, 0x07));
	return p->set_bank(bar0, BANK0);
}

static bool env_flag(const char *name)
{
	const char *v = getenv(name);

	return v && *v && strcmp(v, "0") != 0;
}

/*
 * DFE on the IT6664 input is unverified: opt in with EVERLIVE_EQ_INPUT_DFE=1.
 */
static bool port_dfe(enum gc573_rx_port port)
{
	if (port == GC573_RX_INPUT)
		return env_flag("EVERLIVE_EQ_INPUT_DFE");
	return rx_ports[port].dfe;
}

/*
 * Auto-EQ and apply its result.  Fills @rs, @dfe and @dfe_ok, returns 1
 * when the hardware reported completion, 0 if it failed (RS left at the
 * power-on value), negative errno on I2C failure.
 */
static int eq_auto_and_apply(void *bar0, const struct rx_port *p,
			     u8 rs[CH_COUNT],
			     struct dfe_taps (*dfe)[RS_LADDER_LEN],
			     bool *dfe_ok)
{
	int done = eq_run_auto(bar0, p);

	*dfe_ok = false;
	if (done < 0)
		return done;
	if (!done) {
		for (unsigned int ch = 0; ch < CH_COUNT; ch++)
			rs[ch] = 0x1f;
		TRY(eq_apply(bar0, p, rs, NULL));
		return 0;
	}

	*dfe_ok = p->dfe;
	for (unsigned int ch = 0; ch < CH_COUNT; ch++) {
		u8 lo = 0, hi = 0, r = 0;

		TRY(rd(bar0, p, BANK3, (u8)(0xd5 + ch), &r));
		rs[ch] = r & 0x7f;
		if (!p->dfe)
			continue;
		TRY(upd(bar0, p, BANK3, 0x37, 0xc0, (u8)(ch << 6)));
		TRY(rd(bar0, p, BANK3, 0x63, &lo));
		TRY(rd(bar0, p, BANK3, 0x64, &hi));
		if ((u16)((hi << 8) | lo) == 0x3fff)
			*dfe_ok = false;	/* amplitude search timed out */
	}
	if (*dfe_ok)
		for (unsigned int ch = 0; ch < CH_COUNT; ch++)
			TRY(eq_read_dfe(bar0, p, ch, dfe[ch]));
	TRY(eq_apply(bar0, p, rs, *dfe_ok ? dfe : NULL));
	return 1;
}

int gc573_rx_eq_tune(void *bar0, enum gc573_rx_port port,
		     struct gc573_rx_eq_result *res)
{
	struct dfe_taps dfe[CH_COUNT][RS_LADDER_LEN];
	struct rx_port port_copy;
	const struct rx_port *p;
	struct gc573_rx_ced ced;
	u32 best_score[CH_COUNT];
	u8 best_rs[CH_COUNT];
	u8 rs[CH_COUNT];
	u64 start = mono_ms();
	bool dfe_ok = false;
	u32 auto_total;
	int ret;

	if (!bar0 || !res || port >= GC573_RX_PORT_COUNT)
		return -EINVAL;
	port_copy = rx_ports[port];
	port_copy.dfe = port_dfe(port);
	p = &port_copy;
	memset(res, 0, sizeof(*res));
	memset(dfe, 0, sizeof(dfe));

	if (env_flag("EVERLIVE_NO_AUTO_EQ") || !gc573_rx_link_is_hdmi2(bar0))
		return 0;
	g_ced_poll[port].armed = false;

	/* 1. Baseline on the current EQ; leave an error-free link alone. */
	TRY(ced_measure(bar0, p, CED_WINDOW_MS, &ced));
	res->errors_before = ced_total(&ced);
	res->ced_valid = ced_any_valid(&ced);
	res->before_valid = res->ced_valid;
	if (res->ced_valid && res->errors_before == 0) {
		res->elapsed_ms = (u32)(mono_ms() - start);
		return 0;
	}
	res->ran = true;

	/* 2. Hardware auto-EQ (+ DFE where supported). */
	ret = eq_auto_and_apply(bar0, p, rs, dfe, &dfe_ok);
	if (ret < 0)
		return ret;
	res->auto_eq_done = ret == 1;
	res->dfe_applied = dfe_ok;

	TRY(ced_measure(bar0, p, CED_WINDOW_MS, &ced));
	auto_total = ced_total(&ced);
	res->ced_valid |= ced_any_valid(&ced);
	memcpy(best_rs, rs, sizeof(best_rs));
	for (unsigned int ch = 0; ch < CH_COUNT; ch++)
		best_score[ch] = ced_score(&ced, ch);

	/*
	 * 3. Errors remain: sweep RS per channel with skew forced to 0.  The
	 *    sweep is scored by CED, so it is pointless without valid counts.
	 */
	if (auto_total && ced_any_valid(&ced)) {
		u32 sweep_total = 0;

		res->swept = true;
		TRY(eq_force_zero_skew(bar0, p));
		for (unsigned int i = 0; i < RS_SWEEP_LEN; i++) {
			u8 trial[CH_COUNT] = { rs_sweep[i], rs_sweep[i],
					       rs_sweep[i] };

			TRY(eq_apply(bar0, p, trial, dfe_ok ? dfe : NULL));
			TRY(ced_measure(bar0, p, CED_WINDOW_MS, &ced));
			for (unsigned int ch = 0; ch < CH_COUNT; ch++) {
				if (ced_score(&ced, ch) < best_score[ch]) {
					best_score[ch] = ced_score(&ced, ch);
					best_rs[ch] = rs_sweep[i];
				}
			}
		}
		for (unsigned int ch = 0; ch < CH_COUNT; ch++)
			sweep_total += best_score[ch];

		/* 4. Retrain skew around the chosen RS values. */
		TRY(eq_apply(bar0, p, best_rs, dfe_ok ? dfe : NULL));
		ret = eq_run_skew(bar0, p);
		if (ret < 0)
			return ret;
		TRY(ced_measure(bar0, p, CED_WINDOW_MS, &ced));

		res->skew_trained = true;
		/* Skew training made it worse: keep skew 0. */
		if (ced_total(&ced) > sweep_total) {
			res->skew_trained = false;
			TRY(eq_force_zero_skew(bar0, p));
			TRY(eq_apply(bar0, p, best_rs, dfe_ok ? dfe : NULL));
			TRY(ced_measure(bar0, p, CED_WINDOW_MS, &ced));
		}

		/* Nothing beat plain auto-EQ: run it again and keep that. */
		if (ced_total(&ced) > auto_total) {
			ret = eq_auto_and_apply(bar0, p, rs, dfe, &dfe_ok);
			if (ret < 0)
				return ret;
			memcpy(best_rs, rs, sizeof(best_rs));
			res->dfe_applied = dfe_ok;
			res->kept_auto = true;
			TRY(ced_measure(bar0, p, CED_WINDOW_MS, &ced));
		}
	}

	/* 5. Done: power the EQ engine down and clear its IRQs. */
	TRY(upd(bar0, p, BANK3, 0x22, 0x08, 0x00));
	TRY(wr(bar0, p, BANK0, 0x07, 0xf0));

	memcpy(res->rs, best_rs, sizeof(res->rs));
	res->errors_after = ced_total(&ced);
	for (unsigned int ch = 0; ch < CH_COUNT; ch++)
		res->ch_errors_after[ch] = ced_score(&ced, ch);
	res->elapsed_ms = (u32)(mono_ms() - start);
	return p->set_bank(bar0, BANK0);
}

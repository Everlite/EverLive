/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_it6805.c — Userspace IT6805 HDMI 2.0 Receiver Implementation
 *
 * Mirrors the initialization subset of open_gc573/gc573-it6805-core.c:
 *   - register bank / initial sequence
 *   - CAOF clock-alignment calibration (port 0 + port 1)
 *   - OCLK oscillator calibration (revision 0xb1)
 *   - EDID programming with readback verification
 *   - TTL output / port-0 selection / EQ reset
 *   - HDMI 2.0 PHY switching (bank 3 reg 0x3a[2:1] = 0x04)
 */

#include "gc573_user_it6805.h"
#include "gc573_user_i2c.h"
#include "gc573_user_hw.h"
#include "gc573_user_edid.h"
#include <unistd.h>

#define IT6805_I2C_ADDR			0x48
#define IT6805_REG_BANK_SELECT		0x0f

#define IT6805_BANK_0			0x00
#define IT6805_BANK_1			0x01
#define IT6805_BANK_2			0x02
#define IT6805_BANK_3			0x03
#define IT6805_BANK_5			0x05
#define IT6805_BANK_CAOF_PORT0		0x03
#define IT6805_BANK_CAOF_PORT1_CONTROL	0x04
#define IT6805_BANK_CAOF_PORT1		0x07

#define IT6805_CAOF_DONE_MASK		GENMASK(5, 4)
#define IT6805_CAOF_PULSE_AFTER_POLLS	5
#define IT6805_CAOF_MAX_RESTARTS	6
#define IT6805_CAOF_DELAY_MS		10

#define IT6805_OCLK_READY		0x19
#define IT6805_OCLK_MAX_POLLS		50
#define IT6805_OCLK_MIN_KHZ		28500
#define IT6805_OCLK_MAX_KHZ		47500
#define IT6805_OCLK_DEFAULT_KHZ		38000
#define IT6805_OCLK_PCLK_SCALE		0x200

#define IT6805_TMDS_SAMPLE_COUNT	10
#define IT6805_TMDS_SAMPLE_DELAY_MS	10
#define IT6805_TMDS_SAMPLE_BIAS		10
#define IT6805_PCLK_SAMPLE_COUNT	25
/* Short sample window used by the streaming hot path. */
#define IT6805_PCLK_QUICK_SAMPLE_COUNT	8
#define IT6805_PCLK_SAMPLE_DELAY_MS	3
#define IT6805_PCLK_SAMPLE_SCALE	0x200

#define IT6805_AUDIO_REQUEST_DELAY_POLLS	4
#define IT6805_AUDIO_MONITOR_DELAY_POLLS	3
#define IT6805_AUDIO_RATE_MISMATCH_LIMIT	16
#define IT6805_AUDIO_TMDS_SAMPLE_COUNT		10
#define IT6805_AUDIO_TMDS_SAMPLE_DELAY_MS	3
#define IT6805_AUDIO_N_TOLERANCE		32
/* Common interrupt register 0x10 is write-one-to-clear. */
#define IT6805_COMMON_IRQ_AUDIO		BIT(7)

#define IT6805_BRIDGE_AUDIO_DIVISOR	0x010a0
#define IT6805_BRIDGE_AUDIO_CLOCK_HZ	100000000U
#define IT6805_BRIDGE_AUDIO_RATE_STEP_HZ	100U

static u32 g_reference_khz;
static u32 g_tmds_clock_khz;

struct it6805_reg_update {
	u8 reg;
	u8 mask;
	u8 value;
};

struct it6805_caof_result {
	u8 completion[2];
	u8 interrupt[2];
	u16 status[2];
	unsigned int restarts;
};

struct it6805_oclk_result {
	u32 raw_count;
	u32 clock_khz;
	u32 reference_khz;
	u16 probe[2];
	u16 count[2];
	u8 status_initial;
	u8 status_final;
	u8 selector_offset;
	u8 scale;
	u8 reg91;
	u8 reg92;
	u8 regfd;
	u8 reg44;
	u8 reg45;
	u8 reg46;
	u8 reg47;
	unsigned int polls;
	bool fallback;
};

static u8 g_current_bank = 0xff;

enum it6805_audio_state {
	IT6805_AUDIO_OFF,
	IT6805_AUDIO_REQUEST,
	IT6805_AUDIO_WAIT_READY,
	IT6805_AUDIO_ON,
};

enum it6805_audio_rate_code {
	IT6805_AUDIO_RATE_44_1_KHZ = 0x00,
	IT6805_AUDIO_RATE_48_KHZ = 0x02,
	IT6805_AUDIO_RATE_32_KHZ = 0x03,
	IT6805_AUDIO_RATE_88_2_KHZ = 0x08,
	IT6805_AUDIO_RATE_96_KHZ = 0x0a,
	IT6805_AUDIO_RATE_176_4_KHZ = 0x0c,
	IT6805_AUDIO_RATE_192_KHZ = 0x0e,
	IT6805_AUDIO_RATE_1024_KHZ = 0x35,
};

struct it6805_audio_runtime {
	enum it6805_audio_state state;
	u32 n;
	u32 cts;
	u32 sample_rate_hz;
	u8 format_b0;
	u8 format_b1;
	u8 format_b2;
	u8 channel_count;
	u8 receiver_rate_code;
	u8 selected_rate_code;
	u8 request_delay_polls;
	u8 monitor_delay_polls;
	u8 rate_mismatch_count;
	bool format_valid;
	bool measurement_refresh_pending;
	bool change_pending;
	bool output_enabled;
	bool ready_wait_reported;
};

static struct it6805_audio_runtime g_audio;
static bool g_audio_path_enabled;

int gc573_user_it6805_set_bank(void *bar0, u8 bank)
{
	if (g_current_bank == bank)
		return 0;

	u8 b = bank & 0x07;
	int ret = gc573_user_i2c_write(bar0, GC573_I2C_BUS_PRIMARY,
				       IT6805_I2C_ADDR,
				       IT6805_REG_BANK_SELECT, 1, &b, 1);
	if (ret == 0)
		g_current_bank = bank;
	return ret;
}

static int it6805_read_reg(void *bar0, u8 reg, u8 *val)
{
	return gc573_user_i2c_read(bar0, GC573_I2C_BUS_PRIMARY, IT6805_I2C_ADDR,
				   reg, 1, val, 1);
}

static int it6805_write_reg(void *bar0, u8 reg, u8 val)
{
	return gc573_user_i2c_write(bar0, GC573_I2C_BUS_PRIMARY, IT6805_I2C_ADDR,
				    reg, 1, &val, 1);
}

static int it6805_update_bits(void *bar0, u8 reg, u8 mask, u8 val)
{
	u8 cur = 0;
	int ret = it6805_read_reg(bar0, reg, &cur);
	if (ret < 0)
		return ret;

	u8 new_val = (cur & ~mask) | (val & mask);
	if (new_val == cur)
		return 0;

	return it6805_write_reg(bar0, reg, new_val);
}

static int it6805_apply_updates(void *bar0,
				const struct it6805_reg_update *updates,
				size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const struct it6805_reg_update *up = &updates[i];
		int ret;

		if (up->reg == IT6805_REG_BANK_SELECT) {
			ret = gc573_user_it6805_set_bank(bar0,
							 up->value & 0x07);
		} else {
			ret = it6805_update_bits(bar0, up->reg, up->mask,
						 up->value);
		}
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* Receiver base initialization sequence from gc573-it6805-core.c */
static const struct it6805_reg_update it6805_initial_sequence[] = {
	{ 0x0f, 0xff, 0x00 },
	{ 0x22, 0xff, 0x08 },
	{ 0x22, 0xff, 0x17 },
	{ 0x23, 0xff, 0x1f },
	{ 0x2b, 0xff, 0x1f },
	{ 0x24, 0xff, 0xf8 },
	{ 0x22, 0xff, 0x10 },
	{ 0x23, 0xff, 0xa0 },
	{ 0x2b, 0xff, 0xa0 },
	{ 0x24, 0xff, 0x00 },
	{ 0x34, 0xff, 0x00 },
	{ 0x0f, 0xff, 0x03 },
	{ 0xaa, 0xff, 0xec },
	{ 0x0f, 0xff, 0x00 },
	{ 0x0f, 0xff, 0x03 },
	{ 0xac, 0xff, 0x40 },
	{ 0x0f, 0xff, 0x00 },
	{ 0x3a, 0xff, 0x89 },
	{ 0x49, 0xff, 0xe1 },
	{ 0x43, 0xff, 0x01 },
	{ 0x0f, 0xff, 0x04 },
	{ 0x43, 0xff, 0x01 },
	{ 0x3a, 0xff, 0x89 },
	{ 0x0f, 0xff, 0x03 },
	{ 0xa8, 0xff, 0x0b },
	{ 0x0f, 0xff, 0x00 },
	{ 0x4f, 0xff, 0x84 },
	{ 0x44, 0xff, 0x19 },
	{ 0x46, 0xff, 0x15 },
	{ 0x47, 0xff, 0x88 },
	{ 0xd9, 0xff, 0x00 },
	{ 0xf0, 0xff, 0x78 },
	{ 0xf1, 0xff, 0x10 },
	{ 0x0f, 0xff, 0x03 },
	{ 0x3a, 0xff, 0x02 },
	{ 0x0f, 0xff, 0x00 },
	{ 0x28, 0xff, 0x88 },
	{ 0x6e, 0xff, 0x80 },
	{ 0x77, 0xff, 0x87 },
	{ 0x7b, 0xff, 0x00 },
	{ 0x86, 0xff, 0x00 },
	{ 0x0f, 0xff, 0x00 },
	{ 0x36, 0xff, 0x06 },
	{ 0x8f, 0xff, 0x41 },
	{ 0x0f, 0xff, 0x01 },
	{ 0xc0, 0xff, 0x42 },
	{ 0xc4, 0x70, 0x00 },
	{ 0xc4, 0x80, 0x00 },
	{ 0xc5, 0xff, 0x00 },
	{ 0xc6, 0xff, 0x00 },
	{ 0xc7, 0xff, 0x00 },
	{ 0xc8, 0xff, 0x00 },
	{ 0xc9, 0xff, 0x99 },
	{ 0xca, 0xff, 0x99 },
	{ 0x0f, 0xff, 0x00 },
	{ 0x86, 0x0c, 0x08 },
	{ 0x81, 0x80, 0x80 },
	{ 0x0f, 0x07, 0x01 },
	{ 0x10, 0xff, 0x00 },
	{ 0x11, 0xff, 0x00 },
	{ 0x12, 0xff, 0x00 },
	{ 0x13, 0xff, 0x00 },
	{ 0x28, 0xff, 0x00 },
	{ 0x29, 0xff, 0x00 },
	{ 0x2a, 0xff, 0x00 },
	{ 0x2b, 0xff, 0x00 },
	{ 0x2c, 0xff, 0x00 },
	{ 0xc0, 0xc0, 0x40 },
	{ 0x0f, 0x07, 0x03 },
	{ 0xe3, 0xff, 0x07 },
	{ 0x27, 0xff, 0x9f },
	{ 0x28, 0xff, 0x9f },
	{ 0x29, 0xff, 0x9f },
	{ 0xa7, 0x40, 0x40 },
	{ 0x0f, 0x07, 0x07 },
	{ 0xe3, 0xff, 0x07 },
	{ 0x27, 0xff, 0x9f },
	{ 0x28, 0xff, 0x9f },
	{ 0x29, 0xff, 0x9f },
	{ 0xa7, 0x40, 0x40 },
	{ 0x0f, 0x07, 0x00 },
	{ 0xf8, 0xff, 0xc3 },
	{ 0xf8, 0xff, 0xa5 },
	{ 0x0f, 0x07, 0x01 },
	{ 0x5f, 0xff, 0x04 },
	{ 0x58, 0xff, 0x12 },
	{ 0x58, 0xff, 0x02 },
	{ 0x5f, 0xff, 0x00 },
	{ 0x0f, 0x07, 0x00 },
	{ 0xf8, 0xff, 0xff },
	{ 0x0f, 0x07, 0x05 },
	{ 0x20, 0x03, 0x01 },
	{ 0x0f, 0x07, 0x00 },
	{ 0x0f, 0x07, 0x04 },
	{ 0x3c, 0x20, 0x00 },
	{ 0x0f, 0x07, 0x00 },
	{ 0x91, 0x40, 0x40 },
	{ 0x0f, 0x07, 0x03 },
	{ 0xf0, 0xff, 0xc0 },
	{ 0x0f, 0x07, 0x00 },
	{ 0x21, 0x40, 0x40 },
	{ 0xce, 0x30, 0x00 },
	{ 0x0f, 0x07, 0x04 },
	{ 0xce, 0x30, 0x00 },
	{ 0x42, 0xe0, 0xc0 },
	{ 0x0f, 0x07, 0x00 },
	{ 0x42, 0xe0, 0xc0 },
	{ 0x7b, 0x10, 0x10 },
	{ 0x3c, 0x21, 0x00 },
	{ 0x3b, 0xff, 0x23 },
	{ 0xf6, 0xff, 0x08 },
	{ 0x0f, 0x07, 0x04 },
	{ 0x3c, 0x21, 0x00 },
	{ 0x3b, 0xff, 0x23 },
	{ 0x0f, 0x07, 0x00 },
	{ 0x59, 0xff, 0x00 },
};

static const struct it6805_reg_update it6805_caof_enable[] = {
	{ 0x3a, 0x80, 0x00 },
	{ 0xa0, 0x80, 0x80 },
	{ 0xa1, 0x80, 0x80 },
	{ 0xa2, 0x80, 0x80 },
	{ 0xa4, 0x08, 0x08 },
	{ 0x3b, 0xc0, 0x00 },
	{ 0xa7, 0x10, 0x10 },
	{ 0x48, 0x80, 0x80 },
};

static const struct it6805_reg_update it6805_caof_disable[] = {
	{ 0x3a, 0x80, 0x00 },
	{ 0xa0, 0x80, 0x00 },
	{ 0xa1, 0x80, 0x00 },
	{ 0xa2, 0x80, 0x00 },
};

static int it6805_write_zero_range(void *bar0, u8 first_reg, u8 count)
{
	for (u8 i = 0; i < count; i++) {
		int ret = it6805_write_reg(bar0, first_reg + i, 0);

		if (ret < 0)
			return ret;
	}
	return 0;
}

static int it6805_caof_prepare(void *bar0)
{
	int ret;

	ret = it6805_apply_updates(bar0, it6805_caof_enable,
				   ARRAY_SIZE(it6805_caof_enable));
	if (ret < 0)
		return ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x29, 0x01, 0x01);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x2a, 0x41, 0x41);
	if (ret < 0)
		return ret;
	msleep(IT6805_CAOF_DELAY_MS);
	ret = it6805_update_bits(bar0, 0x2a, 0x40, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x24, 0x04, 0x04);
	if (ret < 0)
		return ret;
	ret = it6805_write_zero_range(bar0, 0x25, 4);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x3c, 0x10, 0x00);
	if (ret < 0)
		return ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT1);
	if (ret < 0)
		return ret;
	ret = it6805_apply_updates(bar0, it6805_caof_enable,
				   ARRAY_SIZE(it6805_caof_enable));
	if (ret < 0)
		return ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x32, 0x41, 0x41);
	if (ret < 0)
		return ret;
	msleep(IT6805_CAOF_DELAY_MS);
	ret = it6805_update_bits(bar0, 0x32, 0x40, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x2c, 0x04, 0x04);
	if (ret < 0)
		return ret;
	ret = it6805_write_zero_range(bar0, 0x2d, 4);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0,
					 IT6805_BANK_CAOF_PORT1_CONTROL);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x3c, 0x10, 0x00);
	if (ret < 0)
		return ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x3a, 0x80, 0x80);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x3a, 0x80, 0x80);
	if (ret < 0)
		return ret;

	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

/* 0x08/0x0d are bank-0 completion flags for both CAOF ports. */
static int it6805_caof_read_done_at(void *bar0,
				    struct it6805_caof_result *result)
{
	int ret;

	ret = it6805_read_reg(bar0, 0x08, &result->completion[0]);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x0d, &result->completion[1]);
	if (ret < 0)
		return ret;

	result->completion[0] &= IT6805_CAOF_DONE_MASK;
	result->completion[1] &= IT6805_CAOF_DONE_MASK;
	return 0;
}

static bool it6805_caof_complete(const struct it6805_caof_result *result)
{
	return result->completion[0] && result->completion[1];
}

static int it6805_caof_pulse(void *bar0, u8 reg)
{
	int ret;

	ret = it6805_update_bits(bar0, reg, 0x40, 0x40);
	if (ret < 0)
		return ret;
	msleep(IT6805_CAOF_DELAY_MS);
	return it6805_update_bits(bar0, reg, 0x40, 0x00);
}

static int it6805_caof_force_retry(void *bar0, unsigned int port)
{
	u8 bank = port ? IT6805_BANK_CAOF_PORT1 : IT6805_BANK_CAOF_PORT0;
	u8 pulse_reg = port ? 0x32 : 0x2a;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, bank);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x3a, 0x80, 0x00);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	return it6805_caof_pulse(bar0, pulse_reg);
}

static int it6805_caof_wait(void *bar0, struct it6805_caof_result *result)
{
	unsigned int polls = 0;
	int ret;

	ret = it6805_caof_read_done_at(bar0, result);
	if (ret < 0)
		return ret;

	while (!it6805_caof_complete(result)) {
		ret = it6805_caof_read_done_at(bar0, result);
		if (ret < 0)
			return ret;

		if (polls >= IT6805_CAOF_PULSE_AFTER_POLLS) {
			if (!result->completion[0]) {
				ret = it6805_caof_pulse(bar0, 0x2a);
				if (ret < 0)
					return ret;
			}
			if (!result->completion[1]) {
				ret = it6805_caof_pulse(bar0, 0x32);
				if (ret < 0)
					return ret;
			}
			polls = 0;
			result->restarts++;
		}

		if (result->restarts >= IT6805_CAOF_MAX_RESTARTS)
			break;
		msleep(IT6805_CAOF_DELAY_MS);
		polls++;
	}

	if (result->restarts < IT6805_CAOF_MAX_RESTARTS)
		return 0;

	if (!result->completion[0]) {
		ret = it6805_caof_force_retry(bar0, 0);
		if (ret < 0)
			return ret;
	}
	if (!result->completion[1])
		return it6805_caof_force_retry(bar0, 1);

	return 0;
}

static int it6805_caof_read_status(void *bar0,
				   struct it6805_caof_result *result)
{
	for (unsigned int port = 0; port < 2; port++) {
		u8 bank = port ? IT6805_BANK_CAOF_PORT1 :
				 IT6805_BANK_CAOF_PORT0;
		u8 status_high;
		u8 status_low;
		u8 interrupt;
		int ret;

		ret = gc573_user_it6805_set_bank(bar0, bank);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0x5a, &status_high);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0x59, &status_low);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0x59, &interrupt);
		if (ret < 0)
			return ret;

		result->status[port] = ((u16)status_high << 4) |
				       (status_low & 0x0f);
		result->interrupt[port] = interrupt & 0xc0;
	}

	return 0;
}

static int it6805_caof_cleanup(void *bar0)
{
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x08, 0x30, 0x30);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x0d, 0x30, 0x30);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x29, 0x01, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x24, 0x04, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x3c, 0x10, 0x10);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x2c, 0x04, 0x00);
	if (ret < 0)
		return ret;

	ret = gc573_user_it6805_set_bank(bar0,
					 IT6805_BANK_CAOF_PORT1_CONTROL);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x3c, 0x10, 0x10);
	if (ret < 0)
		return ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT0);
	if (ret < 0)
		return ret;
	ret = it6805_apply_updates(bar0, it6805_caof_disable,
				   ARRAY_SIZE(it6805_caof_disable));
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT1);
	if (ret < 0)
		return ret;
	ret = it6805_apply_updates(bar0, it6805_caof_disable,
				   ARRAY_SIZE(it6805_caof_disable));
	if (ret < 0)
		return ret;

	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static int it6805_run_caof(void *bar0, struct it6805_caof_result *result)
{
	int cleanup_ret;
	int ret;

	memset(result, 0, sizeof(*result));
	ret = it6805_caof_prepare(bar0);
	if (ret < 0)
		goto cleanup;
	ret = it6805_caof_wait(bar0, result);
	if (ret < 0)
		goto cleanup;
	ret = it6805_caof_read_status(bar0, result);

cleanup:
	cleanup_ret = it6805_caof_cleanup(bar0);
	if (ret >= 0)
		ret = cleanup_ret;

	pr_info("CAOF calibration: done=%u/%u status=0x%03x/0x%03x irq=0x%02x/0x%02x restarts=%u",
		result->completion[0], result->completion[1],
		result->status[0], result->status[1],
		result->interrupt[0], result->interrupt[1],
		result->restarts);
	return ret;
}

static int it6805_oclk_read_selector(void *bar0, u8 offset, u8 selector,
				     u16 *value)
{
	u8 high;
	u8 low;
	int ret;

	ret = it6805_write_reg(bar0, 0x50, offset);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x51, selector);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x54, 0x04);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x61, &low);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x62, &high);
	if (ret < 0)
		return ret;

	*value = ((u16)high << 8) | low;
	return 0;
}

static int it6805_oclk_cleanup(void *bar0)
{
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x5f, 0x00);
	if (ret < 0)
		return ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	return it6805_write_reg(bar0, 0xf8, 0x00);
}

static int it6805_load_oclk(void *bar0, struct it6805_oclk_result *result)
{
	u32 measured_khz;
	int cleanup_ret;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		goto cleanup;
	ret = it6805_write_reg(bar0, 0xf8, 0xc3);
	if (ret < 0)
		goto cleanup;
	ret = it6805_write_reg(bar0, 0xf8, 0xa5);
	if (ret < 0)
		goto cleanup;
	ret = it6805_write_reg(bar0, 0x34, 0x00);
	if (ret < 0)
		goto cleanup;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		goto cleanup;
	ret = it6805_write_reg(bar0, 0x5f, 0x04);
	if (ret < 0)
		goto cleanup;
	ret = it6805_write_reg(bar0, 0x5f, 0x05);
	if (ret < 0)
		goto cleanup;
	ret = it6805_write_reg(bar0, 0x58, 0x12);
	if (ret < 0)
		goto cleanup;
	ret = it6805_write_reg(bar0, 0x58, 0x02);
	if (ret < 0)
		goto cleanup;
	ret = it6805_read_reg(bar0, 0x60, &result->status_initial);
	if (ret < 0)
		goto cleanup;
	result->status_final = result->status_initial;

	if (result->status_initial != IT6805_OCLK_READY) {
		ret = it6805_write_reg(bar0, 0xf8, 0xc3);
		if (ret < 0)
			goto cleanup;
		ret = it6805_write_reg(bar0, 0xf8, 0xa5);
		if (ret < 0)
			goto cleanup;
		ret = it6805_write_reg(bar0, 0x5f, 0x04);
		if (ret < 0)
			goto cleanup;
		ret = it6805_write_reg(bar0, 0x58, 0x12);
		if (ret < 0)
			goto cleanup;
		ret = it6805_write_reg(bar0, 0x58, 0x02);
		if (ret < 0)
			goto cleanup;

		for (unsigned int i = 0; i < IT6805_OCLK_MAX_POLLS; i++) {
			ret = it6805_read_reg(bar0, 0x60,
					      &result->status_final);
			if (ret < 0)
				goto cleanup;
			result->polls = i + 1;
			usleep_range(1000, 2000);
			if (result->status_final == IT6805_OCLK_READY)
				break;
		}

		usleep_range(10000, 11000);
		ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
		if (ret < 0)
			goto cleanup;
		ret = it6805_update_bits(bar0, 0xcf, 0x01, 0x01);
		if (ret < 0)
			goto cleanup;
		ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
		if (ret < 0)
			goto cleanup;
	}

	ret = it6805_write_reg(bar0, 0x57, 0x01);
	if (ret < 0)
		goto cleanup;
	ret = it6805_oclk_read_selector(bar0, 0x00, 0x00,
					&result->probe[0]);
	if (ret < 0)
		goto cleanup;
	ret = it6805_oclk_read_selector(bar0, 0x00, 0x01,
					&result->probe[1]);
	if (ret < 0)
		goto cleanup;
	if (result->probe[0] == 0xffff && !result->probe[1])
		result->selector_offset = 0x04;

	ret = it6805_oclk_read_selector(bar0, result->selector_offset, 0xb0,
					&result->count[0]);
	if (ret < 0)
		goto cleanup;
	ret = it6805_oclk_read_selector(bar0, result->selector_offset, 0xb1,
					&result->count[1]);
	if (ret < 0)
		goto cleanup;

	result->scale = result->count[1] >> 8;
	result->raw_count = ((u32)(result->count[1] & 0xff) << 16) |
			    result->count[0];
	measured_khz = result->raw_count;
	if ((result->scale & 0xc0) == 0xc0)
		measured_khz /= 100;
	if (measured_khz < IT6805_OCLK_MIN_KHZ ||
	    measured_khz > IT6805_OCLK_MAX_KHZ) {
		measured_khz = IT6805_OCLK_DEFAULT_KHZ;
		result->fallback = true;
	}
	result->clock_khz = measured_khz;

cleanup:
	cleanup_ret = it6805_oclk_cleanup(bar0);
	if (ret >= 0)
		ret = cleanup_ret;
	return ret;
}

static int it6805_program_oclk(void *bar0, struct it6805_oclk_result *result)
{
	u32 timer_khz;
	int ret;

	timer_khz = result->clock_khz / 20 + (result->clock_khz >> 1);
	result->reference_khz = result->clock_khz >> 1;
	result->reg91 = timer_khz / 1000 & 0x3f;
	result->reg92 = ((timer_khz % 1000) * 0x100) / 1000;
	result->regfd = (result->clock_khz >> 3) / 25;
	result->reg45 = (result->clock_khz >> 3) / 39;
	result->reg44 = result->clock_khz / 1560;
	result->reg46 = result->clock_khz / 2320;
	result->reg47 = result->clock_khz / 5312;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xaa, 0x1f, 0x0c);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x91, 0x3f, result->reg91);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x92, result->reg92);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0xfd, result->regfd);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xfe, 0x20, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xfe, 0x0f, 0x0c);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xfe, 0x10, 0x10);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xfe, 0x80, 0x80);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x45, result->reg45);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x44, result->reg44);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x46, result->reg46);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x47, result->reg47);
	if (ret < 0)
		return ret;

	return 0;
}

static int it6805_calibrate_oclk(void *bar0, u8 revision,
				 struct it6805_oclk_result *result)
{
	int ret;

	memset(result, 0, sizeof(*result));
	ret = it6805_load_oclk(bar0, result);
	if (ret < 0)
		return ret;

	if (revision != 0xb1) {
		pr_warn("OCLK programming skipped: IT6805 revision 0x%02x",
			revision);
		return 0;
	}

	ret = it6805_program_oclk(bar0, result);
	if (ret < 0)
		return ret;

	pr_info("OCLK calibration: %u kHz (raw 0x%05x scale 0x%02x, %s, status 0x%02x->0x%02x, polls %u)",
		result->clock_khz, result->raw_count, result->scale,
		result->fallback ? "fallback" : "measured",
		result->status_initial, result->status_final, result->polls);
	return 0;
}

static int it6805_disable_video_output(void *bar0)
{
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x28, 0x88);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0xc5, 0xff);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0xc6, 0xff);
	if (ret < 0)
		return ret;
	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static int it6805_configure_ttl(void *bar0)
{
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc0, 0x06, 0x02);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc1, 0x02, 0x02);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc1, 0x20, 0x00);
	if (ret < 0)
		return ret;
	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static int it6805_select_port0(void *bar0)
{
	u8 status13;
	u8 status16;
	unsigned int selected_port;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x13, &status13);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x16, &status16);
	if (ret < 0)
		return ret;

	selected_port = (status13 & BIT(0)) ? 0 :
			((status16 & BIT(0)) ? 1 : 0);
	if (selected_port) {
		pr_err("IT6805 port 1 is active, but GC573 capture uses port 0!");
		return -EOPNOTSUPP;
	}

	ret = it6805_update_bits(bar0, 0x35, 0x01, 0x00);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xe5, 0x1c, 0x00);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xe5, 0x1c, 0x00);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;

	ret = it6805_write_reg(bar0, 0x25, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x26, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x27, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x2a, 0x01);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x2d, 0xff);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x2e, 0xff);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x2f, 0xff);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x32, 0x3e);
	if (ret < 0)
		return ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xa8, 0x08, 0x08);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xa8, 0x08, 0x00);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc5, 0x10, 0x10);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);
	return it6805_update_bits(bar0, 0xc5, 0x10, 0x00);
}

static int it6805_reset_eq_port0(void *bar0, bool set_reg23_bit)
{
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT0);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x2c, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x2d, 0x07);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x07, 0xff);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x23, 0xb0);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);
	ret = it6805_write_reg(bar0, 0x23, 0xa0);
	if (ret < 0)
		return ret;
	if (set_reg23_bit) {
		ret = it6805_update_bits(bar0, 0x23, 0x02, 0x02);
		if (ret < 0)
			return ret;
	}
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT0);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x27, 0x9f);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x28, 0x9f);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x29, 0x9f);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x22, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x4b, 0x80, 0x00);
	if (ret < 0)
		return ret;
	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static int it6805_enter_no_signal(void *bar0)
{
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xe5, 0x1c, 0x00);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x08, 0x04);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x0d, 0x04);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x22, 0x12);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x22, 0x10);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x23, 0xfd, 0xac);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x23, 0xfd, 0xa0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x2b, 0xfd, 0xac);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x2b, 0xfd, 0xa0);
	if (ret < 0)
		return ret;
	return it6805_disable_video_output(bar0);
}

static int it6805_set_port0_hpd(void *bar0, bool high)
{
	u8 status;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x13, &status);
	if (ret < 0)
		return ret;
	if (!(status & BIT(0))) {
		ret = gc573_user_set_gpio(bar0, GC573_GPIO_INPUT_HPD,
					  high, 2);
		if (ret < 0)
			return ret;
	}

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x13, &status);
	if (ret < 0)
		return ret;
	if (!(status & BIT(6))) {
		ret = gc573_user_set_gpio(bar0, GC573_GPIO_INPUT_HPD,
					  high, 2);
		if (ret < 0)
			return ret;
	}

	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static int it6805_handle_5v_port0(void *bar0, bool set_reg23_bit,
				  bool *present)
{
	u8 status;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x13, &status);
	if (ret < 0)
		return ret;
	*present = (status & BIT(0)) != 0;

	if (*present) {
		ret = it6805_disable_video_output(bar0);
		if (ret < 0)
			return ret;
		return it6805_set_port0_hpd(bar0, true);
	}

	ret = it6805_set_port0_hpd(bar0, false);
	if (ret < 0)
		return ret;
	ret = it6805_enter_no_signal(bar0);
	if (ret < 0)
		return ret;
	return it6805_reset_eq_port0(bar0, set_reg23_bit);
}

static int it6805_set_hdmi2_phy(void *bar0, bool hdmi2)
{
	u8 current = 0;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_3);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x3a, &current);
	if (ret < 0)
		return ret;

	u8 wanted = hdmi2 ? 0x04 : 0x02;
	if ((current & 0x06) == wanted) {
		ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
		return ret;
	}

	ret = it6805_update_bits(bar0, 0x3a, 0x06, wanted);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;

	pr_info("IT6805 PHY switched to %s (bank3 0x3a: 0x%02x -> 0x%02x)",
		hdmi2 ? "HDMI 2.0" : "HDMI 1.4", current,
		(current & ~0x06) | wanted);
	return 0;
}

int gc573_user_it6805_init(void *bar0)
{
	struct it6805_caof_result caof;
	struct it6805_oclk_result oclk;
	u8 identity[4] = { 0 };
	u8 revision = 0;
	bool present = false;
	int ret;

	if (!bar0)
		return -EINVAL;

	memset(&g_audio, 0, sizeof(g_audio));
	g_audio.state = IT6805_AUDIO_OFF;
	g_audio_path_enabled = false;

	pr_info("Starting IT6805 userspace initialization...");

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	for (unsigned int i = 0; i < ARRAY_SIZE(identity); i++) {
		ret = it6805_read_reg(bar0, i, &identity[i]);
		if (ret < 0)
			return ret;
	}
	ret = it6805_read_reg(bar0, 0x04, &revision);
	if (ret < 0)
		return ret;
	pr_info("IT6805 identity %02x %02x %02x %02x, revision 0x%02x",
		identity[0], identity[1], identity[2], identity[3], revision);
	if (identity[0] != 0x54 || identity[1] != 0x49 ||
	    (identity[2] != 0x05 && identity[2] != 0x07) ||
	    identity[3] != 0x68) {
		pr_err("Unexpected IT6805 identity!");
		return -ENODEV;
	}

	/* 1. Apply the receiver base initialization sequence. */
	ret = it6805_apply_updates(bar0, it6805_initial_sequence,
				   ARRAY_SIZE(it6805_initial_sequence));
	if (ret < 0) {
		pr_err("IT6805 initial sequence failed: %d", ret);
		return ret;
	}

	/* 2. Run CAOF clock-alignment calibration for both input ports. */
	ret = it6805_run_caof(bar0, &caof);
	if (ret < 0) {
		pr_err("IT6805 CAOF calibration failed: %d", ret);
		return ret;
	}

	/* 3. Disable the video output until a valid signal is confirmed. */
	ret = it6805_disable_video_output(bar0);
	if (ret < 0)
		return ret;

	/* 4. Calibrate the internal oscillator (needed for timing measurement). */
	ret = it6805_calibrate_oclk(bar0, revision, &oclk);
	if (ret < 0) {
		pr_err("IT6805 OCLK calibration failed: %d", ret);
		return ret;
	}
	g_reference_khz = oclk.reference_khz;

	/* 5. Program and verify the EDID, then release HPD. */
	ret = gc573_user_program_edid(bar0);
	if (ret < 0) {
		pr_err("IT6805 EDID programming failed: %d", ret);
		return ret;
	}

	/* 6. Configure the TTL output and select input port 0. */
	ret = it6805_configure_ttl(bar0);
	if (ret < 0)
		return ret;
	ret = it6805_select_port0(bar0);
	if (ret < 0)
		return ret;

	/* 7. Reset the port-0 equalizer and service the 5V/HPD state twice. */
	ret = it6805_reset_eq_port0(bar0, true);
	if (ret < 0)
		return ret;
	for (unsigned int pass = 0; pass < 2; pass++) {
		ret = it6805_handle_5v_port0(bar0, pass == 0, &present);
		if (ret < 0)
			return ret;
	}

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;

	pr_info("[✓] IT6805 initialization complete (5V=%u, HPD released).",
		present);
	return 0;
}

/*
 * The ITE6805 under-reads pixel clock and frame rate together.  1440p120
 * arrives as ~100 fps at ~413 MHz; 483.4 MHz is the CVT-RB clock.  Rescale
 * against that clock before the nearest-rate pick, or 100 wins over 120.
 * A rate that is already a standard step is left alone.
 */
static bool it6805_rescale_frame_rate(u32 frame_rate_hz, u32 pixel_clock_khz,
				      u8 *rate)
{
	static const u32 std_khz[] = {
		25200, 27000, 74250, 148500, 241500, 297000, 483400, 594000,
	};
	static const u8 std_fps[] = {
		24, 25, 30, 50, 60, 120, 144, 180, 240,
	};
	u32 ref = 0, ref_diff = 0, scaled;
	size_t i;

	if (!rate || !frame_rate_hz || !pixel_clock_khz)
		return false;

	for (i = 0; i < ARRAY_SIZE(std_fps); i++) {
		if (frame_rate_hz == std_fps[i]) {
			*rate = std_fps[i];
			return true;
		}
	}

	for (i = 0; i < ARRAY_SIZE(std_khz); i++) {
		u32 diff = pixel_clock_khz > std_khz[i] ?
			pixel_clock_khz - std_khz[i] :
			std_khz[i] - pixel_clock_khz;

		if (!ref || diff < ref_diff) {
			ref = std_khz[i];
			ref_diff = diff;
		}
	}
	if (pixel_clock_khz * 100 < ref * 80 ||
	    pixel_clock_khz * 100 > ref * 92)
		return false;

	scaled = (u32)(((u64)frame_rate_hz * ref + pixel_clock_khz / 2) /
		       pixel_clock_khz);
	for (i = 0; i < ARRAY_SIZE(std_fps); i++) {
		u32 diff = scaled > std_fps[i] ? scaled - std_fps[i] :
			std_fps[i] - scaled;

		if (diff * 100 <= std_fps[i] * 5) {
			*rate = std_fps[i];
			return true;
		}
	}

	return false;
}

static u8 it6805_normalize_frame_rate(u32 frame_rate_x100,
				      u32 pixel_clock_khz)
{
	static const u8 nominal_rates[] = {
		24, 25, 30, 50, 60, 75, 100, 120, 144, 180, 240,
	};
	u32 best_error = ~0U;
	u8 best_rate = 0;
	u8 scaled;
	size_t i;

	if (!frame_rate_x100)
		return 0;
	if (it6805_rescale_frame_rate((frame_rate_x100 + 50) / 100,
				      pixel_clock_khz, &scaled))
		return scaled;

	for (i = 0; i < ARRAY_SIZE(nominal_rates); i++) {
		u32 nominal_x100 = nominal_rates[i] * 100U;
		u32 error = frame_rate_x100 > nominal_x100 ?
			frame_rate_x100 - nominal_x100 :
			nominal_x100 - frame_rate_x100;

		if (error < best_error) {
			best_error = error;
			best_rate = nominal_rates[i];
		}
	}

	/*
	 * Allow wider tolerance (up to 5%) for high-refresh modes (>= 100 Hz),
	 * matching the kernel driver's handling of genlocked retro scalers.
	 */
	if (best_rate >= 100) {
		if (best_error <= best_rate * 5U)
			return best_rate;
	} else {
		if (best_error <= best_rate * 3U)
			return best_rate;
	}

	return (u8)min((frame_rate_x100 + 50) / 100, 255U);
}

static u8 it6805_vic_frame_rate(u8 vic)
{
	switch (vic) {
	case 32:
	case 93:
	case 98:
	case 103:
		return 24;
	case 33:
	case 94:
	case 99:
	case 104:
		return 25;
	case 34:
	case 95:
	case 100:
	case 105:
		return 30;
	case 17:
	case 18:
	case 19:
	case 20:
	case 21:
	case 22:
	case 23:
	case 24:
	case 25:
	case 26:
	case 27:
	case 28:
	case 29:
	case 30:
	case 31:
	case 39:
	case 96:
	case 101:
	case 106:
		return 50;
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:
	case 9:
	case 10:
	case 11:
	case 12:
	case 13:
	case 14:
	case 15:
	case 16:
	case 40:
	case 97:
	case 102:
	case 107:
		return 60;
	case 41:
	case 64:
		return 100;
	case 46:
	case 47:
	case 63:
		return 120;
	default:
		return 0;
	}
}

static int it6805_read_avi_info(void *bar0, struct gc573_user_video_signal *sig)
{
	u8 raw[6] = { 0 };
	int cleanup_ret;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_2);
	if (ret < 0)
		return ret;

	for (size_t i = 0; i < ARRAY_SIZE(raw); i++) {
		ret = it6805_read_reg(bar0, 0x14 + i, &raw[i]);
		if (ret < 0)
			goto restore_bank;
	}

	memcpy(sig->avi_raw, raw, sizeof(raw));
	sig->colorspace = (raw[1] >> 5) & 0x03;
	sig->colorimetry = raw[2] >> 6;
	sig->extended_colorimetry = (raw[3] >> 4) & 0x07;
	sig->rgb_quantization = (raw[3] >> 2) & 0x03;
	sig->vic = raw[4] & 0x7f;
	sig->ycc_quantization = raw[5] >> 6;
	if (!sig->rgb_quantization)
		sig->rgb_quantization = 2U - (sig->vic > 1);
	sig->avi_valid = true;

restore_bank:
	cleanup_ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret >= 0)
		ret = cleanup_ret;

	return ret;
}

int gc573_user_it6805_unmute(void *bar0)
{
	u8 reg4f = 0;
	u8 regaa = 0;
	u8 regc0 = 0;
	int ret;

	if (!bar0)
		return -EINVAL;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x4f, &reg4f);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xaa, &regaa);
	if (ret < 0)
		return ret;

	/*
	 * Same gate as the IT68051 stable-video path in OpenGC570D: page 0
	 * 0x4f bit 5 stays set while 0xaa bit 3 says the source is still
	 * sending Set AVMute. Releasing during that packet leaves legal-range
	 * black (10 80) in every DMA frame.
	 */
	if (regaa & BIT(3)) {
		u8 irq10 = 0, irq11 = 0, irq12 = 0, irq1d = 0;
		u8 irqd4 = 0, irqd5 = 0;
		static u8 seen;

		/*
		 * Same W1C sequence as it6805_ack_common_irq_locked(). A
		 * latched Set-AVMute packet stays visible in 0xaa until the
		 * common and packet IRQs are acknowledged.
		 */
		ret = it6805_read_reg(bar0, 0x10, &irq10);
		if (ret < 0)
			return ret;
		if (irq10) {
			ret = it6805_write_reg(bar0, 0x10, irq10);
			if (ret < 0)
				return ret;
		}
		ret = it6805_read_reg(bar0, 0x11, &irq11);
		if (ret < 0)
			return ret;
		ret = it6805_write_reg(bar0, 0x11, irq11 & 0xbf);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0x12, &irq12);
		if (ret < 0)
			return ret;
		ret = it6805_write_reg(bar0, 0x12, irq12 & 0x7f);
		if (ret < 0)
			return ret;
		if (irq12 & BIT(7)) {
			u8 pkt_a = 0, pkt_b = 0;

			ret = it6805_read_reg(bar0, 0x1a, &pkt_a);
			if (ret < 0)
				return ret;
			ret = it6805_read_reg(bar0, 0x1b, &pkt_b);
			if (ret < 0)
				return ret;
			ret = it6805_write_reg(bar0, 0x12, BIT(7));
			if (ret < 0)
				return ret;
			if (regaa != seen)
				pr_info("IT6805 GCP packet 1a=%02x 1b=%02x", pkt_a, pkt_b);
		}
		ret = it6805_read_reg(bar0, 0x1d, &irq1d);
		if (ret < 0)
			return ret;
		ret = it6805_write_reg(bar0, 0x1d, irq1d);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0xd4, &irqd4);
		if (ret < 0)
			return ret;
		ret = it6805_write_reg(bar0, 0xd4, irqd4);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0xd5, &irqd5);
		if (ret < 0)
			return ret;
		ret = it6805_write_reg(bar0, 0xd5, irqd5);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0xaa, &regaa);
		if (ret < 0)
			return ret;
		if (regaa != seen) {
			pr_info("IT6805 AVMute gate regaa=0x%02x reg4f=0x%02x irq=%02x/%02x/%02x",
				regaa, reg4f, irq10, irq11, irq12);
			seen = regaa;
		}
		if (regaa & BIT(3))
			return -EAGAIN;
	}
	if (!(reg4f & BIT(5)))
		return 0;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc5, BIT(0), BIT(0));
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc5, BIT(0), 0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xc0, &regc0);
	if (ret < 0)
		return ret;
	if (regc0 & BIT(0)) {
		ret = it6805_update_bits(bar0, 0xc5, BIT(0), BIT(0));
		if (ret < 0)
			return ret;
	}
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x4f, 0xa0, 0xa0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x4f, 0xa0, BIT(7));
	if (ret < 0)
		return ret;

	pr_info("IT6805 video unmuted (regaa=0x%02x regc0=0x%02x)",
		regaa, regc0);
	return 0;
}

static int it6805_read_video_timing(void *bar0,
				    struct gc573_user_video_signal *sig,
				    unsigned int pclk_samples, bool sample_tmds)
{
	u32 tmds_sum = IT6805_TMDS_SAMPLE_BIAS;
	u32 pclk_sum = 0;
	u32 pclk_khz;
	u16 htotal = 0, hactive = 0, vtotal = 0, vactive = 0;
	u8 reg43 = 0, reg98 = 0;
	u8 high = 0, low = 0, sample = 0;
	bool measure_rate = pclk_samples != 0;
	bool latch_open = false;
	int ret;

	if (measure_rate && !g_reference_khz)
		return -EAGAIN;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;

	if (measure_rate) {
		u8 reg14 = 0;

		ret = it6805_read_reg(bar0, 0x14, &reg14);
		if (ret < 0)
			return ret;
		it6805_set_hdmi2_phy(bar0, (reg14 & (BIT(6) | BIT(7))) != 0);
	}

	if (sample_tmds) {
		ret = it6805_write_reg(bar0, 0x90, 0x8f);
		if (ret < 0)
			return ret;
		for (unsigned int i = 0; i < IT6805_TMDS_SAMPLE_COUNT; i++) {
			msleep(IT6805_TMDS_SAMPLE_DELAY_MS);
			ret = it6805_read_reg(bar0, 0x48, &sample);
			if (ret < 0)
				return ret;
			tmds_sum += sample;
		}
		ret = it6805_read_reg(bar0, 0x43, &reg43);
		if (ret < 0)
			return ret;
		{
			u32 tmds_factor;

			if (reg43 & BIT(7))
				tmds_factor = 0x2800;
			else if (reg43 & BIT(6))
				tmds_factor = 0x1400;
			else if (reg43 & BIT(5))
				tmds_factor = 0x0a00;
			else
				tmds_factor = 0x0500;
			g_tmds_clock_khz = g_reference_khz * tmds_factor / tmds_sum;
		}
	}

	for (unsigned int i = 0; i < pclk_samples; i++) {
		msleep(IT6805_PCLK_SAMPLE_DELAY_MS);
		ret = it6805_update_bits(bar0, 0x9a, BIT(7), 0);
		if (ret < 0)
			return ret;
		latch_open = true;
		ret = it6805_read_reg(bar0, 0x9a, &high);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0x99, &low);
		if (ret < 0)
			return ret;
		pclk_sum += (((u32)high << 8) | low) & 0x7ff;
		ret = it6805_update_bits(bar0, 0x9a, BIT(7), BIT(7));
		if (ret < 0)
			return ret;
		latch_open = false;
	}
	if (latch_open) {
		ret = it6805_update_bits(bar0, 0x9a, BIT(7), BIT(7));
		if (ret < 0)
			return ret;
	}

	ret = it6805_read_reg(bar0, 0x98, &reg98);
	if (ret < 0)
		return ret;
	if (measure_rate) {
		ret = it6805_read_reg(bar0, 0x9c, &high);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0x9b, &low);
		if (ret < 0)
			return ret;
		htotal = (((u16)high << 8) | low) & 0x3fff;
	}
	ret = it6805_read_reg(bar0, 0x9e, &high);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x9d, &low);
	if (ret < 0)
		return ret;
	hactive = (((u16)high << 8) | low) & 0x3fff;
	if (measure_rate) {
		ret = it6805_read_reg(bar0, 0xa3, &high);
		if (ret < 0)
			return ret;
		ret = it6805_read_reg(bar0, 0xa2, &low);
		if (ret < 0)
			return ret;
		vtotal = (((u16)high << 8) | low) & 0x3fff;
	}
	ret = it6805_read_reg(bar0, 0xa5, &high);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xa4, &low);
	if (ret < 0)
		return ret;
	vactive = (((u16)high << 8) | low) & 0x3fff;

	sig->width = hactive;
	sig->height = vactive;
	sig->interlaced = (reg98 & BIT(1)) != 0;

	/*
	 * The timing-only probe stops here.  It is used by the streaming hot
	 * path, which must not spend the IT6805's ~175 ms pixel-clock sample
	 * window on every poll.
	 */
	if (!measure_rate)
		return (hactive && vactive) ? 0 : -EAGAIN;

	if (!pclk_sum || !htotal || !vtotal || !hactive || !vactive)
		return -EAGAIN;

	pclk_khz = (u32)(((u64)g_reference_khz * IT6805_PCLK_SAMPLE_SCALE *
			  pclk_samples) / pclk_sum);
	sig->pixel_clock_khz = pclk_khz;
	sig->frame_rate_x100 = (u32)(((u64)pclk_khz * 100000) / htotal / vtotal);
	sig->frame_rate_hz = it6805_normalize_frame_rate(sig->frame_rate_x100,
							 pclk_khz);
	if (sig->interlaced && sig->frame_rate_hz)
		sig->frame_rate_hz /= 2;
	return 0;
}

int gc573_user_it6805_poll_status(void *bar0, struct gc573_user_video_signal *sig)
{
	u8 status13 = 0;
	u8 status14 = 0;
	u8 status15 = 0;
	u8 scdt = 0;
	int ret;

	if (!bar0 || !sig)
		return -EINVAL;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x13, &status13);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x14, &status14);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x15, &status15);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x19, &scdt);
	if (ret < 0)
		return ret;

	sig->five_volt_present = (status13 & BIT(0)) != 0;
	sig->status15 = status15;

	/*
	 * 1440p120 / 4K60 use scrambled HDMI 2.0.  Reg 0x14[7:6] is set by
	 * the source once it negotiates the high-speed link; follow it with
	 * the PHY selector in bank 3 reg 0x3a[2:1].
	 */
	bool hdmi2_status = (status14 & (BIT(6) | BIT(7))) != 0;
	ret = it6805_set_hdmi2_phy(bar0, hdmi2_status);
	if (ret < 0)
		return ret;

	/* Authoritative SCDT sync lock from kernel driver. */
	sig->scdt_locked = (scdt != 0xff) && ((scdt & BIT(7)) != 0);

	return 0;
}

int gc573_user_it6805_poll_signal_ex(void *bar0,
				     struct gc573_user_video_signal *sig,
				     enum gc573_user_signal_probe probe)
{
	bool measure_rate = probe != GC573_USER_SIGNAL_PROBE_TIMING;
	unsigned int pclk_samples;
	int ret;

	if (!bar0 || !sig)
		return -EINVAL;

	memset(sig, 0, sizeof(*sig));

	ret = gc573_user_it6805_poll_status(bar0, sig);
	if (ret < 0)
		return ret;
	if (!sig->scdt_locked)
		return 0;

	if (probe == GC573_USER_SIGNAL_PROBE_FULL)
		pclk_samples = IT6805_PCLK_SAMPLE_COUNT;
	else if (probe == GC573_USER_SIGNAL_PROBE_RATE)
		pclk_samples = IT6805_PCLK_QUICK_SAMPLE_COUNT;
	else
		pclk_samples = 0;

	ret = it6805_read_video_timing(bar0, sig, pclk_samples,
				       probe == GC573_USER_SIGNAL_PROBE_FULL);
	if (ret < 0) {
		sig->width = 0;
		sig->height = 0;
		sig->pixel_clock_khz = 0;
		sig->frame_rate_hz = 0;
		sig->frame_rate_x100 = 0;
		sig->interlaced = false;
	}

	ret = it6805_read_avi_info(bar0, sig);
	if (ret < 0) {
		pr_warn("IT6805 AVI InfoFrame read failed: %d", ret);
	} else {
		if (measure_rate) {
			u8 frame_rate = it6805_vic_frame_rate(sig->vic);
			u32 vic_x100;

			if (sig->interlaced && frame_rate)
				frame_rate /= 2;
			/*
			 * A 1440p120 source often still carries a 60 Hz VIC. The
			 * measured pixel clock is the rate the VIP must run at.
			 */
			vic_x100 = (u32)frame_rate * 100U;
			if (frame_rate && sig->frame_rate_x100 &&
			    (vic_x100 > sig->frame_rate_x100 ?
			     vic_x100 - sig->frame_rate_x100 :
			     sig->frame_rate_x100 - vic_x100) <= 500)
				sig->frame_rate_hz = frame_rate;
		}
		if (sig->colorspace == GC573_USER_COLORSPACE_YCBCR420)
			sig->width *= 2;
	}

	if (sig->interlaced)
		sig->height *= 2;

	return 0;
}

int gc573_user_it6805_poll_signal(void *bar0, struct gc573_user_video_signal *sig)
{
	return gc573_user_it6805_poll_signal_ex(bar0, sig,
					GC573_USER_SIGNAL_PROBE_FULL);
}

static bool it6805_use_high_bandwidth(const struct gc573_user_video_signal *sig)
{
	if (sig->colorspace == GC573_USER_COLORSPACE_YCBCR420)
		return false;

	/*
	 * From the official AVXGC573_x64.sys Windows driver:
	 * HActive <= 1920 && PCLK <= 150000 -> SDR mode (single-pixel TTL)
	 * Otherwise -> DDR mode (dual-pixel / high-bandwidth TTL).
	 */
	return sig->width > 1920 || sig->pixel_clock_khz > 150000;
}

static int it6805_apply_ttl_video_path(void *bar0, u8 pixel_mode,
				       bool set_colorspace, u8 colorspace)
{
	u8 pixel_value;
	int ret;

	if (!pixel_mode)
		pixel_value = 0;
	else if (pixel_mode == 1)
		pixel_value = BIT(1);
	else
		return -EOPNOTSUPP;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc0, 0x06, 0x02);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc1, BIT(1), pixel_value);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc1, BIT(5), 0);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	if (set_colorspace)
		ret = it6805_update_bits(bar0, 0x6b, 0x0c,
					 (u8)(colorspace << 2));

	return ret;
}

static int it6805_apply_lvds_video_path(void *bar0, u8 path_mode)
{
	u8 pixel_value;
	int ret;

	if (path_mode == 1)
		pixel_value = 0;
	else if (path_mode == 2)
		pixel_value = BIT(4);
	else
		return -EOPNOTSUPP;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	if (path_mode == 2) {
		ret = it6805_update_bits(bar0, 0xc0, BIT(0), BIT(0));
		if (ret < 0)
			return ret;
	}
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_5);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xd1, BIT(0), 0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xd1, 0x0c, BIT(2));
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xda, BIT(4), 0);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0xd0, 0xf3);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xbd, 0x30, pixel_value);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0xbe, 0x00);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xfe, BIT(4), BIT(4));
	if (ret < 0)
		return ret;

	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static int it6805_set_pixel_clock_gate(void *bar0, u32 pixel_clock_khz)
{
	u8 pixel_selector;
	u8 reg1b = 0;
	u8 regc0 = 0;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x1b, &reg1b);
	if (ret < 0)
		return ret;

	pixel_selector = (0x04010201U >> ((reg1b >> 1) & 0x18)) & 0xff;
	if (!pixel_selector)
		return -ERANGE;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xb0, BIT(0),
				 pixel_clock_khz / pixel_selector > 24999 ?
				 BIT(0) : 0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xc0, &regc0);
	if (ret < 0)
		return ret;
	if (!(regc0 & BIT(0))) {
		ret = it6805_update_bits(bar0, 0xb0, BIT(0), 0);
		if (ret < 0)
			return ret;
	}

	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static int it6805_set_color_depth(void *bar0)
{
	u64 table = 0x2010000000000ULL;
	u8 reg98 = 0;
	u8 value;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x98, &reg98);
	if (ret < 0)
		return ret;

	value = table >> ((reg98 >> 1) & 0x38);
	if (reg98 > 0x6f)
		value = 0;

	return it6805_update_bits(bar0, 0x6b, 0x03, value);
}

static int it6805_detect_dvi(void *bar0, bool *is_dvi)
{
	u8 mode = 0;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x13, &mode);
	if (ret < 0)
		return ret;

	*is_dvi = !(mode & BIT(1));
	return 0;
}

static int it6805_clear_audio_output(void *bar0)
{
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x81, BIT(6), 0);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0xc7, 0x7f);
	if (ret < 0)
		return ret;

	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static u32 it6805_audio_delta(u32 left, u32 right)
{
	return left > right ? left - right : right - left;
}

static int it6805_read_audio_clock(void *bar0, u32 *n, u32 *cts)
{
	u8 n_high = 0;
	u8 n_mid = 0;
	u8 n_low = 0;
	u8 cts_low = 0;
	u8 cts_mid = 0;
	u8 cts_high = 0;
	int cleanup_ret;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x86, BIT(0), BIT(0));
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_2);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xbe, &n_high);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xbf, &n_mid);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xc0, &n_low);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xc0, &cts_low);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xc1, &cts_mid);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xc2, &cts_high);
	cleanup_ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret >= 0)
		ret = cleanup_ret;
	if (ret < 0)
		return ret;

	*n = (u32)n_high << 12 | (u32)n_mid << 4 | (n_low & 0x0f);
	*cts = (u32)cts_mid << 12 | (u32)cts_high << 4 | (cts_low >> 4);

	return 0;
}

static int it6805_read_audio_format(void *bar0, u8 *b0, u8 *b1, u8 *b2)
{
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xb0, b0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xb1, b1);
	if (ret < 0)
		return ret;

	return it6805_read_reg(bar0, 0xb2, b2);
}

static int it6805_reset_audio_logic(void *bar0)
{
	u8 reg8a = 0;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x22, BIT(1), BIT(1));
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x22, BIT(1), 0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x8a, &reg8a);
	for (unsigned int i = 0; ret >= 0 && i < 4; i++)
		ret = it6805_write_reg(bar0, 0x8a, reg8a);

	return ret;
}

static int it6805_set_audio_tristate(void *bar0, bool tristate)
{
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0xc7, tristate ? 0x7f : 0x00);
	if (ret < 0)
		return ret;

	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static u8 it6805_audio_rate_code(u32 rate_khz)
{
	static const struct {
		u16 min_khz;
		u16 max_khz;
		u8 code;
	} ranges[] = {
		{ 26, 38, IT6805_AUDIO_RATE_32_KHZ },
		{ 39, 45, IT6805_AUDIO_RATE_44_1_KHZ },
		{ 46, 58, IT6805_AUDIO_RATE_48_KHZ },
		{ 59, 78, 0x0b },
		{ 79, 92, IT6805_AUDIO_RATE_88_2_KHZ },
		{ 93, 106, IT6805_AUDIO_RATE_96_KHZ },
		{ 107, 166, 0x2b },
		{ 167, 182, IT6805_AUDIO_RATE_176_4_KHZ },
		{ 183, 202, IT6805_AUDIO_RATE_192_KHZ },
		{ 225, 320, 0x1b },
		{ 321, 448, 0x05 },
		{ 449, 638, 0x3b },
		{ 639, 894, 0x09 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) {
		if (rate_khz >= ranges[i].min_khz &&
		    rate_khz <= ranges[i].max_khz)
			return ranges[i].code;
	}

	return IT6805_AUDIO_RATE_1024_KHZ;
}

static u8 it6805_audio_channel_count(u8 b0, u8 b1, u8 previous_count)
{
	if (b0 & BIT(6))
		return 2;

	switch (b1 & 0x3f) {
	case 0x01:
		return 2;
	case 0x03:
		return 4;
	case 0x07:
		return 6;
	case 0x0f:
		return 8;
	case 0x1f:
		return 10;
	case 0x3f:
		return 12;
	default:
		/* Keep the stable count until allocation bits settle. */
		return previous_count;
	}
}

static int it6805_measure_audio_tmds(void *bar0, u32 *clock_khz)
{
	u32 sample_sum = 0;
	u32 factor;
	u8 reg43 = 0;
	u8 sample = 0;
	int ret;

	if (!g_reference_khz)
		return -EAGAIN;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	for (unsigned int i = 0; ret >= 0 && i < IT6805_AUDIO_TMDS_SAMPLE_COUNT;
	     i++) {
		msleep(IT6805_AUDIO_TMDS_SAMPLE_DELAY_MS);
		ret = it6805_read_reg(bar0, 0x48, &sample);
		if (ret >= 0)
			sample_sum += sample + 1U;
	}
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x43, &reg43);
	if (ret < 0)
		return ret;
	if (!sample_sum)
		return -EAGAIN;

	if (reg43 & BIT(7))
		factor = 0x400;
	else if (reg43 & BIT(6))
		factor = 0x200;
	else if (reg43 & BIT(5))
		factor = 0x100;
	else
		factor = 0x080;

	*clock_khz = (u32)(((u64)g_reference_khz *
			    IT6805_AUDIO_TMDS_SAMPLE_COUNT * factor) /
			   sample_sum);
	return 0;
}

static int it6805_bridge_audio_rate(void *bar0, u32 *rate_hz)
{
	u8 *base = (u8 *)bar0;

	for (unsigned int attempt = 0; attempt < 5; attempt++) {
		u32 divisor = readl(base + IT6805_BRIDGE_AUDIO_DIVISOR);
		u32 measured;
		u32 rounded;

		if (!divisor)
			continue;

		measured = IT6805_BRIDGE_AUDIO_CLOCK_HZ / divisor;
		rounded = ((measured + IT6805_BRIDGE_AUDIO_RATE_STEP_HZ / 2) /
			   IT6805_BRIDGE_AUDIO_RATE_STEP_HZ) *
			  IT6805_BRIDGE_AUDIO_RATE_STEP_HZ;
		switch (rounded) {
		case 32000:
		case 44100:
		case 48000:
		case 96000:
		case 192000:
			*rate_hz = rounded;
			return 0;
		default:
			break;
		}
	}

	return -EAGAIN;
}

static int it6805_update_audio_clock(void *bar0)
{
	u32 tmds_clock_khz;
	u32 rate_khz;
	u32 n;
	u32 cts;
	u8 reg81 = 0;
	u8 regb5 = 0;
	u8 regb6 = 0;
	u8 rate_code;
	int ret;

	ret = it6805_read_audio_clock(bar0, &n, &cts);
	if (ret < 0)
		return ret;
	ret = it6805_measure_audio_tmds(bar0, &tmds_clock_khz);
	if (ret < 0)
		return ret;

	g_audio.n = n;
	g_audio.cts = cts;
	g_audio.sample_rate_hz = 0;
	if (!cts || !tmds_clock_khz)
		return 0;

	rate_khz = (u32)(((u64)tmds_clock_khz * n) / ((u64)cts << 7));
	rate_code = it6805_audio_rate_code(rate_khz);
	g_audio.sample_rate_hz = rate_khz * 1000U;
	g_audio.selected_rate_code = rate_code;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xb5, &regb5);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xb6, &regb6);
	if (ret < 0)
		return ret;

	g_audio.receiver_rate_code =
		(regb6 >> 2 & 0x30) | (regb5 & 0x0f);
	if (g_audio.receiver_rate_code == rate_code) {
		ret = it6805_read_reg(bar0, 0x81, &reg81);
		if (ret < 0)
			return ret;
		ret = it6805_update_bits(bar0, 0x81, BIT(6), 0);
		if (ret < 0)
			return ret;
		if (reg81 & BIT(6)) {
			ret = it6805_reset_audio_logic(bar0);
			if (ret < 0)
				return ret;
		}
		g_audio.rate_mismatch_count = 0;
		return 0;
	}

	g_audio.rate_mismatch_count++;
	if (g_audio.rate_mismatch_count < IT6805_AUDIO_RATE_MISMATCH_LIMIT)
		return 0;

	ret = it6805_update_bits(bar0, 0x81, BIT(6), BIT(6));
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x8a, 0x3f, rate_code);
	if (ret < 0)
		return ret;
	ret = it6805_reset_audio_logic(bar0);
	if (ret >= 0)
		g_audio.rate_mismatch_count = 0;

	return ret;
}

static int it6805_start_audio_output(void *bar0)
{
	u8 b0 = 0;
	u8 b1 = 0;
	u8 b2 = 0;
	int ret;

	ret = it6805_read_audio_format(bar0, &b0, &b1, &b2);
	if (ret < 0)
		goto fail;
	g_audio.channel_count = it6805_audio_channel_count(
		b0, b1, g_audio.channel_count);
	ret = it6805_reset_audio_logic(bar0);
	if (ret < 0)
		goto fail;
	ret = it6805_update_audio_clock(bar0);
	if (ret < 0)
		goto fail;
	ret = it6805_set_audio_tristate(bar0, false);
	if (ret < 0)
		goto fail;

	g_audio.state = IT6805_AUDIO_ON;
	g_audio.monitor_delay_polls = IT6805_AUDIO_MONITOR_DELAY_POLLS;
	g_audio.change_pending = false;
	g_audio.output_enabled = true;

	pr_info("IT6805 audio: output enabled (b0=%02x b1=%02x b2=%02x, %u Hz, %u ch)",
		b0, b1, b2, g_audio.sample_rate_hz, g_audio.channel_count);
	return 0;

fail:
	it6805_clear_audio_output(bar0);
	return ret;
}

static int it6805_probe_audio_ready(void *bar0)
{
	u8 reg10 = 0;
	u8 reg19 = 0;
	u8 regb1 = 0;
	u8 regb9 = 0;
	bool ready;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xb9, &regb9);
	if (ret < 0)
		return ret;
	if (regb9 > 0x3f) {
		ret = it6805_reset_audio_logic(bar0);
		if (ret < 0)
			return ret;
	}
	ret = it6805_read_reg(bar0, 0x19, &reg19);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x10, &reg10);
	if (ret < 0)
		return ret;
	/*
	 * The kernel acknowledges the latched common interrupts on every
	 * service pass (it6805_ack_common_irq_locked()).  The userspace path
	 * only did that inside the AVMute gate, so a pending AUDIO bit survived
	 * a mode change and kept this readiness check failing forever.  Clear
	 * just the audio bit (W1C) before evaluating the state.
	 */
	if (reg10 & IT6805_COMMON_IRQ_AUDIO) {
		ret = it6805_write_reg(bar0, 0x10, IT6805_COMMON_IRQ_AUDIO);
		if (ret < 0)
			return ret;
		reg10 &= ~IT6805_COMMON_IRQ_AUDIO;
		pr_info("IT6805 audio: cleared latched audio IRQ");
	}
	ret = it6805_read_reg(bar0, 0xb1, &regb1);
	if (ret < 0)
		return ret;

	ready = reg19 != 0xff && (reg19 & BIT(7)) &&
		(regb1 & BIT(7)) && !(reg10 & IT6805_COMMON_IRQ_AUDIO);
	if (!ready) {
		if (!g_audio.ready_wait_reported) {
			pr_info("IT6805 audio: readiness pending (0xb9=0x%02x 0x19=0x%02x 0x10=0x%02x 0xb1=0x%02x)",
				regb9, reg19, reg10, regb1);
			g_audio.ready_wait_reported = true;
		}
		g_audio.request_delay_polls = IT6805_AUDIO_REQUEST_DELAY_POLLS;
		return 0;
	}

	return it6805_start_audio_output(bar0);
}

static int it6805_refresh_audio_format(void *bar0)
{
	u8 b0 = 0;
	u8 b1 = 0;
	u8 b2 = 0;
	int ret;

	ret = it6805_read_audio_format(bar0, &b0, &b1, &b2);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x8c, BIT(3),
				 (b2 & BIT(1)) ? BIT(3) : 0);
	if (ret < 0)
		return ret;

	g_audio.format_b0 = b0 & 0xf0;
	g_audio.format_b1 = b1;
	g_audio.format_b2 = b2 & BIT(1);
	g_audio.channel_count = it6805_audio_channel_count(
		b0, b1, g_audio.channel_count);
	g_audio.format_valid = true;
	g_audio.measurement_refresh_pending = false;
	return 0;
}

static int it6805_restart_audio(void *bar0)
{
	int ret;

	ret = it6805_clear_audio_output(bar0);
	if (ret < 0)
		return ret;

	memset(&g_audio, 0, sizeof(g_audio));
	g_audio.state = IT6805_AUDIO_REQUEST;
	return 0;
}

static int it6805_monitor_audio(void *bar0)
{
	u32 bridge_rate_hz;
	u32 n = 0;
	u32 cts = 0;
	u8 b0 = 0;
	u8 b1 = 0;
	u8 b2 = 0;
	bool mismatch;
	int ret;

	if (g_audio.monitor_delay_polls) {
		g_audio.monitor_delay_polls--;
		return 0;
	}
	g_audio.monitor_delay_polls = IT6805_AUDIO_MONITOR_DELAY_POLLS;

	if (g_audio.measurement_refresh_pending)
		return it6805_refresh_audio_format(bar0);

	ret = it6805_read_audio_clock(bar0, &n, &cts);
	if (ret < 0)
		return ret;
	ret = it6805_read_audio_format(bar0, &b0, &b1, &b2);
	if (ret < 0)
		return ret;

	mismatch = (b0 & 0xf0) != g_audio.format_b0 ||
		   b1 != g_audio.format_b1 ||
		   (b2 & BIT(1)) != g_audio.format_b2 ||
		   it6805_audio_delta(n, g_audio.n) > IT6805_AUDIO_N_TOLERANCE;
	if (!mismatch &&
	    it6805_bridge_audio_rate(bar0, &bridge_rate_hz) == 0)
		mismatch = it6805_audio_rate_code(bridge_rate_hz / 1000U) !=
			   g_audio.selected_rate_code;
	if (!mismatch) {
		g_audio.cts = cts;
		return 0;
	}

	/* Source format changed: drop the old stream and request again. */
	return it6805_restart_audio(bar0);
}

static int it6805_handle_audio_change(void *bar0)
{
	u32 n = 0;
	u32 cts = 0;
	int ret;

	if (!g_audio.change_pending)
		return 0;
	if (g_audio.state != IT6805_AUDIO_ON) {
		g_audio.change_pending = false;
		return 0;
	}

	ret = it6805_read_audio_clock(bar0, &n, &cts);
	if (ret < 0)
		return ret;

	if (it6805_audio_delta(n, g_audio.n) > IT6805_AUDIO_N_TOLERANCE)
		return it6805_restart_audio(bar0);

	g_audio.n = n;
	g_audio.cts = cts;
	g_audio.change_pending = false;
	return 0;
}

static int it6805_service_audio(void *bar0)
{
	int ret;

	if (!g_audio_path_enabled)
		return 0;

	switch (g_audio.state) {
	case IT6805_AUDIO_OFF:
		return 0;
	case IT6805_AUDIO_REQUEST:
		ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
		if (!ret)
			ret = it6805_update_bits(bar0, 0x8c, BIT(4), BIT(4));
		if (!ret)
			ret = it6805_update_bits(bar0, 0x8c, BIT(4), 0);
		if (ret < 0) {
			gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
			it6805_update_bits(bar0, 0x8c, BIT(4), 0);
			return ret;
		}
		g_audio.format_b0 = 0;
		g_audio.format_b1 = 0;
		g_audio.format_b2 = 0;
		g_audio.format_valid = false;
		g_audio.measurement_refresh_pending = true;
		g_audio.ready_wait_reported = false;
		g_audio.state = IT6805_AUDIO_WAIT_READY;
		g_audio.request_delay_polls =
			IT6805_AUDIO_REQUEST_DELAY_POLLS;
		return 0;
	case IT6805_AUDIO_WAIT_READY:
		if (g_audio.request_delay_polls) {
			g_audio.request_delay_polls--;
			return 0;
		}
		return it6805_probe_audio_ready(bar0);
	case IT6805_AUDIO_ON:
		ret = it6805_handle_audio_change(bar0);
		if (ret < 0 || g_audio.state != IT6805_AUDIO_ON)
			return ret;
		return it6805_monitor_audio(bar0);
	}

	return -EINVAL;
}

static u32 it6805_nominal_audio_rate(void)
{
	switch (g_audio.selected_rate_code) {
	case IT6805_AUDIO_RATE_32_KHZ:
		return 32000;
	case IT6805_AUDIO_RATE_44_1_KHZ:
		return 44100;
	case IT6805_AUDIO_RATE_48_KHZ:
		return 48000;
	default:
		return g_audio.sample_rate_hz;
	}
}

static void it6805_snapshot_audio_format(
	struct gc573_user_audio_format *format)
{
	memset(format, 0, sizeof(*format));
	if (g_audio.state != IT6805_AUDIO_ON || !g_audio.format_valid ||
	    !g_audio.output_enabled || !g_audio.sample_rate_hz)
		return;

	format->rate_hz = it6805_nominal_audio_rate();
	format->channels = g_audio.channel_count;
	format->sample_bits = (g_audio.format_b2 & BIT(1)) ? 24 : 16;
	format->channel_allocation = g_audio.format_b1 & 0x3f;
	format->transport = g_audio.format_b0 >> 4;
	format->valid = true;
}

int gc573_user_it6805_audio_poll(void *bar0,
				 struct gc573_user_audio_format *format)
{
	int ret;

	if (!bar0 || !format)
		return -EINVAL;

	memset(format, 0, sizeof(*format));
	ret = it6805_service_audio(bar0);
	if (ret < 0 && ret != -EAGAIN)
		return ret;
	it6805_snapshot_audio_format(format);
	return 0;
}

int gc573_user_it6805_audio_disable(void *bar0)
{
	if (!bar0)
		return -EINVAL;

	g_audio_path_enabled = false;
	memset(&g_audio, 0, sizeof(g_audio));
	g_audio.state = IT6805_AUDIO_OFF;
	return it6805_clear_audio_output(bar0);
}

static void it6805_begin_audio_output(void *bar0, bool is_dvi)
{
	if (is_dvi) {
		gc573_user_it6805_audio_disable(bar0);
		return;
	}

	memset(&g_audio, 0, sizeof(g_audio));
	g_audio.state = IT6805_AUDIO_REQUEST;
	g_audio_path_enabled = true;
}

static int it6805_release_video_tristate(void *bar0)
{
	u8 reg98 = 0;
	u8 regc0 = 0;
	u8 reg = 0;
	u8 value = 0;
	u8 depth;
	bool write_tail = true;
	int ret;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0x98, &reg98);
	if (ret < 0)
		return ret;
	depth = reg98 & 0xf0;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc5, BIT(7), 0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc6, BIT(7), 0);
	if (ret < 0)
		return ret;
	ret = it6805_read_reg(bar0, 0xc0, &regc0);
	if (ret < 0)
		return ret;

	if (!(regc0 & BIT(0))) {
		bool is_10bit = (depth == 0x10 || depth == 0x50);
		bool is_12bit = (depth == 0x20 || depth == 0x60);

		reg = 0xc5;
		switch (regc0 & 0xc0) {
		case 0x80:
			value = is_12bit ? 0x23 : is_10bit ? 0x63 : 0x03;
			break;
		case 0x40:
		case 0x00:
			value = is_12bit ? 0x38 : is_10bit ? 0x78 : 0x18;
			break;
		default:
			write_tail = false;
			break;
		}
	} else {
		bool is_10bit = (depth == 0x10 || depth == 0x50);

		reg = 0xc6;
		value = is_10bit ? 0x40 : 0;
	}

	if (write_tail) {
		ret = it6805_write_reg(bar0, reg, value);
		if (ret < 0)
			return ret;
	}

	return gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
}

static int it6805_configure_video_output(void *bar0,
					 struct gc573_user_video_signal *sig)
{
	bool high_bandwidth = it6805_use_high_bandwidth(sig);
	u8 lvds_path_mode = 1;
	u8 lvds_pixel_mode = 0;
	u8 ttl_pixel_mode = 0;
	u8 regc4 = 0;
	int ret;

	if (sig->colorspace == GC573_USER_COLORSPACE_YCBCR420) {
		lvds_path_mode = 2;
		lvds_pixel_mode = 1;
	} else if (high_bandwidth) {
		ttl_pixel_mode = 1;
		lvds_path_mode = 2;
		lvds_pixel_mode = 1;
		regc4 = BIT(4);
	}

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x64, BIT(2), 0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x64, BIT(1), BIT(1));
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x64, BIT(1), 0);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_5);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x20, BIT(6), BIT(6));
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc0, BIT(0), 0);
	if (ret < 0)
		return ret;
	ret = it6805_apply_ttl_video_path(bar0, ttl_pixel_mode, false,
					  sig->colorspace);
	if (ret < 0)
		return ret;
	ret = it6805_apply_lvds_video_path(bar0, lvds_path_mode);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0xc4, regc4);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x64, BIT(2), 0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x64, BIT(1), BIT(1));
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x64, BIT(1), 0);
	if (ret < 0)
		return ret;

	sig->dual_pixel = (lvds_pixel_mode == 1);
	sig->ddr = (ttl_pixel_mode == 1);
	return 0;
}

static int it6805_enable_video_output(void *bar0,
				      struct gc573_user_video_signal *sig)
{
	bool is_dvi = false;
	u8 reg48 = 0;
	int ret;

	ret = it6805_detect_dvi(bar0, &is_dvi);
	if (ret < 0)
		return ret;
	ret = it6805_set_pixel_clock_gate(bar0, sig->pixel_clock_khz);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x6b, 0x30,
				 is_dvi ? BIT(4) : (u8)(sig->colorspace << 4));
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x6b, BIT(0), is_dvi ? BIT(0) : 0);
	if (ret < 0)
		return ret;
	if (is_dvi) {
		ret = it6805_read_reg(bar0, 0x48, &reg48);
		if (ret < 0)
			return ret;
		sig->colorimetry = reg48 < 0x34 ? 2 : 1;
	}
	ret = it6805_apply_ttl_video_path(bar0, sig->ddr ? 1 : 0, true,
					  sig->colorspace);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x6e, 0xa0);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x86, 0x00);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x6c, 0x03, 0);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_write_reg(bar0, 0x85, 0x00);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		return ret;
	ret = it6805_set_color_depth(bar0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x4f, 0xa0, 0xa0);
	if (ret < 0)
		return ret;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_1);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0xc5, BIT(7), BIT(7));
	if (ret < 0)
		return ret;
	ret = it6805_clear_audio_output(bar0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x22, BIT(0), BIT(0));
	if (ret < 0)
		return ret;
	msleep(1);
	ret = it6805_update_bits(bar0, 0x22, BIT(0), 0);
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x10, BIT(1), BIT(1));
	if (ret < 0)
		return ret;
	ret = it6805_update_bits(bar0, 0x12, BIT(7), BIT(7));
	if (ret < 0)
		return ret;
	ret = it6805_release_video_tristate(bar0);
	if (ret < 0)
		return ret;

	sig->is_dvi = is_dvi;
	return 0;
}

int gc573_user_it6805_apply_signal_output(void *bar0,
					  struct gc573_user_video_signal *sig)
{
	int ret;

	if (!bar0 || !sig)
		return -EINVAL;
	if (!sig->scdt_locked || !sig->avi_valid || !sig->width ||
	    !sig->height || !sig->pixel_clock_khz || !sig->frame_rate_hz)
		return -ENOLINK;

	ret = it6805_configure_video_output(bar0, sig);
	if (ret < 0)
		return ret;
	ret = it6805_enable_video_output(bar0, sig);
	if (ret < 0)
		return ret;

	/*
	 * The kernel arms the audio request state at the end of its video
	 * output enable path.  DVI sources have no audio path.
	 */
	it6805_begin_audio_output(bar0, sig->is_dvi);

	ret = gc573_user_it6805_unmute(bar0);
	if (ret == -EAGAIN) {
		pr_info("IT6805 video unmute deferred (source AVMute still set)");
		ret = 0;
	}

	return ret;
}

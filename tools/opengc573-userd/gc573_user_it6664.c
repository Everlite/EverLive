/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_it6664.c — Userspace IT6664 HDMI splitter bring-up
 *
 * Mirrors the initialization subset of open_gc573/gc573-it6664-core.c and
 * gc573-it6664-tx.c:
 *   - switch reset and internal map unlock
 *   - reference clock calibration from SIPROM
 *   - RX CAOF calibration and RX register setup
 *   - TX path reset and switch routing
 *   - upstream EDID publication and source-facing HPD
 *   - TX1 power-on / AFE calibration once the RX locks
 */

#include "gc573_user_it6664.h"
#include "gc573_user_i2c.h"
#include "gc573_user_hw.h"
#include "gc573_user_edid.h"
#include <unistd.h>

#define IT6664_SWITCH_ADDR		0x2c
#define IT6664_RX_ADDR			0x38
#define IT6664_EDID_ADDR		0x6c
#define IT6664_TX_COMMON_ADDR		0x4b
#define IT6664_TX0_ADDR			0x34
#define IT6664_TX1_ADDR			0x35
#define IT6664_TX2_ADDR			0x36
#define IT6664_TX3_ADDR			0x37

#define IT6664_SW_REG_BANK		0x0f
#define IT6664_RX_REG_BANK		0x0f
#define IT6664_RX_REG_CAOF_STATUS	0x08
#define IT6664_RX_REG_STATUS		0x13
#define IT6664_RX_REG_STATUS_14		0x14
#define IT6664_RX_REG_SCDT		0x19
#define IT6664_RX_REG_EDID_ENABLE	0x34
#define IT6664_RX_REG_EDID_ADDR_HI	0xc6
#define IT6664_RX_REG_EDID_ADDR_LO	0xc7
#define IT6664_RX_REG_EDID_PORT		0xc8
#define IT6664_RX_REG_EDID_CHECKSUM0	0xc9
#define IT6664_RX_REG_EDID_CHECKSUM1	0xca

#define IT6664_RX_BANK_0		0x00
#define IT6664_RX_BANK_2		0x02
#define IT6664_RX_BANK_3		0x03

#define IT6664_TX_REG_STATUS		0x03
#define IT6664_TX_REG_IRQ_BASE		0x10
#define IT6664_TX_REG_DDC_ENABLE	0x28
#define IT6664_TX_REG_DDC_SLAVE		0x29
#define IT6664_TX_REG_DDC_OFFSET	0x2a
#define IT6664_TX_REG_DDC_COUNT		0x2b
#define IT6664_TX_REG_DDC_HEADER	0x2c
#define IT6664_TX_REG_DDC_COMMAND	0x2e
#define IT6664_TX_REG_DDC_STATUS	0x2f
#define IT6664_TX_REG_DDC_FIFO		0x30
#define IT6664_TX_SCDC_SLAVE		0xa8
#define IT6664_TX_SCDC_VERSION		0x02
#define IT6664_TX_SCDC_TMDS_CONFIG	0x20
#define IT6664_TX_SCDC_RETRIES		11
#define IT6664_TX_SINK_MAX_TMDS_KHZ	600000

#define IT6664_TX_COMMON_REG_TIMER_LO	0x11
#define IT6664_TX_COMMON_REG_TIMER_HI	0x12
#define IT6664_TX_COMMON_REG_TIMER_TOP	0x13
#define IT6664_TX_COMMON_REG_50		0x50

#define IT6664_EDID_BLOCK_SIZE		128
#define IT6664_EDID_SIZE		256

#define IT6664_HEAVY_INIT_DELAY_MS	(70 + 20 + 30)
#define IT6664_RX_CAOF_POLL_COUNT	0x1f
#define IT6664_RCLK_MIN_KHZ		10000
#define IT6664_RCLK_MAX_KHZ		34000
#define IT6664_RCLK_DEFAULT_KHZ		22000
#define IT6664_TX_PCLK_SAMPLES		10
#define IT6664_TX_PCLK_MAX_KHZ		621000

struct it6664_reg_update {
	u8 reg;
	u8 mask;
	u8 value;
};

struct it6664_sw_sequence {
	u8 reg;
	u8 value;
};

static const struct it6664_sw_sequence it6664_siprom_setup[] = {
	{ 0xff, 0xc3 },
	{ 0xff, 0xa5 },
	{ 0x0f, 0x00 },
	{ 0x5f, 0x04 },
	{ 0x5f, 0x05 },
	{ 0x58, 0x12 },
	{ 0x58, 0x02 },
	{ 0x57, 0x01 },
	{ 0x50, 0x00 },
	{ 0x51, 0x00 },
	{ 0x54, 0x04 },
};

static const struct it6664_sw_sequence it6664_siprom_cleanup[] = {
	{ 0x5f, 0x00 },
	{ 0x0f, 0x00 },
	{ 0xff, 0xff },
};

static const struct it6664_sw_sequence it6664_post_rclk_sequence[] = {
	{ 0x2c, 0x69 },
	{ 0x2d, 0x6b },
	{ 0x2e, 0x6d },
	{ 0x2f, 0x6f },
};

static const struct it6664_sw_sequence it6664_rx_reset_sequence[] = {
	{ 0x22, 0x08 },
	{ 0x23, 0x01 },
	{ 0x22, 0x17 },
	{ 0x24, 0xf8 },
	{ 0x23, 0xa0 },
	{ 0x22, 0x00 },
	{ 0x24, 0x00 },
};

static const struct it6664_reg_update it6664_caof_prefix_updates[] = {
	{ IT6664_RX_REG_BANK, 0x03, IT6664_RX_BANK_0 },
	{ 0x29, 0x01, 0x01 },
	{ 0x2a, 0x41, 0x41 },
	{ IT6664_RX_REG_BANK, 0x03, IT6664_RX_BANK_3 },
	{ 0x3a, 0x80, 0x00 },
	{ 0x3b, 0xc0, 0x00 },
	{ 0xa0, 0x80, 0x80 },
	{ 0xa1, 0x80, 0x80 },
	{ 0xa2, 0x80, 0x80 },
	{ 0xa7, 0x10, 0x10 },
	{ 0x48, 0x80, 0x80 },
	{ IT6664_RX_REG_BANK, 0x03, IT6664_RX_BANK_0 },
	{ 0x2a, 0x40, 0x00 },
	{ 0x24, 0x04, 0x04 },
};

static const struct it6664_sw_sequence it6664_caof_zero_sequence[] = {
	{ 0x25, 0x00 },
	{ 0x26, 0x00 },
	{ 0x27, 0x00 },
	{ 0x28, 0x00 },
};

static const struct it6664_reg_update it6664_caof_prefix_tail[] = {
	{ 0x3c, 0x10, 0x00 },
	{ IT6664_RX_REG_BANK, 0x03, IT6664_RX_BANK_3 },
	{ 0x3a, 0x80, 0x80 },
	{ IT6664_RX_REG_BANK, 0x03, IT6664_RX_BANK_0 },
};

static const struct it6664_reg_update it6664_caof_post_updates[] = {
	{ 0x3a, 0x80, 0x00 },
	{ 0xa0, 0x80, 0x00 },
	{ 0xa1, 0x80, 0x00 },
	{ 0xa2, 0x80, 0x00 },
	{ IT6664_RX_REG_BANK, 0x03, IT6664_RX_BANK_0 },
	{ IT6664_RX_REG_CAOF_STATUS, 0x30, 0x30 },
	{ 0x29, 0x01, 0x00 },
	{ 0x24, 0x04, 0x00 },
	{ 0x3c, 0x10, 0x10 },
	{ 0xce, 0x20, 0x00 },
};

static const u8 it6664_tx_irq_enable[] = {
	0xc0, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static bool g_tx1_powered;
static bool g_tx2_powered;
static bool g_tx2_output_enabled;
static bool g_tx2_error_reported;
static bool g_tx2_hpd_last;
static bool g_rx_locked;
static bool g_rx_last_5v;
static u32 g_rclk_khz;
static u8 g_rx_color_depth;
static u32 g_tx1_pclk_khz;
static u32 g_tx2_pclk_khz;

/* Defined next to the TX output helpers; used earlier by enable_source(). */
static int it6664_arm_tx2(void *bar0);
static int it6664_assert_tx2_gate(void *bar0);

static int it6664_raw_read(void *bar0, u8 addr, u8 reg, u8 *val)
{
	return gc573_user_i2c_read(bar0, GC573_I2C_BUS_PRIMARY, addr,
				   reg, 1, val, 1);
}

static int it6664_raw_write(void *bar0, u8 addr, u8 reg, u8 val)
{
	return gc573_user_i2c_write(bar0, GC573_I2C_BUS_PRIMARY, addr,
				    reg, 1, &val, 1);
}

static int it6664_update_bits(void *bar0, u8 addr, u8 reg, u8 mask, u8 val)
{
	u8 cur = 0;
	int ret = it6664_raw_read(bar0, addr, reg, &cur);

	if (ret < 0)
		return ret;

	u8 new_val = (cur & ~mask) | (val & mask);
	if (new_val == cur)
		return 0;

	return it6664_raw_write(bar0, addr, reg, new_val);
}

static int sw_read(void *bar0, u8 reg, u8 *val)
{
	return it6664_raw_read(bar0, IT6664_SWITCH_ADDR, reg, val);
}

static int sw_write(void *bar0, u8 reg, u8 val)
{
	return it6664_raw_write(bar0, IT6664_SWITCH_ADDR, reg, val);
}

static int sw_update(void *bar0, u8 reg, u8 mask, u8 val)
{
	return it6664_update_bits(bar0, IT6664_SWITCH_ADDR, reg, mask, val);
}

static int sw_select_bank(void *bar0, u8 bank)
{
	return sw_update(bar0, IT6664_SW_REG_BANK, 0x01, bank & 0x01);
}

static int rx_read(void *bar0, u8 bank, u8 reg, u8 *val)
{
	int ret = it6664_raw_write(bar0, IT6664_RX_ADDR,
				   IT6664_RX_REG_BANK, bank & 0x03);

	if (ret < 0)
		return ret;

	return it6664_raw_read(bar0, IT6664_RX_ADDR, reg, val);
}

static int rx_write(void *bar0, u8 bank, u8 reg, u8 val)
{
	int ret = it6664_raw_write(bar0, IT6664_RX_ADDR,
				   IT6664_RX_REG_BANK, bank & 0x03);

	if (ret < 0)
		return ret;

	return it6664_raw_write(bar0, IT6664_RX_ADDR, reg, val);
}

static int rx_update(void *bar0, u8 bank, u8 reg, u8 mask, u8 val)
{
	u8 cur = 0;
	int ret = rx_read(bar0, bank, reg, &cur);

	if (ret < 0)
		return ret;

	u8 new_val = (cur & ~mask) | (val & mask);
	if (new_val == cur)
		return 0;

	return rx_write(bar0, bank, reg, new_val);
}

static int tx_read(void *bar0, u8 addr, u8 reg, u8 *val)
{
	return it6664_raw_read(bar0, addr, reg, val);
}

static int tx_write(void *bar0, u8 addr, u8 reg, u8 val)
{
	return it6664_raw_write(bar0, addr, reg, val);
}

static int tx_update(void *bar0, u8 addr, u8 reg, u8 mask, u8 val)
{
	return it6664_update_bits(bar0, addr, reg, mask, val);
}

static int apply_rx_bank_updates(void *bar0, u8 *bank,
				 const struct it6664_reg_update *updates,
				 size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const struct it6664_reg_update *up = &updates[i];
		int ret;

		if (up->reg == IT6664_RX_REG_BANK) {
			*bank = up->value & 0x03;
			ret = it6664_raw_write(bar0, IT6664_RX_ADDR,
					       IT6664_RX_REG_BANK, *bank);
		} else {
			ret = rx_update(bar0, *bank, up->reg, up->mask,
					up->value);
		}
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int sw_write_sequence(void *bar0,
			     const struct it6664_sw_sequence *seq,
			     size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int ret = sw_write(bar0, seq[i].reg, seq[i].value);

		if (ret < 0)
			return ret;
	}
	return 0;
}

static int rx_write_sequence(void *bar0, u8 bank,
			     const struct it6664_sw_sequence *seq,
			     size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int ret = rx_write(bar0, bank, seq[i].reg, seq[i].value);

		if (ret < 0)
			return ret;
	}
	return 0;
}

static int it6664_identity_valid(const u8 *id)
{
	return id[0] == 0x54 && id[1] == 0x49 &&
	       (id[2] == 0x63 || id[2] == 0x64) && id[3] == 0x66;
}

static int it6664_apply_pre_rclk(void *bar0)
{
	int ret;

	ret = sw_write(bar0, 0x0a, 0x01);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x0a, 0x00);
	if (ret < 0)
		return ret;

	/* Bank-1 RX latch right after switch reset. */
	ret = sw_select_bank(bar0, 1);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x73, 0x04, 0x04);
	if (ret < 0)
		return ret;
	/*
	 * Bank-1 bit 7 lets the chip answer SCDC (scramble + 1/40 clock)
	 * ahead of firmware.  Reg 0x20 bits 6:3 are the matching flag.
	 * 1440p120 is 483 MHz and the PS5 drops the link without this.
	 */
	ret = sw_update(bar0, 0x1d, 0x80, 0x80);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x20, 0x78, 0x78);
	if (ret < 0)
		return ret;
	ret = sw_select_bank(bar0, 0);
	if (ret < 0)
		return ret;

	ret = sw_write(bar0, 0x10, 0x6e);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0xf0, 0x71);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x0e, 0x07, 0x00);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x08, 0x0f, 0x0f);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0xf0, 0x71);
	if (ret < 0)
		return ret;

	return sw_write(bar0, 0xf1, 0x97);
}

static int it6664_siprom_set_address(void *bar0, u16 address)
{
	int ret;

	ret = sw_write(bar0, 0x50, (address >> 8) & 0x0f);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x51, address & 0xff);
	if (ret < 0)
		return ret;

	return sw_write(bar0, 0x54, 0x04);
}

static int it6664_read_siprom_pair(void *bar0, u8 *low, u8 *high)
{
	int ret = sw_read(bar0, 0x61, low);

	if (ret < 0)
		return ret;
	return sw_read(bar0, 0x62, high);
}

static int it6664_read_siprom(void *bar0, u32 *raw)
{
	u8 low0 = 0, high0 = 0, low1 = 0, high1 = 0;
	u16 word0, word1, address;
	int cleanup_ret;
	int ret;

	ret = sw_write_sequence(bar0, it6664_siprom_setup,
				ARRAY_SIZE(it6664_siprom_setup));
	if (ret < 0)
		goto cleanup;
	ret = it6664_read_siprom_pair(bar0, &low0, &high0);
	if (ret < 0)
		goto cleanup;

	ret = it6664_siprom_set_address(bar0, 0x0001);
	if (ret < 0)
		goto cleanup;
	ret = it6664_read_siprom_pair(bar0, &low1, &high1);
	if (ret < 0)
		goto cleanup;

	word0 = ((u16)high0 << 8) | low0;
	word1 = ((u16)high1 << 8) | low1;
	address = (word0 == 0xffff && word1 == 0x0000) ? 0x04b0 : 0x00b0;

	ret = it6664_siprom_set_address(bar0, address);
	if (ret < 0)
		goto cleanup;
	ret = it6664_read_siprom_pair(bar0, &low0, &high0);
	if (ret < 0)
		goto cleanup;

	ret = it6664_siprom_set_address(bar0, address + 1);
	if (ret < 0)
		goto cleanup;
	ret = it6664_read_siprom_pair(bar0, &low1, &high1);
	if (ret < 0)
		goto cleanup;

	*raw = low0 | ((u32)high0 << 8) | ((u32)low1 << 16);
	if (high1 > 0xbf)
		*raw /= 100;

cleanup:
	cleanup_ret = sw_write_sequence(bar0, it6664_siprom_cleanup,
					ARRAY_SIZE(it6664_siprom_cleanup));
	if (ret >= 0)
		ret = cleanup_ret;
	return ret;
}

static int it6664_calibrate_rclk(void *bar0)
{
	u32 raw = 0;
	u32 rclk_khz;
	u32 timer;
	u8 fraction, integer;
	int ret;

	ret = it6664_read_siprom(bar0, &raw);
	if (ret < 0)
		return ret;

	if (raw >= IT6664_RCLK_MIN_KHZ && raw <= IT6664_RCLK_MAX_KHZ)
		rclk_khz = raw;
	else
		rclk_khz = IT6664_RCLK_DEFAULT_KHZ;

	timer = rclk_khz * 10;
	ret = tx_write(bar0, IT6664_TX_COMMON_ADDR,
		       IT6664_TX_COMMON_REG_TIMER_LO, timer & 0xff);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, IT6664_TX_COMMON_ADDR,
		       IT6664_TX_COMMON_REG_TIMER_HI, (timer >> 8) & 0xff);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, IT6664_TX_COMMON_ADDR,
			IT6664_TX_COMMON_REG_TIMER_TOP, 0x03,
			(timer >> 16) & 0x03);
	if (ret < 0)
		return ret;

	integer = (rclk_khz / 1000) & 0x3f;
	ret = sw_update(bar0, 0x1e, 0x3f, integer);
	if (ret < 0)
		return ret;
	fraction = ((rclk_khz % 1000) * 0x100) / 1000;
	ret = sw_write(bar0, 0x1f, fraction);
	if (ret < 0)
		return ret;

	ret = tx_update(bar0, IT6664_TX_COMMON_ADDR,
			IT6664_TX_COMMON_REG_50, 0x04, 0x00);
	if (ret < 0)
		return ret;
	for (size_t i = 0; i < ARRAY_SIZE(it6664_post_rclk_sequence); i++) {
		ret = tx_write(bar0, IT6664_TX_COMMON_ADDR,
			       it6664_post_rclk_sequence[i].reg,
			       it6664_post_rclk_sequence[i].value);
		if (ret < 0)
			return ret;
	}

	g_rclk_khz = rclk_khz;
	pr_info("IT6664 RCLK calibration: SIPROM raw %u kHz -> %u kHz",
		raw, rclk_khz);
	return 0;
}

static int it6664_reset_rx(void *bar0)
{
	for (size_t i = 0; i < ARRAY_SIZE(it6664_rx_reset_sequence); i++) {
		int ret;

		if (i == 4)
			usleep_range(10000, 11000);
		ret = rx_write(bar0, IT6664_RX_BANK_0,
			       it6664_rx_reset_sequence[i].reg,
			       it6664_rx_reset_sequence[i].value);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int it6664_prepare_rx_caof(void *bar0)
{
	u8 bank = IT6664_RX_BANK_0;
	int ret;

	ret = apply_rx_bank_updates(bar0, &bank, it6664_caof_prefix_updates,
				    ARRAY_SIZE(it6664_caof_prefix_updates));
	if (ret < 0)
		return ret;

	ret = rx_write_sequence(bar0, bank, it6664_caof_zero_sequence,
				ARRAY_SIZE(it6664_caof_zero_sequence));
	if (ret < 0)
		return ret;

	return apply_rx_bank_updates(bar0, &bank, it6664_caof_prefix_tail,
				     ARRAY_SIZE(it6664_caof_prefix_tail));
}

static int it6664_caof_pulse(void *bar0)
{
	int ret = rx_update(bar0, IT6664_RX_BANK_0, 0x2a, 0x40, 0x40);

	if (ret < 0)
		return ret;
	return rx_update(bar0, IT6664_RX_BANK_0, 0x2a, 0x40, 0x00);
}

static int it6664_wait_rx_caof(void *bar0, u8 *final_status,
			       unsigned int *polls)
{
	u8 status = 0;
	bool timed_out = true;
	int ret;

	for (unsigned int attempt = 0; attempt < IT6664_RX_CAOF_POLL_COUNT;
	     attempt++) {
		ret = rx_read(bar0, IT6664_RX_BANK_0,
			      IT6664_RX_REG_CAOF_STATUS, &status);
		if (ret < 0)
			return ret;
		*polls = attempt + 1;
		if (status & 0x30) {
			timed_out = false;
			break;
		}
		if (attempt > 2) {
			ret = it6664_caof_pulse(bar0);
			if (ret < 0)
				return ret;
		}
	}

	*final_status = status;
	if (!timed_out)
		return 0;

	pr_warn("IT6664 RX CAOF timed out (status 0x%02x)", status);
	ret = rx_update(bar0, IT6664_RX_BANK_3, 0x3a, 0x80, 0x00);
	if (ret < 0)
		return ret;
	return it6664_caof_pulse(bar0);
}

static int it6664_finish_rx_caof(void *bar0, u8 *reg5a, u8 *reg59)
{
	u8 bank = IT6664_RX_BANK_3;
	int ret;

	ret = rx_read(bar0, IT6664_RX_BANK_3, 0x5a, reg5a);
	if (ret < 0)
		return ret;
	ret = rx_read(bar0, IT6664_RX_BANK_3, 0x59, reg59);
	if (ret < 0)
		return ret;

	return apply_rx_bank_updates(bar0, &bank, it6664_caof_post_updates,
				     ARRAY_SIZE(it6664_caof_post_updates));
}

static int it6664_initialize_rx_caof(void *bar0)
{
	u8 status = 0, reg5a = 0, reg59 = 0;
	unsigned int polls = 0;
	int ret;

	ret = it6664_reset_rx(bar0);
	if (ret < 0)
		return ret;
	ret = it6664_prepare_rx_caof(bar0);
	if (ret < 0)
		return ret;
	ret = it6664_wait_rx_caof(bar0, &status, &polls);
	if (ret < 0)
		return ret;
	ret = it6664_finish_rx_caof(bar0, &reg5a, &reg59);
	if (ret < 0)
		return ret;

	pr_info("IT6664 RX CAOF: status 0x%02x polls=%u post=0x%02x/0x%02x",
		status, polls, reg5a, reg59);
	return 0;
}

static int it6664_initialize_rx_registers(void *bar0)
{
	int ret;

	ret = rx_write(bar0, IT6664_RX_BANK_0, 0x56, 0xff);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_0, 0x57, 0xff);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_3, 0xa8, 0x08, 0x08);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_3, 0xa7, 0x40, 0x40);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_3, 0x26, 0x20, 0x00);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_3, 0x27, 0x9f);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_3, 0x28, 0x9f);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_3, 0x29, 0x9f);
	if (ret < 0)
		return ret;

	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x28, 0x59, 0x59);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x2a, 0x01, 0x01);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x43, 0x02, 0x00);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x44, 0x3f, 0x19);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x3c, 0x01, 0x00);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_0, 0x45, 0xdf);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x46, 0x3f, 0x15);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x47, 0xff, 0x88);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_0, 0x49, 0xe1);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_0, 0x23, 0xa0);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_0, 0x53, 0x0f);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_0, 0xe3, 0x04);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0xce, 0x80, 0x00);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x3c, 0x20, 0x00);
	if (ret < 0)
		return ret;

	ret = rx_update(bar0, IT6664_RX_BANK_3, 0xe3, 0x01, 0x01);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_3, 0xe3, 0x06, 0x06);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_3, 0xf0, 0xa0);
	if (ret < 0)
		return ret;

	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x28, 0x88, 0x88);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x3b, 0x20, 0x20);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_0, 0x26, 0xff);
	if (ret < 0)
		return ret;
	return rx_update(bar0, IT6664_RX_BANK_0, 0x42, 0x20, 0x00);
}

/*
 * Official RXHDCP_OFF bits from the GC570D Windows trace: leave the
 * splitter receiver in cleartext mode so a source with HDCP disabled
 * stops sending Set AVMute. Without this the IT6805 stays muted and
 * every DMA frame is legal-range black.
 */
static int it6664_set_rx_hdcp_off(void *bar0)
{
	int ret;

	ret = sw_select_bank(bar0, 0);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x0a, 0x04, 0x04);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x23, 0x42, 0x42);
	if (ret < 0)
		return ret;
	return sw_update(bar0, 0x0c, 0x04, 0x04);
}

static int it6664_rx_set_hpd(void *bar0, bool high)
{
	u8 value = 0;
	int ret;

	if (high) {
		ret = rx_read(bar0, IT6664_RX_BANK_0,
			      IT6664_RX_REG_STATUS, &value);
		if (ret < 0)
			return ret;
		if (!(value & 0x01))
			return 0;
	}

	ret = rx_read(bar0, IT6664_RX_BANK_3, 0xab, &value);
	if (ret < 0)
		return ret;
	if (high && value != 0xca) {
		ret = rx_write(bar0, IT6664_RX_BANK_3, 0xab, 0xca);
		if (ret < 0)
			return ret;
	} else if (!high && value == 0xca) {
		ret = rx_write(bar0, IT6664_RX_BANK_3, 0xab, 0x4a);
		if (ret < 0)
			return ret;
		ret = rx_write(bar0, IT6664_RX_BANK_3, 0xab, 0x00);
		if (ret < 0)
			return ret;
		ret = rx_write(bar0, IT6664_RX_BANK_3, 0xac, 0x00);
		if (ret < 0)
			return ret;
	}

	ret = rx_write(bar0, IT6664_RX_BANK_0, 0x26, high ? 0x00 : 0xff);
	if (ret < 0)
		return ret;
	return rx_write(bar0, IT6664_RX_BANK_0, 0x55, high ? 0xff : 0x00);
}

static int it6664_quiesce_rx(void *bar0)
{
	int ret;

	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x53, 0xe0, 0x00);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x54, 0xff, 0x00);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x55, 0x07, 0x00);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x57, 0x0f, 0x00);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0xc5, 0x10, 0x10);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0xc5, 0x10, 0x00);
	if (ret < 0)
		return ret;

	ret = sw_update(bar0, 0x0a, 0x04, 0x04);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);
	ret = sw_update(bar0, 0x0a, 0x04, 0x00);
	if (ret < 0)
		return ret;

	ret = it6664_rx_set_hpd(bar0, false);
	if (ret < 0)
		return ret;

	ret = rx_write(bar0, IT6664_RX_BANK_3, 0x27, 0x9f);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_3, 0x28, 0x9f);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_3, 0x29, 0x9f);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_3, 0x20, 0x1b);
	if (ret < 0)
		return ret;
	return rx_write(bar0, IT6664_RX_BANK_3, 0x21, 0x03);
}

static int it6664_initialize_rx_power_state(void *bar0)
{
	int ret;

	ret = rx_write(bar0, IT6664_RX_BANK_3, 0xab, 0x4a);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_3, 0xac, 0x40);
	if (ret < 0)
		return ret;

	return it6664_quiesce_rx(bar0);
}

static int it6664_reset_tx_video_clock(void *bar0, u8 addr)
{
	int ret;

	ret = tx_write(bar0, addr, 0x01, 0x06);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x94, 0x01, 0x01);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x94, 0x01, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x01, 0x04);
	if (ret < 0)
		return ret;
	return tx_write(bar0, addr, 0x01, 0x00);
}

static int it6664_reset_tx_port(void *bar0, u8 addr, unsigned int port)
{
	int ret;

	ret = it6664_reset_tx_video_clock(bar0, addr);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x01, 0x20, 0x20);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x01, 0x20, 0x00);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x0c, 0x10 << port);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x0c, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x18, 0x80, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x19, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x1a, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x1b, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x1c, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x35, 0x10, 0x10);
	if (ret < 0)
		return ret;
	return tx_update(bar0, addr, 0x35, 0x10, 0x00);
}

static int it6664_initialize_tx_port(void *bar0, u8 addr)
{
	int ret;

	ret = tx_update(bar0, addr, 0x08, 0x1c, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x02, 0x02, 0x02);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x41, 0x01, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc0, 0x01, 0x01);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x34, 0xc0, 0x80);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x35, 0x03, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x3a, 0xfc, 0x90);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x93, 0xff, 0x40);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x94, 0x3e, 0x26);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc0, 0x10, 0x10);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc1, 0x04, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc3, 0x0f, 0x01);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x18, 0x03, 0x03);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x19, 0x07);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x1a, 0x03);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x1b, 0xff);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x1c, 0x03);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x88, 0x54);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x8a, 0x00);
	if (ret < 0)
		return ret;
	return tx_write(bar0, addr, 0x8b, 0x07);
}

static int it6664_reset_inactive_tx_paths(void *bar0)
{
	u8 status1 = 0, status2 = 0;
	int ret;

	ret = tx_read(bar0, IT6664_TX1_ADDR, IT6664_TX_REG_STATUS, &status1);
	if (ret < 0)
		return ret;
	ret = tx_read(bar0, IT6664_TX2_ADDR, IT6664_TX_REG_STATUS, &status2);
	if (ret < 0)
		return ret;
	if ((status1 | status2) & 0x01)
		return 0;

	ret = sw_update(bar0, 0x1a, 0x02, 0x00);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x19, 0x0f);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x1d, 0xaf);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x19, 0x3f);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x07, 0xff);
	if (ret < 0)
		return ret;
	return sw_update(bar0, 0x1a, 0x02, 0x02);
}

static int it6664_power_down_tx_port(void *bar0, unsigned int port)
{
	static const u8 addr_map[] = {
		IT6664_TX0_ADDR, IT6664_TX1_ADDR,
		IT6664_TX2_ADDR, IT6664_TX3_ADDR,
	};
	u8 addr = addr_map[port];
	int ret;

	ret = sw_update(bar0, 0x08, BIT(port), 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x18, 0xdc, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x19, 0x07, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x1a, 0xff, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x1b, 0xff, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x1c, 0xff, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x84, 0x60, 0x60);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x86, 0x08, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x88, 0x03, 0x03);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x01, 0x26, 0x26);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x0d, 0x03 << (port * 2), 0x00);
	if (ret < 0)
		return ret;
	ret = it6664_reset_inactive_tx_paths(bar0);
	if (ret < 0)
		return ret;
	return it6664_reset_tx_port(bar0, addr, port);
}

static int it6664_initialize_tx_reset_path(void *bar0)
{
	int ret;

	ret = rx_update(bar0, IT6664_RX_BANK_0, 0xc5, 0x01, 0x01);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, IT6664_TX_COMMON_ADDR, 0x20, 0x02);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, IT6664_TX_COMMON_ADDR, 0x20, 0x00);
	if (ret < 0)
		return ret;
	ret = it6664_power_down_tx_port(bar0, 0);
	if (ret < 0)
		return ret;
	ret = it6664_power_down_tx_port(bar0, 3);
	if (ret < 0)
		return ret;

	for (unsigned int port = 1; port <= 2; port++) {
		u8 addr = port == 1 ? IT6664_TX1_ADDR : IT6664_TX2_ADDR;

		ret = tx_update(bar0, addr, 0xc1, 0x01, 0x01);
		if (ret < 0)
			return ret;
		ret = tx_update(bar0, addr, 0x01, 0x01, 0x01);
		if (ret < 0)
			return ret;
		ret = tx_update(bar0, addr, 0x01, 0x01, 0x00);
		if (ret < 0)
			return ret;
		ret = it6664_initialize_tx_port(bar0, addr);
		if (ret < 0)
			return ret;
		ret = it6664_reset_tx_port(bar0, addr, port);
		if (ret < 0)
			return ret;
		ret = it6664_power_down_tx_port(bar0, port);
		if (ret < 0)
			return ret;
	}

	return tx_update(bar0, IT6664_TX_COMMON_ADDR, 0x15, 0x08, 0x08);
}

static int it6664_configure_tx_switch(void *bar0)
{
	int ret;

	ret = tx_write(bar0, IT6664_TX0_ADDR, 0x03, 0x03);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, IT6664_TX0_ADDR, 0x84, 0x60);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, IT6664_TX0_ADDR, 0x86, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, IT6664_TX0_ADDR, 0x88, 0x0b);
	if (ret < 0)
		return ret;

	ret = tx_write(bar0, IT6664_TX3_ADDR, 0x84, 0x60);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, IT6664_TX3_ADDR, 0x86, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, IT6664_TX3_ADDR, 0x88, 0x0b);
	if (ret < 0)
		return ret;

	ret = sw_update(bar0, 0x08, 0x0f, 0x0f);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x0d, 0xff, 0x00);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x6b, 0x3c, 0x00);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x6c, 0x38, 0x20);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x18, 0x10, 0x10);
	if (ret < 0)
		return ret;

	ret = sw_select_bank(bar0, 1);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x10, 0x49, 0x41);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x1d, 0x80, 0x80);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x20, 0x78, 0x78);
	if (ret < 0)
		return ret;
	ret = sw_select_bank(bar0, 0);
	if (ret < 0)
		return ret;

	ret = sw_update(bar0, 0x19, 0x3f, 0x0f);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x2b, 0xff);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x2d, 0x0f);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x2e, 0xff);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x30, 0x0f);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x6d, 0x30, 0x00);
	if (ret < 0)
		return ret;
	usleep_range(10000, 11000);

	for (unsigned int port = 0; port < 4; port++) {
		static const u8 addr_map[] = {
			IT6664_TX0_ADDR, IT6664_TX1_ADDR,
			IT6664_TX2_ADDR, IT6664_TX3_ADDR,
		};
		u8 addr = addr_map[port];

		ret = tx_update(bar0, addr, 0x41, 0x01, 0x00);
		if (ret < 0)
			return ret;
		ret = tx_update(bar0, addr, 0xc1, 0x01, 0x01);
		if (ret < 0)
			return ret;
		ret = tx_update(bar0, addr, 0x88, 0x01, 0x01);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int it6664_refresh_rx_color_depth(void *bar0)
{
	u8 value = 0;
	int ret = rx_read(bar0, IT6664_RX_BANK_0, 0x98, &value);

	if (ret < 0)
		return ret;
	g_rx_color_depth = value >> 4;
	return 0;
}

static int it6664_measure_tx_pclk(void *bar0, u8 addr, u32 *pclk_khz)
{
	u32 initial_count, average_count, denominator, divider;
	u32 sample_sum = 0;
	u64 pclk;
	u8 count_lo = 0, count_hi = 0;
	int ret;

	ret = tx_update(bar0, addr, 0x07, 0x80, 0x80);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);
	ret = tx_update(bar0, addr, 0x07, 0x80, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_read(bar0, addr, 0x06, &count_lo);
	if (ret < 0)
		return ret;
	ret = tx_read(bar0, addr, 0x07, &count_hi);
	if (ret < 0)
		return ret;

	initial_count = ((count_hi & 0x0f) << 9) | (count_lo << 1);
	if (initial_count < 0x10)
		divider = 7;
	else if (initial_count < 0x20)
		divider = 6;
	else if (initial_count < 0x40)
		divider = 5;
	else if (initial_count < 0x80)
		divider = 4;
	else if (initial_count < 0x100)
		divider = 3;
	else if (initial_count < 0x200)
		divider = 2;
	else if (initial_count < 0x400)
		divider = 1;
	else
		divider = 0;

	for (unsigned int sample = 0; sample < IT6664_TX_PCLK_SAMPLES;
	     sample++) {
		u8 selector = divider << 4;

		ret = tx_update(bar0, addr, 0x07, 0xf0, selector | 0x80);
		if (ret < 0)
			return ret;
		ret = tx_update(bar0, addr, 0x07, 0xf0, selector);
		if (ret < 0)
			return ret;
		ret = tx_read(bar0, addr, 0x06, &count_lo);
		if (ret < 0)
			return ret;
		ret = tx_read(bar0, addr, 0x07, &count_hi);
		if (ret < 0)
			return ret;
		sample_sum += ((count_hi & 0x0f) << 9) | (count_lo << 1);
	}

	denominator = IT6664_TX_PCLK_SAMPLES << divider;
	average_count = sample_sum >= denominator ? sample_sum / denominator : 1;
	if (!g_rclk_khz)
		return -ERANGE;
	pclk = ((u64)g_rclk_khz << 12) / average_count;

	ret = it6664_refresh_rx_color_depth(bar0);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xaf, 0xc0, 0x00);
	if (ret < 0)
		return ret;
	if ((g_rx_color_depth & 0x03) == 1)
		pclk = pclk * 5 / 4;
	else if ((g_rx_color_depth & 0x03) == 2)
		pclk = pclk * 3 / 2;
	if (pclk >= IT6664_TX_PCLK_MAX_KHZ)
		pclk = 0;

	*pclk_khz = (u32)pclk;
	return 0;
}

static int it6664_configure_tx_afe(void *bar0, u8 addr, u32 pclk_khz)
{
	u8 low_clock_value = pclk_khz > 150000 ? 0x09 : 0x03;
	u8 reg84 = pclk_khz > 100000 ? 0x04 : 0x03;
	u8 reg88 = pclk_khz > 162000 ? 0x04 : 0x00;
	u8 reg87, reg89, reg8b;
	int ret;

	if (pclk_khz > 375000) {
		reg87 = 0x0e;
		reg89 = 0x25;
		reg8b = 0x0d;
	} else if (pclk_khz > 310000) {
		reg87 = 0x0d;
		reg89 = 0x25;
		reg8b = 0x0b;
	} else {
		reg87 = low_clock_value;
		reg89 = pclk_khz > 150000 ? 0x21 : 0x80;
		reg8b = low_clock_value;
	}

	/*
	 * TX2 splits the same RX stream but drives its own PLL, and the kernel
	 * clamps the port-2 AFE to the low-clock values regardless of the
	 * measured frequency (gc573-it6664-tx.c).
	 */
	if (addr == IT6664_TX2_ADDR) {
		reg87 = low_clock_value;
		reg8b = low_clock_value;
	}

	ret = tx_update(bar0, addr, 0x84, 0x07, reg84);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x88, 0x04, reg88);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x87, 0x1f, reg87);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x89, 0xbf, reg89);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x8a, 0x0f, 0x00);
	if (ret < 0)
		return ret;
	return tx_update(bar0, addr, 0x8b, 0x0f, reg8b);
}

/*
 * Power on one TX port (1 = IT6805 capture path, 2 = HDMI OUT passthrough).
 * The switch exposes one power bit per port; both transmitters share the same
 * register layout, so only the I2C address and the power bit differ.  With RX
 * sync already present the port AFE is calibrated from the measured pixel
 * clock, exactly like the kernel driver's it6664_power_on_tx_port().
 */
static int it6664_power_on_tx_port(void *bar0, unsigned int port)
{
	u8 addr = (port == 1) ? IT6664_TX1_ADDR : IT6664_TX2_ADDR;
	u8 power_bit = (port == 1) ? 0x02 : 0x04;
	u32 pclk_khz = 0;
	int ret;

	if (port != 1 && port != 2)
		return -EINVAL;

	ret = sw_update(bar0, 0x08, power_bit, power_bit);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc1, 0xf0, 0x80);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x84, 0xe0, 0x80);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x86, 0x08, 0x08);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x02, 0x01, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x19, 0x07, 0x07);
	if (ret < 0)
		return ret;

	/* The AFE needs a live stream to measure against. */
	if (!g_rx_locked)
		goto done;

	for (size_t i = 0; i < ARRAY_SIZE(it6664_tx_irq_enable); i++) {
		ret = tx_write(bar0, addr, IT6664_TX_REG_IRQ_BASE + i,
			       it6664_tx_irq_enable[i]);
		if (ret < 0)
			return ret;
	}

	ret = tx_write(bar0, addr, 0x01, 0x24);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x01, 0x00);
	if (ret < 0)
		return ret;

	/*
	 * TX2 splits the identical RX stream.  Its own counter only sees noise
	 * while the port is still unrouted (~25 MHz), which would configure the
	 * AFE far too low and keep the video PLL from locking.  Reuse TX1's
	 * measurement; only fall back to the local counter before TX1 has locked.
	 */
	if (port == 1 || !g_tx1_pclk_khz) {
		ret = it6664_measure_tx_pclk(bar0, addr, &pclk_khz);
		if (ret < 0)
			return ret;
	} else {
		pclk_khz = g_tx1_pclk_khz;
	}
	if (port == 1)
		g_tx1_pclk_khz = pclk_khz;
	else
		g_tx2_pclk_khz = pclk_khz;
	ret = it6664_configure_tx_afe(bar0, addr, pclk_khz);
	if (ret < 0)
		return ret;
	msleep(100);
	ret = it6664_reset_tx_video_clock(bar0, addr);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x18, 0x80, 0x80);
	if (ret < 0)
		return ret;
	pr_info("IT6664 TX%u AFE configured for %u kHz (RX depth 0x%02x)",
		port, pclk_khz, g_rx_color_depth);

 done:
	if (port == 1)
		g_tx1_powered = true;
	else
		g_tx2_powered = true;
	return 0;
}

static int it6664_write_upstream_edid(void *bar0, const u8 *edid)
{
	int ret;

	ret = rx_update(bar0, IT6664_RX_BANK_0,
			IT6664_RX_REG_EDID_ENABLE, 0x01, 0x00);
	if (ret < 0)
		return ret;
	ret = rx_write(bar0, IT6664_RX_BANK_0,
		       IT6664_RX_REG_EDID_ADDR_HI, 0x00);
	if (ret < 0)
		goto close_gate;
	ret = rx_write(bar0, IT6664_RX_BANK_0,
		       IT6664_RX_REG_EDID_ADDR_LO, 0x10);
	if (ret < 0)
		goto close_gate;
	ret = rx_write(bar0, IT6664_RX_BANK_0,
		       IT6664_RX_REG_EDID_PORT, 0x00);
	if (ret < 0)
		goto close_gate;
	ret = rx_write(bar0, IT6664_RX_BANK_0, 0x4b, 0xd9);
	if (ret < 0)
		goto close_gate;

	for (unsigned int i = 0; i < IT6664_EDID_BLOCK_SIZE - 1; i++) {
		ret = it6664_raw_write(bar0, IT6664_EDID_ADDR, i, edid[i]);
		if (ret < 0)
			goto close_gate;
	}
	ret = rx_write(bar0, IT6664_RX_BANK_0,
		       IT6664_RX_REG_EDID_CHECKSUM0,
		       edid[IT6664_EDID_BLOCK_SIZE - 1]);
	if (ret < 0)
		goto close_gate;

	for (unsigned int i = 0; i < IT6664_EDID_BLOCK_SIZE - 1; i++) {
		ret = it6664_raw_write(bar0, IT6664_EDID_ADDR,
				       IT6664_EDID_BLOCK_SIZE + i,
				       edid[IT6664_EDID_BLOCK_SIZE + i]);
		if (ret < 0)
			goto close_gate;
	}
	ret = rx_write(bar0, IT6664_RX_BANK_0,
		       IT6664_RX_REG_EDID_CHECKSUM1,
		       edid[IT6664_EDID_SIZE - 1]);

close_gate:
	{
		int gate_ret = rx_update(bar0, IT6664_RX_BANK_0,
					 IT6664_RX_REG_EDID_ENABLE,
					 0x01, 0x01);

		if (ret >= 0)
			ret = gate_ret;
	}
	return ret;
}

static int it6664_apply_upstream_edid(void *bar0, const u8 *edid)
{
	int ret;

	ret = it6664_write_upstream_edid(bar0, edid);
	if (ret < 0)
		return ret;

	ret = sw_select_bank(bar0, 1);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x10, 0x40, 0x40);
	if (ret < 0)
		return ret;
	ret = sw_select_bank(bar0, 0);
	if (ret < 0)
		return ret;

	ret = rx_update(bar0, IT6664_RX_BANK_0, 0xc5, 0x01, 0x01);
	if (ret < 0)
		return ret;
	ret = it6664_rx_set_hpd(bar0, false);
	if (ret < 0)
		return ret;
	ret = it6664_quiesce_rx(bar0);
	if (ret < 0)
		return ret;
	msleep(500);
	ret = it6664_rx_set_hpd(bar0, true);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_0, 0xc5, 0x01, 0x00);
	if (ret < 0)
		return ret;
	return rx_update(bar0, IT6664_RX_BANK_0,
			 IT6664_RX_REG_EDID_ENABLE, 0x01, 0x01);
}

static int it6664_verify_upstream_edid(void *bar0, const u8 *edid)
{
	unsigned int mismatches = 0;

	for (unsigned int i = 0; i < IT6664_EDID_SIZE; i++) {
		u8 value = 0;
		int ret;

		if ((i & (IT6664_EDID_BLOCK_SIZE - 1)) ==
		    IT6664_EDID_BLOCK_SIZE - 1)
			continue;
		ret = it6664_raw_read(bar0, IT6664_EDID_ADDR, i, &value);
		if (ret < 0)
			return ret;
		if (value != edid[i])
			mismatches++;
	}

	if (mismatches) {
		pr_err("IT6664 upstream EDID mismatch: %u bytes", mismatches);
		return -EIO;
	}
	pr_info("IT6664 upstream EDID verified: %u bytes match.",
		IT6664_EDID_SIZE);
	return 0;
}

int gc573_user_it6664_init(void *bar0)
{
	u8 identity[4] = { 0 };
	u8 rx_reg08 = 0;
	u8 tx_reg50 = 0;
	int ret;

	if (!bar0)
		return -EINVAL;

	g_tx1_powered = false;
	g_tx2_powered = false;
	g_tx2_output_enabled = false;
	g_tx2_error_reported = false;
	g_tx2_hpd_last = false;
	g_rx_locked = false;
	g_rx_color_depth = 0;

	pr_info("Starting IT6664 userspace initialization...");

	for (unsigned int i = 0; i < ARRAY_SIZE(identity); i++) {
		ret = sw_read(bar0, i, &identity[i]);
		if (ret < 0) {
			pr_err("IT6664 identity read failed at reg %u: %d",
			       i, ret);
			return ret;
		}
	}
	pr_info("IT6664 identity %02x %02x %02x %02x",
		identity[0], identity[1], identity[2], identity[3]);
	if (!it6664_identity_valid(identity)) {
		pr_err("Unexpected IT6664 identity!");
		return -ENODEV;
	}

	msleep(IT6664_HEAVY_INIT_DELAY_MS);

	ret = it6664_apply_pre_rclk(bar0);
	if (ret < 0) {
		pr_err("IT6664 pre-RCLK unlock failed: %d", ret);
		return ret;
	}

	/* Sub-maps are routed now; verify RX and TX-common access. */
	ret = rx_read(bar0, IT6664_RX_BANK_0, IT6664_RX_REG_CAOF_STATUS,
		      &rx_reg08);
	if (ret < 0) {
		pr_err("IT6664 RX map not reachable: %d", ret);
		return ret;
	}
	ret = tx_read(bar0, IT6664_TX_COMMON_ADDR, IT6664_TX_COMMON_REG_50,
		      &tx_reg50);
	if (ret < 0) {
		pr_err("IT6664 TX-common map not reachable: %d", ret);
		return ret;
	}
	pr_info("IT6664 maps online (RX 0x08=0x%02x, TXCOM 0x50=0x%02x)",
		rx_reg08, tx_reg50);

	ret = it6664_calibrate_rclk(bar0);
	if (ret < 0) {
		pr_err("IT6664 RCLK calibration failed: %d", ret);
		return ret;
	}

	ret = it6664_initialize_rx_caof(bar0);
	if (ret < 0) {
		pr_err("IT6664 RX CAOF calibration failed: %d", ret);
		return ret;
	}

	ret = it6664_initialize_rx_registers(bar0);
	if (ret < 0) {
		pr_err("IT6664 RX register init failed: %d", ret);
		return ret;
	}

	ret = it6664_initialize_rx_power_state(bar0);
	if (ret < 0) {
		pr_err("IT6664 RX power state init failed: %d", ret);
		return ret;
	}

	ret = it6664_initialize_tx_reset_path(bar0);
	if (ret < 0) {
		pr_err("IT6664 TX reset path failed: %d", ret);
		return ret;
	}

	ret = it6664_configure_tx_switch(bar0);
	if (ret < 0) {
		pr_err("IT6664 TX switch setup failed: %d", ret);
		return ret;
	}

	pr_info("[✓] IT6664 initialization complete (RX HPD low).");
	return 0;
}

int gc573_user_it6664_enable_source(void *bar0)
{
	struct gc573_user_it6664_status status = { 0 };
	const u8 *edid;
	u8 value = 0;
	int ret;

	if (!bar0)
		return -EINVAL;

	edid = gc573_user_edid_data();

	/*
	 * The IT6805 HPD is ready.  Release both TX HPD gates before sampling any
	 * status: a gated port reports HPD = 0 and would be skipped, and both
	 * downstream sinks need a moment to assert HPD after the gate goes high.
	 */
	ret = gc573_user_set_tx_hpd_gate(bar0, 1, true);
	if (ret < 0)
		return ret;
	ret = gc573_user_set_tx_hpd_gate(bar0, 2, true);
	if (ret < 0)
		return ret;
	msleep(50);

	ret = tx_read(bar0, IT6664_TX1_ADDR, IT6664_TX_REG_STATUS, &value);
	if (ret < 0)
		return ret;
	status.tx1_hpd = (value & 0x01) != 0;
	pr_info("IT6664 TX1 status 0x%02x (HPD=%u)", value, status.tx1_hpd);

	if (status.tx1_hpd && !g_tx1_powered) {
		ret = it6664_power_on_tx_port(bar0, 1);
		if (ret < 0)
			return ret;
	}

	ret = tx_read(bar0, IT6664_TX2_ADDR, IT6664_TX_REG_STATUS, &value);
	if (ret < 0)
		return ret;
	status.tx2_hpd = (value & 0x01) != 0;
	/* HDMI OUT is optional: never abort the source enable for it. */
	if (status.tx2_hpd && !g_tx2_powered)
		(void)it6664_arm_tx2(bar0);

	pr_info("Programming IT6664 upstream EDID (%u bytes)...",
		IT6664_EDID_SIZE);
	ret = it6664_apply_upstream_edid(bar0, edid);
	if (ret < 0) {
		pr_err("IT6664 upstream EDID programming failed: %d", ret);
		return ret;
	}
	ret = it6664_verify_upstream_edid(bar0, edid);
	if (ret < 0)
		return ret;

	ret = it6664_set_rx_hdcp_off(bar0);
	if (ret < 0)
		return ret;
	ret = it6664_rx_set_hpd(bar0, false);
	if (ret < 0)
		return ret;
	msleep(100);
	ret = it6664_rx_set_hpd(bar0, true);
	if (ret < 0)
		return ret;

	ret = rx_read(bar0, IT6664_RX_BANK_0, IT6664_RX_REG_STATUS, &value);
	if (ret < 0)
		return ret;
	g_rx_last_5v = (value & 0x01) != 0;
	pr_info("[✓] IT6664 source enabled, RX HDCP off (RX status 0x%02x, 5V=%u).",
		value, !!(value & 0x01));
	return 0;
}

static int it6664_wait_tx_scdc(void *bar0, u8 addr)
{
	u8 status = 0;
	int ret;

	usleep_range(15000, 16000);
	ret = tx_read(bar0, addr, IT6664_TX_REG_DDC_STATUS, &status);
	if (ret < 0)
		return ret;
	if (status & 0x80)
		return 0;

	usleep_range(15000, 16000);
	ret = tx_read(bar0, addr, IT6664_TX_REG_DDC_STATUS, &status);
	if (ret < 0)
		return ret;
	ret = tx_read(bar0, addr, IT6664_TX_REG_DDC_STATUS, &status);
	if (ret < 0)
		return ret;
	ret = tx_read(bar0, addr, 0x12, &status);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x35, 0x10, 0x10);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x35, 0x10, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, IT6664_TX_REG_DDC_ENABLE, 0x01, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, IT6664_TX_REG_DDC_ENABLE, 0x01, 0x01);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_COMMAND, 0x0f);
	if (ret < 0)
		return ret;
	for (unsigned int attempt = 0; attempt < 0xc8; attempt++) {
		ret = tx_read(bar0, addr, IT6664_TX_REG_DDC_STATUS, &status);
		if (ret < 0)
			return ret;
		if (status & 0xb8)
			break;
		usleep_range(1000, 2000);
	}

	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_COMMAND, 0x0f);
	if (ret < 0)
		return ret;
	for (unsigned int attempt = 0; attempt < 0xc8; attempt++) {
		ret = tx_read(bar0, addr, IT6664_TX_REG_DDC_STATUS, &status);
		if (ret < 0)
			return ret;
		if (status & 0xb8)
			break;
		usleep_range(1000, 2000);
	}

	return -EAGAIN;
}

static int it6664_write_tx_scdc(void *bar0, u8 addr, u8 offset, u8 value)
{
	u8 status = 0;
	int cleanup_ret;
	int ret = 0;

	ret = tx_read(bar0, addr, IT6664_TX_REG_STATUS, &status);
	if (ret < 0)
		goto cleanup;
	if (!(status & 0x01)) {
		ret = -ENOLINK;
		goto cleanup;
	}
	ret = tx_update(bar0, addr, IT6664_TX_REG_DDC_ENABLE, 0x01, 0x00);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_COMMAND, 0x09);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_SLAVE,
		       IT6664_TX_SCDC_SLAVE);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_OFFSET, offset);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_COUNT, 0x01);
	if (ret < 0)
		goto cleanup;
	ret = tx_update(bar0, addr, IT6664_TX_REG_DDC_HEADER, 0x03, 0x00);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_FIFO, value);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_COMMAND, 0x01);
	if (ret < 0)
		goto cleanup;
	ret = it6664_wait_tx_scdc(bar0, addr);

cleanup:
	cleanup_ret = tx_update(bar0, addr, IT6664_TX_REG_DDC_ENABLE, 0x01,
				0x00);
	if (!ret)
		ret = cleanup_ret;
	return ret;
}

static int it6664_read_tx_scdc(void *bar0, u8 addr, u8 offset, u8 *value)
{
	u8 status = 0;
	int cleanup_ret;
	int ret = 0;

	if (!value)
		return -EINVAL;
	ret = tx_read(bar0, addr, IT6664_TX_REG_STATUS, &status);
	if (ret < 0)
		goto cleanup;
	if (!(status & 0x01)) {
		ret = -ENOLINK;
		goto cleanup;
	}
	ret = tx_update(bar0, addr, IT6664_TX_REG_DDC_ENABLE, 0x01, 0x00);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_COMMAND, 0x09);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_SLAVE,
		       IT6664_TX_SCDC_SLAVE);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_OFFSET, offset);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_COUNT, 0x01);
	if (ret < 0)
		goto cleanup;
	ret = tx_update(bar0, addr, IT6664_TX_REG_DDC_HEADER, 0x03, 0x00);
	if (ret < 0)
		goto cleanup;
	ret = tx_write(bar0, addr, IT6664_TX_REG_DDC_COMMAND, 0x00);
	if (ret < 0)
		goto cleanup;
	ret = tx_update(bar0, addr, IT6664_TX_REG_DDC_ENABLE, 0x01, 0x01);
	if (ret < 0)
		goto cleanup;
	ret = it6664_wait_tx_scdc(bar0, addr);
	if (ret < 0)
		goto cleanup;
	ret = tx_read(bar0, addr, IT6664_TX_REG_DDC_FIFO, value);

cleanup:
	cleanup_ret = tx_update(bar0, addr, IT6664_TX_REG_DDC_ENABLE, 0x01,
				0x00);
	if (!ret)
		ret = cleanup_ret;
	return ret;
}

static int it6664_setup_tx_scdc(void *bar0, u8 addr)
{
	u8 status = 0;
	u8 value = 0;
	bool read_ok = false;
	int first_ret = 0;
	int ret;

	ret = tx_update(bar0, addr, 0x83, 0x08, 0x08);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc0, 0x46, 0x46);
	if (ret < 0)
		return ret;
	msleep(50);
	ret = tx_update(bar0, addr, 0x3a, 0x03, 0x00);
	if (ret < 0)
		return ret;

	ret = it6664_write_tx_scdc(bar0, addr, IT6664_TX_SCDC_VERSION, 0x01);
	if (ret < 0 && !first_ret)
		first_ret = ret;
	ret = it6664_read_tx_scdc(bar0, addr, IT6664_TX_SCDC_VERSION, &value);
	if (ret < 0 && !first_ret)
		first_ret = ret;

	for (unsigned int attempt = 0; attempt < IT6664_TX_SCDC_RETRIES;
	     attempt++) {
		ret = it6664_write_tx_scdc(bar0, addr,
					   IT6664_TX_SCDC_TMDS_CONFIG, 0x03);
		if (ret < 0 && !first_ret)
			first_ret = ret;
		ret = it6664_read_tx_scdc(bar0, addr,
					  IT6664_TX_SCDC_TMDS_CONFIG, &value);
		read_ok = ret >= 0;
		if (ret < 0 && !first_ret)
			first_ret = ret;

		ret = tx_read(bar0, addr, IT6664_TX_REG_STATUS, &status);
		if (ret < 0)
			return ret;
		if (!(status & 0x01))
			return -ENOLINK;
		if (read_ok && (value & 0x03) == 0x03)
			return 0;
	}

	return read_ok ? 0 : first_ret ? first_ret : -EAGAIN;
}

static int it6664_clear_tx_scdc(void *bar0, u8 addr)
{
	u8 config = 0;
	int first_ret = 0;
	int ret;

	for (unsigned int attempt = 0; attempt < IT6664_TX_SCDC_RETRIES;
	     attempt++) {
		ret = it6664_write_tx_scdc(bar0, addr,
					   IT6664_TX_SCDC_TMDS_CONFIG, 0x00);
		if (ret < 0 && !first_ret)
			first_ret = ret;
		ret = it6664_read_tx_scdc(bar0, addr,
					  IT6664_TX_SCDC_TMDS_CONFIG, &config);
		if (ret < 0 && !first_ret)
			first_ret = ret;
		if (ret >= 0 && !(config & 0x03))
			break;
	}

	ret = tx_update(bar0, addr, 0xc0, 0x02, 0x00);
	if (ret < 0)
		return ret;
	if (config & 0x03)
		return first_ret ? first_ret : -EAGAIN;
	return 0;
}

/* Same tables as it6664_csc_rgb_to_ycbcr[] in gc573-it6664-core.c. */
static const u8 it6664_csc_rgb_to_ycbcr[2][18] = {
	{ 0xb2, 0x04, 0x65, 0x02, 0xe9, 0x00, 0x93, 0x3c, 0x18,
	  0x04, 0x55, 0x3f, 0x49, 0x3d, 0x9f, 0x3e, 0x18, 0x04 },
	{ 0xb8, 0x05, 0xb4, 0x01, 0x94, 0x00, 0x4a, 0x3c, 0x17,
	  0x04, 0x9f, 0x3f, 0xd9, 0x3c, 0x10, 0x3f, 0x17, 0x04 },
};

static int it6664_write_bytes(void *bar0, u8 reg, const u8 *data, size_t len)
{
	int ret = 0;

	for (size_t i = 0; ret >= 0 && i < len; i++)
		ret = sw_write(bar0, reg + i, data[i]);
	return ret;
}

static int it6664_load_rgb_csc(void *bar0, unsigned int table, bool high)
{
	static const u8 zero[3] = { 0x00, 0x00, 0x00 };
	u8 offset_reg = high ? 0x88 : 0x70;
	u8 matrix_reg = high ? 0x93 : 0x73;
	int ret;

	ret = it6664_write_bytes(bar0, offset_reg, zero, sizeof(zero));
	if (ret < 0)
		return ret;
	return it6664_write_bytes(bar0, matrix_reg,
				  it6664_csc_rgb_to_ycbcr[table], 18);
}

/*
 * TX1 (the link into the IT6805) cannot carry deep colour directly.
 * gc573-it6664-tx.c sends that stream through the switch CSC, which
 * repacks it and clears scrambling. The RGB matrix is loaded the same
 * way as it6664_configure_rx_csc().
 */
static int it6664_select_tx1_source(void *bar0, u8 addr, bool *csc_route)
{
	u8 deep_color = g_rx_color_depth & 0x03;
	u8 avi0 = 0;
	u8 avi1 = 0;
	u8 reg14 = 0;
	u8 colorspace;
	unsigned int table;
	int ret;

	*csc_route = false;
	if (deep_color) {
		ret = rx_read(bar0, IT6664_RX_BANK_2, 0x15, &avi0);
		if (ret < 0)
			return ret;
		ret = rx_read(bar0, IT6664_RX_BANK_2, 0x16, &avi1);
		if (ret < 0)
			return ret;
		ret = rx_read(bar0, IT6664_RX_BANK_0, IT6664_RX_REG_STATUS_14,
			      &reg14);
		if (ret < 0)
			return ret;
		colorspace = (avi0 >> 5) & 0x03;
		table = ((avi1 & 0xc0) == 0x80) ? 1 : 0;
		/* RGB only. YCbCr deep colour needs the other matrix. */
		if (colorspace == 0) {
			if (reg14 & 0x80) {
				ret = it6664_load_rgb_csc(bar0, table, true);
				if (ret < 0)
					return ret;
				ret = it6664_load_rgb_csc(bar0, table, false);
				if (ret < 0)
					return ret;
				ret = sw_write(bar0, 0x6b, 0x7a);
				if (ret < 0)
					return ret;
				ret = sw_write(bar0, 0x6c, 0x10);
				if (ret < 0)
					return ret;
			} else {
				ret = it6664_load_rgb_csc(bar0, table, false);
				if (ret < 0)
					return ret;
				ret = sw_write(bar0, 0x6b, 0x4a);
				if (ret < 0)
					return ret;
				ret = sw_write(bar0, 0x6c, 0x00);
				if (ret < 0)
					return ret;
			}
			ret = sw_update(bar0, 0x6b, 0x42, 0x00);
			if (ret < 0)
				return ret;
			ret = sw_update(bar0, 0x67, 0x02, 0x02);
			if (ret < 0)
				return ret;
			ret = tx_update(bar0, addr, 0xaf, 0xc0,
					deep_color == 1 ? 0x40 : 0x80);
			if (ret < 0)
				return ret;
			ret = tx_update(bar0, addr, 0xc1, 0xf0, 0x00);
			if (ret < 0)
				return ret;
			ret = tx_update(bar0, addr, 0xc1, 0x04, 0x04);
			if (ret < 0)
				return ret;
			/* Port 1 source field: CSC = 1. */
			ret = sw_update(bar0, 0x0d, 0x0c, 0x04);
			if (ret < 0)
				return ret;
			ret = tx_update(bar0, addr, 0xad, 0x85, 0x85);
			if (ret < 0)
				return ret;
			ret = tx_update(bar0, addr, 0xae, 0x07, 0x00);
			if (ret < 0)
				return ret;
			ret = tx_update(bar0, addr, 0xae, 0xc0, 0x00);
			if (ret < 0)
				return ret;
			ret = tx_update(bar0, addr, 0xa0, 0x30, 0x00);
			if (ret < 0)
				return ret;
			*csc_route = true;
			pr_info("IT6664 TX1 deep colour via CSC (depth 0x%02x status14=0x%02x table %u)",
				g_rx_color_depth, reg14, table);
			return 0;
		}
	}

	if (!deep_color) {
		ret = tx_update(bar0, addr, 0xc1, 0xf0, 0x00);
		if (ret < 0)
			return ret;
	}
	ret = tx_update(bar0, addr, 0xc1, 0x04, deep_color ? 0x04 : 0x00);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x0d, 0x0c, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xad, 0x81, 0x00);
	if (ret < 0)
		return ret;
	return tx_update(bar0, addr, 0xa0, 0x30, 0x00);
}

/*
 * TX2 drives the physical HDMI OUT connector.  It always takes the RX stream
 * directly: the CSC repack belongs to the capture path and the monitor does
 * its own colour handling.  Switch register 0x0d carries one two-bit source
 * field per TX port; port 2 uses bits 5:4 (0 = direct RX).
 */
static int it6664_select_tx2_source(void *bar0)
{
	const u8 addr = IT6664_TX2_ADDR;
	u8 deep_color = g_rx_color_depth & 0x03;
	int ret;

	if (!deep_color) {
		ret = tx_update(bar0, addr, 0xc1, 0xf0, 0x00);
		if (ret < 0)
			return ret;
	}
	ret = tx_update(bar0, addr, 0xc1, 0x04, deep_color ? 0x04 : 0x00);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x0d, 0x30, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xad, 0x81, 0x00);
	if (ret < 0)
		return ret;
	return tx_update(bar0, addr, 0xa0, 0x30, 0x00);
}

static int it6664_select_tx_source(void *bar0, unsigned int port,
				   bool *csc_route)
{
	if (port == 1)
		return it6664_select_tx1_source(bar0, IT6664_TX1_ADDR,
						csc_route);

	*csc_route = false;
	return it6664_select_tx2_source(bar0);
}

/*
 * Kernel it6664_reset_tx_hdcp() when the source reports no HDCP.
 * A TX port with 0xc1 bit 0 left set makes the splitter emit Set AVMute
 * toward its sink, which then fills every frame with limited black.
 */
static int it6664_clear_tx_hdcp(void *bar0, u8 addr)
{
	int ret;

	ret = tx_update(bar0, addr, 0x91, 0x10, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc1, 0x01, 0x00);
	if (ret < 0)
		return ret;
	return tx_update(bar0, addr, 0xc2, 0x80, 0x00);
}

/*
 * Program the TX protocol mode (reg 0xc0 bit 0: 1 = HDMI, 0 = DVI).  DVI mode
 * suppresses AVI InfoFrames, audio and HDMI vendor data, which modern HDMI
 * sinks reject with "no signal".  Writes only on a mismatch so it can run from
 * the poll loop without disturbing a live link.  *changed is set on a rewrite.
 */
static int it6664_set_tx_hdmi_mode(void *bar0, u8 addr, bool hdmi,
				   bool *changed)
{
	u8 want = hdmi ? 0x01 : 0x00;
	u8 cur = 0;
	int ret;

	if (changed)
		*changed = false;
	ret = tx_read(bar0, addr, 0xc0, &cur);
	if (ret < 0)
		return ret;
	if ((cur & 0x01) == want)
		return 0;
	ret = tx_update(bar0, addr, 0xc0, 0x01, want);
	if (ret < 0)
		return ret;
	if (changed)
		*changed = true;
	pr_info("IT6664 TX 0x%02x protocol mode -> %s (0xc0 0x%02x)",
		addr, want ? "HDMI" : "DVI", (cur & ~0x01) | want);
	return 0;
}

static int it6664_enable_tx_output(void *bar0, unsigned int port)
{
	const u8 addr = (port == 1) ? IT6664_TX1_ADDR : IT6664_TX2_ADDR;
	u32 pclk_khz = (port == 1) ? g_tx1_pclk_khz : g_tx2_pclk_khz;
	u8 rx_status = 0;
	u8 status = 0;
	bool csc_route = false;
	/*
	 * The CSC repack is 8-bit, so it does not scramble. A direct deep-colour
	 * link still follows the 340 MHz TMDS rule.
	 */
	bool scrambling_required;
	int first_scdc_ret = 0;
	int second_scdc_ret = 0;
	int ret;

	if (port != 1 && port != 2)
		return -EINVAL;

	/* Follow the RX HDMI/DVI mode in the TX protocol control. */
	ret = rx_read(bar0, IT6664_RX_BANK_0, IT6664_RX_REG_STATUS, &rx_status);
	if (ret < 0)
		return ret;
	/*
	 * TX1 mirrors the RX protocol (HDMI vs DVI).  TX2 drives a physical HDMI
	 * sink and is always driven in HDMI mode: it is armed before the RX mode
	 * has settled, and a port left at 0xc0 bit 0 = 0 (DVI) withholds AVI
	 * InfoFrames and audio, so HDMI monitors keep reporting "no signal".
	 */
	ret = it6664_set_tx_hdmi_mode(bar0, addr,
				      port == 2 || (rx_status & 0x02) != 0,
				      NULL);
	if (ret < 0)
		return ret;

	ret = it6664_select_tx_source(bar0, port, &csc_route);
	if (ret < 0)
		return ret;
	scrambling_required = csc_route ? false : pclk_khz >= 340000;

	ret = tx_update(bar0, addr, 0x18, 0x0c, 0x0c);
	if (ret < 0)
		return ret;
	ret = tx_write(bar0, addr, 0x85, 0x19);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x1a, 0x0b, 0x0b);
	if (ret < 0)
		return ret;

	if (scrambling_required) {
		first_scdc_ret = it6664_setup_tx_scdc(bar0, addr);
		msleep(100);
	} else {
		ret = it6664_clear_tx_scdc(bar0, addr);
		if (ret < 0)
			return ret;
	}
	ret = tx_update(bar0, addr, 0xc1, 0x08, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc1, 0x08, 0x08);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc2, 0x80, 0x80);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0xc3, 0x30, 0x30);
	if (ret < 0)
		return ret;
	/*
	 * Latch switch stream routing FIFO for the port (Windows CSpliterITE6664::SetTXSource).
	 * Bit 2 for port 1, Bit 3 for port 2.
	 */
	u8 fifo_bit = (port == 1) ? 0x04 : 0x08;
	ret = sw_update(bar0, 0x4e, 0x0f, fifo_bit);
	if (ret < 0)
		return ret;
	usleep_range(10000, 11000);
	ret = sw_update(bar0, 0x4e, 0x0f, 0x00);
	if (ret < 0)
		return ret;

	/* Open TX OE: clear bit 7 of 0x19 (AVMute/Disable) and bits 1:0 of 0x88 */
	ret = tx_update(bar0, addr, 0x19, 0x80, 0x00);
	if (ret < 0)
		return ret;
	ret = tx_update(bar0, addr, 0x88, 0x03, 0x00);
	if (ret < 0)
		return ret;
	msleep(100);
	if (scrambling_required)
		second_scdc_ret = it6664_setup_tx_scdc(bar0, addr);
	else
		second_scdc_ret = it6664_clear_tx_scdc(bar0, addr);
	if (!scrambling_required && second_scdc_ret < 0)
		return second_scdc_ret;

	ret = tx_read(bar0, addr, IT6664_TX_REG_STATUS, &status);
	if (ret < 0)
		return ret;
	if (!(status & 0x08)) {
		/* PLL not locked yet: re-latch the configured AFE clock once. */
		ret = it6664_reset_tx_video_clock(bar0, addr);
		if (ret < 0)
			return ret;
		ret = tx_update(bar0, addr, 0xc1, 0xf0, 0x80);
		if (ret < 0)
			return ret;
		msleep(50);
		ret = tx_read(bar0, addr, IT6664_TX_REG_STATUS, &status);
		if (ret < 0)
			return ret;
	}

	ret = tx_write(bar0, addr, 0x12, 0xff);
	if (ret < 0)
		return ret;
	ret = it6664_clear_tx_hdcp(bar0, addr);
	if (ret < 0)
		return ret;

	/* Bit 3 reports the video PLL lock (e.g. 0x9d = locked, 0x15 = unlocked). */
	pr_info("IT6664 TX%u output enabled (status=0x%02x pclk=%u kHz PLL=%s SCDC=%d/%d%s)",
		port, status, pclk_khz, (status & 0x08) ? "locked" : "unlocked",
		first_scdc_ret, second_scdc_ret,
		scrambling_required ? " scrambled" : "");
	if (!(status & 0x08))
		pr_warn("IT6664 TX%u video PLL did not lock (status=0x%02x, pclk=%u kHz)",
			port, status, pclk_khz);
	return 0;
}

/*
 * (Re-)assert the TX2 HPD gate (GPIO6).  The legacy kernel driver does this
 * after every TX2 power-on.  If the pin is found low it is logged, because a
 * gate that stays low keeps the HDMI OUT sink from ever seeing the link.
 */
static int it6664_assert_tx2_gate(void *bar0)
{
	bool high = false;
	int ret;

	ret = gc573_user_get_tx_hpd_gate(bar0, 2, &high);
	if (ret == 0 && !high)
		pr_warn("IT6664 TX2 HPD gate (GPIO%u) was low; re-asserting",
			GC573_GPIO_TX2_HPD_GATE);
	return gc573_user_set_tx_hpd_gate(bar0, 2, true);
}

/*
 * Log the HDMI OUT state on every transition.  status / 0xc0 protocol mode /
 * 0x85 video-path value distinguish "PLL locked but output never enabled"
 * from "no sink" during field debugging (0x85 == 0x19 means the unblank
 * sequence ran; 0xc2 bit 7 is HDCP, not the output enable).
 */
static void it6664_log_tx2_state(void *bar0, const char *what)
{
	u8 status = 0;
	u8 mode = 0;
	u8 path = 0;

	if (tx_read(bar0, IT6664_TX2_ADDR, IT6664_TX_REG_STATUS, &status) < 0)
		return;
	if (tx_read(bar0, IT6664_TX2_ADDR, 0xc0, &mode) < 0)
		return;
	if (tx_read(bar0, IT6664_TX2_ADDR, 0x85, &path) < 0)
		return;
	pr_info("IT6664 TX2 %s: status=0x%02x %s mode=0x%02x path=0x%02x powered=%u enabled=%u pclk=%u kHz",
		what, status, (status & 0x01) ? "HPD" : "no-HPD", mode, path,
		g_tx2_powered, g_tx2_output_enabled, g_tx2_pclk_khz);
}

/*
 * Enable (unblank) the HDMI OUT transmitter.
 *
 * The splitter must transmit continuously like a hardware distribution
 * amplifier: several monitors only assert HPD once they see a TMDS clock, so
 * gating the unblank sequence on HPD deadlocks (the chip never starts, the
 * monitor never plugs in).  The video PLL and AFE are left untouched here -
 * arm_tx2() brings the port up and it stays warm.
 *
 * Returns -EAGAIN when the video PLL is not locked, so the caller can fall
 * back to a full power cycle.
 */
static int it6664_enable_tx2_output(void *bar0)
{
	u8 status = 0;
	int ret;

	(void)it6664_assert_tx2_gate(bar0);
	ret = it6664_enable_tx_output(bar0, 2);
	if (ret < 0) {
		if (!g_tx2_error_reported) {
			pr_warn("IT6664 TX2 output enable failed: %d", ret);
			g_tx2_error_reported = true;
		}
		return ret;
	}

	ret = tx_read(bar0, IT6664_TX2_ADDR, IT6664_TX_REG_STATUS, &status);
	if (ret < 0)
		return ret;

	if (!(status & 0x08)) {
		/* Video PLL did not lock: a full power-on is the only way out. */
		if (!g_tx2_error_reported) {
			pr_warn("IT6664 TX2 PLL not locked after enable (status=0x%02x)",
				status);
			g_tx2_error_reported = true;
		}
		return -EAGAIN;
	}

	g_tx2_output_enabled = true;
	g_tx2_error_reported = false;
	it6664_log_tx2_state(bar0, "output enabled");
	return 0;
}

/*
 * Arm the optional HDMI OUT port: power it, configure the AFE and unblank the
 * output stage so the transmitter runs for as long as the RX is locked.
 *
 * A monitor that is absent, unplugged or misbehaving must never fail the
 * capture path, so callers ignore the result and failures are logged once
 * until the next HPD edge or RX re-lock.
 */
static int it6664_arm_tx2(void *bar0)
{
	u8 tx2_status = 0;
	int ret;

	ret = tx_read(bar0, IT6664_TX2_ADDR, IT6664_TX_REG_STATUS, &tx2_status);
	if (ret < 0)
		return ret;

	g_tx2_hpd_last = (tx2_status & 0x01) != 0;

	/*
	 * Proceed when the port is cable-connected (HPD or cable detect) or
	 * whenever the upstream RX is locked.  Waiting for persistent HPD would
	 * recreate the deadlock where the monitor never sees HDMI come alive.
	 */
	if (!(tx2_status & 0x11) && !g_rx_locked)
		return 0;

	/* Never disturb a running passthrough link. */
	if (g_tx2_powered && g_tx2_output_enabled)
		return 0;

	/*
	 * A warm port already runs its video PLL from TX1's clock.  Re-powering
	 * it would reset the TX video clock and blank TMDS, so only re-apply the
	 * output stage.
	 */
	if (g_tx2_powered) {
		if (!g_rx_locked)
			return 0;
		return it6664_enable_tx2_output(bar0);
	}

	/* TX2 mirrors TX1's pixel clock whenever it is already known. */
	if (g_tx1_pclk_khz)
		g_tx2_pclk_khz = g_tx1_pclk_khz;

	ret = it6664_power_on_tx_port(bar0, 2);
	if (ret < 0) {
		if (!g_tx2_error_reported) {
			pr_warn("IT6664 TX2 power-on failed: %d; HDMI OUT stays off",
				ret);
			g_tx2_error_reported = true;
		}
		return ret;
	}
	(void)it6664_assert_tx2_gate(bar0);
	it6664_log_tx2_state(bar0, "port powered");

	/* Without RX sync the AFE has no clock to lock onto. */
	if (!g_rx_locked)
		return 0;

	return it6664_enable_tx2_output(bar0);
}

static int it6664_handle_rx_lock(void *bar0)
{
	u8 status14 = 0;
	u8 tx_status = 0;
	int ret;

	ret = rx_update(bar0, IT6664_RX_BANK_0, 0x40, 0x03, 0x00);
	if (ret < 0)
		return ret;
	/* Tell the splitter that the capture receiver has sync. */
	ret = gc573_user_set_gpio(bar0, 3, true, 0);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x0b, 0xff);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x0b, 0x00);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x4e, 0x0f, 0x0f);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x0c, 0x08, 0x08);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x0c, 0x08, 0x00);
	if (ret < 0)
		return ret;
	ret = sw_update(bar0, 0x4e, 0x0f, 0x00);
	if (ret < 0)
		return ret;

	/*
	 * The kernel powers the connected TX ports here, while RX SCDT is
	 * asserted, so the TX AFE can measure the real pixel clock before
	 * the downstream routing registers are latched.
	 */
	ret = tx_read(bar0, IT6664_TX1_ADDR, IT6664_TX_REG_STATUS, &tx_status);
	if (ret < 0)
		return ret;
	/*
	 * Clear the switch CSC gate before TX1 enable. The deep-colour route
	 * sets bit 1 again; doing this afterwards would drop that route.
	 */
	ret = sw_update(bar0, 0x67, 0x0f, 0x00);
	if (ret < 0)
		return ret;
	ret = sw_write(bar0, 0x68, 0x00);
	if (ret < 0)
		return ret;

	/*
	 * TX1 is the internal PCB trace connecting IT6664 directly to the
	 * IT6805 capture receiver. Unlike external HDMI OUT (TX2), TX1 is
	 * hardwired on the board and MUST always be powered and enabled
	 * when RX is locked.
	 *
	 * IT6805 may have its HPD deasserted in standby/no-signal until it
	 * senses TMDS clock/5V. Never gate TX1 power or output on tx_status & 0x01
	 * (HPD), otherwise the two chips deadlock.
	 */
	(void)gc573_user_set_tx_hpd_gate(bar0, 1, true);

	ret = it6664_power_on_tx_port(bar0, 1);
	if (ret < 0)
		pr_warn("IT6664 TX1 power on failed: %d", ret);

	ret = it6664_enable_tx_output(bar0, 1);
	if (ret < 0)
		pr_warn("IT6664 TX1 output enable failed: %d", ret);
	else
		pr_info("IT6664 TX1 output enabled (capture link active).");

	/* HDMI OUT passthrough is optional and must never fail capture. */
	(void)it6664_arm_tx2(bar0);

	ret = it6664_set_rx_hdcp_off(bar0);
	if (ret < 0)
		return ret;

	ret = rx_read(bar0, IT6664_RX_BANK_0, IT6664_RX_REG_STATUS_14,
		      &status14);
	if (ret < 0)
		return ret;
	ret = rx_update(bar0, IT6664_RX_BANK_3, 0xa7, 0x40,
			(status14 & 0x01) << 6);
	if (ret < 0)
		return ret;

	pr_info("IT6664 RX lock handled (status14=0x%02x).", status14);
	return 0;
}

int gc573_user_it6664_poll(void *bar0, struct gc573_user_it6664_status *status)
{
	struct gc573_user_it6664_status local = { 0 };
	u8 value = 0;
	int ret;

	if (!bar0)
		return -EINVAL;
	if (!status)
		status = &local;

	memset(status, 0, sizeof(*status));

	ret = rx_read(bar0, IT6664_RX_BANK_0, IT6664_RX_REG_STATUS, &value);
	if (ret < 0)
		return ret;
	status->rx_status13 = value;
	status->five_volt_present = (value & 0x01) != 0;

	if (status->five_volt_present && !g_rx_last_5v) {
		g_rx_last_5v = true;
		pr_info("IT6664 5V detected (console powered on / connected); pulsing RX HPD...");
		it6664_rx_set_hpd(bar0, false);
		msleep(120);
		it6664_rx_set_hpd(bar0, true);
	} else if (!status->five_volt_present && g_rx_last_5v) {
		g_rx_last_5v = false;
	}

	ret = rx_read(bar0, IT6664_RX_BANK_0, IT6664_RX_REG_STATUS_14, &value);
	if (ret < 0)
		return ret;
	status->rx_status14 = value;

	ret = rx_read(bar0, IT6664_RX_BANK_0, IT6664_RX_REG_SCDT, &value);
	if (ret < 0)
		return ret;
	status->rx_scdt = value;
	status->scdt_locked = (value & 0x80) != 0;

	ret = tx_read(bar0, IT6664_TX1_ADDR, IT6664_TX_REG_STATUS, &value);
	if (ret < 0)
		return ret;
	status->tx1_status = value;
	status->tx1_hpd = (value & 0x01) != 0;

	ret = tx_read(bar0, IT6664_TX2_ADDR, IT6664_TX_REG_STATUS, &value);
	if (ret < 0)
		return ret;
	status->tx2_status = value;
	status->tx2_hpd = (value & 0x01) != 0;

	if (status->scdt_locked && !g_rx_locked) {
		g_rx_locked = true;
		pr_info("IT6664 RX locked (status13=0x%02x scdt=0x%02x).",
			status->rx_status13, status->rx_scdt);
		ret = it6664_handle_rx_lock(bar0);
		if (ret < 0)
			return ret;
	} else if (!status->scdt_locked && g_rx_locked) {
		g_rx_locked = false;
		gc573_user_set_gpio(bar0, 3, false, 0);
		/* Forget TX states so a re-lock re-arms TX1 and TX2 cleanly. */
		g_tx2_powered = false;
		g_tx2_output_enabled = false;
		g_tx2_error_reported = false;
		g_tx2_hpd_last = status->tx2_hpd;
		g_tx1_powered = false;
		g_tx1_pclk_khz = 0;
		pr_info("IT6664 RX lost sync (status13=0x%02x scdt=0x%02x).",
			status->rx_status13, status->rx_scdt);
	}

	if (status->tx1_hpd && !g_tx1_powered) {
		ret = it6664_power_on_tx_port(bar0, 1);
		if (ret < 0)
			return ret;
	}

	/*
	 * HDMI OUT behaves like a hardware distribution amplifier: it keeps
	 * transmitting for as long as the RX is locked, independent of the
	 * downstream HPD.  Several monitors only assert HPD after they see a
	 * TMDS clock, so waiting for HPD before unblanking deadlocks the link
	 * (the exact symptom of #48).  HPD edges are only logged; a live link is
	 * never re-armed and the video PLL is never reset.
	 */
	if (g_rx_locked) {
		bool sink = status->tx2_hpd;
		bool hpd_rise = sink && !g_tx2_hpd_last;
		bool can_retry = !g_tx2_error_reported || hpd_rise;

		if (g_tx2_output_enabled) {
			/*
			 * A live link is left completely alone: re-running the output
			 * stage (SCDC, 0xc1 pulse, 100 ms) on an HPD edge would blank
			 * the picture the monitor is just locking onto.  Only keep the
			 * protocol mode in sync (write-on-mismatch).
			 */
			(void)it6664_set_tx_hdmi_mode(bar0, IT6664_TX2_ADDR, true,
						      NULL);
		} else if (can_retry) {
			if (it6664_arm_tx2(bar0) == -EAGAIN) {
				/* Video PLL down: recover with a full power cycle. */
				g_tx2_powered = false;
				(void)it6664_arm_tx2(bar0);
			}
		}

		if (sink != g_tx2_hpd_last)
			it6664_log_tx2_state(bar0, sink ? "sink HPD asserted" :
					     "sink HPD lost (output kept live)");
		g_tx2_hpd_last = sink;
	} else if (g_tx2_powered || g_tx2_output_enabled ||
		   g_tx2_error_reported) {
		g_tx2_powered = false;
		g_tx2_output_enabled = false;
		g_tx2_error_reported = false;
		g_tx2_hpd_last = status->tx2_hpd;
		pr_info("IT6664 TX2 passthrough idle (RX not locked).");
	}

	return 0;
}

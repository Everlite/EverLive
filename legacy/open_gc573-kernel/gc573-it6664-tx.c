// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/math64.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "gc573.h"
#include "gc573-it6664.h"

#define IT6664_SWITCH_REG_BANK	0x0f
#define IT6664_RX_REG_BANK	0x0f
#define IT6664_RX_REG_STATUS	0x13
#define IT6664_RX_BANK_MASK	GENMASK(1, 0)
#define IT6664_RX_BANK_0		0x00
#define IT6664_RX_BANK_2		0x02
#define IT6664_TX_REG_STATUS	0x03
#define IT6664_TX_REG_IRQ_BASE	0x10
#define IT6664_TX_IRQ_VIDEO_STATUS	BIT(7)
#define IT6664_TX_IRQ1_HDCP_FAIL	BIT(0)
#define IT6664_TX_IRQ1_HDCP_DONE	BIT(1)
#define IT6664_TX_IRQ1_HDCP_STATUS	BIT(6)
#define IT6664_TX_IRQ1_HDCP_START	BIT(7)
#define IT6664_TX_PCLK_SAMPLES	10
#define IT6664_TX_PCLK_MAX_KHZ	621000
#define IT6664_TX_EDID_CHUNK_SIZE	0x20
#define IT6664_TX_EDID_RETRIES	4
#define IT6664_TX_EDID_PARSE_ATTEMPTS	3
#define IT6664_TX_EDID_SESSIONS	3
#define IT6664_TX_REG_DDC_ENABLE	0x28
#define IT6664_TX_REG_DDC_SLAVE	0x29
#define IT6664_TX_REG_DDC_OFFSET	0x2a
#define IT6664_TX_REG_DDC_COUNT	0x2b
#define IT6664_TX_REG_DDC_HEADER	0x2c
#define IT6664_TX_REG_DDC_SEGMENT	0x2d
#define IT6664_TX_REG_DDC_COMMAND	0x2e
#define IT6664_TX_REG_DDC_STATUS	0x2f
#define IT6664_TX_REG_DDC_FIFO	0x30
#define IT6664_TX_REG_DDC_RESET	0x35
#define IT6664_TX_SCDC_SLAVE	0xa8
#define IT6664_TX_SCDC_VERSION	0x02
#define IT6664_TX_SCDC_TMDS_CONFIG	0x20
#define IT6664_TX_SCDC_RETRIES	11
#define IT6664_TX_HDCP_DDC_SLAVE	0x74
#define IT6664_TX_HDCP_BKSV_SIZE	5
#define IT6664_TX_HDCP_FIRE_LIMIT	0xfe
#define IT6664_TX_HDCP_REAUTH_LIMIT	0x1f
#define IT6664_TX_HDCP2_MAX_PCLK_KHZ	330001

static int
it6664_handle_tx_hdcp_irq(struct gc555_it6664 *it6664,
			  unsigned int port, u8 irq);
static int
it6664_handle_tx_hdcp_state(struct gc555_it6664 *it6664,
			    unsigned int port);

static const u8 it6664_tx_irq_enable[] = {
	0xc0, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static const u8 it6664_tx_scaler_mode[] = {
	0x20, 0x21, 0x22, 0x1f, 0x10,
	0x20, 0x21, 0x22, 0x1f, 0x10,
};

static struct regmap *it6664_tx_port_map(struct gc555_it6664 *it6664,
					 unsigned int port)
{
	return it6664->maps[IT6664_MAP_TX_PORT0 + port].regmap;
}

static int it6664_read_byte(struct regmap *map, unsigned int reg, u8 *value)
{
	unsigned int raw;
	int ret;

	ret = regmap_read(map, reg, &raw);
	if (ret)
		return ret;
	*value = raw;

	return 0;
}

static int it6664_reset_tx_video_clock(struct regmap *tx_port)
{
	int ret;

	ret = regmap_write(tx_port, 0x01, 0x06);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x94, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x94, BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x01, 0x04);
	if (ret)
		return ret;

	return regmap_write(tx_port, 0x01, 0x00);
}

static int it6664_reset_tx_port(struct gc555_it6664 *it6664,
				unsigned int port)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	int ret;

	ret = it6664_reset_tx_video_clock(tx_port);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x01, BIT(5), BIT(5));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x01, BIT(5), 0);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x0c, 0x10 << port);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x0c, 0x00);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x18, BIT(7), 0);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x19, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1a, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1b, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1c, 0x00);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x35, BIT(4), BIT(4));
	if (ret)
		return ret;

	return it6664_write_bits(tx_port, 0x35, BIT(4), 0);
}

static int it6664_reset_inactive_tx_paths(struct gc555_it6664 *it6664)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	u8 status1;
	u8 status2;
	int ret;

	ret = it6664_read_byte(it6664_tx_port_map(it6664, 1), 0x03,
			       &status1);
	if (ret)
		return ret;
	ret = it6664_read_byte(it6664_tx_port_map(it6664, 2), 0x03,
			       &status2);
	if (ret)
		return ret;
	if ((status1 | status2) & BIT(0))
		return 0;

	ret = it6664_write_bits(sw, 0x1a, BIT(1), 0);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x19, 0x0f);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x1d, 0xaf);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x19, 0x3f);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x07, 0xff);
	if (ret)
		return ret;

	ret = it6664_write_bits(sw, 0x1a, BIT(1), BIT(1));
	if (ret)
		return ret;

	it6664->runtime.rx.timer_state = IT6664_RX_TIMER_TX_OFF;
	return 0;
}

static int it6664_pulse_bits(struct regmap *map, unsigned int reg,
			     unsigned int mask)
{
	int ret;

	ret = it6664_write_bits(map, reg, mask, mask);
	if (ret)
		return ret;

	return it6664_write_bits(map, reg, mask, 0);
}

static int it6664_release_tx_source(struct gc555_it6664 *it6664,
				    unsigned int port)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	enum it6664_tx_source source = it6664->runtime.tx[port].source;
	unsigned int converter_users = 0;
	unsigned int scaler_users = 0;
	unsigned int active_port;
	int ret;

	for (active_port = 1; active_port <= 2; active_port++) {
		if (it6664->runtime.tx[active_port].source ==
		    IT6664_TX_SOURCE_CONVERTER)
			converter_users++;
		if (it6664->runtime.tx[active_port].source ==
		    IT6664_TX_SOURCE_SCALER)
			scaler_users++;
	}

	if (source == IT6664_TX_SOURCE_CONVERTER && converter_users < 2) {
		ret = it6664_pulse_bits(sw, 0x0b, 0x1c);
		if (ret)
			return ret;

		return regmap_write(sw, 0x67, 0x00);
	}
	if (source != IT6664_TX_SOURCE_SCALER || scaler_users >= 2)
		return 0;

	if (converter_users < 2 &&
	    it6664->runtime.rx.colorspace == IT6664_RX_COLORSPACE_YCBCR420) {
		ret = it6664_pulse_bits(sw, 0x0b, 0x1c);
		if (ret)
			return ret;
		ret = it6664_pulse_bits(sw, 0xe0, 0xe0);
		if (ret)
			return ret;

		return regmap_write(sw, 0x67, 0x00);
	}

	return it6664_pulse_bits(sw, 0xe0, 0xe0);
}

static int it6664_power_down_tx_port(struct gc555_it6664 *it6664,
				     unsigned int port)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	int ret;

	ret = it6664_write_bits(sw, 0x08, BIT(port), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x18, 0xdc, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x19, 0x07, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1a, 0xff, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1b, 0xff, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1c, 0xff, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x84, 0x60, 0x60);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x86, BIT(3), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x88, 0x03, 0x03);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x01, 0x26, 0x26);
	if (ret)
		return ret;
	ret = it6664_release_tx_source(it6664, port);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x0d, 0x03 << (port * 2), 0);
	if (ret)
		return ret;
	ret = it6664_reset_inactive_tx_paths(it6664);
	if (ret)
		return ret;
	ret = it6664_reset_tx_port(it6664, port);
	if (ret)
		return ret;

	state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;
	state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
	state->source_hdcp_level = GC555_HDCP_NONE;
	state->hdcp_fire_version = 0;
	state->hdcp_wait_count = 0;
	state->hdcp_fire_count = 0;
	state->hdcp_status_count = 0;
	state->powered = false;
	state->hpd = false;
	state->afe_configured = false;
	state->high_bandwidth = false;
	state->scrambling_required = false;
	state->video_stable = false;
	state->tmds_stable = false;
	state->scdc_configured = false;
	state->hdcp_going = false;
	state->hdcp_done = false;
	state->hdcp2_done = false;
	state->hdcp2_rsa_busy = false;
	state->force_hdcp1 = false;
	state->source = IT6664_TX_SOURCE_NONE;
	it6664->runtime.tx_hpd_mask &= (u8)~BIT(port);

	return 0;
}

int gc555_it6664_tx_power_down_all(struct gc555_it6664 *it6664)
{
	unsigned int port;
	int ret;

	for (port = 0; port < IT6664_TX_PORT_COUNT; port++) {
		ret = it6664_power_down_tx_port(it6664, port);
		if (ret)
			return ret;
	}

	return 0;
}

static int it6664_refresh_rx_color_depth(struct gc555_it6664 *it6664)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 value;
	int ret;

	ret = it6664_read_byte(rx, 0x98, &value);
	if (ret)
		return ret;

	it6664->runtime.rx.color_depth = value >> 4;
	return 0;
}

static int it6664_measure_tx_pclk(struct gc555_it6664 *it6664,
				  unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u32 initial_count;
	u32 average_count;
	u32 denominator;
	u32 divider;
	u32 sample_sum = 0;
	u64 pclk_khz;
	u8 count_lo;
	u8 count_hi;
	unsigned int sample;
	int ret;

	ret = it6664_write_bits(tx_port, 0x07, BIT(7), BIT(7));
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = it6664_write_bits(tx_port, 0x07, BIT(7), 0);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0x06, &count_lo);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0x07, &count_hi);
	if (ret)
		return ret;

	initial_count = ((count_hi & GENMASK(3, 0)) << 9) | (count_lo << 1);
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

	for (sample = 0; sample < IT6664_TX_PCLK_SAMPLES; sample++) {
		u8 selector = divider << 4;

		ret = it6664_write_bits(tx_port, 0x07, GENMASK(7, 4),
					selector | BIT(7));
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0x07, GENMASK(7, 4),
					selector);
		if (ret)
			return ret;
		ret = it6664_read_byte(tx_port, 0x06, &count_lo);
		if (ret)
			return ret;
		ret = it6664_read_byte(tx_port, 0x07, &count_hi);
		if (ret)
			return ret;
		sample_sum += ((count_hi & GENMASK(3, 0)) << 9) |
			      (count_lo << 1);
	}

	denominator = IT6664_TX_PCLK_SAMPLES << divider;
	average_count = sample_sum >= denominator ?
			sample_sum / denominator : 1;
	if (!it6664->rclk_khz)
		return -ERANGE;
	pclk_khz = div64_u64((u64)it6664->rclk_khz << 12, average_count);

	ret = it6664_refresh_rx_color_depth(it6664);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xaf, GENMASK(7, 6), 0);
	if (ret)
		return ret;
	if ((it6664->runtime.rx.color_depth & GENMASK(1, 0)) == 1)
		pclk_khz = div64_u64(pclk_khz * 5, 4);
	else if ((it6664->runtime.rx.color_depth & GENMASK(1, 0)) == 2)
		pclk_khz = div64_u64(pclk_khz * 3, 2);
	if (pclk_khz >= IT6664_TX_PCLK_MAX_KHZ)
		pclk_khz = 0;

	state->pclk_khz = pclk_khz;
	return 0;
}

static int
it6664_configure_tx_afe(struct gc555_it6664 *it6664, unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u32 pclk_khz = state->pclk_khz;
	bool high_bandwidth;
	u8 low_clock_value;
	u8 reg84;
	u8 reg87;
	u8 reg88;
	u8 reg89;
	u8 reg8b;
	int ret;

	reg84 = pclk_khz > 100000 ? 0x04 : 0x03;
	reg88 = pclk_khz > 162000 ? 0x04 : 0x00;
	low_clock_value = pclk_khz > 150000 ? 0x09 : 0x03;
	if (pclk_khz > 375000) {
		high_bandwidth = true;
		reg87 = 0x0e;
		reg89 = 0x25;
		reg8b = 0x0d;
	} else if (pclk_khz > 310000) {
		high_bandwidth = true;
		reg87 = 0x0d;
		reg89 = 0x25;
		reg8b = 0x0b;
	} else {
		high_bandwidth = false;
		reg87 = low_clock_value;
		reg89 = pclk_khz > 150000 ? 0x21 : 0x80;
		reg8b = low_clock_value;
	}
	if (port == 2) {
		reg87 = low_clock_value;
		reg8b = low_clock_value;
	}

	ret = it6664_write_bits(tx_port, 0x84, GENMASK(2, 0), reg84);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x88, BIT(2), reg88);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x87, GENMASK(4, 0), reg87);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x89, 0xbf, reg89);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x8a, GENMASK(3, 0), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x8b, GENMASK(3, 0), reg8b);
	if (ret)
		return ret;

	state->high_bandwidth = high_bandwidth;
	return 0;
}

static int it6664_power_on_tx_port(struct gc555_it6664 *it6664,
				   unsigned int port)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	unsigned int i;
	int ret;

	ret = it6664_write_bits(sw, 0x08, BIT(port), BIT(port));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc1, 0xf0, 0x80);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x84, 0xe0, 0x80);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x86, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x02, BIT(0), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x19, GENMASK(2, 0),
				GENMASK(2, 0));
	if (ret)
		return ret;
	if (it6664->runtime.rx.scdt ||
	    it6664->runtime.rx.clock_configured) {
		for (i = 0; i < ARRAY_SIZE(it6664_tx_irq_enable); i++) {
			ret = regmap_write(tx_port, IT6664_TX_REG_IRQ_BASE + i,
					   it6664_tx_irq_enable[i]);
			if (ret)
				return ret;
		}
		if (!state->afe_configured) {
			ret = regmap_write(tx_port, 0x01, 0x24);
			if (ret)
				return ret;
			ret = regmap_write(tx_port, 0x01, 0x00);
			if (ret)
				return ret;
			ret = it6664_measure_tx_pclk(it6664, port);
			if (ret)
				return ret;
			ret = it6664_configure_tx_afe(it6664, port);
			if (ret)
				return ret;
			msleep(100);
			ret = it6664_reset_tx_video_clock(tx_port);
			if (ret)
				return ret;
			ret = it6664_write_bits(tx_port, 0x18, BIT(7), BIT(7));
			if (ret)
				return ret;
			state->afe_configured = true;
		}
	}

	state->hdcp_state = IT6664_TX_HDCP_RESET;
	state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;
	state->powered = true;
	state->video_stable = false;

	return 0;
}

static int
it6664_reset_tx_signal_path(struct gc555_it6664 *it6664, unsigned int port)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	int ret;

	ret = it6664_write_bits(tx_port, 0x18, BIT(7), 0);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x19, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1a, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1b, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1c, 0x00);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x88, GENMASK(1, 0),
				GENMASK(1, 0));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x01, GENMASK(1, 0),
				GENMASK(1, 0));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x01, GENMASK(1, 0), 0);
	if (ret)
		return ret;

	state->hdcp_state = IT6664_TX_HDCP_RESET;
	state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;
	state->video_stable = false;
	state->tmds_stable = false;
	return 0;
}

int
gc555_it6664_tx_reset_signal(struct gc555_it6664 *it6664, bool *active)
{
	unsigned int port;
	int ret;

	*active = false;
	for (port = 1; port <= 2; port++) {
		u8 status;

		ret = it6664_read_byte(it6664_tx_port_map(it6664, port),
				       IT6664_TX_REG_STATUS, &status);
		if (ret)
			return ret;
		if (!(status & BIT(0)))
			continue;

		ret = it6664_reset_tx_signal_path(it6664, port);
		if (ret)
			return ret;
		*active = true;
	}
	if (*active)
		for (port = 1; port <= 2; port++)
			it6664->runtime.tx[port].afe_configured = false;

	return 0;
}

int gc555_it6664_tx_power_connected_ports(struct gc555_it6664 *it6664)
{
	unsigned int port;
	int ret;

	for (port = 1; port <= 2; port++) {
		u8 status;

		ret = it6664_read_byte(it6664_tx_port_map(it6664, port),
				       IT6664_TX_REG_STATUS, &status);
		if (ret)
			return ret;
		if (!(status & BIT(0)))
			continue;

		ret = it6664_power_on_tx_port(it6664, port);
		if (ret)
			return ret;
	}

	return 0;
}

static int it6664_initialize_tx_port(struct gc555_it6664 *it6664,
				     unsigned int port)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	int ret;

	ret = it6664_write_bits(tx_port, 0x08, 0x1c, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x02, BIT(1), BIT(1));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x41, BIT(0), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc0, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x34, 0xc0, 0x80);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x35, 0x03, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x3a, 0xfc, 0x90);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x93, 0xff, 0x40);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x94, 0x3e, 0x26);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc0, BIT(4), BIT(4));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc1, BIT(2), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc3, 0x0f, 0x01);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x18, 0x03, 0x03);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x19, 0x07);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1a, 0x03);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1b, 0xff);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1c, 0x03);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x88, 0x54);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x8a, 0x00);
	if (ret)
		return ret;

	return regmap_write(tx_port, 0x8b, 0x07);
}

static int it6664_initialize_tx_reset_path(struct gc555_it6664 *it6664)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	struct regmap *tx_common =
		it6664->maps[IT6664_MAP_TX_COMMON].regmap;
	unsigned int port;
	int ret;

	ret = it6664_write_bits(rx, 0xc5, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = regmap_write(tx_common, 0x20, 0x02);
	if (ret)
		return ret;
	ret = regmap_write(tx_common, 0x20, 0x00);
	if (ret)
		return ret;
	ret = it6664_power_down_tx_port(it6664, 0);
	if (ret)
		return ret;
	ret = it6664_power_down_tx_port(it6664, 3);
	if (ret)
		return ret;

	for (port = 1; port <= 2; port++) {
		struct regmap *tx_port = it6664_tx_port_map(it6664, port);

		ret = it6664_write_bits(tx_port, 0xc1, BIT(0), BIT(0));
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0x01, BIT(0), BIT(0));
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0x01, BIT(0), 0);
		if (ret)
			return ret;
		ret = it6664_initialize_tx_port(it6664, port);
		if (ret)
			return ret;
		ret = it6664_reset_tx_port(it6664, port);
		if (ret)
			return ret;
		ret = it6664_power_down_tx_port(it6664, port);
		if (ret)
			return ret;
	}

	return it6664_write_bits(tx_common, 0x15, BIT(3), BIT(3));
}

static int it6664_configure_tx_switch(struct gc555_it6664 *it6664)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx_port;
	unsigned int port;
	bool bank_one = false;
	int cleanup_ret;
	int ret;

	tx_port = it6664_tx_port_map(it6664, 0);
	ret = regmap_write(tx_port, 0x03, 0x03);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, 0x84, 0x60);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, 0x86, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, 0x88, 0x0b);
	if (ret)
		goto cleanup;

	tx_port = it6664_tx_port_map(it6664, 3);
	ret = regmap_write(tx_port, 0x84, 0x60);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, 0x86, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, 0x88, 0x0b);
	if (ret)
		goto cleanup;

	ret = it6664_write_bits(sw, 0x08, 0x0f, 0x0f);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, 0x0d, 0xff, 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, 0x6b, 0x3c, 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, 0x6c, 0x38, 0x20);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, 0x18, BIT(4), BIT(4));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK,
				BIT(0), BIT(0));
	if (ret)
		goto cleanup;
	bank_one = true;
	ret = it6664_write_bits(sw, 0x10, 0x49, 0x41);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, 0x1d, BIT(7), BIT(7));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, 0x20, 0x78, 0x78);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), 0);
	if (ret)
		goto cleanup;
	bank_one = false;
	ret = it6664_write_bits(sw, 0x19, 0x3f, 0x0f);
	if (ret)
		goto cleanup;
	ret = regmap_write(sw, 0x2b, 0xff);
	if (ret)
		goto cleanup;
	ret = regmap_write(sw, 0x2d, 0x0f);
	if (ret)
		goto cleanup;
	ret = regmap_write(sw, 0x2e, 0xff);
	if (ret)
		goto cleanup;
	ret = regmap_write(sw, 0x30, 0x0f);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, 0x6d, 0x30, 0);
	if (ret)
		goto cleanup;
	usleep_range(10000, 11000);

	for (port = 0; port < IT6664_TX_PORT_COUNT; port++) {
		tx_port = it6664_tx_port_map(it6664, port);
		ret = it6664_write_bits(tx_port, 0x41, BIT(0), 0);
		if (ret)
			goto cleanup;
		ret = it6664_write_bits(tx_port, 0xc1, BIT(0), BIT(0));
		if (ret)
			goto cleanup;
		ret = it6664_write_bits(tx_port, 0x88, BIT(0), BIT(0));
		if (ret)
			goto cleanup;
	}

cleanup:
	if (bank_one) {
		cleanup_ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK,
						BIT(0), 0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static int it6664_read_tx_irq(struct gc555_it6664 *it6664,
			      unsigned int port,
			      struct it6664_tx_irq *snapshot)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	unsigned int i;
	int ret;

	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS,
			       &snapshot->status);
	if (ret)
		return ret;

	for (i = 0; i < IT6664_TX_IRQ_COUNT; i++) {
		ret = it6664_read_byte(tx_port, IT6664_TX_REG_IRQ_BASE + i,
				       &snapshot->irq[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int it6664_ack_tx_irq(struct gc555_it6664 *it6664,
			     unsigned int port,
			     struct it6664_tx_irq_pending *pending)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	unsigned int i;
	int ret;

	for (i = 0; i < IT6664_TX_IRQ_COUNT; i++) {
		if (pending->acknowledged & BIT(i))
			continue;
		ret = regmap_write(tx_port, IT6664_TX_REG_IRQ_BASE + i,
				   pending->snapshot.irq[i]);
		if (ret)
			return ret;
		pending->acknowledged |= BIT(i);
	}

	return 0;
}

static void it6664_set_tx_hpd_state(struct gc555_it6664 *it6664,
				    unsigned int port, bool high)
{
	it6664->runtime.tx[port].hpd = high;
	if (high)
		it6664->runtime.tx_hpd_mask |= BIT(port);
	else
		it6664->runtime.tx_hpd_mask &= (u8)~BIT(port);
}

static int it6664_cycle_tx_port(struct gc555_it6664 *it6664,
				unsigned int port)
{
	int ret;

	ret = it6664_power_down_tx_port(it6664, port);
	if (ret)
		return ret;

	return it6664_power_on_tx_port(it6664, port);
}

static int
it6664_handle_tx_video_status_irq(struct gc555_it6664 *it6664,
				  unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 status;
	u8 rx_status;
	int ret;

	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & BIT(0))) {
		state->video_state = IT6664_TX_VIDEO_STABLE;
		return 0;
	}

	if (!(status & BIT(2))) {
		if (!state->video_stable && it6664->runtime.rx.scdt)
			goto assess_tmds;
		state->video_state = IT6664_TX_VIDEO_STABLE_OFF;
		return 0;
	}
	if (state->video_stable || !it6664->runtime.rx.scdt)
		goto assess_tmds;

	it6664->runtime.tx_hpd_mask |= BIT(port);
	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS, &rx_status);
	if (ret)
		return ret;
	if (!(rx_status & BIT(4))) {
		state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;
		return 0;
	}
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (state->video_state != IT6664_TX_VIDEO_OK &&
	    ((status & BIT(7)) || state->pclk_khz < 31000))
		state->video_state = IT6664_TX_VIDEO_STABLE;

	return 0;

assess_tmds:
	if (!(status & BIT(3))) {
		if (state->tmds_stable)
			state->video_state = IT6664_TX_VIDEO_RESET;
	} else if (state->video_stable && !state->tmds_stable) {
		state->video_state = IT6664_TX_VIDEO_STABLE;
	}

	return 0;
}

static int
it6664_handle_tx1_irq(struct gc555_it6664 *it6664,
		      const struct it6664_tx_irq *snapshot)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, 1);
	struct it6664_tx_port_state *state = &it6664->runtime.tx[1];
	u8 status = snapshot->status;
	int ret;

	if ((snapshot->irq[0] & BIT(0)) && (status & BIT(0))) {
		ret = gc555_it6664_rx_set_hpd(it6664, true);
		if (ret)
			return ret;
		if (it6664->runtime.tx_hpd_mask & BIT(1)) {
			ret = it6664_cycle_tx_port(it6664, 1);
			if (ret)
				return ret;
		}
		ret = gc555_link_set_tx_hpd_gate(it6664->gc555, 1, true);
		if (ret)
			return ret;
		state->hpd = true;
	}

	if (!(snapshot->irq[0] & BIT(1))) {
		if (snapshot->irq[0] & IT6664_TX_IRQ_VIDEO_STATUS)
			return it6664_handle_tx_video_status_irq(it6664, 1);
		return 0;
	}

	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & BIT(1)))
		return 0;

	if (!(it6664->runtime.tx_hpd_mask & BIT(1)) && (status & BIT(2))) {
		state->video_state = IT6664_TX_VIDEO_STABLE;
	} else {
		ret = it6664_power_down_tx_port(it6664, 1);
		if (ret)
			return ret;
	}
	ret = it6664_power_on_tx_port(it6664, 1);
	if (ret)
		return ret;
	ret = gc555_it6664_rx_set_hpd(it6664, true);
	if (ret)
		return ret;
	ret = gc555_link_set_tx_hpd_gate(it6664->gc555, 1, true);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	it6664_set_tx_hpd_state(it6664, 1, status & BIT(0));
	if (snapshot->irq[0] & IT6664_TX_IRQ_VIDEO_STATUS)
		return it6664_handle_tx_video_status_irq(it6664, 1);

	return 0;
}

static void it6664_reset_tx2_edid_state(struct gc555_it6664 *it6664)
{
	memset(&it6664->runtime.sink_edid, 0,
	       sizeof(it6664->runtime.sink_edid));
	memset(&it6664->runtime.tx[2].sink_caps, 0,
	       sizeof(it6664->runtime.tx[2].sink_caps));
	it6664->runtime.tx[2].edid_attempted = false;
	it6664->runtime.tx[2].edid_parsed = false;
	it6664->runtime.tx[2].edid_sessions = 0;
	it6664->runtime.tx[2].dvi_mode = false;
	it6664->runtime.merge_attempted = false;
	it6664->runtime.merged_edid_pending = false;
	it6664->runtime.edid_publish_sessions = 0;
}

static int
it6664_handle_tx2_irq(struct gc555_it6664 *it6664,
		      const struct it6664_tx_irq *snapshot)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, 2);
	struct it6664_tx_port_state *state = &it6664->runtime.tx[2];
	const bool was_hpd = it6664->runtime.tx_hpd_mask & BIT(2);
	u8 status = snapshot->status;
	int ret;

	if (snapshot->irq[0] & BIT(0)) {
		ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
		if (ret)
			return ret;
		if (!(status & BIT(0))) {
			ret = it6664_power_down_tx_port(it6664, 2);
			if (ret)
				return ret;
			it6664_reset_tx2_edid_state(it6664);
			ret = gc555_link_set_tx_hpd_gate(it6664->gc555, 2,
							 false);
			if (ret)
				return ret;
		} else {
			ret = gc555_it6664_rx_set_hpd(it6664, true);
			if (ret)
				return ret;
			if (state->powered)
				ret = it6664_cycle_tx_port(it6664, 2);
			else
				ret = it6664_power_on_tx_port(it6664, 2);
			if (ret)
				return ret;
			ret = gc555_link_set_tx_hpd_gate(it6664->gc555, 2,
							 true);
			if (ret)
				return ret;
			state->hpd = true;
		}
	}

	if (!(snapshot->irq[0] & BIT(1))) {
		if (snapshot->irq[0] & IT6664_TX_IRQ_VIDEO_STATUS)
			return it6664_handle_tx_video_status_irq(it6664, 2);
		return 0;
	}

	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & BIT(1))) {
		ret = it6664_power_down_tx_port(it6664, 2);
		if (ret)
			return ret;
		it6664_reset_tx2_edid_state(it6664);
		return gc555_link_set_tx_hpd_gate(it6664->gc555, 2, false);
	}

	if (!was_hpd && (status & BIT(2))) {
		state->video_state = IT6664_TX_VIDEO_STABLE;
	} else {
		ret = it6664_power_down_tx_port(it6664, 2);
		if (ret)
			return ret;
	}
	ret = it6664_power_on_tx_port(it6664, 2);
	if (ret)
		return ret;
	ret = gc555_it6664_rx_set_hpd(it6664, true);
	if (ret)
		return ret;
	ret = gc555_link_set_tx_hpd_gate(it6664->gc555, 2, true);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	it6664_set_tx_hpd_state(it6664, 2, status & BIT(0));
	if (snapshot->irq[0] & IT6664_TX_IRQ_VIDEO_STATUS)
		return it6664_handle_tx_video_status_irq(it6664, 2);

	return 0;
}

static int it6664_tx_ddc_recover(struct gc555_it6664 *it6664,
				 unsigned int port, u8 command,
				 bool read_status)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 status;
	int ret;

	if (read_status) {
		ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS,
				       &status);
		if (ret)
			return ret;
	}
	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE, BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, command);
	if (ret)
		return ret;

	return it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE, BIT(0), 0);
}

static int it6664_recover_tx_ddc_hang(struct gc555_it6664 *it6664,
				      unsigned int port,
				      struct it6664_tx_irq_pending *pending)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 control;
	int ret;

	if (!pending->ddc_hang_stage) {
		ret = it6664_read_byte(tx_port, 0x41, &control);
		if (ret)
			return ret;
		pending->ddc_hang_restore = control & BIT(0);
		pending->ddc_hang_stage = 1;
	}
	if (pending->ddc_hang_stage == 1 && pending->ddc_hang_restore) {
		ret = it6664_write_bits(tx_port, 0x41, BIT(0), 0);
		if (ret)
			return ret;
		pending->ddc_hang_stage = 2;
	}
	if (pending->ddc_hang_stage == 2) {
		ret = it6664_write_bits(tx_port, 0x01, BIT(5), BIT(5));
		if (ret)
			return ret;
		pending->ddc_hang_stage = 3;
	}
	if (pending->ddc_hang_stage == 1)
		pending->ddc_hang_stage = 3;
	if (pending->ddc_hang_stage == 3) {
		ret = it6664_tx_ddc_recover(it6664, port, 0x0a, false);
		if (ret)
			return ret;
		pending->ddc_hang_stage = 4;
	}
	if (!pending->ddc_hang_restore)
		return 0;
	if (pending->ddc_hang_stage == 4) {
		ret = it6664_write_bits(tx_port, 0x01, BIT(5), 0);
		if (ret)
			return ret;
		pending->ddc_hang_stage = 5;
	}
	if (pending->ddc_hang_stage == 5) {
		ret = it6664_write_bits(tx_port, 0x41, BIT(0), BIT(0));
		if (ret)
			return ret;
		pending->ddc_hang_stage = 6;
	}
	state->hdcp_going = false;
	state->hdcp_done = false;

	return 0;
}

static bool it6664_tx_irq_bit_deferred(unsigned int port,
				       unsigned int reg, unsigned int bit)
{
	if (reg == 1 && bit == 2)
		return true;
	if (reg == 2 && bit <= 1)
		return true;
	if (reg == 3 && bit == 6)
		return true;
	if (port == 1 && reg == 1 &&
	    (bit == 0 || bit == 1))
		return true;

	return false;
}

static int it6664_consume_tx_irq_bit(struct gc555_it6664 *it6664,
				     unsigned int port,
				     struct it6664_tx_irq_pending *pending,
				     unsigned int reg, unsigned int bit)
{
	struct it6664_tx_irq leaf = pending->snapshot;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 value;

	if (it6664_tx_irq_bit_deferred(port, reg, bit)) {
		it6664->runtime.tx[port].deferred_irq[reg] |= BIT(bit);
		return 0;
	}

	memset(leaf.irq, 0, sizeof(leaf.irq));
	leaf.irq[reg] = BIT(bit);
	if (reg == 0) {
		if (bit == 0 || bit == 1 || bit == 7)
			return port == 1 ?
				it6664_handle_tx1_irq(it6664, &leaf) :
				it6664_handle_tx2_irq(it6664, &leaf);
		if (bit == 2)
			return it6664_tx_ddc_recover(it6664, port, 0x0f,
						      true);
		if (bit == 3)
			return it6664_tx_ddc_recover(it6664, port, 0x09,
						      false);
		return 0;
	}
	if (reg == 1) {
		if (bit == 0 || bit == 1 || bit == 6 || bit == 7)
			return it6664_handle_tx_hdcp_irq(it6664, port,
							  BIT(bit));
		return 0;
	}
	if (reg == 2) {
		if (bit == 3)
			return it6664_recover_tx_ddc_hang(it6664, port,
							   pending);
		if (bit == 6) {
			int ret;

			ret = it6664_read_byte(tx_port, 0x9e, &value);
			if (ret)
				return ret;
			return it6664_read_byte(tx_port, 0x9f, &value);
		}
	}

	return 0;
}

static int it6664_consume_tx_irq(struct gc555_it6664 *it6664,
				 unsigned int port,
				 struct it6664_tx_irq_pending *pending)
{
	static const u8 register_order[] = { 0, 3, 1, 2, 4 };
	unsigned int order;
	unsigned int bit;
	unsigned int reg;
	unsigned int index;
	int ret;

	for (order = 0; order < ARRAY_SIZE(register_order); order++) {
		reg = register_order[order];
		for (bit = 0; bit < 8; bit++) {
			index = reg * 8 + bit;
			if (!(pending->snapshot.irq[reg] & BIT(bit)) ||
			    (pending->consumed & BIT_ULL(index)))
				continue;
			ret = it6664_consume_tx_irq_bit(it6664, port, pending,
							reg, bit);
			if (ret)
				return ret;
			pending->consumed |= BIT_ULL(index);
		}
	}

	return 0;
}

static int it6664_handle_tx_irq(struct gc555_it6664 *it6664,
				unsigned int port,
				struct it6664_tx_irq_pending *pending)
{
	int ret;

	ret = it6664_consume_tx_irq(it6664, port, pending);
	if (ret)
		return ret;
	ret = it6664_ack_tx_irq(it6664, port, pending);
	if (ret)
		return ret;

	dev_dbg(it6664->gc555->dev,
		"IT6664 TX%u IRQ handled status=%02x irq=%5ph deferred=%5ph video=%u hdcp=%u stable=%u/%u\n",
		port, pending->snapshot.status, pending->snapshot.irq,
		it6664->runtime.tx[port].deferred_irq,
		it6664->runtime.tx[port].video_state,
		it6664->runtime.tx[port].hdcp_state,
		it6664->runtime.tx[port].video_stable,
		it6664->runtime.tx[port].tmds_stable);

	return 0;
}

static bool it6664_edid_block_valid(const u8 *block)
{
	u8 checksum = 0;
	unsigned int i;

	for (i = 0; i < IT6664_EDID_BLOCK_SIZE; i++)
		checksum += block[i];

	return !checksum;
}

static int it6664_wait_tx_ddc(struct regmap *tx_port)
{
	u8 status;
	unsigned int attempt;
	int ret;

	usleep_range(15000, 16000);
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS, &status);
	if (ret || (status & BIT(7)))
		return ret;

	usleep_range(15000, 16000);
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS, &status);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS, &status);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0x12, &status);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x35, BIT(4), BIT(4));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x35, BIT(4), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE,
				BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE,
				BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x0f);
	if (ret)
		return ret;

	for (attempt = 0; attempt < 0x38; attempt++) {
		ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS,
				       &status);
		if (ret)
			return ret;
		if (status & 0xb8)
			break;
		usleep_range(1000, 2000);
	}

	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x0f);
	if (ret)
		return ret;
	for (attempt = 0; attempt < 0x38; attempt++) {
		ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS,
				       &status);
		if (ret)
			return ret;
		if (status & 0xb8)
			return 0;
		usleep_range(1000, 2000);
	}

	return -ETIMEDOUT;
}

static void it6664_count_hdcp_fire(struct it6664_tx_port_state *state)
{
	if (state->hdcp_fire_count < IT6664_TX_HDCP_FIRE_LIMIT)
		state->hdcp_fire_count++;
}

static int it6664_read_tx_hdcp(struct gc555_it6664 *it6664,
			       unsigned int port, u8 offset, u8 count)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 status;
	int cleanup_ret;
	int ret;

	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		goto cleanup;
	if (!(status & BIT(0))) {
		ret = -ENOLINK;
		goto cleanup;
	}

	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE,
				BIT(0), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x09);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x0f);
	if (ret)
		goto cleanup;
	usleep_range(3000, 4000);
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x0f);
	if (ret)
		goto cleanup;
	usleep_range(3000, 4000);
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x09);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_SLAVE,
			   IT6664_TX_HDCP_DDC_SLAVE);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_OFFSET, offset);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COUNT, count);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_HEADER, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x00);
	if (ret)
		goto cleanup;
	ret = it6664_wait_tx_ddc(tx_port);

cleanup:
	cleanup_ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE,
					BIT(0), 0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static bool it6664_tx_bksv_valid(const u8 *bksv)
{
	unsigned int i;
	unsigned int bits = 0;

	for (i = 0; i < IT6664_TX_HDCP_BKSV_SIZE; i++)
		bits += hweight8(bksv[i]);

	return bits == 20;
}

static int it6664_monitor_tx_hdcp(struct gc555_it6664 *it6664,
				  unsigned int port)
{
	static const u8 hdcp1_status_regs[] = {
		0x58, 0x59, 0x60, 0x61, 0x66, 0x63, 0x64, 0x65,
	};
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 value;
	u8 reg4e;
	u8 reg4f;
	unsigned int i;
	int ret;

	if (state->hdcp_fire_version == 1) {
		for (i = 0; i < ARRAY_SIZE(hdcp1_status_regs); i++) {
			ret = it6664_read_byte(tx_port, hdcp1_status_regs[i],
						&value);
			if (ret)
				return ret;
		}
		return 0;
	}
	if (state->hdcp_fire_version != 2)
		return 0;

	for (i = 0; i < IT6664_TX_HDCP_BKSV_SIZE; i++) {
		ret = it6664_read_byte(tx_port, 0x5b + i, &value);
		if (ret)
			return ret;
	}
	ret = it6664_read_byte(tx_port, 0x4f, &reg4f);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0x4e, &reg4e);
	if (ret)
		return ret;

	if ((reg4e || reg4f) && (reg4e & BIT(6))) {
		msleep(300);
		ret = it6664_pulse_bits(tx_port, 0x01, BIT(5));
		if (ret)
			return ret;
		state->hdcp2_rsa_busy = true;
		ret = regmap_write(tx_port, 0x11, 0xff);
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0x42, BIT(0), BIT(0));
		if (ret)
			return ret;
		state->hdcp_going = true;
		state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
	}
	if (reg4f & BIT(2))
		return it6664_read_byte(tx_port, 0x64, &value);

	return 0;
}

static int it6664_fire_tx_hdcp2(struct gc555_it6664 *it6664,
				unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	int ret;

	ret = it6664_pulse_bits(tx_port, 0x01, BIT(5));
	if (ret)
		return ret;
	state->hdcp2_rsa_busy = true;
	ret = regmap_write(tx_port, 0x11, 0xff);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x42, BIT(0), BIT(0));
	if (ret)
		return ret;
	state->hdcp_going = true;

	return 0;
}

static int it6664_start_tx_hdcp1(struct gc555_it6664 *it6664,
				 unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 transmitter_data[8];
	u8 bksv[IT6664_TX_HDCP_BKSV_SIZE];
	u8 status;
	u8 reg64;
	u8 reg65;
	u8 reg65_first;
	u8 regc0;
	unsigned int attempt;
	int ret;

	state->hdcp_fire_version = 1;
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & BIT(0)))
		return -ENOLINK;

	ret = it6664_write_bits(tx_port, 0x19, 0x07, 0x07);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1a, 0xb0, 0xb0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x42, BIT(4), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x18, BIT(2), BIT(2));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1a, 0x0b, 0x0b);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x41, BIT(0), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x01, BIT(5), BIT(5));
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = it6664_write_bits(tx_port, 0x01, BIT(5), 0);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x49, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0xff, 0xc3);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0xff, 0xa5);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc2, BIT(6), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc2, 0x1f, 0x0a);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x6b, 0x03, 0x03);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0xff, 0xff);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x41, 0x06, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x6f, 0x0f, 0x01);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x19, 0x07, 0x07);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x12, 0x30);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1a, 0x30, 0);
	if (ret)
		return ret;
	ret = it6664_pulse_bits(tx_port, 0x40, BIT(0));
	if (ret)
		return ret;
	ret = regmap_bulk_read(tx_port, 0x50, transmitter_data,
			       sizeof(transmitter_data));
	if (ret)
		return ret;
	ret = regmap_bulk_write(tx_port, 0x48, transmitter_data,
				sizeof(transmitter_data));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x41, BIT(0), BIT(0));
	if (ret)
		return ret;

	ret = it6664_read_tx_hdcp(it6664, port, 0x41, 2);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0x65, &reg65_first);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0x64, &reg64);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0x65, &reg65);
	if (ret)
		return ret;
	it6664_count_hdcp_fire(state);
	ret = it6664_write_bits(tx_port, 0xc2, BIT(7),
				(reg64 & BIT(7)) || (reg65 & BIT(3)) ?
				BIT(7) : 0);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0xc0, &regc0);
	if (ret)
		return ret;
	if ((regc0 & BIT(0)) && !(reg65_first & BIT(4))) {
		for (attempt = 0; attempt < 12; attempt++) {
			ret = it6664_read_tx_hdcp(it6664, port, 0x41, 2);
			if (ret)
				return ret;
			ret = it6664_read_byte(tx_port, 0x65, &reg65_first);
			if (ret)
				return ret;
			ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS,
						&status);
			if (ret)
				return ret;
			if (!(status & BIT(0)))
				return -ENOLINK;
			if (reg65_first & BIT(4))
				break;
			msleep(15);
		}
	}

	ret = it6664_read_tx_hdcp(it6664, port, 0x00,
				  IT6664_TX_HDCP_BKSV_SIZE);
	if (ret)
		return ret;
	ret = regmap_bulk_read(tx_port, 0x5b, bksv, sizeof(bksv));
	if (ret)
		return ret;
	if (it6664_tx_bksv_valid(bksv)) {
		state->hdcp_going = true;
		state->hdcp_done = false;
		ret = it6664_write_bits(tx_port, 0x91, BIT(4), 0);
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0x42, BIT(0), BIT(0));
		if (ret)
			return ret;
		state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
	} else {
		state->hdcp_going = false;
		ret = it6664_write_bits(tx_port, 0x91, BIT(4), BIT(4));
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0xc1, BIT(0), BIT(0));
		if (ret)
			return ret;
		state->hdcp_state = IT6664_TX_HDCP_REAUTH;
	}

	return 0;
}

static int it6664_start_tx_hdcp2(struct gc555_it6664 *it6664,
				 unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct it6664_rx_state *rx_state = &it6664->runtime.rx;
	struct regmap *tx_common =
		it6664->maps[IT6664_MAP_TX_COMMON].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 common_status;
	u8 status;
	u8 reg4b;
	unsigned int attempt;
	bool hdcp1_compatible;
	int ret;

	state->hdcp_fire_version = 2;
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & BIT(0)))
		return -ENOLINK;

	ret = it6664_write_bits(tx_port, 0x42, BIT(4), BIT(4));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x18, BIT(2), BIT(2));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1a, 0x0b, 0x0b);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x41, BIT(0), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x11, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x48, 0x0f, 0);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x49, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x4a, 0x26);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x6b, 0x31, 0x01);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x6f, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x42, 0x60, 0x40);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x42, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x6f, BIT(3), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x19, 0x07, 0x07);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1b, 0xc0, 0xc0);
	if (ret)
		return ret;

	for (attempt = 0; attempt <= 300; attempt++) {
		ret = it6664_read_byte(tx_common, 0x28, &common_status);
		if (ret)
			return ret;
		msleep(1);
		if (!(common_status & 0x07))
			break;
	}
	ret = it6664_write_bits(tx_port, 0x41, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = it6664_read_tx_hdcp(it6664, port, 0x50, 1);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0x4b, &reg4b);
	if (ret)
		return ret;
	if (reg4b & BIT(0)) {
		it6664_count_hdcp_fire(state);
		msleep(150);
		ret = it6664_fire_tx_hdcp2(it6664, port);
		if (ret)
			return ret;
		state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
		return 0;
	}

	hdcp1_compatible =
		(state->pclk_khz < IT6664_TX_HDCP2_MAX_PCLK_KHZ &&
		 rx_state->colorspace != IT6664_RX_COLORSPACE_YCBCR420) ||
		state->source == IT6664_TX_SOURCE_SCALER;
	if (hdcp1_compatible && rx_state->source_hdcp_content_type_valid &&
	    rx_state->source_hdcp_content_type == 1) {
		state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;
		state->hdcp_going = false;
		return it6664_write_bits(tx_port, 0x88, GENMASK(1, 0),
					  GENMASK(1, 0));
	}
	if (!hdcp1_compatible && !state->force_hdcp1) {
		state->force_hdcp1 = true;
		state->video_state = IT6664_TX_VIDEO_STABLE;
		state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
		state->hdcp_going = false;
		return it6664_write_bits(tx_port, 0x88, GENMASK(1, 0),
					  GENMASK(1, 0));
	}

	ret = it6664_start_tx_hdcp1(it6664, port);
	state->hdcp_going = false;
	return ret;
}

static int
it6664_handle_tx_hdcp_irq(struct gc555_it6664 *it6664,
			  unsigned int port, u8 irq)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 reg42;
	u8 value;
	int ret;

	if (irq & IT6664_TX_IRQ1_HDCP_FAIL) {
		ret = it6664_read_byte(tx_port, 0x42, &reg42);
		if (ret)
			return ret;
		if (!(reg42 & BIT(4))) {
			state->hdcp2_done = false;
			state->hdcp_state = IT6664_TX_HDCP_REAUTH;
		} else if ((!(irq & IT6664_TX_IRQ1_HDCP_DONE) &&
			    state->hdcp2_rsa_busy) || state->hdcp2_done) {
			state->hdcp2_done = false;
			state->hdcp_state = IT6664_TX_HDCP_REAUTH;
			state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;
		}
		ret = it6664_write_bits(tx_port, 0x88, GENMASK(1, 0),
					  GENMASK(1, 0));
		if (ret)
			return ret;
		state->hdcp_going = false;
		state->hdcp_done = false;
		ret = it6664_monitor_tx_hdcp(it6664, port);
		if (ret)
			return ret;
		if (state->hdcp_fire_count > 30) {
			ret = it6664_write_bits(tx_port, 0xc1, BIT(0),
						 BIT(0));
			if (ret)
				return ret;
		}
	}

	if (irq & IT6664_TX_IRQ1_HDCP_DONE) {
		state->hdcp2_rsa_busy = false;
		ret = it6664_read_byte(tx_port, 0x42, &reg42);
		if (ret)
			return ret;
		state->hdcp_state = IT6664_TX_HDCP_DONE;
		state->hdcp2_done = reg42 & BIT(4);
		ret = it6664_write_bits(tx_port, 0x88, GENMASK(1, 0), 0);
		if (ret)
			return ret;
		state->hdcp_going = false;
		state->hdcp_done = true;
		state->hdcp_fire_count = 0;
		ret = it6664_monitor_tx_hdcp(it6664, port);
		if (ret)
			return ret;
	}

	if (irq & IT6664_TX_IRQ1_HDCP_STATUS) {
		ret = it6664_read_byte(tx_port, 0x0c, &value);
		if (ret)
			return ret;
	}

	if (irq & IT6664_TX_IRQ1_HDCP_START) {
		ret = it6664_write_bits(tx_port, 0x19, BIT(7), 0);
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0x88, GENMASK(1, 0), 0);
		if (ret)
			return ret;
		msleep(200);
		state->hdcp_state = IT6664_TX_HDCP_GOING;
		state->video_state = IT6664_TX_VIDEO_OK;
	}

	return 0;
}

static int it6664_reset_tx_hdcp(struct gc555_it6664 *it6664,
				unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	int ret;

	ret = it6664_write_bits(tx_port, 0x41, BIT(0), 0);
	if (ret)
		return ret;
	ret = it6664_pulse_bits(tx_port, 0x01, BIT(5));
	if (ret)
		return ret;
	if (it6664->runtime.rx.source_hdcp_raw_level == GC555_HDCP_NONE) {
		ret = it6664_write_bits(tx_port, 0x91, BIT(4), 0);
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0xc1, BIT(0), 0);
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0xc2, BIT(7), 0);
		if (ret)
			return ret;
	}
	state->hdcp2_done = false;
	state->hdcp_going = false;
	state->hdcp_done = false;
	state->hdcp2_rsa_busy = false;
	state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
	state->hdcp_fire_version = 0;

	return 0;
}

static int
it6664_handle_tx_hdcp_done(struct gc555_it6664 *it6664,
			   unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 value;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(tx_port, 0x91, BIT(4), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc1, BIT(0), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc2, BIT(7), 0);
	if (ret)
		return ret;
	state->hdcp2_rsa_busy = false;
	state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_2);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x18, &value);
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int
it6664_handle_tx_hdcp_reauth(struct gc555_it6664 *it6664,
			     unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 reg42;
	int ret;

	ret = it6664_write_bits(tx_port, 0x41, BIT(0), 0);
	if (ret)
		return ret;
	ret = it6664_pulse_bits(tx_port, 0x01, BIT(5));
	if (ret)
		return ret;
	state->hdcp2_done = false;
	state->hdcp_going = false;
	state->hdcp_done = false;
	state->hdcp2_rsa_busy = false;
	state->hdcp_wait_count = 0;
	state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
	if (state->hdcp_fire_count >= IT6664_TX_HDCP_REAUTH_LIMIT) {
		state->video_state = IT6664_TX_VIDEO_RESET;
		return 0;
	}

	ret = it6664_read_byte(tx_port, 0x42, &reg42);
	if (ret)
		return ret;
	if (reg42 & BIT(4))
		return it6664_start_tx_hdcp2(it6664, port);

	return it6664_start_tx_hdcp1(it6664, port);
}

static int
it6664_handle_tx_hdcp_going(struct gc555_it6664 *it6664,
			    unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 version;
	int ret;

	if (state->hdcp_wait_count) {
		state->hdcp_wait_count--;
		msleep(50);
		return 0;
	}
	if (state->hdcp_fire_count > 30) {
		ret = it6664_write_bits(tx_port, 0xc1, BIT(0), BIT(0));
		if (ret)
			return ret;
	}
	ret = it6664_write_bits(tx_port, 0x1a, 0x44, 0x44);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1c, 0x07, 0x07);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x19, 0x07, 0x07);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1a, 0xb0, 0xb0);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x1b, 0xff);
	if (ret)
		return ret;
	state->hdcp2_done = false;
	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0xd6, &version);
	if (ret)
		return ret;
	if (!(version & BIT(6)) || state->hdcp_fire_count > 13)
		return it6664_start_tx_hdcp1(it6664, port);
	if (!state->hdcp2_rsa_busy)
		return it6664_start_tx_hdcp2(it6664, port);

	return 0;
}

static int
it6664_handle_tx_hdcp_state(struct gc555_it6664 *it6664,
			    unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct it6664_rx_state *rx_state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	enum gc555_hdcp_level source_level = state->source_hdcp_level;
	u8 version;
	int ret;

	if (!state->hpd)
		state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
	if (rx_state->source_hdcp_raw_level != GC555_HDCP_NONE)
		source_level = rx_state->source_hdcp_raw_level;
	else if (rx_state->source_hdcp_valid &&
		 rx_state->source_hdcp_level == GC555_HDCP_NONE)
		source_level = GC555_HDCP_NONE;
	if (source_level != state->source_hdcp_level) {
		state->source_hdcp_level = source_level;
		state->hdcp_done = false;
		state->hdcp2_done = false;
		if (source_level == GC555_HDCP_NONE && state->hpd)
			state->hdcp_state = IT6664_TX_HDCP_RESET;
		else if (state->hpd)
			state->hdcp_state = IT6664_TX_HDCP_CHECK;
	}

	switch (state->hdcp_state) {
	case IT6664_TX_HDCP_RESET:
		return it6664_reset_tx_hdcp(it6664, port);
	case IT6664_TX_HDCP_GOING:
		return it6664_handle_tx_hdcp_going(it6664, port);
	case IT6664_TX_HDCP_DONE:
		return it6664_handle_tx_hdcp_done(it6664, port);
	case IT6664_TX_HDCP_REAUTH:
		return it6664_handle_tx_hdcp_reauth(it6664, port);
	case IT6664_TX_HDCP_CHECK:
		if (it6664->runtime.rx.source_hdcp_raw_level ==
		    GC555_HDCP_NONE)
			return 0;
		if (!state->hdcp_done)
			state->hdcp_state = IT6664_TX_HDCP_GOING;
		ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
		if (ret)
			return ret;
		ret = it6664_read_byte(rx, 0xd6, &version);
		if (ret)
			return ret;
		state->hdcp_wait_count = 5;
		return 0;
	default:
		return 0;
	}
}

static int it6664_read_tx_edid_chunk(struct gc555_it6664 *it6664,
				     unsigned int block,
				     unsigned int offset, u8 *buffer)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, 2);
	u8 status;
	int cleanup_ret;
	int ret;

	if (block >= IT6664_EDID_MAX_BLOCKS ||
	    offset + IT6664_TX_EDID_CHUNK_SIZE > IT6664_EDID_BLOCK_SIZE)
		return -EINVAL;

	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE,
				BIT(0), BIT(0));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(tx_port, 0x19, BIT(2), BIT(2));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(tx_port, 0x1d, BIT(3), BIT(3));
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x09);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_SLAVE, 0xa0);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_OFFSET,
			   (u8)(offset - block * IT6664_EDID_BLOCK_SIZE));
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COUNT,
			   IT6664_TX_EDID_CHUNK_SIZE);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_HEADER, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_SEGMENT, block >> 1);
	if (ret)
		goto cleanup;
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		goto cleanup;
	if (!(status & BIT(0))) {
		ret = -ENOLINK;
		goto cleanup;
	}
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x03);
	if (ret)
		goto cleanup;
	ret = it6664_wait_tx_ddc(tx_port);
	if (ret)
		goto cleanup;
	ret = regmap_bulk_read(tx_port, IT6664_TX_REG_DDC_FIFO, buffer,
			       IT6664_TX_EDID_CHUNK_SIZE);

cleanup:
	cleanup_ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE,
					BIT(0), 0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int it6664_read_tx_edid_block(struct gc555_it6664 *it6664,
				     unsigned int block, u8 *buffer)
{
	unsigned int attempt;
	int ret = -EIO;

	for (attempt = 0; attempt < IT6664_TX_EDID_RETRIES; attempt++) {
		unsigned int offset;

		memset(buffer, 0, IT6664_EDID_BLOCK_SIZE);
		for (offset = 0; offset < IT6664_EDID_BLOCK_SIZE;
		     offset += IT6664_TX_EDID_CHUNK_SIZE) {
			ret = it6664_read_tx_edid_chunk(it6664, block,
							offset,
							buffer + offset);
			if (ret)
				break;
		}
		if (ret)
			continue;
		if (it6664_edid_block_valid(buffer))
			return 0;
		ret = -EBADMSG;
	}

	return ret;
}

static void it6664_truncate_sink_edid(u8 *edid, unsigned int blocks)
{
	u8 checksum = 0;
	unsigned int i;

	edid[126] = blocks - 1;
	edid[127] = 0;
	for (i = 0; i < IT6664_EDID_BLOCK_SIZE - 1; i++)
		checksum += edid[i];
	edid[127] = -checksum;
}

static int
it6664_read_sink_edid_once(struct gc555_it6664 *it6664,
			   struct it6664_sink_edid *sink,
			   struct gc555_edid_caps *caps)
{
	unsigned int requested_blocks = 1;
	unsigned int block;
	int ret;

	memset(sink, 0, sizeof(*sink));
	memset(caps, 0, sizeof(*caps));
	for (block = 0; block < requested_blocks; block++) {
		u8 *destination = sink->data + block * IT6664_EDID_BLOCK_SIZE;
		unsigned int extensions;

		ret = it6664_read_tx_edid_block(it6664, block, destination);
		if (ret)
			return ret;
		if (!block) {
			extensions = min_t(unsigned int, destination[126],
					   IT6664_EDID_MAX_BLOCKS - 1);
			requested_blocks = extensions + 1;
		}
	}
	/* Keep the base-block count consistent with the bounded local image. */
	if (sink->data[126] >= requested_blocks)
		it6664_truncate_sink_edid(sink->data, requested_blocks);

	for (block = 1; block < requested_blocks; block++) {
		const u8 *candidate =
			sink->data + block * IT6664_EDID_BLOCK_SIZE;

		if (candidate[0] == 0x02 && candidate[1] == 0x03) {
			sink->cta_block = block;
			break;
		}
	}

	sink->length = requested_blocks * IT6664_EDID_BLOCK_SIZE;
	ret = gc555_edid_parse_caps(sink->data, sink->length, caps);
	if (ret)
		return ret;
	sink->valid = true;
	dev_dbg(it6664->gc555->dev,
		"IT6664 TX2 sink EDID blocks=%u cta=%u HDMI=%u SCDC=%u TMDS=%u 4K=%u/%u Y420=%u/%u DC=%u/%u/%u/%u\n",
		requested_blocks, sink->cta_block, caps->hdmi,
		caps->scdc, caps->max_tmds_clock_khz,
		caps->supports_4k30, caps->supports_4k60,
		caps->supports_ycbcr420_4k60,
		caps->requires_ycbcr420_4k60,
		caps->deep_color_30, caps->deep_color_36,
		caps->deep_color_ycbcr420_30,
		caps->deep_color_ycbcr420_36);

	return 0;
}

int gc555_it6664_tx_read_sink_edid(struct gc555_it6664 *it6664)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[2];
	struct regmap *tx_port = it6664_tx_port_map(it6664, 2);
	struct it6664_sink_edid sink = {};
	struct gc555_edid_caps caps = {};
	unsigned int attempt;
	int ret = -EIO;

	state->edid_attempted = true;
	state->edid_sessions++;
	for (attempt = 0; attempt < IT6664_TX_EDID_PARSE_ATTEMPTS; attempt++) {
		ret = it6664_read_sink_edid_once(it6664, &sink, &caps);
		if (!ret) {
			it6664->runtime.sink_edid = sink;
			state->sink_caps = caps;
			state->edid_parsed = true;
			state->dvi_mode = !caps.hdmi;
			return 0;
		}
		if (attempt + 1 == IT6664_TX_EDID_PARSE_ATTEMPTS)
			break;
		ret = it6664_pulse_bits(tx_port, IT6664_TX_REG_DDC_RESET, BIT(4));
		if (ret)
			break;
		ret = it6664_pulse_bits(tx_port, IT6664_TX_REG_DDC_ENABLE, BIT(0));
		if (ret)
			break;
	}

	if (state->hpd && state->edid_sessions < IT6664_TX_EDID_SESSIONS)
		state->edid_attempted = false;
	return ret;
}

static int it6664_wait_tx_scdc(struct regmap *tx_port)
{
	u8 status;
	unsigned int attempt;
	int ret;

	usleep_range(15000, 16000);
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS, &status);
	if (ret)
		return ret;
	if (status & BIT(7))
		return 0;

	usleep_range(15000, 16000);
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS, &status);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS, &status);
	if (ret)
		return ret;
	ret = it6664_read_byte(tx_port, 0x12, &status);
	if (ret)
		return ret;
	ret = it6664_pulse_bits(tx_port, 0x35, BIT(4));
	if (ret)
		return ret;
	ret = it6664_pulse_bits(tx_port, IT6664_TX_REG_DDC_ENABLE, BIT(0));
	if (ret)
		return ret;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x0f);
	if (ret)
		return ret;
	for (attempt = 0; attempt < 0xc8; attempt++) {
		ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS,
				       &status);
		if (ret)
			return ret;
		if (status & 0xb8)
			break;
		usleep_range(1000, 2000);
	}

	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x0f);
	if (ret)
		return ret;
	for (attempt = 0; attempt < 0xc8; attempt++) {
		ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_STATUS,
				       &status);
		if (ret)
			return ret;
		if (status & 0xb8)
			break;
		usleep_range(1000, 2000);
	}

	return -EAGAIN;
}

static int it6664_write_tx_scdc(struct gc555_it6664 *it6664,
				unsigned int port, u8 offset, u8 value)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 status;
	int cleanup_ret;
	int ret = 0;

	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		goto cleanup;
	if (!(status & BIT(0))) {
		ret = -ENOLINK;
		goto cleanup;
	}
	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE, BIT(0), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x09);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_SLAVE,
			   IT6664_TX_SCDC_SLAVE);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_OFFSET, offset);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COUNT, 0x01);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_HEADER,
				GENMASK(1, 0), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_FIFO, value);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x01);
	if (ret)
		goto cleanup;
	ret = it6664_wait_tx_scdc(tx_port);

cleanup:
	cleanup_ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE,
					BIT(0), 0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int it6664_read_tx_scdc(struct gc555_it6664 *it6664,
			       unsigned int port, u8 offset, u8 *value)
{
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 status;
	int cleanup_ret;
	int ret = 0;

	if (!value)
		return -EINVAL;
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		goto cleanup;
	if (!(status & BIT(0))) {
		ret = -ENOLINK;
		goto cleanup;
	}
	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE, BIT(0), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x09);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_SLAVE,
			   IT6664_TX_SCDC_SLAVE);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_OFFSET, offset);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COUNT, 0x01);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_HEADER,
				GENMASK(1, 0), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(tx_port, IT6664_TX_REG_DDC_COMMAND, 0x00);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE, BIT(0), 0);
	if (ret)
		goto cleanup;
	ret = it6664_wait_tx_scdc(tx_port);
	if (ret)
		goto cleanup;
	ret = it6664_read_byte(tx_port, IT6664_TX_REG_DDC_FIFO, value);

cleanup:
	cleanup_ret = it6664_write_bits(tx_port, IT6664_TX_REG_DDC_ENABLE,
					BIT(0), 0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int it6664_setup_tx_scdc(struct gc555_it6664 *it6664,
				unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 status;
	u8 value = 0;
	unsigned int attempt;
	bool read_ok = false;
	int first_ret = 0;
	int ret;

	state->scdc_configured = false;
	state->scdc_version = 0;
	state->scdc_config = 0;
	state->scdc_attempts = 0;
	ret = it6664_write_bits(tx_port, 0x83, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc0, 0x46, 0x46);
	if (ret)
		return ret;
	msleep(50);
	ret = it6664_write_bits(tx_port, 0x3a, GENMASK(1, 0), 0);
	if (ret)
		return ret;

	ret = it6664_write_tx_scdc(it6664, port,
				   IT6664_TX_SCDC_VERSION, 0x01);
	if (ret && !first_ret)
		first_ret = ret;
	ret = it6664_read_tx_scdc(it6664, port,
				  IT6664_TX_SCDC_VERSION, &value);
	if (ret) {
		if (!first_ret)
			first_ret = ret;
	} else {
		state->scdc_version = value;
	}

	for (attempt = 0; attempt < IT6664_TX_SCDC_RETRIES; attempt++) {
		ret = it6664_write_tx_scdc(it6664, port,
					   IT6664_TX_SCDC_TMDS_CONFIG,
					    0x03);
		if (ret && !first_ret)
			first_ret = ret;
		ret = it6664_read_tx_scdc(it6664, port,
					  IT6664_TX_SCDC_TMDS_CONFIG,
					   &value);
		read_ok = !ret;
		if (ret && !first_ret)
			first_ret = ret;
		state->scdc_attempts = attempt + 1;
		if (read_ok)
			state->scdc_config = value;

		ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
		if (ret)
			return ret;
		if (!(status & BIT(0)))
			return -ENOLINK;
		if (read_ok &&
		    (value & GENMASK(1, 0)) == GENMASK(1, 0)) {
			state->scdc_configured = true;
			return 0;
		}
	}

	return read_ok ? 0 : first_ret ?: -EAGAIN;
}

static int it6664_set_tx_protocol_mode(struct gc555_it6664 *it6664,
				       unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 rx_status;
	int ret;

	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS, &rx_status);
	if (ret)
		return ret;

	return it6664_write_bits(tx_port, 0xc0, BIT(0),
				 (rx_status & BIT(1)) && !state->dvi_mode ? BIT(0) : 0);
}

static bool
it6664_sink_supports_rx_depth(const struct it6664_tx_port_state *state,
			      const struct it6664_rx_state *rx_state)
{
	u8 depth = rx_state->color_depth & GENMASK(1, 0);
	bool supported;

	if (!depth)
		return true;
	if (state->dvi_mode)
		return false;
	if (depth == 1)
		supported = state->sink_caps.deep_color_30;
	else if (depth == 2)
		supported = state->sink_caps.deep_color_36;
	else
		return false;

	switch (rx_state->colorspace) {
	case IT6664_RX_COLORSPACE_RGB:
		return supported;
	case IT6664_RX_COLORSPACE_YCBCR444:
		return supported && state->sink_caps.deep_color_ycbcr444;
	case IT6664_RX_COLORSPACE_YCBCR420:
		return depth == 1 ?
			state->sink_caps.deep_color_ycbcr420_30 :
			state->sink_caps.deep_color_ycbcr420_36;
	default:
		return false;
	}
}

static bool it6664_direct_tx_source_ready(struct gc555_it6664 *it6664,
					  unsigned int port)
{
	const struct it6664_rx_state *rx_state = &it6664->runtime.rx;
	const struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	u8 depth = rx_state->color_depth & GENMASK(1, 0);

	if (rx_state->video_timing.high_frame_rate)
		return true;

	switch (rx_state->colorspace) {
	case IT6664_RX_COLORSPACE_RGB:
		return true;
	case IT6664_RX_COLORSPACE_YCBCR422:
		return !depth;
	case IT6664_RX_COLORSPACE_YCBCR444:
		return rx_state->csc_output_mode == IT6664_RX_COLORSPACE_RGB &&
			((!rx_state->converter_output_mode_request &&
			  !rx_state->converter_output_mode) ||
			 (rx_state->converter_output_mode_request == 3 &&
			  rx_state->converter_output_mode == 3));
	case IT6664_RX_COLORSPACE_YCBCR420:
		return port == 2 && state->sink_caps.supports_4k60 &&
			(state->sink_caps.supports_ycbcr420_4k60 ||
			 state->sink_caps.requires_ycbcr420_4k60) &&
			rx_state->converter_output_mode_request == 2 &&
			rx_state->converter_output_mode == 2 &&
			rx_state->csc_output_mode == 2;
	default:
		return false;
	}
}

static int it6664_select_direct_tx_source(struct gc555_it6664 *it6664,
					  unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	bool deep_color = it6664->runtime.rx.color_depth & GENMASK(1, 0);
	int ret;

	if (!it6664_direct_tx_source_ready(it6664, port))
		return -EOPNOTSUPP;

	if (!deep_color) {
		ret = it6664_write_bits(tx_port, 0xc1, 0xf0, 0);
		if (ret)
			return ret;
	}
	ret = it6664_write_bits(tx_port, 0xc1, BIT(2),
				deep_color ? BIT(2) : 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x0d, 0x03 << (port * 2), 0);
	if (ret)
		return ret;
	state->source = IT6664_TX_SOURCE_DIRECT;
	state->scrambling_required =
		it6664->runtime.rx.video_timing.high_frame_rate ?
		state->pclk_khz > 300000 : state->pclk_khz >= 340000;
	ret = it6664_write_bits(tx_port, 0xad, 0x81, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xa0, 0x30, 0);
	if (ret)
		return ret;

	return 0;
}

static int it6664_select_converter_tx_source(struct gc555_it6664 *it6664,
					     unsigned int port)
{
	struct it6664_rx_state *rx_state = &it6664->runtime.rx;
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 depth = rx_state->color_depth & GENMASK(1, 0);
	int ret;

	if (port != 2 || depth ||
	    rx_state->colorspace != IT6664_RX_COLORSPACE_YCBCR420 ||
	    !state->sink_caps.supports_4k60 ||
	    state->sink_caps.supports_ycbcr420_4k60 ||
	    state->sink_caps.requires_ycbcr420_4k60 ||
	    rx_state->converter_output_mode_request != 2 ||
	    rx_state->converter_output_mode != 2 ||
	    rx_state->csc_output_mode != 2)
		return -EOPNOTSUPP;

	ret = it6664_write_bits(tx_port, 0xc1, GENMASK(7, 4), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc1, BIT(2), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x0d, 0x03 << (port * 2),
				IT6664_TX_SOURCE_CONVERTER << (port * 2));
	if (ret)
		return ret;
	state->source = IT6664_TX_SOURCE_CONVERTER;
	state->scrambling_required = true;
	ret = it6664_write_bits(tx_port, 0xad, 0x81, 0x81);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xae, GENMASK(2, 0),
				rx_state->converter_output_mode);
	if (ret)
		return ret;

	return it6664_write_bits(tx_port, 0xa0, 0x30, 0x10);
}

static int it6664_select_csc_tx_source(struct gc555_it6664 *it6664,
				       unsigned int port)
{
	struct it6664_rx_state *rx_state = &it6664->runtime.rx;
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	struct regmap *active_tx;
	u8 deep_color = rx_state->color_depth & GENMASK(1, 0);
	u8 converter_input_depth;
	unsigned int active_port;
	int ret;

	if (deep_color == 1)
		converter_input_depth = BIT(6);
	else if (deep_color == 2)
		converter_input_depth = BIT(7);
	else
		return -EOPNOTSUPP;

	if (rx_state->colorspace == IT6664_RX_COLORSPACE_RGB) {
		ret = it6664_write_bits(sw, 0x6b, 0x42, 0);
		if (ret)
			return ret;
		rx_state->csc_output_mode = IT6664_RX_COLORSPACE_RGB;
		for (active_port = 0; active_port < IT6664_TX_PORT_COUNT;
		     active_port++) {
			if (it6664->runtime.tx[active_port].source !=
			    IT6664_TX_SOURCE_CSC)
				continue;
			active_tx = it6664_tx_port_map(it6664, active_port);
			ret = it6664_write_bits(active_tx, 0xae, GENMASK(2, 0),
						IT6664_RX_COLORSPACE_RGB);
			if (ret)
				return ret;
		}
	} else if ((rx_state->colorspace != IT6664_RX_COLORSPACE_YCBCR422 &&
		    rx_state->colorspace != IT6664_RX_COLORSPACE_YCBCR444) ||
		   rx_state->csc_output_mode != IT6664_RX_COLORSPACE_RGB) {
		return -EOPNOTSUPP;
	}

	ret = it6664_write_bits(sw, 0x67, BIT(1), BIT(1));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xaf, GENMASK(7, 6),
				converter_input_depth);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc1, GENMASK(7, 4), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc1, BIT(2), BIT(2));
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x0d, 0x03 << (port * 2),
				IT6664_TX_SOURCE_CSC << (port * 2));
	if (ret)
		return ret;
	state->source = IT6664_TX_SOURCE_CSC;
	state->scrambling_required = false;
	ret = it6664_write_bits(tx_port, 0xad, 0x85, 0x85);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xae, GENMASK(2, 0),
				rx_state->csc_output_mode);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xae, GENMASK(7, 6),
				rx_state->csc_output_quantization << 6);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xa0, 0x30, 0);
	if (ret)
		return ret;

	return 0;
}

static int it6664_read_rx_bank2_byte(struct gc555_it6664 *it6664,
				     unsigned int reg, u8 *value)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_2);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, reg, value);
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static u8 it6664_tx_scaler_mode_for_timing(u8 timing)
{
	if (timing < 0x5d || timing > 0x66)
		return 0x10;

	return it6664_tx_scaler_mode[timing - 0x5d];
}

static int it6664_select_scaler_tx_source(struct gc555_it6664 *it6664,
					  unsigned int port)
{
	struct it6664_rx_state *rx_state = &it6664->runtime.rx;
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	struct regmap *active_tx;
	u8 deep_color = rx_state->color_depth & GENMASK(1, 0);
	u8 scaler_input_depth = 0;
	u8 rx17;
	u8 rx18;
	u8 scaler_mode;
	unsigned int active_port;
	int ret;

	if (rx_state->colorspace != IT6664_RX_COLORSPACE_YCBCR420 ||
	    rx_state->converter_output_mode_request != 2 ||
	    rx_state->converter_output_mode != 2 ||
	    rx_state->csc_output_mode != 2)
		return -EOPNOTSUPP;
	if (port == 2 &&
	    (deep_color || state->dvi_mode ||
	     state->sink_caps.supports_4k60 ||
	     !state->sink_caps.supports_1080p))
		return -EOPNOTSUPP;
	if (port != 1 && port != 2)
		return -EOPNOTSUPP;
	if (deep_color == 1)
		scaler_input_depth = BIT(6);
	else if (deep_color == 2)
		scaler_input_depth = BIT(7);
	else if (deep_color)
		return -EOPNOTSUPP;

	ret = it6664_write_bits(sw, 0x67, BIT(3),
				scaler_input_depth ? BIT(3) : 0);
	if (ret)
		return ret;
	for (active_port = 0; active_port < IT6664_TX_PORT_COUNT;
	     active_port++) {
		if (it6664->runtime.tx[active_port].source !=
		    IT6664_TX_SOURCE_SCALER)
			continue;
		active_tx = it6664_tx_port_map(it6664, active_port);
		ret = it6664_write_bits(active_tx, 0xaf, GENMASK(7, 6),
					scaler_input_depth);
		if (ret)
			return ret;
	}
	ret = it6664_write_bits(tx_port, 0xaf, GENMASK(7, 6),
				scaler_input_depth);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc1, BIT(2),
				deep_color ? BIT(2) : 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x0d, 0x03 << (port * 2),
				IT6664_TX_SOURCE_SCALER << (port * 2));
	if (ret)
		return ret;
	state->source = IT6664_TX_SOURCE_SCALER;
	state->scrambling_required = false;
	ret = it6664_read_rx_bank2_byte(it6664, 0x18, &rx18);
	if (ret)
		return ret;
	scaler_mode = it6664_tx_scaler_mode_for_timing(rx18);
	ret = it6664_write_bits(tx_port, 0xad, 0xa1, 0xa1);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xae, GENMASK(2, 0),
				rx_state->converter_output_mode);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0xb0, scaler_mode);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xa0, 0x30, 0x20);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xa3, BIT(7), BIT(7));
	if (ret)
		return ret;
	ret = it6664_read_rx_bank2_byte(it6664, 0x17, &rx17);
	if (ret)
		return ret;
	if ((rx17 & 0x60) == 0x60) {
		ret = it6664_write_bits(tx_port, 0xad, BIT(1), BIT(1));
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0xae, 0x32, 0x02);
		if (ret)
			return ret;
	}

	return 0;
}

static int it6664_select_tx_source(struct gc555_it6664 *it6664,
				   unsigned int port)
{
	struct it6664_rx_state *rx_state = &it6664->runtime.rx;
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	u8 depth;
	int ret;

	if (!rx_state->video_timing.valid) {
		ret = gc555_it6664_rx_refresh_video_timing(it6664);
		if (ret)
			return ret;
	}
	depth = rx_state->color_depth & GENMASK(1, 0);
	ret = it6664_set_tx_protocol_mode(it6664, port);
	if (ret)
		return ret;
	/* High-rate paths must bypass converter and scaler bandwidth limits. */
	if (rx_state->video_timing.high_frame_rate) {
		if (!it6664_sink_supports_rx_depth(state, rx_state))
			return -EOPNOTSUPP;
		return it6664_select_direct_tx_source(it6664, port);
	}

	if (port == 2 &&
	    rx_state->colorspace == IT6664_RX_COLORSPACE_YCBCR420) {
		if (it6664_direct_tx_source_ready(it6664, port) &&
		    it6664_sink_supports_rx_depth(state, rx_state))
			return it6664_select_direct_tx_source(it6664, port);
		ret = it6664_select_converter_tx_source(it6664, port);
		if (ret != -EOPNOTSUPP)
			return ret;
		return it6664_select_scaler_tx_source(it6664, port);
	}
	if (port == 2 && depth &&
	    it6664_sink_supports_rx_depth(state, rx_state))
		return it6664_select_direct_tx_source(it6664, port);
	if (rx_state->colorspace == IT6664_RX_COLORSPACE_YCBCR420)
		return it6664_select_scaler_tx_source(it6664, port);
	if (!depth)
		return it6664_select_direct_tx_source(it6664, port);

	return it6664_select_csc_tx_source(it6664, port);
}

static int it6664_prepare_tx_output_afe(struct gc555_it6664 *it6664,
					unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	int ret;

	if (state->afe_configured)
		return 0;
	ret = it6664_measure_tx_pclk(it6664, port);
	if (ret)
		return ret;
	ret = it6664_configure_tx_afe(it6664, port);
	if (ret)
		return ret;
	msleep(50);
	ret = it6664_reset_tx_video_clock(tx_port);
	if (ret)
		return ret;

	state->afe_configured = true;
	return 0;
}

static int it6664_clear_tx_scdc(struct gc555_it6664 *it6664,
				unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	u8 config = 0;
	unsigned int attempt;
	int first_ret = 0;
	int ret;

	if (state->sink_caps.scdc && state->hpd) {
		for (attempt = 0; attempt < IT6664_TX_SCDC_RETRIES; attempt++) {
			ret = it6664_write_tx_scdc(it6664, port,
						   IT6664_TX_SCDC_TMDS_CONFIG,
						   0x00);
			if (ret && !first_ret)
				first_ret = ret;
			ret = it6664_read_tx_scdc(it6664, port,
						  IT6664_TX_SCDC_TMDS_CONFIG,
						  &config);
			if (ret && !first_ret)
				first_ret = ret;
			state->scdc_attempts = attempt + 1;
			if (!ret && !(config & GENMASK(1, 0)))
				break;
		}
		if (ret)
			return first_ret ?: ret;
		if (config & GENMASK(1, 0))
			return first_ret ?: -EAGAIN;
	}

	ret = it6664_write_bits(tx_port, 0xc0, BIT(1), 0);
	if (ret)
		return ret;
	state->scdc_configured = false;
	state->scdc_config = config;

	return 0;
}

static int it6664_enable_tx_output(struct gc555_it6664 *it6664,
				   unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	bool scrambling_required;
	u8 status;
	int first_scdc_ret = 0;
	int second_scdc_ret = 0;
	int ret;

	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & BIT(0))) {
		state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;
		return 0;
	}
	if (port == 2 && !state->edid_parsed)
		return 0;
	ret = it6664_prepare_tx_output_afe(it6664, port);
	if (ret)
		return ret;
	ret = it6664_select_tx_source(it6664, port);
	if (ret == -EOPNOTSUPP) {
		dev_dbg_ratelimited(it6664->gc555->dev,
				    "IT6664 TX%u output deferred for RX format %u/%u route %u/%u/%u\n",
				    port, it6664->runtime.rx.colorspace,
				    it6664->runtime.rx.color_depth,
				    it6664->runtime.rx.converter_output_mode_request,
				    it6664->runtime.rx.converter_output_mode,
				    it6664->runtime.rx.csc_output_mode);
		return 0;
	}
	if (ret)
		return ret;
	state->video_stable = true;
	state->tmds_stable = true;
	scrambling_required = state->scrambling_required;
	if (scrambling_required &&
	    (!state->sink_caps.scdc ||
	     state->sink_caps.max_tmds_clock_khz < state->pclk_khz)) {
		ret = it6664_clear_tx_scdc(it6664, port);
		if (ret)
			return ret;
		dev_dbg_ratelimited(it6664->gc555->dev,
				    "IT6664 TX%u output deferred: sink cannot carry %u kHz TMDS\n",
				    port, state->pclk_khz);
		return 0;
	}

	ret = it6664_write_bits(tx_port, 0x18, 0x0c, 0x0c);
	if (ret)
		return ret;
	ret = regmap_write(tx_port, 0x85, 0x19);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x1a, 0x0b, 0x0b);
	if (ret)
		return ret;

	if (scrambling_required) {
		first_scdc_ret = it6664_setup_tx_scdc(it6664, port);
		msleep(100);
	} else {
		ret = it6664_clear_tx_scdc(it6664, port);
		if (ret)
			return ret;
	}
	ret = it6664_write_bits(tx_port, 0xc1, BIT(3), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc1, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc2, BIT(7), BIT(7));
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc3, 0x30, 0x30);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0x88, GENMASK(1, 0), 0);
	if (ret)
		return ret;
	msleep(100);
	if (scrambling_required)
		second_scdc_ret = it6664_setup_tx_scdc(it6664, port);
	else
		second_scdc_ret = it6664_clear_tx_scdc(it6664, port);
	if (!scrambling_required && second_scdc_ret)
		return second_scdc_ret;

	ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & BIT(3))) {
		ret = it6664_reset_tx_video_clock(tx_port);
		if (ret)
			return ret;
		ret = it6664_write_bits(tx_port, 0xc1, 0xf0, 0x80);
		if (ret)
			return ret;
		msleep(50);
		ret = it6664_read_byte(tx_port, IT6664_TX_REG_STATUS, &status);
		if (ret)
			return ret;
	}

	if ((status & GENMASK(3, 0)) == GENMASK(3, 0)) {
		if (scrambling_required && second_scdc_ret) {
			state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
			state->video_state = IT6664_TX_VIDEO_STABLE;
		} else {
			state->hdcp_state = IT6664_TX_HDCP_CHECK;
			state->video_state = IT6664_TX_VIDEO_OK;
		}
	} else if ((status & GENMASK(1, 0)) != GENMASK(1, 0)) {
		state->hdcp_state = IT6664_TX_HDCP_WAIT_IRQ;
		state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;
	}
	ret = regmap_write(tx_port, 0x12, 0xff);
	if (ret)
		return ret;

	dev_dbg(it6664->gc555->dev,
		"IT6664 TX%u output status=%02x pclk=%u source=%u scdc=%d/%d %02x/%u video=%u\n",
		port, status, state->pclk_khz, (unsigned int)state->source,
		first_scdc_ret, second_scdc_ret, state->scdc_config,
		state->scdc_attempts, state->video_state);
	return 0;
}

static int it6664_reset_tx_output_state(struct gc555_it6664 *it6664,
					unsigned int port, bool stable_off)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	int ret;

	if (stable_off) {
		ret = it6664_write_bits(tx_port, 0x1a, 0x4f, 0);
		if (ret)
			return ret;
	}
	ret = it6664_write_bits(tx_port, 0x88, GENMASK(1, 0),
				GENMASK(1, 0));
	if (ret)
		return ret;
	state->video_stable = false;
	state->tmds_stable = false;
	ret = it6664_reset_tx_video_clock(tx_port);
	if (ret)
		return ret;
	if (!stable_off) {
		ret = it6664_write_bits(tx_port, 0x12, BIT(2), BIT(2));
		if (ret)
			return ret;
		msleep(100);
	}
	state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;
	state->hdcp_state = IT6664_TX_HDCP_RESET;

	return 0;
}

static int it6664_complete_tx_output(struct gc555_it6664 *it6664,
				     unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];
	struct regmap *tx_port = it6664_tx_port_map(it6664, port);
	int ret;

	ret = it6664_write_bits(tx_port, 0x91, BIT(4), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_port, 0xc1, BIT(0), 0);
	if (ret)
		return ret;
	state->video_state = IT6664_TX_VIDEO_WAIT_IRQ;

	return 0;
}

static int it6664_handle_tx_video_state(struct gc555_it6664 *it6664,
					unsigned int port)
{
	struct it6664_tx_port_state *state = &it6664->runtime.tx[port];

	switch (state->video_state) {
	case IT6664_TX_VIDEO_STABLE:
		return it6664_enable_tx_output(it6664, port);
	case IT6664_TX_VIDEO_STABLE_OFF:
		return it6664_reset_tx_output_state(it6664, port, true);
	case IT6664_TX_VIDEO_RESET:
		return it6664_reset_tx_output_state(it6664, port, false);
	case IT6664_TX_VIDEO_OK:
		return it6664_complete_tx_output(it6664, port);
	default:
		return 0;
	}
}

int gc555_it6664_tx_poll(struct gc555_it6664 *it6664)
{
	struct it6664_tx_port_state *hdcp_state;
	unsigned int port;
	int ret;

	for (port = 1; port <= 2; port++) {
		struct it6664_tx_irq_pending *pending =
			&it6664->runtime.tx_irq[port];
		struct it6664_tx_irq *snapshot = &pending->snapshot;
		unsigned int i;
		bool active = false;

		if (!pending->valid) {
			ret = it6664_read_tx_irq(it6664, port, snapshot);
			if (ret)
				return ret;
			for (i = 0; i < IT6664_TX_IRQ_COUNT; i++)
				active |= pending->snapshot.irq[i] != 0;
			if (!active)
				continue;
			pending->consumed = 0;
			pending->acknowledged = 0;
			pending->valid = true;
		}

		ret = it6664_handle_tx_irq(it6664, port, pending);
		if (ret)
			return ret;
		memset(pending, 0, sizeof(*pending));
	}
	for (port = 1; port <= 2; port++) {
		ret = it6664_handle_tx_video_state(it6664, port);
		if (ret)
			return ret;
	}

	ret = it6664_handle_tx_hdcp_state(it6664, 2);
	if (ret)
		return ret;

	hdcp_state = &it6664->runtime.tx[2];
	if (hdcp_state->hdcp_status_count < 0x101) {
		hdcp_state->hdcp_status_count++;
		return 0;
	}
	hdcp_state->hdcp_status_count = 0;
	if (!hdcp_state->hdcp_done)
		return 0;

	return it6664_monitor_tx_hdcp(it6664, 2);
}

int gc555_it6664_tx_read_mode(struct gc555_it6664 *it6664,
			      unsigned int port, u8 *status, u8 *mode)
{
	struct regmap *tx_port;
	unsigned int value;
	int ret;

	if (!it6664 || !status || !mode || port >= IT6664_TX_PORT_COUNT)
		return -EINVAL;

	tx_port = it6664_tx_port_map(it6664, port);
	ret = regmap_read(tx_port, 0x03, &value);
	if (ret)
		return ret;
	*status = value;
	ret = regmap_read(tx_port, 0xc0, &value);
	if (ret)
		return ret;
	*mode = value;

	return 0;
}

int gc555_it6664_tx_init(struct gc555_it6664 *it6664)
{
	struct gc555_edid_caps *caps = &it6664->runtime.tx[1].sink_caps;
	const u8 *edid;
	size_t edid_size;
	int ret;

	ret = it6664_initialize_tx_reset_path(it6664);
	if (ret)
		return ret;
	ret = gc555_edid_get(&edid, &edid_size);
	if (ret)
		return ret;
	ret = gc555_edid_parse_caps(edid, edid_size, caps);
	if (ret)
		return ret;

	return it6664_configure_tx_switch(it6664);
}

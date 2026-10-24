// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "gc573.h"
#include "gc573-it6664.h"

#include <linux/moduleparam.h>

/*
 * GC573: merging the passthrough monitor's EDID strips every mode the
 * monitor lacks (e.g. 1440p120), so consoles refuse those modes even
 * though the capture path handles them. Default to the capture EDID.
 */
static bool edid_merge;
module_param(edid_merge, bool, 0644);
MODULE_PARM_DESC(edid_merge,
		 "Merge passthrough monitor EDID into the advertised EDID (default: off)");

#define IT6664_SWITCH_I2C_ADDRESS	0x2c
#define IT6664_RX_PORT0_I2C_ADDRESS	0x38
#define IT6664_RX_EDID_RAM_I2C_ADDRESS	0x6c
#define IT6664_TX_COMMON_I2C_ADDRESS	0x4b
#define IT6664_TX_PORT0_I2C_ADDRESS	0x34
#define IT6664_TX_PORT1_I2C_ADDRESS	0x35
#define IT6664_TX_PORT2_I2C_ADDRESS	0x36
#define IT6664_TX_PORT3_I2C_ADDRESS	0x37
#define IT6664_SWITCH_REG_IRQ_STATUS	0x05
#define IT6664_SWITCH_REG_IRQ6		0x06
#define IT6664_SWITCH_REG_IRQ7		0x07
#define IT6664_SWITCH_REG_08		0x08
#define IT6664_SWITCH_REG_RESET		0x0a
#define IT6664_SWITCH_REG_0C		0x0c
#define IT6664_SWITCH_REG_0E		0x0e
#define IT6664_SWITCH_REG_BANK		0x0f
#define IT6664_SWITCH_REG_10		0x10
#define IT6664_SWITCH_REG_15		0x15
#define IT6664_SWITCH_REG_HDCP_IRQ_MASK	0x19
#define IT6664_SWITCH_REG_HDCP_CONTROL	0x1a
#define IT6664_SWITCH_REG_HDCP_CONFIG	0x1c
#define IT6664_SWITCH_REG_RCLK_INTEGER	0x1e
#define IT6664_SWITCH_REG_RCLK_FRACTION	0x1f
#define IT6664_SWITCH_REG_CSC_CONTROL	0x6b
#define IT6664_SWITCH_REG_CSC_MODE	0x6c
#define IT6664_SWITCH_REG_CSC_LOW_OFFSET	0x70
#define IT6664_SWITCH_REG_CSC_LOW_MATRIX	0x73
#define IT6664_SWITCH_REG_CSC_HIGH_OFFSET	0x88
#define IT6664_SWITCH_REG_CSC_HIGH_MATRIX	0x93
#define IT6664_SWITCH_REG_SIPROM_ADDR_HI	0x50
#define IT6664_SWITCH_REG_SIPROM_ADDR_LO	0x51
#define IT6664_SWITCH_REG_SIPROM_COMMAND	0x54
#define IT6664_SWITCH_REG_SIPROM_DATA_LO	0x61
#define IT6664_SWITCH_REG_SIPROM_DATA_HI	0x62
#define IT6664_SWITCH_REG_73		0x73
#define IT6664_SWITCH_REG_RX_PORT0_MAP	0xf0
#define IT6664_SWITCH_REG_TX_COMMON_MAP	0xf1
#define IT6664_SWITCH_REG_UNLOCK		0xff
#define IT6664_REG_ID_BASE		0x00
#define IT6664_RX_REG_CAOF_STATUS	0x08
#define IT6664_RX_REG_BANK		0x0f
#define IT6664_RX_REG_STATUS		0x13
#define IT6664_RX_REG_STATUS_14		0x14
#define IT6664_RX_REG_PACKET_SELECT	0x77
#define IT6664_RX_REG_EDID_ENABLE	0x34
#define IT6664_RX_REG_EDID_ADDRESS_HI	0xc6
#define IT6664_RX_REG_EDID_ADDRESS_LO	0xc7
#define IT6664_RX_REG_EDID_PORT		0xc8
#define IT6664_RX_REG_EDID_CHECKSUM0	0xc9
#define IT6664_RX_REG_EDID_CHECKSUM1	0xca
#define IT6664_RX_REG_HDCP_STATUS	0xd0
#define IT6664_RX_REG_HDCP_VERSION	0xd6
#define IT6664_RX_BANK_MASK		GENMASK(1, 0)
#define IT6664_RX_BANK_0		0x00
#define IT6664_RX_BANK_2		0x02
#define IT6664_RX_BANK_3		0x03
#define IT6664_RX_CAOF_POLL_COUNT	0x1f
#define IT6664_TX_COMMON_REG_TIMER_LO	0x11
#define IT6664_TX_COMMON_REG_TIMER_HI	0x12
#define IT6664_TX_COMMON_REG_TIMER_TOP	0x13
#define IT6664_TX_COMMON_REG_50		0x50
#define IT6664_HEAVY_INIT_DELAY_MS	(70 + 20 + 30)
#define IT6664_RCLK_MIN_KHZ		10000
#define IT6664_RCLK_MAX_KHZ		34000
#define IT6664_RCLK_DEFAULT_KHZ		22000
#define IT6664_RUNTIME_INTERVAL_MS	20
#define IT6664_EDID_PUBLISH_SESSIONS	3
#define IT6664_HDCP_DEBOUNCE_SAMPLES	11
#define IT6664_SWITCH_IRQ_RX		BIT(4)
#define IT6664_SWITCH_IRQ_SHARED		GENMASK(6, 5)
#define IT6664_RX_IRQ_SOURCE_CHANGE	BIT(0)
#define IT6664_RX_IRQ_SIGNAL_START	(BIT(6) | BIT(2) | BIT(1))
#define IT6664_RX_IRQ05_NOOP		(BIT(6) | BIT(4))
#define IT6664_RX_IRQ05_SUPPORTED	(IT6664_RX_IRQ_SOURCE_CHANGE | \
					 IT6664_RX_IRQ_SIGNAL_START | BIT(4))
#define IT6664_RX_IRQ06_ACK_ONLY		(BIT(7) | BIT(6))
#define IT6664_RX_IRQ07_EQ_RESULT	(BIT(7) | BIT(6) | BIT(4))
#define IT6664_RX_IRQ07_BANK2_STATUS	BIT(2)
#define IT6664_RX_IRQ07_SUPPORTED	(IT6664_RX_IRQ07_EQ_RESULT | \
					 IT6664_RX_IRQ07_BANK2_STATUS)
#define IT6664_RX_IRQ08_ACK_ONLY		(BIT(6) | BIT(2))
#define IT6664_RX_IRQ10_SCDT_CHANGE	BIT(1)
#define IT6664_RX_IRQ10_CLOCK_CHANGE	BIT(2)
#define IT6664_RX_IRQ10_ACTIONS		(IT6664_RX_IRQ10_SCDT_CHANGE | \
					 IT6664_RX_IRQ10_CLOCK_CHANGE)
#define IT6664_RX_IRQ10_SUPPORTED	GENMASK(3, 0)
#define IT6664_RX_IRQ10_NOOP		(BIT(3) | BIT(0))
#define IT6664_RX_IRQ11_COLOR_DEPTH	BIT(3)
#define IT6664_RX_IRQ11_BANK2_STATUS	BIT(2)
#define IT6664_RX_IRQ11_ACK_ONLY		(BIT(6) | BIT(5) | BIT(4))
#define IT6664_RX_IRQ11_SUPPORTED	(IT6664_RX_IRQ11_ACK_ONLY | \
					 IT6664_RX_IRQ11_COLOR_DEPTH | \
					 IT6664_RX_IRQ11_BANK2_STATUS)
#define IT6664_RX_IRQ12_SUPPORTED	(BIT(7) | BIT(5) | BIT(0))
#define IT6664_RX_PACKET_DRM		0x87
#define IT6664_RX_EQ_NOT_READY_LIMIT	16
#define IT6664_RX_EQ_RECOVERY_LIMIT	3
#define IT6664_RX_EQ_MANUAL_CANDIDATE_COUNT	7
#define IT6664_RX_VIDEO_SAMPLE_COUNT	100
#define IT6664_RX_VIDEO_COUNTER_SCALE	0xc800

struct it6664_init_observation {
	u8 regs02_03[2];
	u8 reg03;
	u8 reg15;
};

struct it6664_caof_result {
	u8 initial_status;
	u8 status;
	u8 reg5a;
	u8 reg59[2];
	unsigned int polls;
	bool timed_out;
};

struct it6664_rx_snapshot {
	u8 reg08;
	u8 reg13;
	u8 reg14;
	u8 reg19;
};

struct it6664_rx_irq {
	u8 reg05;
	u8 reg06;
	u8 reg07;
	u8 reg08;
	u8 reg09;
	u8 reg10;
	u8 reg11;
	u8 reg12;
	u8 reg13;
	u8 reg14;
	u8 reg15;
	u8 reg19;
	u8 reg1a;
	u8 reg1b;
	u8 reg1d;
};

struct it6664_reg_update {
	u8 reg;
	u8 mask;
	u8 value;
};

static const struct i2c_board_info it6664_map_board_info[IT6664_MAP_COUNT] = {
	[IT6664_MAP_SWITCH] = {
		I2C_BOARD_INFO("gc555-it6664-switch",
			       IT6664_SWITCH_I2C_ADDRESS),
	},
	[IT6664_MAP_RX_PORT0] = {
		I2C_BOARD_INFO("gc555-it6664-rxp0",
			       IT6664_RX_PORT0_I2C_ADDRESS),
	},
	[IT6664_MAP_RX_EDID_RAM] = {
		I2C_BOARD_INFO("gc555-it6664-edid",
			       IT6664_RX_EDID_RAM_I2C_ADDRESS),
	},
	[IT6664_MAP_TX_COMMON] = {
		I2C_BOARD_INFO("gc555-it6664-txcom",
			       IT6664_TX_COMMON_I2C_ADDRESS),
	},
	[IT6664_MAP_TX_PORT0] = {
		I2C_BOARD_INFO("gc555-it6664-txp0",
			       IT6664_TX_PORT0_I2C_ADDRESS),
	},
	[IT6664_MAP_TX_PORT1] = {
		I2C_BOARD_INFO("gc555-it6664-txp1",
			       IT6664_TX_PORT1_I2C_ADDRESS),
	},
	[IT6664_MAP_TX_PORT2] = {
		I2C_BOARD_INFO("gc555-it6664-txp2",
			       IT6664_TX_PORT2_I2C_ADDRESS),
	},
	[IT6664_MAP_TX_PORT3] = {
		I2C_BOARD_INFO("gc555-it6664-txp3",
			       IT6664_TX_PORT3_I2C_ADDRESS),
	},
};

static const struct regmap_config it6664_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static const struct reg_sequence it6664_siprom_setup[] = {
	{ IT6664_SWITCH_REG_UNLOCK, 0xc3 },
	{ IT6664_SWITCH_REG_UNLOCK, 0xa5 },
	{ IT6664_SWITCH_REG_BANK, 0x00 },
	{ 0x5f, 0x04 },
	{ 0x5f, 0x05 },
	{ 0x58, 0x12 },
	{ 0x58, 0x02 },
	{ 0x57, 0x01 },
	{ IT6664_SWITCH_REG_SIPROM_ADDR_HI, 0x00 },
	{ IT6664_SWITCH_REG_SIPROM_ADDR_LO, 0x00 },
	{ IT6664_SWITCH_REG_SIPROM_COMMAND, 0x04 },
};

static const struct reg_sequence it6664_siprom_cleanup[] = {
	{ 0x5f, 0x00 },
	{ IT6664_SWITCH_REG_BANK, 0x00 },
	{ IT6664_SWITCH_REG_UNLOCK, 0xff },
};

static const struct reg_sequence it6664_post_rclk_sequence[] = {
	{ 0x2c, 0x69 },
	{ 0x2d, 0x6b },
	{ 0x2e, 0x6d },
	{ 0x2f, 0x6f },
};

static const struct reg_sequence it6664_rx_reset_sequence[] = {
	{ 0x22, 0x08 },
	{ 0x23, 0x01 },
	{ 0x22, 0x17 },
	{ 0x24, 0xf8 },
	{ 0x23, 0xa0 },
	{ 0x22, 0x00 },
	{ 0x24, 0x00 },
};

static const struct it6664_reg_update it6664_caof_prefix_updates[] = {
	{ IT6664_RX_REG_BANK, IT6664_RX_BANK_MASK, IT6664_RX_BANK_0 },
	{ 0x29, BIT(0), BIT(0) },
	{ 0x2a, BIT(6) | BIT(0), BIT(6) | BIT(0) },
	{ IT6664_RX_REG_BANK, IT6664_RX_BANK_MASK, IT6664_RX_BANK_3 },
	{ 0x3a, BIT(7), 0 },
	{ 0x3b, GENMASK(7, 6), 0 },
	{ 0xa0, BIT(7), BIT(7) },
	{ 0xa1, BIT(7), BIT(7) },
	{ 0xa2, BIT(7), BIT(7) },
	{ 0xa7, BIT(4), BIT(4) },
	{ 0x48, BIT(7), BIT(7) },
	{ IT6664_RX_REG_BANK, IT6664_RX_BANK_MASK, IT6664_RX_BANK_0 },
	{ 0x2a, BIT(6), 0 },
	{ 0x24, BIT(2), BIT(2) },
};

static const struct reg_sequence it6664_caof_zero_sequence[] = {
	{ 0x25, 0x00 },
	{ 0x26, 0x00 },
	{ 0x27, 0x00 },
	{ 0x28, 0x00 },
};

static const struct it6664_reg_update it6664_caof_prefix_tail[] = {
	{ 0x3c, BIT(4), 0 },
	{ IT6664_RX_REG_BANK, IT6664_RX_BANK_MASK, IT6664_RX_BANK_3 },
	{ 0x3a, BIT(7), BIT(7) },
	{ IT6664_RX_REG_BANK, IT6664_RX_BANK_MASK, IT6664_RX_BANK_0 },
};

static const struct it6664_reg_update it6664_caof_post_updates[] = {
	{ 0x3a, BIT(7), 0 },
	{ 0xa0, BIT(7), 0 },
	{ 0xa1, BIT(7), 0 },
	{ 0xa2, BIT(7), 0 },
	{ IT6664_RX_REG_BANK, IT6664_RX_BANK_MASK, IT6664_RX_BANK_0 },
	{ IT6664_RX_REG_CAOF_STATUS, GENMASK(5, 4), GENMASK(5, 4) },
	{ 0x29, BIT(0), 0 },
	{ 0x24, BIT(2), 0 },
	{ 0x3c, BIT(4), BIT(4) },
	{ 0xce, BIT(5), 0 },
};

static const u8 it6664_csc_zero_offset[3] = { 0x00, 0x00, 0x00 };

static const u8 it6664_csc_ycbcr_to_rgb[2][18] = {
	{ 0x00, 0x08, 0x6b, 0x3a, 0x50, 0x3d, 0x00, 0x08, 0xf5,
	  0x0a, 0x02, 0x00, 0x00, 0x08, 0xfd, 0x3f, 0xda, 0x0d },
	{ 0x00, 0x08, 0x55, 0x3c, 0x88, 0x3e, 0x00, 0x08, 0x51,
	  0x0c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x84, 0x0e },
};

static const u8 it6664_csc_rgb_to_ycbcr[2][18] = {
	{ 0xb2, 0x04, 0x65, 0x02, 0xe9, 0x00, 0x93, 0x3c, 0x18,
	  0x04, 0x55, 0x3f, 0x49, 0x3d, 0x9f, 0x3e, 0x18, 0x04 },
	{ 0xb8, 0x05, 0xb4, 0x01, 0x94, 0x00, 0x4a, 0x3c, 0x17,
	  0x04, 0x9f, 0x3f, 0xd9, 0x3c, 0x10, 0x3f, 0x17, 0x04 },
};

static bool it6664_identity_valid(const u8 *identity)
{
	return identity[0] == 0x54 && identity[1] == 0x49 &&
	       (identity[2] == 0x63 || identity[2] == 0x64) &&
	       identity[3] == 0x66;
}

static int it6664_map_init(struct gc555_it6664 *it6664,
			   struct i2c_adapter *adapter,
			   enum it6664_map_id id)
{
	struct it6664_map *map = &it6664->maps[id];

	map->client = i2c_new_client_device(adapter,
					    &it6664_map_board_info[id]);
	if (IS_ERR(map->client))
		return PTR_ERR(map->client);

	map->regmap = regmap_init_i2c(map->client, &it6664_regmap_config);
	if (IS_ERR(map->regmap))
		return PTR_ERR(map->regmap);

	return 0;
}

static int
it6664_read_init_observation(struct gc555_it6664 *it6664,
			     struct it6664_init_observation *observation)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	unsigned int value;
	int ret;

	ret = regmap_bulk_read(sw, 0x02, observation->regs02_03,
			       sizeof(observation->regs02_03));
	if (ret)
		return ret;
	ret = regmap_read(sw, 0x03, &value);
	if (ret)
		return ret;
	observation->reg03 = value;

	ret = regmap_read(sw, IT6664_SWITCH_REG_15, &value);
	if (ret)
		return ret;
	observation->reg15 = value;

	return 0;
}

static int it6664_apply_pre_rclk_sequence(struct gc555_it6664 *it6664)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	int ret;

	ret = regmap_write(sw, IT6664_SWITCH_REG_RESET, 0x01);
	if (ret)
		return ret;
	ret = regmap_write(sw, IT6664_SWITCH_REG_RESET, 0x00);
	if (ret)
		return ret;

	/* Set the bank-1 RX latch immediately after switch reset. */
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK,
				BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_73, BIT(2), BIT(2));
	if (ret)
		return ret;
	/*
	 * Bank 1 bit 7 lets the chip answer SCDC (scramble + 1/40 clock)
	 * ahead of firmware. Reg 0x20 bits 6:3 are the matching flag read
	 * (update-read, 2-byte status, 200 ms poll). 1440p120 is 483 MHz
	 * and the PS5 drops the link if this pair is missing. Same two
	 * writes as it6664_hdmi2sp_initial.
	 */
	ret = it6664_write_bits(sw, 0x1d, BIT(7), BIT(7));
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x20, 0x78, 0x78);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), 0);
	if (ret)
		return ret;

	ret = regmap_write(sw, IT6664_SWITCH_REG_10, 0x6e);
	if (ret)
		return ret;
	ret = regmap_write(sw, IT6664_SWITCH_REG_RX_PORT0_MAP, 0x71);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_0E,
				GENMASK(2, 0), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_08,
				GENMASK(3, 0), GENMASK(3, 0));
	if (ret)
		return ret;
	ret = regmap_write(sw, IT6664_SWITCH_REG_RX_PORT0_MAP, 0x71);
	if (ret)
		return ret;

	return regmap_write(sw, IT6664_SWITCH_REG_TX_COMMON_MAP, 0x97);
}

static int it6664_write_sequence(struct regmap *map,
				 const struct reg_sequence *sequence,
				 size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = regmap_write(map, sequence[i].reg, sequence[i].def);
		if (ret)
			return ret;
	}

	return 0;
}

static int
it6664_apply_updates(struct regmap *map,
		     const struct it6664_reg_update *updates,
		     size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = it6664_write_bits(map, updates[i].reg, updates[i].mask,
					updates[i].value);
		if (ret)
			return ret;
	}

	return 0;
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

static int it6664_select_rx_bank(struct regmap *rx, u8 bank)
{
	return it6664_write_bits(rx, IT6664_RX_REG_BANK,
				 IT6664_RX_BANK_MASK, bank);
}

static int it6664_set_siprom_address(struct regmap *sw, u16 address)
{
	int ret;

	ret = regmap_write(sw, IT6664_SWITCH_REG_SIPROM_ADDR_HI,
			   (address >> 8) & 0x0f);
	if (ret)
		return ret;
	ret = regmap_write(sw, IT6664_SWITCH_REG_SIPROM_ADDR_LO,
			   address & 0xff);
	if (ret)
		return ret;

	return regmap_write(sw, IT6664_SWITCH_REG_SIPROM_COMMAND, 0x04);
}

static int it6664_read_siprom_pair(struct regmap *sw, u8 *low, u8 *high)
{
	int ret;

	ret = it6664_read_byte(sw, IT6664_SWITCH_REG_SIPROM_DATA_LO, low);
	if (ret)
		return ret;

	return it6664_read_byte(sw, IT6664_SWITCH_REG_SIPROM_DATA_HI, high);
}

static int it6664_read_siprom(struct gc555_it6664 *it6664, u32 *raw)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	u8 low0 = 0;
	u8 high0 = 0;
	u8 low1 = 0;
	u8 high1 = 0;
	u16 word0;
	u16 word1;
	u16 address;
	int cleanup_ret;
	int ret;

	ret = it6664_write_sequence(sw, it6664_siprom_setup,
				    ARRAY_SIZE(it6664_siprom_setup));
	if (ret)
		goto cleanup;
	ret = it6664_read_siprom_pair(sw, &low0, &high0);
	if (ret)
		goto cleanup;

	ret = it6664_set_siprom_address(sw, 0x0001);
	if (ret)
		goto cleanup;
	ret = it6664_read_siprom_pair(sw, &low1, &high1);
	if (ret)
		goto cleanup;

	word0 = ((u16)high0 << 8) | low0;
	word1 = ((u16)high1 << 8) | low1;
	address = word0 == 0xffff && word1 == 0x0000 ? 0x04b0 : 0x00b0;

	ret = it6664_set_siprom_address(sw, address);
	if (ret)
		goto cleanup;
	ret = it6664_read_siprom_pair(sw, &low0, &high0);
	if (ret)
		goto cleanup;

	ret = it6664_set_siprom_address(sw, address + 1);
	if (ret)
		goto cleanup;
	ret = it6664_read_siprom_pair(sw, &low1, &high1);
	if (ret)
		goto cleanup;

	*raw = low0 | ((u32)high0 << 8) | ((u32)low1 << 16);
	if (high1 > 0xbf)
		*raw /= 100;

cleanup:
	cleanup_ret = it6664_write_sequence(sw, it6664_siprom_cleanup,
					    ARRAY_SIZE(it6664_siprom_cleanup));
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int it6664_calibrate_rclk(struct gc555_it6664 *it6664)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx_common =
		it6664->maps[IT6664_MAP_TX_COMMON].regmap;
	u32 raw = 0;
	u32 rclk_khz;
	u32 timer;
	u8 fraction;
	u8 integer;
	int ret;

	ret = it6664_read_siprom(it6664, &raw);
	if (ret)
		return ret;

	if (raw >= IT6664_RCLK_MIN_KHZ && raw <= IT6664_RCLK_MAX_KHZ)
		rclk_khz = raw;
	else
		rclk_khz = IT6664_RCLK_DEFAULT_KHZ;

	timer = rclk_khz * 10;
	ret = regmap_write(tx_common, IT6664_TX_COMMON_REG_TIMER_LO,
			   timer & 0xff);
	if (ret)
		return ret;
	ret = regmap_write(tx_common, IT6664_TX_COMMON_REG_TIMER_HI,
			   (timer >> 8) & 0xff);
	if (ret)
		return ret;
	ret = it6664_write_bits(tx_common, IT6664_TX_COMMON_REG_TIMER_TOP,
				GENMASK(1, 0), (timer >> 16) & GENMASK(1, 0));
	if (ret)
		return ret;

	integer = (rclk_khz / 1000) & GENMASK(5, 0);
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_RCLK_INTEGER,
				GENMASK(5, 0), integer);
	if (ret)
		return ret;
	fraction = ((rclk_khz % 1000) * 0x100) / 1000;
	ret = regmap_write(sw, IT6664_SWITCH_REG_RCLK_FRACTION, fraction);
	if (ret)
		return ret;

	ret = it6664_write_bits(tx_common, IT6664_TX_COMMON_REG_50, BIT(2), 0);
	if (ret)
		return ret;
	ret = it6664_write_sequence(tx_common, it6664_post_rclk_sequence,
				    ARRAY_SIZE(it6664_post_rclk_sequence));
	if (ret)
		return ret;

	it6664->siprom_raw = raw;
	it6664->rclk_khz = rclk_khz;
	return 0;
}

static int it6664_reset_rx(struct gc555_it6664 *it6664)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(it6664_rx_reset_sequence); i++) {
		if (i == 4)
			usleep_range(10000, 11000);
		ret = regmap_write(rx, it6664_rx_reset_sequence[i].reg,
				   it6664_rx_reset_sequence[i].def);
		if (ret)
			return ret;
	}

	return 0;
}

static int it6664_prepare_rx_caof(struct gc555_it6664 *it6664)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int ret;

	ret = it6664_apply_updates(rx, it6664_caof_prefix_updates,
				   ARRAY_SIZE(it6664_caof_prefix_updates));
	if (ret)
		return ret;
	ret = it6664_write_sequence(rx, it6664_caof_zero_sequence,
				    ARRAY_SIZE(it6664_caof_zero_sequence));
	if (ret)
		return ret;

	return it6664_apply_updates(rx, it6664_caof_prefix_tail,
				      ARRAY_SIZE(it6664_caof_prefix_tail));
}

static int it6664_wait_rx_caof(struct gc555_it6664 *it6664,
			       struct it6664_caof_result *result)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	unsigned int attempt;
	int ret;

	result->timed_out = true;
	for (attempt = 0; attempt < IT6664_RX_CAOF_POLL_COUNT; attempt++) {
		ret = it6664_read_byte(rx, IT6664_RX_REG_CAOF_STATUS,
				       &result->status);
		if (ret)
			return ret;
		result->polls = attempt + 1;
		if (result->status & GENMASK(5, 4)) {
			result->timed_out = false;
			return 0;
		}
		if (attempt > 2) {
			ret = it6664_write_bits(rx, 0x2a, BIT(6), BIT(6));
			if (ret)
				return ret;
			ret = it6664_write_bits(rx, 0x2a, BIT(6), 0);
			if (ret)
				return ret;
		}
	}

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x3a, BIT(7), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x2a, BIT(6), BIT(6));
	if (ret)
		return ret;

	return it6664_write_bits(rx, 0x2a, BIT(6), 0);
}

static int it6664_finish_rx_caof(struct gc555_it6664 *it6664,
				 struct it6664_caof_result *result)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int ret;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x5a, &result->reg5a);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x59, &result->reg59[0]);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x59, &result->reg59[1]);
	if (ret)
		return ret;

	return it6664_apply_updates(rx, it6664_caof_post_updates,
				      ARRAY_SIZE(it6664_caof_post_updates));
}

static int it6664_initialize_rx_caof(struct gc555_it6664 *it6664,
				     struct it6664_caof_result *result)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int cleanup_ret;
	int ret;

	ret = it6664_reset_rx(it6664);
	if (!ret)
		ret = it6664_prepare_rx_caof(it6664);
	if (!ret)
		ret = it6664_wait_rx_caof(it6664, result);
	if (!ret)
		ret = it6664_finish_rx_caof(it6664, result);

	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int it6664_initialize_rx_registers(struct gc555_it6664 *it6664)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int cleanup_ret;
	int ret;

	ret = regmap_write(rx, 0x56, 0xff);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x57, 0xff);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0xa8, BIT(3), BIT(3));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0xa7, BIT(6), BIT(6));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x26, BIT(5), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x27, 0xff, 0x9f);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x28, 0xff, 0x9f);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x29, 0xff, 0x9f);
	if (ret)
		goto cleanup;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x28, 0x59, 0x59);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x2a, BIT(0), BIT(0));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x43, BIT(1), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x44, GENMASK(5, 0), 0x19);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x3c, BIT(0), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x45, 0xdf);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x46, GENMASK(5, 0), 0x15);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x47, 0xff, 0x88);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x49, 0xe1);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x23, 0xa0);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x53, 0x0f);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0xe3, 0x04);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0xce, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x3c, BIT(5), 0);
	if (ret)
		goto cleanup;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0xe3, BIT(0), BIT(0));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0xe3, BIT(2) | BIT(1), 0x03);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0xf0, 0xa0);
	if (ret)
		goto cleanup;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x28, BIT(7) | BIT(3), BIT(7) | BIT(3));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x3b, BIT(5), BIT(5));
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x26, 0xff);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x42, BIT(5), 0);

cleanup:
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int it6664_prepare_initial_rx_hpd(struct gc555_it6664 *it6664)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0xab, 0x4a);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0xac, 0x40);

cleanup:
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int it6664_quiesce_rx_hpd(struct gc555_it6664 *it6664)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int cleanup_ret;
	int ret;

	if (it6664->runtime.rx.bus_mode != 2)
		return gc555_it6664_rx_set_hpd(it6664, false);

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0xab, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0xac, 0x00);

cleanup:
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static void it6664_reset_rx_source_epoch(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	enum it6664_rx_timer_state timer_state = state->timer_state;
	bool hdcp_enabled = state->hdcp_enabled;

	memset(state, 0, sizeof(*state));
	state->timer_state = timer_state;
	state->hdcp_enabled = hdcp_enabled;
}

int gc555_it6664_rx_set_hpd(struct gc555_it6664 *it6664, bool high)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	unsigned int value;
	int cleanup_ret;
	int ret;
	u8 hpd_mode;

	if (high) {
		ret = regmap_read(rx, IT6664_RX_REG_STATUS, &value);
		if (ret)
			return ret;
		if (!(value & BIT(0)))
			return 0;
	}

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = regmap_read(rx, 0xab, &value);
	if (ret)
		goto cleanup;
	if (high && value != 0xca) {
		ret = regmap_write(rx, 0xab, 0xca);
	} else if (!high && value == 0xca) {
		ret = regmap_write(rx, 0xab, 0x4a);
		if (ret)
			goto cleanup;
		ret = regmap_write(rx, 0xab, 0x00);
		if (ret)
			goto cleanup;
		ret = regmap_write(rx, 0xac, 0x00);
	}

cleanup:
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;

	hpd_mode = high && it6664->runtime.rx.bus_mode == 2 ? 0x0c : 0x00;
	ret = regmap_write(rx, 0x26, high ? hpd_mode : 0xff);
	if (ret)
		return ret;

	return regmap_write(rx, 0x55, high ? 0xff : 0x00);
}

static int it6664_quiesce_rx(struct gc555_it6664 *it6664)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(rx, 0x53, 0xe0, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x54, 0xff, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x55, 0x07, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x57, 0x0f, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0xc5, BIT(4), BIT(4));
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0xc5, BIT(4), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_RESET,
				BIT(2), BIT(2));
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_RESET, BIT(2), 0);
	if (ret)
		return ret;
	ret = it6664_quiesce_rx_hpd(it6664);
	if (ret)
		return ret;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x27, 0x9f);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x28, 0x9f);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x29, 0x9f);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x20, 0x1b);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x21, 0x03);

cleanup:
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (!ret)
		it6664_reset_rx_source_epoch(it6664);

	return ret;
}

static int it6664_initialize_rx_power_state(struct gc555_it6664 *it6664)
{
	int ret;

	ret = it6664_prepare_initial_rx_hpd(it6664);
	if (ret)
		return ret;

	return it6664_quiesce_rx(it6664);
}

static int it6664_read_rx_snapshot(struct gc555_it6664 *it6664,
				   struct it6664_rx_snapshot *snapshot)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int ret;

	ret = it6664_read_byte(rx, 0x08, &snapshot->reg08);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x13, &snapshot->reg13);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x14, &snapshot->reg14);
	if (ret)
		return ret;

	return it6664_read_byte(rx, 0x19, &snapshot->reg19);
}

static int
it6664_read_rx_irq(struct gc555_it6664 *it6664,
		   struct it6664_rx_irq *irq)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int ret;

	ret = it6664_read_byte(rx, 0x05, &irq->reg05);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x06, &irq->reg06);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x07, &irq->reg07);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x08, &irq->reg08);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x09, &irq->reg09);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x10, &irq->reg10);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x11, &irq->reg11);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x12, &irq->reg12);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x13, &irq->reg13);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x14, &irq->reg14);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x15, &irq->reg15);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x19, &irq->reg19);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x1a, &irq->reg1a);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x1b, &irq->reg1b);
	if (ret)
		return ret;

	return it6664_read_byte(rx, 0x1d, &irq->reg1d);
}

static int
it6664_ack_rx_irq(struct gc555_it6664 *it6664,
		  const struct it6664_rx_irq *irq)
{
	static const u8 ack_regs[] = {
		0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11,
	};
	const u8 values[] = {
		irq->reg05, irq->reg06, irq->reg07, irq->reg08,
		irq->reg09, irq->reg10, irq->reg11,
	};
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ack_regs); i++) {
		ret = regmap_write(rx, ack_regs[i], values[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static bool it6664_rx_irq_supported(const struct it6664_rx_irq *irq)
{
	return !(irq->reg05 & ~IT6664_RX_IRQ05_SUPPORTED) &&
	       !(irq->reg06 & ~(BIT(0) | IT6664_RX_IRQ06_ACK_ONLY)) &&
	       !(irq->reg07 & ~IT6664_RX_IRQ07_SUPPORTED) &&
	       !(irq->reg08 & ~IT6664_RX_IRQ08_ACK_ONLY) && !irq->reg09 &&
	       !(irq->reg10 & ~IT6664_RX_IRQ10_SUPPORTED) &&
	       !(irq->reg11 & ~IT6664_RX_IRQ11_SUPPORTED) &&
	       !(irq->reg12 & ~IT6664_RX_IRQ12_SUPPORTED);
}

static bool it6664_rx_irq_is_noop(const struct it6664_rx_irq *irq)
{
	return (irq->reg05 || irq->reg10) &&
	       !(irq->reg05 & ~IT6664_RX_IRQ05_NOOP) &&
	       !irq->reg06 && !irq->reg07 && !irq->reg08 && !irq->reg09 &&
	       !(irq->reg10 & ~IT6664_RX_IRQ10_NOOP) &&
	       !irq->reg11 && !irq->reg12;
}

static int
it6664_handle_rx_source_loss(struct gc555_it6664 *it6664,
			     const struct it6664_rx_irq *irq)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int cleanup_ret;
	int ret;

	ret = it6664_quiesce_rx(it6664);
	if (ret)
		return ret;
	ret = gc555_it6664_tx_power_down_all(it6664);
	if (ret)
		return ret;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0xe5, GENMASK(4, 2), 0);

cleanup:
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;

	dev_dbg(it6664->gc555->dev,
		"IT6664 RX source-loss IRQ handled irq=%02x status=%02x\n",
		irq->reg05, irq->reg13);
	return 0;
}

static int
it6664_handle_rx_detect_bus(struct gc555_it6664 *it6664,
			    const struct it6664_rx_irq *irq)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	bool pulse_active = false;
	bool rx_bank_three = false;
	unsigned int status;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_0C,
				BIT(2), BIT(2));
	if (ret)
		return ret;
	pulse_active = true;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_10, BIT(6), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_10,
				BIT(6), BIT(6));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_0C, BIT(2), 0);
	if (ret)
		goto cleanup;
	pulse_active = false;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (ret)
		return ret;
	ret = regmap_read(rx, IT6664_RX_REG_STATUS, &status);
	if (ret)
		return ret;
	if ((status & (BIT(6) | BIT(0))) != BIT(0))
		return -EAGAIN;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	rx_bank_three = true;
	ret = it6664_write_bits(rx, 0x3a, GENMASK(2, 1), BIT(1));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	rx_bank_three = false;
	ret = it6664_write_bits(rx, 0x29, BIT(0), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x26, GENMASK(3, 2), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, 0x69, BIT(5) | GENMASK(3, 0), 0);
	if (ret)
		goto cleanup;

	it6664->runtime.rx.bus_mode = 0;
	ret = gc555_it6664_rx_set_hpd(it6664, true);
	if (ret)
		goto cleanup;

	dev_dbg(it6664->gc555->dev,
		"IT6664 RX detect-bus IRQ handled irq=%02x/%02x/%02x/%02x/%02x/%02x/%02x status=%02x/%02x/%02x/%02x\n",
		irq->reg05, irq->reg06, irq->reg07, irq->reg08,
		irq->reg09, irq->reg10, irq->reg11, irq->reg12,
		irq->reg13, irq->reg14, irq->reg19);

	return 0;

cleanup:
	if (rx_bank_three) {
		cleanup_ret =
			it6664_write_bits(rx, IT6664_RX_REG_BANK,
					  IT6664_RX_BANK_MASK,
					  IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}
	if (pulse_active) {
		cleanup_ret =
			it6664_write_bits(sw, IT6664_SWITCH_REG_0C,
					  BIT(2), 0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static bool
it6664_rx_irq_is_signal_start(const struct it6664_rx_state *state,
			      const struct it6664_rx_irq *irq)
{
	bool coalesced_start;
	bool retained_stable_level;

	if (irq->reg05 == IT6664_RX_IRQ_SIGNAL_START &&
	    irq->reg06 == BIT(0) &&
	    irq->reg07 == IT6664_RX_IRQ07_BANK2_STATUS &&
	    !irq->reg08 && !irq->reg09 && !irq->reg10 && !irq->reg11 &&
	    (irq->reg13 & (BIT(7) | BIT(3))) == (BIT(7) | BIT(3)))
		return true;

	/*
	 * A source already transmitting during probe can coalesce later IRQs
	 * into the first snapshot, or retain only the SCDT level after a warm
	 * driver reload.  Seed the same signal-start leaf only when the live
	 * clock, receiver, lane-ready, and SCDT levels prove that tuple mature.
	 */
	coalesced_start =
		(irq->reg05 & IT6664_RX_IRQ_SIGNAL_START) ==
			IT6664_RX_IRQ_SIGNAL_START &&
		(irq->reg06 & BIT(0)) &&
		(irq->reg07 & IT6664_RX_IRQ07_BANK2_STATUS);
	retained_stable_level =
		(irq->reg14 & GENMASK(5, 3)) == GENMASK(5, 3);
	return !state->signal_started &&
	       (coalesced_start || retained_stable_level) &&
	       (irq->reg13 & (BIT(7) | BIT(4) | BIT(3) | BIT(0))) ==
			(BIT(7) | BIT(4) | BIT(3) | BIT(0)) &&
	       (irq->reg19 & BIT(7));
}

static bool
it6664_rx_irq_is_signal_restart(const struct it6664_rx_state *state,
				const struct it6664_rx_irq *irq)
{
	return state->signal_started &&
	       (irq->reg05 & IT6664_RX_IRQ_SIGNAL_START) ==
			IT6664_RX_IRQ_SIGNAL_START &&
	       (irq->reg06 & BIT(0)) &&
	       (irq->reg10 & IT6664_RX_IRQ10_ACTIONS) &&
	       (irq->reg13 & (BIT(7) | BIT(4) | BIT(3) | BIT(0))) ==
			(BIT(7) | BIT(4) | BIT(3) | BIT(0)) &&
	       (irq->reg19 & BIT(7));
}

static bool
it6664_rx_irq_is_reg12(const struct it6664_rx_state *state,
		       const struct it6664_rx_irq *irq)
{
	return state->signal_started &&
	       !irq->reg05 && !irq->reg06 && !irq->reg07 && !irq->reg08 &&
	       !irq->reg09 && !irq->reg10 && !irq->reg11 && irq->reg12 &&
	       !(irq->reg12 & ~IT6664_RX_IRQ12_SUPPORTED);
}

static bool
it6664_rx_irq_is_eq_result(const struct it6664_rx_state *state,
			   const struct it6664_rx_irq *irq)
{
	bool running = state->signal_started && state->irq12_handled &&
		(state->eq14_running || state->eq20_running);
	bool supported = (irq->reg07 & IT6664_RX_IRQ07_EQ_RESULT) &&
		!(irq->reg07 & ~IT6664_RX_IRQ07_SUPPORTED);

	if (!running || !supported)
		return false;
	if (irq->reg05 || irq->reg06)
		return false;
	if (!irq->reg08 && !irq->reg09 && !irq->reg10 && !irq->reg11 &&
	    !irq->reg12)
		return true;

	return !irq->reg09 && !irq->reg11 &&
	       (irq->reg13 & BIT(4)) && (irq->reg19 & BIT(7));
}

static bool it6664_rx_irq_is_scdt(const struct it6664_rx_irq *irq)
{
	return !irq->reg05 && !irq->reg06 && !irq->reg07 && !irq->reg08 &&
	       !irq->reg09 && (irq->reg10 & IT6664_RX_IRQ10_ACTIONS) &&
	       !(irq->reg10 & ~IT6664_RX_IRQ10_SUPPORTED) &&
	       !irq->reg11 && !irq->reg12;
}

static bool
it6664_rx_irq_is_coalesced_stable(const struct it6664_rx_state *state,
				  const struct it6664_rx_irq *irq)
{
	bool actionable;

	actionable = (irq->reg06 & BIT(0)) ||
		     (irq->reg07 & IT6664_RX_IRQ07_SUPPORTED) ||
		     (irq->reg10 & IT6664_RX_IRQ10_ACTIONS) ||
		     (irq->reg11 & (IT6664_RX_IRQ11_COLOR_DEPTH |
				      IT6664_RX_IRQ11_BANK2_STATUS)) ||
		     (irq->reg12 & IT6664_RX_IRQ12_SUPPORTED);

	return state->signal_started && actionable &&
	       !(irq->reg05 & ~IT6664_RX_IRQ05_NOOP) &&
	       !(irq->reg08 & ~IT6664_RX_IRQ08_ACK_ONLY) && !irq->reg09 &&
	       it6664_rx_irq_supported(irq) &&
	       (irq->reg13 & (BIT(7) | BIT(4) | BIT(3) | BIT(0))) ==
			(BIT(7) | BIT(4) | BIT(3) | BIT(0)) &&
	       (irq->reg19 & BIT(7));
}

static int it6664_handle_rx_scdt_loss(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	bool tx_active = false;
	int ret;

	state->scdt = false;
	memset(&state->video_timing, 0, sizeof(state->video_timing));
	ret = gc555_link_set_splitter_scdt(it6664->gc555, false);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x40, GENMASK(1, 0), BIT(1));
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x0d, 0x00);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x31, GENMASK(1, 0), 0);
	if (ret)
		return ret;
	ret = gc555_it6664_tx_reset_signal(it6664, &tx_active);
	if (tx_active)
		state->clock_configured = false;
	if (ret)
		return ret;

	state->eq_terminal_sampled = false;
	state->eq_terminal_valid = false;
	state->eq_recovery_needed = false;
	return 0;
}

static int it6664_handle_rx_scdt_lock(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	bool was_scdt = state->scdt;
	u8 status14;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(rx, 0x40, GENMASK(1, 0), 0);
	if (ret)
		return ret;
	ret = gc555_link_set_splitter_scdt(it6664->gc555, true);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x0b, 0xff);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x0b, 0x00);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x4e, GENMASK(3, 0), GENMASK(3, 0));
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x0c, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x0c, BIT(3), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x4e, GENMASK(3, 0), 0);
	if (ret)
		return ret;

	if (was_scdt) {
		memset(&state->video_timing, 0,
		       sizeof(state->video_timing));
		state->clock_configured = false;
		ret = regmap_write(sw, 0x0d, 0x00);
		if (ret)
			return ret;
	}
	state->scdt = true;
	if (!state->clock_configured) {
		ret = gc555_it6664_tx_power_connected_ports(it6664);
		if (ret)
			return ret;
	}

	ret = it6664_write_bits(sw, 0x67, GENMASK(3, 0), 0);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x68, 0x00);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS_14, &status14);
	if (ret)
		return ret;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0xa7, BIT(6),
				(status14 & BIT(0)) << 6);

	cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;

	state->clock_configured = true;
	return 0;
}

static int it6664_handle_rx_scdt_irq(struct gc555_it6664 *it6664,
				     const struct it6664_rx_irq *irq)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int ret;

	if (irq->reg10 & IT6664_RX_IRQ10_CLOCK_CHANGE) {
		ret = it6664_write_bits(rx, 0x40, GENMASK(1, 0), 0);
		if (ret)
			return ret;
	}
	if (!(irq->reg10 & IT6664_RX_IRQ10_SCDT_CHANGE))
		return 0;
	if (irq->reg19 & BIT(7))
		return it6664_handle_rx_scdt_lock(it6664);

	return it6664_handle_rx_scdt_loss(it6664);
}

static int it6664_handle_rx_signal_irq(struct gc555_it6664 *it6664)
{
	struct it6664_runtime *runtime = &it6664->runtime;
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	struct regmap *tx;
	u8 status;
	unsigned int port;
	int ret;

	ret = regmap_write(rx, 0x05, BIT(2));
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x53, 0xe0, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x54, 0xff, 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x55, 0x07, 0);
	if (ret)
		return ret;
	ret = regmap_write(rx, 0x05, 0xe8);
	if (ret)
		return ret;
	ret = regmap_write(rx, 0x06, 0xfe);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS, &status);
	if (ret)
		return ret;
	if (!(status & BIT(3)))
		return -EAGAIN;

	ret = it6664_write_bits(rx, 0x23, BIT(1), 0);
	if (ret)
		return ret;
	for (port = 0; port < IT6664_TX_PORT_COUNT; port++) {
		tx = it6664->maps[IT6664_MAP_TX_PORT0 + port].regmap;
		ret = it6664_read_byte(tx, 0x03, &status);
		if (ret)
			return ret;
		runtime->tx[port].rx_sense = status & BIT(1);
	}
	ret = it6664_write_bits(rx, 0x54, BIT(0), BIT(0));
	if (ret)
		return ret;

	if (runtime->rx.bus_mode ||
	    runtime->rx.timer_state == IT6664_RX_TIMER_TX_OFF)
		return 0;

	ret = it6664_write_bits(sw, 0x1a, BIT(1), 0);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x19, 0x0f);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x1d, 0xa0);
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

	runtime->rx.timer_state = IT6664_RX_TIMER_ARMED;
	return 0;
}

static int
it6664_handle_rx_eq_start(struct gc555_it6664 *it6664,
			  const struct it6664_rx_irq *irq)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int ret;

	ret = it6664_write_bits(rx, 0x53, 0xe0, 0xe0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x55, 0x07, 0x07);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x57, 0x0f, 0x0f);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x5d, 0x06, 0x06);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x5e, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x5f, BIT(0), BIT(0));
	if (ret)
		return ret;

	if (irq->reg14 & BIT(6)) {
		if (!state->eq20_done && !state->eq20_running)
			state->eq_state = IT6664_RX_EQ_START;
	} else if (!state->eq14_done && !state->eq14_running) {
		state->eq_state = IT6664_RX_EQ_START;
	}

	return 0;
}

static int it6664_read_rx_bank2_status(struct gc555_it6664 *it6664)
{
	static const u8 regs[] = { 0x24, 0x25, 0x26, 0x27 };
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 value;
	unsigned int i;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
				IT6664_RX_BANK_MASK, IT6664_RX_BANK_2);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = it6664_read_byte(rx, regs[i], &value);
		if (ret)
			break;
	}
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
					IT6664_RX_BANK_MASK, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int
it6664_handle_rx_eq_result_irq(struct gc555_it6664 *it6664,
			       const struct it6664_rx_irq *irq)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	bool bank_three = false;
	int cleanup_ret;
	int ret;

	if (irq->reg07 & IT6664_RX_IRQ07_BANK2_STATUS) {
		ret = it6664_read_rx_bank2_status(it6664);
		if (ret)
			return ret;
	}

	state->eq_state = IT6664_RX_EQ_READ_RESULT;
	if (!(irq->reg07 & BIT(7)))
		return 0;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = it6664_write_bits(rx, 0x22, BIT(2), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x22, GENMASK(5, 3), 0);

cleanup:
	if (bank_three) {
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static int it6664_handle_rx_reg12_reset(struct gc555_it6664 *it6664)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	bool pulse_active = false;
	u8 value;
	int cleanup_ret;
	int ret;

	ret = it6664_read_byte(rx, 0x1a, &value);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x1b, &value);
	if (ret)
		return ret;
	ret = regmap_write(rx, 0x1a, 0xff);
	if (ret)
		return ret;
	ret = regmap_write(rx, 0x1b, 0x07);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_0C,
				BIT(3), BIT(3));
	if (ret)
		return ret;
	pulse_active = true;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_0C, BIT(3), 0);
	if (!ret)
		pulse_active = false;

	if (pulse_active) {
		cleanup_ret = it6664_write_bits(sw, IT6664_SWITCH_REG_0C, BIT(3), 0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static int it6664_observe_rx_drm_infoframe(struct gc555_it6664 *it6664)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 packet[IT6664_DRM_INFOFRAME_CAPTURE_SIZE] = {};
	u8 header = 0;
	int cleanup_ret;
	int ret;

	ret = regmap_write(rx, IT6664_RX_REG_PACKET_SELECT,
			   IT6664_RX_PACKET_DRM);
	if (ret)
		return ret;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_2);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0x24, &header);
	if (!ret && header == IT6664_RX_PACKET_DRM)
		ret = regmap_bulk_read(rx, 0x24, packet, sizeof(packet));
	cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;

	return 0;
}

static int
it6664_write_csc_matrix(struct regmap *sw, bool high, const u8 *matrix)
{
	unsigned int offset_reg = high ? IT6664_SWITCH_REG_CSC_HIGH_OFFSET :
					 IT6664_SWITCH_REG_CSC_LOW_OFFSET;
	unsigned int matrix_reg = high ? IT6664_SWITCH_REG_CSC_HIGH_MATRIX :
					 IT6664_SWITCH_REG_CSC_LOW_MATRIX;
	int ret;

	ret = regmap_bulk_write(sw, offset_reg, it6664_csc_zero_offset,
				sizeof(it6664_csc_zero_offset));
	if (ret)
		return ret;

	return regmap_bulk_write(sw, matrix_reg, matrix,
				 sizeof(it6664_csc_ycbcr_to_rgb[0]));
}

static int
it6664_read_rx_timing_word(struct regmap *rx, unsigned int hi_reg,
			   unsigned int lo_reg, u16 *value)
{
	u8 hi;
	u8 lo;
	int ret;

	ret = it6664_read_byte(rx, hi_reg, &hi);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, lo_reg, &lo);
	if (ret)
		return ret;

	*value = (((u16)hi << 8) | lo) & GENMASK(13, 0);
	return 0;
}

int gc555_it6664_rx_refresh_video_timing(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct it6664_rx_video_timing timing = {};
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u64 adjusted_pixel_clock_khz;
	u64 pixel_clock_khz;
	u32 sample_sum = 0;
	u8 dummy;
	u8 count_hi;
	u8 count_lo;
	u8 depth;
	unsigned int sample;
	int ret;

	memset(&state->video_timing, 0, sizeof(state->video_timing));
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		return ret;

	/* Prime the receiver counters in the required order before sampling. */
	for (sample = 0; sample < IT6664_RX_VIDEO_SAMPLE_COUNT; sample++) {
		ret = it6664_read_byte(rx, 0x48, &dummy);
		if (ret)
			return ret;
	}
	ret = it6664_read_byte(rx, 0x43, &dummy);
	if (ret)
		return ret;

	for (sample = 0; sample < IT6664_RX_VIDEO_SAMPLE_COUNT; sample++) {
		ret = it6664_read_byte(rx, 0x9a, &count_hi);
		if (ret)
			return ret;
		ret = it6664_read_byte(rx, 0x99, &count_lo);
		if (ret)
			return ret;
		sample_sum += (((u32)count_hi << 8) | count_lo) &
			      GENMASK(9, 0);
	}

	ret = it6664_read_byte(rx, 0x98, &depth);
	if (ret)
		return ret;
	ret = it6664_read_rx_timing_word(rx, 0x9c, 0x9b, &timing.htotal);
	if (ret)
		return ret;
	ret = it6664_read_rx_timing_word(rx, 0x9e, 0x9d, &timing.hactive);
	if (ret)
		return ret;
	ret = it6664_read_rx_timing_word(rx, 0xa3, 0xa2, &timing.vtotal);
	if (ret)
		return ret;
	ret = it6664_read_rx_timing_word(rx, 0xa5, 0xa4, &timing.vactive);
	if (ret)
		return ret;

	if (!it6664->rclk_khz || !sample_sum || !timing.htotal ||
	    !timing.vtotal)
		return -ERANGE;
	pixel_clock_khz = div64_u64((u64)it6664->rclk_khz *
				    IT6664_RX_VIDEO_COUNTER_SCALE,
				    sample_sum);
	if (pixel_clock_khz > U32_MAX)
		return -ERANGE;
	timing.pixel_clock_khz = pixel_clock_khz;
	timing.frame_rate_hz = div64_u64(pixel_clock_khz * 1000,
					 (u64)timing.htotal * timing.vtotal);
	timing.is_4k30 =
		((u32)timing.hactive <<
		 (state->colorspace == IT6664_RX_COLORSPACE_YCBCR420)) >
		0xeff && timing.frame_rate_hz < 40;

	adjusted_pixel_clock_khz = pixel_clock_khz;
	switch ((depth >> 4) & GENMASK(1, 0)) {
	case 1:
		adjusted_pixel_clock_khz =
			div64_u64(adjusted_pixel_clock_khz * 5, 4);
		break;
	case 2:
		adjusted_pixel_clock_khz =
			div64_u64(adjusted_pixel_clock_khz * 3, 2);
		break;
	default:
		break;
	}
	if (adjusted_pixel_clock_khz > U32_MAX)
		return -ERANGE;
	timing.adjusted_pixel_clock_khz = adjusted_pixel_clock_khz;
	timing.high_frame_rate = timing.frame_rate_hz > 100 &&
				 adjusted_pixel_clock_khz > 30000;
	timing.valid = true;
	state->color_depth = depth >> 4;
	state->video_timing = timing;

	return 0;
}

static int it6664_configure_rx_csc(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 bank2_status[0x12];
	u8 avi[2];
	u8 reg14 = 0;
	u8 table;
	u8 converter_output_mode_request = 0;
	u8 converter_output_mode = 0;
	u8 csc_output_mode;
	u8 csc_control;
	u8 csc_mode;
	const u8 *rgb_to_ycbcr;
	const u8 *ycbcr_to_rgb;
	enum it6664_rx_colorspace colorspace;
	unsigned int port;
	int cleanup_ret;
	int ret;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_2);
	if (ret)
		return ret;
	ret = regmap_bulk_read(rx, 0x15, avi, sizeof(avi));
	cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;

	colorspace = (avi[0] >> 5) & 0x03;
	table = (avi[1] & GENMASK(7, 6)) == BIT(7);
	rgb_to_ycbcr = it6664_csc_rgb_to_ycbcr[table];
	ycbcr_to_rgb = it6664_csc_ycbcr_to_rgb[table];
	if (colorspace != IT6664_RX_COLORSPACE_YCBCR420) {
		ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS_14, &reg14);
		if (ret)
			return ret;
	}

	if (colorspace == IT6664_RX_COLORSPACE_YCBCR420) {
		ret = it6664_write_csc_matrix(sw, true, ycbcr_to_rgb);
		if (ret)
			return ret;
		ret = it6664_write_csc_matrix(sw, false, ycbcr_to_rgb);
		if (ret)
			return ret;
		converter_output_mode_request = 2;
		converter_output_mode = 2;
		csc_output_mode = 2;
		csc_control = 0x28;
		csc_mode = 0x28;
	} else if (reg14 & BIT(7)) {
		if (colorspace == IT6664_RX_COLORSPACE_RGB) {
			ret = it6664_write_csc_matrix(sw, true, rgb_to_ycbcr);
			if (!ret)
				ret = it6664_write_csc_matrix(sw, false,
							      rgb_to_ycbcr);
			converter_output_mode_request = 3;
			converter_output_mode = 3;
			csc_output_mode = 2;
			csc_control = 0x7a;
			csc_mode = 0x10;
		} else if (colorspace == IT6664_RX_COLORSPACE_YCBCR444) {
			ret = it6664_write_csc_matrix(sw, true, ycbcr_to_rgb);
			if (!ret)
				ret = it6664_write_csc_matrix(sw, false, ycbcr_to_rgb);
			converter_output_mode_request = 3;
			converter_output_mode = 3;
			csc_output_mode = 0;
			csc_control = 0x73;
			csc_mode = 0x18;
		} else {
			ret = it6664_write_csc_matrix(sw, false, ycbcr_to_rgb);
			csc_output_mode = 0;
			csc_control = 0x43;
			csc_mode = 0x00;
		}
		if (ret)
			return ret;
	} else if (colorspace == IT6664_RX_COLORSPACE_RGB) {
		ret = it6664_write_csc_matrix(sw, false, rgb_to_ycbcr);
		if (ret)
			return ret;
		csc_output_mode = 2;
		csc_control = 0x4a;
		csc_mode = 0x00;
	} else {
		ret = it6664_write_csc_matrix(sw, false, ycbcr_to_rgb);
		if (ret)
			return ret;
		csc_output_mode = 0;
		csc_control = 0x43;
		csc_mode = 0x00;
	}

	ret = regmap_write(sw, IT6664_SWITCH_REG_CSC_CONTROL, csc_control);
	if (ret)
		return ret;
	ret = regmap_write(sw, IT6664_SWITCH_REG_CSC_MODE, csc_mode);
	if (ret)
		return ret;

	state->colorspace = colorspace;
	state->converter_output_mode_request = converter_output_mode_request;
	state->converter_output_mode = converter_output_mode;
	state->csc_output_mode = csc_output_mode;
	state->csc_output_quantization = 0;
	for (port = 0; port < IT6664_TX_PORT_COUNT; port++)
		if (it6664->runtime.tx[port].video_state == IT6664_TX_VIDEO_OK)
			it6664->runtime.tx[port].video_state = IT6664_TX_VIDEO_RESET;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_2);
	if (ret)
		return ret;
	ret = regmap_bulk_read(rx, 0x12, bank2_status, sizeof(bank2_status));
	cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int it6664_ack_rx_reg12(struct gc555_it6664 *it6664,
			       const struct it6664_rx_irq *irq)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int ret;

	ret = it6664_write_bits(rx, 0x23, BIT(1),
				state->hdcp_enabled ? 0 : BIT(1));
	if (ret)
		return ret;
	ret = regmap_write(rx, 0x12, irq->reg12);
	if (ret)
		return ret;

	state->irq12_handled = true;
	return 0;
}

static int it6664_handle_rx_reg12(struct gc555_it6664 *it6664,
				  const struct it6664_rx_irq *irq)
{
	int ret;

	if (irq->reg12 & (BIT(7) | BIT(0)))
		memset(&it6664->runtime.rx.video_timing, 0,
		       sizeof(it6664->runtime.rx.video_timing));

	if (irq->reg12 & BIT(7)) {
		ret = it6664_handle_rx_reg12_reset(it6664);
		if (ret)
			return ret;
	}
	if (irq->reg12 & BIT(5)) {
		ret = it6664_observe_rx_drm_infoframe(it6664);
		if (ret)
			return ret;
	}
	if (irq->reg12 & BIT(0)) {
		ret = it6664_configure_rx_csc(it6664);
		if (ret)
			return ret;
	}

	return 0;
}

static int it6664_set_rx_sareq(struct gc555_it6664 *it6664, u8 parameter)
{
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	bool bank_three = false;
	int cleanup_ret;
	int ret;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = it6664_write_bits(rx, 0x20, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x22, 0x00);
	if (ret)
		goto cleanup;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;
	ret = regmap_write(rx, 0x07, 0xff);
	if (ret)
		return ret;
	ret = regmap_write(rx, 0x23, 0xb0);
	if (ret)
		return ret;
	ret = regmap_write(rx, 0x23, 0xa0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x3b, GENMASK(2, 0), 0x03);
	if (ret)
		return ret;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = regmap_write(rx, 0x26, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x27, 0x1f);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x28, 0x1f);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x29, 0x1f);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x2d, GENMASK(2, 0), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x30, (parameter << 2) ^ BIT(7));
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x31, 0xb0);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x32, 0x43);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x33, 0x47);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x34, 0x4b);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x35, 0x53);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x36, GENMASK(7, 6), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x37, 0x0b);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x38, 0xf2);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x39, 0x0d);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x4a, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x4b, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x54, BIT(7), BIT(7));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x54, GENMASK(5, 3), GENMASK(5, 3));
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x55, 0x40);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x22, BIT(2), BIT(2));

cleanup:
	if (bank_three) {
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static int it6664_start_rx_eq14(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	bool bank_three = false;
	int cleanup_ret;
	int ret;

	ret = regmap_write(rx, 0x07, 0xff);
	if (ret)
		return ret;
	ret = regmap_write(rx, 0x23, 0xb0);
	if (ret)
		return ret;
	ret = regmap_write(rx, 0x23, 0xa0);
	if (ret)
		return ret;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = regmap_write(rx, 0x2c, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x2d, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x20, 0x36);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x21, 0x0e);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x26, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x27, 0x1f);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x28, 0x1f);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x29, 0x1f);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x22, GENMASK(5, 3), GENMASK(5, 3));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x22, BIT(2), BIT(2));
	if (ret)
		goto cleanup;
	usleep_range(1000, 2000);
	ret = it6664_write_bits(rx, 0x22, BIT(2), 0);

cleanup:
	if (bank_three) {
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}
	if (ret)
		return ret;

	if (state->eq14_retry_count != U8_MAX)
		state->eq14_retry_count++;
	state->eq14_done = false;
	state->eq14_running = true;
	state->eq20_running = false;
	state->eq_state = IT6664_RX_EQ_IDLE;
	state->eq_terminal_sampled = false;
	state->eq_terminal_valid = false;
	state->eq_recovery_needed = false;
	return 0;
}

static int
it6664_start_rx_eq20(struct gc555_it6664 *it6664, u8 reg14)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	int ret;

	ret = it6664_write_bits(rx, 0x53, BIT(5), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x05, BIT(5), BIT(5));
	if (ret)
		return ret;
	if (reg14 & BIT(7)) {
		ret = it6664_set_rx_sareq(it6664, 0);
		if (ret)
			return ret;
	}

	state->eq20.path = IT6664_RX_EQ20_NONE;
	state->eq20_done = false;
	state->eq20_running = true;
	state->eq14_running = false;
	state->eq_terminal_sampled = false;
	state->eq_terminal_valid = false;
	state->eq_recovery_needed = false;
	if (reg14 & BIT(7))
		state->eq_state = IT6664_RX_EQ_IDLE;
	return 0;
}

static int it6664_measure_rx_eq20(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 amp_status[IT6664_RX_EQ_LANE_COUNT];
	u8 invalid_lo;
	u8 invalid_hi;
	u8 reg4a = 0;
	u8 reg37 = 0;
	u8 ignored;
	bool have_reg4a = false;
	bool have_reg37 = false;
	bool bank_three = false;
	unsigned int i;
	int cleanup_ret;
	int ret;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = it6664_read_byte(rx, 0x4a, &reg4a);
	if (ret)
		goto cleanup;
	have_reg4a = true;
	ret = it6664_write_bits(rx, 0x4a, BIT(7), BIT(7));
	if (ret)
		goto cleanup;
	for (i = 0; i < ARRAY_SIZE(state->eq20.snapshot); i++) {
		ret = it6664_read_byte(rx, 0x4b + i, &state->eq20.snapshot[i]);
		if (ret)
			goto cleanup;
	}
	ret = it6664_write_bits(rx, 0x4a, BIT(7), reg4a & BIT(7));
	if (ret)
		goto cleanup;

	ret = it6664_read_byte(rx, 0x37, &reg37);
	if (ret)
		goto cleanup;
	have_reg37 = true;
	for (i = 0; i < ARRAY_SIZE(state->eq20.invalid_mask); i++) {
		ret = it6664_write_bits(rx, 0x37, GENMASK(7, 6), i << 6);
		if (ret)
			goto cleanup;
		ret = it6664_read_byte(rx, 0x63, &invalid_lo);
		if (ret)
			goto cleanup;
		ret = it6664_read_byte(rx, 0x64, &invalid_hi);
		if (ret)
			goto cleanup;
		ret = it6664_read_byte(rx, 0x6d, &amp_status[i]);
		if (ret)
			goto cleanup;
		ret = it6664_read_byte(rx, 0x6e, &ignored);
		if (ret)
			goto cleanup;
		state->eq20.invalid_mask[i] = invalid_lo | (invalid_hi << 8);
	}
	ret = it6664_write_bits(rx, 0x37, GENMASK(7, 6),
				reg37 & GENMASK(7, 6));
	if (ret)
		goto cleanup;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;

	if (state->eq20.invalid_mask[0] == 0x3fff ||
	    state->eq20.invalid_mask[1] == 0x3fff ||
	    state->eq20.invalid_mask[2] == 0x3fff)
		state->eq20.path = IT6664_RX_EQ20_INVALID;
	else if (amp_status[0] && amp_status[1] && amp_status[2])
		state->eq20.path = IT6664_RX_EQ20_RESTORE_SNAPSHOT;
	else
		state->eq20.path = IT6664_RX_EQ20_SCORE_AMP;

	return 0;

cleanup:
	if (bank_three) {
		if (have_reg37) {
			cleanup_ret = it6664_write_bits(rx, 0x37, GENMASK(7, 6), reg37);
			if (!ret)
				ret = cleanup_ret;
		}
		if (have_reg4a) {
			cleanup_ret = it6664_write_bits(rx, 0x4a, BIT(7), reg4a);
			if (!ret)
				ret = cleanup_ret;
		}
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static u8 it6664_abs_byte_diff_half(u8 left, u8 right)
{
	return left >= right ? (left - right) >> 1 : (right - left) >> 1;
}

static int
it6664_score_rx_eq20_lane(struct gc555_it6664 *it6664, unsigned int lane)
{
	static const u8 candidate_fixed[IT6664_RX_EQ20_CANDIDATE_COUNT] = {
		0x7f, 0x7e, 0x3f, 0x3e, 0x1f, 0x1e, 0x0f,
		0x0e, 0x07, 0x06, 0x03, 0x02, 0x01, 0x00,
	};
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 best_tuning[IT6664_RX_EQ20_TUNING_SIZE] = {};
	u8 best_primary = 0xff;
	u8 best_secondary = 0xff;
	u8 selected_raw = 0xff;
	u8 selected_index = 0xff;
	u8 center;
	u8 sample_a;
	u8 sample_b;
	u8 sample_c;
	u8 sum_ab;
	u8 sum_abc;
	u8 center_x2;
	u8 center_x3;
	u8 primary;
	u8 secondary;
	u8 delta_a;
	u8 delta_b;
	u8 delta_c;
	u8 ignored;
	u16 invalid_mask;
	unsigned int candidate;
	unsigned int i;
	int ret;

	if (lane >= IT6664_RX_EQ_LANE_COUNT)
		return -EINVAL;

	invalid_mask = state->eq20.invalid_mask[lane];
	ret = it6664_write_bits(rx, 0x37, GENMASK(7, 6), lane << 6);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, 0xd5 + lane, &ignored);
	if (ret)
		return ret;

	for (candidate = 0; candidate < ARRAY_SIZE(candidate_fixed);
	     candidate++) {
		ret = it6664_write_bits(rx, 0x36, GENMASK(3, 0), candidate);
		if (ret)
			return ret;
		ret = it6664_read_byte(rx, 0x5d, &center);
		if (ret)
			return ret;
		ret = it6664_read_byte(rx, 0x5e, &sample_a);
		if (ret)
			return ret;
		ret = it6664_read_byte(rx, 0x5f, &sample_b);
		if (ret)
			return ret;
		ret = it6664_read_byte(rx, 0x60, &sample_c);
		if (ret)
			return ret;

		sum_ab = sample_a + sample_b;
		sum_abc = sum_ab + sample_c;
		center_x2 = center * 2;
		center_x3 = center * 3;
		primary = it6664_abs_byte_diff_half(sum_ab, center_x2);
		secondary = it6664_abs_byte_diff_half(sum_abc, center_x3);
		delta_a = it6664_abs_byte_diff_half(sample_a, center);
		delta_b = it6664_abs_byte_diff_half(sample_b, center);
		delta_c = it6664_abs_byte_diff_half(sample_c, center);

		/* Any sample overflow tightens the shared delta-A term. */
		if (delta_a & 0xe0)
			delta_a = 0x1f;
		if (delta_b & 0xf0)
			delta_a = 0x0f;
		if (delta_c & 0xf8)
			delta_a = 0x07;

		if (!(invalid_mask & BIT(0)) &&
		    (primary < best_primary ||
		     (primary == best_primary && secondary <= best_secondary))) {
			selected_raw = candidate_fixed[candidate];
			selected_index = candidate;
			best_primary = primary;
			best_secondary = secondary;
			best_tuning[0] = 0x40 + delta_a +
				(sample_a < center ? 0x20 : 0);
			best_tuning[1] = 0x20 + delta_b +
				(sample_b < center ? 0x10 : 0);
			best_tuning[2] = 0x10 + delta_c +
				(sample_c < center ? 0x08 : 0);
		}

		memcpy(state->eq20.tuning[candidate][lane], best_tuning,
		       sizeof(best_tuning));
		/* Consume one validity bit per candidate. */
		invalid_mask = (invalid_mask >> 1) & 0x7f;
	}

	if (selected_index == 0xff)
		return -EAGAIN;
	for (i = 0x61; i <= 0x62; i++) {
		ret = it6664_read_byte(rx, i, &ignored);
		if (ret)
			return ret;
	}
	for (i = 0x6b; i <= 0x6c; i++) {
		ret = it6664_read_byte(rx, i, &ignored);
		if (ret)
			return ret;
	}

	state->eq20.seed[lane] = selected_raw ^ BIT(7);
	ret = regmap_write(rx, 0x27 + lane, state->eq20.seed[lane]);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(best_tuning); i++) {
		ret = regmap_write(rx,
				   0x4b + lane * IT6664_RX_EQ20_TUNING_SIZE + i,
				   best_tuning[i]);
		if (ret)
			return ret;
	}
	ret = it6664_write_bits(rx, 0x4b, BIT(7), BIT(7));
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x4a, BIT(7), BIT(7));
	if (ret)
		return ret;

	return it6664_write_bits(rx, 0x4a, BIT(7), 0);
}

static int it6664_finish_rx_eq20_result(struct gc555_it6664 *it6664)
{
	static const u8 restore_masks[IT6664_RX_EQ20_SNAPSHOT_SIZE] = {
		0x7f, 0x3f, 0x1f, 0x7f, 0x3f, 0x1f, 0x7f, 0x3f, 0x1f,
	};
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 value;
	bool bank_three = false;
	unsigned int i;
	int cleanup_ret;
	int ret;

	if (state->eq20.path == IT6664_RX_EQ20_INVALID)
		return -EAGAIN;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	for (i = 0; i < ARRAY_SIZE(state->eq20.seed); i++) {
		ret = it6664_read_byte(rx, 0xd5 + i, &value);
		if (ret)
			goto cleanup;
		if (i == IT6664_RX_EQ_LANE_COUNT - 1)
			state->eq20.seed[i] = (value & 0x7f) | BIT(7);
		else
			state->eq20.seed[i] = (value & 0x7f) ^ BIT(7);
	}

	if (state->eq20.path == IT6664_RX_EQ20_RESTORE_SNAPSHOT) {
		ret = it6664_write_bits(rx, 0x4a, BIT(7), 0);
		if (ret)
			goto cleanup;
		ret = it6664_write_bits(rx, 0x37, GENMASK(7, 6), BIT(7));
		if (ret)
			goto cleanup;
		for (i = 0; i < ARRAY_SIZE(restore_masks); i++) {
			ret = it6664_write_bits(rx, 0x4b + i, restore_masks[i],
						state->eq20.snapshot[i]);
			if (ret)
				goto cleanup;
		}
		ret = it6664_write_bits(rx, 0x4b, BIT(7), BIT(7));
		if (ret)
			goto cleanup;
	} else if (state->eq20.path == IT6664_RX_EQ20_SCORE_AMP) {
		ret = it6664_write_bits(rx, 0x4a, BIT(7), 0);
		if (ret)
			goto cleanup;
		for (i = 0; i < IT6664_RX_EQ_LANE_COUNT; i++) {
			ret = it6664_score_rx_eq20_lane(it6664, i);
			if (ret)
				goto cleanup;
		}
	} else {
		ret = -EAGAIN;
		goto cleanup;
	}

	ret = it6664_write_bits(rx, 0x22, 0x44, BIT(6));
	if (ret)
		goto cleanup;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;
	ret = it6664_write_bits(rx, 0x07, GENMASK(5, 4), GENMASK(5, 4));
	if (ret)
		return ret;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	for (i = 0; i < ARRAY_SIZE(state->eq20.seed); i++) {
		ret = regmap_write(rx, 0x27 + i, state->eq20.seed[i]);
		if (ret)
			goto cleanup;
	}
	ret = it6664_write_bits(rx, 0x22, BIT(6), BIT(6));
	if (ret)
		goto cleanup;
	for (i = 0; i < ARRAY_SIZE(state->eq20.readback); i++) {
		ret = it6664_read_byte(rx, 0x4b + i, &state->eq20.readback[i]);
		if (ret)
			goto cleanup;
	}
	ret = regmap_write(rx, 0xe9, BIT(7));
	if (ret)
		goto cleanup;
	usleep_range(10000, 11000);
	ret = regmap_write(rx, 0xe9, BIT(7));
	if (ret)
		goto cleanup;
	usleep_range(10000, 11000);
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;

	ret = regmap_write(sw, 0x0b, 0xff);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x0b, 0x00);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x4e, GENMASK(3, 0), GENMASK(3, 0));
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x0c, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x0c, BIT(3), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(sw, 0x4e, GENMASK(3, 0), 0);
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x53, BIT(5), BIT(5));
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x05, BIT(5), BIT(5));
	if (ret)
		return ret;

	state->eq_state = IT6664_RX_EQ_VALIDATE;
	state->eq_terminal_sampled = false;
	state->eq_terminal_valid = false;
	state->eq_recovery_needed = false;
	return 0;

cleanup:
	if (bank_three) {
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static int it6664_finish_rx_eq14_result(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 d0[3];
	u8 lane[3];
	u8 fixed[3];
	u8 selected;
	bool valid[3];
	bool bank_three = false;
	unsigned int i;
	int cleanup_ret;
	int ret;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	for (i = 0; i < ARRAY_SIZE(lane); i++) {
		ret = it6664_read_byte(rx, 0xd5 + i, &lane[i]);
		if (ret)
			goto cleanup;
		lane[i] &= 0x7f;
	}
	for (i = 0; i < ARRAY_SIZE(d0); i++) {
		ret = it6664_read_byte(rx, 0xd0, &d0[i]);
		if (ret)
			goto cleanup;
	}

	valid[0] = (d0[0] & 0x03) == 0x03;
	valid[1] = (d0[1] & 0x0c) == 0x0c;
	valid[2] = (d0[2] & 0x30) == 0x30;
	if (!valid[0] && !valid[1] && !valid[2]) {
		ret = -EAGAIN;
		goto cleanup;
	}

	selected = valid[0] ? lane[0] : valid[1] ? lane[1] : lane[2];
	for (i = 0; i < ARRAY_SIZE(fixed); i++)
		fixed[i] = (valid[i] ? lane[i] : selected) ^ BIT(7);

	ret = it6664_write_bits(rx, 0x22, 0x44, BIT(6));
	if (ret)
		goto cleanup;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;
	ret = it6664_write_bits(rx, 0x07, GENMASK(5, 4), GENMASK(5, 4));
	if (ret)
		return ret;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = it6664_write_bits(rx, 0x22, GENMASK(5, 3), 0);
	if (ret)
		goto cleanup;
	for (i = 0; i < ARRAY_SIZE(fixed); i++) {
		ret = regmap_write(rx, 0x27 + i, fixed[i]);
		if (ret)
			goto cleanup;
	}
	ret = regmap_write(rx, 0xe9, BIT(7));
	if (ret)
		goto cleanup;
	usleep_range(10000, 11000);
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;
	ret = it6664_write_bits(rx, 0x53, BIT(5), BIT(5));
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x05, BIT(5), BIT(5));
	if (ret)
		return ret;

	memcpy(state->eq_fixed, fixed, sizeof(state->eq_fixed));
	memset(state->eq_lane_failed, 0, sizeof(state->eq_lane_failed));
	state->eq14_retry_count = 0;
	state->eq14_running = false;
	state->eq14_done = true;
	state->eq_state = IT6664_RX_EQ_VALIDATE;
	state->eq_terminal_sampled = false;
	state->eq_terminal_valid = false;
	state->eq_recovery_needed = false;
	dev_dbg(it6664->gc555->dev,
		"IT6664 RX EQ14 result fixed=%02x/%02x/%02x\n",
		fixed[0], fixed[1], fixed[2]);
	return 0;

cleanup:
	if (bank_three) {
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static int it6664_read_rx_eq_irqs(struct regmap *rx, u8 irq[3])
{
	static const u8 regs[] = { 0xb9, 0xbe, 0xbf };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = it6664_read_byte(rx, regs[i], &irq[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int it6664_ack_rx_eq_irqs(struct regmap *rx)
{
	static const u8 regs[] = { 0xb9, 0xbe, 0xbf };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regmap_write(rx, regs[i], 0xff);
		if (ret)
			return ret;
	}

	return 0;
}

static int
it6664_sample_rx_bit_errors(struct gc555_it6664 *it6664,
			    bool require_lane_ready, bool *all_valid,
			    u8 *reg14_out)
{
	static const u8 lane_selectors[] = { 0x00, 0x20, 0x40 };
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 irq[3];
	u8 low;
	u8 high;
	u8 reg14;
	bool bank_three = false;
	unsigned int i;
	int cleanup_ret;
	int ret;

	ret = it6664_read_rx_eq_irqs(rx, irq);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS_14, &reg14);
	if (ret)
		return ret;
	*reg14_out = reg14;
	if (require_lane_ready && (reg14 & GENMASK(5, 3)) != GENMASK(5, 3))
		return -EAGAIN;

	ret = it6664_write_bits(rx, 0x3b, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = it6664_write_bits(rx, 0x55, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0xe9, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0xe9, 0x00);
	if (ret)
		goto cleanup;

	*all_valid = true;
	memset(state->eq_lane_failed, 0, sizeof(state->eq_lane_failed));
	for (i = 0; i < ARRAY_SIZE(lane_selectors); i++) {
		if (i) {
			ret = regmap_write(rx, 0xe9, lane_selectors[i]);
			if (ret)
				goto cleanup;
		}
		ret = it6664_read_byte(rx, 0xea, &low);
		if (ret)
			goto cleanup;
		ret = it6664_read_byte(rx, 0xeb, &high);
		if (ret)
			goto cleanup;

		state->eq_lane_failed[i] = !(high & BIT(7)) || low > 0x80;
		if (state->eq_lane_failed[i]) {
			*all_valid = false;
			continue;
		}
		ret = it6664_read_byte(rx, 0x27 + i, &state->eq_fixed[i]);
		if (ret)
			goto cleanup;
	}

	ret = regmap_write(rx, 0xe9, BIT(7));
	if (ret)
		goto cleanup;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;

	return it6664_ack_rx_eq_irqs(rx);

cleanup:
	if (bank_three) {
		regmap_write(rx, 0xe9, BIT(7));
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static int it6664_recover_rx_eq20_skew(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 reg07;
	u8 value;
	bool bank_three = false;
	bool skew_started = false;
	unsigned int waits = 0;
	unsigned int lane;
	unsigned int reg;
	int cleanup_ret;
	int ret;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	skew_started = true;
	ret = it6664_write_bits(rx, 0x22, BIT(2), 0);
	if (ret)
		goto cleanup;
	for (lane = 0; lane < ARRAY_SIZE(state->eq20.seed); lane++) {
		ret = regmap_write(rx, 0x27 + lane, state->eq20.seed[lane]);
		if (ret)
			goto cleanup;
	}
	ret = regmap_write(rx, 0x2d, 0x00);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x30, 0x94);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x31, 0xb0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x37, BIT(4), BIT(4));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x54, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x22, BIT(2), BIT(2));
	if (ret)
		goto cleanup;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;

	ret = it6664_read_byte(rx, 0x07, &reg07);
	if (ret)
		goto cleanup;
	while (!(reg07 & BIT(4)) && waits < 1000) {
		usleep_range(1000, 2000);
		waits++;
		ret = it6664_read_byte(rx, 0x07, &reg07);
		if (ret)
			goto cleanup;
	}
	ret = it6664_write_bits(rx, 0x07, BIT(4), BIT(4));
	if (ret)
		goto cleanup;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		goto cleanup;
	bank_three = true;
	ret = it6664_write_bits(rx, 0x37, BIT(4), 0);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x22, BIT(2), 0);
	if (ret)
		goto cleanup;
	ret = it6664_read_byte(rx, 0x74, &value);
	if (ret)
		goto cleanup;
	ret = it6664_read_byte(rx, 0x74, &state->eq20.skew_status);
	if (ret)
		goto cleanup;
	for (lane = 0; lane < IT6664_RX_EQ_LANE_COUNT; lane++) {
		ret = it6664_write_bits(rx, 0x37, GENMASK(7, 6), lane << 6);
		if (ret)
			goto cleanup;
		for (reg = 0x75; reg <= 0x7a; reg++) {
			ret = it6664_read_byte(rx, reg, &value);
			if (ret)
				goto cleanup;
		}
	}
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;

	return 0;

cleanup:
	if (skew_started) {
		if (!bank_three) {
			cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
			bank_three = !cleanup_ret;
		}
		if (bank_three) {
			it6664_write_bits(rx, 0x37, BIT(4), 0);
			it6664_write_bits(rx, 0x22, BIT(2), 0);
			it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		}
	} else if (bank_three) {
		it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	}

	return ret;
}

static int it6664_reset_and_reseed_rx_eq(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 reg13;
	u8 reg14;
	bool bank_three = false;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(rx, 0x07, GENMASK(7, 4), GENMASK(7, 4));
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x23, BIT(4), BIT(4));
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = it6664_write_bits(rx, 0x23, BIT(4), 0);
	if (ret)
		return ret;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = regmap_write(rx, 0x20, 0x1b);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x21, 0x03);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x22, 0x00);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0x4b, BIT(7), 0);
	if (ret)
		goto cleanup;
	ret = regmap_write(rx, 0x2d, 0x00);
	if (ret)
		goto cleanup;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;
	ret = it6664_write_bits(rx, 0x53, BIT(5), BIT(5));
	if (ret)
		return ret;
	ret = it6664_write_bits(rx, 0x05, BIT(5), BIT(5));
	if (ret)
		return ret;

	state->eq14_running = false;
	state->eq20_done = false;
	state->eq20_running = false;
	state->eq20.path = IT6664_RX_EQ20_NONE;
	state->eq_state = IT6664_RX_EQ_IDLE;
	state->eq_postcheck_delay = 0;
	state->eq_terminal_sampled = false;
	state->eq_terminal_valid = false;
	state->eq_recovery_needed = false;

	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS, &reg13);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS_14, &reg14);
	if (ret)
		return ret;
	if ((reg13 & BIT(4)) && (reg14 & BIT(6)))
		state->eq_state = IT6664_RX_EQ_START;

	return 0;

cleanup:
	if (bank_three) {
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static bool
it6664_rx_irq_is_mode_class_change(
		const struct it6664_rx_state *state,
		const struct it6664_rx_irq *irq)
{
	return state->signal_started && state->eq_terminal_sampled &&
	       (irq->reg13 & BIT(4)) &&
	       ((state->eq_terminal_reg14 ^ irq->reg14) & BIT(6));
}

static int
it6664_rearm_rx_mode_class(struct gc555_it6664 *it6664,
			   const struct it6664_rx_irq *irq)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	u8 previous_reg14 = state->eq_terminal_reg14;
	int ret;

	ret = it6664_handle_rx_scdt_loss(it6664);
	if (ret)
		return ret;
	ret = it6664_reset_and_reseed_rx_eq(it6664);
	if (ret)
		return ret;

	state->eq14_done = false;
	state->eq14_retry_count = 0;
	state->eq_validate_not_ready = 0;
	state->eq_validate_recoveries = 0;
	state->eq_monitor_not_ready = 0;
	state->eq_monitor_recoveries = 0;
	state->eq_manual_lane2_index = 0;
	state->eq_manual_lane2_runs = 0;
	state->eq_terminal_reg14 = 0;
	state->mode_rearm_pending = true;
	state->eq_state = IT6664_RX_EQ_START;

	dev_dbg(it6664->gc555->dev,
		"IT6664 RX mode class rearmed reg14=%02x->%02x\n",
		previous_reg14, irq->reg14);
	return 0;
}

static int it6664_validate_rx_eq(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	bool all_valid;
	u8 reg14;
	int ret;

	ret = it6664_sample_rx_bit_errors(it6664, true, &all_valid, &reg14);
	if (ret == -EAGAIN) {
		if (state->eq20_running && (reg14 & BIT(6))) {
			if (state->eq_validate_not_ready != U8_MAX)
				state->eq_validate_not_ready++;
			if (state->eq_validate_not_ready >=
			    IT6664_RX_EQ_NOT_READY_LIMIT &&
			    state->eq_validate_recoveries <
			    IT6664_RX_EQ_RECOVERY_LIMIT) {
				state->eq_validate_not_ready = 0;
				state->eq_validate_recoveries++;
				return it6664_reset_and_reseed_rx_eq(it6664);
			}
		} else {
			state->eq_validate_not_ready = 0;
		}
		return 0;
	}
	if (ret)
		return ret;
	state->eq_validate_not_ready = 0;
	if (state->eq20_running) {
		if ((reg14 & GENMASK(6, 3)) != GENMASK(6, 3))
			return 0;
		if (!all_valid) {
			ret = it6664_recover_rx_eq20_skew(it6664);
			if (ret)
				return ret;
		}
		state->eq20_done = true;
		state->eq20_running = false;
	} else if (reg14 & BIT(6)) {
		return 0;
	}

	state->eq_state = IT6664_RX_EQ_MONITOR;
	state->eq_postcheck_delay = 2;
	state->eq_monitor_not_ready = 0;
	state->eq_manual_lane2_index = 0;
	state->eq_manual_lane2_runs = 0;
	state->eq_terminal_sampled = false;
	state->eq_terminal_valid = false;
	state->eq_recovery_needed = false;
	return 0;
}

static int it6664_adjust_rx_eq20_lane2(struct gc555_it6664 *it6664)
{
	static const u8 fixed_values[IT6664_RX_EQ_MANUAL_CANDIDATE_COUNT] = {
		0x8f, 0x86, 0x83, 0x81, 0x80, 0x9f, 0xbf,
	};
	static const u8 tuning_indices[IT6664_RX_EQ_MANUAL_CANDIDATE_COUNT] = {
		0x06, 0x09, 0x0a, 0x0c, 0x0d, 0x04, 0x02,
	};
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	const u8 *tuning;
	u8 ignored;
	u8 index;
	u8 next_index;
	bool bank_three = false;
	unsigned int i;
	int cleanup_ret;
	int ret;

	index = state->eq_manual_lane2_index;
	if (index >= ARRAY_SIZE(fixed_values) ||
	    state->eq_manual_lane2_runs >= ARRAY_SIZE(fixed_values))
		return -ERANGE;
	tuning = state->eq20.tuning[tuning_indices[index]][2];
	state->eq_manual_lane2_runs++;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_3);
	if (ret)
		return ret;
	bank_three = true;
	ret = regmap_write(rx, 0x29, fixed_values[index]);
	if (ret)
		goto cleanup;
	ret = it6664_read_byte(rx, 0x29, &ignored);
	if (ret)
		goto cleanup;
	next_index = index + 1;
	if (next_index >= ARRAY_SIZE(fixed_values))
		next_index = 0;
	state->eq_manual_lane2_index = next_index;
	for (i = 0; i < IT6664_RX_EQ20_TUNING_SIZE; i++) {
		ret = regmap_write(rx, 0x51 + i, tuning[i]);
		if (ret)
			goto cleanup;
	}
	ret = regmap_write(rx, 0xe9, BIT(7));
	if (ret)
		goto cleanup;
	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto cleanup;
	bank_three = false;
	ret = it6664_ack_rx_eq_irqs(rx);
	if (ret)
		return ret;

	state->eq_terminal_sampled = false;
	state->eq_terminal_valid = false;
	state->eq_recovery_needed = false;
	return 0;

cleanup:
	if (bank_three) {
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
	}

	return ret;
}

static int it6664_monitor_rx_eq(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 irq[3];
	u8 sampled_reg14;
	u8 reg13;
	u8 reg14;
	bool all_valid;
	int ret;

	if (state->eq_terminal_sampled)
		return 0;
	if (state->eq_postcheck_delay) {
		state->eq_postcheck_delay--;
		return 0;
	}

	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS_14, &reg14);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS, &reg13);
	if (ret)
		return ret;
	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS, &reg13);
	if (ret)
		return ret;
	if (!(reg13 & BIT(4)))
		return 0;

	ret = it6664_read_rx_eq_irqs(rx, irq);
	if (ret)
		return ret;
	if ((reg14 & GENMASK(5, 3)) != GENMASK(5, 3)) {
		ret = it6664_ack_rx_eq_irqs(rx);
		if (ret)
			return ret;
		if (state->eq20_done && (reg14 & BIT(6))) {
			if (state->eq_monitor_not_ready != U8_MAX)
				state->eq_monitor_not_ready++;
			if (state->eq_monitor_not_ready >=
			    IT6664_RX_EQ_NOT_READY_LIMIT &&
			    state->eq_monitor_recoveries <
			    IT6664_RX_EQ_RECOVERY_LIMIT) {
				state->eq_monitor_not_ready = 0;
				state->eq_monitor_recoveries++;
				return it6664_reset_and_reseed_rx_eq(it6664);
			}
		} else {
			state->eq_monitor_not_ready = 0;
		}
		return 0;
	}
	state->eq_monitor_not_ready = 0;

	ret = it6664_sample_rx_bit_errors(it6664, false, &all_valid, &sampled_reg14);
	if (ret)
		return ret;

	state->eq_terminal_sampled = true;
	state->eq_terminal_valid = all_valid;
	state->eq_terminal_reg14 = sampled_reg14;
	state->mode_rearm_pending = false;
	state->eq_recovery_needed = !all_valid &&
		(irq[0] || irq[1] || irq[2] || (reg14 & BIT(6)));
	if (all_valid) {
		state->eq_validate_recoveries = 0;
		state->eq_monitor_recoveries = 0;
	}
	dev_dbg(it6664->gc555->dev,
		"IT6664 RX EQ terminal status=%02x/%02x valid=%u recovery=%u\n",
		reg13, sampled_reg14, state->eq_terminal_valid,
		state->eq_recovery_needed);
	if (all_valid && !state->scdt) {
		u8 reg19;

		ret = it6664_read_byte(rx, 0x19, &reg19);
		if (ret)
			return ret;
		if (reg19 & BIT(7)) {
			dev_dbg(it6664->gc555->dev,
				"IT6664 RX SCDT level replay status=%02x/%02x/%02x\n",
				reg13, sampled_reg14, reg19);
			return it6664_handle_rx_scdt_lock(it6664);
		}
	}
	if (state->eq_recovery_needed && state->eq20_done &&
	    (reg14 & BIT(6)) &&
	    state->eq20.path == IT6664_RX_EQ20_RESTORE_SNAPSHOT &&
	    !state->eq_lane_failed[0] && !state->eq_lane_failed[1] &&
	    state->eq_lane_failed[2] &&
	    state->eq_manual_lane2_runs < IT6664_RX_EQ_MANUAL_CANDIDATE_COUNT)
		return it6664_adjust_rx_eq20_lane2(it6664);

	return 0;
}

static int it6664_rx_autoeq_poll(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 reg13;
	u8 reg14;
	int ret;

	if (!state->irq12_handled)
		return 0;
	if (state->eq_state == IT6664_RX_EQ_READ_RESULT) {
		if (state->eq20_running) {
			ret = it6664_measure_rx_eq20(it6664);
			if (!ret)
				ret = it6664_finish_rx_eq20_result(it6664);
		} else {
			ret = it6664_finish_rx_eq14_result(it6664);
		}
		return ret == -EAGAIN ? 0 : ret;
	}
	if (state->eq_state == IT6664_RX_EQ_VALIDATE)
		return it6664_validate_rx_eq(it6664);
	if (state->eq_state == IT6664_RX_EQ_MONITOR)
		return it6664_monitor_rx_eq(it6664);
	if (state->eq_state != IT6664_RX_EQ_START)
		return 0;

	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS, &reg13);
	if (ret)
		return ret;
	if (!(reg13 & BIT(4)))
		return 0;
	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS_14, &reg14);
	if (ret)
		return ret;

	if (reg14 & BIT(6)) {
		if (state->eq20_done || state->eq20_running)
			return 0;
		return it6664_start_rx_eq20(it6664, reg14);
	}
	if (state->eq14_done || state->eq14_running)
		return 0;

	return it6664_start_rx_eq14(it6664);
}

static int
it6664_handle_rx_signal_start(struct gc555_it6664 *it6664,
			      const struct it6664_rx_irq *irq)
{
	struct it6664_runtime *runtime = &it6664->runtime;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 rx_sense_mask = 0;
	u8 status;
	unsigned int port;
	int ret;

	runtime->rx.signal_started = false;
	runtime->rx.irq12_handled = false;
	runtime->rx.converter_output_mode_request = 0;
	runtime->rx.converter_output_mode = 0;
	runtime->rx.csc_output_mode = 0;
	runtime->rx.csc_output_quantization = 0;
	memset(&runtime->rx.video_timing, 0,
	       sizeof(runtime->rx.video_timing));

	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS, &status);
	if (ret)
		return ret;
	it6664->runtime.rx.bus_mode = (status >> 5) & 0x02;

	ret = it6664_handle_rx_signal_irq(it6664);
	if (ret)
		return ret;
	ret = it6664_handle_rx_eq_start(it6664, irq);
	if (ret)
		return ret;
	ret = it6664_read_rx_bank2_status(it6664);
	if (ret)
		return ret;
	for (port = 0; port < IT6664_TX_PORT_COUNT; port++)
		if (runtime->tx[port].rx_sense)
			rx_sense_mask |= BIT(port);
	runtime->rx.signal_started = true;

	dev_dbg(it6664->gc555->dev,
		"IT6664 RX signal-start IRQ handled irq=%02x/%02x/%02x pending12=%02x bus=%u timer=%u senses=%x eq=%u\n",
		irq->reg05, irq->reg06, irq->reg07, irq->reg12,
		runtime->rx.bus_mode, runtime->rx.timer_state, rx_sense_mask,
		runtime->rx.eq_state);

	return 0;
}

static int
it6664_handle_rx_reg11(struct gc555_it6664 *it6664,
		       const struct it6664_rx_irq *irq)
{
	static const u8 status_regs[] = { 0x2a, 0x29, 0x28, 0x2c };
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	unsigned int i;
	u8 depth;
	u8 value;
	int cleanup_ret;
	int ret;

	if (irq->reg11 & IT6664_RX_IRQ11_BANK2_STATUS) {
		ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_2);
		if (ret)
			return ret;
		for (i = 0; i < ARRAY_SIZE(status_regs); i++) {
			ret = it6664_read_byte(rx, status_regs[i], &value);
			if (ret)
				break;
		}
		cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (!ret)
			ret = cleanup_ret;
		if (ret)
			return ret;
	}

	if (!(irq->reg11 & IT6664_RX_IRQ11_COLOR_DEPTH))
		return 0;

	ret = it6664_read_byte(rx, 0x98, &depth);
	if (ret)
		return ret;

	it6664->runtime.rx.color_depth = depth >> 4;
	return 0;
}

static int
it6664_handle_rx_signal_restart(struct gc555_it6664 *it6664,
				const struct it6664_rx_irq *irq)
{
	struct it6664_rx_irq reg12_irq = *irq;
	int ret;

	ret = it6664_handle_rx_signal_start(it6664, irq);
	if (ret)
		return ret;
	ret = it6664_handle_rx_scdt_irq(it6664, irq);
	if (ret)
		return ret;
	if (!(irq->reg10 & IT6664_RX_IRQ10_SCDT_CHANGE) &&
	    (irq->reg14 & GENMASK(5, 3)) == GENMASK(5, 3) &&
	    (irq->reg19 & BIT(7))) {
		ret = it6664_handle_rx_scdt_lock(it6664);
		if (ret)
			return ret;
	}
	ret = it6664_handle_rx_reg11(it6664, irq);
	if (ret)
		return ret;

	reg12_irq.reg12 &= IT6664_RX_IRQ12_SUPPORTED;
	if (reg12_irq.reg12)
		return it6664_handle_rx_reg12(it6664, &reg12_irq);

	return 0;
}

static int
it6664_handle_rx_coalesced_stable(struct gc555_it6664 *it6664,
				  const struct it6664_rx_irq *irq)
{
	int ret;

	if (irq->reg06 & BIT(0)) {
		ret = it6664_handle_rx_eq_start(it6664, irq);
		if (ret)
			return ret;
	}

	if (irq->reg07 & IT6664_RX_IRQ07_EQ_RESULT) {
		ret = it6664_handle_rx_eq_result_irq(it6664, irq);
		if (ret)
			return ret;
	}
	if (irq->reg07 & IT6664_RX_IRQ07_BANK2_STATUS) {
		ret = it6664_read_rx_bank2_status(it6664);
		if (ret)
			return ret;
	}

	if (irq->reg10 & IT6664_RX_IRQ10_ACTIONS) {
		ret = it6664_handle_rx_scdt_irq(it6664, irq);
		if (ret)
			return ret;
	}

	ret = it6664_handle_rx_reg11(it6664, irq);
	if (ret)
		return ret;

	if (irq->reg12)
		return it6664_handle_rx_reg12(it6664, irq);

	return 0;
}

static int it6664_rx_poll(struct gc555_it6664 *it6664)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct it6664_rx_irq irq = {};
	bool signal_start_event;
	bool eq_result_event;
	bool mode_class_change;
	bool reg12_event;
	bool scdt_event;
	bool stable_coalesced_event;
	bool signal_restart_event;
	bool handled = true;
	u8 common;
	int ret;

	ret = it6664_read_byte(sw, IT6664_SWITCH_REG_IRQ_STATUS, &common);
	if (ret || !(common & IT6664_SWITCH_IRQ_RX))
		return ret;
	ret = it6664_read_rx_irq(it6664, &irq);
	if (ret)
		return ret;
	mode_class_change =
		it6664_rx_irq_is_mode_class_change(&it6664->runtime.rx, &irq);
	signal_start_event =
		it6664_rx_irq_is_signal_start(&it6664->runtime.rx, &irq);
	signal_restart_event =
		it6664_rx_irq_is_signal_restart(&it6664->runtime.rx, &irq);
	if (!signal_start_event && !signal_restart_event &&
	    !it6664_rx_irq_supported(&irq))
		goto deferred;
	reg12_event = it6664_rx_irq_is_reg12(&it6664->runtime.rx, &irq);
	eq_result_event =
		it6664_rx_irq_is_eq_result(&it6664->runtime.rx, &irq);
	scdt_event = it6664_rx_irq_is_scdt(&irq);
	stable_coalesced_event =
		it6664_rx_irq_is_coalesced_stable(&it6664->runtime.rx, &irq);

	if (mode_class_change) {
		ret = it6664_rearm_rx_mode_class(it6664, &irq);
	} else if (signal_restart_event || signal_start_event) {
		ret = it6664_handle_rx_signal_restart(it6664, &irq);
	} else if (irq.reg05 & IT6664_RX_IRQ_SOURCE_CHANGE) {
		if (irq.reg13 & BIT(0))
			ret = it6664_handle_rx_detect_bus(it6664, &irq);
		else
			ret = it6664_handle_rx_source_loss(it6664, &irq);
	} else if (reg12_event) {
		ret = it6664_handle_rx_reg12(it6664, &irq);
	} else if (stable_coalesced_event) {
		ret = it6664_handle_rx_coalesced_stable(it6664, &irq);
	} else if (eq_result_event) {
		ret = it6664_handle_rx_eq_result_irq(it6664, &irq);
	} else if (scdt_event) {
		ret = it6664_handle_rx_scdt_irq(it6664, &irq);
	} else if (it6664_rx_irq_is_noop(&irq)) {
		ret = 0;
	} else {
		handled = false;
		ret = 0;
	}
	if (ret)
		return ret;
	if (!handled)
		goto deferred;

	ret = it6664_ack_rx_irq(it6664, &irq);
	if (ret)
		return ret;
	if (irq.reg12)
		return it6664_ack_rx_reg12(it6664, &irq);

	return 0;

deferred:
	dev_dbg_ratelimited(it6664->gc555->dev,
			    "IT6664 RX IRQ deferred common=%02x irq=%02x/%02x/%02x/%02x/%02x/%02x/%02x status=%02x/%02x/%02x/%02x\n",
			    common, irq.reg05, irq.reg06, irq.reg07,
			    irq.reg08, irq.reg09, irq.reg10, irq.reg11,
			    irq.reg12, irq.reg13, irq.reg14, irq.reg19);
	return 0;
}

static int
it6664_write_upstream_edid(struct gc555_it6664 *it6664, const u8 *edid)
{
	struct regmap *edid_ram =
		it6664->maps[IT6664_MAP_RX_EDID_RAM].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	unsigned int i;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(rx, IT6664_RX_REG_EDID_ENABLE, BIT(0), 0);
	if (ret)
		return ret;
	ret = regmap_write(rx, IT6664_RX_REG_EDID_ADDRESS_HI, 0x00);
	if (ret)
		goto close_gate;
	ret = regmap_write(rx, IT6664_RX_REG_EDID_ADDRESS_LO, 0x10);
	if (ret)
		goto close_gate;
	ret = regmap_write(rx, IT6664_RX_REG_EDID_PORT, 0x00);
	if (ret)
		goto close_gate;
	ret = regmap_write(rx, 0x4b, 0xd9);
	if (ret)
		goto close_gate;

	for (i = 0; i < IT6664_EDID_BLOCK_SIZE - 1; i++) {
		ret = regmap_write(edid_ram, i, edid[i]);
		if (ret)
			goto close_gate;
	}
	ret = regmap_write(rx, IT6664_RX_REG_EDID_CHECKSUM0,
			   edid[IT6664_EDID_BLOCK_SIZE - 1]);
	if (ret)
		goto close_gate;

	for (i = 0; i < IT6664_EDID_BLOCK_SIZE - 1; i++) {
		ret = regmap_write(edid_ram, i | IT6664_EDID_BLOCK_SIZE,
				   edid[IT6664_EDID_BLOCK_SIZE + i]);
		if (ret)
			goto close_gate;
	}
	ret = regmap_write(rx, IT6664_RX_REG_EDID_CHECKSUM1,
			   edid[IT6664_EDID_SIZE - 1]);

close_gate:
	cleanup_ret = it6664_write_bits(rx, IT6664_RX_REG_EDID_ENABLE,
					BIT(0), BIT(0));
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int
it6664_apply_upstream_edid(struct gc555_it6664 *it6664, const u8 *edid)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	bool bank_one = false;
	bool rx_gate = false;
	int cleanup_ret;
	int ret;

	if (!edid)
		return -EINVAL;

	ret = it6664_write_upstream_edid(it6664, edid);
	if (ret)
		goto cleanup;

	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK,
				BIT(0), BIT(0));
	if (ret)
		goto cleanup;
	bank_one = true;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_10,
				BIT(6), BIT(6));
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), 0);
	if (ret)
		goto cleanup;
	bank_one = false;

	ret = it6664_write_bits(rx, 0xc5, BIT(0), BIT(0));
	if (ret)
		goto cleanup;
	rx_gate = true;
	ret = gc555_it6664_rx_set_hpd(it6664, false);
	if (ret)
		goto cleanup;
	ret = it6664_quiesce_rx(it6664);
	if (ret)
		goto cleanup;
	msleep(500);
	ret = gc555_it6664_rx_set_hpd(it6664, true);
	if (ret)
		goto cleanup;
	ret = it6664_write_bits(rx, 0xc5, BIT(0), 0);
	if (ret)
		goto cleanup;
	rx_gate = false;
	ret = it6664_write_bits(rx, IT6664_RX_REG_EDID_ENABLE,
				BIT(0), BIT(0));
	if (ret)
		goto cleanup;

cleanup:
	if (bank_one) {
		cleanup_ret =
			it6664_write_bits(sw, IT6664_SWITCH_REG_BANK,
					  BIT(0), 0);
		if (!ret)
			ret = cleanup_ret;
	}
	if (rx_gate) {
		cleanup_ret = it6664_write_bits(rx, 0xc5, BIT(0), 0);
		if (!ret)
			ret = cleanup_ret;
	}
	if (ret)
		it6664_write_bits(rx, IT6664_RX_REG_EDID_ENABLE,
				  BIT(0), BIT(0));
	if (ret)
		gc555_it6664_rx_set_hpd(it6664, false);

	return ret;
}

static int
it6664_publish_upstream_edid(struct gc555_it6664 *it6664, const u8 *edid,
			     enum it6664_upstream_edid source)
{
	struct device *dev = it6664->gc555->dev;
	u8 previous[IT6664_EDID_SIZE];
	bool previous_valid;
	int rollback_ret;
	int ret;

	if (!edid)
		return -EINVAL;
	if (source != IT6664_UPSTREAM_EDID_FIXED &&
	    source != IT6664_UPSTREAM_EDID_MERGED)
		return -EINVAL;

	mutex_lock(&it6664->input_edid_lock);
	previous_valid = it6664->input_edid_valid;
	if (previous_valid)
		memcpy(previous, it6664->input_edid, sizeof(previous));
	mutex_unlock(&it6664->input_edid_lock);

	ret = it6664_apply_upstream_edid(it6664, edid);
	if (ret) {
		if (previous_valid) {
			rollback_ret =
				it6664_apply_upstream_edid(it6664, previous);
			if (rollback_ret)
				dev_err_ratelimited(dev, "EDID restore failed: %d\n",
						    rollback_ret);
		}
		return ret;
	}

	it6664->runtime.tx[1].edid_parsed = true;
	it6664->runtime.tx[1].edid_attempted = true;
	it6664->runtime.tx[1].dvi_mode = false;
	it6664->runtime.upstream_edid = source;
	mutex_lock(&it6664->input_edid_lock);
	memcpy(it6664->input_edid, edid, sizeof(it6664->input_edid));
	it6664->input_edid_valid = true;
	mutex_unlock(&it6664->input_edid_lock);
	dev_info(it6664->gc555->dev,
		 "IT6664 %s upstream EDID published checksums=%02x/%02x\n",
		source == IT6664_UPSTREAM_EDID_MERGED ? "merged" : "fixed",
		edid[IT6664_EDID_BLOCK_SIZE - 1],
		edid[IT6664_EDID_SIZE - 1]);

	return 0;
}

static int it6664_publish_fixed_edid(struct gc555_it6664 *it6664)
{
	const u8 *edid;
	size_t edid_size;
	int ret;

	ret = gc555_edid_get(&edid, &edid_size);
	if (ret)
		return ret;
	if (edid_size != IT6664_EDID_SIZE)
		return -EINVAL;

	return it6664_publish_upstream_edid(it6664, edid,
					     IT6664_UPSTREAM_EDID_FIXED);
}

static int it6664_publish_merged_edid(struct gc555_it6664 *it6664)
{
	struct it6664_runtime *runtime = &it6664->runtime;

	return it6664_publish_upstream_edid(it6664, runtime->merged_edid,
					     IT6664_UPSTREAM_EDID_MERGED);
}

static u8 it6664_hdcp_count_next(u8 count)
{
	if (count < IT6664_HDCP_DEBOUNCE_SAMPLES)
		count++;

	return count;
}

static void
it6664_update_source_hdcp(struct it6664_rx_state *state,
			 enum gc555_hdcp_level raw_level)
{
	u8 count;

	state->source_hdcp_raw_level = raw_level;
	switch (raw_level) {
	case GC555_HDCP_NONE:
		state->source_hdcp_none_count =
			it6664_hdcp_count_next(state->source_hdcp_none_count);
		state->source_hdcp_1x_count = 0;
		state->source_hdcp_2x_count = 0;
		count = state->source_hdcp_none_count;
		break;
	case GC555_HDCP_1X:
		state->source_hdcp_none_count = 0;
		state->source_hdcp_1x_count =
			it6664_hdcp_count_next(state->source_hdcp_1x_count);
		state->source_hdcp_2x_count = 0;
		count = state->source_hdcp_1x_count;
		break;
	case GC555_HDCP_2X:
		state->source_hdcp_none_count = 0;
		state->source_hdcp_1x_count = 0;
		state->source_hdcp_2x_count =
			it6664_hdcp_count_next(state->source_hdcp_2x_count);
		count = state->source_hdcp_2x_count;
		break;
	default:
		return;
	}

	if (count >= IT6664_HDCP_DEBOUNCE_SAMPLES) {
		state->source_hdcp_level = raw_level;
		state->source_hdcp_valid = true;
	}

	/*
	 * The blob's capture flag starts clear and only rises when the
	 * receiver reports HDCP. An unconfirmed sample must not pretend
	 * the link is protected.
	 */
	if (raw_level != GC555_HDCP_NONE)
		WRITE_ONCE(state->source_hdcp_effective_level, raw_level);
	else if (state->source_hdcp_valid)
		WRITE_ONCE(state->source_hdcp_effective_level, GC555_HDCP_NONE);
}

static int it6664_sample_source_hdcp(struct gc555_it6664 *it6664)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	enum gc555_hdcp_level raw_level;
	u8 status;
	u8 version;
	int ret;

	ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
	if (ret)
		goto fail_closed;
	ret = it6664_read_byte(rx, IT6664_RX_REG_HDCP_STATUS, &status);
	if (ret)
		goto fail_closed;
	if (!(status & BIT(0))) {
		raw_level = GC555_HDCP_NONE;
	} else {
		ret = it6664_read_byte(rx, IT6664_RX_REG_HDCP_VERSION,
					&version);
		if (ret)
			goto fail_closed;
		raw_level = version & BIT(6) ?
			GC555_HDCP_2X : GC555_HDCP_1X;
	}

	state->source_hdcp_last_error = 0;
	it6664_update_source_hdcp(state, raw_level);
	return 0;

fail_closed:
	state->source_hdcp_last_error = ret;
	WRITE_ONCE(state->source_hdcp_effective_level, GC555_HDCP_1X);
	return ret;
}

static void it6664_seed_tx_hdcp_check(struct gc555_it6664 *it6664)
{
	unsigned int port;

	for (port = 0; port < IT6664_TX_PORT_COUNT; port++) {
		struct it6664_tx_port_state *state = &it6664->runtime.tx[port];

		if (state->hpd && !state->hdcp_done)
			state->hdcp_state = IT6664_TX_HDCP_CHECK;
	}
}

static void it6664_reset_tx_hdcp_states(struct gc555_it6664 *it6664)
{
	unsigned int port;

	for (port = 0; port < IT6664_TX_PORT_COUNT; port++) {
		struct it6664_tx_port_state *state = &it6664->runtime.tx[port];

		state->hdcp_done = false;
		state->hdcp2_done = false;
		state->hdcp_state = IT6664_TX_HDCP_RESET;
	}
}

static int it6664_reset_shared_hdcp_engine(struct regmap *sw)
{
	int enable_ret;
	int ret;

	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_HDCP_CONTROL, BIT(0), 0);
	if (!ret)
		ret = regmap_write(sw, IT6664_SWITCH_REG_HDCP_IRQ_MASK, 0x0f);
	if (!ret)
		ret = regmap_write(sw, IT6664_SWITCH_REG_HDCP_CONFIG, 0xaf);
	if (!ret)
		ret = regmap_write(sw, IT6664_SWITCH_REG_HDCP_IRQ_MASK, 0x3f);
	if (!ret)
		ret = regmap_write(sw, IT6664_SWITCH_REG_IRQ7, 0xff);
	enable_ret = it6664_write_bits(sw, IT6664_SWITCH_REG_HDCP_CONTROL, BIT(0), BIT(0));
	if (!ret)
		ret = enable_ret;

	return ret;
}

static int
it6664_read_switch_irq(struct gc555_it6664 *it6664, struct it6664_switch_irq *irq)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	int cleanup_ret;
	int ret;

	ret = it6664_read_byte(sw, IT6664_SWITCH_REG_IRQ6, &irq->irq6);
	if (ret)
		return ret;
	ret = it6664_read_byte(sw, IT6664_SWITCH_REG_IRQ7, &irq->irq7);
	if (ret)
		return ret;

	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = it6664_read_byte(sw, 0x21, &irq->irq21);
	if (ret)
		goto cleanup;
	ret = it6664_read_byte(sw, 0x22, &irq->irq22);
	if (ret)
		goto cleanup;
	ret = it6664_read_byte(sw, 0x23, &irq->irq23);

cleanup:
	cleanup_ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), 0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int
it6664_read_switch_encryption_status(struct gc555_it6664 *it6664,
				     bool *enabled)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	u8 status;
	int cleanup_ret;
	int ret;

	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = it6664_read_byte(sw, 0x23, &status);
	cleanup_ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), 0);
	if (!ret)
		ret = cleanup_ret;
	if (!ret)
		*enabled = status & BIT(4);

	return ret;
}

static int
it6664_handle_switch_timer0(struct gc555_it6664 *it6664,
			    struct it6664_switch_irq_pending *pending)
{
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 restore = it6664->runtime.rx.bus_mode == 2 ? 0x0c : 0;
	int ret;

	if (pending->timer0_stage == 0) {
		ret = it6664_write_bits(sw, 0x1a, BIT(0), 0);
		if (ret)
			return ret;
		ret = regmap_write(sw, 0x19, 0x0f);
		if (ret)
			return ret;
		ret = regmap_write(sw, 0x1c, 0x00);
		if (ret)
			return ret;
		ret = regmap_write(rx, 0x26, 0xff);
		if (ret)
			return ret;
		ret = gc555_it6664_rx_set_hpd(it6664, false);
		if (ret)
			return ret;
		pending->timer0_stage = 1;
	}
	if (pending->timer0_stage == 1) {
		msleep(100);
		pending->timer0_stage = 2;
	}
	if (pending->timer0_stage == 2) {
		ret = gc555_it6664_rx_set_hpd(it6664, true);
		if (ret)
			return ret;
		pending->timer0_stage = 3;
	}
	if (pending->timer0_stage == 3) {
		ret = regmap_write(rx, 0x26, restore);
		if (ret)
			return ret;
		pending->timer0_stage = 4;
	}

	return 0;
}

static int
it6664_handle_switch_timer(struct gc555_it6664 *it6664,
			   struct it6664_switch_irq_pending *pending)
{
	struct it6664_rx_state *state = &it6664->runtime.rx;
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *rx = it6664->maps[IT6664_MAP_RX_PORT0].regmap;
	u8 reg13;
	u8 reg14;
	u8 reg_e5;
	u8 e5_value;
	u8 tx_status;
	struct regmap *tx;
	bool hpd = false;
	unsigned int port;
	int cleanup_ret;
	int ret;

	if (pending->timer1_stage == 1) {
		ret = it6664_write_bits(rx, 0xe5, BIT(4), BIT(4));
		if (ret)
			return ret;
		pending->timer1_stage = 2;
	}
	if (pending->timer1_stage == 2) {
		ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
		if (ret)
			return ret;
		pending->timer1_stage = 3;
	}
	if (pending->timer1_stage == 3)
		return 0;

	if (state->timer_state != IT6664_RX_TIMER_ARMED &&
	    state->timer_state != IT6664_RX_TIMER_TX_OFF)
		return 0;

	ret = it6664_write_bits(sw, 0x1a, BIT(1), 0);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x19, 0x0f);
	if (ret)
		return ret;
	ret = regmap_write(sw, 0x1d, 0x00);
	if (ret)
		return ret;

	if (state->timer_state == IT6664_RX_TIMER_TX_OFF) {
		for (port = 1; port <= 2; port++) {
			tx = it6664->maps[IT6664_MAP_TX_PORT0 + port].regmap;
			ret = it6664_read_byte(tx, 0x03, &tx_status);
			if (ret)
				return ret;
			hpd |= tx_status & BIT(0);
		}
		ret = gc555_it6664_rx_set_hpd(it6664, hpd);
		if (ret)
			return ret;
		state->timer_state = IT6664_RX_TIMER_IDLE;
		return 0;
	}

	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS_14, &reg14);
	if (ret)
		return ret;
	if (!(reg14 & GENMASK(5, 3))) {
		ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS, &reg13);
		if (ret)
			return ret;
		if (reg13 & BIT(4)) {
			ret = it6664_write_bits(sw, 0x1a, BIT(1), 0);
			if (ret)
				return ret;
			ret = regmap_write(sw, 0x19, 0x0f);
			if (ret)
				return ret;
			ret = regmap_write(sw, 0x1d, 0x81);
			if (ret)
				return ret;
			ret = regmap_write(sw, 0x19, 0x3f);
			if (ret)
				return ret;
			ret = regmap_write(sw, IT6664_SWITCH_REG_IRQ7, 0xff);
			if (ret)
				return ret;
			ret = it6664_write_bits(sw, 0x1a, BIT(1), BIT(1));
			if (ret)
				return ret;

			ret = it6664_write_bits(rx, IT6664_RX_REG_BANK,
						IT6664_RX_BANK_MASK,
						IT6664_RX_BANK_3);
			if (ret)
				goto restore_rx_bank;
			ret = it6664_read_byte(rx, 0xe5, &reg_e5);
			if (!ret) {
				e5_value = reg_e5 & GENMASK(3, 2) ?
					0 : GENMASK(3, 2);
				ret = it6664_write_bits(rx, 0xe5, GENMASK(3, 2), e5_value);
				if (!ret)
					pending->timer1_stage = 1;
			}
			if (!ret)
				ret = it6664_write_bits(rx, 0xe5, BIT(4), BIT(4));
			if (!ret)
				pending->timer1_stage = 2;

restore_rx_bank:
			cleanup_ret = it6664_select_rx_bank(rx, IT6664_RX_BANK_0);
			if (!ret)
				ret = cleanup_ret;
			if (!ret)
				pending->timer1_stage = 3;
			return ret;
		}
	}

	ret = it6664_read_byte(rx, IT6664_RX_REG_STATUS_14, &reg14);
	if (!ret && (reg14 & GENMASK(5, 3)))
		state->timer_state = IT6664_RX_TIMER_IDLE;

	return ret;
}

static int
it6664_consume_switch_irq6(struct gc555_it6664 *it6664,
			   const struct it6664_switch_irq *irq, unsigned int bit)
{
	struct it6664_rx_state *rx_state = &it6664->runtime.rx;
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	struct regmap *tx;
	bool encryption_enabled;
	u8 content_type;
	unsigned int port;
	int cleanup_ret;
	int ret;

	switch (bit) {
	case 0:
		ret = it6664_reset_shared_hdcp_engine(sw);
		break;
	case 1:
		for (port = 0; port < IT6664_TX_PORT_COUNT; port++)
			if (it6664->runtime.tx[port].hpd)
				it6664->runtime.tx[port].hdcp_state =
					IT6664_TX_HDCP_CHECK;
		ret = 0;
		break;
	case 2:
		rx_state->source_hdcp_content_type_valid = false;
		ret = 0;
		break;
	case 3:
		ret = it6664_read_switch_encryption_status(it6664, &encryption_enabled);
		if (ret)
			break;
		if (encryption_enabled) {
			it6664_seed_tx_hdcp_check(it6664);
		} else {
			rx_state->source_hdcp_content_type_valid = false;
			it6664_reset_tx_hdcp_states(it6664);
		}
		break;
	case 4:
		if (!(irq->irq21 & BIT(7)))
			return 0;
		ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK,
					BIT(0), BIT(0));
		if (ret)
			break;
		ret = regmap_write(sw, 0x17, 0x3a);
		if (!ret)
			ret = it6664_read_byte(sw, 0x25, &content_type);
		cleanup_ret =
			it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), 0);
		if (!ret)
			ret = cleanup_ret;
		if (ret)
			break;
		rx_state->source_hdcp_content_type = content_type;
		rx_state->source_hdcp_content_type_valid = true;
		if (!content_type)
			break;
		for (port = 0; port < IT6664_TX_PORT_COUNT; port++) {
			if (it6664->runtime.tx[port].hdcp_fire_version != 1 ||
			    !it6664->runtime.tx[port].hdcp_done)
				continue;
			tx = it6664->maps[IT6664_MAP_TX_PORT0 + port].regmap;
			ret = it6664_write_bits(tx, 0x88, GENMASK(1, 0), GENMASK(1, 0));
			if (ret)
				break;
		}
		break;
	default:
		ret = 0;
		break;
	}

	return ret;
}

static int
it6664_consume_switch_irq(struct gc555_it6664 *it6664,
			  struct it6664_switch_irq_pending *pending)
{
	const struct it6664_switch_irq *irq = &pending->snapshot;
	unsigned int bit;
	unsigned int index;
	int ret;

	for (bit = 0; bit < 8; bit++) {
		index = bit;
		if (!(irq->irq6 & BIT(bit)) || (pending->consumed & BIT(index)))
			continue;
		ret = it6664_consume_switch_irq6(it6664, irq, bit);
		if (ret)
			return ret;
		pending->consumed |= BIT(index);
	}
	for (bit = 0; bit < 8; bit++) {
		index = bit + 8;
		if (!(irq->irq7 & BIT(bit)) || (pending->consumed & BIT(index)))
			continue;
		if (bit == 4)
			ret = it6664_handle_switch_timer0(it6664, pending);
		else if (bit == 5)
			ret = it6664_handle_switch_timer(it6664, pending);
		else
			ret = 0;
		if (ret)
			return ret;
		pending->consumed |= BIT(index);
	}

	return 0;
}

static int
it6664_ack_switch_irq(struct gc555_it6664 *it6664, struct it6664_switch_irq_pending *pending)
{
	const struct it6664_switch_irq *irq = &pending->snapshot;
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	const u8 bank1_values[] = { irq->irq21, irq->irq22, irq->irq23 };
	unsigned int i;
	int cleanup_ret;
	int ret;

	if (!(pending->acknowledged & BIT(0))) {
		ret = regmap_write(sw, IT6664_SWITCH_REG_IRQ6, irq->irq6);
		if (ret)
			return ret;
		pending->acknowledged |= BIT(0);
	}
	if (!(pending->acknowledged & BIT(1))) {
		ret = regmap_write(sw, IT6664_SWITCH_REG_IRQ7, irq->irq7);
		if (ret)
			return ret;
		pending->acknowledged |= BIT(1);
	}

	ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), BIT(0));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(bank1_values); i++) {
		if (pending->acknowledged & BIT(i + 2))
			continue;
		ret = regmap_write(sw, 0x21 + i, bank1_values[i]);
		if (ret)
			break;
		pending->acknowledged |= BIT(i + 2);
	}
	cleanup_ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), 0);
	if (!ret)
		ret = cleanup_ret;

	return ret;
}

static int it6664_handle_switch_hdcp_irq(struct gc555_it6664 *it6664)
{
	struct it6664_switch_irq_pending *pending =
		&it6664->runtime.switch_irq;
	struct regmap *sw = it6664->maps[IT6664_MAP_SWITCH].regmap;
	u8 common;
	int ret;

	if (!pending->valid) {
		ret = it6664_write_bits(sw, IT6664_SWITCH_REG_BANK, BIT(0), 0);
		if (ret)
			return ret;
		ret = it6664_read_byte(sw, IT6664_SWITCH_REG_IRQ_STATUS, &common);
		if (ret || !(common & IT6664_SWITCH_IRQ_SHARED))
			return ret;
		ret = it6664_read_switch_irq(it6664, &pending->snapshot);
		if (ret)
			return ret;
		pending->consumed = 0;
		pending->acknowledged = 0;
		pending->timer0_stage = 0;
		pending->valid = true;
	}

	ret = it6664_consume_switch_irq(it6664, pending);
	if (ret)
		return ret;
	ret = it6664_ack_switch_irq(it6664, pending);
	if (ret)
		return ret;

	memset(pending, 0, sizeof(*pending));
	return 0;
}

static void it6664_runtime_work(struct work_struct *work)
{
	struct it6664_runtime *runtime =
		container_of(to_delayed_work(work),
			     struct it6664_runtime, work);
	struct gc555_it6664 *it6664 =
		container_of(runtime, struct gc555_it6664, runtime);
	unsigned long delay = msecs_to_jiffies(IT6664_RUNTIME_INTERVAL_MS);
	u8 merged_edid[IT6664_EDID_SIZE];
	int hdcp_ret;
	int ret;

	if (!atomic_read(&runtime->enabled))
		return;

	ret = it6664_handle_switch_hdcp_irq(it6664);
	if (!ret)
		ret = it6664_rx_poll(it6664);
	if (!ret)
		ret = it6664_rx_autoeq_poll(it6664);
	if (!ret)
		ret = gc555_it6664_tx_poll(it6664);
	hdcp_ret = it6664_sample_source_hdcp(it6664);
	if (!ret)
		ret = hdcp_ret;
	if (!ret && runtime->tx[1].hpd &&
	    runtime->upstream_edid == IT6664_UPSTREAM_EDID_NONE &&
	    !runtime->tx[1].edid_attempted &&
	    runtime->edid_publish_sessions < IT6664_EDID_PUBLISH_SESSIONS) {
		runtime->edid_publish_sessions++;
		ret = it6664_publish_fixed_edid(it6664);
	}
	if (!ret && runtime->tx[2].hpd && !runtime->tx[2].edid_attempted)
		ret = gc555_it6664_tx_read_sink_edid(it6664);
	if (!ret && edid_merge && runtime->sink_edid.valid &&
	    !runtime->merge_attempted) {
		runtime->merge_attempted = true;
		ret = gc555_edid_merge(runtime->sink_edid.data,
				       runtime->sink_edid.length,
				       merged_edid, sizeof(merged_edid));
		if (!ret &&
		    (runtime->upstream_edid != IT6664_UPSTREAM_EDID_MERGED ||
		     memcmp(runtime->merged_edid, merged_edid,
			    sizeof(merged_edid)))) {
			memcpy(runtime->merged_edid, merged_edid,
			       sizeof(merged_edid));
			runtime->merged_edid_pending = true;
			runtime->edid_publish_sessions = 0;
		}
	}
	if (!ret && runtime->merged_edid_pending &&
	    runtime->edid_publish_sessions < IT6664_EDID_PUBLISH_SESSIONS) {
		runtime->edid_publish_sessions++;
		ret = it6664_publish_merged_edid(it6664);
		if (!ret) {
			runtime->merged_edid_pending = false;
			runtime->edid_publish_sessions = 0;
		}
	}
	if (ret && ret != -EAGAIN && ret != -ESHUTDOWN &&
	    atomic_read(&runtime->enabled))
		dev_warn_ratelimited(it6664->gc555->dev,
				     "IT6664 runtime update failed: %d\n", ret);

	if (atomic_read(&runtime->enabled))
		queue_delayed_work(runtime->wq, &runtime->work, delay);
}

static void it6664_reset_runtime(struct gc555_it6664 *it6664)
{
	memset(&it6664->runtime, 0, sizeof(it6664->runtime));
	it6664->runtime.rx.hdcp_enabled = true;
}

static int
it6664_restore_hardware(struct gc555_it6664 *it6664,
			struct it6664_caof_result *caof,
			struct it6664_rx_snapshot *rx_snapshot)
{
	struct device *dev = it6664->gc555->dev;
	int ret;

	ret = it6664_calibrate_rclk(it6664);
	if (ret) {
		dev_err(dev, "failed to calibrate IT6664 reference clock: %d\n",
			ret);
		return ret;
	}
	ret = it6664_initialize_rx_caof(it6664, caof);
	if (ret) {
		dev_err(dev, "failed to initialize IT6664 RX CAOF: %d\n", ret);
		return ret;
	}
	ret = it6664_initialize_rx_registers(it6664);
	if (ret) {
		dev_err(dev, "failed to initialize IT6664 RX registers: %d\n",
			ret);
		return ret;
	}
	ret = it6664_initialize_rx_power_state(it6664);
	if (ret) {
		dev_err(dev, "failed to initialize IT6664 RX power state: %d\n",
			ret);
		return ret;
	}
	ret = gc555_it6664_tx_init(it6664);
	if (ret) {
		dev_err(dev, "failed to initialize IT6664 TX paths: %d\n", ret);
		return ret;
	}
	ret = it6664_read_rx_snapshot(it6664, rx_snapshot);
	if (ret) {
		dev_err(dev, "failed to read initialized IT6664 RX state: %d\n",
			ret);
		return ret;
	}

	return 0;
}

static int it6664_runtime_start(struct gc555_it6664 *it6664)
{
	struct it6664_runtime *runtime = &it6664->runtime;
	unsigned long delay = msecs_to_jiffies(IT6664_RUNTIME_INTERVAL_MS);

	runtime->wq = alloc_ordered_workqueue("gc555-it6664", WQ_MEM_RECLAIM);
	if (!runtime->wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&runtime->work, it6664_runtime_work);
	atomic_set(&runtime->enabled, 1);
	queue_delayed_work(runtime->wq, &runtime->work, delay);

	return 0;
}

static void it6664_runtime_stop(struct gc555_it6664 *it6664)
{
	struct it6664_runtime *runtime = &it6664->runtime;

	if (!runtime->wq)
		return;

	atomic_set(&runtime->enabled, 0);
	cancel_delayed_work_sync(&runtime->work);
	memset(&runtime->switch_irq, 0, sizeof(runtime->switch_irq));
	destroy_workqueue(runtime->wq);
	runtime->wq = NULL;
}

static void gc555_it6664_release(struct gc555_it6664 *it6664)
{
	unsigned int id;

	if (!it6664)
		return;
	it6664_runtime_stop(it6664);

	for (id = IT6664_MAP_COUNT; id-- > 0;) {
		struct it6664_map *map = &it6664->maps[id];

		if (!IS_ERR_OR_NULL(map->regmap))
			regmap_exit(map->regmap);
		if (!IS_ERR_OR_NULL(map->client))
			i2c_unregister_device(map->client);
	}
}

int gc555_it6664_init(struct gc555_dev *gc555)
{
	struct it6664_caof_result caof = {};
	struct it6664_init_observation observation = {};
	struct it6664_rx_snapshot rx_snapshot = {};
	struct i2c_adapter *adapter;
	struct gc555_it6664 *it6664;
	unsigned int port;
	unsigned int rx_reg08;
	unsigned int tx_reg50;
	int ret;

	adapter = gc555_i2c_get_adapter(gc555, GC555_I2C_BUS_PRIMARY);
	if (!adapter)
		return -ENODEV;

	it6664 = devm_kzalloc(gc555->dev, sizeof(*it6664), GFP_KERNEL);
	if (!it6664)
		return -ENOMEM;
	it6664->gc555 = gc555;
	mutex_init(&it6664->input_edid_lock);
	it6664_reset_runtime(it6664);

	ret = it6664_map_init(it6664, adapter, IT6664_MAP_SWITCH);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to initialize IT6664 switch map\n");
		goto release;
	}

	ret = regmap_bulk_read(it6664->maps[IT6664_MAP_SWITCH].regmap,
			       IT6664_REG_ID_BASE,
			       it6664->identity,
			       sizeof(it6664->identity));
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to read IT6664 identity\n");
		goto release;
	}
	if (!it6664_identity_valid(it6664->identity)) {
		ret = dev_err_probe(gc555->dev, -ENODEV,
				    "unexpected IT6664 identity %4ph\n",
				    it6664->identity);
		goto release;
	}

	msleep(IT6664_HEAVY_INIT_DELAY_MS);
	ret = it6664_read_init_observation(it6664, &observation);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to read IT6664 init prologue\n");
		goto release;
	}
	ret = it6664_apply_pre_rclk_sequence(it6664);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to initialize IT6664 internal maps\n");
		goto release;
	}

	ret = it6664_map_init(it6664, adapter, IT6664_MAP_RX_PORT0);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to initialize IT6664 RX port-0 map\n");
		goto release;
	}
	ret = it6664_map_init(it6664, adapter, IT6664_MAP_RX_EDID_RAM);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to initialize IT6664 EDID RAM map\n");
		goto release;
	}
	ret = regmap_read(it6664->maps[IT6664_MAP_RX_PORT0].regmap,
			  IT6664_RX_REG_CAOF_STATUS, &rx_reg08);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to read IT6664 RX port-0 map\n");
		goto release;
	}
	caof.initial_status = rx_reg08;

	ret = it6664_map_init(it6664, adapter, IT6664_MAP_TX_COMMON);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to initialize IT6664 TX-common map\n");
		goto release;
	}
	ret = regmap_read(it6664->maps[IT6664_MAP_TX_COMMON].regmap,
			  IT6664_TX_COMMON_REG_50, &tx_reg50);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to read IT6664 TX-common map\n");
		goto release;
	}
	for (port = 0; port < IT6664_TX_PORT_COUNT; port++) {
		ret = it6664_map_init(it6664, adapter,
				      IT6664_MAP_TX_PORT0 + port);
		if (ret) {
			ret = dev_err_probe(gc555->dev, ret,
					    "failed to initialize IT6664 TX port %u map\n",
					    port);
			goto release;
		}
	}
	ret = it6664_restore_hardware(it6664, &caof, &rx_snapshot);
	if (ret)
		goto release;

	dev_dbg(gc555->dev, "IT6664 identity %4ph\n", it6664->identity);
	dev_dbg(gc555->dev,
		"IT6664 init prologue regs02-03=%2ph reg03=%02x reg15=%02x\n",
		observation.regs02_03, observation.reg03, observation.reg15);
	dev_dbg(gc555->dev, "IT6664 RX reg08 %#02x\n", rx_reg08);
	dev_dbg(gc555->dev, "IT6664 TX-common reg50 %#02x\n", tx_reg50);
	dev_dbg(gc555->dev, "IT6664 SIPROM raw %u, RCLK %u kHz\n",
		it6664->siprom_raw, it6664->rclk_khz);
	dev_dbg(gc555->dev,
		"IT6664 RX CAOF status %02x->%02x polls=%u timeout=%u post=%02x/%2ph\n",
		caof.initial_status, caof.status, caof.polls, caof.timed_out,
		caof.reg5a, caof.reg59);
	dev_dbg(gc555->dev, "IT6664 RX initialized 08/13/14/19=%02x/%02x/%02x/%02x\n",
		rx_snapshot.reg08, rx_snapshot.reg13, rx_snapshot.reg14,
		rx_snapshot.reg19);
	ret = it6664_runtime_start(it6664);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to start IT6664 runtime worker\n");
		goto release;
	}
	gc555->it6664 = it6664;
	return 0;

release:
	gc555_it6664_release(it6664);
	return ret;
}

void gc555_it6664_cleanup(struct gc555_dev *gc555)
{
	struct gc555_it6664 *it6664;

	if (!gc555 || !gc555->it6664)
		return;

	it6664 = gc555->it6664;
	gc555->it6664 = NULL;
	gc555_it6664_release(it6664);
}

void gc555_it6664_suspend(struct gc555_dev *gc555)
{
	if (gc555 && gc555->it6664)
		it6664_runtime_stop(gc555->it6664);
}

int gc555_it6664_resume(struct gc555_dev *gc555)
{
	struct it6664_caof_result caof = {};
	struct it6664_rx_snapshot rx_snapshot = {};
	struct gc555_it6664 *it6664;
	unsigned int rx_reg08;
	u8 identity[IT6664_ID_LENGTH];
	int ret;

	if (!gc555 || !gc555->it6664)
		return -ENODEV;
	it6664 = gc555->it6664;

	ret = regmap_bulk_read(
		it6664->maps[IT6664_MAP_SWITCH].regmap,
		IT6664_REG_ID_BASE, identity, sizeof(identity));
	if (ret)
		return ret;
	if (!it6664_identity_valid(identity))
		return -ENODEV;

	msleep(IT6664_HEAVY_INIT_DELAY_MS);
	ret = it6664_apply_pre_rclk_sequence(it6664);
	if (ret)
		return ret;

	it6664_reset_runtime(it6664);
	ret = regmap_read(it6664->maps[IT6664_MAP_RX_PORT0].regmap,
			  IT6664_RX_REG_CAOF_STATUS, &rx_reg08);
	if (ret)
		return ret;
	caof.initial_status = rx_reg08;

	ret = it6664_restore_hardware(it6664, &caof, &rx_snapshot);
	if (ret)
		return ret;

	return it6664_runtime_start(it6664);
}

int gc555_it6664_get_source_hdcp(struct gc555_it6664 *it6664,
				 enum gc555_hdcp_level *level)
{
	if (!level)
		return -EINVAL;
	if (!it6664)
		return -ENODEV;

	*level = READ_ONCE(it6664->runtime.rx.source_hdcp_effective_level);
	return 0;
}

int gc555_it6664_get_input_edid(struct gc555_it6664 *it6664, u8 *edid,
				size_t size)
{
	int ret = 0;

	if (!edid || size != IT6664_EDID_SIZE)
		return -EINVAL;
	if (!it6664)
		return -ENODEV;

	mutex_lock(&it6664->input_edid_lock);
	if (!it6664->input_edid_valid)
		ret = -ENODATA;
	else
		memcpy(edid, it6664->input_edid, size);
	mutex_unlock(&it6664->input_edid_lock);

	return ret;
}

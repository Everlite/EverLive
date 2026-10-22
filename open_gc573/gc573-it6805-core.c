// SPDX-License-Identifier: GPL-2.0-only
/*
 * Derived from gc555 driver by ostrich (GPL-2.0-only).
 * Modified for GC573 dual-pixel DDR & 1440p120 ITE6805 timings by Everlite & contributors (2026).
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#if __has_include(<linux/unaligned.h>)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif
#include <linux/workqueue.h>

#include "gc573.h"

#define IT6805_I2C_ADDRESS	0x48
#define IT6805_EDID_I2C_ADDRESS	0x54
#define IT6805_REG_ID_BASE	0x00
#define IT6805_REG_REVISION	0x04
#define IT6805_REG_BANK_SELECT	0x0f
#define IT6805_BANK_MASK	GENMASK(2, 0)
#define IT6805_BANK_0		0x00
#define IT6805_BANK_1		0x01
#define IT6805_BANK_3		0x03
#define IT6805_BANK_CAOF_PORT0	0x03
#define IT6805_BANK_CAOF_PORT1_CONTROL	0x04
/* Port 1 CAOF result registers reside in bank 7. */
#define IT6805_BANK_CAOF_PORT1	0x07
#define IT6805_ID_LENGTH	4
#define IT6805_CAOF_DONE_MASK	GENMASK(5, 4)
#define IT6805_CAOF_PULSE_AFTER_POLLS	5
#define IT6805_CAOF_MAX_RESTARTS	6
#define IT6805_CAOF_DELAY_MS	10
#define IT6805_OCLK_READY	0x19
#define IT6805_OCLK_MAX_POLLS	50
#define IT6805_OCLK_MIN_KHZ	28500
#define IT6805_OCLK_MAX_KHZ	47500
#define IT6805_OCLK_DEFAULT_KHZ	38000
#define IT6805_EDID_BLOCK_SIZE	128
#define IT6805_EDID_BLOCK_COUNT	2
#define IT6805_EDID_SIZE		(IT6805_EDID_BLOCK_SIZE * \
				 IT6805_EDID_BLOCK_COUNT)
#define IT6805_RUNTIME_FAST_INTERVAL_MS	20
#define IT6805_RUNTIME_INTERVAL_MS	100
#define IT6805_RUNTIME_FAST_POLLS	20
#define IT6805_VIDEO_CONFIRM_POLLS	6
#define IT6805_COMMON_IRQ_SCDT	BIT(1)
#define IT6805_COMMON_IRQ_AVMUTE_SET	BIT(4)
#define IT6805_COMMON_IRQ_AVMUTE_CLEAR	BIT(5)
#define IT6805_COMMON_IRQ_AUDIO	BIT(7)
#define IT6805_PACKET_IRQ_AVI	BIT(0)
#define IT6805_PACKET_IRQ_AUX	BIT(7)
#define IT6805_DRM_ABSENT_IRQ	BIT(6)
#define IT6805_DRM_INFOFRAME_SIZE	31
#define IT6805_DRM_STABLE_POLLS	7
#define IT6805_DRM_PACKET_TYPE	0x87
#define IT6805_DRM_EOTF_MASK	GENMASK(2, 0)
#define IT6805_DRM_EOTF_PQ	2
#define IT6805_DRM_EOTF_HLG	3
#define IT6805_TMDS_SAMPLE_COUNT	10
#define IT6805_TMDS_SAMPLE_DELAY_MS	10
#define IT6805_TMDS_SAMPLE_BIAS	10
#define IT6805_PCLK_SAMPLE_COUNT	25
#define IT6805_PCLK_SAMPLE_DELAY_MS	3
#define IT6805_PCLK_SAMPLE_SCALE	0x200
#define IT6805_HIGH_BANDWIDTH_PCLK_KHZ	150000
#define IT6805_AUDIO_REQUEST_DELAY_POLLS	4
#define IT6805_AUDIO_MONITOR_DELAY_POLLS	3
#define IT6805_AUDIO_RATE_MISMATCH_LIMIT	16
#define IT6805_AUDIO_TMDS_SAMPLE_DELAY_MS	3
#define IT6805_AUDIO_N_TOLERANCE	2
#define IT6805_AUDIO_CTS_TOLERANCE	20
enum it6805_system_action {
	IT6805_SYS_ACTION_5V		= BIT(0),
	IT6805_SYS_ACTION_CLEAR_ERRORS	= BIT(1),
	IT6805_SYS_ACTION_RESET_EQ	= BIT(2),
	IT6805_SYS_ACTION_MODE_CHANGE	= BIT(3),
	IT6805_SYS_ACTION_LOST_RATIO	= BIT(4),
	IT6805_SYS_ACTION_ECC		= BIT(5),
	IT6805_SYS_ACTION_DESKEW	= BIT(6),
	IT6805_SYS_ACTION_DETECTOR	= BIT(7),
};

enum it6805_video_state {
	IT6805_VIDEO_POWER_OFF,
	IT6805_VIDEO_WAIT_5V,
	IT6805_VIDEO_WAIT_SYNC,
	IT6805_VIDEO_CONFIRM_SYNC,
	IT6805_VIDEO_ACTIVE,
};

enum it6805_video_colorspace {
	IT6805_COLORSPACE_RGB,
	IT6805_COLORSPACE_YCBCR422,
	IT6805_COLORSPACE_YCBCR444,
	IT6805_COLORSPACE_YCBCR420,
};

enum it6805_audio_state {
	IT6805_AUDIO_OFF,
	IT6805_AUDIO_REQUEST,
	IT6805_AUDIO_WAIT_READY,
	IT6805_AUDIO_ON,
};

enum it6805_eq_state {
	IT6805_EQ_NO_PENDING_STATE,
	IT6805_EQ_PROCESS_RESULTS = 3,
};

enum it6805_audio_rate_code {
	IT6805_AUDIO_RATE_44_1_KHZ = 0x00,
	IT6805_AUDIO_RATE_48_KHZ = 0x02,
	IT6805_AUDIO_RATE_32_KHZ = 0x03,
	IT6805_AUDIO_RATE_384_KHZ = 0x05,
	IT6805_AUDIO_RATE_88_2_KHZ = 0x08,
	IT6805_AUDIO_RATE_768_KHZ = 0x09,
	IT6805_AUDIO_RATE_96_KHZ = 0x0a,
	IT6805_AUDIO_RATE_64_KHZ = 0x0b,
	IT6805_AUDIO_RATE_176_4_KHZ = 0x0c,
	IT6805_AUDIO_RATE_192_KHZ = 0x0e,
	IT6805_AUDIO_RATE_256_KHZ = 0x1b,
	IT6805_AUDIO_RATE_128_KHZ = 0x2b,
	IT6805_AUDIO_RATE_1024_KHZ = 0x35,
	IT6805_AUDIO_RATE_512_KHZ = 0x3b,
};

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

struct it6805_input_path_result {
	u8 block_checksum[IT6805_EDID_BLOCK_COUNT];
	u8 port_checksum[2];
	u8 physical_address_offset;
	u8 ttl_c0;
	u8 ttl_c1;
	u8 status13;
	u8 status16;
	u8 selected_port;
	unsigned int edid_mismatches;
	unsigned int hpd_actions;
	bool five_volt;
	bool hpd_before;
	bool hpd_after;
};

struct it6805_5v_result {
	u8 status;
	unsigned int hpd_actions;
	bool present;
};

struct it6805_runtime_snapshot {
	u8 port0_sys_irq[4];
	u8 port0_eq_irq;
	u8 common_irq[7];
	u8 port0_status[3];
	u8 scdt;
};

struct it6805_common_irq {
	u8 reg10;
	u8 reg11;
	u8 reg12;
	u8 reg1a;
	u8 reg1b;
	u8 reg1d;
	u8 regd4;
	u8 regd5;
	u8 avi_checksum;
	u8 avi_db1;
};

struct it6805_video_timing {
	u32 pixel_clock_khz;
	u32 tmds_clock_khz;
	u32 field_rate_x100;
	u16 htotal;
	u16 hactive;
	u16 hsync;
	u16 hfront;
	u16 hback;
	u16 vtotal;
	u16 vactive;
	u16 vsync;
	u16 vfront;
	u16 vback;
	u8 frame_rate_hz;
	u8 tmds_mode;
	u8 sync_status_first;
	u8 sync_status_second;
	bool interlaced;
	bool valid;
};

struct it6805_avi_info {
	u8 raw[6];
	enum it6805_video_colorspace colorspace;
	u8 colorimetry;
	u8 extended_colorimetry;
	u8 rgb_quantization;
	u8 ycc_quantization;
	u8 scan_info;
	u8 vic;
	bool valid;
};

struct it6805_drm_static_metadata {
	u16 display_primaries_x[3];
	u16 display_primaries_y[3];
	u16 white_point_x;
	u16 white_point_y;
	u16 max_display_mastering_luminance;
	u16 min_display_mastering_luminance;
	u16 max_cll;
	u16 max_fall;
	u8 eotf;
	u8 descriptor_id;
};

struct it6805_drm_info {
	u8 raw[IT6805_DRM_INFOFRAME_SIZE];
	struct it6805_drm_static_metadata static_metadata;
	enum gc555_video_hdr_mode hdr_mode;
	u8 stable_polls;
	bool present;
};

struct it6805_video_output {
	u8 ttl_pixel_mode;
	u8 lvds_path_mode;
	u8 lvds_pixel_mode;
	bool is_dvi;
	bool configured;
	bool enabled;
	bool unmuted;
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
};

struct it6805_eq_runtime {
	u16 primary[3];
	u16 secondary[3];
	enum it6805_eq_state pending_state;
	u8 irq_flags;
	bool result_saturated;
	bool result_zero;
};

struct it6805_system_runtime {
	u8 ecc_errors;
	u8 ecc_recoveries;
	u8 deskew_errors;
	u8 deskew_recoveries;
	u8 lock_count[3];
	bool hdmi2_phy;
};

struct it6805_runtime {
	struct workqueue_struct *wq;
	struct delayed_work work;
	struct it6805_common_irq last_common_irq;
	struct it6805_video_timing timing;
	struct it6805_avi_info avi;
	struct it6805_drm_info drm;
	struct it6805_video_output output;
	struct it6805_audio_runtime audio;
	struct gc555_hdmi_audio_format reported_audio_format;
	struct it6805_eq_runtime eq;
	struct it6805_system_runtime system;
	enum it6805_video_state video_state;
	unsigned int poll_count;
	u8 video_state_delay_polls;
	bool scdt;
	bool scdt_valid;
	bool avi_change_pending;
	atomic_t enabled;
};

struct gc555_it6805 {
	struct gc555_dev *gc555;
	struct i2c_client *client;
	struct regmap *regmap;
	struct i2c_client *edid_client;
	struct regmap *edid_regmap;
	struct mutex io_lock; /* Serializes bank selection and register I/O. */
	u8 bank;
	bool bank_valid;
	u8 identity[IT6805_ID_LENGTH];
	u8 revision;
	u8 input_color_depth_code;
	struct it6805_runtime runtime;
	u32 oclk_khz;
	u32 reference_khz;
};

static int it6805_detect_dvi_locked(struct gc555_it6805 *it6805,
				    bool *is_dvi);
static int it6805_set_hdmi2_phy_locked(struct gc555_it6805 *it6805,
				       bool hdmi2);

/* Receiver base initialization sequence, including required bank changes. */
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

static const struct regmap_config it6805_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static bool it6805_identity_valid(const u8 *identity)
{
	return identity[0] == 0x54 && identity[1] == 0x49 &&
	       (identity[2] == 0x05 || identity[2] == 0x07) &&
	       identity[3] == 0x68;
}

static int it6805_read_locked(struct gc555_it6805 *it6805, u8 reg,
			      u8 *value)
{
	unsigned int reg_value;
	int ret;

	ret = regmap_read(it6805->regmap, reg, &reg_value);
	if (ret) {
		it6805->bank_valid = false;
		return ret;
	}

	*value = reg_value;
	return 0;
}

static int it6805_write_locked(struct gc555_it6805 *it6805, u8 reg,
			       u8 value)
{
	int ret;

	ret = regmap_write(it6805->regmap, reg, value);
	if (ret) {
		it6805->bank_valid = false;
		return ret;
	}

	if (reg == IT6805_REG_BANK_SELECT) {
		it6805->bank = value & IT6805_BANK_MASK;
		it6805->bank_valid = true;
	}

	return 0;
}

static int it6805_update_bits_locked(struct gc555_it6805 *it6805, u8 reg,
				     u8 mask, u8 value)
{
	u8 reg_value;
	u8 updated;
	int ret;

	ret = it6805_read_locked(it6805, reg, &reg_value);
	if (ret)
		return ret;

	updated = (reg_value & ~mask) | (value & mask);
	return it6805_write_locked(it6805, reg, updated);
}

static int it6805_select_bank_locked(struct gc555_it6805 *it6805, u8 bank)
{
	u8 reg_value;
	int ret;

	if (bank > IT6805_BANK_MASK)
		return -EINVAL;
	if (it6805->bank_valid && it6805->bank == bank)
		return 0;

	ret = it6805_read_locked(it6805, IT6805_REG_BANK_SELECT, &reg_value);
	if (ret)
		return ret;

	it6805->bank = reg_value & IT6805_BANK_MASK;
	it6805->bank_valid = true;
	if (it6805->bank == bank)
		return 0;

	reg_value = (reg_value & ~IT6805_BANK_MASK) | bank;
	return it6805_write_locked(it6805, IT6805_REG_BANK_SELECT,
				    reg_value);
}

static int it6805_set_bank_locked(struct gc555_it6805 *it6805, u8 bank)
{
	if (bank > IT6805_BANK_MASK)
		return -EINVAL;

	return it6805_update_bits_locked(it6805, IT6805_REG_BANK_SELECT,
					 IT6805_BANK_MASK, bank);
}

static int it6805_bulk_read(struct gc555_it6805 *it6805, u8 bank, u8 reg,
			    void *values, size_t value_count)
{
	int ret;

	if (!values || !value_count || value_count > 0x100U - reg)
		return -EINVAL;

	mutex_lock(&it6805->io_lock);
	ret = it6805_select_bank_locked(it6805, bank);
	if (!ret)
		ret = regmap_bulk_read(it6805->regmap, reg, values,
				       value_count);
	if (ret)
		it6805->bank_valid = false;
	mutex_unlock(&it6805->io_lock);

	return ret;
}

static int it6805_apply_initial_sequence(struct gc555_it6805 *it6805,
					 size_t *failed_index)
{
	size_t i;
	int ret = 0;

	mutex_lock(&it6805->io_lock);
	for (i = 0; i < ARRAY_SIZE(it6805_initial_sequence); i++) {
		const struct it6805_reg_update *update;

		update = &it6805_initial_sequence[i];
		ret = it6805_update_bits_locked(it6805,
						update->reg,
						update->mask,
						update->value);
		if (ret)
			break;
	}
	mutex_unlock(&it6805->io_lock);

	if (failed_index)
		*failed_index = i;

	return ret;
}

static int it6805_apply_updates_locked(struct gc555_it6805 *it6805,
				       const struct it6805_reg_update *updates,
				       size_t update_count)
{
	size_t i;
	int ret;

	for (i = 0; i < update_count; i++) {
		ret = it6805_update_bits_locked(it6805, updates[i].reg,
						updates[i].mask,
						updates[i].value);
		if (ret)
			return ret;
	}

	return 0;
}

static int it6805_caof_apply_locked(struct gc555_it6805 *it6805, u8 bank,
				    const struct it6805_reg_update *updates,
				    size_t update_count)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, bank);
	if (ret)
		return ret;

	return it6805_apply_updates_locked(it6805, updates, update_count);
}

static int it6805_write_zero_range_locked(struct gc555_it6805 *it6805,
					  u8 first_reg, u8 count)
{
	u8 i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = it6805_write_locked(it6805, first_reg + i, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static int it6805_caof_prepare_locked(struct gc555_it6805 *it6805)
{
	int ret;

	ret = it6805_caof_apply_locked(it6805, IT6805_BANK_CAOF_PORT0,
				       it6805_caof_enable,
				       ARRAY_SIZE(it6805_caof_enable));
	if (ret)
		return ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x29, 0x01, 0x01);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x2a, 0x41, 0x41);
	if (ret)
		return ret;
	msleep(IT6805_CAOF_DELAY_MS);
	ret = it6805_update_bits_locked(it6805, 0x2a, 0x40, 0x00);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x24, 0x04, 0x04);
	if (ret)
		return ret;
	ret = it6805_write_zero_range_locked(it6805, 0x25, 4);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x3c, 0x10, 0x00);
	if (ret)
		return ret;

	ret = it6805_caof_apply_locked(it6805, IT6805_BANK_CAOF_PORT1,
				       it6805_caof_enable,
				       ARRAY_SIZE(it6805_caof_enable));
	if (ret)
		return ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x32, 0x41, 0x41);
	if (ret)
		return ret;
	msleep(IT6805_CAOF_DELAY_MS);
	ret = it6805_update_bits_locked(it6805, 0x32, 0x40, 0x00);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x2c, 0x04, 0x04);
	if (ret)
		return ret;
	ret = it6805_write_zero_range_locked(it6805, 0x2d, 4);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805,
				     IT6805_BANK_CAOF_PORT1_CONTROL);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x3c, 0x10, 0x00);
	if (ret)
		return ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x3a, 0x80, 0x80);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT1);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x3a, 0x80, 0x80);
	if (ret)
		return ret;

	return it6805_set_bank_locked(it6805, IT6805_BANK_0);
}

static int it6805_caof_read_done_locked(struct gc555_it6805 *it6805,
					struct it6805_caof_result *result)
{
	int ret;

	ret = it6805_read_locked(it6805, 0x08, &result->completion[0]);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x0d, &result->completion[1]);
	if (ret)
		return ret;

	result->completion[0] &= IT6805_CAOF_DONE_MASK;
	result->completion[1] &= IT6805_CAOF_DONE_MASK;
	return 0;
}

static bool it6805_caof_complete(const struct it6805_caof_result *result)
{
	return result->completion[0] && result->completion[1];
}

static int it6805_caof_pulse_locked(struct gc555_it6805 *it6805, u8 reg)
{
	int ret;

	ret = it6805_update_bits_locked(it6805, reg, 0x40, 0x40);
	if (ret)
		return ret;
	msleep(IT6805_CAOF_DELAY_MS);

	return it6805_update_bits_locked(it6805, reg, 0x40, 0x00);
}

static int it6805_caof_force_retry_locked(struct gc555_it6805 *it6805,
					  unsigned int port)
{
	u8 bank = port ? IT6805_BANK_CAOF_PORT1 : IT6805_BANK_CAOF_PORT0;
	u8 pulse_reg = port ? 0x32 : 0x2a;
	int ret;

	ret = it6805_set_bank_locked(it6805, bank);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x3a, 0x80, 0x00);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;

	return it6805_caof_pulse_locked(it6805, pulse_reg);
}

static int it6805_caof_wait_locked(struct gc555_it6805 *it6805,
				   struct it6805_caof_result *result)
{
	unsigned int polls = 0;
	int ret;

	ret = it6805_caof_read_done_locked(it6805, result);
	if (ret)
		return ret;

	while (!it6805_caof_complete(result)) {
		ret = it6805_caof_read_done_locked(it6805, result);
		if (ret)
			return ret;

		if (polls >= IT6805_CAOF_PULSE_AFTER_POLLS) {
			if (!result->completion[0]) {
				ret = it6805_caof_pulse_locked(it6805, 0x2a);
				if (ret)
					return ret;
			}
			if (!result->completion[1]) {
				ret = it6805_caof_pulse_locked(it6805, 0x32);
				if (ret)
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
		ret = it6805_caof_force_retry_locked(it6805, 0);
		if (ret)
			return ret;
	}
	if (!result->completion[1])
		return it6805_caof_force_retry_locked(it6805, 1);

	return 0;
}

static int it6805_caof_read_status_locked(struct gc555_it6805 *it6805,
					  struct it6805_caof_result *result)
{
	u8 status_high;
	u8 status_low;
	u8 interrupt;
	unsigned int port;
	int ret;

	for (port = 0; port < 2; port++) {
		u8 bank = port ? IT6805_BANK_CAOF_PORT1 :
				 IT6805_BANK_CAOF_PORT0;

		ret = it6805_set_bank_locked(it6805, bank);
		if (ret)
			return ret;
		ret = it6805_read_locked(it6805, 0x5a, &status_high);
		if (ret)
			return ret;
		ret = it6805_read_locked(it6805, 0x59, &status_low);
		if (ret)
			return ret;
		ret = it6805_read_locked(it6805, 0x59, &interrupt);
		if (ret)
			return ret;

		result->status[port] = (status_high << 4) |
					(status_low & 0x0f);
		result->interrupt[port] = interrupt & 0xc0;
	}

	return 0;
}

static int it6805_caof_cleanup_locked(struct gc555_it6805 *it6805)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x08, 0x30, 0x30);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x0d, 0x30, 0x30);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x29, 0x01, 0x00);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x24, 0x04, 0x00);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x3c, 0x10, 0x10);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x2c, 0x04, 0x00);
	if (ret)
		return ret;

	ret = it6805_set_bank_locked(it6805,
				     IT6805_BANK_CAOF_PORT1_CONTROL);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x3c, 0x10, 0x10);
	if (ret)
		return ret;

	ret = it6805_caof_apply_locked(it6805, IT6805_BANK_CAOF_PORT0,
				       it6805_caof_disable,
				       ARRAY_SIZE(it6805_caof_disable));
	if (ret)
		return ret;
	ret = it6805_caof_apply_locked(it6805, IT6805_BANK_CAOF_PORT1,
				       it6805_caof_disable,
				       ARRAY_SIZE(it6805_caof_disable));
	if (ret)
		return ret;

	return it6805_set_bank_locked(it6805, IT6805_BANK_0);
}

static int it6805_run_caof(struct gc555_it6805 *it6805,
			   struct it6805_caof_result *result)
{
	int cleanup_ret;
	int ret;

	mutex_lock(&it6805->io_lock);
	ret = it6805_caof_prepare_locked(it6805);
	if (!ret)
		ret = it6805_caof_wait_locked(it6805, result);
	if (!ret)
		ret = it6805_caof_read_status_locked(it6805, result);

	cleanup_ret = it6805_caof_cleanup_locked(it6805);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		it6805_select_bank_locked(it6805, IT6805_BANK_0);
	mutex_unlock(&it6805->io_lock);

	return ret;
}

static int it6805_clear_video_output_locked(struct gc555_it6805 *it6805)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_write_locked(it6805, 0xc5, 0xff);
	if (!ret)
		ret = it6805_write_locked(it6805, 0xc6, 0xff);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);

	return ret;
}

static int it6805_clear_audio_output_locked(struct gc555_it6805 *it6805)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x81, BIT(6), 0);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_write_locked(it6805, 0xc7, 0x7f);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);

	return ret;
}

static int it6805_disable_video_output_locked(struct gc555_it6805 *it6805)
{
	u8 value;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x98, &value);
	if (!ret)
		it6805->input_color_depth_code = value & 0xf0;
	if (!ret)
		ret = it6805_clear_video_output_locked(it6805);

	return ret;
}

static int it6805_disable_video_output(struct gc555_it6805 *it6805)
{
	int ret;

	mutex_lock(&it6805->io_lock);
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x28, 0x88);
	if (!ret)
		ret = it6805_disable_video_output_locked(it6805);
	if (ret)
		it6805_select_bank_locked(it6805, IT6805_BANK_0);
	mutex_unlock(&it6805->io_lock);

	return ret;
}

static int it6805_oclk_read_selector_locked(struct gc555_it6805 *it6805,
					    u8 offset, u8 selector,
					    u16 *value)
{
	u8 high;
	u8 low;
	int ret;

	ret = it6805_write_locked(it6805, 0x50, offset);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x51, selector);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x54, 0x04);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x61, &low);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x62, &high);
	if (ret)
		return ret;

	*value = (high << 8) | low;
	return 0;
}

static int it6805_oclk_cleanup_locked(struct gc555_it6805 *it6805)
{
	int bank0_ret;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x5f, 0x00);

	bank0_ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!bank0_ret)
		bank0_ret = it6805_write_locked(it6805, 0xf8, 0x00);

	return ret ? ret : bank0_ret;
}

static int it6805_load_oclk_locked(struct gc555_it6805 *it6805,
				   struct it6805_oclk_result *result)
{
	u32 measured_khz;
	unsigned int i;
	int cleanup_ret;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		goto cleanup;
	ret = it6805_write_locked(it6805, 0xf8, 0xc3);
	if (ret)
		goto cleanup;
	ret = it6805_write_locked(it6805, 0xf8, 0xa5);
	if (ret)
		goto cleanup;
	ret = it6805_write_locked(it6805, 0x34, 0x00);
	if (ret)
		goto cleanup;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (ret)
		goto cleanup;
	ret = it6805_write_locked(it6805, 0x5f, 0x04);
	if (ret)
		goto cleanup;
	ret = it6805_write_locked(it6805, 0x5f, 0x05);
	if (ret)
		goto cleanup;
	ret = it6805_write_locked(it6805, 0x58, 0x12);
	if (ret)
		goto cleanup;
	ret = it6805_write_locked(it6805, 0x58, 0x02);
	if (ret)
		goto cleanup;
	ret = it6805_read_locked(it6805, 0x60, &result->status_initial);
	if (ret)
		goto cleanup;
	result->status_final = result->status_initial;

	if (result->status_initial != IT6805_OCLK_READY) {
		ret = it6805_write_locked(it6805, 0xf8, 0xc3);
		if (ret)
			goto cleanup;
		ret = it6805_write_locked(it6805, 0xf8, 0xa5);
		if (ret)
			goto cleanup;
		ret = it6805_write_locked(it6805, 0x5f, 0x04);
		if (ret)
			goto cleanup;
		ret = it6805_write_locked(it6805, 0x58, 0x12);
		if (ret)
			goto cleanup;
		ret = it6805_write_locked(it6805, 0x58, 0x02);
		if (ret)
			goto cleanup;

		for (i = 0; i < IT6805_OCLK_MAX_POLLS; i++) {
			ret = it6805_read_locked(it6805, 0x60,
						 &result->status_final);
			if (ret)
				goto cleanup;
			result->polls = i + 1;
			usleep_range(1000, 2000);
			if (result->status_final == IT6805_OCLK_READY)
				break;
		}

		usleep_range(10000, 11000);
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
		if (ret)
			goto cleanup;
		ret = it6805_update_bits_locked(it6805, 0xcf, 0x01, 0x01);
		if (ret)
			goto cleanup;
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
		if (ret)
			goto cleanup;
	}

	ret = it6805_write_locked(it6805, 0x57, 0x01);
	if (ret)
		goto cleanup;
	ret = it6805_oclk_read_selector_locked(it6805, 0x00, 0x00,
					       &result->probe[0]);
	if (ret)
		goto cleanup;
	ret = it6805_oclk_read_selector_locked(it6805, 0x00, 0x01,
					       &result->probe[1]);
	if (ret)
		goto cleanup;
	if (result->probe[0] == 0xffff && !result->probe[1])
		result->selector_offset = 0x04;

	ret = it6805_oclk_read_selector_locked(it6805,
					       result->selector_offset, 0xb0,
					       &result->count[0]);
	if (ret)
		goto cleanup;
	ret = it6805_oclk_read_selector_locked(it6805,
					       result->selector_offset, 0xb1,
					       &result->count[1]);
	if (ret)
		goto cleanup;

	result->scale = result->count[1] >> 8;
	result->raw_count = (result->count[1] & 0xff) << 16 |
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
	cleanup_ret = it6805_oclk_cleanup_locked(it6805);
	return ret ? ret : cleanup_ret;
}

static int it6805_program_oclk_locked(struct gc555_it6805 *it6805,
				      struct it6805_oclk_result *result)
{
	u32 timer_khz;
	int ret;

	if (it6805->revision != 0xb1)
		return -EOPNOTSUPP;

	timer_khz = result->clock_khz / 20 + (result->clock_khz >> 1);
	result->reference_khz = result->clock_khz >> 1;
	result->reg91 = timer_khz / 1000 & 0x3f;
	result->reg92 = ((timer_khz % 1000) * 0x100) / 1000;
	result->regfd = (result->clock_khz >> 3) / 25;
	result->reg45 = (result->clock_khz >> 3) / 39;
	result->reg44 = result->clock_khz / 1560;
	result->reg46 = result->clock_khz / 2320;
	result->reg47 = result->clock_khz / 5312;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xaa, 0x1f, 0x0c);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x91, 0x3f, result->reg91);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x92, result->reg92);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xfd, result->regfd);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xfe, 0x20, 0x00);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xfe, 0x0f, 0x0c);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xfe, 0x10, 0x10);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xfe, 0x80, 0x80);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x45, result->reg45);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x44, result->reg44);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x46, result->reg46);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x47, result->reg47);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;

	ret = it6805_read_locked(it6805, 0x91, &result->reg91);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x92, &result->reg92);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x44, &result->reg44);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x45, &result->reg45);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x46, &result->reg46);
	if (ret)
		return ret;

	return it6805_read_locked(it6805, 0x47, &result->reg47);
}

static int it6805_calibrate_oclk(struct gc555_it6805 *it6805,
				 struct it6805_oclk_result *result)
{
	int ret;

	mutex_lock(&it6805->io_lock);
	ret = it6805_load_oclk_locked(it6805, result);
	if (!ret)
		ret = it6805_program_oclk_locked(it6805, result);
	if (ret)
		it6805_select_bank_locked(it6805, IT6805_BANK_0);
	mutex_unlock(&it6805->io_lock);

	if (!ret) {
		it6805->oclk_khz = result->clock_khz;
		it6805->reference_khz = result->reference_khz;
	}

	return ret;
}

static int it6805_set_port0_hpd_locked(struct gc555_it6805 *it6805,
				       bool high,
					       unsigned int *actions)
{
	u8 status;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x13, &status);
	if (ret)
		return ret;
	if (!(status & BIT(0))) {
		ret = gc555_link_set_input_hpd(it6805->gc555, high);
		if (ret)
			return ret;
		(*actions)++;
	}

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x13, &status);
	if (ret)
		return ret;
	if (!(status & BIT(6))) {
		ret = gc555_link_set_input_hpd(it6805->gc555, high);
		if (ret)
			return ret;
		(*actions)++;
	}

	return it6805_set_bank_locked(it6805, IT6805_BANK_0);
}

static int it6805_write_edid_block_locked(struct gc555_it6805 *it6805,
					  const u8 *edid,
					   unsigned int block,
					   u8 *checksum)
{
	unsigned int base;
	unsigned int i;
	u8 sum = 0;
	int ret;

	if (block >= IT6805_EDID_BLOCK_COUNT)
		return -EINVAL;

	base = block * IT6805_EDID_BLOCK_SIZE;
	for (i = 0; i < IT6805_EDID_BLOCK_SIZE - 1; i++) {
		ret = regmap_write(it6805->edid_regmap, base + i,
				   edid[base + i]);
		if (ret)
			return ret;
		sum += edid[base + i];
	}

	*checksum = -sum;
	return 0;
}

static int it6805_verify_edid_locked(struct gc555_it6805 *it6805,
				     const u8 *edid,
				      unsigned int *mismatches)
{
	unsigned int actual;
	unsigned int i;
	int ret;

	*mismatches = 0;
	for (i = 0; i < IT6805_EDID_SIZE; i++) {
		if ((i & (IT6805_EDID_BLOCK_SIZE - 1)) ==
		    IT6805_EDID_BLOCK_SIZE - 1)
			continue;

		ret = regmap_read(it6805->edid_regmap, i, &actual);
		if (ret)
			return ret;
		if ((u8)actual != edid[i])
			(*mismatches)++;
	}

	return *mismatches ? -EIO : 0;
}

static u8 it6805_find_hdmi_physical_address(const u8 *edid, size_t size)
{
	size_t data_end;
	size_t offset;

	if (size != IT6805_EDID_SIZE || edid[128] != 0x02 ||
	    edid[129] != 0x03 || edid[130] <= 4)
		return 0;

	data_end = 128 + edid[130];
	if (data_end > size)
		return 0;

	for (offset = 132; offset < data_end; ) {
		u8 length = edid[offset] & 0x1f;
		size_t next = offset + length + 1;

		if (next > data_end)
			return 0;
		if ((edid[offset] >> 5) == 0x03 && length >= 5 &&
		    edid[offset + 1] == 0x03 &&
		    edid[offset + 2] == 0x0c &&
		    edid[offset + 3] == 0x00)
			return offset + 4;
		offset = next;
	}

	return 0;
}

static int it6805_program_edid_locked(struct gc555_it6805 *it6805,
				      const u8 *edid, size_t size,
				       struct it6805_input_path_result *result)
{
	u8 offset;
	int ret;

	if (size != IT6805_EDID_SIZE)
		return -EINVAL;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x4b, 0xa9);
	if (ret)
		return ret;
	ret = it6805_set_port0_hpd_locked(it6805, false,
					  &result->hpd_actions);
	if (ret)
		return ret;

	ret = it6805_write_edid_block_locked(it6805, edid, 0,
					     &result->block_checksum[0]);
	if (ret)
		return ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xc9,
				  result->block_checksum[0]);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805,
				     IT6805_BANK_CAOF_PORT1_CONTROL);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xc9,
				  result->block_checksum[0]);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_write_edid_block_locked(it6805, edid, 1,
					     &result->block_checksum[1]);
	if (ret)
		return ret;

	offset = it6805_find_hdmi_physical_address(edid, size);
	if (!offset)
		return -EINVAL;
	result->physical_address_offset = offset;
	result->port_checksum[0] = result->block_checksum[1] + edid[offset] +
				   edid[offset + 1] - 0x10;
	result->port_checksum[1] = result->block_checksum[1] + edid[offset] +
				   edid[offset + 1] - 0x20;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xc6, offset);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xc7, 0x10);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xc8, 0x00);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xca, result->port_checksum[0]);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805,
				     IT6805_BANK_CAOF_PORT1_CONTROL);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xc7, 0x20);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xc8, 0x00);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0xca, result->port_checksum[1]);
	if (ret)
		return ret;

	ret = it6805_verify_edid_locked(it6805, edid,
					&result->edid_mismatches);
	if (ret)
		return ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xc5, 0x01, 0x00);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805,
				     IT6805_BANK_CAOF_PORT1_CONTROL);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xc5, 0x01, 0x00);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xc5, 0x10, 0x10);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = it6805_update_bits_locked(it6805, 0xc5, 0x10, 0x00);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805,
				     IT6805_BANK_CAOF_PORT1_CONTROL);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xc5, 0x10, 0x10);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = it6805_update_bits_locked(it6805, 0xc5, 0x10, 0x00);
	if (ret)
		return ret;

	return it6805_set_bank_locked(it6805, IT6805_BANK_0);
}

static int
it6805_configure_ttl_locked(struct gc555_it6805 *it6805,
			    struct it6805_input_path_result *result)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xc0, 0x06, 0x02);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xc1, 0x02, 0x02);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xc1, 0x20, 0x00);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0xc0, &result->ttl_c0);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0xc1, &result->ttl_c1);
	if (ret)
		return ret;

	return it6805_set_bank_locked(it6805, IT6805_BANK_0);
}

static int
it6805_select_port0_locked(struct gc555_it6805 *it6805,
			   struct it6805_input_path_result *result)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x13, &result->status13);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x16, &result->status16);
	if (ret)
		return ret;

	result->selected_port = result->status13 & BIT(0) ? 0 :
				(result->status16 & BIT(0) ? 1 : 0);
	if (result->selected_port)
		return -EOPNOTSUPP;

	ret = it6805_update_bits_locked(it6805, 0x35, 0x01, 0x00);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xe5, 0x1c, 0x00);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT1);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xe5, 0x1c, 0x00);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;

	ret = it6805_write_locked(it6805, 0x25, 0x00);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x26, 0x00);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x27, 0x00);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x2a, 0x01);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x2d, 0xff);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x2e, 0xff);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x2f, 0xff);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x32, 0x3e);
	if (ret)
		return ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xa8, 0x08, 0x08);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT1);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xa8, 0x08, 0x00);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xc5, 0x10, 0x10);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	return it6805_update_bits_locked(it6805, 0xc5, 0x10, 0x00);
}

static int it6805_reset_eq_port0_locked(struct gc555_it6805 *it6805,
					bool set_reg23_bit)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x2c, 0x00);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x2d, 0x07);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x07, 0xff);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x23, 0xb0);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = it6805_write_locked(it6805, 0x23, 0xa0);
	if (ret)
		return ret;
	if (set_reg23_bit) {
		ret = it6805_update_bits_locked(it6805, 0x23, 0x02, 0x02);
		if (ret)
			return ret;
	}
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x27, 0x9f);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x28, 0x9f);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x29, 0x9f);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x22, 0x00);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x4b, 0x80, 0x00);
	if (ret)
		return ret;

	return it6805_set_bank_locked(it6805, IT6805_BANK_0);
}

static int it6805_start_eq_port0_locked(struct gc555_it6805 *it6805)
{
	u8 status;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x14, &status);
	if (!ret && (status & (BIT(6) | BIT(7))))
		it6805_set_hdmi2_phy_locked(it6805, true);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xa7, BIT(6),
						status & (BIT(6) | BIT(0)) ?
						BIT(6) : 0);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);

	return ret;
}

static int it6805_restart_eq_port0_locked(struct gc555_it6805 *it6805)
{
	int ret;

	ret = it6805_reset_eq_port0_locked(it6805, false);
	if (ret)
		return ret;

	return it6805_start_eq_port0_locked(it6805);
}

static int it6805_enter_no_signal_locked(struct gc555_it6805 *it6805)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0xe5, 0x1c, 0x00);
	if (ret)
		return ret;
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x08, 0x04);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x0d, 0x04);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x22, 0x12);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x22, 0x10);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x23, 0xfd, 0xac);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x23, 0xfd, 0xa0);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x2b, 0xfd, 0xac);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x2b, 0xfd, 0xa0);
	if (ret)
		return ret;

	return it6805_disable_video_output_locked(it6805);
}

static void it6805_invalidate_signal_state(struct it6805_runtime *runtime)
{
	runtime->timing = (struct it6805_video_timing){};
	runtime->avi = (struct it6805_avi_info){};
	runtime->drm = (struct it6805_drm_info){};
	runtime->output = (struct it6805_video_output){};
	runtime->audio = (struct it6805_audio_runtime){};
	runtime->avi_change_pending = false;
}

static int
it6805_handle_5v_port0_locked(struct gc555_it6805 *it6805,
			      struct it6805_5v_result *result,
			      bool set_reg23_bit)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;
	ret = it6805_read_locked(it6805, 0x13, &result->status);
	if (ret)
		return ret;
	result->present = result->status & BIT(0);
	it6805->runtime.scdt = false;
	it6805->runtime.scdt_valid = false;
	it6805->runtime.video_state_delay_polls = 0;
	it6805_invalidate_signal_state(&it6805->runtime);

	if (result->present) {
		it6805->runtime.video_state = IT6805_VIDEO_WAIT_SYNC;
		ret = it6805_disable_video_output_locked(it6805);
		if (ret)
			return ret;
		return it6805_set_port0_hpd_locked(it6805, true,
						       &result->hpd_actions);
	}

	it6805->runtime.video_state = IT6805_VIDEO_WAIT_5V;
	ret = it6805_set_port0_hpd_locked(it6805, false,
					  &result->hpd_actions);
	if (ret)
		return ret;
	ret = it6805_enter_no_signal_locked(it6805);
	if (ret)
		return ret;

	return it6805_reset_eq_port0_locked(it6805, set_reg23_bit);
}

static int it6805_restore_input_hpd(struct gc555_it6805 *it6805,
				    bool previous)
{
	bool hpd_high;
	int ret;

	ret = gc555_link_get_input_hpd(it6805->gc555, &hpd_high);
	if (ret || hpd_high == previous)
		return ret;

	return gc555_link_set_input_hpd(it6805->gc555, previous);
}

static int
it6805_initialize_input_path(struct gc555_it6805 *it6805,
			     struct it6805_input_path_result *result)
{
	const u8 *edid;
	struct it6805_5v_result power = {};
	size_t edid_size;
	int cleanup_ret;
	int restore_ret;
	unsigned int pass;
	int ret;

	ret = gc555_edid_get(&edid, &edid_size);
	if (ret)
		return ret;
	ret = gc555_link_get_input_hpd(it6805->gc555,
				       &result->hpd_before);
	if (ret)
		return ret;

	mutex_lock(&it6805->io_lock);
	ret = it6805_program_edid_locked(it6805, edid, edid_size, result);
	if (!ret)
		ret = it6805_configure_ttl_locked(it6805, result);
	if (!ret)
		ret = it6805_select_port0_locked(it6805, result);
	if (!ret)
		ret = it6805_reset_eq_port0_locked(it6805, true);
	for (pass = 0; !ret && pass < 2; pass++) {
		power = (struct it6805_5v_result){};
		ret = it6805_handle_5v_port0_locked(it6805, &power, pass == 0);
		result->status13 = power.status;
		result->five_volt = power.present;
		result->hpd_actions += power.hpd_actions;
	}

	cleanup_ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		it6805_select_bank_locked(it6805, IT6805_BANK_0);
	mutex_unlock(&it6805->io_lock);

	if (!ret)
		ret = gc555_link_get_input_hpd(it6805->gc555,
					       &result->hpd_after);
	if (!ret)
		return 0;

	restore_ret = it6805_restore_input_hpd(it6805, result->hpd_before);
	if (restore_ret)
		dev_warn(it6805->gc555->dev,
			 "failed to restore input HPD after IT6805 error: %d\n",
			 restore_ret);

	return ret;
}

static int it6805_read_regs_locked(struct gc555_it6805 *it6805,
				   const u8 *registers, u8 *values,
				   size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = it6805_read_locked(it6805, registers[i], &values[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int
it6805_read_runtime_snapshot_locked(struct gc555_it6805 *it6805,
				    struct it6805_runtime_snapshot *snapshot)
{
	static const u8 port0_sys_irq_registers[] = {
		0x05, 0x06, 0x08, 0x09,
	};
	static const u8 common_irq_registers[] = {
		0x09, 0x10, 0x11, 0x12, 0x1d, 0xd4, 0xd5,
	};
	static const u8 port0_status_registers[] = {
		0x13, 0x14, 0x15,
	};
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;

	ret = it6805_read_regs_locked(it6805, port0_sys_irq_registers,
				      snapshot->port0_sys_irq,
				      ARRAY_SIZE(snapshot->port0_sys_irq));
	if (ret)
		return ret;

	ret = it6805_read_regs_locked(it6805, port0_status_registers,
				      snapshot->port0_status,
				      ARRAY_SIZE(snapshot->port0_status));
	if (ret)
		return ret;

	ret = it6805_read_locked(it6805, 0x07, &snapshot->port0_eq_irq);
	if (ret)
		return ret;

	ret = it6805_read_regs_locked(it6805, common_irq_registers,
				      snapshot->common_irq,
				      ARRAY_SIZE(snapshot->common_irq));
	if (ret)
		return ret;

	return it6805_read_locked(it6805, 0x19, &snapshot->scdt);
}

static int
it6805_read_runtime_snapshot(struct gc555_it6805 *it6805,
			     struct it6805_runtime_snapshot *snapshot)
{
	int cleanup_ret;
	int ret;

	mutex_lock(&it6805->io_lock);
	ret = it6805_read_runtime_snapshot_locked(it6805, snapshot);
	cleanup_ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		it6805_select_bank_locked(it6805, IT6805_BANK_0);
	mutex_unlock(&it6805->io_lock);

	return ret;
}

static unsigned long
it6805_classify_system_irq(const struct it6805_runtime_snapshot *snapshot)
{
	unsigned long actions = 0;
	u8 reg05 = snapshot->port0_sys_irq[0];
	u8 reg06 = snapshot->port0_sys_irq[1];
	u8 reg08 = snapshot->port0_sys_irq[2];
	u8 reg09 = snapshot->port0_sys_irq[3];

	if (reg05 & BIT(0))
		actions |= IT6805_SYS_ACTION_5V;
	if ((reg05 & BIT(1)) || (reg09 & (BIT(0) | BIT(3))))
		actions |= IT6805_SYS_ACTION_CLEAR_ERRORS;
	if (((reg05 & BIT(2)) && !(snapshot->port0_status[0] & BIT(4))) ||
	    ((reg06 & BIT(0)) && (snapshot->port0_status[1] & GENMASK(5, 3))))
		actions |= IT6805_SYS_ACTION_RESET_EQ;
	if ((reg05 & BIT(4)) && (snapshot->port0_status[0] & BIT(3)))
		actions |= IT6805_SYS_ACTION_MODE_CHANGE;
	if (reg05 & BIT(5))
		actions |= IT6805_SYS_ACTION_ECC;
	if (reg05 & BIT(6))
		actions |= IT6805_SYS_ACTION_DESKEW;
	if (reg08 & BIT(0))
		actions |= IT6805_SYS_ACTION_LOST_RATIO |
			   IT6805_SYS_ACTION_RESET_EQ;
	if (reg08 & BIT(7))
		actions |= IT6805_SYS_ACTION_DETECTOR;

	return actions;
}

static int it6805_pulse_port0_hpd_locked(struct gc555_it6805 *it6805)
{
	int ret;

	ret = gc555_link_set_input_hpd(it6805->gc555, false);
	if (ret)
		return ret;
	msleep(100);

	return gc555_link_set_input_hpd(it6805->gc555, true);
}

static int it6805_reset_all_logic_port0_locked(struct gc555_it6805 *it6805,
					       struct it6805_5v_result *power)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x08, 0x04);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x22, 0x12);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x22, 0x10);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x23, 0xfd, 0xac);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x23, 0xfd, 0xa0);
	if (!ret && (it6805->revision & 0xfe) == 0xa0)
		ret = it6805_update_bits_locked(it6805, 0xc5, BIT(4), BIT(4));
	if (!ret && (it6805->revision & 0xfe) == 0xa0) {
		usleep_range(1000, 2000);
		ret = it6805_update_bits_locked(it6805, 0xc5, BIT(4), 0);
	}
	if (ret)
		return ret;

	return it6805_handle_5v_port0_locked(it6805, power, false);
}

static int it6805_handle_ecc_error_locked(struct gc555_it6805 *it6805,
					  struct it6805_5v_result *power)
{
	struct it6805_system_runtime *system = &it6805->runtime.system;

	if (++system->ecc_errors < 12)
		return 0;
	system->ecc_errors = 0;
	if (++system->ecc_recoveries < 3)
		return it6805_pulse_port0_hpd_locked(it6805);

	system->ecc_recoveries = 0;
	return it6805_reset_all_logic_port0_locked(it6805, power);
}

static int it6805_handle_deskew_error_locked(struct gc555_it6805 *it6805,
					     struct it6805_5v_result *power,
					     bool hdmi2)
{
	struct it6805_system_runtime *system = &it6805->runtime.system;
	int ret;

	if (hdmi2 || ++system->deskew_errors <= 10)
		return 0;
	system->deskew_errors = 0;
	ret = it6805_start_eq_port0_locked(it6805);
	if (ret)
		return ret;
	if (++system->deskew_recoveries < 3)
		return 0;

	system->deskew_recoveries = 0;
	return it6805_reset_all_logic_port0_locked(it6805, power);
}

/*
 * Bank 3 reg 0x3A[2:1] selects the receiver PHY. Init leaves 0x02 (HDMI 1.4).
 * A 1440p120 source sets reg 0x14[6]; the PHY then has to follow with 0x04
 * or the scrambled 483 MHz link never locks. Same write as the blob's
 * iTE6805_INT_HDMI_Mode_Chg.
 */
static int it6805_set_hdmi2_phy_locked(struct gc555_it6805 *it6805, bool hdmi2)
{
	struct it6805_system_runtime *system = &it6805->runtime.system;
	int ret;

	if (system->hdmi2_phy == hdmi2)
		return 0;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_3);
	if (ret)
		return ret;
	ret = it6805_update_bits_locked(it6805, 0x3a, 0x06,
					hdmi2 ? 0x04 : 0x02);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	else
		it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;

	system->hdmi2_phy = hdmi2;
	dev_info(it6805->gc555->dev, "ITE6805 PHY %s\n",
		 hdmi2 ? "HDMI 2.0" : "HDMI 1.4");
	return 0;
}

static int
it6805_handle_port0_system_irq_locked(struct gc555_it6805 *it6805,
				      struct it6805_runtime_snapshot *snapshot,
				      struct it6805_5v_result *power)
{
	struct it6805_system_runtime *system = &it6805->runtime.system;
	unsigned long actions;
	bool hdmi2;
	int ret;

	ret = it6805_read_runtime_snapshot_locked(it6805, snapshot);
	if (ret)
		return ret;
	if (snapshot->port0_status[1] & (BIT(6) | BIT(7))) {
		hdmi2 = true;
	} else if (it6805->runtime.timing.valid &&
		   it6805->runtime.video_state == IT6805_VIDEO_ACTIVE) {
		hdmi2 = it6805->runtime.timing.pixel_clock_khz >= 340000;
	} else {
		hdmi2 = system->hdmi2_phy;
	}
	ret = it6805_set_hdmi2_phy_locked(it6805, hdmi2);
	if (ret)
		return ret;

	actions = it6805_classify_system_irq(snapshot);
	if (!(snapshot->port0_sys_irq[0] | snapshot->port0_sys_irq[1] |
	      snapshot->port0_sys_irq[2] |
	      (snapshot->port0_sys_irq[3] & ~BIT(2))))
		return 0;

	ret = it6805_write_locked(it6805, 0x05, snapshot->port0_sys_irq[0]);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x06, snapshot->port0_sys_irq[1]);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x08, snapshot->port0_sys_irq[2]);
	if (ret)
		return ret;
	ret = it6805_write_locked(it6805, 0x09,
				  snapshot->port0_sys_irq[3] & ~BIT(2));
	if (ret)
		return ret;

	/* Stock dispatches saved events in register order after W1C. */
	if (actions & IT6805_SYS_ACTION_MODE_CHANGE) {
		bool is_dvi;

		ret = it6805_detect_dvi_locked(it6805, &is_dvi);
		if (ret)
			return ret;
		if (is_dvi && it6805->runtime.audio.state != IT6805_AUDIO_OFF) {
			it6805->runtime.audio.state = IT6805_AUDIO_OFF;
			it6805->runtime.audio.rate_mismatch_count = 0;
			ret = it6805_clear_audio_output_locked(it6805);
			if (ret)
				return ret;
		}
	}
	if (actions & IT6805_SYS_ACTION_ECC) {
		ret = it6805_handle_ecc_error_locked(it6805, power);
		if (ret)
			return ret;
	}
	if (actions & IT6805_SYS_ACTION_DESKEW) {
		ret = it6805_handle_deskew_error_locked(it6805, power, hdmi2);
		if (ret)
			return ret;
	}
	if (snapshot->port0_sys_irq[0] & BIT(2)) {
		memset(system->lock_count, 0, sizeof(system->lock_count));
		if (!(snapshot->port0_status[0] & BIT(4))) {
			ret = it6805_restart_eq_port0_locked(it6805);
			if (ret)
				return ret;
		}
	}
	if (snapshot->port0_sys_irq[0] & BIT(1))
		system->ecc_errors = 0;
	if (snapshot->port0_sys_irq[0] & BIT(0)) {
		ret = it6805_handle_5v_port0_locked(it6805, power, false);
		if (ret)
			return ret;
	}

	if (snapshot->port0_sys_irq[1] & BIT(6))
		system->lock_count[2]++;
	if (snapshot->port0_sys_irq[1] & BIT(5))
		system->lock_count[1]++;
	if (snapshot->port0_sys_irq[1] & BIT(4))
		system->lock_count[0]++;
	if (!hdmi2 && system->lock_count[0] > 20 &&
	    system->lock_count[1] > 20 && system->lock_count[2] > 20) {
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
		if (!ret)
			ret = it6805_update_bits_locked(it6805, 0xe5, 0x1c, 0x1c);
		if (!ret)
			ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
		if (ret)
			return ret;
	}
	if ((snapshot->port0_sys_irq[1] & BIT(0)) &&
	    (snapshot->port0_status[1] & GENMASK(5, 3))) {
		ret = it6805_restart_eq_port0_locked(it6805);
		if (ret)
			return ret;
	}

	if (actions & IT6805_SYS_ACTION_LOST_RATIO) {
		it6805->runtime.video_state = IT6805_VIDEO_WAIT_SYNC;
		it6805->runtime.video_state_delay_polls = 0;
		it6805_invalidate_signal_state(&it6805->runtime);
		ret = it6805_disable_video_output_locked(it6805);
		if (ret)
			return ret;
		ret = it6805_restart_eq_port0_locked(it6805);
		if (ret)
			return ret;
	}
	if (actions & IT6805_SYS_ACTION_DETECTOR) {
		ret = it6805_update_bits_locked(it6805, 0x4c, BIT(7), BIT(7));
		if (ret)
			return ret;
	}
	if (snapshot->port0_sys_irq[3] & (BIT(0) | BIT(3)))
		system->ecc_errors = 0;

	return 0;
}

static int
it6805_handle_port0_system_irq(struct gc555_it6805 *it6805,
			       struct it6805_runtime_snapshot *snapshot,
			       struct it6805_5v_result *power)
{
	int cleanup_ret;
	int ret;

	mutex_lock(&it6805->io_lock);
	ret = it6805_handle_port0_system_irq_locked(it6805, snapshot, power);
	cleanup_ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		it6805_select_bank_locked(it6805, IT6805_BANK_0);
	mutex_unlock(&it6805->io_lock);

	return ret;
}

static int it6805_read_eq_results_locked(struct gc555_it6805 *it6805)
{
	struct it6805_eq_runtime *eq = &it6805->runtime.eq;
	unsigned int lane;
	int ret;

	*eq = (struct it6805_eq_runtime){ .irq_flags = eq->irq_flags };
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (ret)
		return ret;

	for (lane = 0; lane < ARRAY_SIZE(eq->primary); lane++) {
		u8 low;
		u8 high;

		ret = it6805_update_bits_locked(it6805, 0x37, GENMASK(7, 6),
						lane << 6);
		if (!ret)
			ret = it6805_read_locked(it6805, 0x63, &low);
		if (!ret)
			ret = it6805_read_locked(it6805, 0x64, &high);
		if (!ret)
			eq->primary[lane] = ((u16)high << 8) | low;
		if (!ret)
			ret = it6805_read_locked(it6805, 0x6d, &low);
		if (!ret)
			ret = it6805_read_locked(it6805, 0x6e, &high);
		if (ret)
			return ret;
		eq->secondary[lane] = ((u16)high << 8) | low;
		eq->result_saturated |= eq->primary[lane] == 0x3fff;
	}

	if (!eq->result_saturated) {
		for (lane = 0; lane < ARRAY_SIZE(eq->secondary); lane++)
			eq->result_zero |= !eq->secondary[lane];
	}

	return 0;
}

static int it6805_handle_port0_eq_irq_locked(struct gc555_it6805 *it6805,
					     u8 irq)
{
	int ret;

	if (!irq)
		return 0;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x07, irq & 0xf7);
	if (ret)
		return ret;

	it6805->runtime.eq.irq_flags |= irq & GENMASK(7, 4);
	if (!(irq & GENMASK(7, 4)))
		return 0;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_CAOF_PORT0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x22, BIT(2), 0);
	if (!ret && (irq & BIT(5)))
		ret = it6805_read_eq_results_locked(it6805);
	if (!ret && (irq & BIT(5)) && it6805->runtime.eq.result_zero) {
		it6805->runtime.eq.irq_flags &= ~BIT(5);
		it6805->runtime.eq.irq_flags |= BIT(4);
		it6805->runtime.eq.pending_state = IT6805_EQ_PROCESS_RESULTS;
	}

	return ret;
}

static bool
it6805_common_irq_pending(const struct it6805_runtime_snapshot *snapshot)
{
	size_t i;

	/* common_irq[0] is port-0 SYS register 0x09. */
	for (i = 1; i < ARRAY_SIZE(snapshot->common_irq); i++) {
		if (snapshot->common_irq[i])
			return true;
	}

	return false;
}

static int it6805_read_scdt_locked(struct gc555_it6805 *it6805,
				   bool *asserted)
{
	u8 value;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x19, &value);
	if (!ret)
		*asserted = value != 0xff && (value & BIT(7));

	return ret;
}

static int it6805_apply_scdt_locked(struct gc555_it6805 *it6805,
				    bool asserted)
{
	struct it6805_runtime *runtime = &it6805->runtime;
	int ret;

	if (runtime->scdt_valid && runtime->scdt == asserted)
		return 0;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x40, GENMASK(1, 0),
					   asserted ? 0 : BIT(1));
	if (!ret && !asserted)
		ret = it6805_clear_video_output_locked(it6805);
	if (!ret && !asserted)
		ret = it6805_clear_audio_output_locked(it6805);
	if (ret)
		return ret;

	runtime->scdt = asserted;
	runtime->scdt_valid = true;
	if (asserted) {
		runtime->video_state = IT6805_VIDEO_CONFIRM_SYNC;
		runtime->video_state_delay_polls = IT6805_VIDEO_CONFIRM_POLLS;
	} else {
		runtime->video_state = IT6805_VIDEO_WAIT_SYNC;
		runtime->video_state_delay_polls = 0;
		it6805_invalidate_signal_state(runtime);
	}

	return 0;
}

static int it6805_handle_scdt_change_locked(struct gc555_it6805 *it6805)
{
	bool asserted;
	int ret;

	ret = it6805_read_scdt_locked(it6805, &asserted);
	if (ret)
		return ret;

	return it6805_apply_scdt_locked(it6805, asserted);
}

static int it6805_probe_wait_sync_locked(struct gc555_it6805 *it6805)
{
	bool asserted;
	int ret;

	if (it6805->runtime.video_state != IT6805_VIDEO_WAIT_SYNC)
		return 0;

	ret = it6805_read_scdt_locked(it6805, &asserted);
	if (ret || !asserted)
		return ret;

	return it6805_apply_scdt_locked(it6805, true);
}

static int
it6805_handle_avmute_irq_locked(struct gc555_it6805 *it6805, u8 reg10)
{
	struct it6805_video_output *output = &it6805->runtime.output;
	u8 avmute;
	int ret;

	if (!(reg10 & (IT6805_COMMON_IRQ_AVMUTE_SET |
		       IT6805_COMMON_IRQ_AVMUTE_CLEAR)) || !output->enabled)
		return 0;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xaa, &avmute);
	if (ret)
		return ret;

	output->unmuted = false;
	if (!(avmute & BIT(3)))
		return 0;

	ret = it6805_update_bits_locked(it6805, 0x4f, 0xa0, 0xa0);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xc5, BIT(7), BIT(7));
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);

	return ret;
}

static int
it6805_ack_common_irq_locked(struct gc555_it6805 *it6805,
			     struct it6805_common_irq *irq)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x10, &irq->reg10);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x10, irq->reg10);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x11, &irq->reg11);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x11, irq->reg11 & 0xbf);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x12, &irq->reg12);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x12, irq->reg12 & 0x7f);
	if (!ret && (irq->reg12 & IT6805_PACKET_IRQ_AUX))
		ret = it6805_read_locked(it6805, 0x1a, &irq->reg1a);
	if (!ret && (irq->reg12 & IT6805_PACKET_IRQ_AUX))
		ret = it6805_read_locked(it6805, 0x1b, &irq->reg1b);
	if (!ret && (irq->reg12 & IT6805_PACKET_IRQ_AUX))
		ret = it6805_write_locked(it6805, 0x12,
					  IT6805_PACKET_IRQ_AUX);
	if (!ret && (irq->reg12 & IT6805_PACKET_IRQ_AVI))
		ret = it6805_set_bank_locked(it6805, 0x02);
	if (!ret && (irq->reg12 & IT6805_PACKET_IRQ_AVI))
		ret = it6805_read_locked(it6805, 0x14, &irq->avi_checksum);
	if (!ret && (irq->reg12 & IT6805_PACKET_IRQ_AVI))
		ret = it6805_read_locked(it6805, 0x15, &irq->avi_db1);
	if (!ret && (irq->reg12 & IT6805_PACKET_IRQ_AVI))
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x1d, &irq->reg1d);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x1d, irq->reg1d);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xd4, &irq->regd4);
	if (!ret)
		ret = it6805_write_locked(it6805, 0xd4, irq->regd4);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xd5, &irq->regd5);
	if (!ret)
		ret = it6805_write_locked(it6805, 0xd5, irq->regd5);
	if (ret)
		return ret;

	it6805->runtime.last_common_irq = *irq;
	if (irq->reg12 & IT6805_PACKET_IRQ_AVI) {
		it6805->runtime.avi_change_pending = true;
	}
	ret = it6805_handle_avmute_irq_locked(it6805, irq->reg10);
	if (ret)
		return ret;
	if (irq->reg10 & IT6805_COMMON_IRQ_AUDIO)
		it6805->runtime.audio.change_pending = true;
	if (irq->reg10 & IT6805_COMMON_IRQ_SCDT)
		return it6805_handle_scdt_change_locked(it6805);

	return 0;
}

/*
 * The ITE6805 under-reads pixel clock and frame rate together. 1440p120
 * arrives as ~100 fps at ~413 MHz; 483.4 MHz is the CVT-RB clock. Rescale
 * against that clock before the nearest-rate pick, or 100 wins over 120.
 * A rate that is already a standard step is left alone.
 */
static bool it6805_rescale_frame_rate(u32 frame_rate_hz, u32 pixel_clock_khz,
				      u8 *rate)
{
	static const u32 std_khz[] = {
		25200, 27000, 74250, 148500, 241500, 297000, 483400, 594000,
	};
	static const u8 std_fps[] = { 24, 25, 30, 50, 60, 120, 144, 180, 240 };
	u32 ref = 0, ref_diff = 0, scaled, i;

	if (!rate)
		return false;
	if (!frame_rate_hz || !pixel_clock_khz)
		return false;
	for (i = 0; i < ARRAY_SIZE(std_fps); i++) {
		if (frame_rate_hz == std_fps[i]) {
			*rate = std_fps[i];
			return true;
		}
	}
	/*
	 * Only an under-read of the nearest standard clock counts. 1440p120
	 * arrives as ~100 fps at ~413 MHz; 483.4 MHz is the CVT-RB clock. Rescale
	 * against that clock before the nearest-rate pick, or 100 wins over 120.
	 * A rate that is already a standard step is left alone.
	 */
	for (i = 0; i < ARRAY_SIZE(std_khz); i++) {
		u32 diff = pixel_clock_khz > std_khz[i] ?
			pixel_clock_khz - std_khz[i] : std_khz[i] - pixel_clock_khz;

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
				     u32 pixel_clock_khz);

static void it6805_framerate_selfcheck(void)
{
	static bool done;
	u8 rate;

	if (done)
		return;
	done = true;
	WARN_ON(!it6805_rescale_frame_rate(100, 413228, &rate) || rate != 120);
	WARN_ON(!it6805_rescale_frame_rate(102, 411856, &rate) || rate != 120);
	WARN_ON(!it6805_rescale_frame_rate(51, 126457, &rate) || rate != 60);
	WARN_ON(!it6805_rescale_frame_rate(60, 126457, &rate) || rate != 60);
	WARN_ON(it6805_normalize_frame_rate(12200, 494440) != 120);
	WARN_ON(it6805_normalize_frame_rate(11856, 580000) != 120);
	WARN_ON(it6805_normalize_frame_rate(11600, 580000) != 120);
	WARN_ON(it6805_normalize_frame_rate(18000, 445500) != 180);
	WARN_ON(it6805_normalize_frame_rate(17900, 445500) != 180);
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
	if (it6805_rescale_frame_rate(DIV_ROUND_CLOSEST(frame_rate_x100, 100U),
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
	 * Allow wider tolerance (up to 5%) for high-refresh modes (>= 100 Hz).
	 * This cleanly accommodates genlocked retro scalers (e.g. RetroTINK 4K /
	 * OSSC Pro doubling off-spec ~58-59 Hz PAL 60Hz or arcade signals to
	 * ~116-119 Hz) without snapping into neighboring rates.
	 */
	if (best_rate >= 100) {
		if (best_error <= best_rate * 5U)
			return best_rate;
	} else {
		if (best_error <= best_rate * 3U)
			return best_rate;
	}

	return min_t(u32, DIV_ROUND_CLOSEST(frame_rate_x100, 100U),
		     U8_MAX);
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
	case 31:
	case 96:
	case 101:
	case 106:
		return 50;
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 16:
	case 97:
	case 102:
	case 107:
		return 60;
	case 41:
	case 64:
		return 100;
	case 47:
	case 63:
		return 120;
	default:
		return 0;
	}
}

static int it6805_refresh_video_timing_locked(struct gc555_it6805 *it6805)
{
	struct it6805_video_timing timing = {};
	u32 tmds_sample_sum = IT6805_TMDS_SAMPLE_BIAS;
	u32 pclk_sample_sum = 0;
	u32 tmds_factor;
	u8 reg98 = 0;
	u8 reg43 = 0;
	u8 reg14 = 0;
	u8 sample = 0;
	u8 high = 0;
	u8 low = 0;
	bool pclk_latch_open = false;
	unsigned int i;
	int cleanup_ret;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x14, &reg14);
	if (!ret && (reg14 & (BIT(6) | BIT(7))))
		it6805_set_hdmi2_phy_locked(it6805, true);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x90, 0x8f);
	for (i = 0; !ret && i < IT6805_TMDS_SAMPLE_COUNT; i++) {
		msleep(IT6805_TMDS_SAMPLE_DELAY_MS);
		ret = it6805_read_locked(it6805, 0x48, &sample);
		if (!ret)
			tmds_sample_sum += sample;
	}
	if (!ret)
		ret = it6805_read_locked(it6805, 0x43, &reg43);

	for (i = 0; !ret && i < IT6805_PCLK_SAMPLE_COUNT; i++) {
		msleep(IT6805_PCLK_SAMPLE_DELAY_MS);
		ret = it6805_update_bits_locked(it6805, 0x9a, BIT(7), 0);
		if (!ret)
			pclk_latch_open = true;
		if (!ret)
			ret = it6805_read_locked(it6805, 0x9a, &high);
		if (!ret)
			ret = it6805_read_locked(it6805, 0x99, &low);
		if (!ret)
			pclk_sample_sum += (((u32)high << 8) | low) & 0x7ff;
		if (!ret)
			ret = it6805_update_bits_locked(it6805, 0x9a, BIT(7),
							 BIT(7));
		if (!ret)
			pclk_latch_open = false;
	}
	if (pclk_latch_open) {
		cleanup_ret = it6805_update_bits_locked(it6805, 0x9a, BIT(7),
							 BIT(7));
		if (!ret)
			ret = cleanup_ret;
	}

	if (!ret)
		ret = it6805_read_locked(it6805, 0x98, &reg98);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x9c, &high);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x9b, &low);
	if (!ret)
		timing.htotal = (((u16)high << 8) | low) & 0x3fff;
	if (!ret)
		ret = it6805_read_locked(it6805, 0x9e, &high);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x9d, &low);
	if (!ret)
		timing.hactive = (((u16)high << 8) | low) & 0x3fff;
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa1, &high);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa0, &low);
	if (!ret)
		timing.hfront = low | ((u16)(high & 0xf0) << 4);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa1, &high);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x9f, &low);
	if (!ret)
		timing.hsync = (((u16)high << 8) | low) & 0x1ff;
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa3, &high);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa2, &low);
	if (!ret)
		timing.vtotal = (((u16)high << 8) | low) & 0x3fff;
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa5, &high);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa4, &low);
	if (!ret)
		timing.vactive = (((u16)high << 8) | low) & 0x3fff;
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa8, &high);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa7, &low);
	if (!ret)
		timing.vfront = low | ((u16)(high & 0xf0) << 4);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa8, &high);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xa6, &low);
	if (!ret)
		timing.vsync = (((u16)high << 8) | low) & 0x1ff;
	if (!ret)
		ret = it6805_read_locked(it6805, 0xaa,
					  &timing.sync_status_first);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xaa,
					  &timing.sync_status_second);
	if (ret)
		return ret;
	if (!pclk_sample_sum || !timing.htotal || !timing.vtotal)
		return -EAGAIN;

	timing.pixel_clock_khz =
		(u64)it6805->reference_khz * IT6805_PCLK_SAMPLE_SCALE *
		IT6805_PCLK_SAMPLE_COUNT / pclk_sample_sum;
	if (reg43 & BIT(7))
		tmds_factor = 0x2800;
	else if (reg43 & BIT(6))
		tmds_factor = 0x1400;
	else if (reg43 & BIT(5))
		tmds_factor = 0x0a00;
	else
		tmds_factor = 0x0500;
	timing.tmds_clock_khz =
		it6805->reference_khz * tmds_factor / tmds_sample_sum;
	timing.hback = (timing.htotal - timing.hactive - timing.hsync -
			timing.hfront) & 0xffff;
	timing.vback = (timing.vtotal - timing.vactive - timing.vsync -
			timing.vfront) & 0xffff;
	timing.field_rate_x100 =
		(u64)timing.pixel_clock_khz * 100000 /
		timing.htotal / timing.vtotal;
	timing.frame_rate_hz =
		it6805_normalize_frame_rate(timing.field_rate_x100,
					    timing.pixel_clock_khz);
	timing.interlaced = reg98 & BIT(1);
	if (timing.interlaced && timing.frame_rate_hz)
		timing.frame_rate_hz /= 2;
	timing.tmds_mode = reg43;

	if (timing.hactive == 1440 && timing.vactive == 900 &&
	    (timing.pixel_clock_khz < 95000 ||
	     timing.pixel_clock_khz >= 120000))
		return -EAGAIN;
	if (timing.hactive < 320 ||
	    timing.vactive * (timing.interlaced ? 2U : 1U) < 240 ||
	    !timing.pixel_clock_khz)
		return -EAGAIN;

	timing.valid = true;
	it6805->runtime.timing = timing;
	/*
	 * 1440p120 is 483 MHz. The scrambled PHY is 0x04; init leaves the
	 * HDMI 1.4 value 0x02. Follow reg 0x14[7:6] and the measured clock.
	 */
	return it6805_set_hdmi2_phy_locked(it6805,
					   (reg14 & (BIT(6) | BIT(7))) ||
					   timing.pixel_clock_khz >= 340000);
}

static enum gc555_video_hdr_mode
it6805_classify_hdr(const struct it6805_runtime *runtime,
		    const struct it6805_drm_info *drm)
{
	u8 eotf;
	bool bt2020;

	eotf = drm->static_metadata.eotf & IT6805_DRM_EOTF_MASK;
	if (drm->raw[0] != IT6805_DRM_PACKET_TYPE ||
	    (eotf != IT6805_DRM_EOTF_PQ && eotf != IT6805_DRM_EOTF_HLG))
		return GC555_VIDEO_HDR_SDR;

	if (eotf == IT6805_DRM_EOTF_HLG)
		return GC555_VIDEO_HDR_HLG;

	bt2020 = runtime->avi.colorimetry == 3 &&
		 runtime->avi.extended_colorimetry >= 5 &&
		 runtime->avi.extended_colorimetry <= 6;

	return bt2020 ? GC555_VIDEO_HDR_PQ_BT2020 : GC555_VIDEO_HDR_PQ;
}

static void it6805_decode_drm_static_metadata(struct it6805_drm_info *drm)
{
	const u8 *payload = &drm->raw[4];
	unsigned int i;

	drm->static_metadata.eotf = payload[0];
	drm->static_metadata.descriptor_id = payload[1];
	for (i = 0; i < ARRAY_SIZE(drm->static_metadata.display_primaries_x);
	     i++) {
		drm->static_metadata.display_primaries_x[i] =
			get_unaligned_le16(&payload[2 + i * 4]);
		drm->static_metadata.display_primaries_y[i] =
			get_unaligned_le16(&payload[4 + i * 4]);
	}
	drm->static_metadata.white_point_x = get_unaligned_le16(&payload[14]);
	drm->static_metadata.white_point_y = get_unaligned_le16(&payload[16]);
	drm->static_metadata.max_display_mastering_luminance =
		get_unaligned_le16(&payload[18]);
	drm->static_metadata.min_display_mastering_luminance =
		get_unaligned_le16(&payload[20]);
	drm->static_metadata.max_cll = get_unaligned_le16(&payload[22]);
	drm->static_metadata.max_fall = get_unaligned_le16(&payload[24]);
}

static bool
it6805_avi_material_changed(const struct it6805_avi_info *old,
			    const struct it6805_avi_info *new)
{
	return old->colorspace != new->colorspace ||
	       old->colorimetry != new->colorimetry ||
	       old->extended_colorimetry != new->extended_colorimetry ||
	       old->rgb_quantization != new->rgb_quantization ||
	       old->ycc_quantization != new->ycc_quantization ||
	       old->raw[0] != new->raw[0] || old->raw[1] != new->raw[1];
}

static int it6805_refresh_avi_info_locked(struct gc555_it6805 *it6805)
{
	struct it6805_runtime *runtime = &it6805->runtime;
	struct it6805_avi_info avi = {};
	bool reconfigure;
	u8 frame_rate;
	size_t i;
	int ret;

	ret = it6805_set_bank_locked(it6805, 0x02);
	for (i = 0; !ret && i < ARRAY_SIZE(avi.raw); i++)
		ret = it6805_read_locked(it6805, 0x14 + i, &avi.raw[i]);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		return ret;

	avi.colorspace = (avi.raw[1] >> 5) & 0x03;
	avi.colorimetry = avi.raw[2] >> 6;
	avi.extended_colorimetry = (avi.raw[3] >> 4) & 0x07;
	avi.rgb_quantization = (avi.raw[3] >> 2) & 0x03;
	avi.vic = avi.raw[4] & 0x7f;
	avi.ycc_quantization = avi.raw[5] >> 6;
	avi.scan_info = avi.raw[1] & 0x03;
	if (!avi.rgb_quantization)
		avi.rgb_quantization = 2U - (avi.vic > 1);

	frame_rate = it6805_vic_frame_rate(avi.vic);
	if (runtime->timing.interlaced && frame_rate)
		frame_rate /= 2;
	if (frame_rate)
		runtime->timing.frame_rate_hz = frame_rate;
	avi.valid = true;
	reconfigure = runtime->avi_change_pending && runtime->avi.valid &&
		      runtime->output.configured &&
		      it6805_avi_material_changed(&runtime->avi, &avi);
	runtime->avi = avi;
	if (runtime->drm.present)
		runtime->drm.hdr_mode =
			it6805_classify_hdr(runtime, &runtime->drm);
	runtime->avi_change_pending = false;
	if (reconfigure)
		runtime->output = (struct it6805_video_output){};

	return 0;
}

static int it6805_read_drm_infoframe_locked(struct gc555_it6805 *it6805,
					    u8 *frame)
{
	int restore_ret;
	int ret;
	size_t i;

	ret = it6805_set_bank_locked(it6805, 0x02);
	for (i = 0; !ret && i < IT6805_DRM_INFOFRAME_SIZE; i++)
		ret = it6805_read_locked(it6805, 0x24 + i, &frame[i]);
	restore_ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = restore_ret;

	return ret;
}

static int it6805_refresh_drm_info_locked(struct gc555_it6805 *it6805)
{
	struct it6805_drm_info *drm = &it6805->runtime.drm;
	u8 frame[IT6805_DRM_INFOFRAME_SIZE];
	u8 packet_type;
	u8 irq;
	int restore_ret;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x11, &irq);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x11,
					  irq & IT6805_DRM_ABSENT_IRQ);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, 0x02);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x24, &packet_type);
	restore_ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = restore_ret;
	if (ret)
		return ret;

	if ((irq & IT6805_DRM_ABSENT_IRQ) || !packet_type) {
		*drm = (struct it6805_drm_info){};
		return 0;
	}

	if (!drm->present) {
		drm->stable_polls++;
		if (drm->stable_polls < IT6805_DRM_STABLE_POLLS)
			return 0;
		drm->stable_polls = 0;
		drm->present = true;
	}

	ret = it6805_read_drm_infoframe_locked(it6805, frame);
	if (ret)
		return ret;

	memcpy(drm->raw, frame, sizeof(drm->raw));
	it6805_decode_drm_static_metadata(drm);
	drm->hdr_mode = it6805_classify_hdr(&it6805->runtime, drm);

	return 0;
}

static u32 it6805_active_width(const struct it6805_runtime *runtime)
{
	u32 width = runtime->timing.hactive;

	if (runtime->avi.colorspace == IT6805_COLORSPACE_YCBCR420)
		width *= 2;

	return width;
}

static u32 it6805_active_height(const struct it6805_runtime *runtime)
{
	return runtime->timing.vactive *
		(runtime->timing.interlaced ? 2U : 1U);
}

static bool
it6805_use_high_bandwidth_output(const struct it6805_runtime *runtime)
{
	u32 width = it6805_active_width(runtime);

	if (runtime->avi.colorspace == IT6805_COLORSPACE_YCBCR420)
		return false;

	/*
	 * From official AVXGC573_x64.sys Windows driver:
	 * HActive <= 1920 && PCLK <= 150000 -> SDR Mode (Single-pixel TTL)
	 * Otherwise -> DDR Mode (Dual-pixel / high-bandwidth TTL)
	 */
	return width > 1920 ||
	       runtime->timing.pixel_clock_khz > IT6805_HIGH_BANDWIDTH_PCLK_KHZ;
}

static int
it6805_apply_ttl_video_path_locked(struct gc555_it6805 *it6805,
				    u8 pixel_mode, bool set_colorspace,
				    enum it6805_video_colorspace colorspace)
{
	u8 pixel_value;
	int ret;

	if (!pixel_mode)
		pixel_value = 0;
	else if (pixel_mode == 1)
		pixel_value = BIT(1);
	else
		return -EOPNOTSUPP;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xc0, 0x06, 0x02);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xc1, BIT(1),
						   pixel_value);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xc1, BIT(5), 0);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret && set_colorspace)
		ret = it6805_update_bits_locked(it6805, 0x6b, 0x0c,
						   colorspace << 2);

	return ret;
}

static int
it6805_apply_lvds_video_path_locked(struct gc555_it6805 *it6805,
				     u8 path_mode)
{
	u8 pixel_value;
	int ret;

	if (path_mode == 1)
		pixel_value = 0;
	else if (path_mode == 2)
		pixel_value = BIT(4);
	else
		return -EOPNOTSUPP;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret && path_mode == 2)
		ret = it6805_update_bits_locked(it6805, 0xc0, BIT(0), BIT(0));
	if (!ret)
		ret = it6805_set_bank_locked(it6805, 0x05);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xd1, BIT(0), 0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xd1, 0x0c, BIT(2));
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xda, BIT(4), 0);
	if (!ret)
		ret = it6805_write_locked(it6805, 0xd0, 0xf3);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xbd, 0x30,
						   pixel_value);
	if (!ret)
		ret = it6805_write_locked(it6805, 0xbe, 0x00);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xfe, BIT(4), BIT(4));
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);

	return ret;
}

static int it6805_configure_video_output_locked(struct gc555_it6805 *it6805)
{
	struct it6805_video_output output = {};
	bool high_bandwidth = it6805_use_high_bandwidth_output(&it6805->runtime);
	u8 regc4 = 0;
	int ret;

	output.lvds_path_mode = 1;
	if (it6805->runtime.avi.colorspace == IT6805_COLORSPACE_YCBCR420) {
		output.lvds_path_mode = 2;
		output.lvds_pixel_mode = 1;
	} else if (high_bandwidth) {
		output.ttl_pixel_mode = 1;
		output.lvds_path_mode = 2;
		output.lvds_pixel_mode = 1;
		regc4 = BIT(4);
	}

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x64, BIT(2), 0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x64, BIT(1), BIT(1));
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x64, BIT(1), 0);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, 0x05);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x20, BIT(6), BIT(6));
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xc0, BIT(0), 0);
	if (!ret)
		ret = it6805_apply_ttl_video_path_locked(
			it6805, output.ttl_pixel_mode, false,
			it6805->runtime.avi.colorspace);
	if (!ret)
		ret = it6805_apply_lvds_video_path_locked(
			it6805, output.lvds_path_mode);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_write_locked(it6805, 0xc4, regc4);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x64, BIT(2), 0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x64, BIT(1), BIT(1));
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x64, BIT(1), 0);
	if (ret)
		return ret;

	output.configured = true;
	it6805->runtime.output = output;

	return 0;
}

static int it6805_set_pixel_clock_gate_locked(struct gc555_it6805 *it6805)
{
	u8 pixel_selector;
	u8 reg1b;
	u8 regc0;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x1b, &reg1b);
	if (ret)
		return ret;

	pixel_selector = (0x04010201U >> ((reg1b >> 1) & 0x18)) & 0xff;
	if (!pixel_selector)
		return -ERANGE;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_update_bits_locked(
			it6805, 0xb0, BIT(0),
			it6805->runtime.timing.pixel_clock_khz / pixel_selector >
				24999 ? BIT(0) : 0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xc0, &regc0);
	if (!ret && !(regc0 & BIT(0)))
		ret = it6805_update_bits_locked(it6805, 0xb0, BIT(0), 0);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);

	return ret;
}

static int it6805_set_color_depth_locked(struct gc555_it6805 *it6805)
{
	u64 table = 0x2010000000000ULL;
	u8 reg98;
	u8 value;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x98, &reg98);
	if (ret)
		return ret;

	value = table >> ((reg98 >> 1) & 0x38);
	if (reg98 > 0x6f)
		value = 0;
	it6805->input_color_depth_code = reg98 & 0xf0;

	return it6805_update_bits_locked(it6805, 0x6b, 0x03, value);
}

static bool it6805_pair_proves_hdmi(struct gc555_it6805 *it6805)
{
	const struct it6805_runtime *runtime = &it6805->runtime;
	bool is_hdmi;

	if (runtime->avi.colorspace != IT6805_COLORSPACE_YCBCR444 ||
	    runtime->avi.vic != 97 || it6805_active_width(runtime) != 3840 ||
	    it6805_active_height(runtime) != 2160 ||
	    runtime->timing.frame_rate_hz != 60 ||
	    runtime->timing.pixel_clock_khz <= 500000 ||
	    runtime->timing.pixel_clock_khz >= 621000 ||
	    runtime->timing.interlaced || runtime->output.ttl_pixel_mode != 1)
		return false;

	if (gc555_link_tx_is_hdmi(it6805->gc555, 1, &is_hdmi))
		return false;

	return is_hdmi;
}

static int it6805_detect_dvi_locked(struct gc555_it6805 *it6805,
				    bool *is_dvi)
{
	u8 mode;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x13, &mode);
	if (ret)
		return ret;

	*is_dvi = !(mode & BIT(1));
	/* This exact 4K60 link can report DVI despite an HDMI TX protocol. */
	if (*is_dvi && it6805_pair_proves_hdmi(it6805))
		*is_dvi = false;

	return 0;
}

static int it6805_release_video_tristate_locked(struct gc555_it6805 *it6805)
{
	u8 reg98 = 0;
	u8 regc0 = 0;
	u8 reg = 0;
	u8 value = 0;
	u8 depth;
	bool write_tail = true;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x98, &reg98);
	depth = reg98 & 0xf0;
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xc5, BIT(7), 0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xc6, BIT(7), 0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xc0, &regc0);
	if (!ret && !(regc0 & BIT(0))) {
		bool is_10bit = (depth == 0x10 || depth == 0x50);
		bool is_12bit = (depth == 0x20 || depth == 0x60);

		reg = 0xc5;
		switch (regc0 & 0xc0) {
		case 0x80:
			value = is_12bit ? 0x23 :
				is_10bit ? 0x63 : 0x03;
			break;
		case 0x40:
		case 0x00:
			value = is_12bit ? 0x38 :
				is_10bit ? 0x78 : 0x18;
			break;
		default:
			write_tail = false;
			break;
		}
	} else if (!ret) {
		bool is_10bit = (depth == 0x10 || depth == 0x50);

		reg = 0xc6;
		value = is_10bit ? 0x40 : 0;
	}
	if (!ret && write_tail)
		ret = it6805_write_locked(it6805, reg, value);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);

	return ret;
}

static int it6805_enable_video_output_locked(struct gc555_it6805 *it6805)
{
	struct it6805_runtime *runtime = &it6805->runtime;
	bool is_dvi;
	u8 reg48;
	int ret;

	ret = it6805_detect_dvi_locked(it6805, &is_dvi);
	if (!ret)
		ret = it6805_set_pixel_clock_gate_locked(it6805);
	if (!ret)
		ret = it6805_update_bits_locked(
			it6805, 0x6b, 0x30,
			is_dvi ? BIT(4) : runtime->avi.colorspace << 4);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x6b, BIT(0),
						   is_dvi ? BIT(0) : 0);
	if (!ret && is_dvi)
		ret = it6805_read_locked(it6805, 0x48, &reg48);
	if (!ret && is_dvi)
		runtime->avi.colorimetry = reg48 < 0x34 ? 2 : 1;
	if (!ret)
		ret = it6805_apply_ttl_video_path_locked(
			it6805, runtime->output.ttl_pixel_mode, true,
			runtime->avi.colorspace);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x6e, 0xa0);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x86, 0x00);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x6c, 0x03, 0);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_write_locked(it6805, 0x85, 0x00);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_set_color_depth_locked(it6805);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x4f, 0xa0, 0xa0);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0xc5, BIT(7), BIT(7));
	if (!ret)
		ret = it6805_clear_audio_output_locked(it6805);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x22, BIT(0), BIT(0));
	if (!ret)
		msleep(1);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x22, BIT(0), 0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x10, BIT(1), BIT(1));
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x12, BIT(7), BIT(7));
	if (!ret)
		ret = it6805_release_video_tristate_locked(it6805);
	if (ret)
		return ret;

	runtime->output.is_dvi = is_dvi;
	runtime->output.enabled = true;
	runtime->audio = (struct it6805_audio_runtime){
		.state = IT6805_AUDIO_REQUEST,
	};

	return 0;
}

static int it6805_unmute_video_locked(struct gc555_it6805 *it6805)
{
	u8 reg4f;
	u8 regaa;
	u8 regc0;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x4f, &reg4f);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xaa, &regaa);
	if (ret)
		return ret;
	if ((reg4f & BIT(5)) && (regaa & BIT(3)))
		return -EAGAIN;

	if (reg4f & BIT(5)) {
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
		if (!ret)
			ret = it6805_update_bits_locked(it6805, 0xc5,
							 BIT(0), BIT(0));
		if (!ret)
			ret = it6805_update_bits_locked(it6805, 0xc5, BIT(0), 0);
		if (!ret)
			ret = it6805_read_locked(it6805, 0xc0, &regc0);
		if (!ret)
			ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
		if (!ret && (regc0 & BIT(0))) {
			ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
			if (!ret)
				ret = it6805_update_bits_locked(
					it6805, 0xc5, BIT(0), BIT(0));
			if (!ret)
				ret = it6805_set_bank_locked(it6805,
							 IT6805_BANK_0);
		}
		if (!ret)
			ret = it6805_update_bits_locked(it6805, 0x4f,
							 0xa0, 0xa0);
		if (!ret)
			ret = it6805_update_bits_locked(it6805, 0x4f,
							 0xa0, BIT(7));
	}
	if (ret)
		return ret;

	it6805->runtime.output.unmuted = true;

	return 0;
}

static u32 it6805_audio_delta(u32 left, u32 right)
{
	return left > right ? left - right : right - left;
}

static int it6805_read_audio_clock_locked(struct gc555_it6805 *it6805,
					  u32 *n, u32 *cts)
{
	u8 n_high;
	u8 n_mid;
	u8 n_low;
	u8 cts_low;
	u8 cts_mid;
	u8 cts_high;
	int cleanup_ret;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x86, BIT(0), BIT(0));
	if (!ret)
		ret = it6805_set_bank_locked(it6805, 0x02);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xbe, &n_high);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xbf, &n_mid);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xc0, &n_low);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xc0, &cts_low);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xc1, &cts_mid);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xc2, &cts_high);
	cleanup_ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		return ret;

	*n = (u32)n_high << 12 | (u32)n_mid << 4 | (n_low & 0x0f);
	*cts = (u32)cts_mid << 12 | (u32)cts_high << 4 | (cts_low >> 4);

	return 0;
}

static int it6805_read_audio_format_locked(struct gc555_it6805 *it6805,
					   u8 *b0, u8 *b1, u8 *b2)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xb0, b0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xb1, b1);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xb2, b2);

	return ret;
}

static int it6805_reset_audio_logic_locked(struct gc555_it6805 *it6805)
{
	u8 reg8a;
	unsigned int i;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x22, BIT(1), BIT(1));
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x22, BIT(1), 0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x8a, &reg8a);
	for (i = 0; !ret && i < 4; i++)
		ret = it6805_write_locked(it6805, 0x8a, reg8a);

	return ret;
}

static int it6805_set_audio_tristate_locked(struct gc555_it6805 *it6805,
					    bool tristate)
{
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_1);
	if (!ret)
		ret = it6805_write_locked(it6805, 0xc7,
					  tristate ? 0x7f : 0x00);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);

	return ret;
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
		{ 59, 78, IT6805_AUDIO_RATE_64_KHZ },
		{ 79, 92, IT6805_AUDIO_RATE_88_2_KHZ },
		{ 93, 106, IT6805_AUDIO_RATE_96_KHZ },
		{ 107, 166, IT6805_AUDIO_RATE_128_KHZ },
		{ 167, 182, IT6805_AUDIO_RATE_176_4_KHZ },
		{ 183, 202, IT6805_AUDIO_RATE_192_KHZ },
		{ 225, 320, IT6805_AUDIO_RATE_256_KHZ },
		{ 321, 448, IT6805_AUDIO_RATE_384_KHZ },
		{ 449, 638, IT6805_AUDIO_RATE_512_KHZ },
		{ 639, 894, IT6805_AUDIO_RATE_768_KHZ },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(ranges); i++) {
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

static int it6805_measure_audio_tmds_locked(struct gc555_it6805 *it6805,
					    u32 *clock_khz)
{
	u32 sample_sum = 0;
	u32 factor;
	u8 reg43;
	u8 sample;
	unsigned int i;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	for (i = 0; !ret && i < IT6805_TMDS_SAMPLE_COUNT; i++) {
		msleep(IT6805_AUDIO_TMDS_SAMPLE_DELAY_MS);
		ret = it6805_read_locked(it6805, 0x48, &sample);
		if (!ret)
			sample_sum += sample + 1U;
	}
	if (!ret)
		ret = it6805_read_locked(it6805, 0x43, &reg43);
	if (ret)
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
	*clock_khz = (u64)it6805->reference_khz *
		IT6805_TMDS_SAMPLE_COUNT * factor / sample_sum;

	return 0;
}

static int it6805_update_audio_clock_locked(struct gc555_it6805 *it6805)
{
	struct it6805_audio_runtime *audio = &it6805->runtime.audio;
	u32 tmds_clock_khz;
	u32 rate_khz;
	u32 n;
	u32 cts;
	u8 reg81;
	u8 regb5;
	u8 regb6;
	u8 rate_code;
	int ret;

	ret = it6805_read_audio_clock_locked(it6805, &n, &cts);
	if (!ret)
		ret = it6805_measure_audio_tmds_locked(it6805, &tmds_clock_khz);
	if (ret)
		return ret;

	audio->n = n;
	audio->cts = cts;
	audio->sample_rate_hz = 0;
	it6805->runtime.timing.tmds_clock_khz = tmds_clock_khz;
	if (!cts || !tmds_clock_khz)
		return 0;

	rate_khz = div64_u64((u64)tmds_clock_khz * n,
			     (u64)cts << 7);
	rate_code = it6805_audio_rate_code(rate_khz);
	audio->sample_rate_hz = rate_khz * 1000U;
	audio->selected_rate_code = rate_code;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xb5, &regb5);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xb6, &regb6);
	if (ret)
		return ret;

	audio->receiver_rate_code =
		(regb6 >> 2 & 0x30) | (regb5 & 0x0f);
	if (audio->receiver_rate_code == rate_code) {
		ret = it6805_read_locked(it6805, 0x81, &reg81);
		if (!ret)
			ret = it6805_update_bits_locked(it6805, 0x81,
						 BIT(6), 0);
		if (!ret && (reg81 & BIT(6)))
			ret = it6805_reset_audio_logic_locked(it6805);
		if (!ret)
			audio->rate_mismatch_count = 0;
		return ret;
	}

	audio->rate_mismatch_count++;
	if (audio->rate_mismatch_count < IT6805_AUDIO_RATE_MISMATCH_LIMIT)
		return 0;

	ret = it6805_update_bits_locked(it6805, 0x81, BIT(6), BIT(6));
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x8a, 0x3f,
						 rate_code);
	if (!ret)
		ret = it6805_reset_audio_logic_locked(it6805);
	if (!ret)
		audio->rate_mismatch_count = 0;

	return ret;
}

static int it6805_restart_audio_locked(struct gc555_it6805 *it6805)
{
	struct it6805_audio_runtime *audio = &it6805->runtime.audio;
	int ret;

	ret = it6805_clear_audio_output_locked(it6805);
	if (ret)
		return ret;

	audio->state = IT6805_AUDIO_REQUEST;
	audio->n = 0;
	audio->cts = 0;
	audio->sample_rate_hz = 0;
	audio->format_b0 = 0;
	audio->format_b1 = 0;
	audio->format_b2 = 0;
	audio->channel_count = 0;
	audio->request_delay_polls = 0;
	audio->monitor_delay_polls = 0;
	audio->rate_mismatch_count = 0;
	audio->format_valid = false;
	audio->measurement_refresh_pending = false;
	audio->change_pending = false;
	audio->output_enabled = false;

	return 0;
}

static int it6805_request_audio_locked(struct gc555_it6805 *it6805)
{
	struct it6805_audio_runtime *audio = &it6805->runtime.audio;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x8c, BIT(4), BIT(4));
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x8c, BIT(4), 0);
	if (ret) {
		it6805_set_bank_locked(it6805, IT6805_BANK_0);
		it6805_update_bits_locked(it6805, 0x8c, BIT(4), 0);
		return ret;
	}

	audio->format_b0 = 0;
	audio->format_b1 = 0;
	audio->format_b2 = 0;
	audio->format_valid = false;
	audio->measurement_refresh_pending = true;
	audio->state = IT6805_AUDIO_WAIT_READY;
	audio->request_delay_polls = IT6805_AUDIO_REQUEST_DELAY_POLLS;

	return 0;
}

static int it6805_start_audio_output_locked(struct gc555_it6805 *it6805)
{
	struct it6805_audio_runtime *audio = &it6805->runtime.audio;
	u8 b0;
	u8 b1;
	u8 b2;
	int ret;

	ret = it6805_read_audio_format_locked(it6805, &b0, &b1, &b2);
	if (!ret)
		audio->channel_count =
			it6805_audio_channel_count(b0, b1,
						    audio->channel_count);
	if (!ret)
		ret = it6805_reset_audio_logic_locked(it6805);
	if (!ret)
		ret = it6805_update_audio_clock_locked(it6805);
	if (!ret)
		ret = it6805_set_audio_tristate_locked(it6805, false);
	if (ret) {
		it6805_clear_audio_output_locked(it6805);
		return ret;
	}

	audio->state = IT6805_AUDIO_ON;
	audio->monitor_delay_polls = IT6805_AUDIO_MONITOR_DELAY_POLLS;
	audio->change_pending = false;
	audio->output_enabled = true;

	return 0;
}

static int it6805_probe_audio_ready_locked(struct gc555_it6805 *it6805)
{
	struct it6805_audio_runtime *audio = &it6805->runtime.audio;
	u8 reg10;
	u8 reg19;
	u8 regb1;
	u8 regb9;
	bool ready;
	int ret;

	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xb9, &regb9);
	if (!ret && regb9 > 0x3f)
		ret = it6805_reset_audio_logic_locked(it6805);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x19, &reg19);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x10, &reg10);
	if (!ret)
		ret = it6805_read_locked(it6805, 0xb1, &regb1);
	if (ret)
		return ret;

	ready = reg19 != 0xff && (reg19 & BIT(7)) &&
		(regb1 & BIT(7)) && !(reg10 & IT6805_COMMON_IRQ_AUDIO);
	if (!ready) {
		audio->request_delay_polls = IT6805_AUDIO_REQUEST_DELAY_POLLS;
		return 0;
	}

	return it6805_start_audio_output_locked(it6805);
}

static int it6805_refresh_audio_format_locked(struct gc555_it6805 *it6805)
{
	struct it6805_audio_runtime *audio = &it6805->runtime.audio;
	u8 b0;
	u8 b1;
	u8 b2;
	int ret;

	ret = it6805_read_audio_format_locked(it6805, &b0, &b1, &b2);
	if (!ret)
		ret = it6805_update_bits_locked(it6805, 0x8c, BIT(3),
						 b2 & BIT(1) ? BIT(3) : 0);
	if (ret)
		return ret;

	audio->format_b0 = b0 & 0xf0;
	audio->format_b1 = b1;
	audio->format_b2 = b2 & BIT(1);
	audio->channel_count =
		it6805_audio_channel_count(b0, b1, audio->channel_count);
	audio->format_valid = true;
	audio->measurement_refresh_pending = false;

	return 0;
}

static int it6805_monitor_audio_locked(struct gc555_it6805 *it6805)
{
	struct it6805_audio_runtime *audio = &it6805->runtime.audio;
	u32 bridge_rate_hz;
	u32 n;
	u32 cts;
	u8 b0;
	u8 b1;
	u8 b2;
	bool mismatch;
	int ret;

	if (audio->monitor_delay_polls) {
		audio->monitor_delay_polls--;
		return 0;
	}
	audio->monitor_delay_polls = IT6805_AUDIO_MONITOR_DELAY_POLLS;

	if (audio->measurement_refresh_pending)
		return it6805_refresh_audio_format_locked(it6805);

	ret = it6805_read_audio_clock_locked(it6805, &n, &cts);
	if (!ret)
		ret = it6805_read_audio_format_locked(it6805, &b0, &b1, &b2);
	if (ret)
		return ret;

	mismatch = (b0 & 0xf0) != audio->format_b0 ||
		b1 != audio->format_b1 ||
		(b2 & BIT(1)) != audio->format_b2 ||
		it6805_audio_delta(n, audio->n) > IT6805_AUDIO_N_TOLERANCE;
	if (!mismatch &&
	    !gc555_bridge_get_audio_rate(it6805->gc555, &bridge_rate_hz))
		mismatch = it6805_audio_rate_code(bridge_rate_hz / 1000U) !=
			   audio->selected_rate_code;
	if (!mismatch) {
		audio->cts = cts;
		return 0;
	}

	return it6805_restart_audio_locked(it6805);
}

static int it6805_handle_audio_change_locked(struct gc555_it6805 *it6805)
{
	struct it6805_audio_runtime *audio = &it6805->runtime.audio;
	u32 n;
	u32 cts;
	int ret;

	if (!audio->change_pending)
		return 0;
	if (audio->state != IT6805_AUDIO_ON) {
		audio->change_pending = false;
		return 0;
	}

	ret = it6805_read_audio_clock_locked(it6805, &n, &cts);
	if (ret)
		return ret;

	if (it6805_audio_delta(n, audio->n) > IT6805_AUDIO_N_TOLERANCE)
		return it6805_restart_audio_locked(it6805);

	audio->n = n;
	audio->cts = cts;
	audio->change_pending = false;

	return 0;
}

static int it6805_service_audio_locked(struct gc555_it6805 *it6805)
{
	struct it6805_audio_runtime *audio = &it6805->runtime.audio;
	int ret;

	if (!it6805->runtime.output.enabled)
		return 0;

	switch (audio->state) {
	case IT6805_AUDIO_OFF:
		return 0;
	case IT6805_AUDIO_REQUEST:
		return it6805_request_audio_locked(it6805);
	case IT6805_AUDIO_WAIT_READY:
		if (audio->request_delay_polls) {
			audio->request_delay_polls--;
			return 0;
		}
		return it6805_probe_audio_ready_locked(it6805);
	case IT6805_AUDIO_ON:
		ret = it6805_handle_audio_change_locked(it6805);
		if (ret || audio->state != IT6805_AUDIO_ON)
			return ret;
		return it6805_monitor_audio_locked(it6805);
	}

	return -EINVAL;
}

static u32 it6805_nominal_audio_rate(const struct it6805_audio_runtime *audio)
{
	switch (audio->selected_rate_code) {
	case IT6805_AUDIO_RATE_32_KHZ:
		return GC555_AUDIO_RATE_32000_HZ;
	case IT6805_AUDIO_RATE_44_1_KHZ:
		return GC555_AUDIO_RATE_44100_HZ;
	case IT6805_AUDIO_RATE_48_KHZ:
		return GC555_AUDIO_RATE_48000_HZ;
	default:
		return audio->sample_rate_hz;
	}
}

static void
it6805_snapshot_audio_format_locked(struct gc555_it6805 *it6805,
				    struct gc555_hdmi_audio_format *format)
{
	const struct it6805_audio_runtime *audio = &it6805->runtime.audio;

	*format = (struct gc555_hdmi_audio_format){};
	format->generation =
		it6805->runtime.reported_audio_format.generation;
	if (audio->state != IT6805_AUDIO_ON || !audio->format_valid ||
	    !audio->output_enabled || !audio->sample_rate_hz)
		return;

	format->rate_hz = it6805_nominal_audio_rate(audio);
	format->channels = audio->channel_count;
	format->channel_allocation = audio->format_b1 & 0x3f;
	format->transport = audio->format_b0 >> 4;
	format->valid = true;
}

static bool
it6805_audio_format_equal(const struct gc555_hdmi_audio_format *left,
			  const struct gc555_hdmi_audio_format *right)
{
	return left->valid == right->valid &&
	       (!left->valid ||
		(left->rate_hz == right->rate_hz &&
		 left->channels == right->channels &&
		 left->channel_allocation == right->channel_allocation &&
		 left->transport == right->transport));
}

static bool
it6805_update_audio_format_locked(struct gc555_it6805 *it6805,
				  struct gc555_hdmi_audio_format *format)
{
	bool changed;

	it6805_snapshot_audio_format_locked(it6805, format);
	changed = !it6805_audio_format_equal(format,
				&it6805->runtime.reported_audio_format);
	if (changed) {
		format->generation =
			it6805->runtime.reported_audio_format.generation + 1;
		it6805->runtime.reported_audio_format = *format;
	} else {
		*format = it6805->runtime.reported_audio_format;
	}

	return changed;
}

static void it6805_publish_audio_format(struct gc555_it6805 *it6805)
{
	struct gc555_hdmi_audio_format format;
	bool changed;

	mutex_lock(&it6805->io_lock);
	changed = it6805_update_audio_format_locked(it6805, &format);
	mutex_unlock(&it6805->io_lock);

	if (changed)
		gc555_audio_hdmi_format_changed(it6805->gc555);
}

static void it6805_tick_video_state(struct it6805_runtime *runtime)
{
	if (runtime->video_state != IT6805_VIDEO_CONFIRM_SYNC)
		return;
	if (runtime->video_state_delay_polls)
		runtime->video_state_delay_polls--;
	if (!runtime->video_state_delay_polls)
		runtime->video_state = IT6805_VIDEO_ACTIVE;
}

static int
it6805_service_common_runtime(struct gc555_it6805 *it6805,
			      const struct it6805_runtime_snapshot *snapshot)
{
	struct it6805_common_irq irq = {};
	int cleanup_ret;
	int drm_ret = 0;
	int ret = 0;

	mutex_lock(&it6805->io_lock);
	if (it6805_common_irq_pending(snapshot))
		ret = it6805_ack_common_irq_locked(it6805, &irq);
	if (!ret)
		ret = it6805_probe_wait_sync_locked(it6805);
	if (!ret)
		it6805_tick_video_state(&it6805->runtime);
	if (!ret &&
	    it6805->runtime.video_state == IT6805_VIDEO_ACTIVE &&
	    !it6805->runtime.timing.valid)
		ret = it6805_refresh_video_timing_locked(it6805);
	if (!ret && it6805->runtime.timing.valid &&
	    (!it6805->runtime.avi.valid ||
	     it6805->runtime.avi_change_pending))
		ret = it6805_refresh_avi_info_locked(it6805);
	if (!ret && it6805->runtime.avi.valid &&
	    !it6805->runtime.output.configured)
		ret = it6805_configure_video_output_locked(it6805);
	if (!ret && it6805->runtime.output.configured &&
	    !it6805->runtime.output.enabled)
		ret = it6805_enable_video_output_locked(it6805);
	if (!ret && it6805->runtime.output.enabled)
		drm_ret = it6805_refresh_drm_info_locked(it6805);
	if (!ret && it6805->runtime.output.enabled)
		ret = it6805_service_audio_locked(it6805);
	if (!ret && it6805->runtime.output.enabled &&
	    !it6805->runtime.output.unmuted)
		ret = it6805_unmute_video_locked(it6805);

	cleanup_ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (!ret)
		ret = drm_ret;
	if (ret)
		it6805_select_bank_locked(it6805, IT6805_BANK_0);
	mutex_unlock(&it6805->io_lock);

	return ret;
}

static unsigned long
it6805_runtime_delay(const struct it6805_runtime *runtime)
{
	unsigned int delay_ms = IT6805_RUNTIME_INTERVAL_MS;

	if (runtime->poll_count < IT6805_RUNTIME_FAST_POLLS)
		delay_ms = IT6805_RUNTIME_FAST_INTERVAL_MS;

	return msecs_to_jiffies(delay_ms);
}

static int it6805_runtime_poll(struct gc555_it6805 *it6805)
{
	struct it6805_runtime_snapshot snapshot = {};
	struct it6805_5v_result power = {};
	int ret;

	ret = it6805_handle_port0_system_irq(it6805, &snapshot, &power);
	if (ret)
		return ret;

	mutex_lock(&it6805->io_lock);
	ret = it6805_handle_port0_eq_irq_locked(it6805,
						snapshot.port0_eq_irq);
	if (!ret)
		ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (ret)
		it6805_select_bank_locked(it6805, IT6805_BANK_0);
	mutex_unlock(&it6805->io_lock);
	if (ret)
		return ret;

	return it6805_service_common_runtime(it6805, &snapshot);
}

static void it6805_runtime_work(struct work_struct *work)
{
	struct it6805_runtime *runtime =
		container_of(to_delayed_work(work), struct it6805_runtime, work);
	struct gc555_it6805 *it6805 =
		container_of(runtime, struct gc555_it6805, runtime);
	int ret;

	if (!atomic_read(&runtime->enabled))
		return;

	ret = it6805_runtime_poll(it6805);
	it6805_publish_audio_format(it6805);
	if (ret && ret != -EAGAIN && ret != -ESHUTDOWN &&
	    atomic_read(&runtime->enabled))
		dev_warn_ratelimited(it6805->gc555->dev,
				     "IT6805 runtime update failed: %d\n", ret);

	runtime->poll_count++;
	if (atomic_read(&runtime->enabled))
		queue_delayed_work(runtime->wq, &runtime->work,
				   it6805_runtime_delay(runtime));
}

static int it6805_runtime_start(struct gc555_it6805 *it6805)
{
	struct it6805_runtime *runtime = &it6805->runtime;

	runtime->wq = alloc_ordered_workqueue("gc555-it6805", WQ_MEM_RECLAIM);
	if (!runtime->wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&runtime->work, it6805_runtime_work);
	atomic_set(&runtime->enabled, 1);
	queue_delayed_work(runtime->wq, &runtime->work,
			   it6805_runtime_delay(runtime));

	return 0;
}

static void it6805_runtime_stop(struct gc555_it6805 *it6805)
{
	struct it6805_runtime *runtime = &it6805->runtime;

	if (!runtime->wq)
		return;

	atomic_set(&runtime->enabled, 0);
	cancel_delayed_work_sync(&runtime->work);
	destroy_workqueue(runtime->wq);
	runtime->wq = NULL;
}

static enum gc555_video_input_class
it6805_video_input_class(u32 width, u32 height)
{
	if (width >= 1920 && height >= 2160)
		return GC555_VIDEO_INPUT_UHD;
	if (height <= 576)
		return GC555_VIDEO_INPUT_SD;
	if (width > 720 || height > 576)
		return GC555_VIDEO_INPUT_HD;

	return GC555_VIDEO_INPUT_SD;
}

static enum gc555_video_colorimetry
it6805_video_colorimetry(const struct it6805_runtime *runtime,
			 u32 width, u32 height)
{
	if ((runtime->avi.colorimetry == 3 &&
	     runtime->avi.extended_colorimetry >= 5 &&
	     runtime->avi.extended_colorimetry <= 6) ||
	    runtime->drm.hdr_mode == GC555_VIDEO_HDR_HLG ||
	    runtime->drm.hdr_mode == GC555_VIDEO_HDR_PQ_BT2020)
		return GC555_VIDEO_COLORIMETRY_BT2020;
	if (it6805_video_input_class(width, height) == GC555_VIDEO_INPUT_SD)
		return GC555_VIDEO_COLORIMETRY_BT601;

	return GC555_VIDEO_COLORIMETRY_BT709;
}

int gc555_it6805_get_video_signal(struct gc555_it6805 *it6805,
				  struct gc555_video_signal *signal)
{
	struct gc555_video_signal sample = {};
	const struct it6805_runtime *runtime;
	int ret = 0;

	if (!signal)
		return -EINVAL;
	if (!it6805)
		return -ENODEV;

	mutex_lock(&it6805->io_lock);
	runtime = &it6805->runtime;
	if (!runtime->scdt_valid || !runtime->scdt ||
	    runtime->video_state != IT6805_VIDEO_ACTIVE) {
		ret = -ENOLINK;
		goto unlock;
	}
	if (!runtime->timing.valid || !runtime->avi.valid ||
	    !runtime->output.configured || !runtime->output.enabled ||
	    !runtime->output.unmuted) {
		ret = -EAGAIN;
		goto unlock;
	}

	sample.width = it6805_active_width(runtime);
	sample.height = it6805_active_height(runtime);
	sample.pixel_clock_khz = runtime->timing.pixel_clock_khz;
	sample.frame_rate_hz = runtime->timing.frame_rate_hz;
	sample.frame_rate_x100 = runtime->timing.field_rate_x100;
	sample.hfrontporch = runtime->timing.hfront;
	sample.hsync = runtime->timing.hsync;
	sample.hbackporch = runtime->timing.hback;
	sample.vfrontporch = runtime->timing.vfront;
	sample.vsync = runtime->timing.vsync;
	sample.vbackporch = runtime->timing.vback;
	sample.cea861_vic = runtime->avi.vic;
	if (!sample.width || !sample.height || !sample.pixel_clock_khz ||
	    !sample.frame_rate_hz) {
		ret = -EAGAIN;
		goto unlock;
	}

	sample.input_class = it6805_video_input_class(sample.width,
						      sample.height);
	switch (runtime->avi.colorspace) {
	case IT6805_COLORSPACE_RGB:
		sample.sampling = GC555_VIDEO_SAMPLING_RGB;
		break;
	case IT6805_COLORSPACE_YCBCR422:
		sample.sampling = GC555_VIDEO_SAMPLING_YUV422;
		break;
	case IT6805_COLORSPACE_YCBCR444:
		sample.sampling = GC555_VIDEO_SAMPLING_YUV444;
		break;
	case IT6805_COLORSPACE_YCBCR420:
		sample.sampling = GC555_VIDEO_SAMPLING_YUV420;
		break;
	}
	if (sample.sampling == GC555_VIDEO_SAMPLING_RGB)
		sample.encoding = runtime->avi.rgb_quantization == 1 ?
			GC555_VIDEO_ENCODING_RGB_LIMITED :
			GC555_VIDEO_ENCODING_RGB_FULL;
	else
		sample.encoding = GC555_VIDEO_ENCODING_YUV;
	sample.colorimetry = it6805_video_colorimetry(runtime, sample.width,
						       sample.height);
	sample.hdr_mode = runtime->drm.hdr_mode;
	sample.interlaced = runtime->timing.interlaced;
	sample.dual_pixel = runtime->output.lvds_pixel_mode == 1;
	sample.ddr = runtime->output.ttl_pixel_mode == 1;
	*signal = sample;

unlock:
	mutex_unlock(&it6805->io_lock);
	return ret;
}

int gc555_it6805_get_input_power(struct gc555_it6805 *it6805, bool *present)
{
	u8 status;
	int cleanup_ret;
	int ret;

	if (!present)
		return -EINVAL;
	if (!it6805)
		return -ENODEV;

	mutex_lock(&it6805->io_lock);
	ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = it6805_read_locked(it6805, 0x13, &status);
	cleanup_ret = it6805_set_bank_locked(it6805, IT6805_BANK_0);
	if (!ret)
		ret = cleanup_ret;
	if (ret)
		it6805_select_bank_locked(it6805, IT6805_BANK_0);
	mutex_unlock(&it6805->io_lock);
	if (ret)
		return ret;

	*present = status & BIT(0);
	return 0;
}

int gc555_it6805_get_audio_format(struct gc555_it6805 *it6805,
				  struct gc555_hdmi_audio_format *format)
{
	bool changed;

	if (!it6805 || !format)
		return -EINVAL;

	mutex_lock(&it6805->io_lock);
	changed = it6805_update_audio_format_locked(it6805, format);
	mutex_unlock(&it6805->io_lock);
	if (changed)
		gc555_audio_hdmi_format_changed(it6805->gc555);

	return format->valid ? 0 : -ENODATA;
}

static void gc555_it6805_release(struct gc555_it6805 *it6805)
{
	if (!it6805)
		return;
	it6805_runtime_stop(it6805);

	if (!IS_ERR_OR_NULL(it6805->edid_regmap))
		regmap_exit(it6805->edid_regmap);
	if (!IS_ERR_OR_NULL(it6805->edid_client))
		i2c_unregister_device(it6805->edid_client);
	if (!IS_ERR_OR_NULL(it6805->regmap))
		regmap_exit(it6805->regmap);
	if (!IS_ERR_OR_NULL(it6805->client))
		i2c_unregister_device(it6805->client);
}

static void it6805_reset_runtime(struct gc555_it6805 *it6805)
{
	memset(&it6805->runtime, 0, sizeof(it6805->runtime));
	it6805->bank_valid = false;
	it6805->input_color_depth_code = 0;
	it6805->oclk_khz = 0;
	it6805->reference_khz = 0;
}

static int
it6805_restore_hardware(struct gc555_it6805 *it6805,
			struct it6805_caof_result *caof,
			struct it6805_oclk_result *oclk,
			struct it6805_input_path_result *input_path,
			size_t *failed_index)
{
	struct device *dev = it6805->gc555->dev;
	int ret;

	ret = it6805_apply_initial_sequence(it6805, failed_index);
	if (ret) {
		dev_err(dev, "IT6805 initialization failed at operation %zu: %d\n",
			*failed_index, ret);
		return ret;
	}
	ret = it6805_run_caof(it6805, caof);
	if (ret) {
		dev_err(dev, "IT6805 CAOF calibration failed: %d\n", ret);
		return ret;
	}
	ret = it6805_disable_video_output(it6805);
	if (ret) {
		dev_err(dev, "failed to disable IT6805 video output: %d\n",
			ret);
		return ret;
	}
	ret = it6805_calibrate_oclk(it6805, oclk);
	if (ret) {
		dev_err(dev, "failed to calibrate IT6805 oscillator: %d\n",
			ret);
		return ret;
	}
	ret = it6805_initialize_input_path(it6805, input_path);
	if (ret) {
		dev_err(dev, "failed to initialize IT6805 input path: %d\n",
			ret);
		return ret;
	}

	return 0;
}

int gc555_it6805_init(struct gc555_dev *gc555)
{
	static const struct i2c_board_info board_info = {
		I2C_BOARD_INFO("gc555-it6805", IT6805_I2C_ADDRESS),
	};
	static const struct i2c_board_info edid_board_info = {
		I2C_BOARD_INFO("gc555-it6805-edid", IT6805_EDID_I2C_ADDRESS),
	};
	struct it6805_caof_result caof = {};
	struct it6805_input_path_result input_path = {};
	struct it6805_oclk_result oclk = {};
	struct it6805_runtime_snapshot runtime = {};
	struct it6805_runtime_snapshot irq = {};
	struct it6805_runtime_snapshot after = {};
	struct it6805_5v_result irq_power = {};
	struct i2c_adapter *adapter;
	struct gc555_it6805 *it6805;
	size_t failed_index;
	int after_ret;
	int irq_ret;
	int snapshot_ret;
	int ret;

	it6805_framerate_selfcheck();

	adapter = gc555_i2c_get_adapter(gc555, GC555_I2C_BUS_PRIMARY);
	if (!adapter)
		return -ENODEV;

	it6805 = devm_kzalloc(gc555->dev, sizeof(*it6805), GFP_KERNEL);
	if (!it6805)
		return -ENOMEM;
	it6805->gc555 = gc555;
	mutex_init(&it6805->io_lock);
	it6805_reset_runtime(it6805);

	it6805->client = i2c_new_client_device(adapter, &board_info);
	if (IS_ERR(it6805->client))
		return dev_err_probe(gc555->dev, PTR_ERR(it6805->client),
				     "failed to create IT6805 client\n");

	it6805->regmap = regmap_init_i2c(it6805->client,
					 &it6805_regmap_config);
	if (IS_ERR(it6805->regmap)) {
		ret = dev_err_probe(gc555->dev, PTR_ERR(it6805->regmap),
				    "failed to initialize IT6805 regmap\n");
		goto release;
	}
	it6805->edid_client = i2c_new_client_device(adapter,
						    &edid_board_info);
	if (IS_ERR(it6805->edid_client)) {
		ret = dev_err_probe(gc555->dev, PTR_ERR(it6805->edid_client),
				    "failed to create IT6805 EDID client\n");
		goto release;
	}
	it6805->edid_regmap = regmap_init_i2c(it6805->edid_client,
					      &it6805_regmap_config);
	if (IS_ERR(it6805->edid_regmap)) {
		ret = dev_err_probe(gc555->dev, PTR_ERR(it6805->edid_regmap),
				    "failed to initialize IT6805 EDID regmap\n");
		goto release;
	}

	ret = it6805_bulk_read(it6805, IT6805_BANK_0, IT6805_REG_ID_BASE,
			       it6805->identity, sizeof(it6805->identity));
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to read IT6805 identity\n");
		goto release;
	}
	if (!it6805_identity_valid(it6805->identity)) {
		ret = dev_err_probe(gc555->dev, -ENODEV,
				    "unexpected IT6805 identity %4ph\n",
				    it6805->identity);
		goto release;
	}
	ret = it6805_bulk_read(it6805, IT6805_BANK_0, IT6805_REG_REVISION,
			       &it6805->revision, sizeof(it6805->revision));
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to read IT6805 revision\n");
		goto release;
	}

	ret = it6805_restore_hardware(it6805, &caof, &oclk, &input_path,
				      &failed_index);
	if (ret)
		goto release;
	snapshot_ret = it6805_read_runtime_snapshot(it6805, &runtime);
	irq_ret = it6805_handle_port0_system_irq(it6805, &irq, &irq_power);
	if (irq_ret) {
		ret = dev_err_probe(gc555->dev, irq_ret,
				    "failed to handle IT6805 port-0 system IRQ\n");
		goto release;
	}
	after_ret = it6805_read_runtime_snapshot(it6805, &after);

	dev_dbg(gc555->dev, "IT6805 identity %4ph, revision %#x\n",
		it6805->identity, it6805->revision);
	dev_dbg(gc555->dev,
		"IT6805 CAOF completion %02x/%02x status %03x/%03x interrupt %02x/%02x restarts %u\n",
		caof.completion[0], caof.completion[1],
		caof.status[0], caof.status[1],
		caof.interrupt[0], caof.interrupt[1], caof.restarts);
	dev_dbg(gc555->dev, "IT6805 input color-depth code %#x\n",
		it6805->input_color_depth_code);
	dev_dbg(gc555->dev,
		"IT6805 OCLK %u kHz ref %u kHz raw %#x status %02x/%02x polls %u probes %04x/%04x offset %02x counts %04x/%04x scale %02x fallback %u\n",
		oclk.clock_khz, oclk.reference_khz, oclk.raw_count,
		oclk.status_initial, oclk.status_final, oclk.polls,
		oclk.probe[0], oclk.probe[1], oclk.selector_offset,
		oclk.count[0], oclk.count[1], oclk.scale, oclk.fallback);
	dev_dbg(gc555->dev,
		"IT6805 OCLK registers 91=%02x 92=%02x fd=%02x 44=%02x 45=%02x 46=%02x 47=%02x\n",
		oclk.reg91, oclk.reg92, oclk.regfd, oclk.reg44,
		oclk.reg45, oclk.reg46, oclk.reg47);
	dev_dbg(gc555->dev,
		"IT6805 EDID checksums %02x/%02x ports %02x/%02x physical offset %02x mismatches %u\n",
		input_path.block_checksum[0], input_path.block_checksum[1],
		input_path.port_checksum[0], input_path.port_checksum[1],
		input_path.physical_address_offset,
		input_path.edid_mismatches);
	dev_dbg(gc555->dev,
		"IT6805 input path port %u status 13=%02x 16=%02x TTL c0=%02x c1=%02x 5V=%u HPD=%u->%u actions=%u\n",
		input_path.selected_port, input_path.status13,
		input_path.status16, input_path.ttl_c0, input_path.ttl_c1,
		input_path.five_volt, input_path.hpd_before,
		input_path.hpd_after, input_path.hpd_actions);
	if (snapshot_ret)
		dev_dbg(gc555->dev,
			"failed to read IT6805 runtime snapshot: %d\n",
			snapshot_ret);
	else
		dev_dbg(gc555->dev,
			"IT6805 runtime IRQ sys=%4ph eq=%02x common=%7ph status=%3ph scdt=%02x\n",
			runtime.port0_sys_irq, runtime.port0_eq_irq,
			runtime.common_irq, runtime.port0_status, runtime.scdt);
	if (irq_ret == -EAGAIN)
		dev_dbg(gc555->dev,
			"IT6805 port-0 5V IRQ subset inactive; no IRQ acknowledged\n");
	else
		dev_dbg(gc555->dev,
			"IT6805 port-0 5V IRQ handled sys=%4ph 5V=%u HPD actions=%u video state=%u\n",
			irq.port0_sys_irq, irq_power.present,
			irq_power.hpd_actions, it6805->runtime.video_state);
	if (after_ret)
		dev_dbg(gc555->dev,
			"failed to read post-IRQ IT6805 snapshot: %d\n",
			after_ret);
	else
		dev_dbg(gc555->dev,
			"IT6805 post-IRQ sys=%4ph eq=%02x common=%7ph status=%3ph scdt=%02x\n",
			after.port0_sys_irq, after.port0_eq_irq,
			after.common_irq, after.port0_status, after.scdt);
	ret = it6805_runtime_start(it6805);
	if (ret) {
		ret = dev_err_probe(gc555->dev, ret,
				    "failed to start IT6805 runtime worker\n");
		goto release;
	}
	gc555->it6805 = it6805;
	return 0;

release:
	gc555_it6805_release(it6805);
	return ret;
}

void gc555_it6805_cleanup(struct gc555_dev *gc555)
{
	struct gc555_it6805 *it6805;

	if (!gc555 || !gc555->it6805)
		return;

	it6805 = gc555->it6805;
	gc555->it6805 = NULL;
	gc555_it6805_release(it6805);
}

void gc555_it6805_suspend(struct gc555_dev *gc555)
{
	if (gc555 && gc555->it6805)
		it6805_runtime_stop(gc555->it6805);
}

int gc555_it6805_resume(struct gc555_dev *gc555)
{
	struct it6805_input_path_result input_path = {};
	struct it6805_runtime_snapshot irq = {};
	struct it6805_5v_result irq_power = {};
	struct it6805_caof_result caof = {};
	struct it6805_oclk_result oclk = {};
	struct gc555_it6805 *it6805;
	size_t failed_index;
	u8 identity[IT6805_ID_LENGTH];
	int irq_ret;
	int ret;

	if (!gc555 || !gc555->it6805)
		return -ENODEV;
	it6805 = gc555->it6805;

	it6805_reset_runtime(it6805);
	ret = it6805_bulk_read(it6805, IT6805_BANK_0,
			       IT6805_REG_ID_BASE, identity, sizeof(identity));
	if (ret)
		return ret;
	if (!it6805_identity_valid(identity))
		return -ENODEV;

	ret = it6805_bulk_read(it6805, IT6805_BANK_0, IT6805_REG_REVISION,
			       &it6805->revision, sizeof(it6805->revision));
	if (ret)
		return ret;
	ret = it6805_restore_hardware(it6805, &caof, &oclk, &input_path,
				      &failed_index);
	if (ret)
		return ret;

	irq_ret = it6805_handle_port0_system_irq(it6805, &irq, &irq_power);
	if (irq_ret)
		return irq_ret;

	return it6805_runtime_start(it6805);
}

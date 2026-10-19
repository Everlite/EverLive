// SPDX-License-Identifier: GPL-2.0-only
/*
 * Derived from gc555 driver by ostrich (GPL-2.0-only).
 * Modified for GC573 PCIe Artix-7 FPGA VIP clocking by Everlite & contributors (2026).
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "gc573.h"

#define GC555_FPGA_TLP_FIX		0x0004
#define GC555_FPGA_VIP_CTRL		0x1000
#define GC555_FPGA_VIP_FRAME_PERIOD	0x103c
#define GC555_FPGA_VIP_COLOR_CTRL	0x1040
#define GC555_FPGA_VIP_FRAME_RATE	0x1080
#define GC555_FPGA_VIP_PIXEL_MODE	0x1088
#define GC555_FPGA_VIP_RESET		0x50000
#define GC555_FPGA_BRIGHTNESS		0x1060
#define GC555_FPGA_CONTRAST		0x105c
#define GC555_FPGA_HUE			0x1064
#define GC555_FPGA_SATURATION		0x1068
#define GC555_FPGA_HSCALER_BASE		0x40000
#define GC555_FPGA_VSCALER_BASE		0x60000
#define GC555_FPGA_SCALER_COEFF_OFFSET	0x800
#define GC555_FPGA_HSCALER_PHASE_OFFSET	0x2000
#define GC555_FPGA_SCALER_PHASES		64U
#define GC555_FPGA_SCALER_TAPS		6U
#define GC555_FPGA_PIXELS_PER_CLOCK	4U
#define GC555_FPGA_RGB_MATRIX_COEFFS	13U
#define GC555_FPGA_RGB_TO_YUV_BT709_FULL	0U
#define GC555_FPGA_RGB_TO_YUV_BT709_LIMITED	1U
#define GC555_FPGA_RGB_TO_YUV_BT601_FULL	2U
#define GC555_FPGA_RGB_TO_YUV_BT601_LIMITED	3U
#define GC555_FPGA_RGB_TO_YUV_BT2020_FULL	4U
#define GC555_FPGA_RGB_TO_YUV_BT2020_LIMITED	5U
#define GC555_FPGA_RGB_TO_YUV_COEFF_COUNT	6U
#define GC555_FPGA_RGB_LIMITED_TO_FULL_COEFF	6U
#define GC555_FPGA_MAX_WIDTH		4096U
#define GC555_FPGA_MAX_HEIGHT		2160U
#define GC555_FPGA_MAX_FRAME_RATE	240U
#define GC555_FPGA_PHASE_SHIFT		6U
#define GC555_FPGA_CLOCK_HZ		148500000U

#define GC555_FPGA_COLOR_INPUT_MODE_MASK	(BIT(19) | BIT(5) | BIT(0))
#define GC555_FPGA_OUTPUT_FORMAT_MASK	GENMASK(15, 8)
#define GC555_FPGA_OUTPUT_FORMAT_NV12	(0x0fU << 8)
#define GC555_FPGA_OUTPUT_FORMAT_P010	(0x14U << 8)
#define GC555_FPGA_OUTPUT_FORMAT_BGR24	(0x02U << 8)
#define GC555_FPGA_OUTPUT_FORMAT_RGB32	(0x06U << 8)
#define GC555_FPGA_COLOR_ADJUST_ENABLE	BIT(2)

#define GC555_FPGA_BRIGHTNESS_DEFAULT	0x200U
#define GC555_FPGA_BRIGHTNESS_MAX	0x3ffU
#define GC555_FPGA_CONTRAST_DEFAULT	0x100U
#define GC555_FPGA_CONTRAST_MAX		0x1ffU
#define GC555_FPGA_HUE_DEFAULT		0U
#define GC555_FPGA_HUE_MAX		360U
#define GC555_FPGA_SATURATION_DEFAULT	0x80U
#define GC555_FPGA_SATURATION_MAX	0x1ffU

struct gc555_fpga_video_config {
	u32 input_width;
	u32 input_height;
	u32 output_width;
	u32 output_height;
	u32 input_frame_rate_hz;
	u32 input_frame_rate_x100;
	u32 output_frame_rate_hz;
	enum gc555_video_input_class input_class;
	enum gc555_video_encoding input_encoding;
	enum gc555_video_sampling input_sampling;
	enum gc555_video_colorimetry input_colorimetry;
	enum gc555_video_hdr_mode input_hdr_mode;
	enum gc555_video_format output_format;
	bool interlaced;
	bool dual_pixel;
	bool ddr;
};

struct gc555_fpga {
	struct gc555_dev *gc555;
	/* Serializes VIP programming and clip-output state. */
	struct mutex lock;
	u32 brightness;
	u32 contrast;
	u32 hue;
	u32 saturation;
	bool configured;
};

static const u16
gc555_fpga_rgb_to_yuv_coeff[GC555_FPGA_RGB_TO_YUV_COEFF_COUNT]
				  [GC555_FPGA_RGB_MATRIX_COEFFS] = {
	[GC555_FPGA_RGB_TO_YUV_BT709_FULL] = {
		0x0350, 0x274f, 0x03f8, 0x0bb0, 0x0100, 0x15ac, 0x1c1c,
		0x0670, 0x0800, 0x1988, 0x0294, 0x1c1c, 0x0800,
	},
	[GC555_FPGA_RGB_TO_YUV_BT709_LIMITED] = {
		0x0350, 0x2dc6, 0x049f, 0x0d9b, 0x0000, 0x193b, 0x20bb,
		0x0780, 0x0800, 0x1dba, 0x0300, 0x20bb, 0x0800,
	},
	[GC555_FPGA_RGB_TO_YUV_BT601_FULL] = {
		0x0350, 0x2043, 0x0644, 0x106f, 0x0100, 0x12a0, 0x1c1c,
		0x097c, 0x0800, 0x1788, 0x0493, 0x1c1c, 0x0800,
	},
	[GC555_FPGA_RGB_TO_YUV_BT601_LIMITED] = {
		0x0350, 0x2591, 0x074c, 0x1323, 0x0000, 0x15af, 0x20bb,
		0x0b0c, 0x0800, 0x1b68, 0x0553, 0x20bb, 0x0800,
	},
	[GC555_FPGA_RGB_TO_YUV_BT2020_FULL] = {
		0x0350, 0x2544, 0x0342, 0x0e70, 0x0100, 0x1443, 0x1c1c,
		0x07d9, 0x0800, 0x19d9, 0x0242, 0x1c1c, 0x0800,
	},
	[GC555_FPGA_RGB_TO_YUV_BT2020_LIMITED] = {
		0x0350, 0x2b64, 0x03cc, 0x10d0, 0x0000, 0x1797, 0x20bb,
		0x0924, 0x0800, 0x1e19, 0x02a1, 0x20bb, 0x0800,
	},
};

static const u16
gc555_fpga_rgb_limited_to_full_coeff[GC555_FPGA_RGB_MATRIX_COEFFS] = {
	0x0888, 0x4a85, 0x0000, 0x0000, 0x012a, 0x0000, 0x4a85,
	0x0000, 0x012a, 0x0000, 0x0000, 0x4a85, 0x012a,
};

static const s16 gc555_fpga_six_tap_coeff[GC555_FPGA_SCALER_PHASES]
					 [GC555_FPGA_SCALER_TAPS] = {
	{ -132,  236, 3824,  236, -132,  64 },
	{ -116,  184, 3816,  292, -144,  64 },
	{ -100,  132, 3812,  348, -160,  64 },
	{  -88,   84, 3808,  404, -176,  64 },
	{  -72,   36, 3796,  464, -192,  64 },
	{  -60,   -8, 3780,  524, -208,  68 },
	{  -48,  -52, 3768,  588, -228,  68 },
	{  -32,  -96, 3748,  652, -244,  68 },
	{  -20, -136, 3724,  716, -260,  72 },
	{   -8, -172, 3696,  784, -276,  72 },
	{    0, -208, 3676,  848, -292,  72 },
	{   12, -244, 3640,  920, -308,  76 },
	{   20, -276, 3612,  988, -324,  76 },
	{   32, -304, 3568, 1060, -340,  80 },
	{   40, -332, 3532, 1132, -356,  80 },
	{   48, -360, 3492, 1204, -372,  84 },
	{   56, -384, 3448, 1276, -388,  88 },
	{   64, -408, 3404, 1352, -404,  88 },
	{   72, -428, 3348, 1428, -416,  92 },
	{   76, -448, 3308, 1500, -432,  92 },
	{   84, -464, 3248, 1576, -444,  96 },
	{   88, -480, 3200, 1652, -460,  96 },
	{   92, -492, 3140, 1728, -472, 100 },
	{   96, -504, 3080, 1804, -484, 104 },
	{  100, -516, 3020, 1880, -492, 104 },
	{  104, -524, 2956, 1960, -504, 104 },
	{  104, -532, 2892, 2036, -512, 108 },
	{  108, -540, 2832, 2108, -520, 108 },
	{  108, -544, 2764, 2184, -528, 112 },
	{  112, -544, 2688, 2260, -532, 112 },
	{  112, -548, 2624, 2336, -540, 112 },
	{  112, -548, 2556, 2408, -544, 112 },
	{  112, -544, 2480, 2480, -544, 112 },
	{  112, -544, 2408, 2556, -548, 112 },
	{  112, -540, 2336, 2624, -548, 112 },
	{  112, -532, 2260, 2688, -544, 112 },
	{  112, -528, 2184, 2764, -544, 108 },
	{  108, -520, 2108, 2832, -540, 108 },
	{  108, -512, 2036, 2892, -532, 104 },
	{  104, -504, 1960, 2956, -524, 104 },
	{  104, -492, 1880, 3020, -516, 100 },
	{  104, -484, 1804, 3080, -504,  96 },
	{  100, -472, 1728, 3140, -492,  92 },
	{   96, -460, 1652, 3200, -480,  88 },
	{   96, -444, 1576, 3248, -464,  84 },
	{   92, -432, 1500, 3308, -448,  76 },
	{   92, -416, 1428, 3348, -428,  72 },
	{   88, -404, 1352, 3404, -408,  64 },
	{   88, -388, 1276, 3448, -384,  56 },
	{   84, -372, 1204, 3492, -360,  48 },
	{   80, -356, 1132, 3532, -332,  40 },
	{   80, -340, 1060, 3568, -304,  32 },
	{   76, -324,  988, 3612, -276,  20 },
	{   76, -308,  920, 3640, -244,  12 },
	{   72, -292,  848, 3676, -208,   0 },
	{   72, -276,  784, 3696, -172,  -8 },
	{   72, -260,  716, 3724, -136, -20 },
	{   68, -244,  652, 3748,  -96, -32 },
	{   68, -228,  588, 3768,  -52, -48 },
	{   68, -208,  524, 3780,   -8, -60 },
	{   64, -192,  464, 3796,   36, -72 },
	{   64, -176,  404, 3808,   84, -88 },
	{   64, -160,  348, 3812,  132, -100 },
	{   64, -144,  292, 3816,  184, -116 },
};

static int gc555_fpga_read(struct gc555_fpga *fpga, u32 offset, u32 *value)
{
	int ret;

	ret = gc555_bridge_read(fpga->gc555, offset, value);
	if (!ret && *value == U32_MAX)
		return -ENODEV;

	return ret;
}

static int gc555_fpga_write(struct gc555_fpga *fpga, u32 offset, u32 value)
{
	return gc555_bridge_write(fpga->gc555, offset, value);
}

static int gc555_fpga_update_bits(struct gc555_fpga *fpga, u32 offset,
				  u32 mask, u32 value)
{
	u32 reg;
	int ret;

	ret = gc555_fpga_read(fpga, offset, &reg);
	if (ret)
		return ret;

	return gc555_fpga_write(fpga, offset,
				(reg & ~mask) | (value & mask));
}

static int gc555_fpga_program_color_adjustment(struct gc555_fpga *fpga)
{
	bool neutral;
	int ret;

	ret = gc555_fpga_write(fpga, GC555_FPGA_BRIGHTNESS,
			      fpga->brightness);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_CONTRAST,
				      fpga->contrast);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_HUE, fpga->hue);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_SATURATION,
				      fpga->saturation);
	neutral = fpga->brightness == GC555_FPGA_BRIGHTNESS_DEFAULT &&
		  fpga->contrast == GC555_FPGA_CONTRAST_DEFAULT &&
		  fpga->hue == GC555_FPGA_HUE_DEFAULT &&
		  fpga->saturation == GC555_FPGA_SATURATION_DEFAULT;
	if (!ret)
		ret = gc555_fpga_update_bits(
			fpga, GC555_FPGA_VIP_COLOR_CTRL,
			GC555_FPGA_COLOR_ADJUST_ENABLE,
			neutral ? 0 : GC555_FPGA_COLOR_ADJUST_ENABLE);

	return ret;
}

static bool
gc555_fpga_input_is_yuv(const struct gc555_fpga_video_config *config)
{
	return config->input_encoding == GC555_VIDEO_ENCODING_YUV;
}

static bool
gc555_fpga_output_is_rgb(const struct gc555_fpga_video_config *config)
{
	return config->output_format == GC555_VIDEO_FORMAT_BGR24 ||
	       config->output_format == GC555_VIDEO_FORMAT_RGB32;
}

static int
gc555_fpga_output_selector(const struct gc555_fpga_video_config *config,
			   u32 *selector)
{
	switch (config->output_format) {
	case GC555_VIDEO_FORMAT_YUYV:
		*selector = 0;
		return 0;
	case GC555_VIDEO_FORMAT_NV12:
		*selector = GC555_FPGA_OUTPUT_FORMAT_NV12;
		return 0;
	case GC555_VIDEO_FORMAT_P010:
		*selector = GC555_FPGA_OUTPUT_FORMAT_P010;
		return 0;
	case GC555_VIDEO_FORMAT_BGR24:
		*selector = GC555_FPGA_OUTPUT_FORMAT_BGR24;
		return 0;
	case GC555_VIDEO_FORMAT_RGB32:
		*selector = GC555_FPGA_OUTPUT_FORMAT_RGB32;
		return 0;
	}

	return -EINVAL;
}

static u32
gc555_fpga_tlp_fix_mode(const struct gc555_fpga_video_config *config)
{
	u32 span = config->output_width * config->output_height;

	if (config->output_format == GC555_VIDEO_FORMAT_BGR24) {
		span *= 3;
		if (span & 0x0e)
			return 2;
		if ((span & 0x3e) && !(span & 0x1e))
			return 1;
	} else if (config->output_format == GC555_VIDEO_FORMAT_RGB32) {
		if (span & 0x07)
			return 2;
		if ((span & 0x1f) && !(span & 0x0f))
			return 1;
	} else if (config->output_format == GC555_VIDEO_FORMAT_P010) {
		u32 half_span = (span & 0x7fffffffU) >> 1;
		u32 quarter_span = span >> 2;
		bool aligned_32 = !(half_span & 0x1f) &&
			!(quarter_span & 0x1f);
		bool aligned_16 = !(half_span & 0x0f) &&
			!(quarter_span & 0x0f);
		bool aligned_8 = !(half_span & 0x07) &&
			!(quarter_span & 0x07);

		if (aligned_32)
			return 0;
		if (aligned_16 && aligned_8)
			return 1;
		if (aligned_8)
			return 2;
	}

	return 0;
}

static u32
gc555_fpga_input_mode(const struct gc555_fpga_video_config *config)
{
	u32 mode = 0;

	if (config->input_sampling == GC555_VIDEO_SAMPLING_YUV422)
		mode |= BIT(0);
	else if (config->input_sampling == GC555_VIDEO_SAMPLING_YUV420)
		mode |= BIT(19);
	if (config->input_class == GC555_VIDEO_INPUT_UHD)
		mode |= BIT(5);

	return mode;
}

static int gc555_fpga_program_rgb_matrix(struct gc555_fpga *fpga,
					 const u16 *coeff)
{
	unsigned int index;
	int ret;

	ret = gc555_fpga_write(fpga, 0x10c0, coeff[0]);
	for (index = 1; !ret && index < GC555_FPGA_RGB_MATRIX_COEFFS;
	     index += 2)
		ret = gc555_fpga_write(
			fpga, 0x10c0 + (index + 1) * 2,
			coeff[index] | (coeff[index + 1] << 16));

	return ret;
}

static u32 gc555_fpga_rgb_to_yuv_selector(const struct gc555_fpga_video_config *config)
{
	bool limited = config->input_encoding ==
		GC555_VIDEO_ENCODING_RGB_LIMITED;

	switch (config->input_colorimetry) {
	case GC555_VIDEO_COLORIMETRY_BT601:
		return limited ? GC555_FPGA_RGB_TO_YUV_BT601_LIMITED :
			GC555_FPGA_RGB_TO_YUV_BT601_FULL;
	case GC555_VIDEO_COLORIMETRY_BT2020:
		return limited ? GC555_FPGA_RGB_TO_YUV_BT2020_LIMITED :
			GC555_FPGA_RGB_TO_YUV_BT2020_FULL;
	case GC555_VIDEO_COLORIMETRY_UNKNOWN:
	case GC555_VIDEO_COLORIMETRY_BT709:
	default:
		return limited ? GC555_FPGA_RGB_TO_YUV_BT709_LIMITED :
			GC555_FPGA_RGB_TO_YUV_BT709_FULL;
	}
}

static u32
gc555_fpga_pixel_mode(const struct gc555_fpga_video_config *config)
{
	return (config->ddr ? BIT(1) : 0) |
	       (config->dual_pixel ? BIT(0) : 0);
}

static int
gc555_fpga_program_color_path(struct gc555_fpga *fpga,
			     const struct gc555_fpga_video_config *config)
{
	const u16 *coefficients;
	u32 frame_rate_flags = 0;
	u32 selector;
	int ret;

	if (gc555_fpga_input_is_yuv(config))
		frame_rate_flags |= BIT(1);
	if (gc555_fpga_output_is_rgb(config))
		frame_rate_flags |= BIT(2);

	ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_COLOR_CTRL,
				     GC555_FPGA_COLOR_INPUT_MODE_MASK,
				     gc555_fpga_input_mode(config));
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_FRAME_RATE,
					     BIT(1) | BIT(2),
					     frame_rate_flags);
	if (ret)
		return ret;

	if (gc555_fpga_output_is_rgb(config)) {
		if (config->input_encoding ==
		    GC555_VIDEO_ENCODING_RGB_LIMITED) {
			ret = gc555_fpga_program_rgb_matrix(
				fpga, gc555_fpga_rgb_limited_to_full_coeff);
			if (!ret)
				ret = gc555_fpga_update_bits(
					fpga, GC555_FPGA_VIP_COLOR_CTRL,
					GENMASK(10, 8),
					GC555_FPGA_RGB_LIMITED_TO_FULL_COEFF
						<< 8);
			if (!ret)
				ret = gc555_fpga_update_bits(
					fpga, GC555_FPGA_VIP_COLOR_CTRL,
					BIT(1) | BIT(3), BIT(1));
			return ret;
		}

		return gc555_fpga_update_bits(fpga,
					      GC555_FPGA_VIP_COLOR_CTRL,
					      BIT(1) | BIT(3), 0);
	}
	if (gc555_fpga_input_is_yuv(config))
		return gc555_fpga_update_bits(fpga,
					      GC555_FPGA_VIP_COLOR_CTRL,
					      BIT(1) | BIT(3), 0);

	selector = gc555_fpga_rgb_to_yuv_selector(config);
	coefficients = gc555_fpga_rgb_to_yuv_coeff[selector];
	ret = gc555_fpga_program_rgb_matrix(fpga, coefficients);
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_COLOR_CTRL,
					     GENMASK(10, 8), selector << 8);
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_COLOR_CTRL,
					     BIT(1), BIT(1));
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_COLOR_CTRL,
					     BIT(3), 0);

	return ret;
}

static int gc555_fpga_program_coefficients(struct gc555_fpga *fpga,
					   u32 scaler_base)
{
	unsigned int phase;
	unsigned int pair;
	int ret = 0;

	for (phase = 0; !ret && phase < GC555_FPGA_SCALER_PHASES; phase++) {
		for (pair = 0; !ret && pair < GC555_FPGA_SCALER_TAPS / 2;
		     pair++) {
			u32 low = (u16)gc555_fpga_six_tap_coeff[phase]
				[pair * 2];
			u32 high = (u16)gc555_fpga_six_tap_coeff[phase]
				[pair * 2 + 1];
			u32 index = phase * (GC555_FPGA_SCALER_TAPS / 2) +
				pair;

			ret = gc555_fpga_write(
				fpga,
				scaler_base + GC555_FPGA_SCALER_COEFF_OFFSET +
					index * sizeof(u32), low | (high << 16));
		}
	}

	return ret;
}

static u64 gc555_fpga_build_phase_word(u32 *phase_accumulator,
				       u32 *source_lane, u32 *output_pixel,
				       u32 output_width, u32 phase_step)
{
	u64 packed = 0;
	unsigned int lane;

	for (lane = 0; lane < GC555_FPGA_PIXELS_PER_CLOCK; lane++) {
		u32 phase = (*phase_accumulator >>
			     (16 - GC555_FPGA_PHASE_SHIFT)) &
			    (GC555_FPGA_SCALER_PHASES - 1);
		u32 valid = 0;

		if (*phase_accumulator >> 16) {
			*phase_accumulator -= 1U << 16;
			(*source_lane)++;
		}
		if (!(*phase_accumulator >> 16) &&
		    *output_pixel < output_width) {
			*phase_accumulator += phase_step;
			(*output_pixel)++;
			valid = 1;
		}

		packed |= (u64)(phase | (*source_lane << 6) |
				(valid << 9)) << (lane * 10);
	}

	if (*source_lane >= GC555_FPGA_PIXELS_PER_CLOCK)
		*source_lane &= GC555_FPGA_PIXELS_PER_CLOCK - 1;

	return packed;
}

static int gc555_fpga_program_horizontal_phases(struct gc555_fpga *fpga,
						u32 input_width,
						u32 output_width)
{
	u32 phase_accumulator = 0;
	u32 source_lane = 0;
	u32 output_pixel = 0;
	u32 phase_step = (input_width << 16) / output_width;
	u32 active_entries = DIV_ROUND_UP(max(input_width, output_width),
					  GC555_FPGA_PIXELS_PER_CLOCK);
	u32 max_entries = GC555_FPGA_MAX_WIDTH /
			  GC555_FPGA_PIXELS_PER_CLOCK;
	u32 entry;
	int ret = 0;

	for (entry = 0; !ret && entry < max_entries; entry++) {
		u64 packed = 0;
		u32 offset = GC555_FPGA_HSCALER_BASE +
			GC555_FPGA_HSCALER_PHASE_OFFSET + entry * sizeof(u64);

		if (entry < active_entries)
			packed = gc555_fpga_build_phase_word(
				&phase_accumulator, &source_lane, &output_pixel,
				output_width, phase_step);

		ret = gc555_fpga_write(fpga, offset, lower_32_bits(packed));
		if (!ret)
			ret = gc555_fpga_write(
				fpga, offset + sizeof(u32), upper_32_bits(packed));
	}

	return ret;
}

static int
gc555_fpga_setup_scalers(struct gc555_fpga *fpga,
			 const struct gc555_fpga_video_config *config)
{
	u32 vertical_step = (config->input_height << 16) /
			    config->output_height;
	u32 horizontal_step = (config->input_width << 16) /
			      config->output_width;
	int ret;

	ret = gc555_fpga_program_coefficients(fpga,
					       GC555_FPGA_VSCALER_BASE);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_VSCALER_BASE + 0x10,
					config->input_height);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_VSCALER_BASE + 0x18,
					config->input_width);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_VSCALER_BASE + 0x20,
					config->output_height);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_VSCALER_BASE + 0x28,
					vertical_step);
	if (!ret)
		ret = gc555_fpga_program_coefficients(
			fpga, GC555_FPGA_HSCALER_BASE);
	if (!ret)
		ret = gc555_fpga_program_horizontal_phases(
			fpga, config->input_width, config->output_width);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_HSCALER_BASE + 0x10,
					config->output_height);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_HSCALER_BASE + 0x18,
					config->input_width);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_HSCALER_BASE + 0x20,
					config->output_width);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_HSCALER_BASE + 0x28,
					0);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_HSCALER_BASE + 0x30,
					horizontal_step);

	return ret;
}

static int gc555_fpga_start_scaler(struct gc555_fpga *fpga,
				   u32 scaler_base)
{
	u32 control;
	int ret;

	ret = gc555_fpga_write(fpga, scaler_base, 0x80);
	if (!ret)
		ret = gc555_fpga_read(fpga, scaler_base, &control);
	if (!ret)
		ret = gc555_fpga_write(fpga, scaler_base,
					(control & 0x80) | BIT(0));

	return ret;
}

static bool
gc555_fpga_is_native_4k_high_rate(
	const struct gc555_fpga_video_config *config)
{
	bool supported_rate = (config->input_frame_rate_hz >= 48 &&
			       config->input_frame_rate_hz <= 52) ||
			      (config->input_frame_rate_hz >= 57 &&
			       config->input_frame_rate_hz <= 63);

	return config->input_class == GC555_VIDEO_INPUT_UHD &&
	       config->input_width == 3840 && config->input_height == 2160 &&
	       config->output_width == 3840 && config->output_height == 2160 &&
	       supported_rate &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz;
}

static bool
gc555_fpga_is_native_4k_low_rate(
	const struct gc555_fpga_video_config *config)
{
	bool supported_rate = (config->input_frame_rate_hz >= 23 &&
			       config->input_frame_rate_hz <= 25) ||
			      (config->input_frame_rate_hz >= 29 &&
			       config->input_frame_rate_hz <= 31);

	return config->input_class == GC555_VIDEO_INPUT_UHD &&
	       config->input_width == 3840 && config->input_height == 2160 &&
	       config->output_width == 3840 && config->output_height == 2160 &&
	       supported_rate &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz;
}

static bool
gc555_fpga_is_native_2880x2160(
	const struct gc555_fpga_video_config *config)
{
	bool supported_rate = (config->input_frame_rate_hz >= 23 &&
			       config->input_frame_rate_hz <= 25) ||
			      (config->input_frame_rate_hz >= 29 &&
			       config->input_frame_rate_hz <= 31) ||
			      (config->input_frame_rate_hz >= 48 &&
			       config->input_frame_rate_hz <= 52) ||
			      (config->input_frame_rate_hz >= 57 &&
			       config->input_frame_rate_hz <= 63);

	return config->input_class == GC555_VIDEO_INPUT_UHD &&
	       config->input_width == 2880 && config->input_height == 2160 &&
	       config->output_width == 2880 && config->output_height == 2160 &&
	       supported_rate &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz;
}

static bool
gc555_fpga_is_native_1440_high_rate(
	const struct gc555_fpga_video_config *config)
{
	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 2560 && config->input_height == 1440 &&
	       config->output_width == 2560 && config->output_height == 1440 &&
	       config->input_frame_rate_hz >= 95 &&
	       config->input_frame_rate_hz <= 150 &&
	       config->output_frame_rate_hz >= 95 &&
	       config->output_frame_rate_hz <= 150;
}

static bool
gc555_fpga_is_native_1440p60(const struct gc555_fpga_video_config *config)
{
	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 2560 && config->input_height == 1440 &&
	       config->output_width == 2560 && config->output_height == 1440 &&
	       config->input_frame_rate_hz >= 48 &&
	       config->input_frame_rate_hz <= 72 &&
	       config->output_frame_rate_hz >= 48 &&
	       config->output_frame_rate_hz <= 72;
}

static bool
gc555_fpga_is_native_1440_high_rate_yuv444(
	const struct gc555_fpga_video_config *config)
{
	return gc555_fpga_is_native_1440_high_rate(config) &&
	       config->input_frame_rate_hz == 144 &&
	       config->output_frame_rate_hz == 144 &&
	       config->input_encoding == GC555_VIDEO_ENCODING_YUV &&
	       config->input_sampling == GC555_VIDEO_SAMPLING_YUV444 &&
	       config->input_colorimetry == GC555_VIDEO_COLORIMETRY_BT709;
}

static bool
gc555_fpga_is_native_3440x1440(
	const struct gc555_fpga_video_config *config)
{
	bool supported_rate = (config->input_frame_rate_hz >= 48 &&
			       config->input_frame_rate_hz <= 52) ||
			      (config->input_frame_rate_hz >= 57 &&
			       config->input_frame_rate_hz <= 63) ||
			      (config->input_frame_rate_hz >= 98 &&
			       config->input_frame_rate_hz <= 102);

	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 3440 && config->input_height == 1440 &&
	       config->output_width == 3440 && config->output_height == 1440 &&
	       supported_rate &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz;
}

static bool
gc555_fpga_is_native_3840x1080(
	const struct gc555_fpga_video_config *config)
{
	bool supported_rate = (config->input_frame_rate_hz >= 48 &&
			       config->input_frame_rate_hz <= 52) ||
			      (config->input_frame_rate_hz >= 57 &&
			       config->input_frame_rate_hz <= 63) ||
			      (config->input_frame_rate_hz >= 115 &&
			       config->input_frame_rate_hz <= 125);

	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 3840 && config->input_height == 1080 &&
	       config->output_width == 3840 && config->output_height == 1080 &&
	       supported_rate &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz;
}

static bool
gc555_fpga_is_native_1920x1200p60(
	const struct gc555_fpga_video_config *config)
{
	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 1920 && config->input_height == 1200 &&
	       config->output_width == 1920 && config->output_height == 1200 &&
	       config->input_frame_rate_hz >= 57 &&
	       config->input_frame_rate_hz <= 63 &&
	       config->output_frame_rate_hz >= 57 &&
	       config->output_frame_rate_hz <= 63;
}

static bool
gc555_fpga_is_native_1080_high_rate(
	const struct gc555_fpga_video_config *config)
{
	bool supported_rate = (config->input_frame_rate_hz >= 94 &&
			       config->input_frame_rate_hz <= 105) ||
			      (config->input_frame_rate_hz >= 115 &&
			       config->input_frame_rate_hz <= 125) ||
			      (config->input_frame_rate_hz >= 140 &&
			       config->input_frame_rate_hz <= 148) ||
			      (config->input_frame_rate_hz >= 175 &&
			       config->input_frame_rate_hz <= 185) ||
			      (config->input_frame_rate_hz >= 235 &&
			       config->input_frame_rate_hz <= 245);

	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 1920 && config->input_height == 1080 &&
	       config->output_width == 1920 && config->output_height == 1080 &&
	       supported_rate &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz;
}

static bool
gc555_fpga_is_native_2560x1080p60(
	const struct gc555_fpga_video_config *config)
{
	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 2560 && config->input_height == 1080 &&
	       config->output_width == 2560 && config->output_height == 1080 &&
	       config->input_frame_rate_hz >= 57 &&
	       config->input_frame_rate_hz <= 61 &&
	       config->output_frame_rate_hz >= 57 &&
	       config->output_frame_rate_hz <= 61;
}

static bool
gc555_fpga_is_native_2560x1080p60_rgb(
	const struct gc555_fpga_video_config *config)
{
	return gc555_fpga_is_native_2560x1080p60(config) &&
	       (config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED ||
		config->input_encoding == GC555_VIDEO_ENCODING_RGB_FULL);
}

static bool
gc555_fpga_is_native_2560x1080p144(
	const struct gc555_fpga_video_config *config)
{
	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 2560 && config->input_height == 1080 &&
	       config->output_width == 2560 && config->output_height == 1080 &&
	       config->input_frame_rate_hz == 144 &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz;
}

static bool
gc555_fpga_is_native_1080p_low_rate(
	const struct gc555_fpga_video_config *config)
{
	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 1920 && config->input_height == 1080 &&
	       config->output_width == 1920 && config->output_height == 1080 &&
	       !config->interlaced &&
	       config->input_frame_rate_hz >= 23 &&
	       config->input_frame_rate_hz <= 72 &&
	       config->output_frame_rate_hz >= 23 &&
	       config->output_frame_rate_hz <= 72;
}

static bool
gc555_fpga_is_native_1080p_low_rate_rgb_limited(
	const struct gc555_fpga_video_config *config)
{
	return gc555_fpga_is_native_1080p_low_rate(config) &&
	       config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED;
}

static bool
gc555_fpga_is_native_1440_rgb_limited(
	const struct gc555_fpga_video_config *config)
{
	return (gc555_fpga_is_native_1440_high_rate(config) ||
		gc555_fpga_is_native_1440p60(config)) &&
	       config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED;
}

static bool gc555_fpga_is_native_hdr_p010(
				const struct gc555_fpga_video_config *config)
{
	bool native_mode = gc555_fpga_is_native_4k_high_rate(config) ||
		gc555_fpga_is_native_4k_low_rate(config) ||
		gc555_fpga_is_native_2880x2160(config) ||
		gc555_fpga_is_native_1440_high_rate(config) ||
		gc555_fpga_is_native_1440p60(config) ||
		gc555_fpga_is_native_3440x1440(config) ||
		gc555_fpga_is_native_3840x1080(config) ||
		gc555_fpga_is_native_1920x1200p60(config) ||
		gc555_fpga_is_native_1080_high_rate(config) ||
		gc555_fpga_is_native_2560x1080p60(config) ||
		gc555_fpga_is_native_2560x1080p144(config) ||
		gc555_fpga_is_native_1080p_low_rate(config);
	bool supported_transport =
		((config->input_encoding == GC555_VIDEO_ENCODING_RGB_FULL ||
		  config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED) &&
		 config->input_sampling == GC555_VIDEO_SAMPLING_RGB) ||
		(config->input_encoding == GC555_VIDEO_ENCODING_YUV &&
		 config->input_sampling == GC555_VIDEO_SAMPLING_YUV422);

	return native_mode && supported_transport && !config->interlaced &&
	       (config->input_colorimetry == GC555_VIDEO_COLORIMETRY_BT2020 ||
	        config->input_colorimetry == GC555_VIDEO_COLORIMETRY_BT709) &&
	       (config->input_hdr_mode == GC555_VIDEO_HDR_PQ_BT2020 ||
	        config->input_hdr_mode == GC555_VIDEO_HDR_PQ) &&
	       config->output_format == GC555_VIDEO_FORMAT_P010;
}

/*
 * GC573: consoles and video processors (PS5, Xbox, RetroTINK 4K) frequently
 * signal BT.2020 or PQ HDR10 even when the capture side requests an 8-bit
 * format (such as YUYV or NV12). The FPGA carries BT.2020 RGB->YUV matrices,
 * so pass such signals through cleanly instead of refusing to stream.
 * PQ content is delivered untone-mapped to userspace.
 */
static bool gc555_fpga_is_native_hdr_passthrough(
				const struct gc555_fpga_video_config *config)
{
	bool native_mode = gc555_fpga_is_native_4k_high_rate(config) ||
		gc555_fpga_is_native_4k_low_rate(config) ||
		gc555_fpga_is_native_2880x2160(config) ||
		gc555_fpga_is_native_1440_high_rate(config) ||
		gc555_fpga_is_native_1440p60(config) ||
		gc555_fpga_is_native_3440x1440(config) ||
		gc555_fpga_is_native_3840x1080(config) ||
		gc555_fpga_is_native_1920x1200p60(config) ||
		gc555_fpga_is_native_1080_high_rate(config) ||
		gc555_fpga_is_native_2560x1080p60(config) ||
		gc555_fpga_is_native_2560x1080p144(config) ||
		gc555_fpga_is_native_1080p_low_rate(config);
	bool supported_transport =
		((config->input_encoding == GC555_VIDEO_ENCODING_RGB_FULL ||
		  config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED) &&
		 config->input_sampling == GC555_VIDEO_SAMPLING_RGB) ||
		(config->input_encoding == GC555_VIDEO_ENCODING_YUV &&
		 config->input_sampling == GC555_VIDEO_SAMPLING_YUV422);

	return native_mode && supported_transport && !config->interlaced &&
	       (config->input_colorimetry == GC555_VIDEO_COLORIMETRY_BT2020 ||
	        config->input_hdr_mode == GC555_VIDEO_HDR_PQ ||
	        config->input_hdr_mode == GC555_VIDEO_HDR_PQ_BT2020);
}

static bool
gc555_fpga_is_native_1080i_rgb_limited(
	const struct gc555_fpga_video_config *config)
{
	bool supported_rate = config->input_frame_rate_hz == 25 ||
		config->input_frame_rate_hz == 30;

	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 1920 && config->input_height == 1080 &&
	       config->output_width == 1920 && config->output_height == 1080 &&
	       config->interlaced && supported_rate &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz &&
	       config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED;
}

static bool
gc555_fpga_is_native_720p_rgb_limited(
	const struct gc555_fpga_video_config *config)
{
	return config->input_class == GC555_VIDEO_INPUT_HD &&
	       config->input_width == 1280 && config->input_height == 720 &&
	       config->output_width == 1280 && config->output_height == 720 &&
	       config->input_frame_rate_hz >= 49 &&
	       config->input_frame_rate_hz <= 61 &&
	       config->output_frame_rate_hz >= 49 &&
	       config->output_frame_rate_hz <= 61 &&
	       config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED;
}

static bool
gc555_fpga_is_native_640x480_rgb_full(
	const struct gc555_fpga_video_config *config)
{
	bool supported_rate = config->input_frame_rate_hz == 60 ||
		config->input_frame_rate_hz == 75;

	return config->input_class == GC555_VIDEO_INPUT_SD &&
	       config->input_width == 640 && config->input_height == 480 &&
	       config->output_width == 640 && config->output_height == 480 &&
	       supported_rate &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz &&
	       config->input_encoding == GC555_VIDEO_ENCODING_RGB_FULL;
}

static bool
gc555_fpga_is_native_640x480_rgb_limited(
	const struct gc555_fpga_video_config *config)
{
	bool supported_rate = config->input_frame_rate_hz == 60 ||
		config->input_frame_rate_hz == 75;

	return config->input_class == GC555_VIDEO_INPUT_SD &&
	       config->input_width == 640 && config->input_height == 480 &&
	       config->output_width == 640 && config->output_height == 480 &&
	       supported_rate &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz &&
	       config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED;
}

static bool
gc555_fpga_is_native_broadcast_sd(
	const struct gc555_fpga_video_config *config)
{
	bool native_480 = (config->input_width == 720 ||
			   config->input_width == 1440) &&
		config->input_height == 480 &&
		((!config->interlaced &&
		  ((config->input_frame_rate_hz >= 57 &&
		    config->input_frame_rate_hz <= 63) ||
		   (config->input_frame_rate_hz >= 48 &&
		    config->input_frame_rate_hz <= 52) ||
		   (config->input_frame_rate_hz >= 23 &&
		    config->input_frame_rate_hz <= 31))) ||
		 (config->interlaced &&
		  config->input_frame_rate_hz >= 28 &&
		  config->input_frame_rate_hz <= 31));
	bool native_576 = (config->input_width == 720 ||
			   config->input_width == 1440) &&
		config->input_height == 576 &&
		((!config->interlaced &&
		  ((config->input_frame_rate_hz >= 48 &&
		    config->input_frame_rate_hz <= 52) ||
		   (config->input_frame_rate_hz >= 57 &&
		    config->input_frame_rate_hz <= 63) ||
		   (config->input_frame_rate_hz >= 24 &&
		    config->input_frame_rate_hz <= 26))) ||
		 (config->interlaced &&
		  config->input_frame_rate_hz >= 24 &&
		  config->input_frame_rate_hz <= 26));
	bool native_240 = (config->input_width == 1440 ||
			   config->input_width == 1920 ||
			   config->input_width == 2880) &&
		config->input_height == 240 &&
		!config->interlaced &&
		config->input_frame_rate_hz >= 48 &&
		config->input_frame_rate_hz <= 63;
	bool native_400 = (config->input_width == 640 ||
			   config->input_width == 720) &&
		config->input_height == 400 &&
		!config->interlaced &&
		config->input_frame_rate_hz >= 68 &&
		config->input_frame_rate_hz <= 72;

	return config->input_class == GC555_VIDEO_INPUT_SD &&
	       (native_480 || native_576 || native_240 || native_400) &&
	       config->output_width == config->input_width &&
	       config->output_height == config->input_height &&
	       config->output_frame_rate_hz == config->input_frame_rate_hz &&
	       (config->input_encoding == GC555_VIDEO_ENCODING_RGB_FULL ||
	        config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED);
}

static bool
gc555_fpga_is_native_broadcast_sd_rgb_limited(
	const struct gc555_fpga_video_config *config)
{
	return gc555_fpga_is_native_broadcast_sd(config) &&
	       config->input_encoding == GC555_VIDEO_ENCODING_RGB_LIMITED;
}

static bool
gc555_fpga_is_native_cea_yuv(const struct gc555_fpga_video_config *config)
{
	bool native_4k = (config->input_width == 3840 ||
			  config->input_width == 2880) &&
		config->input_height == 2160 && !config->interlaced &&
		((config->input_frame_rate_hz >= 23 &&
		  config->input_frame_rate_hz <= 25) ||
		 (config->input_frame_rate_hz >= 29 &&
		  config->input_frame_rate_hz <= 31) ||
		 (config->input_frame_rate_hz >= 48 &&
		  config->input_frame_rate_hz <= 52) ||
		 (config->input_frame_rate_hz >= 57 &&
		  config->input_frame_rate_hz <= 63));
	bool native_1080p = config->input_width == 1920 &&
		config->input_height == 1080 && !config->interlaced &&
		((config->input_frame_rate_hz >= 23 &&
		  config->input_frame_rate_hz <= 25) ||
		 (config->input_frame_rate_hz >= 29 &&
		  config->input_frame_rate_hz <= 31) ||
		 (config->input_frame_rate_hz >= 48 &&
		  config->input_frame_rate_hz <= 52) ||
		 (config->input_frame_rate_hz >= 57 &&
		  config->input_frame_rate_hz <= 63) ||
		 (config->input_frame_rate_hz >= 68 &&
		  config->input_frame_rate_hz <= 72) ||
		 (config->input_frame_rate_hz >= 94 &&
		  config->input_frame_rate_hz <= 105) ||
		 (config->input_frame_rate_hz >= 115 &&
		  config->input_frame_rate_hz <= 125));
	bool native_1080i = config->input_width == 1920 &&
		config->input_height == 1080 && config->interlaced &&
		((config->input_frame_rate_hz >= 24 &&
		  config->input_frame_rate_hz <= 26) ||
		 (config->input_frame_rate_hz >= 29 &&
		  config->input_frame_rate_hz <= 31)) &&
		config->input_sampling != GC555_VIDEO_SAMPLING_YUV420;
	bool native_720p = config->input_width == 1280 &&
		config->input_height == 720 && !config->interlaced &&
		((config->input_frame_rate_hz >= 48 &&
		  config->input_frame_rate_hz <= 52) ||
		 (config->input_frame_rate_hz >= 57 &&
		  config->input_frame_rate_hz <= 63));
	bool native_480 = (config->input_width == 720 ||
			   config->input_width == 1440) &&
		config->input_height == 480 &&
		((!config->interlaced &&
		  ((config->input_frame_rate_hz >= 57 &&
		    config->input_frame_rate_hz <= 63) ||
		   (config->input_frame_rate_hz >= 48 &&
		    config->input_frame_rate_hz <= 52) ||
		   (config->input_frame_rate_hz >= 23 &&
		    config->input_frame_rate_hz <= 31))) ||
		 (config->interlaced &&
		  config->input_frame_rate_hz >= 28 &&
		  config->input_frame_rate_hz <= 31));
	bool native_576 = (config->input_width == 720 ||
			   config->input_width == 1440) &&
		config->input_height == 576 &&
		((!config->interlaced &&
		  ((config->input_frame_rate_hz >= 48 &&
		    config->input_frame_rate_hz <= 52) ||
		   (config->input_frame_rate_hz >= 57 &&
		    config->input_frame_rate_hz <= 63) ||
		   (config->input_frame_rate_hz >= 24 &&
		    config->input_frame_rate_hz <= 26))) ||
		 (config->interlaced &&
		  config->input_frame_rate_hz >= 24 &&
		  config->input_frame_rate_hz <= 26));
	bool native_240 = (config->input_width == 1440 ||
			   config->input_width == 1920 ||
			   config->input_width == 2880) &&
		config->input_height == 240 &&
		!config->interlaced &&
		config->input_frame_rate_hz >= 48 &&
		config->input_frame_rate_hz <= 63;
	bool native_400 = (config->input_width == 640 ||
			   config->input_width == 720) &&
		config->input_height == 400 && !config->interlaced &&
		config->input_frame_rate_hz >= 68 &&
		config->input_frame_rate_hz <= 72;
	bool native_sd = native_480 || native_576 || native_240 || native_400;
	bool native_size = config->output_width == config->input_width &&
		config->output_height == config->input_height &&
		config->output_frame_rate_hz == config->input_frame_rate_hz;
	bool sampling_supported =
		config->input_sampling == GC555_VIDEO_SAMPLING_YUV422 ||
		config->input_sampling == GC555_VIDEO_SAMPLING_YUV444 ||
		config->input_sampling == GC555_VIDEO_SAMPLING_YUV420;
	bool colorimetry_supported = native_sd ?
		(config->input_colorimetry == GC555_VIDEO_COLORIMETRY_BT601 ||
		 config->input_colorimetry == GC555_VIDEO_COLORIMETRY_BT709) :
		config->input_colorimetry == GC555_VIDEO_COLORIMETRY_BT709;
	bool input_class_supported;

	if (native_sd)
		input_class_supported =
			config->input_class == GC555_VIDEO_INPUT_SD;
	else if (native_4k)
		input_class_supported =
			config->input_class == GC555_VIDEO_INPUT_UHD;
	else
		input_class_supported =
			config->input_class == GC555_VIDEO_INPUT_HD;

	return config->input_encoding == GC555_VIDEO_ENCODING_YUV &&
	       sampling_supported && colorimetry_supported &&
	       input_class_supported && native_size &&
	       (native_4k || native_1080p || native_1080i || native_720p ||
		native_sd);
}

static bool gc555_fpga_output_format_valid(enum gc555_video_format format)
{
	return format <= GC555_VIDEO_FORMAT_RGB32;
}

static int
gc555_fpga_validate_config(const struct gc555_fpga_video_config *config)
{
	bool native_4k_high_rate;
	bool native_4k_low_rate;
	bool native_2880x2160;
	bool native_1440_high_rate;
	bool native_1440_high_rate_yuv444;
	bool native_1440_rgb_limited;
	bool native_1440p60;
	bool native_3440x1440;
	bool native_3840x1080;
	bool native_1920x1200p60;
	bool native_1080_high_rate;
	bool native_2560x1080p60;
	bool native_2560x1080p60_rgb;
	bool native_2560x1080p144;
	bool native_1080p_low_rate_rgb_limited;
	bool native_1080i_rgb_limited;
	bool native_720p_rgb_limited;
	bool native_640x480_rgb_full;
	bool native_640x480_rgb_limited;
	bool native_broadcast_sd;
	bool native_broadcast_sd_rgb_limited;
	bool native_hdr_p010;
	bool native_cea_yuv;
	bool native_sd;
	bool input_mode_supported;

	/* VIP programming depends on the complete input transport tuple. */
	if (!config->input_width || !config->input_height ||
	    !config->output_width || !config->output_height)
		return -EINVAL;
	if (config->input_width > GC555_FPGA_MAX_WIDTH ||
	    config->output_width > GC555_FPGA_MAX_WIDTH ||
	    config->input_height > GC555_FPGA_MAX_HEIGHT ||
	    config->output_height > GC555_FPGA_MAX_HEIGHT)
		return -ERANGE;

	native_4k_high_rate = gc555_fpga_is_native_4k_high_rate(config);
	native_4k_low_rate = gc555_fpga_is_native_4k_low_rate(config);
	native_2880x2160 = gc555_fpga_is_native_2880x2160(config);
	native_1440_high_rate = gc555_fpga_is_native_1440_high_rate(config);
	native_1440_high_rate_yuv444 =
		gc555_fpga_is_native_1440_high_rate_yuv444(config);
	native_1440_rgb_limited =
		gc555_fpga_is_native_1440_rgb_limited(config);
	native_1440p60 = gc555_fpga_is_native_1440p60(config);
	native_3440x1440 = gc555_fpga_is_native_3440x1440(config);
	native_3840x1080 = gc555_fpga_is_native_3840x1080(config);
	native_1920x1200p60 = gc555_fpga_is_native_1920x1200p60(config);
	native_1080_high_rate = gc555_fpga_is_native_1080_high_rate(config);
	native_2560x1080p60 = gc555_fpga_is_native_2560x1080p60(config);
	native_2560x1080p60_rgb =
		gc555_fpga_is_native_2560x1080p60_rgb(config);
	native_2560x1080p144 = gc555_fpga_is_native_2560x1080p144(config);
	native_1080p_low_rate_rgb_limited =
		gc555_fpga_is_native_1080p_low_rate_rgb_limited(config);
	native_1080i_rgb_limited =
		gc555_fpga_is_native_1080i_rgb_limited(config);
	native_720p_rgb_limited =
		gc555_fpga_is_native_720p_rgb_limited(config);
	native_640x480_rgb_full =
		gc555_fpga_is_native_640x480_rgb_full(config);
	native_640x480_rgb_limited =
		gc555_fpga_is_native_640x480_rgb_limited(config);
	native_broadcast_sd =
		gc555_fpga_is_native_broadcast_sd(config);
	native_broadcast_sd_rgb_limited =
		gc555_fpga_is_native_broadcast_sd_rgb_limited(config);
	native_hdr_p010 = gc555_fpga_is_native_hdr_p010(config) ||
		gc555_fpga_is_native_hdr_passthrough(config);
	native_cea_yuv = gc555_fpga_is_native_cea_yuv(config);
	native_sd = native_640x480_rgb_full ||
		native_640x480_rgb_limited ||
		native_broadcast_sd;

	if (native_cea_yuv &&
	    config->input_sampling == GC555_VIDEO_SAMPLING_YUV420)
		input_mode_supported = config->dual_pixel && !config->ddr;
	else if (config->input_height == 240 &&
		 (config->input_width == 1440 || config->input_width == 1920 ||
		  config->input_width == 2880))
		input_mode_supported = (config->dual_pixel && config->ddr) ||
				       (!config->dual_pixel && !config->ddr);
	else if (native_4k_high_rate || native_4k_low_rate ||
		 native_2880x2160 ||
		 native_1440_high_rate || native_1440p60 ||
		 native_3440x1440 || native_3840x1080 ||
		 native_1920x1200p60 || native_1080_high_rate ||
		 native_2560x1080p60 || native_2560x1080p144)
		input_mode_supported = config->dual_pixel && config->ddr;
	else
		input_mode_supported = !config->dual_pixel && !config->ddr;

	if (!gc555_fpga_output_format_valid(config->output_format) ||
	    (gc555_fpga_output_is_rgb(config) &&
	     ((config->input_encoding != GC555_VIDEO_ENCODING_RGB_FULL &&
	       config->input_encoding != GC555_VIDEO_ENCODING_RGB_LIMITED) ||
	      config->input_sampling != GC555_VIDEO_SAMPLING_RGB)) ||
	    (config->interlaced && !native_1080i_rgb_limited &&
	     !native_broadcast_sd && !native_cea_yuv) ||
	    (config->input_class != GC555_VIDEO_INPUT_HD && !native_sd &&
	     !(native_4k_high_rate || native_4k_low_rate || native_2880x2160) &&
	     !native_cea_yuv) ||
	    (config->input_hdr_mode != GC555_VIDEO_HDR_SDR &&
	     config->input_hdr_mode != GC555_VIDEO_HDR_HLG &&
	     !native_hdr_p010) ||
	    !input_mode_supported ||
	    (!native_cea_yuv &&
	     config->input_encoding != GC555_VIDEO_ENCODING_RGB_FULL &&
	     !native_1440_high_rate_yuv444 && !native_1440_rgb_limited &&
	     !native_2560x1080p60_rgb &&
	     !native_1080p_low_rate_rgb_limited &&
	     !native_1080_high_rate &&
	     !native_3840x1080 &&
	     !native_1920x1200p60 &&
	     !native_3440x1440 &&
	     !native_2560x1080p144 &&
	     !native_4k_high_rate &&
	     !native_4k_low_rate &&
	     !native_2880x2160 &&
	     !native_1080i_rgb_limited && !native_720p_rgb_limited &&
	     !native_broadcast_sd_rgb_limited &&
	     !native_640x480_rgb_limited && !native_hdr_p010) ||
	    (!native_cea_yuv &&
	     config->input_sampling != GC555_VIDEO_SAMPLING_RGB &&
	     !native_1440_high_rate_yuv444 && !native_hdr_p010) ||
	    (!native_cea_yuv &&
	     config->input_colorimetry != GC555_VIDEO_COLORIMETRY_BT709 &&
	     !native_hdr_p010 &&
	     !(native_sd && config->input_colorimetry ==
			     GC555_VIDEO_COLORIMETRY_BT601))) {
		pr_info_ratelimited("gc573: reject %ux%u@%u class=%d enc=%d samp=%d colo=%d hdr=%d out=%d il=%d dual=%d ddr=%d\n",
				    config->input_width, config->input_height,
				    config->input_frame_rate_hz,
				    config->input_class, config->input_encoding,
				    config->input_sampling,
				    config->input_colorimetry,
				    config->input_hdr_mode,
				    config->output_format, config->interlaced,
				    config->dual_pixel, config->ddr);
		return -EOPNOTSUPP;
	}

	if (config->input_width != config->output_width ||
	    config->input_height != config->output_height)
		return -EOPNOTSUPP;
	if (config->output_width < 721 && config->output_height < 577 &&
	    !native_sd && !native_cea_yuv)
		return -EOPNOTSUPP;

	return 0;
}

static int
gc555_fpga_build_video_config(const struct gc555_video_signal *input,
			      enum gc555_video_format output_format,
			      u32 output_width, u32 output_height,
			      u32 output_frame_rate_hz,
			      struct gc555_fpga_video_config *config)
{
	if (!input || !config)
		return -EINVAL;

	*config = (struct gc555_fpga_video_config) {
		.input_width = input->width,
		.input_height = input->height,
		.output_width = output_width,
		.output_height = output_height,
		.input_frame_rate_hz = input->frame_rate_hz,
		.input_frame_rate_x100 = input->frame_rate_x100,
		.output_frame_rate_hz = output_frame_rate_hz,
		.input_class = input->input_class,
		.input_encoding = input->encoding,
		.input_sampling = input->sampling,
		.input_colorimetry = input->colorimetry,
		.input_hdr_mode = input->hdr_mode,
		.output_format = output_format,
		.interlaced = input->interlaced,
		.dual_pixel = input->dual_pixel,
		.ddr = input->ddr,
	};

	return gc555_fpga_validate_config(config);
}

int gc555_fpga_validate_video(const struct gc555_video_signal *input,
			      enum gc555_video_format output_format,
			       u32 output_width, u32 output_height,
			       u32 output_frame_rate_hz)
{
	struct gc555_fpga_video_config config;

	return gc555_fpga_build_video_config(input, output_format, output_width,
					      output_height,
					      output_frame_rate_hz, &config);
}

static int
gc555_fpga_configure_locked(struct gc555_fpga *fpga,
			   const struct gc555_fpga_video_config *config)
{
	u32 clip_height = config->input_height;
	u32 output_selector;
	u32 frame_rate;
	u32 unused;
	int ret;

	ret = gc555_fpga_output_selector(config, &output_selector);
	if (ret)
		return ret;
	if (config->interlaced) {
		u32 field_height = config->input_height / 2U;

		clip_height = (field_height << 16) | field_height;
	}

	/* DMA enables output only after its descriptor tables are armed. */
	ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_CTRL, BIT(0), 0);
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_TLP_FIX,
					     GENMASK(1, 0),
					     gc555_fpga_tlp_fix_mode(config));
	if (!ret)
		ret = gc555_fpga_read(fpga, 0x1010, &unused);
	if (!ret)
		ret = gc555_fpga_program_color_adjustment(fpga);
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_CTRL,
					     GC555_FPGA_OUTPUT_FORMAT_MASK,
					     output_selector);
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_CTRL,
					     BIT(2) | BIT(3), 0);
	if (!ret)
		ret = gc555_fpga_write(fpga, 0x1020, 0);
	if (!ret)
		ret = gc555_fpga_write(fpga, 0x1024, config->input_width);
	if (!ret)
		ret = gc555_fpga_write(fpga, 0x1028, 0);
	if (!ret)
		ret = gc555_fpga_write(fpga, 0x102c, clip_height);
	if (ret)
		return ret;
	msleep(50);

	ret = gc555_fpga_write(fpga, GC555_FPGA_VIP_RESET, 0);
	if (ret)
		return ret;
	usleep_range(5000, 6000);
	ret = gc555_fpga_write(fpga, GC555_FPGA_VIP_RESET, 3);
	if (ret)
		return ret;
	usleep_range(5000, 6000);

	ret = gc555_fpga_setup_scalers(fpga, config);
	if (!ret)
		ret = gc555_fpga_program_color_path(fpga, config);
	frame_rate = clamp(config->output_frame_rate_hz, 5U,
			   GC555_FPGA_MAX_FRAME_RATE);
	/* Bits 31:16 are the frame rate. A later write of 90 dropped 120 Hz. */
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_FRAME_RATE,
					     GENMASK(31, 16),
					     frame_rate << 16);
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_CTRL,
					     BIT(5), 0);
	if (!ret) {
		u32 frame_period;

		if (config->input_frame_rate_x100 >= 500 &&
		    config->input_frame_rate_x100 <= (GC555_FPGA_MAX_FRAME_RATE * 100))
			frame_period = div_u64((u64)GC555_FPGA_CLOCK_HZ * 100,
					       config->input_frame_rate_x100);
		else
			frame_period = GC555_FPGA_CLOCK_HZ / frame_rate;

		ret = gc555_fpga_write(fpga, GC555_FPGA_VIP_FRAME_PERIOD,
				       frame_period);
	}
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_CTRL,
					     BIT(7), 0);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_VIP_PIXEL_MODE,
					gc555_fpga_pixel_mode(config));
	if (!ret)
		ret = gc555_fpga_start_scaler(fpga,
					       GC555_FPGA_VSCALER_BASE);
	if (!ret)
		ret = gc555_fpga_start_scaler(fpga,
					       GC555_FPGA_HSCALER_BASE);
	if (!ret)
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_CTRL,
					     BIT(6), 0);

	return ret;
}

int gc555_fpga_configure(struct gc555_dev *gc555,
			 const struct gc555_video_signal *input,
			 enum gc555_video_format output_format,
			 u32 output_width, u32 output_height,
			 u32 output_frame_rate_hz)
{
	struct gc555_fpga_video_config config;
	struct gc555_fpga *fpga;
	int ret;

	if (!gc555 || !input)
		return -EINVAL;
	fpga = gc555->fpga;
	if (!fpga)
		return -ENODEV;

	ret = gc555_fpga_build_video_config(input, output_format, output_width,
					    output_height,
					     output_frame_rate_hz, &config);
	if (ret)
		return ret;

	mutex_lock(&fpga->lock);
	fpga->configured = false;
	ret = gc555_fpga_configure_locked(fpga, &config);
	if (ret)
		gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_CTRL, BIT(0), 0);
	else
		fpga->configured = true;
	mutex_unlock(&fpga->lock);

	return ret;
}

int gc555_fpga_update_color_path(struct gc555_dev *gc555,
				 const struct gc555_video_signal *input,
				 enum gc555_video_format output_format,
				 u32 output_width, u32 output_height,
				 u32 output_frame_rate_hz)
{
	struct gc555_fpga_video_config config;
	struct gc555_fpga *fpga;
	int ret;

	if (!gc555 || !input)
		return -EINVAL;
	fpga = gc555->fpga;
	if (!fpga)
		return -ENODEV;

	ret = gc555_fpga_build_video_config(input, output_format, output_width,
					    output_height, output_frame_rate_hz,
					    &config);
	if (ret && ret != -EOPNOTSUPP)
		return ret;

	mutex_lock(&fpga->lock);
	if (!fpga->configured)
		ret = 0;
	else
		ret = gc555_fpga_program_color_path(fpga, &config);
	mutex_unlock(&fpga->lock);

	return ret;
}

int gc555_fpga_reset_vip(struct gc555_dev *gc555)
{
	struct gc555_fpga *fpga;
	int ret;

	if (!gc555 || !gc555->fpga)
		return -ENODEV;

	fpga = gc555->fpga;
	mutex_lock(&fpga->lock);
	fpga->configured = false;
	ret = gc555_fpga_write(fpga, GC555_FPGA_VIP_RESET, 0);
	if (!ret)
		usleep_range(5000, 6000);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_VIP_RESET, 3);
	mutex_unlock(&fpga->lock);

	return ret;
}

int gc555_fpga_set_output_enabled(struct gc555_dev *gc555, bool enabled)
{
	struct gc555_fpga *fpga;
	int ret;

	if (!gc555)
		return -EINVAL;
	fpga = gc555->fpga;
	if (!fpga)
		return -ENODEV;

	mutex_lock(&fpga->lock);
	if (enabled && !fpga->configured)
		ret = -EPIPE;
	else
		ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_CTRL,
					     BIT(0), enabled ? BIT(0) : 0);
	mutex_unlock(&fpga->lock);

	return ret;
}

int gc555_fpga_set_frame_rate(struct gc555_dev *gc555, u32 frame_rate_hz,
			      u32 frame_rate_x100)
{
	struct gc555_fpga *fpga;
	u32 frame_rate;
	u32 frame_period;
	int ret;

	if (!gc555 || !gc555->fpga)
		return -ENODEV;
	fpga = gc555->fpga;
	frame_rate = clamp(frame_rate_hz, 5U, GC555_FPGA_MAX_FRAME_RATE);

	if (frame_rate_x100 >= 500 &&
	    frame_rate_x100 <= (GC555_FPGA_MAX_FRAME_RATE * 100))
		frame_period = div_u64((u64)GC555_FPGA_CLOCK_HZ * 100,
				       frame_rate_x100);
	else
		frame_period = GC555_FPGA_CLOCK_HZ / frame_rate;

	mutex_lock(&fpga->lock);
	ret = gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_FRAME_RATE,
				     GENMASK(31, 16), frame_rate << 16);
	if (!ret)
		ret = gc555_fpga_write(fpga, GC555_FPGA_VIP_FRAME_PERIOD,
				       frame_period);
	mutex_unlock(&fpga->lock);

	return ret;
}

int gc555_fpga_set_color_control(struct gc555_dev *gc555,
				 enum gc555_fpga_color_control control,
				 u32 value)
{
	struct gc555_fpga *fpga;
	u32 *cached_value;
	u32 maximum;
	u32 previous;
	int ret = 0;

	if (!gc555)
		return -EINVAL;
	fpga = gc555->fpga;
	if (!fpga)
		return -ENODEV;

	mutex_lock(&fpga->lock);
	switch (control) {
	case GC555_FPGA_COLOR_BRIGHTNESS:
		cached_value = &fpga->brightness;
		maximum = GC555_FPGA_BRIGHTNESS_MAX;
		break;
	case GC555_FPGA_COLOR_CONTRAST:
		cached_value = &fpga->contrast;
		maximum = GC555_FPGA_CONTRAST_MAX;
		break;
	case GC555_FPGA_COLOR_HUE:
		cached_value = &fpga->hue;
		maximum = GC555_FPGA_HUE_MAX;
		break;
	case GC555_FPGA_COLOR_SATURATION:
		cached_value = &fpga->saturation;
		maximum = GC555_FPGA_SATURATION_MAX;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret && value > maximum) {
		ret = -ERANGE;
	} else if (!ret) {
		previous = *cached_value;
		*cached_value = value;
		if (fpga->configured) {
			ret = gc555_fpga_program_color_adjustment(fpga);
			if (ret)
				*cached_value = previous;
		}
	}
	mutex_unlock(&fpga->lock);

	return ret;
}

int gc555_fpga_init(struct gc555_dev *gc555)
{
	struct gc555_fpga *fpga;

	if (!gc555_bridge_is_ready(gc555))
		return -ENODEV;

	fpga = devm_kzalloc(gc555->dev, sizeof(*fpga), GFP_KERNEL);
	if (!fpga)
		return -ENOMEM;

	fpga->gc555 = gc555;
	mutex_init(&fpga->lock);
	fpga->brightness = GC555_FPGA_BRIGHTNESS_DEFAULT;
	fpga->contrast = GC555_FPGA_CONTRAST_DEFAULT;
	fpga->hue = GC555_FPGA_HUE_DEFAULT;
	fpga->saturation = GC555_FPGA_SATURATION_DEFAULT;
	gc555->fpga = fpga;

	return 0;
}

void gc555_fpga_cleanup(struct gc555_dev *gc555)
{
	struct gc555_fpga *fpga;

	if (!gc555 || !gc555->fpga)
		return;

	fpga = gc555->fpga;
	mutex_lock(&fpga->lock);
	gc555_fpga_update_bits(fpga, GC555_FPGA_VIP_CTRL, BIT(0), 0);
	fpga->configured = false;
	mutex_unlock(&fpga->lock);
	gc555->fpga = NULL;
}

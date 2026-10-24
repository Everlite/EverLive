/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_fpga.c — Userspace GC573 FPGA VIP / scaler configuration
 *
 * Derived from open_gc573/gc573-fpga.c (GPL-2.0-only).
 */

#include "gc573_user_fpga.h"

#define GC573_FPGA_HSCALER_BASE		0x40000
#define GC573_FPGA_VSCALER_BASE		0x60000
#define GC573_FPGA_SCALER_COEFF_OFFSET	0x800
#define GC573_FPGA_HSCALER_PHASE_OFFSET	0x2000
#define GC573_FPGA_SCALER_PHASES	64U
#define GC573_FPGA_SCALER_TAPS		6U
#define GC573_FPGA_PIXELS_PER_CLOCK	4U
#define GC573_FPGA_RGB_MATRIX_COEFFS	13U
#define GC573_FPGA_PHASE_SHIFT		6U
#define GC573_FPGA_CLOCK_HZ		148500000U

#define GC573_FPGA_COLOR_INPUT_MODE_MASK	(BIT(19) | BIT(5) | BIT(0))
#define GC573_FPGA_OUTPUT_FORMAT_MASK		GENMASK(15, 8)
#define GC573_FPGA_COLOR_ADJUST_ENABLE		BIT(2)

#define GC573_FPGA_BRIGHTNESS_DEFAULT	0x200U
#define GC573_FPGA_CONTRAST_DEFAULT	0x100U
#define GC573_FPGA_HUE_DEFAULT		0U
#define GC573_FPGA_SATURATION_DEFAULT	0x80U

#define GC573_FPGA_BRIGHTNESS		0x01060
#define GC573_FPGA_CONTRAST		0x0105c
#define GC573_FPGA_HUE			0x01064
#define GC573_FPGA_SATURATION		0x01068

/* 2 % safety margin, matching the kernel driver default. */
#define GC573_FPGA_FRAME_PERIOD_MARGIN	20U

enum gc573_rgb_to_yuv_selector {
	GC573_RGB_TO_YUV_BT709_FULL = 0,
	GC573_RGB_TO_YUV_BT709_LIMITED,
	GC573_RGB_TO_YUV_BT601_FULL,
	GC573_RGB_TO_YUV_BT601_LIMITED,
	GC573_RGB_TO_YUV_BT2020_FULL,
	GC573_RGB_TO_YUV_BT2020_LIMITED,
};

static const u16 gc573_rgb_to_yuv_coeff[6][GC573_FPGA_RGB_MATRIX_COEFFS] = {
	[GC573_RGB_TO_YUV_BT709_FULL] = {
		0x0350, 0x274f, 0x03f8, 0x0bb0, 0x0100, 0x15ac, 0x1c1c,
		0x0670, 0x0800, 0x1988, 0x0294, 0x1c1c, 0x0800,
	},
	[GC573_RGB_TO_YUV_BT709_LIMITED] = {
		0x0350, 0x2dc6, 0x049f, 0x0d9b, 0x0000, 0x193b, 0x20bb,
		0x0780, 0x0800, 0x1dba, 0x0300, 0x20bb, 0x0800,
	},
	[GC573_RGB_TO_YUV_BT601_FULL] = {
		0x0350, 0x2043, 0x0644, 0x106f, 0x0100, 0x12a0, 0x1c1c,
		0x097c, 0x0800, 0x1788, 0x0493, 0x1c1c, 0x0800,
	},
	[GC573_RGB_TO_YUV_BT601_LIMITED] = {
		0x0350, 0x2591, 0x074c, 0x1323, 0x0000, 0x15af, 0x20bb,
		0x0b0c, 0x0800, 0x1b68, 0x0553, 0x20bb, 0x0800,
	},
	[GC573_RGB_TO_YUV_BT2020_FULL] = {
		0x0350, 0x2544, 0x0342, 0x0e70, 0x0100, 0x1443, 0x1c1c,
		0x07d9, 0x0800, 0x19d9, 0x0242, 0x1c1c, 0x0800,
	},
	[GC573_RGB_TO_YUV_BT2020_LIMITED] = {
		0x0350, 0x2b64, 0x03cc, 0x10d0, 0x0000, 0x1797, 0x20bb,
		0x0924, 0x0800, 0x1e19, 0x02a1, 0x20bb, 0x0800,
	},
};



static const s16 gc573_six_tap_coeff[GC573_FPGA_SCALER_PHASES]
				     [GC573_FPGA_SCALER_TAPS] = {
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

static inline u32 gc573_fpga_read(void *bar0, u32 offset)
{
	return readl((u8 *)bar0 + offset);
}

static inline void gc573_fpga_write(void *bar0, u32 offset, u32 value)
{
	writel(value, (u8 *)bar0 + offset);
}

static void gc573_fpga_update_bits(void *bar0, u32 offset, u32 mask, u32 value)
{
	u32 reg = gc573_fpga_read(bar0, offset);

	gc573_fpga_write(bar0, offset, (reg & ~mask) | (value & mask));
}

/*
 * Dimensions verified by the kernel mode table.  The rate check below is
 * intentionally a little wider than an exact match so PAL 50/100 Hz,
 * 1080p 144/180/240 Hz and film BFI (95.94 Hz) all pass.
 */
static const struct {
	u16 width;
	u16 height;
} gc573_verified_modes[] = {
	{ 640, 400 }, { 640, 480 }, { 720, 400 }, { 720, 480 },
	{ 720, 576 }, { 800, 600 }, { 1024, 768 }, { 1280, 800 },
	{ 1280, 960 }, { 1280, 720 }, { 1280, 1024 }, { 1360, 765 },
	{ 1440, 240 }, { 1440, 480 }, { 1440, 576 }, { 1440, 900 },
	{ 1680, 1050 }, { 1920, 240 }, { 1920, 1080 }, { 1920, 1200 },
	{ 2560, 1080 }, { 2560, 1440 }, { 2880, 240 }, { 2880, 2160 },
	{ 3440, 1440 }, { 3840, 1080 }, { 3840, 2160 },
};

static const u16 gc573_nominal_rates_hz[] = {
	24, 25, 30, 50, 60, 70, 75, 95, 96, 100, 120, 144, 180, 240,
};

static bool gc573_rate_known(u32 frame_rate_hz, u32 frame_rate_x100)
{
	for (size_t i = 0; i < ARRAY_SIZE(gc573_nominal_rates_hz); i++) {
		u32 nominal = gc573_nominal_rates_hz[i];
		u32 nominal_x100 = nominal * 100U;
		u32 tolerance = max(200U, nominal * 6U);
		u32 measured = frame_rate_x100 ?
			frame_rate_x100 : frame_rate_hz * 100U;

		if (!measured)
			return false;
		if ((measured > nominal_x100 ? measured - nominal_x100 :
					       nominal_x100 - measured) <= tolerance)
			return true;
	}

	return false;
}

bool gc573_user_fpga_mode_known(u32 width, u32 height, u32 frame_rate_hz,
				u32 frame_rate_x100)
{
	bool dimensions_known = false;

	for (size_t i = 0; i < ARRAY_SIZE(gc573_verified_modes); i++) {
		if (gc573_verified_modes[i].width == width &&
		    gc573_verified_modes[i].height == height) {
			dimensions_known = true;
			break;
		}
	}

	return dimensions_known &&
	       gc573_rate_known(frame_rate_hz, frame_rate_x100);
}

/*
 * Mirrors the kernel's high-bandwidth mode families: wide scans and
 * 1920-wide modes above 94 Hz (or 1920x1200p60) use dual-pixel DDR output.
 * 240p super-resolution modes are accepted in either transport.
 */
static bool
gc573_user_fpga_high_bandwidth(const struct gc573_user_fpga_config *cfg)
{
	if (cfg->interlaced || cfg->input_height == 240)
		return false;
	if (cfg->input_width > 1920)
		return true;
	if (cfg->input_width == 1920 && cfg->input_height == 1200 &&
	    cfg->input_frame_rate_hz >= 57)
		return true;
	return cfg->input_width == 1920 && cfg->input_height == 1080 &&
	       cfg->input_frame_rate_hz >= 94;
}

int gc573_user_fpga_validate_config(const struct gc573_user_fpga_config *cfg)
{
	bool high_bandwidth;

	if (!cfg)
		return -EINVAL;
	if (!cfg->input_width || !cfg->input_height ||
	    !cfg->output_width || !cfg->output_height)
		return -EINVAL;
	if (cfg->input_width > GC573_FPGA_MAX_WIDTH ||
	    cfg->output_width > GC573_FPGA_MAX_WIDTH ||
	    cfg->input_height > GC573_FPGA_MAX_HEIGHT ||
	    cfg->output_height > GC573_FPGA_MAX_HEIGHT)
		return -ERANGE;
	if (cfg->output_width & 1U)
		return -EINVAL;
	if (cfg->input_width != cfg->output_width ||
	    cfg->input_height != cfg->output_height) {
		pr_err("FPGA geometry mismatch: input %ux%u, output %ux%u; userspace downscaling is disabled",
		       cfg->input_width, cfg->input_height,
		       cfg->output_width, cfg->output_height);
		return -EOPNOTSUPP;
	}
	if (cfg->interlaced && cfg->input_height < 2)
		return -EINVAL;
	if (!cfg->output_frame_rate_hz ||
	    cfg->output_frame_rate_hz > GC573_FPGA_MAX_FRAME_RATE * 2U)
		return -ERANGE;

	high_bandwidth = gc573_user_fpga_high_bandwidth(cfg);
	if (cfg->input_sampling == GC573_USER_SAMPLING_YUV420) {
		/* The kernel requires dual-pixel LVDS and no DDR for 4:2:0. */
		if (!(cfg->dual_pixel && !cfg->ddr))
			pr_warn("FPGA transport mismatch for %ux%u@%u: YCbCr 4:2:0 needs dual-pixel LVDS (dual=%u ddr=%u)",
				cfg->input_width, cfg->input_height,
				cfg->input_frame_rate_hz, cfg->dual_pixel,
				cfg->ddr);
	} else if (cfg->input_height == 240 &&
	    (cfg->input_width == 1440 || cfg->input_width == 1920 ||
	     cfg->input_width == 2880)) {
		/* CRT super-resolution accepts single- and dual-pixel output. */
	} else if (high_bandwidth && !(cfg->dual_pixel && cfg->ddr)) {
		pr_warn("FPGA transport mismatch for %ux%u@%u: expected dual-pixel DDR (dual=%u ddr=%u)",
			cfg->input_width, cfg->input_height,
			cfg->input_frame_rate_hz, cfg->dual_pixel, cfg->ddr);
	} else if (!high_bandwidth && (cfg->dual_pixel || cfg->ddr)) {
		pr_warn("FPGA transport mismatch for %ux%u@%u: expected single-pixel TTL (dual=%u ddr=%u)",
			cfg->input_width, cfg->input_height,
			cfg->input_frame_rate_hz, cfg->dual_pixel, cfg->ddr);
	}

	return 0;
}

u32 gc573_user_fpga_frame_period(u32 frame_rate_hz, u32 frame_rate_x100)
{
	u64 rate_x100;

	if (frame_rate_x100 >= 500 &&
	    frame_rate_x100 <= GC573_FPGA_MAX_FRAME_RATE * 100)
		rate_x100 = frame_rate_x100;
	else
		rate_x100 = (u64)frame_rate_hz * 100;

	if (!rate_x100)
		rate_x100 = 6000;

	return (u32)(((u64)GC573_FPGA_CLOCK_HZ * 100 * 1000) /
		     (rate_x100 * (1000 + GC573_FPGA_FRAME_PERIOD_MARGIN)));
}

static void gc573_fpga_program_color_adjustment(void *bar0)
{
	gc573_fpga_write(bar0, GC573_FPGA_BRIGHTNESS,
			 GC573_FPGA_BRIGHTNESS_DEFAULT);
	gc573_fpga_write(bar0, GC573_FPGA_CONTRAST,
			 GC573_FPGA_CONTRAST_DEFAULT);
	gc573_fpga_write(bar0, GC573_FPGA_HUE, GC573_FPGA_HUE_DEFAULT);
	gc573_fpga_write(bar0, GC573_FPGA_SATURATION,
			 GC573_FPGA_SATURATION_DEFAULT);
	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_COLOR_CTRL,
			       GC573_FPGA_COLOR_ADJUST_ENABLE, 0);
}

static u32 gc573_fpga_input_mode(const struct gc573_user_fpga_config *cfg)
{
	u32 mode = 0;

	if (cfg->input_sampling == GC573_USER_SAMPLING_YUV422)
		mode |= BIT(0);
	else if (cfg->input_sampling == GC573_USER_SAMPLING_YUV420)
		mode |= BIT(19);
	if (cfg->input_class == GC573_USER_INPUT_UHD)
		mode |= BIT(5);

	return mode;
}

static void gc573_fpga_program_rgb_matrix(void *bar0, const u16 *coeff)
{
	gc573_fpga_write(bar0, 0x10c0, coeff[0]);
	for (unsigned int index = 1; index < GC573_FPGA_RGB_MATRIX_COEFFS;
	     index += 2)
		gc573_fpga_write(bar0, 0x10c0 + (index + 1) * 2,
				 coeff[index] | (coeff[index + 1] << 16));
}

static enum gc573_rgb_to_yuv_selector
gc573_fpga_rgb_to_yuv_selector(const struct gc573_user_fpga_config *cfg)
{
	bool limited = cfg->input_encoding == GC573_USER_ENCODING_RGB_LIMITED;

	/*
	 * SD sources (<= 576 lines, including 240p super-resolution) use the
	 * BT.601 matrix even when the source leaves colorimetry unspecified.
	 */
	if (cfg->input_class == GC573_USER_INPUT_SD ||
	    cfg->input_colorimetry == GC573_USER_COLORIMETRY_BT601)
		return limited ? GC573_RGB_TO_YUV_BT601_LIMITED :
				 GC573_RGB_TO_YUV_BT601_FULL;

	switch (cfg->input_colorimetry) {
	case GC573_USER_COLORIMETRY_BT2020:
		return limited ? GC573_RGB_TO_YUV_BT2020_LIMITED :
				 GC573_RGB_TO_YUV_BT2020_FULL;
	case GC573_USER_COLORIMETRY_UNKNOWN:
	case GC573_USER_COLORIMETRY_BT709:
	default:
		return limited ? GC573_RGB_TO_YUV_BT709_LIMITED :
				 GC573_RGB_TO_YUV_BT709_FULL;
	}
}

static void
gc573_fpga_program_color_path(void *bar0,
			      const struct gc573_user_fpga_config *cfg)
{
	u32 frame_rate_flags = 0;

	if (cfg->input_encoding == GC573_USER_ENCODING_YUV)
		frame_rate_flags |= BIT(1);

	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_COLOR_CTRL,
			       GC573_FPGA_COLOR_INPUT_MODE_MASK,
			       gc573_fpga_input_mode(cfg));
	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_FRAME_RATE,
			       BIT(1) | BIT(2), frame_rate_flags);

	if (cfg->input_encoding == GC573_USER_ENCODING_YUV) {
		/*
		 * YCbCr 4:2:2/4:4:4/4:2:0 input is already in the DMA's YUV
		 * domain.  Bypass the RGB->YUV matrix and pass it through.
		 */
		gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_COLOR_CTRL,
				       BIT(1) | BIT(3), 0);
		return;
	}


	{
		enum gc573_rgb_to_yuv_selector selector =
			gc573_fpga_rgb_to_yuv_selector(cfg);

		gc573_fpga_program_rgb_matrix(bar0,
					      gc573_rgb_to_yuv_coeff[selector]);
		gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_COLOR_CTRL,
				       GENMASK(10, 8), (u32)selector << 8);
		gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_COLOR_CTRL,
				       BIT(1), BIT(1));
		gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_COLOR_CTRL,
				       BIT(3), 0);
	}
}

static void gc573_fpga_program_coefficients(void *bar0, u32 scaler_base)
{
	for (unsigned int phase = 0; phase < GC573_FPGA_SCALER_PHASES; phase++) {
		for (unsigned int pair = 0;
		     pair < GC573_FPGA_SCALER_TAPS / 2; pair++) {
			u32 low = (u16)gc573_six_tap_coeff[phase][pair * 2];
			u32 high = (u16)gc573_six_tap_coeff[phase]
							[pair * 2 + 1];
			u32 index = phase * (GC573_FPGA_SCALER_TAPS / 2) + pair;

			gc573_fpga_write(bar0,
					 scaler_base +
					 GC573_FPGA_SCALER_COEFF_OFFSET +
					 index * sizeof(u32),
					 low | (high << 16));
		}
	}
}

static u64 gc573_fpga_build_phase_word(u32 *phase_accumulator,
				       u32 *source_lane, u32 *output_pixel,
				       u32 output_width, u32 phase_step)
{
	u64 packed = 0;

	for (unsigned int lane = 0; lane < GC573_FPGA_PIXELS_PER_CLOCK;
	     lane++) {
		u32 phase = (*phase_accumulator >>
			     (16 - GC573_FPGA_PHASE_SHIFT)) &
			    (GC573_FPGA_SCALER_PHASES - 1);
		u32 valid = 0;

		if (*phase_accumulator >> 16) {
			*phase_accumulator -= 1U << 16;
			(*source_lane)++;
		}
		if (!(*phase_accumulator >> 16) && *output_pixel < output_width) {
			*phase_accumulator += phase_step;
			(*output_pixel)++;
			valid = 1;
		}

		packed |= (u64)(phase | (*source_lane << 6) | (valid << 9)) <<
			  (lane * 10);
	}

	if (*source_lane >= GC573_FPGA_PIXELS_PER_CLOCK)
		*source_lane &= GC573_FPGA_PIXELS_PER_CLOCK - 1;

	return packed;
}

static void gc573_fpga_program_horizontal_phases(void *bar0,
						 u32 input_width,
						 u32 output_width)
{
	u32 phase_accumulator = 0;
	u32 source_lane = 0;
	u32 output_pixel = 0;
	u32 phase_step = (input_width << 16) / output_width;
	u32 active_entries = (max(input_width, output_width) +
			      GC573_FPGA_PIXELS_PER_CLOCK - 1) /
			     GC573_FPGA_PIXELS_PER_CLOCK;
	u32 max_entries = GC573_FPGA_MAX_WIDTH /
			  GC573_FPGA_PIXELS_PER_CLOCK;

	for (u32 entry = 0; entry < max_entries; entry++) {
		u64 packed = 0;
		u32 offset = GC573_FPGA_HSCALER_BASE +
			     GC573_FPGA_HSCALER_PHASE_OFFSET +
			     entry * sizeof(u64);

		if (entry < active_entries)
			packed = gc573_fpga_build_phase_word(
				&phase_accumulator, &source_lane,
				&output_pixel, output_width, phase_step);

		gc573_fpga_write(bar0, offset, (u32)packed);
		gc573_fpga_write(bar0, offset + sizeof(u32),
				 (u32)(packed >> 32));
	}
}

static void gc573_fpga_setup_scalers(void *bar0,
				     const struct gc573_user_fpga_config *cfg)
{
	u32 vertical_step = (cfg->input_height << 16) / cfg->output_height;
	u32 horizontal_step = (cfg->input_width << 16) / cfg->output_width;

	gc573_fpga_program_coefficients(bar0, GC573_FPGA_VSCALER_BASE);
	gc573_fpga_write(bar0, GC573_FPGA_VSCALER_BASE + 0x10,
			 cfg->input_height);
	gc573_fpga_write(bar0, GC573_FPGA_VSCALER_BASE + 0x18,
			 cfg->input_width);
	gc573_fpga_write(bar0, GC573_FPGA_VSCALER_BASE + 0x20,
			 cfg->output_height);
	gc573_fpga_write(bar0, GC573_FPGA_VSCALER_BASE + 0x28,
			 vertical_step);

	gc573_fpga_program_coefficients(bar0, GC573_FPGA_HSCALER_BASE);
	gc573_fpga_program_horizontal_phases(bar0, cfg->input_width,
					     cfg->output_width);
	gc573_fpga_write(bar0, GC573_FPGA_HSCALER_BASE + 0x10,
			 cfg->output_height);
	gc573_fpga_write(bar0, GC573_FPGA_HSCALER_BASE + 0x18,
			 cfg->input_width);
	gc573_fpga_write(bar0, GC573_FPGA_HSCALER_BASE + 0x20,
			 cfg->output_width);
	gc573_fpga_write(bar0, GC573_FPGA_HSCALER_BASE + 0x28, 0);
	gc573_fpga_write(bar0, GC573_FPGA_HSCALER_BASE + 0x30,
			 horizontal_step);
}

static void gc573_fpga_start_scaler(void *bar0, u32 scaler_base)
{
	u32 control;

	gc573_fpga_write(bar0, scaler_base, 0x80);
	control = gc573_fpga_read(bar0, scaler_base);
	gc573_fpga_write(bar0, scaler_base, (control & 0x80) | BIT(0));
}

static u32 gc573_fpga_tlp_fix_mode(void)
{
	/* Packed YUYV is naturally aligned; the kernel uses mode 0. */
	return 0;
}

int gc573_user_fpga_configure(void *bar0,
			      const struct gc573_user_fpga_config *cfg)
{
	u32 clip_height;
	u32 frame_rate;
	int ret;

	if (!bar0 || !cfg)
		return -EINVAL;

	ret = gc573_user_fpga_validate_config(cfg);
	if (ret < 0)
		return ret;

	if (!cfg->input_width || !cfg->input_height ||
	    !cfg->output_width || !cfg->output_height)
		return -EINVAL;
	if (cfg->input_width > GC573_FPGA_MAX_WIDTH ||
	    cfg->output_width > GC573_FPGA_MAX_WIDTH ||
	    cfg->input_height > GC573_FPGA_MAX_HEIGHT ||
	    cfg->output_height > GC573_FPGA_MAX_HEIGHT)
		return -ERANGE;
	if (cfg->output_width & 1)
		return -EINVAL;

	clip_height = cfg->input_height;
	if (cfg->interlaced) {
		u32 field_height = cfg->input_height / 2U;

		clip_height = (field_height << 16) | field_height;
	}

	/* DMA enables output only after its descriptor tables are armed. */
	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_CTRL, BIT(0), 0);
	gc573_fpga_update_bits(bar0, GC573_FPGA_TLP_FIX, GENMASK(1, 0),
			       gc573_fpga_tlp_fix_mode());
	(void)gc573_fpga_read(bar0, 0x1010);
	gc573_fpga_program_color_adjustment(bar0);
	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_CTRL,
			       GC573_FPGA_OUTPUT_FORMAT_MASK, 0);
	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_CTRL, BIT(2) | BIT(3), 0);
	gc573_fpga_write(bar0, 0x1020, 0);
	gc573_fpga_write(bar0, 0x1024, cfg->input_width);
	gc573_fpga_write(bar0, 0x1028, 0);
	gc573_fpga_write(bar0, 0x102c, clip_height);
	msleep(50);

	gc573_fpga_write(bar0, GC573_FPGA_VIP_RESET, 0);
	usleep_range(5000, 6000);
	gc573_fpga_write(bar0, GC573_FPGA_VIP_RESET, 3);
	usleep_range(5000, 6000);

	gc573_fpga_setup_scalers(bar0, cfg);
	gc573_fpga_program_color_path(bar0, cfg);

	frame_rate = clamp(cfg->output_frame_rate_hz, 5U,
			   GC573_FPGA_MAX_FRAME_RATE);
	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_FRAME_RATE,
			       GENMASK(31, 16), frame_rate << 16);
	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_CTRL, BIT(5), 0);
	gc573_fpga_write(bar0, GC573_FPGA_VIP_FRAME_PERIOD,
			 gc573_user_fpga_frame_period(frame_rate,
						      cfg->input_frame_rate_x100));
	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_CTRL, BIT(7), 0);
	gc573_fpga_write(bar0, GC573_FPGA_VIP_PIXEL_MODE,
			 (cfg->ddr ? BIT(1) : 0) |
			 (cfg->dual_pixel ? BIT(0) : 0));
	gc573_fpga_start_scaler(bar0, GC573_FPGA_VSCALER_BASE);
	gc573_fpga_start_scaler(bar0, GC573_FPGA_HSCALER_BASE);
	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_CTRL, BIT(6), 0);

	return 0;
}

int gc573_user_fpga_set_output_enabled(void *bar0, bool enabled)
{
	if (!bar0)
		return -EINVAL;

	gc573_fpga_update_bits(bar0, GC573_FPGA_VIP_CTRL, BIT(0),
			       enabled ? BIT(0) : 0);
	return 0;
}

int gc573_user_fpga_reset_vip(void *bar0)
{
	if (!bar0)
		return -EINVAL;

	gc573_fpga_write(bar0, GC573_FPGA_VIP_RESET, 0);
	usleep_range(5000, 6000);
	gc573_fpga_write(bar0, GC573_FPGA_VIP_RESET, 3);

	return 0;
}

int gc573_user_fpga_reset_dma(void *bar0)
{
	u32 status = 0;
	bool completed = false;

	if (!bar0)
		return -EINVAL;

	gc573_fpga_write(bar0, GC573_DMA_RESET, GC573_DMA_RESET_VIDEO);
	for (unsigned int attempt = 0; attempt < 11; attempt++) {
		usleep_range(1000, 2000);
		status = gc573_fpga_read(bar0, GC573_DMA_RESET);
		/* Bit 0 is the request. The core reset completes on bit 3. */
		if (status & BIT(3)) {
			completed = true;
			break;
		}
	}
	msleep(30);

	if (!completed)
		pr_warn("DMA reset bit was not observed, settled status 0x%08x",
			status);

	return 0;
}

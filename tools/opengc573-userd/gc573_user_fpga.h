/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_fpga.h — Userspace GC573 FPGA VIP / scaler configuration
 *
 * Port of the VIP programming path from open_gc573/gc573-fpga.c.  The
 * userspace driver currently targets packed YUYV output; RGB input is
 * converted by the FPGA's RGB->YUV matrix, YUV input is passed through.
 */

#ifndef GC573_USER_FPGA_H
#define GC573_USER_FPGA_H

#include "gc573-user-compat.h"

#define GC573_FPGA_MAX_WIDTH		4096U
#define GC573_FPGA_MAX_HEIGHT		2160U
#define GC573_FPGA_MAX_FRAME_RATE	240U

/* Register offsets used by the VIP and DMA engines. */
#define GC573_FPGA_TLP_FIX		0x00004
#define GC573_FPGA_VIP_CTRL		0x01000
#define GC573_FPGA_VIP_FRAME_PERIOD	0x0103c
#define GC573_FPGA_VIP_COLOR_CTRL	0x01040
#define GC573_FPGA_VIP_FRAME_RATE	0x01080
#define GC573_FPGA_VIP_PIXEL_MODE	0x01088
#define GC573_FPGA_VIP_RESET		0x50000

#define GC573_DMA_RESET			0x0000c
#define GC573_DMA_INTERRUPT_STATUS	0x00010
#define GC573_DMA_INTERRUPT_ENABLE	0x0001c
#define GC573_DMA_CONTROL		0x00304
#define GC573_DMA_TABLE_BASE		0x00308
#define GC573_DMA_CHROMA_CONTROL	0x00504
#define GC573_DMA_THIRD_CONTROL		0x00604
#define GC573_DMA_VIDEO_CHANNEL		0x00300

#define GC573_DMA_IRQ_TERMINATED	BIT(0)
#define GC573_DMA_IRQ_COMPLETE		BIT(1)
#define GC573_DMA_IRQ_AUDIO_COMPLETE	BIT(5)
#define GC573_DMA_IRQ_MASK		(GC573_DMA_IRQ_TERMINATED | \
					 GC573_DMA_IRQ_COMPLETE)
#define GC573_DMA_IRQ_ACK_MASK		(0x80001fffU | BIT(13))
/*
 * The video path may acknowledge everything except the audio period bit.
 * Audio owns BIT(5) and clears it under the same IRQ lock, so neither engine
 * can erase the other's completion.
 */
#define GC573_DMA_IRQ_VIDEO_ACK_MASK \
	(GC573_DMA_IRQ_ACK_MASK & ~GC573_DMA_IRQ_AUDIO_COMPLETE)
#define GC573_DMA_DESCRIPTOR_CONTROL	0x80008000U
#define GC573_DMA_RESET_VIDEO		BIT(0)

enum gc573_user_input_class {
	GC573_USER_INPUT_SD = 0,
	GC573_USER_INPUT_HD,
	GC573_USER_INPUT_UHD,
};

enum gc573_user_encoding {
	GC573_USER_ENCODING_RGB_FULL = 0,
	GC573_USER_ENCODING_RGB_LIMITED,
	GC573_USER_ENCODING_YUV,
};

enum gc573_user_sampling {
	GC573_USER_SAMPLING_RGB = 0,
	GC573_USER_SAMPLING_YUV422,
	GC573_USER_SAMPLING_YUV444,
	GC573_USER_SAMPLING_YUV420,
};

enum gc573_user_colorimetry {
	GC573_USER_COLORIMETRY_UNKNOWN = 0,
	GC573_USER_COLORIMETRY_BT601,
	GC573_USER_COLORIMETRY_BT709,
	GC573_USER_COLORIMETRY_BT2020,
};

struct gc573_user_fpga_config {
	u32 input_width;
	u32 input_height;
	u32 output_width;
	u32 output_height;
	u32 input_frame_rate_hz;
	u32 input_frame_rate_x100;
	u32 output_frame_rate_hz;
	enum gc573_user_input_class input_class;
	enum gc573_user_encoding input_encoding;
	enum gc573_user_sampling input_sampling;
	enum gc573_user_colorimetry input_colorimetry;
	bool interlaced;
	bool dual_pixel;
	bool ddr;
};

/**
 * gc573_user_fpga_mode_known - Check a mode against the kernel's verified table
 * @width: Active input width
 * @height: Active input height
 * @frame_rate_hz: Normalized input frame rate
 * @frame_rate_x100: Measured field/frame rate in 1/100 Hz (may be 0)
 *
 * The userspace engine still attempts unknown modes, but logging them makes
 * retro scaler and hot-switch diagnosis much easier.
 */
bool gc573_user_fpga_mode_known(u32 width, u32 height, u32 frame_rate_hz,
				u32 frame_rate_x100);

/**
 * gc573_user_fpga_validate_config - Reject configurations the VIP cannot run
 *
 * Mirrors the hard limits from gc555_fpga_validate_config().  Input and
 * output geometry must match: the userspace DMA path does not wire the FPGA
 * downscaler, and a mismatched output leaves the VIP producing input-height
 * lines into a short buffer (DMA stall).
 */
int gc573_user_fpga_validate_config(const struct gc573_user_fpga_config *cfg);

/**
 * gc573_user_fpga_configure - Program the VIP, color path and scalers
 * @bar0: Mapped BAR0
 * @cfg: Input/output description
 *
 * Leaves the VIP output disabled; call gc573_user_fpga_set_output_enabled()
 * after the DMA descriptor tables have been armed.
 */
int gc573_user_fpga_configure(void *bar0,
			      const struct gc573_user_fpga_config *cfg);

int gc573_user_fpga_set_output_enabled(void *bar0, bool enabled);

int gc573_user_fpga_reset_vip(void *bar0);

/**
 * gc573_user_fpga_reset_dma - Reset the video DMA engine
 *
 * Mirrors gc555_dma_reset_video(): pulse RESET bit 0 and wait for it to
 * self-clear.
 */
int gc573_user_fpga_reset_dma(void *bar0);

u32 gc573_user_fpga_frame_period(u32 frame_rate_hz, u32 frame_rate_x100);

#endif /* GC573_USER_FPGA_H */

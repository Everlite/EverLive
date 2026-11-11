/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_hdr.h — HDR10/HLG to SDR tonemapping for the EverLive streamer
 *
 * The FPGA delivers BT.2020 YCbCr with the transfer function untouched, so an
 * HDR source arrives as PQ- or HLG-encoded YUYV.  Interpreting that as BT.709
 * gamma gives the washed-out, milky picture described in issue #42.
 *
 * The mapping runs on the packed YUYV frame before it is handed to SDL and to
 * the V4L2 loopback sink, so the gaming window and OBS see the same result.
 * It is a per-luma 8-bit lookup table plus a 2x2 chroma matrix derived from the
 * BT.2020 -> BT.709 conversion: one table lookup and four multiplies per two
 * pixels, which keeps the pass memory-bandwidth bound (~1-3 ms at 1440p,
 * ~3-6 ms at 4K on one core).
 */

#ifndef GC573_USER_HDR_H
#define GC573_USER_HDR_H

#include <stdbool.h>

enum gc573_hdr_type {
	GC573_HDR_SDR = 0,
	GC573_HDR_PQ,		/* SMPTE ST 2084 (HDR10) */
	GC573_HDR_HLG,		/* Hybrid Log-Gamma (BT.2100) */
};

/**
 * gc573_hdr_init - Precompute the static BT.2020 -> BT.709 chroma matrix
 *
 * Call once before the render loop.  Idempotent.
 */
void gc573_hdr_init(void);

/**
 * gc573_hdr_set_mode - Select the transfer function and rebuild the LUT
 * @type: GC573_HDR_SDR disables the pass
 * @exposure: linear exposure bias (1.0 = neutral, 2.0 = +1 EV, 0.5 = -1 EV)
 *
 * Rebuilding the 256-entry table costs microseconds and only happens when the
 * signal or the exposure changes.
 */
void gc573_hdr_set_mode(enum gc573_hdr_type type, float exposure);

/**
 * gc573_hdr_active - True when the tonemapping pass is enabled
 */
bool gc573_hdr_active(void);

/**
 * gc573_hdr_tonemap_yuyv - Tonemap a packed YUYV frame in place
 * @frame: Packed YUYV buffer
 * @width: Frame width (must be even)
 * @height: Frame height
 *
 * Safe to call from the render thread; touches only the frame buffer.
 */
void gc573_hdr_tonemap_yuyv(void *frame, unsigned int width,
			    unsigned int height);

/**
 * gc573_hdr_label - Short human readable status for OSD/log
 */
const char *gc573_hdr_label(void);

#endif /* GC573_USER_HDR_H */

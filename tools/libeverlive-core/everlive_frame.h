// SPDX-License-Identifier: GPL-2.0-only
#ifndef EVERLIVE_FRAME_H
#define EVERLIVE_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert packed YUY2 (YUYV) 4:2:2 buffer to packed 24-bit RGB (RGB888).
 *
 * Fast fixed-point conversion using the BT.601 color matrix.
 *
 * @param src     Pointer to source YUY2 buffer (width * height * 2 bytes).
 * @param dst     Pointer to destination RGB888 buffer (width * height * 3 bytes).
 * @param width   Frame width in pixels (must be even).
 * @param height  Frame height in pixels.
 */
void everlive_yuy2_to_rgb(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * Convert and downscale packed YUY2 (YUYV) buffer to packed 24-bit RGB (RGB888).
 *
 * Uses nearest-neighbour point sampling for low-overhead preview scaling.
 *
 * @param src    Pointer to source YUY2 buffer.
 * @param dst    Pointer to destination RGB888 buffer.
 * @param src_w  Source width in pixels.
 * @param src_h  Source height in pixels.
 * @param dst_w  Destination width in pixels.
 * @param dst_h  Destination height in pixels.
 */
void everlive_yuy2_to_rgb_scaled(const uint8_t *src, uint8_t *dst,
                                int src_w, int src_h,
                                int dst_w, int dst_h);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_FRAME_H */

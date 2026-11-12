// SPDX-License-Identifier: GPL-2.0-only
#include "everlive_frame.h"

static inline uint8_t clamp_u8(int val)
{
	if (val < 0)
		return 0;
	if (val > 255)
		return 255;
	return (uint8_t)val;
}

/* Arithmetic right shift by 8 without relying on implementation-defined
 * behaviour for negative operands. */
static inline int shift8(int val)
{
	return val >= 0 ? (val >> 8) : -(((-val) + 255) >> 8);
}

void everlive_yuy2_to_rgb(const uint8_t *src, uint8_t *dst, int width, int height)
{
	if (!src || !dst || width <= 0 || height <= 0)
		return;

	/* YUY2 packs two pixels into each 4-byte macro-pixel, so an odd width
	 * cannot be represented; reject it rather than desynchronising rows. */
	if ((width & 1) != 0)
		return;

	size_t total_pairs = ((size_t)width * (size_t)height) / 2;
	const uint8_t *s = src;
	uint8_t *d = dst;

	for (size_t i = 0; i < total_pairs; i++) {
		int y0 = s[0];
		int u  = s[1] - 128;
		int y1 = s[2];
		int v  = s[3] - 128;
		s += 4;

		int r_off = shift8(359 * v);
		int g_off = shift8(88 * u + 183 * v);
		int b_off = shift8(454 * u);

		/* Pixel 0 */
		d[0] = clamp_u8(y0 + r_off);
		d[1] = clamp_u8(y0 - g_off);
		d[2] = clamp_u8(y0 + b_off);

		/* Pixel 1 */
		d[3] = clamp_u8(y1 + r_off);
		d[4] = clamp_u8(y1 - g_off);
		d[5] = clamp_u8(y1 + b_off);
		d += 6;
	}
}

void everlive_yuy2_to_rgb_scaled(const uint8_t *src, uint8_t *dst,
				 int src_w, int src_h,
				 int dst_w, int dst_h)
{
	if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || (src_w & 1))
		return;

	/* Fixed-point step in 16.16 format */
	uint32_t step_x = ((uint32_t)src_w << 16) / (uint32_t)dst_w;
	uint32_t step_y = ((uint32_t)src_h << 16) / (uint32_t)dst_h;

	uint32_t src_y_fp = 0;
	for (int dy = 0; dy < dst_h; dy++) {
		int sy = (int)(src_y_fp >> 16);
		if (sy >= src_h)
			sy = src_h - 1;
		src_y_fp += step_y;

		const uint8_t *row = src + (size_t)sy * src_w * 2;
		uint8_t *d = dst + (size_t)dy * dst_w * 3;
		uint32_t src_x_fp = 0;

		for (int dx = 0; dx < dst_w; dx++) {
			int sx = (int)(src_x_fp >> 16);
			if (sx >= src_w)
				sx = src_w - 1;
			src_x_fp += step_x;

			/* In YUY2, pair index is (sx & ~1) * 2 */
			int pair_base = (sx & ~1) * 2;
			int y = (sx & 1) ? row[pair_base + 2] : row[pair_base + 0];
			int u = row[pair_base + 1] - 128;
			int v = row[pair_base + 3] - 128;

			int r_off = shift8(359 * v);
			int g_off = shift8(88 * u + 183 * v);
			int b_off = shift8(454 * u);

			d[0] = clamp_u8(y + r_off);
			d[1] = clamp_u8(y - g_off);
			d[2] = clamp_u8(y + b_off);
			d += 3;
		}
	}
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unit tests for EverLive Native motion-compensated frame generation
 * (gc573_user_fg_native.c).
 */
#include "gc573_user_fg_native.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass;

#define CHECK(cond, msg) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "  [FAIL] %s\n", (msg));		\
		return 1;						\
	}								\
	printf("  [PASS] %s\n", (msg));					\
	g_pass++;							\
} while (0)

static void fill_pattern_yuyv(uint8_t *buf, uint32_t w, uint32_t h,
			      uint32_t stride, int shift_x)
{
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = buf + (size_t)y * stride;
		for (uint32_t x = 0; x < w; x += 2U) {
			int sx0 = (int)x - shift_x;
			int sx1 = (int)x + 1 - shift_x;
			uint8_t y0 = (sx0 >= 24 && sx0 < 56 && y >= 24 && y < 56) ? 220 : 32;
			uint8_t y1 = (sx1 >= 24 && sx1 < 56 && y >= 24 && y < 56) ? 220 : 32;
			row[x * 2U + 0] = y0;
			row[x * 2U + 1] = 128;
			row[x * 2U + 2] = y1;
			row[x * 2U + 3] = 128;
		}
	}
}

int main(void)
{
	struct gc573_fg_native *ctx;
	const uint32_t w = 128;
	const uint32_t h = 96;
	const uint32_t pad_stride = w * 2U + 32U;
	uint8_t *f0 = calloc(1, (size_t)pad_stride * h);
	uint8_t *f1 = calloc(1, (size_t)pad_stride * h);
	uint8_t *out = calloc(1, (size_t)w * 2U * h);
	uint8_t dummy[64] = { 0 };

	assert(f0 && f1 && out);
	printf("=== GC573 Native Framegen Tests ===\n");

	ctx = gc573_fg_native_create();
	CHECK(ctx != NULL, "gc573_fg_native_create returns valid context");
	CHECK(!gc573_fg_native_ready(ctx), "not ready before two frames pushed");
	CHECK(gc573_fg_native_prev(ctx) == NULL, "prev is NULL before ready");

	/* Bounds & invalid dimension checks */
	CHECK(!gc573_fg_native_push(ctx, NULL, w, h, w * 2U), "rejects NULL frame");
	CHECK(!gc573_fg_native_push(ctx, dummy, 32, 32, 64), "rejects undersized frame");
	CHECK(!gc573_fg_native_push(ctx, dummy, 127, 96, 254), "rejects odd width");
	CHECK(!gc573_fg_native_push(ctx, dummy, 16384, 4320, 32768), "rejects oversized >8K width");

	/* Non-compact stride + motion synthesis */
	fill_pattern_yuyv(f0, w, h, pad_stride, 0);
	fill_pattern_yuyv(f1, w, h, pad_stride, 8);

	CHECK(!gc573_fg_native_push(ctx, f0, w, h, pad_stride), "first push returns false (not ready yet)");
	CHECK(gc573_fg_native_push(ctx, f1, w, h, pad_stride), "second push returns true (ready)");
	CHECK(gc573_fg_native_ready(ctx), "ready after two valid frames");
	CHECK(gc573_fg_native_prev(ctx) != NULL, "prev pointer available when ready");

	/* Synthesize with out-of-range t values and midpoint */
	CHECK(gc573_fg_native_synthesize(ctx, -0.5f, out, w * 2U), "synthesize clamps t < 0");
	CHECK(gc573_fg_native_synthesize(ctx, 1.5f, out, w * 2U), "synthesize clamps t > 1");
	CHECK(gc573_fg_native_synthesize(ctx, 0.5f, out, w * 2U), "synthesize midpoint t = 0.5 succeeds");

	/* Reset clears readiness */
	gc573_fg_native_reset(ctx);
	CHECK(!gc573_fg_native_ready(ctx), "reset clears ready state");
	CHECK(!gc573_fg_native_synthesize(ctx, 0.5f, out, w * 2U), "synthesize fails after reset");

	gc573_fg_native_destroy(ctx);
	free(f0);
	free(f1);
	free(out);
	printf("All %d native framegen tests passed.\n", g_pass);
	return 0;
}

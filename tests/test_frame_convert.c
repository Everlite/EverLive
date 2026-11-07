// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "everlive_frame.h"

static void test_yuy2_to_rgb_pure_colors(void)
{
	printf("[*] Testing pure color YUY2 -> RGB conversions...\n");

	/* Test Black: Y=16 (or 0), U=128, V=128 */
	uint8_t black_yuy2[4] = { 0, 128, 0, 128 };
	uint8_t black_rgb[6] = { 0 };
	everlive_yuy2_to_rgb(black_yuy2, black_rgb, 2, 1);
	assert(black_rgb[0] == 0 && black_rgb[1] == 0 && black_rgb[2] == 0);
	assert(black_rgb[3] == 0 && black_rgb[4] == 0 && black_rgb[5] == 0);

	/* Test White: Y=255, U=128, V=128 */
	uint8_t white_yuy2[4] = { 255, 128, 255, 128 };
	uint8_t white_rgb[6] = { 0 };
	everlive_yuy2_to_rgb(white_yuy2, white_rgb, 2, 1);
	assert(white_rgb[0] == 255 && white_rgb[1] == 255 && white_rgb[2] == 255);
	assert(white_rgb[3] == 255 && white_rgb[4] == 255 && white_rgb[5] == 255);

	/* Test Red: Y=81, U=90, V=240 */
	uint8_t red_yuy2[4] = { 81, 90, 81, 240 };
	uint8_t red_rgb[6] = { 0 };
	everlive_yuy2_to_rgb(red_yuy2, red_rgb, 2, 1);
	/* Red component should be dominant */
	assert(red_rgb[0] > 200);
	assert(red_rgb[1] < 100);
	assert(red_rgb[2] < 100);

	printf("    Pure colors OK.\n");
}

static void test_yuy2_to_rgb_scaled(void)
{
	printf("[*] Testing YUY2 -> RGB downscaling...\n");

	int src_w = 1920;
	int src_h = 1080;
	int dst_w = 960;
	int dst_h = 540;

	uint8_t *src = malloc(src_w * src_h * 2);
	uint8_t *dst = malloc(dst_w * dst_h * 3);
	assert(src && dst);

	/* Fill with alternating pattern */
	for (int i = 0; i < src_w * src_h * 2; i += 4) {
		src[i + 0] = 128; /* Y0 */
		src[i + 1] = 64;  /* U */
		src[i + 2] = 200; /* Y1 */
		src[i + 3] = 192; /* V */
	}

	everlive_yuy2_to_rgb_scaled(src, dst, src_w, src_h, dst_w, dst_h);

	/* Verify non-zero scaled output and bounds */
	bool non_zero = false;
	for (int i = 0; i < dst_w * dst_h * 3; i++) {
		if (dst[i] > 0)
			non_zero = true;
	}
	assert(non_zero);

	free(src);
	free(dst);
	printf("    Scaled conversion OK.\n");
}

static void test_invalid_parameters(void)
{
	printf("[*] Testing edge cases & NULL robustness...\n");
	uint8_t buf[16] = { 0 };

	/* NULL pointers must not crash */
	everlive_yuy2_to_rgb(NULL, buf, 2, 1);
	everlive_yuy2_to_rgb(buf, NULL, 2, 1);
	everlive_yuy2_to_rgb(buf, buf, 0, 1);
	everlive_yuy2_to_rgb(buf, buf, 2, 0);

	everlive_yuy2_to_rgb_scaled(NULL, buf, 4, 4, 2, 2);
	everlive_yuy2_to_rgb_scaled(buf, NULL, 4, 4, 2, 2);
	everlive_yuy2_to_rgb_scaled(buf, buf, 0, 4, 2, 2);
	everlive_yuy2_to_rgb_scaled(buf, buf, 4, 0, 2, 2);
	everlive_yuy2_to_rgb_scaled(buf, buf, 4, 4, 0, 2);
	everlive_yuy2_to_rgb_scaled(buf, buf, 4, 4, 2, 0);

	printf("    Edge cases OK.\n");
}

int main(void)
{
	printf("=== Running Frame Converter Unit Tests ===\n");
	test_yuy2_to_rgb_pure_colors();
	test_yuy2_to_rgb_scaled();
	test_invalid_parameters();
	printf("=== All Frame Converter Tests Passed! ===\n");
	return 0;
}

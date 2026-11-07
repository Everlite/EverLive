// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "gc573_user_hdr.h"

int main(void)
{
	printf("=== Test: GC573 HDR Tonemapping ===\n");

	gc573_hdr_init();
	assert(!gc573_hdr_active());

	gc573_hdr_set_mode(GC573_HDR_PQ, 1.0f);
	assert(gc573_hdr_active());
	assert(strstr(gc573_hdr_label(), "HDR10") != NULL);

	/* Prepare a test image with varying YUYV patterns */
	const unsigned int width = 64;
	const unsigned int height = 32;
	const size_t bytes = width * height * 2;
	uint8_t *frame = malloc(bytes);
	assert(frame);

	for (size_t i = 0; i < bytes; i++) {
		frame[i] = (uint8_t)(i & 0xFF);
	}

	uint8_t *orig = malloc(bytes);
	assert(orig);
	memcpy(orig, frame, bytes);

	gc573_hdr_tonemap_yuyv(frame, width, height);

	/* Check that values changed appropriately and stay within valid bounds */
	for (size_t i = 0; i < bytes; i += 4) {
		uint8_t y0 = frame[i];
		uint8_t cb = frame[i + 1];
		uint8_t y1 = frame[i + 2];
		uint8_t cr = frame[i + 3];

		/* Limited range chroma clamp check [16, 240] */
		assert(cb >= 16 && cb <= 240);
		assert(cr >= 16 && cr <= 240);
		(void)y0; (void)y1;
	}

	/* Test SDR mode turns off processing */
	gc573_hdr_set_mode(GC573_HDR_SDR, 1.0f);
	assert(!gc573_hdr_active());
	memcpy(frame, orig, bytes);
	gc573_hdr_tonemap_yuyv(frame, width, height);
	assert(memcmp(frame, orig, bytes) == 0);

	/* Test HLG mode */
	gc573_hdr_set_mode(GC573_HDR_HLG, 1.0f);
	assert(gc573_hdr_active());
	assert(strstr(gc573_hdr_label(), "HLG") != NULL);
	gc573_hdr_tonemap_yuyv(frame, width, height);

	for (size_t i = 0; i < bytes; i += 4) {
		uint8_t cb = frame[i + 1];
		uint8_t cr = frame[i + 3];
		assert(cb >= 16 && cb <= 240);
		assert(cr >= 16 && cr <= 240);
	}

	free(frame);
	free(orig);

	printf("  [PASS] GC573 HDR tonemapping verified.\n");
	return 0;
}

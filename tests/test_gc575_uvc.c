// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_gc575_uvc.c — pure unit tests for the GC575 AT framing and YUY2 path
 *
 * No hardware is touched.  These tests lock down the recovered AT envelope
 * and the NV12→YUY2 conversion used by opengc575-userd.
 */

#include "gc575_v4l2.h"
#include "gc575_xu.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_PASS(desc) printf("  [PASS] %s\n", (desc))

static void test_at_framing(void)
{
	uint8_t frame[32];
	size_t len;

	/* Captured AT23 request: a1 06 00 00 23 00 00 00 36 */
	len = gc575_at_build(1, 0x23, NULL, 0, frame, sizeof(frame));
	assert(len == 9);
	assert(frame[0] == 0xa1);
	assert(frame[1] == 0x06);
	assert(frame[2] == 0x00);
	assert(frame[3] == 0x00);
	assert(frame[4] == 0x23);
	assert(frame[5] == 0x00);
	assert(frame[6] == 0x00);
	assert(frame[7] == 0x00);
	assert(frame[8] == 0x36);
	TEST_PASS("AT23 request frame matches the captured envelope");

	/* AT b0 profile 1: service 1, command 0xb0, 8-byte payload. */
	{
		uint8_t payload[8] = { 1, 0, 0, 0, 1, 0, 0, 0 };
		len = gc575_at_build(1, 0xb0, payload, sizeof(payload),
				     frame, sizeof(frame));
		assert(len == 17);
		assert(frame[0] == 0xa1);
		assert(frame[1] == 0x0e); /* L = 12 -> L + 2 */
		assert(frame[4] == 0xb0);
		assert(frame[12] == 1);   /* profile index */
	}
	TEST_PASS("AT b0 profile payload is encoded correctly");
}

static void test_yuy2_passthrough(void)
{
	struct gc575_capture cap;
	struct v4l2_buffer buf;
	uint8_t src[16] = {
		1, 2, 3, 4, 5, 6, 7, 8,
		9, 10, 11, 12, 13, 14, 15, 16,
	};
	uint8_t dst[16] = { 0 };
	size_t out = 0;

	memset(&cap, 0, sizeof(cap));
	cap.width = 4;
	cap.height = 2;
	cap.bytesperline = 8;
	cap.pixelformat = V4L2_PIX_FMT_YUYV;
	cap.count = 1;
	cap.buffers[0].start = src;
	cap.buffers[0].length = sizeof(src);

	memset(&buf, 0, sizeof(buf));
	buf.index = 0;
	buf.bytesused = sizeof(src);

	assert(gc575_convert_to_yuy2(&cap, &buf, dst, sizeof(dst), &out) == 0);
	assert(out == sizeof(src));
	assert(memcmp(dst, src, sizeof(src)) == 0);
	TEST_PASS("YUYV frames pass through unchanged");
}

static void test_nv12_to_yuy2(void)
{
	struct gc575_capture cap;
	struct v4l2_buffer buf;
	/*
	 * 4x2 NV12: Y plane 8 bytes, UV plane 4 bytes (U,V,U,V per row pair).
	 * Expected YUY2 row 0: Y0 U0 Y1 V0  Y2 U1 Y3 V1
	 * Expected YUY2 row 1: Y4 U0 Y5 V0  Y6 U1 Y7 V1
	 */
	uint8_t src[12] = {
		10, 20, 30, 40, 50, 60, 70, 80,  /* Y */
		100, 110, 120, 130,             /* U0 V0 U1 V1 */
	};
	uint8_t dst[16] = { 0 };
	size_t out = 0;

	memset(&cap, 0, sizeof(cap));
	cap.width = 4;
	cap.height = 2;
	cap.bytesperline = 4;
	cap.pixelformat = V4L2_PIX_FMT_NV12;
	cap.count = 1;
	cap.buffers[0].start = src;
	cap.buffers[0].length = sizeof(src);

	memset(&buf, 0, sizeof(buf));
	buf.index = 0;
	buf.bytesused = sizeof(src);

	assert(gc575_convert_to_yuy2(&cap, &buf, dst, sizeof(dst), &out) == 0);
	assert(out == sizeof(dst));
	assert(dst[0] == 10 && dst[1] == 100 && dst[2] == 20 && dst[3] == 110);
	assert(dst[4] == 30 && dst[5] == 120 && dst[6] == 40 && dst[7] == 130);
	assert(dst[8] == 50 && dst[9] == 100 && dst[10] == 60 && dst[11] == 110);
	assert(dst[12] == 70 && dst[13] == 120 && dst[14] == 80 && dst[15] == 130);
	TEST_PASS("NV12→YUY2 conversion matches the expected 2x2 block layout");
}

int main(void)
{
	printf("==================================================\n");
	printf(" GC575 UVC framing and YUY2 conversion tests      \n");
	printf("==================================================\n");

	test_at_framing();
	test_yuy2_passthrough();
	test_nv12_to_yuy2();

	printf("==================================================\n");
	printf(" ALL GC575 UVC TESTS PASSED!\n");
	printf("==================================================\n");
	return 0;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_v4l2_shim.c — Unit tests for the shared V4L2 virtual-camera state
 * machine.  The libc interposer is intentionally not linked here.
 */

#include <errno.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "everlive_framestore.h"
#include "everlive_v4l2_card.h"
#include "everlive_v4l2_state.h"

#define TEST_SHM_NAME "/everlive_v4l2_shim_test"

static struct everlive_framestore *g_fs;

static const struct everlive_v4l2_size test_sizes[] = {
	{ 1920, 1080 },
	{ 1280, 720 },
	{ 640, 360 },
};

static const uint32_t test_fps[] = { 60, 30, 50 };

static const struct everlive_v4l2_clamp test_clamps[] = {
	{ 640, 480, 640, 360 },
	{ 1280, 720, 1280, 720 },
	{ 1920, 1080, 1920, 1080 },
};

static const struct everlive_v4l2_card test_card = {
	.shm_name = TEST_SHM_NAME,
	.slug = "test",
	.driver = "test-user",
	.card_name = "Test EverLive Camera",
	.port_names = { "Test HDMI 1", "Test HDMI 2" },
	.bus_suffixes = { "-hdmi1", "-hdmi2" },
	.fallback_bdf = "0000:06:00.0",
	.node_base = 60,
	.port_count = 2,
	.max_width = 1920,
	.max_height = 1080,
	.default_width = 1920,
	.default_height = 1080,
	.mmap_base = 0x50000000U,
	.mmap_stride = 0x00800000U,
	.sizes = test_sizes,
	.size_count = sizeof(test_sizes) / sizeof(test_sizes[0]),
	.fps_denominators = test_fps,
	.fps_count = sizeof(test_fps) / sizeof(test_fps[0]),
	.clamps = test_clamps,
	.clamp_count = sizeof(test_clamps) / sizeof(test_clamps[0]),
};

const struct everlive_v4l2_card *everlive_v4l2_card_descriptor(void)
{
	return &test_card;
}

static void cleanup(void)
{
	if (g_fs) {
		everlive_framestore_destroy(g_fs);
		g_fs = NULL;
	}
	everlive_v4l2_state_shutdown();
	shm_unlink(TEST_SHM_NAME);
}

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		cleanup(); \
		exit(1); \
	} \
} while (0)

static void check_fmt(struct everlive_v4l2_handle *h, uint32_t req_w,
		      uint32_t req_h, uint32_t req_pf, uint32_t want_w,
		      uint32_t want_h, uint32_t want_pf)
{
	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = req_w;
	fmt.fmt.pix.height = req_h;
	fmt.fmt.pix.pixelformat = req_pf;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_TRY_FMT, &fmt) == 0);
	CHECK(fmt.fmt.pix.width == want_w);
	CHECK(fmt.fmt.pix.height == want_h);
	CHECK(fmt.fmt.pix.pixelformat == want_pf);
	CHECK(fmt.fmt.pix.field == V4L2_FIELD_NONE);
	CHECK(fmt.fmt.pix.sizeimage ==
	      want_w * want_h * (want_pf == V4L2_PIX_FMT_YUYV ? 2U : 4U));
}

static void test_state_machine(void)
{
	struct everlive_framestore_config cfg = {
		.name = TEST_SHM_NAME,
		.pci_bdf = "0000:06:00.0",
		.port_count = 2,
		.slot_count = 3,
		.frame_stride = 1920 * 1080 * 2,
	};
	struct everlive_v4l2_handle *h;
	struct v4l2_capability cap;
	struct v4l2_fmtdesc fmtdesc;
	struct v4l2_frmsizeenum frmsize;
	struct v4l2_frmivalenum frmival;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer buf;
	struct v4l2_input input;
	struct v4l2_streamparm parm;
	uint8_t *frame;
	uint8_t *readback;
	const size_t frame_bytes = 640U * 360U * 2U;
	int fd;

	cleanup();
	g_fs = everlive_framestore_create(&cfg);
	CHECK(g_fs != NULL);
	CHECK(everlive_framestore_publish(g_fs, 0, NULL, 0, 0, 0, false,
					 true) == false);

	CHECK(everlive_v4l2_state_init(everlive_v4l2_card_descriptor()) == 0);
	fd = everlive_v4l2_open_virtual(0);
	CHECK(fd >= 0);
	h = everlive_v4l2_find_handle(fd);
	CHECK(h != NULL);
	CHECK(h->port == 0);
	CHECK(h->width == 1920 && h->height == 1080);
	CHECK(h->pixfmt == V4L2_PIX_FMT_YUYV);

	/* QUERYCAP uses the BDF from the store and the port suffix. */
	memset(&cap, 0, sizeof(cap));
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_QUERYCAP, &cap) == 0);
	CHECK(strcmp((char *)cap.driver, "test-user") == 0);
	CHECK(strcmp((char *)cap.card, "Test HDMI 1") == 0);
	CHECK(strcmp((char *)cap.bus_info, "PCI:0000:06:00.0-hdmi1") == 0);
	CHECK(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE);
	CHECK(cap.device_caps & V4L2_CAP_STREAMING);
	CHECK(cap.device_caps & V4L2_CAP_READWRITE);

	/* All seven formats are enumerated. */
	for (uint32_t i = 0; i < 7; i++) {
		memset(&fmtdesc, 0, sizeof(fmtdesc));
		fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		fmtdesc.index = i;
		CHECK(everlive_v4l2_ioctl(h, VIDIOC_ENUM_FMT, &fmtdesc) == 0);
		CHECK(fmtdesc.pixelformat != 0);
	}
	memset(&fmtdesc, 0, sizeof(fmtdesc));
	fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmtdesc.index = 7;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_ENUM_FMT, &fmtdesc) == -1);
	CHECK(errno == EINVAL);

	/* Default format. */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_G_FMT, &fmt) == 0);
	CHECK(fmt.fmt.pix.width == 1920);
	CHECK(fmt.fmt.pix.height == 1080);
	CHECK(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV);

	/* Clamping behaviour matches the legacy GC570D engine. */
	check_fmt(h, 1920, 1080, V4L2_PIX_FMT_YUYV, 1920, 1080,
		  V4L2_PIX_FMT_YUYV);
	check_fmt(h, 1280, 720, V4L2_PIX_FMT_YUYV, 1280, 720,
		  V4L2_PIX_FMT_YUYV);
	check_fmt(h, 640, 480, V4L2_PIX_FMT_YUYV, 640, 360,
		  V4L2_PIX_FMT_YUYV);
	check_fmt(h, 1921, 1081, V4L2_PIX_FMT_YUYV, 1920, 1080,
		  V4L2_PIX_FMT_YUYV);
	check_fmt(h, 641, 361, V4L2_PIX_FMT_YUYV, 1280, 720,
		  V4L2_PIX_FMT_YUYV);
	check_fmt(h, 800, 600, 0x12345678U, 1280, 720,
		  V4L2_PIX_FMT_YUYV);

	/* Set a small format for the buffer tests. */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 640;
	fmt.fmt.pix.height = 360;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_S_FMT, &fmt) == 0);
	CHECK(h->width == 640 && h->height == 360);

	/* Enumerations. */
	for (uint32_t i = 0; i < 3; i++) {
		memset(&frmsize, 0, sizeof(frmsize));
		frmsize.index = i;
		CHECK(everlive_v4l2_ioctl(h, VIDIOC_ENUM_FRAMESIZES,
					  &frmsize) == 0);
		CHECK(frmsize.discrete.width == test_sizes[i].width);
		CHECK(frmsize.discrete.height == test_sizes[i].height);
	}
	memset(&frmsize, 0, sizeof(frmsize));
	frmsize.index = 3;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_ENUM_FRAMESIZES, &frmsize) == -1);
	CHECK(errno == EINVAL);
	for (uint32_t i = 0; i < 3; i++) {
		memset(&frmival, 0, sizeof(frmival));
		frmival.index = i;
		CHECK(everlive_v4l2_ioctl(h, VIDIOC_ENUM_FRAMEINTERVALS,
					  &frmival) == 0);
		CHECK(frmival.discrete.numerator == 1);
		CHECK(frmival.discrete.denominator == test_fps[i]);
	}
	memset(&frmival, 0, sizeof(frmival));
	frmival.index = 3;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_ENUM_FRAMEINTERVALS,
				  &frmival) == -1);
	CHECK(errno == EINVAL);

	/* Inputs and timing. */
	memset(&input, 0, sizeof(input));
	input.index = 0;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_ENUMINPUT, &input) == 0);
	CHECK(strcmp((char *)input.name, "HDMI IN 1") == 0);
	input.index = 1;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_ENUMINPUT, &input) == -1);
	CHECK(errno == EINVAL);
	{
		int selected = -1;

		CHECK(everlive_v4l2_ioctl(h, VIDIOC_G_INPUT, &selected) == 0);
		CHECK(selected == 0);
		selected = 0;
		CHECK(everlive_v4l2_ioctl(h, VIDIOC_S_INPUT, &selected) == 0);
		selected = 1;
		CHECK(everlive_v4l2_ioctl(h, VIDIOC_S_INPUT, &selected) == -1);
		CHECK(errno == EINVAL);
	}
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_G_PARM, &parm) == 0);
	CHECK(parm.parm.capture.readbuffers == 4);
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = 30;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_S_PARM, &parm) == 0);
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_G_PARM, &parm) == 0);
	CHECK(parm.parm.capture.timeperframe.numerator == 1);
	CHECK(parm.parm.capture.timeperframe.denominator == 30);

	/* Read path before any frame is published: black output. */
	readback = malloc(frame_bytes);
	CHECK(readback != NULL);
	memset(readback, 0xaa, frame_bytes);
	CHECK(everlive_v4l2_read_frame(fd, readback, frame_bytes - 1) == -1);
	CHECK(errno == EINVAL);
	CHECK(everlive_v4l2_read_frame(fd, readback, frame_bytes) ==
	      (ssize_t)frame_bytes);
	for (size_t i = 0; i < frame_bytes; i++)
		CHECK(readback[i] == 0);

	/* REQBUFS clamps 1 to 2 and allocates the right size. */
	memset(&rb, 0, sizeof(rb));
	rb.count = 1;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_REQBUFS, &rb) == 0);
	CHECK(rb.count == 2);
	CHECK(h->req_count == 2);
	CHECK(h->bufs[0].size == frame_bytes);

	/* Format changes are rejected after buffers exist. */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 1280;
	fmt.fmt.pix.height = 720;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_S_FMT, &fmt) == -1);
	CHECK(errno == EBUSY);

	memset(&buf, 0, sizeof(buf));
	buf.index = 0;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_QUERYBUF, &buf) == 0);
	CHECK(buf.length == frame_bytes);
	CHECK(buf.bytesused == frame_bytes);
	CHECK(buf.m.offset == test_card.mmap_base);
	CHECK(everlive_v4l2_mmap_buffer(fd, buf.m.offset) != NULL);
	CHECK(everlive_v4l2_mmap_buffer(fd,
				       test_card.mmap_base +
				       test_card.mmap_stride) != NULL);
	CHECK(everlive_v4l2_mmap_buffer(fd,
				       test_card.mmap_base +
				       3ULL * test_card.mmap_stride) == NULL);
	CHECK(everlive_v4l2_mmap_buffer(fd, 0) == NULL);

	CHECK(everlive_v4l2_ioctl(h, VIDIOC_QBUF, &buf) == 0);
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_DQBUF, &buf) == -1);
	CHECK(errno == EAGAIN);

	/* Publish a frame, then stream it. */
	frame = malloc(frame_bytes);
	CHECK(frame != NULL);
	memset(frame, 0x80, frame_bytes);
	CHECK(everlive_framestore_publish(g_fs, 0, frame, frame_bytes,
					  640, 360, false, true));
	{
		int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		CHECK(everlive_v4l2_ioctl(h, VIDIOC_STREAMON, &type) == 0);
	}
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_DQBUF, &buf) == 0);
	CHECK(buf.index == 0);
	CHECK(buf.bytesused == frame_bytes);
	CHECK(buf.length == frame_bytes);
	CHECK(buf.sequence == 0);
	{
		const uint8_t *p = h->bufs[0].addr;

		for (size_t i = 0; i < frame_bytes; i++)
			CHECK(p[i] == 0x80);
	}
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_QBUF, &buf) == 0);
	{
		int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		CHECK(everlive_v4l2_ioctl(h, VIDIOC_STREAMOFF, &type) == 0);
	}

	/* Full-frame read path now sees the published frame. */
	memset(readback, 0xaa, frame_bytes);
	CHECK(everlive_v4l2_read_frame(fd, readback, frame_bytes) ==
	      (ssize_t)frame_bytes);
	for (size_t i = 0; i < frame_bytes; i++)
		CHECK(readback[i] == 0x80);

	/* REQBUFS 99 clamps to 8; 0 frees everything. */
	memset(&rb, 0, sizeof(rb));
	rb.count = 99;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_REQBUFS, &rb) == 0);
	CHECK(rb.count == EVERLIVE_V4L2_MAX_BUFS);
	CHECK(h->req_count == EVERLIVE_V4L2_MAX_BUFS);
	rb.count = 0;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_REQBUFS, &rb) == 0);
	CHECK(h->req_count == 0);
	CHECK(everlive_v4l2_mmap_buffer(fd, test_card.mmap_base) == NULL);

	/* Controls are unsupported, unknown ioctls are ENOTTY. */
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_G_CTRL, NULL) == -1);
	CHECK(errno == EINVAL);
	CHECK(everlive_v4l2_ioctl(h, 0xdeadbeefUL, NULL) == -1);
	CHECK(errno == ENOTTY);

	/* Close and duplicate handling. */
	{
		int dupfd = dup(fd);

		CHECK(dupfd >= 0);
		CHECK(everlive_v4l2_dup_virtual(fd, dupfd) == 0);
		CHECK(everlive_v4l2_find_handle(dupfd) != NULL);
		CHECK(everlive_v4l2_close_virtual(dupfd) == 0);
	}
	CHECK(everlive_v4l2_close_virtual(fd) == 0);
	CHECK(everlive_v4l2_find_handle(fd) == NULL);
	CHECK(everlive_v4l2_open_count() == 0);

	free(frame);
	free(readback);
}

static void test_absent_store(void)
{
	struct everlive_v4l2_handle *h;
	struct v4l2_format fmt;
	uint8_t *frame;
	const size_t frame_bytes = 640U * 360U * 2U;
	int fd;

	/* The previous test unlinks the store on cleanup. */
	cleanup();
	CHECK(everlive_v4l2_state_init(everlive_v4l2_card_descriptor()) == 0);
	fd = everlive_v4l2_open_virtual(0);
	CHECK(fd >= 0);
	h = everlive_v4l2_find_handle(fd);
	CHECK(h != NULL);
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 640;
	fmt.fmt.pix.height = 360;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	CHECK(everlive_v4l2_ioctl(h, VIDIOC_S_FMT, &fmt) == 0);

	frame = malloc(frame_bytes);
	CHECK(frame != NULL);
	memset(frame, 0xaa, frame_bytes);
	CHECK(everlive_v4l2_read_frame(fd, frame, frame_bytes) ==
	      (ssize_t)frame_bytes);
	for (size_t i = 0; i < frame_bytes; i++)
		CHECK(frame[i] == 0);

	free(frame);
	CHECK(everlive_v4l2_close_virtual(fd) == 0);
}

int main(void)
{
	printf("=== Running V4L2 Shim State Tests ===\n");
	test_state_machine();
	test_absent_store();
	cleanup();
	printf("=== All V4L2 Shim State Tests Passed! ===\n");
	return 0;
}

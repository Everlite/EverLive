// SPDX-License-Identifier: GPL-2.0-only
/*
 * hws_v4l2out.c — v4l2loopback writer for the HWS UHD engine.
 *
 * The native geometry is negotiated with VIDIOC_S_FMT.  When a reader (OBS)
 * already holds a fixed format, the writer adopts it and scales every frame
 * into it (nearest neighbour on YUYV macropixels), so the picture appears at
 * once; the native mode is retried every 2 s.
 */

#include "hws_v4l2out.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000;
}

static int set_fmt(int fd, unsigned int w, unsigned int h, unsigned int *ow, unsigned int *oh)
{
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };

	f.fmt.pix.width = w;
	f.fmt.pix.height = h;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.bytesperline = w * 2;
	f.fmt.pix.sizeimage = w * h * 2;
	f.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
	f.fmt.pix.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) {
		/* A reader holds the format: adopt whatever is configured. */
		memset(&f, 0, sizeof(f));
		f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		if (ioctl(fd, VIDIOC_G_FMT, &f) < 0)
			return -errno;
	}
	if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV || !f.fmt.pix.width || !f.fmt.pix.height)
		return -EINVAL;
	*ow = f.fmt.pix.width;
	*oh = f.fmt.pix.height;
	return 0;
}

int hws_v4l2out_open(struct hws_v4l2out *o, const char *path)
{
	memset(o, 0, sizeof(*o));
	o->fd = -1;
	snprintf(o->path, sizeof(o->path), "%s", path);
	o->fd = open(path, O_RDWR | O_NONBLOCK);
	if (o->fd < 0) {
		int err = errno;

		fprintf(stderr, "[HWS-USER][WARN] cannot open %s: %s\n", path, strerror(err));
		return -err;
	}
	return 0;
}

void hws_v4l2out_close(struct hws_v4l2out *o)
{
	if (o->fd >= 0)
		close(o->fd);
	o->fd = -1;
	free(o->scaled);
	o->scaled = NULL;
	o->scaled_size = 0;
	o->configured = false;
}

/* Nearest-neighbour YUYV scale, two pixels (one macropixel) at a time. */
static void scale_yuyv(const uint8_t *src, unsigned int sw, unsigned int sh,
		       uint8_t *dst, unsigned int dw, unsigned int dh)
{
	unsigned int smp = sw / 2, dmp = dw / 2;

	for (unsigned int y = 0; y < dh; y++) {
		const uint32_t *srow = (const uint32_t *)(src + (size_t)(y * sh / dh) * sw * 2);
		uint32_t *drow = (uint32_t *)(dst + (size_t)y * dw * 2);

		for (unsigned int x = 0; x < dmp; x++)
			drow[x] = srow[x * smp / dmp];
	}
}

int hws_v4l2out_write(struct hws_v4l2out *o, const uint8_t *frame, unsigned int w, unsigned int h)
{
	const uint8_t *out = frame;
	size_t bytes;
	ssize_t n;

	if (o->fd < 0 || !frame || !w || !h)
		return -EINVAL;
	/* Renegotiate on a new source size, and retry native every 2 s while scaling. */
	if (!o->configured || o->src_w != w || o->src_h != h ||
	    ((o->out_w != w || o->out_h != h) && now_ms() - o->last_try_ms > 2000)) {
		unsigned int ow = 0, oh = 0;

		o->last_try_ms = now_ms();
		if (set_fmt(o->fd, w, h, &ow, &oh) < 0) {
			o->configured = false;
			o->dropped++;
			return -EIO;
		}
		if (!o->configured || ow != o->out_w || oh != o->out_h) {
			printf("[HWS-USER] V4L2 %s: %ux%u%s\n", o->path, ow, oh,
			       (ow != w || oh != h) ? " (reader's format, scaling)" : "");
			fflush(stdout);
		}
		o->configured = true;
		o->src_w = w;
		o->src_h = h;
		o->out_w = ow & ~1u;
		o->out_h = oh;
	}
	bytes = (size_t)o->out_w * o->out_h * 2;
	if (o->out_w != w || o->out_h != h) {
		if (o->scaled_size < bytes) {
			uint8_t *p = realloc(o->scaled, bytes);

			if (!p)
				return -ENOMEM;
			o->scaled = p;
			o->scaled_size = bytes;
		}
		scale_yuyv(frame, w, h, o->scaled, o->out_w, o->out_h);
		out = o->scaled;
	}
	n = write(o->fd, out, bytes);
	if (n != (ssize_t)bytes) {
		o->dropped++;
		if (n < 0 && errno != EAGAIN)
			o->configured = false;
		return -EIO;
	}
	o->frames++;
	return 0;
}

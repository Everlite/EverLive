// SPDX-License-Identifier: GPL-2.0-only
/*
 * everlive_v4l2_state.c — Reusable V4L2 virtual-camera state machine
 *
 * The per-card preload DSO supplies the libc/libv4l2 interposition and holds
 * its global lock around every call into this module.  Buffer mappings,
 * timerfd closure and frame-store mapping use raw syscalls so they cannot
 * recurse through the interposed symbols while that lock is held.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "everlive_v4l2_state.h"

#include "everlive_framestore.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static struct everlive_v4l2_handle g_handles[EVERLIVE_V4L2_MAX_FDS];
static _Atomic int g_open_handle_count;
static const struct everlive_v4l2_card *g_card;
static struct everlive_framestore *g_framestore;

static const struct everlive_v4l2_card *state_card(void)
{
	return g_card ? g_card : everlive_v4l2_card_descriptor();
}

static void *state_mmap(size_t length)
{
#if defined(SYS_mmap)
	return (void *)syscall(SYS_mmap, NULL, length, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#elif defined(SYS_mmap2)
	return (void *)syscall(SYS_mmap2, NULL, length, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
#error "everlive_v4l2_state requires SYS_mmap or SYS_mmap2"
#endif
}

static int state_munmap(void *addr, size_t length)
{
	return (int)syscall(SYS_munmap, addr, length);
}

static int state_close(int fd)
{
	return (int)syscall(SYS_close, fd);
}

static struct everlive_framestore *get_framestore(void)
{
	const struct everlive_v4l2_card *card;

	if (g_framestore)
		return g_framestore;

	card = state_card();
	if (!card || !card->shm_name)
		return NULL;

	g_framestore = everlive_framestore_attach(card->shm_name);
	return g_framestore;
}

static uint32_t calc_bytesperline(uint32_t w, uint32_t pixfmt)
{
	if (pixfmt == V4L2_PIX_FMT_YUYV || pixfmt == V4L2_PIX_FMT_UYVY)
		return w * 2;
	if (pixfmt == V4L2_PIX_FMT_NV12 || pixfmt == V4L2_PIX_FMT_YUV420)
		return w;
	if (pixfmt == V4L2_PIX_FMT_RGB24 || pixfmt == V4L2_PIX_FMT_BGR24)
		return w * 3;
	return w * 4;
}

static uint32_t calc_sizeimage(uint32_t w, uint32_t h, uint32_t pixfmt)
{
	if (pixfmt == V4L2_PIX_FMT_YUYV || pixfmt == V4L2_PIX_FMT_UYVY)
		return w * h * 2;
	if (pixfmt == V4L2_PIX_FMT_NV12 || pixfmt == V4L2_PIX_FMT_YUV420)
		return w * h * 3 / 2;
	if (pixfmt == V4L2_PIX_FMT_RGB24 || pixfmt == V4L2_PIX_FMT_BGR24)
		return w * h * 3;
	return w * h * 4;
}

static inline uint8_t clamp_byte(int v)
{
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static inline void yuv_to_rgb(int y, int u, int v, uint8_t *r, uint8_t *g,
			      uint8_t *b)
{
	int c = y - 16;
	int d = u - 128;
	int e = v - 128;

	if (c < 0)
		c = 0;
	*r = clamp_byte((298 * c + 459 * e + 128) >> 8);
	*g = clamp_byte((298 * c - 55 * d - 136 * e + 128) >> 8);
	*b = clamp_byte((298 * c + 541 * d + 128) >> 8);
}

static void convert_yuy2_frame(const uint8_t *src, uint32_t src_w,
			       uint32_t src_h, uint8_t *dst, size_t dst_max,
			       uint32_t dst_w, uint32_t dst_h,
			       uint32_t pixfmt)
{
	const struct everlive_v4l2_card *card = state_card();
	uint32_t max_w = card && card->max_width >= 2 ? card->max_width : 1920U;
	uint32_t max_h = card && card->max_height >= 2 ? card->max_height : 1080U;
	size_t needed;

	if (src_w < 2 || src_w > max_w)
		src_w = max_w;
	if (src_h < 2 || src_h > max_h)
		src_h = max_h;
	src_w &= ~1U; /* YUV 4:2:2 requires an even width */
	src_h &= ~1U;
	if (dst_w & 1U)
		dst_w &= ~1U;
	if (dst_h & 1U)
		dst_h &= ~1U;

	needed = calc_sizeimage(dst_w, dst_h, pixfmt);
	if (!dst || dst_max < needed)
		return;
	if (!src) {
		memset(dst, 0, needed);
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_YUYV) {
		if (dst_w == src_w && dst_h == src_h) {
			memcpy(dst, src, needed);
			return;
		}
		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 2;

			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;

				memcpy(drow + x * 2, srow + sx * 2, 4);
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_UYVY) {
		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 2;

			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;

				drow[x * 2 + 0] = p[1];
				drow[x * 2 + 1] = p[0];
				drow[x * 2 + 2] = p[3];
				drow[x * 2 + 3] = p[2];
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_YUV420 || pixfmt == V4L2_PIX_FMT_NV12) {
		size_t y_size = (size_t)dst_w * dst_h;
		uint8_t *y_plane = dst;
		uint8_t *u_plane = dst + y_size;
		uint8_t *v_plane = dst + y_size + y_size / 4;

		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *yd = y_plane + (size_t)y * dst_w;
			bool even_y = (y & 1) == 0;

			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;

				yd[x] = p[0];
				yd[x + 1] = p[2];
				if (even_y) {
					if (pixfmt == V4L2_PIX_FMT_NV12) {
						uint8_t *uv = u_plane +
							(size_t)(y / 2) * dst_w + x;

						uv[0] = p[1];
						uv[1] = p[3];
					} else {
						u_plane[(y / 2) * (dst_w / 2) +
							(x / 2)] = p[1];
						v_plane[(y / 2) * (dst_w / 2) +
							(x / 2)] = p[3];
					}
				}
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_RGB24 || pixfmt == V4L2_PIX_FMT_BGR24) {
		bool bgr = (pixfmt == V4L2_PIX_FMT_BGR24);

		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 3;

			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;
				uint8_t r0, g0, b0, r1, g1, b1;

				yuv_to_rgb(p[0], p[1], p[3], &r0, &g0, &b0);
				yuv_to_rgb(p[2], p[1], p[3], &r1, &g1, &b1);
				if (bgr) {
					drow[x * 3 + 0] = b0;
					drow[x * 3 + 1] = g0;
					drow[x * 3 + 2] = r0;
					drow[(x + 1) * 3 + 0] = b1;
					drow[(x + 1) * 3 + 1] = g1;
					drow[(x + 1) * 3 + 2] = r1;
				} else {
					drow[x * 3 + 0] = r0;
					drow[x * 3 + 1] = g0;
					drow[x * 3 + 2] = b0;
					drow[(x + 1) * 3 + 0] = r1;
					drow[(x + 1) * 3 + 1] = g1;
					drow[(x + 1) * 3 + 2] = b1;
				}
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_XBGR32 || pixfmt == V4L2_PIX_FMT_BGR32) {
		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 4;

			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;
				uint8_t r0, g0, b0, r1, g1, b1;

				yuv_to_rgb(p[0], p[1], p[3], &r0, &g0, &b0);
				yuv_to_rgb(p[2], p[1], p[3], &r1, &g1, &b1);
				drow[x * 4 + 0] = b0;
				drow[x * 4 + 1] = g0;
				drow[x * 4 + 2] = r0;
				drow[x * 4 + 3] = 255;
				drow[(x + 1) * 4 + 0] = b1;
				drow[(x + 1) * 4 + 1] = g1;
				drow[(x + 1) * 4 + 2] = r1;
				drow[(x + 1) * 4 + 3] = 255;
			}
		}
	}
}

/*
 * Snapshot and convert one frame-store port.  read_begin()/read_validate()
 * accept a frame only when the producer has not replaced it during the
 * conversion, so a slow or lapping writer produces a black frame rather than
 * a torn one.
 */
static void shm_convert_port(unsigned int port, uint8_t *dst, size_t dst_max,
			     uint32_t dst_w, uint32_t dst_h, uint32_t pixfmt)
{
	struct everlive_framestore *fs = get_framestore();
	int tries;

	if (!fs || !everlive_framestore_alive(fs, 1000000000ULL)) {
		convert_yuy2_frame(NULL, 0, 0, dst, dst_max,
				   dst_w, dst_h, pixfmt);
		return;
	}

	for (tries = 0; tries < 8; tries++) {
		struct everlive_framestore_frame frame;

		if (!everlive_framestore_read_begin(fs, port, &frame)) {
			usleep(100);
			continue;
		}

		convert_yuy2_frame(frame.data, frame.width, frame.height,
				   dst, dst_max, dst_w, dst_h, pixfmt);

		if (everlive_framestore_read_validate(fs, port, &frame))
			return;

		usleep(100);
	}

	convert_yuy2_frame(NULL, 0, 0, dst, dst_max, dst_w, dst_h, pixfmt);
}

static void clamp_format(const struct everlive_v4l2_card *card,
			 uint32_t *width, uint32_t *height)
{
	uint32_t w = *width;
	uint32_t h = *height;
	uint32_t out_w = 1920;
	uint32_t out_h = 1080;

	if (card && card->default_width && card->default_height) {
		out_w = card->default_width;
		out_h = card->default_height;
	}

	if (card && card->clamps) {
		for (size_t i = 0; i < card->clamp_count; i++) {
			const struct everlive_v4l2_clamp *clamp = &card->clamps[i];

			out_w = clamp->width;
			out_h = clamp->height;
			if (w <= clamp->request_width &&
			    h <= clamp->request_height)
				break;
		}
	}

	*width = out_w & ~1U;
	*height = out_h & ~1U;
}

static struct everlive_v4l2_handle *find_handle(int fd)
{
	if (fd < 0 ||
	    atomic_load_explicit(&g_open_handle_count, memory_order_relaxed) <= 0)
		return NULL;

	for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_FDS; i++) {
		if (g_handles[i].in_use && g_handles[i].fd == fd)
			return &g_handles[i];
	}
	return NULL;
}

struct everlive_v4l2_handle *everlive_v4l2_find_handle(int fd)
{
	return find_handle(fd);
}

bool everlive_v4l2_is_virtual_fd(int fd)
{
	return find_handle(fd) != NULL;
}

int everlive_v4l2_open_count(void)
{
	return atomic_load_explicit(&g_open_handle_count, memory_order_relaxed);
}

static int handle_virtual_ioctl(struct everlive_v4l2_handle *h,
				unsigned long request, void *arg)
{
	const struct everlive_v4l2_card *card = state_card();
	int ret = 0;

	switch (request) {
	case VIDIOC_QUERYCAP: {
		struct v4l2_capability *cap = arg;
		const char *bdf = NULL;
		const char *suffix = "";
		const char *name = NULL;
		struct everlive_framestore *fs;

		if (!cap) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		fs = get_framestore();
		if (fs)
			bdf = everlive_framestore_bdf(fs);
		if ((!bdf || !*bdf) && card && card->fallback_bdf)
			bdf = card->fallback_bdf;
		if (!bdf)
			bdf = "";
		if (card && h->port < EVERLIVE_V4L2_MAX_PORTS &&
		    card->bus_suffixes[h->port])
			suffix = card->bus_suffixes[h->port];
		if (card && h->port < EVERLIVE_V4L2_MAX_PORTS &&
		    card->port_names[h->port])
			name = card->port_names[h->port];
		if (!name && card)
			name = card->card_name;

		memset(cap, 0, sizeof(*cap));
		snprintf((char *)cap->driver, sizeof(cap->driver), "%s",
			 card && card->driver ? card->driver : "everlive-v4l2");
		snprintf((char *)cap->card, sizeof(cap->card), "%s",
			 name ? name : "EverLive virtual camera");
		snprintf((char *)cap->bus_info, sizeof(cap->bus_info),
			 "PCI:%s%s", bdf, suffix);
		cap->version = (6 << 16) | (12 << 8) | 0;
		cap->device_caps = V4L2_CAP_VIDEO_CAPTURE |
				   V4L2_CAP_STREAMING |
				   V4L2_CAP_READWRITE;
		cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
		break;
	}
	case VIDIOC_ENUM_FMT: {
		static const uint32_t fmts[] = {
			V4L2_PIX_FMT_YUYV,
			V4L2_PIX_FMT_UYVY,
			V4L2_PIX_FMT_BGR24,
			V4L2_PIX_FMT_XBGR32,
			V4L2_PIX_FMT_NV12,
			V4L2_PIX_FMT_YUV420,
			V4L2_PIX_FMT_RGB24,
		};
		static const char *names[] = {
			"YUYV 4:2:2",
			"UYVY 4:2:2",
			"24-bit BGR",
			"32-bit BGRx",
			"Y/UV 4:2:0 (NV12)",
			"Planar YUV 4:2:0 (I420)",
			"24-bit RGB",
		};
		struct v4l2_fmtdesc *f = arg;

		if (!f || f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
		    f->index >= sizeof(fmts) / sizeof(fmts[0])) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		f->flags = 0;
		f->pixelformat = fmts[f->index];
		snprintf((char *)f->description, sizeof(f->description), "%s",
			 names[f->index]);
		break;
	}
	case VIDIOC_G_FMT: {
		struct v4l2_format *f = arg;

		if (!f || f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		memset(&f->fmt.pix, 0, sizeof(f->fmt.pix));
		f->fmt.pix.width = h->width;
		f->fmt.pix.height = h->height;
		f->fmt.pix.pixelformat = h->pixfmt;
		f->fmt.pix.field = V4L2_FIELD_NONE;
		f->fmt.pix.bytesperline = calc_bytesperline(h->width, h->pixfmt);
		f->fmt.pix.sizeimage = calc_sizeimage(h->width, h->height,
						     h->pixfmt);
		f->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
		f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_709;
		f->fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
		f->fmt.pix.xfer_func = V4L2_XFER_FUNC_709;
		break;
	}
	case VIDIOC_TRY_FMT:
	case VIDIOC_S_FMT: {
		struct v4l2_format *f = arg;
		uint32_t w;
		uint32_t ht;
		uint32_t pf;

		if (!f || f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		w = f->fmt.pix.width;
		ht = f->fmt.pix.height;
		clamp_format(card, &w, &ht);

		pf = f->fmt.pix.pixelformat;
		if (pf != V4L2_PIX_FMT_YUYV && pf != V4L2_PIX_FMT_UYVY &&
		    pf != V4L2_PIX_FMT_NV12 && pf != V4L2_PIX_FMT_YUV420 &&
		    pf != V4L2_PIX_FMT_RGB24 && pf != V4L2_PIX_FMT_BGR24 &&
		    pf != V4L2_PIX_FMT_XBGR32 && pf != V4L2_PIX_FMT_BGR32)
			pf = V4L2_PIX_FMT_YUYV;

		f->fmt.pix.width = w;
		f->fmt.pix.height = ht;
		f->fmt.pix.pixelformat = pf;
		f->fmt.pix.field = V4L2_FIELD_NONE;
		f->fmt.pix.bytesperline = calc_bytesperline(w, pf);
		f->fmt.pix.sizeimage = calc_sizeimage(w, ht, pf);
		f->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
		f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_709;
		f->fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
		f->fmt.pix.xfer_func = V4L2_XFER_FUNC_709;
		if (request == VIDIOC_S_FMT) {
			if (h->req_count > 0 &&
			    (w != h->width || ht != h->height ||
			     pf != h->pixfmt)) {
				errno = EBUSY;
				ret = -1;
				break;
			}
			h->width = w;
			h->height = ht;
			h->pixfmt = pf;
		}
		break;
	}
	case VIDIOC_ENUM_FRAMESIZES: {
		struct v4l2_frmsizeenum *fs = arg;

		if (!fs || !card || !card->sizes ||
		    fs->index >= card->size_count) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fs->discrete.width = card->sizes[fs->index].width;
		fs->discrete.height = card->sizes[fs->index].height;
		break;
	}
	case VIDIOC_ENUM_FRAMEINTERVALS: {
		struct v4l2_frmivalenum *fi = arg;

		if (!fi || !card || !card->fps_denominators ||
		    fi->index >= card->fps_count) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		fi->discrete.numerator = 1;
		fi->discrete.denominator =
			card->fps_denominators[fi->index];
		break;
	}
	case VIDIOC_ENUMINPUT: {
		struct v4l2_input *inp = arg;
		uint32_t idx;

		if (!inp || inp->index > 0) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		idx = inp->index;
		memset(inp, 0, sizeof(*inp));
		inp->index = idx;
		snprintf((char *)inp->name, sizeof(inp->name), "HDMI IN %u",
			 h->port + 1U);
		inp->type = V4L2_INPUT_TYPE_CAMERA;
		break;
	}
	case VIDIOC_G_INPUT: {
		int *i = arg;

		if (!i) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		*i = 0;
		break;
	}
	case VIDIOC_S_INPUT: {
		int *i = arg;

		if (!i || *i != 0) {
			errno = EINVAL;
			ret = -1;
		}
		break;
	}
	case VIDIOC_G_PARM:
	case VIDIOC_S_PARM: {
		struct v4l2_streamparm *sp = arg;

		if (!sp || sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		if (request == VIDIOC_S_PARM &&
		    sp->parm.capture.timeperframe.numerator > 0 &&
		    sp->parm.capture.timeperframe.denominator > 0) {
			h->fps_num = sp->parm.capture.timeperframe.numerator;
			h->fps_den = sp->parm.capture.timeperframe.denominator;
		}
		memset(&sp->parm.capture, 0, sizeof(sp->parm.capture));
		sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
		sp->parm.capture.timeperframe.numerator = h->fps_num;
		sp->parm.capture.timeperframe.denominator = h->fps_den;
		sp->parm.capture.readbuffers = 4;
		break;
	}
	case VIDIOC_REQBUFS: {
		struct v4l2_requestbuffers *rb = arg;
		size_t sz;

		if (!rb || rb->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
		    rb->memory != V4L2_MEMORY_MMAP) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_BUFS; i++) {
			if (h->bufs[i].addr && h->bufs[i].size)
				state_munmap(h->bufs[i].addr, h->bufs[i].size);
			memset(&h->bufs[i], 0, sizeof(h->bufs[i]));
		}
		if (rb->count == 0) {
			h->req_count = 0;
			break;
		}
		if (rb->count < 2)
			rb->count = 2;
		if (rb->count > EVERLIVE_V4L2_MAX_BUFS)
			rb->count = EVERLIVE_V4L2_MAX_BUFS;
		sz = calc_sizeimage(h->width, h->height, h->pixfmt);
		for (uint32_t i = 0; i < rb->count; i++) {
			void *p = state_mmap(sz);

			if (p == MAP_FAILED) {
				errno = ENOMEM;
				ret = -1;
				break;
			}
			h->bufs[i].addr = p;
			h->bufs[i].size = sz;
		}
		h->req_count = rb->count;
		break;
	}
	case VIDIOC_QUERYBUF: {
		struct v4l2_buffer *b = arg;

		if (!b || b->index >= h->req_count) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		b->bytesused = h->bufs[b->index].size;
		b->length = h->bufs[b->index].size;
		b->field = V4L2_FIELD_NONE;
		b->memory = V4L2_MEMORY_MMAP;
		b->m.offset = card->mmap_base +
			      b->index * card->mmap_stride;
		b->flags = V4L2_BUF_FLAG_MAPPED |
			   V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
			   (h->bufs[b->index].queued ?
			    V4L2_BUF_FLAG_QUEUED : 0) |
			   (h->bufs[b->index].done ?
			    V4L2_BUF_FLAG_DONE : 0);
		break;
	}
	case VIDIOC_QBUF: {
		struct v4l2_buffer *b = arg;

		if (!b || b->index >= h->req_count) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		h->bufs[b->index].queued = true;
		h->bufs[b->index].done = false;
		b->flags = V4L2_BUF_FLAG_MAPPED |
			   V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
			   V4L2_BUF_FLAG_QUEUED;
		break;
	}
	case VIDIOC_DQBUF: {
		struct v4l2_buffer *b = arg;
		struct pollfd pfd;
		int found = -1;
		int pr;
		uint64_t expirations;

		if (!b) {
			errno = EINVAL;
			ret = -1;
			break;
		}
		for (uint32_t i = 0; i < h->req_count; i++) {
			if (h->bufs[i].queued) {
				found = (int)i;
				break;
			}
		}
		if (found < 0 || !h->streaming) {
			errno = EAGAIN;
			ret = -1;
			break;
		}
		pfd.fd = h->fd;
		pfd.events = POLLIN;
		pr = poll(&pfd, 1, 1000);
		if (pr <= 0) {
			if (pr == 0)
				errno = EAGAIN;
			ret = -1;
			break;
		}
		while (read(h->fd, &expirations, sizeof(expirations)) ==
		       sizeof(expirations))
			;

		shm_convert_port(h->port, h->bufs[found].addr,
				 h->bufs[found].size,
				 h->width, h->height, h->pixfmt);

		h->bufs[found].queued = false;
		h->bufs[found].done = true;
		b->index = found;
		b->bytesused = h->bufs[found].size;
		b->length = h->bufs[found].size;
		b->field = V4L2_FIELD_NONE;
		b->sequence = h->sequence++;
		b->memory = V4L2_MEMORY_MMAP;
		b->m.offset = card->mmap_base + found * card->mmap_stride;
		b->flags = V4L2_BUF_FLAG_MAPPED |
			   V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		{
			struct timespec ts;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			b->timestamp.tv_sec = ts.tv_sec;
			b->timestamp.tv_usec = ts.tv_nsec / 1000;
		}
		break;
	}
	case VIDIOC_STREAMON: {
		uint64_t interval_ns;
		struct itimerspec its;

		h->streaming = true;
		interval_ns = (1000000000ULL * h->fps_num) /
			      (h->fps_den ? h->fps_den : 60);
		if (interval_ns < 4000000ULL)
			interval_ns = 16666667ULL;
		memset(&its, 0, sizeof(its));
		its.it_interval.tv_sec = (time_t)(interval_ns / 1000000000ULL);
		its.it_interval.tv_nsec = (long)(interval_ns % 1000000000ULL);
		its.it_value.tv_nsec = 1000000L;
		timerfd_settime(h->fd, 0, &its, NULL);
		break;
	}
	case VIDIOC_STREAMOFF: {
		struct itimerspec its;

		h->streaming = false;
		for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_BUFS; i++) {
			h->bufs[i].queued = false;
			h->bufs[i].done = false;
		}
		memset(&its, 0, sizeof(its));
		timerfd_settime(h->fd, 0, &its, NULL);
		break;
	}
	case VIDIOC_QUERYCTRL:
	case VIDIOC_QUERY_EXT_CTRL:
	case VIDIOC_QUERYMENU:
	case VIDIOC_G_CTRL:
	case VIDIOC_S_CTRL:
	case VIDIOC_G_EXT_CTRLS:
	case VIDIOC_S_EXT_CTRLS:
	case VIDIOC_TRY_EXT_CTRLS:
		errno = EINVAL;
		ret = -1;
		break;
	default:
		errno = ENOTTY;
		ret = -1;
		break;
	}

	return ret;
}

int everlive_v4l2_open_virtual(unsigned int port)
{
	const struct everlive_v4l2_card *card = state_card();
	struct everlive_v4l2_handle *h = NULL;
	int slot = -1;
	int tfd;

	if (!card || port >= card->port_count) {
		errno = EINVAL;
		return -1;
	}

	for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_FDS; i++) {
		if (!g_handles[i].in_use) {
			slot = (int)i;
			break;
		}
	}
	if (slot < 0) {
		errno = EMFILE;
		return -1;
	}

	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (tfd < 0)
		return -1;

	h = &g_handles[slot];
	memset(h, 0, sizeof(*h));
	h->in_use = true;
	h->fd = tfd;
	h->port = port;
	h->width = card->default_width;
	h->height = card->default_height;
	h->pixfmt = V4L2_PIX_FMT_YUYV;
	h->fps_num = 1;
	h->fps_den = 60;
	atomic_fetch_add_explicit(&g_open_handle_count, 1,
				  memory_order_relaxed);
	return tfd;
}

int everlive_v4l2_close_virtual(int fd)
{
	struct everlive_v4l2_handle *h = find_handle(fd);
	int ret;

	if (!h) {
		errno = EBADF;
		return -1;
	}

	for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_BUFS; i++) {
		if (h->bufs[i].addr && h->bufs[i].size)
			state_munmap(h->bufs[i].addr, h->bufs[i].size);
	}
	ret = state_close(fd);
	memset(h, 0, sizeof(*h));
	atomic_fetch_sub_explicit(&g_open_handle_count, 1,
				  memory_order_relaxed);
	return ret;
}

int everlive_v4l2_dup_virtual(int oldfd, int newfd)
{
	struct everlive_v4l2_handle *old = find_handle(oldfd);
	int slot = -1;

	if (!old)
		return -1;

	for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_FDS; i++) {
		if (!g_handles[i].in_use) {
			slot = (int)i;
			break;
		}
	}
	if (slot < 0)
		return -1;

	g_handles[slot] = *old;
	g_handles[slot].fd = newfd;
	memset(g_handles[slot].bufs, 0, sizeof(g_handles[slot].bufs));
	g_handles[slot].req_count = 0;
	atomic_fetch_add_explicit(&g_open_handle_count, 1,
				  memory_order_relaxed);
	return 0;
}

int everlive_v4l2_ioctl(struct everlive_v4l2_handle *h,
			unsigned long request, void *arg)
{
	if (!h) {
		errno = EBADF;
		return -1;
	}
	return handle_virtual_ioctl(h, request, arg);
}

void *everlive_v4l2_mmap_buffer(int fd, uint64_t offset)
{
	const struct everlive_v4l2_card *card = state_card();
	struct everlive_v4l2_handle *h = find_handle(fd);
	uint64_t delta;
	unsigned int idx;

	if (!h || !card || offset < card->mmap_base || !card->mmap_stride)
		return NULL;

	delta = offset - card->mmap_base;
	idx = (unsigned int)(delta / card->mmap_stride);
	if (idx < h->req_count && h->bufs[idx].addr)
		return h->bufs[idx].addr;
	return NULL;
}

bool everlive_v4l2_munmap_buffer(void *addr)
{
	for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_FDS; i++) {
		if (!g_handles[i].in_use)
			continue;
		for (unsigned int j = 0; j < EVERLIVE_V4L2_MAX_BUFS; j++) {
			if (g_handles[i].bufs[j].addr == addr)
				return true;
		}
	}
	return false;
}

ssize_t everlive_v4l2_read_frame(int fd, void *buffer, size_t n)
{
	struct everlive_v4l2_handle *h = find_handle(fd);
	size_t sz;

	if (!h) {
		errno = EBADF;
		return -1;
	}

	sz = calc_sizeimage(h->width, h->height, h->pixfmt);
	if (n < sz) {
		errno = EINVAL;
		return -1;
	}

	shm_convert_port(h->port, buffer, n, h->width, h->height, h->pixfmt);
	return (ssize_t)sz;
}

void everlive_v4l2_state_shutdown(void)
{
	for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_FDS; i++) {
		if (g_handles[i].in_use)
			everlive_v4l2_close_virtual(g_handles[i].fd);
	}
	atomic_store_explicit(&g_open_handle_count, 0, memory_order_relaxed);
	if (g_framestore) {
		everlive_framestore_destroy(g_framestore);
		g_framestore = NULL;
	}
	g_card = NULL;
}

int everlive_v4l2_state_init(const struct everlive_v4l2_card *card)
{
	if (!card)
		return -EINVAL;

	everlive_v4l2_state_shutdown();
	memset(g_handles, 0, sizeof(g_handles));
	atomic_store_explicit(&g_open_handle_count, 0, memory_order_relaxed);
	g_card = card;
	return 0;
}

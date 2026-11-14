// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc575_v4l2.c — UVC/V4L2 capture helpers for the GC575
 *
 * The GC575 capture node is a standard uvcvideo node.  This file performs
 * device discovery, format/interval negotiation, MMAP streaming and the
 * YUY2 conversion required by the EverLive framestore.  MJPEG decoding is
 * optional and is compiled in when libavcodec is available.
 */

#include "gc575_v4l2.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef HAVE_LIBAVCODEC
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#endif

#define GC575_USB_VENDOR  "07ca"
#define GC575_USB_PRODUCT "0575"

static int read_sysfs_id(const char *dir, const char *name,
			 char *out, size_t out_size)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "r");
	if (!f)
		return -errno;
	if (!fgets(out, (int)out_size, f)) {
		fclose(f);
		return -EIO;
	}
	fclose(f);
	out[strcspn(out, "\r\n")] = '\0';
	return 0;
}

static bool usb_ids_match(const char *dir, const char *vendor, const char *product)
{
	char v[16] = { 0 };
	char p[16] = { 0 };

	if (read_sysfs_id(dir, "idVendor", v, sizeof(v)) < 0 ||
	    read_sysfs_id(dir, "idProduct", p, sizeof(p)) < 0)
		return false;

	return strcasecmp(v, vendor) == 0 && strcasecmp(p, product) == 0;
}

/* The sysfs "device" link points at the USB interface; walk up to the device. */
static bool sysfs_node_is_gc575(const char *video_sysfs)
{
	char link[PATH_MAX];
	char resolved[PATH_MAX];
	char parent[PATH_MAX + 4];

	snprintf(link, sizeof(link), "%s/device", video_sysfs);
	if (!realpath(link, resolved))
		return false;

	/* resolved is usually .../1-2/1-2:1.0 */
	for (int i = 0; i < 4; i++) {
		if (usb_ids_match(resolved, GC575_USB_VENDOR, GC575_USB_PRODUCT))
			return true;
		snprintf(parent, sizeof(parent), "%s/..", resolved);
		if (!realpath(parent, resolved))
			break;
	}

	return false;
}

static bool node_has_video_capture(const char *dev)
{
	struct v4l2_capability cap;
	int fd = open(dev, O_RDWR | O_NONBLOCK);
	uint32_t caps;

	if (fd < 0)
		return false;
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		close(fd);
		return false;
	}
	close(fd);

	caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ?
	       cap.device_caps : cap.capabilities;
	return (caps & V4L2_CAP_VIDEO_CAPTURE) &&
	       (caps & V4L2_CAP_STREAMING);
}

int gc575_find_video_node(char *out, size_t out_size)
{
	glob_t g;
	int ret = -ENODEV;

	if (!out || out_size == 0)
		return -EINVAL;
	out[0] = '\0';

	if (glob("/sys/class/video4linux/video*", 0, NULL, &g) != 0)
		return -ENODEV;

	for (size_t i = 0; i < g.gl_pathc; i++) {
		if (!sysfs_node_is_gc575(g.gl_pathv[i]))
			continue;

		char dev[64];
		snprintf(dev, sizeof(dev), "/dev/%s",
			 strrchr(g.gl_pathv[i], '/') + 1);
		if (!node_has_video_capture(dev))
			continue;

		snprintf(out, out_size, "%s", dev);
		ret = 0;
		break;
	}

	globfree(&g);
	return ret;
}

#ifdef HAVE_LIBAVCODEC
struct gc575_mjpeg_decoder {
	AVCodecContext *ctx;
	SwsContext *sws;
};

static void *mjpeg_decoder_new(void)
{
	struct gc575_mjpeg_decoder *dec;
	const AVCodec *codec;

	dec = calloc(1, sizeof(*dec));
	if (!dec)
		return NULL;

	codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
	if (!codec) {
		free(dec);
		return NULL;
	}

	dec->ctx = avcodec_alloc_context3(codec);
	if (!dec->ctx) {
		free(dec);
		return NULL;
	}
	if (avcodec_open2(dec->ctx, codec, NULL) < 0) {
		avcodec_free_context(&dec->ctx);
		free(dec);
		return NULL;
	}
	return dec;
}

static void mjpeg_decoder_free(void *opaque)
{
	struct gc575_mjpeg_decoder *dec = opaque;

	if (!dec)
		return;
	sws_freeContext(dec->sws);
	avcodec_free_context(&dec->ctx);
	free(dec);
}

static int mjpeg_decode(void *opaque, const uint8_t *src, size_t src_size,
			uint8_t *dst, size_t dst_size, size_t *out_bytes)
{
	struct gc575_mjpeg_decoder *dec = opaque;
	AVPacket *pkt = NULL;
	AVFrame *frame = NULL;
	uint8_t *dst_data[4] = { dst, NULL, NULL, NULL };
	int dst_linesize[4] = { 0, 0, 0, 0 };
	int ret;

	if (!dec || !src || !dst || !out_bytes)
		return -EINVAL;

	pkt = av_packet_alloc();
	frame = av_frame_alloc();
	if (!pkt || !frame) {
		ret = -ENOMEM;
		goto out;
	}

	if (av_new_packet(pkt, (int)src_size) < 0) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(pkt->data, src, src_size);

	ret = avcodec_send_packet(dec->ctx, pkt);
	if (ret < 0)
		goto out;

	ret = avcodec_receive_frame(dec->ctx, frame);
	if (ret == AVERROR(EAGAIN)) {
		ret = -EAGAIN;
		goto out;
	}
	if (ret < 0)
		goto out;

	if ((size_t)frame->width * frame->height * 2 > dst_size) {
		ret = -ENOSPC;
		goto out;
	}

	dec->sws = sws_getCachedContext(
		dec->sws, frame->width, frame->height, frame->format,
		frame->width, frame->height, AV_PIX_FMT_YUYV422,
		SWS_POINT, NULL, NULL, NULL);
	if (!dec->sws) {
		ret = -EIO;
		goto out;
	}

	dst_linesize[0] = frame->width * 2;
	sws_scale(dec->sws, (const uint8_t *const *)frame->data,
		  frame->linesize, 0, frame->height, dst_data, dst_linesize);

	*out_bytes = (size_t)frame->width * frame->height * 2;
	ret = 0;

out:
	av_frame_free(&frame);
	av_packet_free(&pkt);
	return ret;
}
#else
static void *mjpeg_decoder_new(void) { return NULL; }
static void mjpeg_decoder_free(void *opaque) { (void)opaque; }
static int mjpeg_decode(void *opaque, const uint8_t *src, size_t src_size,
			uint8_t *dst, size_t dst_size, size_t *out_bytes)
{
	(void)opaque;
	(void)src;
	(void)src_size;
	(void)dst;
	(void)dst_size;
	(void)out_bytes;
	return -ENOTSUP;
}
#endif

static void set_stream_fps(int fd, unsigned int fps)
{
	struct v4l2_streamparm parm = {};

	if (!fps)
		return;
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_G_PARM, &parm) < 0)
		return;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;
	if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0)
		fprintf(stderr, "[gc575] %u fps not accepted: %s\n",
			fps, strerror(errno));
}

int gc575_capture_open(struct gc575_capture *cap, const char *device,
		       uint32_t want_fmt, unsigned int want_width,
		       unsigned int want_height, unsigned int want_fps)
{
	struct v4l2_capability vcap;
	struct v4l2_format fmt = {};
	struct v4l2_requestbuffers req = {};
	uint32_t caps;
	int fd;

	if (!cap || !device)
		return -EINVAL;
	memset(cap, 0, sizeof(*cap));

	fd = open(device, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	cap->fd = fd;

	if (ioctl(fd, VIDIOC_QUERYCAP, &vcap) < 0) {
		int ret = -errno;
		close(fd);
		cap->fd = -1;
		return ret;
	}
	caps = (vcap.capabilities & V4L2_CAP_DEVICE_CAPS) ?
	       vcap.device_caps : vcap.capabilities;
	if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
		close(fd);
		cap->fd = -1;
		return -ENODEV;
	}

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
		int ret = -errno;
		close(fd);
		cap->fd = -1;
		return ret;
	}

	if (want_fmt || want_width || want_height) {
		if (want_fmt)
			fmt.fmt.pix.pixelformat = want_fmt;
		if (want_width)
			fmt.fmt.pix.width = want_width;
		if (want_height)
			fmt.fmt.pix.height = want_height;
		fmt.fmt.pix.field = V4L2_FIELD_NONE;

		if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
			fprintf(stderr,
				"[gc575] requested mode %ux%u fourcc 0x%08x rejected: %s\n",
				fmt.fmt.pix.width, fmt.fmt.pix.height,
				fmt.fmt.pix.pixelformat, strerror(errno));
			memset(&fmt, 0, sizeof(fmt));
			fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
				int ret = -errno;
				close(fd);
				cap->fd = -1;
				return ret;
			}
		}
	}

	if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
		int ret = -errno;
		close(fd);
		cap->fd = -1;
		return ret;
	}

	cap->width = fmt.fmt.pix.width;
	cap->height = fmt.fmt.pix.height;
	cap->pixelformat = fmt.fmt.pix.pixelformat;
	cap->bytesperline = fmt.fmt.pix.bytesperline ?
			    fmt.fmt.pix.bytesperline : fmt.fmt.pix.width;

	set_stream_fps(fd, want_fps);

	/* Read back the interval the driver actually selected. */
	{
		struct v4l2_streamparm parm = {};
		parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0) {
			unsigned int num = parm.parm.capture.timeperframe.numerator;
			unsigned int den = parm.parm.capture.timeperframe.denominator;
			cap->fps = (num && den) ? (den / num) : 0;
		}
	}

	if (cap->pixelformat == V4L2_PIX_FMT_MJPEG) {
		cap->decoder = mjpeg_decoder_new();
		if (!cap->decoder)
			fprintf(stderr,
				"[gc575] MJPEG selected but libavcodec is unavailable\n");
	}

	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
		int ret = -errno;
		gc575_capture_close(cap);
		return ret ? ret : -ENOMEM;
	}
	if (req.count > GC575_MAX_BUFFERS)
		req.count = GC575_MAX_BUFFERS;
	cap->count = req.count;

	for (unsigned int i = 0; i < cap->count; i++) {
		struct v4l2_buffer buf = {};
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			gc575_capture_close(cap);
			return -errno;
		}
		cap->buffers[i].length = buf.length;
		cap->buffers[i].start = mmap(NULL, buf.length,
					     PROT_READ | PROT_WRITE,
					     MAP_SHARED, fd, buf.m.offset);
		if (cap->buffers[i].start == MAP_FAILED) {
			cap->buffers[i].start = NULL;
			gc575_capture_close(cap);
			return -errno;
		}
	}

	return 0;
}

int gc575_capture_start(struct gc575_capture *cap)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (!cap || cap->fd < 0)
		return -EINVAL;
	if (cap->streaming)
		return 0;

	for (unsigned int i = 0; i < cap->count; i++) {
		struct v4l2_buffer buf = {};
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl(cap->fd, VIDIOC_QBUF, &buf) < 0)
			return -errno;
	}

	if (ioctl(cap->fd, VIDIOC_STREAMON, &type) < 0)
		return -errno;

	cap->streaming = true;
	return 0;
}

int gc575_capture_dq(struct gc575_capture *cap, struct v4l2_buffer *buf,
		     int timeout_ms)
{
	struct pollfd pfd;
	int ret;

	if (!cap || !buf || cap->fd < 0)
		return -EINVAL;

	pfd.fd = cap->fd;
	pfd.events = POLLIN;
	ret = poll(&pfd, 1, timeout_ms);
	if (ret == 0)
		return -ETIMEDOUT;
	if (ret < 0)
		return -errno;

	memset(buf, 0, sizeof(*buf));
	buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf->memory = V4L2_MEMORY_MMAP;
	if (ioctl(cap->fd, VIDIOC_DQBUF, buf) < 0)
		return -errno;
	return 0;
}

int gc575_capture_q(struct gc575_capture *cap, struct v4l2_buffer *buf)
{
	if (!cap || !buf || cap->fd < 0)
		return -EINVAL;
	if (ioctl(cap->fd, VIDIOC_QBUF, buf) < 0)
		return -errno;
	return 0;
}

void gc575_capture_stop(struct gc575_capture *cap)
{
	if (!cap || cap->fd < 0)
		return;

	if (cap->streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl(cap->fd, VIDIOC_STREAMOFF, &type);
		cap->streaming = false;
	}

	for (unsigned int i = 0; i < cap->count; i++) {
		if (cap->buffers[i].start) {
			munmap(cap->buffers[i].start, cap->buffers[i].length);
			cap->buffers[i].start = NULL;
			cap->buffers[i].length = 0;
		}
	}

	if (cap->count) {
		struct v4l2_requestbuffers req = {
			.count = 0,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
		};
		ioctl(cap->fd, VIDIOC_REQBUFS, &req);
		cap->count = 0;
	}
}

void gc575_capture_close(struct gc575_capture *cap)
{
	if (!cap)
		return;
	gc575_capture_stop(cap);
	mjpeg_decoder_free(cap->decoder);
	cap->decoder = NULL;
	if (cap->fd >= 0) {
		close(cap->fd);
		cap->fd = -1;
	}
}

int gc575_convert_to_yuy2(struct gc575_capture *cap,
			  const struct v4l2_buffer *buf,
			  uint8_t *dst, size_t dst_size,
			  size_t *out_bytes)
{
	const uint8_t *src;
	size_t src_size;
	size_t need;

	if (!cap || !buf || !dst || !out_bytes)
		return -EINVAL;
	if (buf->index >= cap->count)
		return -EINVAL;

	src = cap->buffers[buf->index].start;
	src_size = buf->bytesused ? buf->bytesused : cap->buffers[buf->index].length;
	need = (size_t)cap->width * cap->height * 2;
	if (dst_size < need)
		return -ENOSPC;

	switch (cap->pixelformat) {
	case V4L2_PIX_FMT_YUYV:
		if (src_size > dst_size)
			src_size = dst_size;
		memcpy(dst, src, src_size);
		*out_bytes = src_size;
		return 0;

	case V4L2_PIX_FMT_NV12: {
		size_t y_stride = cap->bytesperline ? cap->bytesperline : cap->width;
		size_t uv_stride = y_stride;
		const uint8_t *y_plane = src;
		const uint8_t *uv_plane = src + y_stride * cap->height;

		if (src_size < y_stride * cap->height +
			       uv_stride * ((cap->height + 1) / 2))
			return -EIO;

		for (uint32_t row = 0; row < cap->height; row++) {
			const uint8_t *yp = y_plane + row * y_stride;
			const uint8_t *uvp = uv_plane +
					     (row / 2) * uv_stride;
			uint8_t *dp = dst + row * cap->width * 2;

			for (uint32_t col = 0; col + 1 < cap->width; col += 2) {
				dp[col * 2 + 0] = yp[col];
				dp[col * 2 + 1] = uvp[col];
				dp[col * 2 + 2] = yp[col + 1];
				dp[col * 2 + 3] = uvp[col + 1];
			}
		}
		*out_bytes = need;
		return 0;
	}

	case V4L2_PIX_FMT_MJPEG:
		return mjpeg_decode(cap->decoder, src, src_size,
				    dst, dst_size, out_bytes);

	default:
		return -ENOTSUP;
	}
}

int gc575_list_modes(const char *device)
{
	struct v4l2_capability cap;
	struct v4l2_fmtdesc fmt;
	int fd;

	if (!device)
		return -EINVAL;
	fd = open(device, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return -errno;

	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		int ret = -errno;
		close(fd);
		return ret;
	}
	printf("%s: %s (%s)\n", device, cap.card, cap.bus_info);

	for (unsigned int fi = 0;; fi++) {
		memset(&fmt, 0, sizeof(fmt));
		fmt.index = fi;
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
			break;

		printf("  %c%c%c%c  %s\n",
		       fmt.pixelformat & 0xff,
		       (fmt.pixelformat >> 8) & 0xff,
		       (fmt.pixelformat >> 16) & 0xff,
		       (fmt.pixelformat >> 24) & 0xff,
		       fmt.description);

		for (unsigned int si = 0;; si++) {
			struct v4l2_frmsizeenum size = {};
			size.index = si;
			size.pixel_format = fmt.pixelformat;
			if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0)
				break;
			if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE)
				break;

			printf("    %ux%u\n", size.discrete.width,
			       size.discrete.height);

			for (unsigned int ii = 0;; ii++) {
				struct v4l2_frmivalenum ival = {};
				ival.index = ii;
				ival.pixel_format = fmt.pixelformat;
				ival.width = size.discrete.width;
				ival.height = size.discrete.height;
				if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS,
					  &ival) < 0)
					break;
				if (ival.type != V4L2_FRMIVAL_TYPE_DISCRETE)
					break;
				if (ival.discrete.numerator)
					printf("      %.3f fps\n",
					       (double)ival.discrete.denominator /
					       (double)ival.discrete.numerator);
			}
		}
	}

	close(fd);
	return 0;
}

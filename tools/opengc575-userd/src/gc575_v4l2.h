/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc575_v4l2.h — UVC/V4L2 capture helpers for the GC575
 */

#ifndef GC575_V4L2_H
#define GC575_V4L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC575_MAX_BUFFERS 8

struct gc575_capture {
	int fd;
	bool streaming;
	unsigned int count;
	struct gc575_buffer {
		void *start;
		size_t length;
	} buffers[GC575_MAX_BUFFERS];
	uint32_t width;
	uint32_t height;
	uint32_t pixelformat;
	uint32_t bytesperline;
	unsigned int fps;
	void *decoder; /* optional MJPEG decoder state */
};

/* Find the first UVC capture node belonging to 07ca:0575. */
int gc575_find_video_node(char *out, size_t out_size);

/*
 * Open @device and negotiate a mode.  A zero @want_fmt / @want_width /
 * @want_height / @want_fps means "keep the driver default".
 */
int gc575_capture_open(struct gc575_capture *cap, const char *device,
		       uint32_t want_fmt, unsigned int want_width,
		       unsigned int want_height, unsigned int want_fps);

int gc575_capture_start(struct gc575_capture *cap);
int gc575_capture_dq(struct gc575_capture *cap, struct v4l2_buffer *buf,
		     int timeout_ms);
int gc575_capture_q(struct gc575_capture *cap, struct v4l2_buffer *buf);
void gc575_capture_stop(struct gc575_capture *cap);
void gc575_capture_close(struct gc575_capture *cap);

/*
 * Convert one dequeued buffer to YUY2 in @dst.  Returns 0 on success and sets
 * @out_bytes.  Supported source formats: YUYV, NV12, MJPEG (when built with
 * libavcodec).  Returns -ENOTSUP for other formats.
 */
int gc575_convert_to_yuy2(struct gc575_capture *cap,
			  const struct v4l2_buffer *buf,
			  uint8_t *dst, size_t dst_size,
			  size_t *out_bytes);

/* Print the formats/sizes/intervals exposed by @device. */
int gc575_list_modes(const char *device);

#ifdef __cplusplus
}
#endif

#endif /* GC575_V4L2_H */

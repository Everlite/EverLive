/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_nosignal.h — Procedural YUYV "NO HDMI SIGNAL" standby frame
 *
 * Generates a dark-gaming standby picture entirely in software (no image
 * libraries, no compressed blobs) and hands it out as a 16-bit YUYV buffer,
 * ready for SDL_PIXELFORMAT_YUY2 textures and for write() to a
 * v4l2loopback node.
 */

#ifndef GC573_USER_NOSIGNAL_H
#define GC573_USER_NOSIGNAL_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define GC573_NO_SIGNAL_DEFAULT_WIDTH	1920U
#define GC573_NO_SIGNAL_DEFAULT_HEIGHT	1080U

struct gc573_no_signal {
	pthread_mutex_t lock;
	uint8_t *pixels;
	unsigned int width;
	unsigned int height;
	size_t bytes;
	uint64_t generation;
};

/**
 * gc573_no_signal_ensure - (Re)generate the standby frame for a geometry
 * @ns: Standby frame state
 * @width: Frame width in pixels (must be even, <= 7680)
 * @height: Frame height in pixels (<= 4320)
 *
 * Cheap when the requested geometry is already cached: the picture is only
 * rendered once per size.  Safe to call from the render thread every tick.
 *
 * Returns 0 on success, -EINVAL for unusable geometry, -ENOMEM on allocation
 * failure.
 */
int gc573_no_signal_ensure(struct gc573_no_signal *ns, unsigned int width,
			   unsigned int height);

/**
 * gc573_no_signal_frame - Borrow the current immutable YUYV frame
 * @ns: Standby frame state
 * @bytes: Optional output for the frame size in bytes
 *
 * The returned buffer stays valid until the next gc573_no_signal_ensure()
 * call that changes the geometry, and must not be freed by the caller.
 */
const void *gc573_no_signal_frame(struct gc573_no_signal *ns, size_t *bytes);

unsigned int gc573_no_signal_width(const struct gc573_no_signal *ns);
unsigned int gc573_no_signal_height(const struct gc573_no_signal *ns);

/**
 * gc573_no_signal_destroy - Release the cached frame
 */
void gc573_no_signal_destroy(struct gc573_no_signal *ns);

#endif /* GC573_USER_NOSIGNAL_H */

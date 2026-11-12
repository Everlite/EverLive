/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_v4l2_card.h — Card descriptor for the shared V4L2 preload engine
 *
 * Each preload DSO provides exactly one everlive_v4l2_card_descriptor().  The
 * state machine and the libc interposer use it for node numbers, names,
 * formats, mmap layout and the shared frame-store name.
 */

#ifndef EVERLIVE_V4L2_CARD_H
#define EVERLIVE_V4L2_CARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVERLIVE_V4L2_MAX_PORTS	2U
#define EVERLIVE_V4L2_MAX_SIZES	8U
#define EVERLIVE_V4L2_MAX_FPS	8U

struct everlive_v4l2_size {
	uint32_t width;
	uint32_t height;
};

/*
 * S_FMT/TRY_FMT clamp bucket.
 *
 * Requests up to request_width x request_height are answered with
 * width x height.  Buckets are listed smallest first; the last bucket is also
 * the fallback for larger requests.
 */
struct everlive_v4l2_clamp {
	uint32_t request_width;
	uint32_t request_height;
	uint32_t width;
	uint32_t height;
};

struct everlive_v4l2_card {
	const char *shm_name;
	const char *slug;
	const char *driver;
	const char *card_name;
	const char *port_names[EVERLIVE_V4L2_MAX_PORTS];
	const char *bus_suffixes[EVERLIVE_V4L2_MAX_PORTS];
	const char *fallback_bdf;
	unsigned int node_base;
	unsigned int port_count;
	unsigned int max_width;
	unsigned int max_height;
	unsigned int default_width;
	unsigned int default_height;
	uint32_t mmap_base;
	uint32_t mmap_stride;
	const struct everlive_v4l2_size *sizes;
	size_t size_count;
	const uint32_t *fps_denominators;
	size_t fps_count;
	const struct everlive_v4l2_clamp *clamps;
	size_t clamp_count;
};

const struct everlive_v4l2_card *everlive_v4l2_card_descriptor(void);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_V4L2_CARD_H */

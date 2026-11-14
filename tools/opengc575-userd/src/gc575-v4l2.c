// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc575-v4l2.c — GC575 descriptor for the shared V4L2 preload engine.
 *
 * The daemon captures the real uvcvideo node and publishes YUY2 frames to
 * /gc575_v4l2_shm.  This descriptor exposes those frames as /dev/video64
 * inside an LD_PRELOADed process (OBS, Discord, FFmpeg, everlive_display).
 */

#include "everlive_v4l2_card.h"

static const struct everlive_v4l2_size gc575_sizes[] = {
	{ 3840, 2160 },
	{ 2560, 1440 },
	{ 1920, 1080 },
	{ 1280, 720 },
	{ 640, 360 },
};

static const uint32_t gc575_fps[] = { 240, 144, 120, 60, 50, 30, 24 };

static const struct everlive_v4l2_clamp gc575_clamps[] = {
	{ 640, 480, 640, 360 },
	{ 1280, 720, 1280, 720 },
	{ 1920, 1080, 1920, 1080 },
	{ 2560, 1440, 2560, 1440 },
	{ 3840, 2160, 3840, 2160 },
};

static const struct everlive_v4l2_card gc575_card = {
	.shm_name = "/gc575_v4l2_shm",
	.slug = "gc575",
	.driver = "gc575-user",
	.card_name = "EverLive GC575 Live Gamer 4K 2.1",
	.port_names = {
		"EverLive GC575 Live Gamer 4K 2.1",
		NULL,
	},
	.bus_suffixes = { NULL, NULL },
	.fallback_bdf = "everlive:gc575",
	.node_base = 64,
	.port_count = 1,
	.max_width = 3840,
	.max_height = 2160,
	.default_width = 1920,
	.default_height = 1080,
	.mmap_base = 0x50000000U,
	.mmap_stride = 0x02000000U, /* 32 MB stride for 4K frames */
	.sizes = gc575_sizes,
	.size_count = sizeof(gc575_sizes) / sizeof(gc575_sizes[0]),
	.fps_denominators = gc575_fps,
	.fps_count = sizeof(gc575_fps) / sizeof(gc575_fps[0]),
	.clamps = gc575_clamps,
	.clamp_count = sizeof(gc575_clamps) / sizeof(gc575_clamps[0]),
};

const struct everlive_v4l2_card *everlive_v4l2_card_descriptor(void)
{
	return &gc575_card;
}

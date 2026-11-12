// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc573-v4l2.c — GC573 (Live Gamer 4K) descriptor for the shared V4L2
 * preload engine.
 */

#include "everlive_v4l2_card.h"

static const struct everlive_v4l2_size gc573_sizes[] = {
	{ 3840, 2160 },
	{ 2560, 1440 },
	{ 1920, 1080 },
	{ 1280, 720 },
	{ 640, 360 },
};

static const uint32_t gc573_fps[] = { 60, 120, 240, 144, 30, 50 };

static const struct everlive_v4l2_clamp gc573_clamps[] = {
	{ 640, 480, 640, 360 },
	{ 1280, 720, 1280, 720 },
	{ 1920, 1080, 1920, 1080 },
	{ 2560, 1440, 2560, 1440 },
	{ 3840, 2160, 3840, 2160 },
};

static const struct everlive_v4l2_card gc573_card = {
	.shm_name = "/gc573_v4l2_shm",
	.slug = "gc573",
	.driver = "gc573-user",
	.card_name = "EverLive GC573 Live Gamer 4K",
	.port_names = {
		"EverLive GC573 Live Gamer 4K",
		NULL,
	},
	.bus_suffixes = { NULL, NULL },
	.fallback_bdf = "everlive:gc573",
	.node_base = 63,
	.port_count = 1,
	.max_width = 3840,
	.max_height = 2160,
	.default_width = 1920,
	.default_height = 1080,
	.mmap_base = 0x50000000U,
	.mmap_stride = 0x02000000U, /* 32 MB stride for 4K frames */
	.sizes = gc573_sizes,
	.size_count = sizeof(gc573_sizes) / sizeof(gc573_sizes[0]),
	.fps_denominators = gc573_fps,
	.fps_count = sizeof(gc573_fps) / sizeof(gc573_fps[0]),
	.clamps = gc573_clamps,
	.clamp_count = sizeof(gc573_clamps) / sizeof(gc573_clamps[0]),
};

const struct everlive_v4l2_card *everlive_v4l2_card_descriptor(void)
{
	return &gc573_card;
}

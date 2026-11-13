// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc570d-v4l2.c — GC570D (Live Gamer DUO) descriptor for the shared V4L2
 * preload engine.
 */

#include "everlive_v4l2_card.h"

static const struct everlive_v4l2_size gc570d_sizes[] = {
	{ 1920, 1080 },
	{ 1280, 720 },
	{ 640, 360 },
};

static const uint32_t gc570d_fps[] = { 60, 30, 50 };

static const struct everlive_v4l2_clamp gc570d_clamps[] = {
	{ 640, 480, 640, 360 },
	{ 1280, 720, 1280, 720 },
	{ 1920, 1080, 1920, 1080 },
};

static const struct everlive_v4l2_card gc570d_card = {
	.shm_name = "/gc570d_v4l2_shm",
	.slug = "gc570d",
	.driver = "gc570d-user",
	.card_name = "EverLive GC570D",
	.port_names = {
		"GC570D HDMI 1 (Passthrough)",
		"GC570D HDMI 2 (Capture)",
	},
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
	.sizes = gc570d_sizes,
	.size_count = sizeof(gc570d_sizes) / sizeof(gc570d_sizes[0]),
	.fps_denominators = gc570d_fps,
	.fps_count = sizeof(gc570d_fps) / sizeof(gc570d_fps[0]),
	.clamps = gc570d_clamps,
	.clamp_count = sizeof(gc570d_clamps) / sizeof(gc570d_clamps[0]),
};

const struct everlive_v4l2_card *everlive_v4l2_card_descriptor(void)
{
	return &gc570d_card;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * hwsuhd-v4l2.c — HWS UHD (ACASIS VS2583 / AVMatrix VC12-4K) descriptor for
 * the shared V4L2 preload engine.  Node /dev/video64 (EverLive range 60-69;
 * 60/61 GC570D, 63 GC573).
 */

#include "everlive_v4l2_card.h"

static const struct everlive_v4l2_size hwsuhd_sizes[] = {
	{ 3840, 2160 },
	{ 2560, 1440 },
	{ 1920, 1080 },
	{ 1280, 720 },
	{ 640, 360 },
};

static const uint32_t hwsuhd_fps[] = { 60, 120, 144, 240, 30, 50 };

static const struct everlive_v4l2_clamp hwsuhd_clamps[] = {
	{ 640, 480, 640, 360 },
	{ 1280, 720, 1280, 720 },
	{ 1920, 1080, 1920, 1080 },
	{ 2560, 1440, 2560, 1440 },
	{ 3840, 2160, 3840, 2160 },
};

static const struct everlive_v4l2_card hwsuhd_card = {
	.shm_name = "/hwsuhd_v4l2_shm",
	.slug = "hwsuhd",
	.driver = "hwsuhd-user",
	.card_name = "EverLive HWS UHD Capture",
	.port_names = {
		"EverLive HWS UHD Capture",
		NULL,
	},
	.bus_suffixes = { NULL, NULL },
	.fallback_bdf = "everlive:hwsuhd",
	.node_base = 64,
	.port_count = 1,
	.max_width = 3840,
	.max_height = 2160,
	.default_width = 1920,
	.default_height = 1080,
	.mmap_base = 0x58000000U,
	.mmap_stride = 0x02000000U, /* 32 MB: 4K RGB32 is 33,177,600 bytes */
	.sizes = hwsuhd_sizes,
	.size_count = sizeof(hwsuhd_sizes) / sizeof(hwsuhd_sizes[0]),
	.fps_denominators = hwsuhd_fps,
	.fps_count = sizeof(hwsuhd_fps) / sizeof(hwsuhd_fps[0]),
	.clamps = hwsuhd_clamps,
	.clamp_count = sizeof(hwsuhd_clamps) / sizeof(hwsuhd_clamps[0]),
};

const struct everlive_v4l2_card *everlive_v4l2_card_descriptor(void)
{
	return &hwsuhd_card;
}

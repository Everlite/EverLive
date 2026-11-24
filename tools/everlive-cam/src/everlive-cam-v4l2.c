// SPDX-License-Identifier: GPL-2.0-only
/*
 * everlive-cam-v4l2.c — EverLive webcam bridge descriptor for the shared V4L2
 * preload engine.  Node /dev/video65 (EverLive range 60-69; 60/61 GC570D,
 * 63 GC573, 64 HWS UHD).  The frame store is published by everlive_camd.
 */

#include "everlive_v4l2_card.h"

static const struct everlive_v4l2_size cam_sizes[] = {
	{ 1920, 1080 },
	{ 1280, 720 },
	{ 2560, 1440 },
	{ 3840, 2160 },
	{ 960, 540 },
	{ 640, 360 },
};

static const uint32_t cam_fps[] = { 30, 60, 25, 24 };

static const struct everlive_v4l2_clamp cam_clamps[] = {
	{ 640, 480, 640, 360 },
	{ 960, 540, 960, 540 },
	{ 1280, 720, 1280, 720 },
	{ 1920, 1080, 1920, 1080 },
	{ 2560, 1440, 2560, 1440 },
	{ 3840, 2160, 3840, 2160 },
};

static const struct everlive_v4l2_card cam_card = {
	.shm_name = "/everlive_cam_v4l2_shm",
	.slug = "everlive-cam",
	.driver = "everlive-cam",
	.card_name = "EverLive Webcam",
	.port_names = {
		"EverLive Webcam",
		NULL,
	},
	.bus_suffixes = { NULL, NULL },
	.fallback_bdf = "everlive:cam",
	.node_base = 65,
	.port_count = 1,
	.max_width = 3840,
	.max_height = 2160,
	.default_width = 1920,
	.default_height = 1080,
	.mmap_base = 0x59000000U,
	.mmap_stride = 0x02000000U, /* 32 MB: 4K RGB32 is 33,177,600 bytes */
	.sizes = cam_sizes,
	.size_count = sizeof(cam_sizes) / sizeof(cam_sizes[0]),
	.fps_denominators = cam_fps,
	.fps_count = sizeof(cam_fps) / sizeof(cam_fps[0]),
	.clamps = cam_clamps,
	.clamp_count = sizeof(cam_clamps) / sizeof(cam_clamps[0]),
};

const struct everlive_v4l2_card *everlive_v4l2_card_descriptor(void)
{
	return &cam_card;
}

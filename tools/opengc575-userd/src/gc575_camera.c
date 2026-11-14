// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc575_camera.c — GC575 shared-memory YUY2 frame-store publisher
 *
 * The GC575 captures NV12, YUY2 or MJPEG.  The shared framestore contract is
 * YUY2, so the capture path converts before publishing.  This mirrors the
 * GC573 camera wrapper and keeps the preload DSO independent of UVC details.
 */

#include "gc575_camera.h"

#include "everlive_framestore.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct everlive_framestore *g_camera_fs;

int gc575_camera_start(const char *bus_id)
{
	struct everlive_framestore_config cfg = {
		.name = "/gc575_v4l2_shm",
		.pci_bdf = bus_id,
		.port_count = 1,
		.slot_count = 3,
		.frame_stride = 3840U * 2160U * 2U,
	};
	int err;

	if (g_camera_fs)
		return 0;

	errno = 0;
	g_camera_fs = everlive_framestore_create(&cfg);
	if (!g_camera_fs) {
		err = errno ? errno : EIO;
		fprintf(stderr,
			"[gc575 WARN] Virtual camera frame store unavailable: %s\n",
			strerror(err));
		return -err;
	}

	return 0;
}

void gc575_camera_stop(void)
{
	if (!g_camera_fs)
		return;

	everlive_framestore_destroy(g_camera_fs);
	g_camera_fs = NULL;
}

bool gc575_camera_active(void)
{
	return g_camera_fs != NULL;
}

bool gc575_camera_publish(const void *frame, size_t bytes,
			  unsigned int width, unsigned int height,
			  bool no_signal)
{
	if (!g_camera_fs || !frame || bytes == 0)
		return false;
	if (bytes > everlive_framestore_frame_stride(g_camera_fs))
		return false;

	return everlive_framestore_publish(g_camera_fs, 0, frame, bytes,
					   width, height, no_signal,
					   !no_signal);
}

void gc575_camera_mark_no_signal(void)
{
	if (g_camera_fs)
		everlive_framestore_mark_no_signal(g_camera_fs, 0);
}

void gc575_camera_reset(void)
{
	if (g_camera_fs)
		everlive_framestore_reset(g_camera_fs, 0);
}

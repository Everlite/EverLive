// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc573_camera.c — GC573 virtual-camera frame-store publisher
 *
 * The streamer publishes post-tonemap frames from the preview thread.  Idle
 * periods publish the generated standby card from the main loop, which keeps
 * the heartbeat fresh and prevents OBS/Discord from showing a frozen picture.
 */

#include "gc573_camera.h"

#include "everlive_framestore.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct everlive_framestore *g_camera_fs;

int gc573_camera_start(const char *pci_bdf)
{
	struct everlive_framestore_config cfg = {
		.name = "/gc573_v4l2_shm",
		.pci_bdf = pci_bdf,
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
			"[gc573 WARN] Virtual camera frame store unavailable: %s\n",
			strerror(err));
		return -err;
	}

	return 0;
}

void gc573_camera_stop(void)
{
	if (!g_camera_fs)
		return;

	everlive_framestore_destroy(g_camera_fs);
	g_camera_fs = NULL;
}

bool gc573_camera_active(void)
{
	return g_camera_fs != NULL;
}

bool gc573_camera_publish(const void *frame, size_t bytes,
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

void gc573_camera_mark_no_signal(void)
{
	if (g_camera_fs)
		everlive_framestore_mark_no_signal(g_camera_fs, 0);
}

void gc573_camera_reset(void)
{
	if (g_camera_fs)
		everlive_framestore_reset(g_camera_fs, 0);
}

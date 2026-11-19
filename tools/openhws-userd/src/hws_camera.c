// SPDX-License-Identifier: GPL-2.0-only
/*
 * hws_camera.c — HWS UHD virtual-camera frame-store publisher.
 *
 * Live frames are published from the output thread; with no signal the
 * standby card is published at ~30 fps, which keeps the heartbeat fresh.
 */

#include "hws_camera.h"
#include "hws_user_regs.h"

#include "everlive_framestore.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct everlive_framestore *g_fs;

int hws_camera_start(const char *pci_bdf)
{
	struct everlive_framestore_config cfg = {
		.name = HWS_CAMERA_SHM_NAME,
		.pci_bdf = pci_bdf,
		.port_count = 1,
		.slot_count = 3,
		.frame_stride = HWS_MAX_WIDTH * HWS_MAX_HEIGHT * 2U,
	};

	if (g_fs)
		return 0;
	errno = 0;
	g_fs = everlive_framestore_create(&cfg);
	if (!g_fs) {
		int err = errno ? errno : EIO;

		fprintf(stderr, "[HWS-USER][WARN] virtual camera frame store unavailable: %s\n",
			strerror(err));
		return -err;
	}
	return 0;
}

void hws_camera_stop(void)
{
	if (g_fs) {
		everlive_framestore_destroy(g_fs);
		g_fs = NULL;
	}
}

bool hws_camera_active(void)
{
	return g_fs != NULL;
}

bool hws_camera_publish(const void *frame, size_t bytes, unsigned int width,
			unsigned int height, bool no_signal)
{
	if (!g_fs || !frame || !bytes || bytes > everlive_framestore_frame_stride(g_fs))
		return false;
	return everlive_framestore_publish(g_fs, 0, frame, bytes, width, height,
					   no_signal, !no_signal);
}

void hws_camera_mark_no_signal(void)
{
	if (g_fs)
		everlive_framestore_mark_no_signal(g_fs, 0);
}

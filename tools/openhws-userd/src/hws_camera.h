/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hws_camera.h — HWS UHD virtual-camera frame-store publisher
 * (/dev/shm/hwsuhd_v4l2_shm, read by libhwsuhd-v4l2.so).
 */
#ifndef HWS_CAMERA_H
#define HWS_CAMERA_H

#include <stdbool.h>
#include <stddef.h>

#define HWS_CAMERA_SHM_NAME	"/hwsuhd_v4l2_shm"

int hws_camera_start(const char *pci_bdf);
void hws_camera_stop(void);
bool hws_camera_active(void);
bool hws_camera_publish(const void *frame, size_t bytes, unsigned int width,
			unsigned int height, bool no_signal);
void hws_camera_mark_no_signal(void);

#endif /* HWS_CAMERA_H */

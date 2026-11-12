// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc573_camera.h — GC573 virtual-camera frame-store publisher
 */

#ifndef GC573_CAMERA_H
#define GC573_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int gc573_camera_start(const char *pci_bdf);
void gc573_camera_stop(void);

bool gc573_camera_active(void);

/* Publish one post-tonemap YUYV frame. */
bool gc573_camera_publish(const void *frame, size_t bytes,
			  unsigned int width, unsigned int height,
			  bool no_signal);

/* Mark the camera feed as temporarily unavailable without destroying it. */
void gc573_camera_mark_no_signal(void);

/* Reset the port state after a capture restart or resume. */
void gc573_camera_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* GC573_CAMERA_H */

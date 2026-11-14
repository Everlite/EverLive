/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc575_camera.h — GC575 shared-memory YUY2 frame-store publisher
 */

#ifndef GC575_CAMERA_H
#define GC575_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int gc575_camera_start(const char *bus_id);
void gc575_camera_stop(void);
bool gc575_camera_active(void);

bool gc575_camera_publish(const void *frame, size_t bytes,
			  unsigned int width, unsigned int height,
			  bool no_signal);

void gc575_camera_mark_no_signal(void);
void gc575_camera_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* GC575_CAMERA_H */

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_video_tap.h — Native PipeWire Video/Source for the GC573
 *
 * Publishes the captured YUY2 frames as a normal PipeWire camera node so OBS,
 * Discord, browsers and FFmpeg can consume the card without v4l2loopback.
 * The module mirrors the GC570D producer: a private thread-loop, a driving
 * stream and a timer paced by the source frame rate.
 */

#ifndef GC573_USER_VIDEO_TAP_H
#define GC573_USER_VIDEO_TAP_H

#include <stdbool.h>
#include <stddef.h>

#define GC573_VIDEO_TAP_DEFAULT	"gc573_video"

/**
 * gc573_video_tap_start - Create the PipeWire client and video source node
 * @node_name: Node name to publish (NULL/"" for the default)
 *
 * Returns 0 on success, a negative errno when PipeWire is unavailable, or
 * 0 immediately when the tap is already running.
 */
int gc573_video_tap_start(const char *node_name);

/**
 * gc573_video_tap_set_format - Publish a new capture geometry
 * @width: Source width in pixels
 * @height: Source height in pixels
 * @fps_x100: Source frame rate in hundredths (5994 for 59.94 Hz)
 *
 * Reconnects the stream when the geometry or rate changes; a no-op otherwise.
 * Safe to call on every frame from the capture thread.
 */
void gc573_video_tap_set_format(unsigned int width, unsigned int height,
				unsigned int fps_x100);

/**
 * gc573_video_tap_write - Publish one YUY2 frame
 * @data: Packed YUY2 frame (stride = width * 2)
 * @bytes: Size of the frame
 * @width: Frame width
 * @height: Frame height
 *
 * The frame is copied into the tap's latest-frame slot; the PipeWire process
 * callback copies it into a graph buffer at the negotiated rate.  A slow
 * reader therefore never stalls capture.
 */
void gc573_video_tap_write(const void *data, size_t bytes,
			   unsigned int width, unsigned int height);

/**
 * gc573_video_tap_stop - Tear the PipeWire client down
 */
void gc573_video_tap_stop(void);

/**
 * gc573_video_tap_active - True when the tap is running
 */
bool gc573_video_tap_active(void);

#endif /* GC573_USER_VIDEO_TAP_H */

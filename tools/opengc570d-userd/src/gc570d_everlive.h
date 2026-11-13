// SPDX-License-Identifier: GPL-2.0-only
/*
 * EverLive GC570D presentation frontend.
 *
 * The capture daemon owns the VFIO device and publishes the two HDMI ports
 * into the GC570D POSIX shared-memory frame store.  This frontend consumes
 * those frames for the EverLive SDL2 window and the optional dual
 * v4l2loopback virtual cameras, so the capture thread never blocks on a
 * compositor, OBS or a PipeWire graph reconfiguration.
 */
#ifndef GC570D_EVERLIVE_H
#define GC570D_EVERLIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gc570d_dev;
struct everlive_framestore;

struct gc570d_everlive_options {
	bool enable_sdl;
	bool headless;
	bool pip;
	bool osd;
	bool fullscreen;
	bool vsync;
	bool studio;
	bool muted;
	int port;                 /* 0 = HDMI IN 1, 1 = HDMI IN 2 */
	int volume_pct;           /* 0..150 (default 100) */
	const char *aspect;       /* auto|16:9|4:3|stretch */
	const char *scale;        /* fit|integer */
	const char *osd_pos;      /* tr|br|bl|tl */
	const char *v4l2[2];      /* optional /dev/video10 and /dev/video11 */
	const char *title;
};

int gc570d_everlive_start(struct everlive_framestore *framestore,
			  struct gc570d_dev *gc,
			  const struct gc570d_everlive_options *options);
void gc570d_everlive_stop(void);
void gc570d_everlive_set_dev(struct gc570d_dev *gc);
bool gc570d_everlive_quit_requested(void);
void gc570d_everlive_set_port(int port);
void gc570d_everlive_set_osd(bool enabled);
void gc570d_everlive_set_osd_pos(const char *pos);
void gc570d_everlive_set_pip(bool enabled);
void gc570d_everlive_set_aspect(const char *aspect);
void gc570d_everlive_set_scale(const char *scale);
void gc570d_everlive_set_volume(int volume_pct);
void gc570d_everlive_set_mute(bool muted);
void gc570d_everlive_push_audio(int port, const uint8_t *pcm, size_t bytes);

/* Read-only accessors for the control plane (safe before start). */
bool gc570d_everlive_running(void);
bool gc570d_everlive_headless(void);
int gc570d_everlive_port(void);
bool gc570d_everlive_pip(void);
bool gc570d_everlive_osd(void);
const char *gc570d_everlive_osd_pos(void);
const char *gc570d_everlive_aspect(void);
const char *gc570d_everlive_scale(void);
int gc570d_everlive_volume(void);
bool gc570d_everlive_muted(void);
const char *gc570d_everlive_v4l2(int port);

#endif /* GC570D_EVERLIVE_H */

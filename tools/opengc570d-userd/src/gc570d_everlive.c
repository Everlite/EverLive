// SPDX-License-Identifier: GPL-2.0-only
/*
 * EverLive GC570D presentation frontend.
 *
 * Copyright (C) 2026 Everlite
 *
 * The hardware capture engine is derived from neoyagami/OpenGC570D and the
 * PolygonPrincess/OpenGC570D-Userspace port.  This file only consumes the
 * shared-memory frames that engine publishes; it never touches BAR0, VFIO or
 * the DMA rings.
 */
#define GC570D_USE_SYSTEM_V4L2
#include <linux/videodev2.h>

#include "gc570d-user-compat.h"
#include "gc570d.h"
#include "gc570d-shm.h"
#include "gc570d_everlive.h"
#include "gc570d_font8x8.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
#endif

#define EVERLIVE_FRAME_BYTES GC570D_SHM_FRAME_BYTES
#define EVERLIVE_V4L2_RETRY_NS (2ULL * 1000000000ULL)
#define EVERLIVE_STATUS_NS (1ULL * 1000000000ULL)

enum everlive_aspect_mode {
	EVERLIVE_ASPECT_AUTO = 0,
	EVERLIVE_ASPECT_16_9 = 1,
	EVERLIVE_ASPECT_4_3 = 2,
	EVERLIVE_ASPECT_STRETCH = 3,
};

enum everlive_scale_mode {
	EVERLIVE_SCALE_FIT = 0,
	EVERLIVE_SCALE_INTEGER = 1,
};

enum everlive_osd_pos {
	EVERLIVE_OSD_TR = 0,
	EVERLIVE_OSD_BR = 1,
	EVERLIVE_OSD_BL = 2,
	EVERLIVE_OSD_TL = 3,
};

struct everlive_frame {
	uint32_t width;
	uint32_t height;
	uint32_t no_signal;
	uint32_t streaming;
	uint64_t seq;
	uint64_t timestamp_ns;
	uint8_t *pixels;
};

struct everlive_v4l2 {
	char path[96];
	int fd;
	uint32_t width;
	uint32_t height;
	uint64_t frames;
	uint64_t dropped;
	uint64_t last_try_ns;
	bool configured;
};

struct everlive_state {
	struct gc570d_shm_region *shm;
	struct gc570d_dev *gc;
	struct gc570d_everlive_options opt;
	pthread_t thread;
	atomic_bool started;
	atomic_bool running;
	atomic_bool quit;
	atomic_int port;
	atomic_bool pip;
	atomic_bool osd;
	atomic_int osd_pos;
	atomic_int aspect;
	atomic_int scale;
	atomic_int volume_pct;
	atomic_bool muted;
#ifdef HAVE_SDL2
	atomic_uint audio_dev;
#endif
	struct everlive_frame frame[2];
	struct everlive_v4l2 v4l2[2];
	uint64_t last_seen[2];
	uint64_t last_status_ns;
	uint64_t last_fps_ns;
	uint64_t last_fps_frames;
	uint32_t last_profile[2];
};

static struct everlive_state g_everlive = {
	.started = ATOMIC_VAR_INIT(false),
	.running = ATOMIC_VAR_INIT(false),
	.quit = ATOMIC_VAR_INIT(false),
	.port = ATOMIC_VAR_INIT(0),
	.pip = ATOMIC_VAR_INIT(false),
	.osd = ATOMIC_VAR_INIT(true),
	.osd_pos = ATOMIC_VAR_INIT(EVERLIVE_OSD_TR),
	.aspect = ATOMIC_VAR_INIT(EVERLIVE_ASPECT_AUTO),
	.scale = ATOMIC_VAR_INIT(EVERLIVE_SCALE_FIT),
	.volume_pct = ATOMIC_VAR_INIT(100),
	.muted = ATOMIC_VAR_INIT(false),
#ifdef HAVE_SDL2
	.audio_dev = ATOMIC_VAR_INIT(0),
#endif
	.v4l2 = {
		{ .fd = -1 },
		{ .fd = -1 },
	},
};

static int parse_aspect_mode(const char *aspect)
{
	if (!aspect)
		return EVERLIVE_ASPECT_AUTO;
	if (strcmp(aspect, "16:9") == 0)
		return EVERLIVE_ASPECT_16_9;
	if (strcmp(aspect, "4:3") == 0)
		return EVERLIVE_ASPECT_4_3;
	if (strcmp(aspect, "stretch") == 0)
		return EVERLIVE_ASPECT_STRETCH;
	return EVERLIVE_ASPECT_AUTO;
}

static int parse_scale_mode(const char *scale)
{
	if (scale && strcmp(scale, "integer") == 0)
		return EVERLIVE_SCALE_INTEGER;
	return EVERLIVE_SCALE_FIT;
}

static int parse_osd_pos(const char *pos)
{
	if (!pos)
		return EVERLIVE_OSD_TR;
	if (strcmp(pos, "br") == 0)
		return EVERLIVE_OSD_BR;
	if (strcmp(pos, "bl") == 0)
		return EVERLIVE_OSD_BL;
	if (strcmp(pos, "tl") == 0)
		return EVERLIVE_OSD_TL;
	return EVERLIVE_OSD_TR;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#ifdef HAVE_SDL2
static const char *shm_port_name(int port)
{
	return port == 0 ? "HDMI IN 1" : "HDMI IN 2";
}

static const char *aspect_mode_label(int mode)
{
	switch (mode) {
	case EVERLIVE_ASPECT_16_9:
		return "16:9";
	case EVERLIVE_ASPECT_4_3:
		return "4:3";
	case EVERLIVE_ASPECT_STRETCH:
		return "Stretch";
	case EVERLIVE_ASPECT_AUTO:
	default:
		return "Auto";
	}
}
#endif

static const char *shm_profile_name(uint32_t profile)
{
	switch (profile) {
	case GC570D_VIDEO0_SDR_RGB:
		return "RGB";
	case GC570D_VIDEO0_SDR_YUV420_BT709:
		return "YCbCr 4:2:0";
	case GC570D_VIDEO0_HDR_BT2020:
	default:
		return "BT.2020 HDR-to-SDR";
	}
}

static unsigned int shm_port_channels(const struct everlive_state *st, int port)
{
	struct gc570d_dev *gc = READ_ONCE(st->gc);

	if (!gc)
		return 2;
	if (port == 0)
		return gc->audio0_channels ? gc->audio0_channels : 2;
	return gc->audio_channels ? gc->audio_channels : 2;
}

static unsigned int shm_port_rate(const struct everlive_state *st, int port)
{
	struct gc570d_dev *gc = READ_ONCE(st->gc);

	if (!gc)
		return 48000;
	if (port == 0)
		return gc->audio0_rate ? gc->audio0_rate : 48000;
	return gc->audio_rate ? gc->audio_rate : 48000;
}

static unsigned int shm_port_source_width(const struct everlive_state *st, int port)
{
	struct gc570d_dev *gc = READ_ONCE(st->gc);

	if (!gc)
		return st->frame[port].width;
	return port == 0 ?
		(gc->video0_source_width ? gc->video0_source_width :
		 gc->video0_width) :
		(gc->video_source_width ? gc->video_source_width :
		 GC570D_WIDTH);
}

static unsigned int shm_port_source_height(const struct everlive_state *st, int port)
{
	struct gc570d_dev *gc = READ_ONCE(st->gc);

	if (!gc)
		return st->frame[port].height;
	return port == 0 ?
		(gc->video0_source_height ? gc->video0_source_height :
		 gc->video0_height) :
		(gc->video_source_height ? gc->video_source_height :
		 GC570D_HEIGHT);
}

static unsigned int shm_port_source_fps(const struct everlive_state *st, int port)
{
	struct gc570d_dev *gc = READ_ONCE(st->gc);

	if (!gc)
		return 60;
	if (port == 0)
		return gc->video0_source_fps ? gc->video0_source_fps : 60;
	return gc->video_source_fps ? gc->video_source_fps : 60;
}

static unsigned int shm_port_profile(const struct everlive_state *st, int port)
{
	struct gc570d_dev *gc = READ_ONCE(st->gc);

	if (!gc)
		return GC570D_VIDEO0_HDR_BT2020;
	return port == 0 ? gc->video0_source_profile : gc->video_source_profile;
}

/*
 * Snapshot one port with a seqlock.  The DMA completion path publishes the
 * sequence number with release semantics after the payload copy; retrying on a
 * changed sequence prevents a half-written frame from reaching SDL or OBS.
 */
static bool everlive_snapshot(struct everlive_state *st, int port)
{
	struct gc570d_shm_port *sp = &st->shm->ports[port];
	struct everlive_frame *frame = &st->frame[port];
	uint64_t seq;
	uint32_t width;
	uint32_t height;
	uint32_t bytes;
	unsigned int tries;

	for (tries = 0; tries < 8; tries++) {
		seq = __atomic_load_n(&sp->seq, __ATOMIC_ACQUIRE);
		if (!seq || (seq & 1))
			continue;
		if (seq == st->last_seen[port])
			return false;

		width = __atomic_load_n(&sp->width, __ATOMIC_ACQUIRE);
		height = __atomic_load_n(&sp->height, __ATOMIC_ACQUIRE);
		if (width < 2 || height < 2 || width > 1920 || height > 1080)
			return false;
		bytes = width * height * 2;
		if (bytes > EVERLIVE_FRAME_BYTES)
			return false;

		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		memcpy(frame->pixels, sp->yuy2, bytes);
		frame->width = width;
		frame->height = height;
		frame->no_signal = __atomic_load_n(&sp->no_signal, __ATOMIC_RELAXED);
		frame->streaming = __atomic_load_n(&sp->streaming, __ATOMIC_RELAXED);
		frame->timestamp_ns = __atomic_load_n(&sp->timestamp_ns,
						      __ATOMIC_RELAXED);

		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&sp->seq, __ATOMIC_ACQUIRE) == seq) {
			frame->seq = seq;
			st->last_seen[port] = seq;
			st->last_profile[port] = shm_port_profile(st, port);
			return true;
		}
	}

	return false;
}

static int everlive_v4l2_open(struct everlive_v4l2 *out, uint32_t width,
			      uint32_t height)
{
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	int fd;

	fd = open(out->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	memset(&cap, 0, sizeof(cap));
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		close(fd);
		return -errno;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = width * 2;
	fmt.fmt.pix.sizeimage = width * height * 2;
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
	fmt.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_709;
	fmt.fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	fmt.fmt.pix.xfer_func = V4L2_XFER_FUNC_709;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		int saved = errno;

		close(fd);
		return -saved;
	}

	out->fd = fd;
	out->width = fmt.fmt.pix.width;
	out->height = fmt.fmt.pix.height;
	out->configured = true;
	fprintf(stderr,
		"[gc570d INFO] %s: virtual camera %s configured %ux%u YUYV\n",
		out->path, out->path, out->width, out->height);
	return 0;
}

static void everlive_v4l2_close(struct everlive_v4l2 *out)
{
	if (out->fd >= 0) {
		close(out->fd);
		out->fd = -1;
	}
	out->configured = false;
}

static void everlive_v4l2_write(struct everlive_v4l2 *out,
				const struct everlive_frame *frame,
				uint64_t now)
{
	size_t bytes;
	ssize_t written;

	if (!out->path[0] || !frame->pixels || !frame->width || !frame->height)
		return;

	if (out->fd < 0) {
		if (now - out->last_try_ns < EVERLIVE_V4L2_RETRY_NS)
			return;
		out->last_try_ns = now;
		if (everlive_v4l2_open(out, frame->width, frame->height) < 0)
			return;
	}

	if (out->width != frame->width || out->height != frame->height) {
		if (now - out->last_try_ns < EVERLIVE_V4L2_RETRY_NS)
			return;
		out->last_try_ns = now;
		everlive_v4l2_close(out);
		if (everlive_v4l2_open(out, frame->width, frame->height) < 0)
			return;
	}

	bytes = (size_t)frame->width * frame->height * 2;
	written = write(out->fd, frame->pixels, bytes);
	if (written == (ssize_t)bytes) {
		out->frames++;
		return;
	}
	if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		out->dropped++;
		return;
	}
	if (written < 0 && errno != EINTR) {
		out->dropped++;
		everlive_v4l2_close(out);
	}
}

#ifdef HAVE_SDL2
struct everlive_sdl {
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *texture[2];
	uint32_t texture_w[2];
	uint32_t texture_h[2];
	int texture_scale_mode[2];
	SDL_Texture *overlay;
	uint32_t *overlay_pixels;
	uint32_t overlay_w;
	uint32_t overlay_h;
	uint64_t overlay_built_ns;
	SDL_AudioDeviceID audio_dev;
	bool fullscreen;
};

static void sdl_draw_glyph(uint32_t *pixels, uint32_t stride, int x, int y,
			   unsigned char ch, int scale, uint32_t color)
{
	const uint8_t *glyph;
	int row;
	int col;

	if (ch >= 128)
		ch = '?';
	glyph = font8x8_basic[ch];
	for (row = 0; row < 8; row++) {
		uint8_t bits = glyph[row];

		for (col = 0; col < 8; col++) {
			int px;
			int py;

			if (!(bits & (1U << col)))
				continue;
			for (py = 0; py < scale; py++) {
				for (px = 0; px < scale; px++) {
					int dx = x + col * scale + px;
					int dy = y + row * scale + py;

					if (dx < 0 || dy < 0 ||
					    dx >= (int)stride)
						continue;
					pixels[(size_t)dy * stride + dx] = color;
				}
			}
		}
	}
}

static void sdl_draw_text(uint32_t *pixels, uint32_t stride, int x, int y,
			  const char *text, int scale, uint32_t color)
{
	while (*text) {
		sdl_draw_glyph(pixels, stride, x, y, (unsigned char)*text,
			       scale, color);
		x += 8 * scale;
		text++;
	}
}

static void sdl_build_overlay(struct everlive_sdl *sdl, const char *line1,
			      const char *line2, const char *line3)
{
	const uint32_t bg = 0xb8000000U;
	const uint32_t fg = 0xffffffffU;
	const uint32_t accent = 0xff00ccffU;
	const uint32_t dim = 0xffa0a8b8U;
	const uint32_t width = 660;
	const uint32_t height = 104;
	const int scale = 2;
	uint32_t y;

	if (sdl->overlay_w != width || sdl->overlay_h != height ||
	    !sdl->overlay_pixels) {
		free(sdl->overlay_pixels);
		sdl->overlay_pixels = calloc((size_t)width * height,
					     sizeof(uint32_t));
		if (!sdl->overlay_pixels)
			return;
		sdl->overlay_w = width;
		sdl->overlay_h = height;
		if (sdl->overlay)
			SDL_DestroyTexture(sdl->overlay);
		sdl->overlay = SDL_CreateTexture(sdl->renderer,
						 SDL_PIXELFORMAT_ARGB8888,
						 SDL_TEXTUREACCESS_STREAMING,
						 width, height);
		if (sdl->overlay) {
			SDL_SetTextureBlendMode(sdl->overlay,
						SDL_BLENDMODE_BLEND);
		}
	}
	if (!sdl->overlay_pixels || !sdl->overlay)
		return;

	for (y = 0; y < height; y++)
		memset(sdl->overlay_pixels + (size_t)y * width, 0,
		       width * sizeof(uint32_t));

	/* Soft translucent card with a cyan accent bar. */
	for (y = 0; y < height; y++) {
		uint32_t x;

		for (x = 0; x < width; x++) {
			uint32_t color = bg;
			uint8_t alpha = 0xb8;

			if (x < 8 || y < 1 || y >= height - 1)
				color = accent;
			else if (alpha)
				color = (uint32_t)alpha << 24;
			sdl->overlay_pixels[(size_t)y * width + x] = color;
		}
	}

	sdl_draw_text(sdl->overlay_pixels, width, 20, 12, line1, scale, fg);
	sdl_draw_text(sdl->overlay_pixels, width, 20, 44, line2, scale, dim);
	sdl_draw_text(sdl->overlay_pixels, width, 20, 72, line3, scale, accent);

	SDL_UpdateTexture(sdl->overlay, NULL, sdl->overlay_pixels,
			  (int)(width * sizeof(uint32_t)));
}

static bool sdl_init(struct everlive_state *st, struct everlive_sdl *sdl)
{
	Uint32 flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
	Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
	int window_w = 1280;
	int window_h = 720;

	if (st->opt.vsync)
		renderer_flags |= SDL_RENDERER_PRESENTVSYNC;

	SDL_SetHint(SDL_HINT_APP_NAME, "EverLive");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "EverLive");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "HDMI Game Audio");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "game");

	if (SDL_Init(flags) < 0) {
		fprintf(stderr, "[gc570d WARN] SDL init failed: %s\n", SDL_GetError());
		return false;
	}

	sdl->window = SDL_CreateWindow(
		st->opt.title ? st->opt.title : "EverLive DUO",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		window_w, window_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
	if (!sdl->window) {
		fprintf(stderr, "[gc570d WARN] SDL window failed: %s\n", SDL_GetError());
		return false;
	}

	sdl->renderer = SDL_CreateRenderer(sdl->window, -1, renderer_flags);
	if (!sdl->renderer) {
		sdl->renderer = SDL_CreateRenderer(sdl->window, -1, 0);
		if (!sdl->renderer) {
			fprintf(stderr, "[gc570d WARN] SDL renderer failed: %s\n",
				SDL_GetError());
			return false;
		}
	}
	SDL_SetRenderDrawBlendMode(sdl->renderer, SDL_BLENDMODE_BLEND);

	if (st->opt.fullscreen) {
		SDL_SetWindowFullscreen(sdl->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
		sdl->fullscreen = true;
	}

	/* Open low-latency SDL2 desktop audio monitoring for the active HDMI input */
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
		SDL_AudioSpec want, have;

		memset(&want, 0, sizeof(want));
		want.freq = 48000;
		want.format = AUDIO_S16LSB;
		want.channels = 2;
		want.samples = 1024;
		sdl->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (sdl->audio_dev > 0) {
			SDL_PauseAudioDevice(sdl->audio_dev, 0);
			atomic_store_explicit(&st->audio_dev,
					      (unsigned int)sdl->audio_dev,
					      __ATOMIC_RELEASE);
			printf("SDL audio: opened %d Hz, %d ch, 16-bit\n",
			       have.freq, have.channels);
			fflush(stdout);
		} else {
			fprintf(stderr,
				"[gc570d WARN] SDL audio unavailable (%s); PipeWire capture nodes remain active\n",
				SDL_GetError());
		}
	}

	return true;
}

static void sdl_destroy(struct everlive_state *st, struct everlive_sdl *sdl)
{
	int i;

	atomic_store_explicit(&st->audio_dev, 0, __ATOMIC_RELEASE);
	if (sdl->audio_dev > 0) {
		SDL_CloseAudioDevice(sdl->audio_dev);
		sdl->audio_dev = 0;
	}
	for (i = 0; i < 2; i++) {
		if (sdl->texture[i])
			SDL_DestroyTexture(sdl->texture[i]);
		sdl->texture[i] = NULL;
	}
	if (sdl->overlay)
		SDL_DestroyTexture(sdl->overlay);
	sdl->overlay = NULL;
	free(sdl->overlay_pixels);
	sdl->overlay_pixels = NULL;
	if (sdl->renderer)
		SDL_DestroyRenderer(sdl->renderer);
	if (sdl->window)
		SDL_DestroyWindow(sdl->window);
	sdl->renderer = NULL;
	sdl->window = NULL;
	SDL_Quit();
}

static void sdl_update_texture(struct everlive_state *st,
			       struct everlive_sdl *sdl, int port,
			       const struct everlive_frame *frame)
{
	int want_scale;

	if (!sdl->renderer || !frame->pixels || !frame->width || !frame->height)
		return;

	want_scale = atomic_load_explicit(&st->scale, __ATOMIC_RELAXED);

	if (!sdl->texture[port] ||
	    sdl->texture_w[port] != frame->width ||
	    sdl->texture_h[port] != frame->height ||
	    sdl->texture_scale_mode[port] != want_scale) {
		if (sdl->texture[port])
			SDL_DestroyTexture(sdl->texture[port]);
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
			    want_scale == EVERLIVE_SCALE_INTEGER ? "0" : "1");
		sdl->texture[port] = SDL_CreateTexture(
			sdl->renderer, SDL_PIXELFORMAT_YUY2,
			SDL_TEXTUREACCESS_STREAMING,
			(int)frame->width, (int)frame->height);
		if (sdl->texture[port]) {
			SDL_SetTextureBlendMode(sdl->texture[port],
						SDL_BLENDMODE_NONE);
			sdl->texture_w[port] = frame->width;
			sdl->texture_h[port] = frame->height;
			sdl->texture_scale_mode[port] = want_scale;
		} else {
			sdl->texture_w[port] = 0;
			sdl->texture_h[port] = 0;
		}
	}

	if (!sdl->texture[port])
		return;

	SDL_UpdateTexture(sdl->texture[port], NULL, frame->pixels,
			  (int)(frame->width * 2));
}

static SDL_Rect sdl_fit_rect(int out_w, int out_h, uint32_t src_w,
			     uint32_t src_h, int aspect_mode, int scale_mode)
{
	SDL_Rect dst = { 0, 0, out_w, out_h };
	int fit_w = out_w;
	int fit_h = out_h;
	uint64_t ar_num = src_w;
	uint64_t ar_den = src_h;
	bool anamorphic_4_3 = (src_w == 720 && (src_h == 480 || src_h == 576));

	if (!src_w || !src_h)
		return dst;

	if (aspect_mode == EVERLIVE_ASPECT_STRETCH)
		return dst;

	if (scale_mode == EVERLIVE_SCALE_INTEGER &&
	    aspect_mode == EVERLIVE_ASPECT_AUTO &&
	    !anamorphic_4_3 &&
	    out_w >= (int)src_w && out_h >= (int)src_h) {
		int scale = out_w / (int)src_w;
		int sy = out_h / (int)src_h;

		if (sy < scale)
			scale = sy;
		if (scale < 1)
			scale = 1;
		fit_w = (int)src_w * scale;
		fit_h = (int)src_h * scale;
		dst.x = ((out_w - fit_w) / 2) & ~1;
		dst.y = (out_h - fit_h) / 2;
		dst.w = fit_w;
		dst.h = fit_h;
		return dst;
	}

	if (aspect_mode == EVERLIVE_ASPECT_16_9) {
		ar_num = 16;
		ar_den = 9;
	} else if (aspect_mode == EVERLIVE_ASPECT_4_3 ||
		   (aspect_mode == EVERLIVE_ASPECT_AUTO && anamorphic_4_3)) {
		ar_num = 4;
		ar_den = 3;
	}

	if ((uint64_t)out_w * ar_den <= (uint64_t)out_h * ar_num)
		fit_h = (int)((uint64_t)out_w * ar_den / ar_num);
	else
		fit_w = (int)((uint64_t)out_h * ar_num / ar_den);

	dst.x = (out_w - fit_w) / 2;
	dst.y = (out_h - fit_h) / 2;
	dst.w = fit_w;
	dst.h = fit_h;
	return dst;
}

static void sdl_render_frame(struct everlive_state *st, struct everlive_sdl *sdl)
{
	int port = atomic_load_explicit(&st->port, __ATOMIC_RELAXED);
	int aspect_mode = atomic_load_explicit(&st->aspect, __ATOMIC_RELAXED);
	int scale_mode = atomic_load_explicit(&st->scale, __ATOMIC_RELAXED);
	int out_w = 1280;
	int out_h = 720;
	SDL_Rect dst;
	SDL_Rect pip_dst;

	SDL_GetRendererOutputSize(sdl->renderer, &out_w, &out_h);
	SDL_SetRenderDrawColor(sdl->renderer, 5, 7, 11, 255);
	SDL_RenderClear(sdl->renderer);

	if (sdl->texture[port]) {
		dst = sdl_fit_rect(out_w, out_h, st->frame[port].width,
				   st->frame[port].height, aspect_mode,
				   scale_mode);
		SDL_RenderCopy(sdl->renderer, sdl->texture[port], NULL, &dst);
	}

	if (atomic_load_explicit(&st->pip, __ATOMIC_RELAXED)) {
		int other = port ? 0 : 1;

		if (sdl->texture[other]) {
			int pip_w = out_w / 4;
			int pip_h = out_h / 4;

			pip_dst.x = out_w - pip_w - 24;
			pip_dst.y = out_h - pip_h - 24;
			pip_dst.w = pip_w;
			pip_dst.h = pip_h;
			SDL_SetRenderDrawColor(sdl->renderer, 0, 204, 255, 255);
			SDL_RenderDrawRect(sdl->renderer, &pip_dst);
			SDL_RenderCopy(sdl->renderer, sdl->texture[other], NULL,
				       &pip_dst);
		}
	}

	if (atomic_load_explicit(&st->osd, __ATOMIC_RELAXED) && sdl->overlay) {
		const int margin = 24;
		SDL_Rect overlay_dst = {
			.x = margin,
			.y = margin,
			.w = (int)sdl->overlay_w,
			.h = (int)sdl->overlay_h,
		};
		int pos = atomic_load_explicit(&st->osd_pos, __ATOMIC_RELAXED);

		if (pos == EVERLIVE_OSD_BR) {
			overlay_dst.x = out_w - overlay_dst.w - margin;
			overlay_dst.y = out_h - overlay_dst.h - margin;
		} else if (pos == EVERLIVE_OSD_BL) {
			overlay_dst.x = margin;
			overlay_dst.y = out_h - overlay_dst.h - margin;
		} else if (pos != EVERLIVE_OSD_TL) {
			overlay_dst.x = out_w - overlay_dst.w - margin;
			overlay_dst.y = margin;
		}
		SDL_RenderCopy(sdl->renderer, sdl->overlay, NULL, &overlay_dst);
	}

	SDL_RenderPresent(sdl->renderer);
}

static bool sdl_handle_events(struct everlive_state *st,
			      struct everlive_sdl *sdl)
{
	SDL_Event event;

	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_QUIT)
			return false;
		if (event.type != SDL_KEYDOWN || event.key.repeat)
			continue;

		switch (event.key.keysym.scancode) {
		case SDL_SCANCODE_1:
			gc570d_everlive_set_port(0);
			sdl->overlay_built_ns = 0;
			break;
		case SDL_SCANCODE_2:
			gc570d_everlive_set_port(1);
			sdl->overlay_built_ns = 0;
			break;
		case SDL_SCANCODE_P:
			atomic_store_explicit(&st->pip,
				!atomic_load_explicit(&st->pip, __ATOMIC_RELAXED),
				__ATOMIC_RELAXED);
			break;
		case SDL_SCANCODE_O:
		case SDL_SCANCODE_TAB:
			atomic_store_explicit(&st->osd,
				!atomic_load_explicit(&st->osd, __ATOMIC_RELAXED),
				__ATOMIC_RELAXED);
			break;
		case SDL_SCANCODE_C: {
			int next_pos = (atomic_load_explicit(&st->osd_pos,
							     __ATOMIC_RELAXED) + 1) & 3;
			atomic_store_explicit(&st->osd_pos, next_pos,
					      __ATOMIC_RELAXED);
			break;
		}
		case SDL_SCANCODE_A: {
			int next_aspect = (atomic_load_explicit(&st->aspect,
								__ATOMIC_RELAXED) + 1) & 3;
			atomic_store_explicit(&st->aspect, next_aspect,
					      __ATOMIC_RELAXED);
			sdl->overlay_built_ns = 0;
			break;
		}
		case SDL_SCANCODE_L: {
			int next_scale = atomic_load_explicit(&st->scale,
							      __ATOMIC_RELAXED) ==
					 EVERLIVE_SCALE_INTEGER ?
					 EVERLIVE_SCALE_FIT :
					 EVERLIVE_SCALE_INTEGER;
			atomic_store_explicit(&st->scale, next_scale,
					      __ATOMIC_RELAXED);
			sdl->overlay_built_ns = 0;
			break;
		}
		case SDL_SCANCODE_M: {
			bool next_mute = !atomic_load_explicit(&st->muted,
							       __ATOMIC_RELAXED);
			gc570d_everlive_set_mute(next_mute);
			sdl->overlay_built_ns = 0;
			break;
		}
		case SDL_SCANCODE_UP:
		case SDL_SCANCODE_EQUALS:
		case SDL_SCANCODE_KP_PLUS: {
			int v = atomic_load_explicit(&st->volume_pct,
						     __ATOMIC_RELAXED) + 5;
			if (v > 150)
				v = 150;
			atomic_store_explicit(&st->muted, false, __ATOMIC_RELAXED);
			gc570d_everlive_set_volume(v);
			sdl->overlay_built_ns = 0;
			break;
		}
		case SDL_SCANCODE_DOWN:
		case SDL_SCANCODE_MINUS:
		case SDL_SCANCODE_KP_MINUS: {
			int v = atomic_load_explicit(&st->volume_pct,
						     __ATOMIC_RELAXED) - 5;
			if (v < 0)
				v = 0;
			gc570d_everlive_set_volume(v);
			sdl->overlay_built_ns = 0;
			break;
		}
		case SDL_SCANCODE_F:
		case SDL_SCANCODE_F11:
			sdl->fullscreen = !sdl->fullscreen;
			SDL_SetWindowFullscreen(sdl->window,
				sdl->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
			break;
		case SDL_SCANCODE_ESCAPE:
		case SDL_SCANCODE_Q:
			return false;
		default:
			break;
		}
	}
	return true;
}
#endif /* HAVE_SDL2 */

static void everlive_log_status(struct everlive_state *st, uint64_t now)
{
	struct gc570d_dev *gc = READ_ONCE(st->gc);
	int port = atomic_load_explicit(&st->port, __ATOMIC_RELAXED);
	unsigned int src_w = shm_port_source_width(st, port);
	unsigned int src_h = shm_port_source_height(st, port);
	unsigned int src_fps = shm_port_source_fps(st, port);
	unsigned int out_w = st->frame[port].width ? st->frame[port].width :
		(port == 0 && gc ? gc->video0_width : GC570D_WIDTH);
	unsigned int out_h = st->frame[port].height ? st->frame[port].height :
		(port == 0 && gc ? gc->video0_height : GC570D_HEIGHT);
	unsigned int channels = shm_port_channels(st, port);
	unsigned int rate = shm_port_rate(st, port);
	unsigned int profile = shm_port_profile(st, port);
	double fps = 0.0;
	uint64_t frames;

	if (st->last_fps_ns && now > st->last_fps_ns &&
	    st->last_seen[port] >= st->last_fps_frames) {
		fps = (double)(st->last_seen[port] - st->last_fps_frames) *
			1000000000.0 / (double)(now - st->last_fps_ns);
	}
	st->last_fps_ns = now;
	st->last_fps_frames = st->last_seen[port];

	frames = st->last_seen[port];
	printf("[VIDEO] Capturing %ux%up @ %u.%02u Hz (VIP %u Hz, %s, pclk 0 kHz) -> %ux%u\n",
	       src_w, src_h, src_fps, 0U, src_fps,
	       shm_profile_name(profile), out_w, out_h);
	printf("[STREAM] %ux%up @ %u.%02u Hz -> %ux%u | FPS: %.1f | Frames: %llu\n",
	       src_w, src_h, src_fps, 0U, out_w, out_h, fps,
	       (unsigned long long)frames);
	printf("[AUDIO] %u Hz %u ch 16-bit%s\n", rate, channels,
	       gc && ((port == 0 && gc->audio0_recovering) ||
		      (port != 0 && gc->audio_recovering)) ?
	       " (recovering)" : "");
	if (gc && port == 0 && READ_ONCE(gc->video0_no_signal))
		printf("[EVERLIVE DUO] HDMI IN 1 has no signal; showing the built-in placeholder\n");
	if (gc && port == 1 && READ_ONCE(gc->video_no_signal))
		printf("[EVERLIVE DUO] HDMI IN 2 has no signal; showing the built-in placeholder\n");
	fflush(stdout);
}

static void *everlive_thread(void *arg)
{
	struct everlive_state *st = arg;
#ifdef HAVE_SDL2
	struct everlive_sdl sdl;
	bool sdl_ready = false;
#endif
	bool keep_running = true;
	uint64_t next_status_ns = now_ns() + EVERLIVE_STATUS_NS;
	int i;

	for (i = 0; i < 2; i++) {
		snprintf(st->v4l2[i].path, sizeof(st->v4l2[i].path), "%s",
			 st->opt.v4l2[i] ? st->opt.v4l2[i] : "");
		st->v4l2[i].fd = -1;
	}

#ifdef HAVE_SDL2
	memset(&sdl, 0, sizeof(sdl));
	if (st->opt.enable_sdl && !st->opt.headless) {
		sdl_ready = sdl_init(st, &sdl);
		if (!sdl_ready)
			fprintf(stderr,
				"[gc570d WARN] EverLive SDL window unavailable; continuing without a preview\n");
	}
#endif

	while (!atomic_load_explicit(&st->quit, __ATOMIC_RELAXED)) {
		uint64_t now = now_ns();
		int active_port = atomic_load_explicit(&st->port, __ATOMIC_RELAXED);
		bool updated[2] = { false, false };

		for (i = 0; i < 2; i++) {
			if (everlive_snapshot(st, i))
				updated[i] = true;
		}
		/* /dev/video10 follows the selected DUO input; /dev/video11 is dedicated to HDMI 2 */
		if (updated[active_port])
			everlive_v4l2_write(&st->v4l2[0], &st->frame[active_port], now);
		if (updated[1])
			everlive_v4l2_write(&st->v4l2[1], &st->frame[1], now);

#ifdef HAVE_SDL2
		if (sdl_ready) {
			for (i = 0; i < 2; i++) {
				if (updated[i])
					sdl_update_texture(st, &sdl, i, &st->frame[i]);
			}

			if (now - sdl.overlay_built_ns >= EVERLIVE_STATUS_NS) {
				int port = atomic_load_explicit(&st->port,
								__ATOMIC_RELAXED);
				int aspect = atomic_load_explicit(&st->aspect,
								  __ATOMIC_RELAXED);
				int scale = atomic_load_explicit(&st->scale,
								 __ATOMIC_RELAXED);
				int vol = atomic_load_explicit(&st->volume_pct,
							       __ATOMIC_RELAXED);
				bool muted = atomic_load_explicit(&st->muted,
								  __ATOMIC_RELAXED);
				char line1[96];
				char line2[96];
				char line3[96];

				snprintf(line1, sizeof(line1),
					 "EverLive DUO - %s  (%s%s)",
					 shm_port_name(port),
					 aspect_mode_label(aspect),
					 scale == EVERLIVE_SCALE_INTEGER ?
					 " Integer" : "");
				snprintf(line2, sizeof(line2),
					 "%ux%u -> %ux%u YUYV  |  %u Hz %u ch LPCM  |  Vol %s%d%%",
					 shm_port_source_width(st, port),
					 shm_port_source_height(st, port),
					 st->frame[port].width ? st->frame[port].width : 1920,
					 st->frame[port].height ? st->frame[port].height : 1080,
					 shm_port_rate(st, port),
					 shm_port_channels(st, port),
					 muted ? "Muted/" : "", vol);
				snprintf(line3, sizeof(line3),
					 "1/2 Input  P PiP  A Aspect  L Scale  M Mute  Tab OSD  F Full");
				sdl_build_overlay(&sdl, line1, line2, line3);
				sdl.overlay_built_ns = now;
			}

			if (!sdl_handle_events(st, &sdl)) {
				keep_running = false;
				break;
			}
			sdl_render_frame(st, &sdl);
		}
#else
		(void)keep_running;
#endif

		if (!keep_running)
			break;

		if (now >= next_status_ns) {
			everlive_log_status(st, now);
			next_status_ns = now + EVERLIVE_STATUS_NS;
		}

		usleep(2000);
	}

	if (!keep_running)
		atomic_store_explicit(&st->quit, true, __ATOMIC_RELAXED);

	for (i = 0; i < 2; i++)
		everlive_v4l2_close(&st->v4l2[i]);
#ifdef HAVE_SDL2
	if (sdl_ready)
		sdl_destroy(st, &sdl);
#endif
	atomic_store_explicit(&st->running, false, __ATOMIC_RELEASE);
	return NULL;
}

int gc570d_everlive_start(struct gc570d_shm_region *shm,
			  struct gc570d_dev *gc,
			  const struct gc570d_everlive_options *options)
{
	int i;
	int vol;

	if (!shm || !options)
		return -EINVAL;
	if (!options->enable_sdl && !options->v4l2[0] && !options->v4l2[1])
		return 0;
	if (atomic_load_explicit(&g_everlive.running, __ATOMIC_ACQUIRE))
		return -EBUSY;

	memset(&g_everlive, 0, sizeof(g_everlive));
	g_everlive.shm = shm;
	g_everlive.gc = gc;
	g_everlive.opt = *options;
	vol = options->volume_pct;
	if (vol < 0)
		vol = 0;
	if (vol > 150)
		vol = 150;
	atomic_init(&g_everlive.started, false);
	atomic_init(&g_everlive.running, false);
	atomic_init(&g_everlive.quit, false);
	atomic_init(&g_everlive.port, options->port ? 1 : 0);
	atomic_init(&g_everlive.pip, options->pip);
	atomic_init(&g_everlive.osd, options->osd);
	atomic_init(&g_everlive.osd_pos, parse_osd_pos(options->osd_pos));
	atomic_init(&g_everlive.aspect, parse_aspect_mode(options->aspect));
	atomic_init(&g_everlive.scale, parse_scale_mode(options->scale));
	atomic_init(&g_everlive.volume_pct, vol);
	atomic_init(&g_everlive.muted, options->muted);
#ifdef HAVE_SDL2
	atomic_init(&g_everlive.audio_dev, 0);
#endif

	for (i = 0; i < 2; i++) {
		g_everlive.frame[i].pixels = calloc(1, EVERLIVE_FRAME_BYTES);
		if (!g_everlive.frame[i].pixels) {
			int j;

			for (j = 0; j < i; j++) {
				free(g_everlive.frame[j].pixels);
				g_everlive.frame[j].pixels = NULL;
			}
			return -ENOMEM;
		}
	}

	if (pthread_create(&g_everlive.thread, NULL, everlive_thread,
			   &g_everlive) != 0) {
		for (i = 0; i < 2; i++) {
			free(g_everlive.frame[i].pixels);
			g_everlive.frame[i].pixels = NULL;
		}
		return -EIO;
	}

	atomic_store_explicit(&g_everlive.started, true, __ATOMIC_RELEASE);
	atomic_store_explicit(&g_everlive.running, true, __ATOMIC_RELEASE);
	fprintf(stderr,
		"[gc570d INFO] EverLive frontend started (SDL %s, V4L2 %s%s%s)\n",
		options->enable_sdl && !options->headless ? "yes" : "headless",
		options->v4l2[0] ? options->v4l2[0] : "off",
		options->v4l2[1] ? ", " : "",
		options->v4l2[1] ? options->v4l2[1] : "");
	return 0;
}

void gc570d_everlive_stop(void)
{
	int i;

	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;

	atomic_store_explicit(&g_everlive.quit, true, __ATOMIC_RELAXED);
	pthread_join(g_everlive.thread, NULL);
	atomic_store_explicit(&g_everlive.started, false, __ATOMIC_RELEASE);
	atomic_store_explicit(&g_everlive.running, false, __ATOMIC_RELEASE);

	for (i = 0; i < 2; i++) {
		free(g_everlive.frame[i].pixels);
		g_everlive.frame[i].pixels = NULL;
	}
}

void gc570d_everlive_set_dev(struct gc570d_dev *gc)
{
	WRITE_ONCE(g_everlive.gc, gc);
}

bool gc570d_everlive_quit_requested(void)
{
	return atomic_load_explicit(&g_everlive.quit, __ATOMIC_RELAXED);
}

void gc570d_everlive_set_port(int port)
{
	int next = port ? 1 : 0;

	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	if (atomic_exchange_explicit(&g_everlive.port, next, __ATOMIC_RELAXED) != next) {
		g_everlive.last_fps_ns = 0;
#ifdef HAVE_SDL2
		unsigned int dev = atomic_load_explicit(&g_everlive.audio_dev,
							__ATOMIC_ACQUIRE);
		if (dev > 0)
			SDL_ClearQueuedAudio((SDL_AudioDeviceID)dev);
#endif
	}
}

void gc570d_everlive_set_osd(bool enabled)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	atomic_store_explicit(&g_everlive.osd, enabled, __ATOMIC_RELAXED);
}

void gc570d_everlive_set_osd_pos(const char *pos)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	atomic_store_explicit(&g_everlive.osd_pos, parse_osd_pos(pos),
			      __ATOMIC_RELAXED);
}

void gc570d_everlive_set_pip(bool enabled)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	atomic_store_explicit(&g_everlive.pip, enabled, __ATOMIC_RELAXED);
}

void gc570d_everlive_set_aspect(const char *aspect)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	atomic_store_explicit(&g_everlive.aspect, parse_aspect_mode(aspect),
			      __ATOMIC_RELAXED);
}

void gc570d_everlive_set_scale(const char *scale)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	atomic_store_explicit(&g_everlive.scale, parse_scale_mode(scale),
			      __ATOMIC_RELAXED);
}

void gc570d_everlive_set_volume(int volume_pct)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	if (volume_pct < 0)
		volume_pct = 0;
	if (volume_pct > 150)
		volume_pct = 150;
	atomic_store_explicit(&g_everlive.volume_pct, volume_pct,
			      __ATOMIC_RELAXED);
}

void gc570d_everlive_set_mute(bool muted)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	atomic_store_explicit(&g_everlive.muted, muted, __ATOMIC_RELAXED);
#ifdef HAVE_SDL2
	if (muted) {
		unsigned int dev = atomic_load_explicit(&g_everlive.audio_dev,
							__ATOMIC_ACQUIRE);
		if (dev > 0)
			SDL_ClearQueuedAudio((SDL_AudioDeviceID)dev);
	}
#endif
}

void gc570d_everlive_push_audio(int port, const uint8_t *pcm, size_t bytes)
{
#ifdef HAVE_SDL2
	unsigned int dev;
	int active_port;
	int vol;
	bool muted;
	int16_t scaled[2048];
	const int16_t *src;
	size_t samples;
	size_t i;

	if (!pcm || !bytes)
		return;
	if (!atomic_load_explicit(&g_everlive.running, __ATOMIC_ACQUIRE))
		return;

	dev = atomic_load_explicit(&g_everlive.audio_dev, __ATOMIC_ACQUIRE);
	if (!dev)
		return;

	active_port = atomic_load_explicit(&g_everlive.port, __ATOMIC_RELAXED);
	if (port != active_port)
		return;

	muted = atomic_load_explicit(&g_everlive.muted, __ATOMIC_RELAXED);
	vol = atomic_load_explicit(&g_everlive.volume_pct, __ATOMIC_RELAXED);
	if (muted || vol <= 0)
		return;

	/* Bound SDL audio latency to ~120 ms (48000 Hz * 4 bytes * 0.12s = 23040 bytes) */
	if (SDL_GetQueuedAudioSize((SDL_AudioDeviceID)dev) > 23040U)
		SDL_ClearQueuedAudio((SDL_AudioDeviceID)dev);

	if (vol == 100) {
		SDL_QueueAudio((SDL_AudioDeviceID)dev, pcm, (Uint32)bytes);
		return;
	}

	if (bytes > sizeof(scaled))
		bytes = sizeof(scaled);
	samples = bytes / sizeof(int16_t);
	src = (const int16_t *)pcm;
	for (i = 0; i < samples; i++) {
		int32_t s = ((int32_t)src[i] * vol) / 100;

		if (s > 32767)
			s = 32767;
		else if (s < -32768)
			s = -32768;
		scaled[i] = (int16_t)s;
	}
	SDL_QueueAudio((SDL_AudioDeviceID)dev, scaled,
		       (Uint32)(samples * sizeof(int16_t)));
#else
	(void)port;
	(void)pcm;
	(void)bytes;
#endif
}

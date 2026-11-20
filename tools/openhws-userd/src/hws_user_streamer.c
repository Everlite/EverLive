// SPDX-License-Identifier: GPL-2.0-only
/*
 * hws_user_streamer.c — EverLive engine for the HWS UHD capture family
 * (ACASIS VS2583 / AVMatrix VC12-4K) under vfio-pci.
 *
 *   capture thread  (hws_capture.c)  interrupts, zero-copy slots, input, audio
 *   output thread                    frame store (preload) + v4l2loopback
 *   main thread                      SDL window or headless loop, control socket
 *
 * The command line accepts the GC573 streamer's options so the Console Hub
 * can launch either engine the same way; options for hardware this card does
 * not have (LEDs, EDID, HDR, frame generation) are accepted and ignored.
 */

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include "everlive_osd.h"
#endif
#ifdef HAVE_X11
#include <X11/Xlib.h>
#endif

#include "hws_audio_tap.h"
#include "hws_camera.h"
#include "hws_capture.h"
#include "hws_font8x8.h"
#include "hws_ipc.h"
#include "hws_log.h"
#include "hws_nosignal.h"
#include "hws_user_regs.h"
#include "hws_v4l2out.h"

#ifndef HWS_ENGINE_VERSION
#define HWS_ENGINE_VERSION "1.0.0"
#endif

#define WINDOW_TITLE		"EverLive — HWS UHD Capture"
#define DEFAULT_WINDOW_W	1280
#define DEFAULT_WINDOW_H	720
#define NO_SIGNAL_PERIOD_US	33333	/* standby card at ~30 fps */
#define AUDIO_MAX_QUEUE_MS	80	/* drop playback backlog beyond this */

enum { CONSUMER_PRESENT = 0, CONSUMER_OUTPUT = 1 };
enum aspect_mode { ASPECT_AUTO, ASPECT_16_9, ASPECT_4_3, ASPECT_STRETCH };
static const char *const kAspectNames[] = { "auto", "16:9", "4:3", "stretch" };
static const char *const kOsdPosNames[] = { "top-right", "bottom-right", "bottom-left", "top-left" };

/* ---- Settings (main thread unless noted) ------------------------------ */

static volatile sig_atomic_t g_quit;
static struct hws_capture *g_cap;
static struct hws_no_signal g_nosignal = { .lock = PTHREAD_MUTEX_INITIALIZER };

static bool g_headless;
static bool g_fullscreen;
static bool g_vsync;
static bool g_osd = true;
static int g_osd_pos;
static int g_osd_mode = HWS_IPC_OSD_MODE_CARD;			/* enum hws_ipc_osd_pos */
static int g_aspect = ASPECT_AUTO;
static bool g_scale_integer;
static bool g_streamer_mode;
static unsigned long g_parent_winid;
static bool g_play_audio = true;
static char g_toast[128];
static uint64_t g_toast_until_ms;

static _Atomic float g_gain = 1.0f;	/* read by the capture thread */
static atomic_bool g_muted;

/* v4l2loopback: owned by the output thread, reconfigured through these. */
static pthread_mutex_t g_v4l2_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_v4l2_path[64];
static bool g_v4l2_enabled;
static bool g_v4l2_dirty;
static struct hws_v4l2out g_v4l2 = { .fd = -1 };

static bool g_camera = true;

#ifdef HAVE_SDL2
static SDL_AudioDeviceID g_audio_dev;
static struct everlive_osd_card g_card;
static struct everlive_osd_popup g_popup;
static bool g_popup_open;
static bool g_popup_dragging;
static bool g_popup_dirty;
static bool g_osd_font_attempted;
static float g_popup_scale;
static int g_popup_volume = 100;
static SDL_Rect g_card_dest;
static SDL_Rect g_popup_dest;
#endif

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

static uint64_t now_ms(void)
{
	return now_us() / 1000;
}

static void on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

static void toast(const char *msg)
{
	snprintf(g_toast, sizeof(g_toast), "%s", msg);
	g_toast_until_ms = now_ms() + 2500;
	printf("[HWS-USER] %s\n", msg);
}

/* ---- Audio (capture thread) ------------------------------------------- */

static void audio_packet(const int16_t *pcm, size_t bytes, void *user)
{
	int16_t buf[HWS_AUDIO_PACKET_BYTES / 2];
	size_t n = bytes / 2;
	float gain = atomic_load(&g_gain);

	(void)user;
	if (n > sizeof(buf) / 2)
		n = sizeof(buf) / 2;
	if (atomic_load(&g_muted)) {
		memset(buf, 0, n * 2);
	} else if (gain == 1.0f) {
		memcpy(buf, pcm, n * 2);
	} else {
		for (size_t i = 0; i < n; i++) {
			float v = pcm[i] * gain;

			buf[i] = v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int16_t)v;
		}
	}
	if (hws_audio_tap_active())
		hws_audio_tap_write(buf, n * 2, 16);
#ifdef HAVE_SDL2
	if (g_audio_dev) {
		Uint32 max = HWS_AUDIO_RATE * 4 * AUDIO_MAX_QUEUE_MS / 1000;

		if (SDL_GetQueuedAudioSize(g_audio_dev) > max)
			SDL_ClearQueuedAudio(g_audio_dev);
		SDL_QueueAudio(g_audio_dev, buf, (Uint32)(n * 2));
	}
#endif
}

/* ---- Output thread: frame store + v4l2loopback ------------------------ */

static void output_apply_v4l2_config(void)
{
	char path[64];
	bool enabled;

	pthread_mutex_lock(&g_v4l2_lock);
	if (!g_v4l2_dirty) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return;
	}
	g_v4l2_dirty = false;
	enabled = g_v4l2_enabled;
	snprintf(path, sizeof(path), "%s", g_v4l2_path);
	pthread_mutex_unlock(&g_v4l2_lock);

	hws_v4l2out_close(&g_v4l2);
	if (enabled && path[0])
		hws_v4l2out_open(&g_v4l2, path);
}

static void output_publish(const uint8_t *frame, size_t bytes, unsigned int w,
			   unsigned int h, bool no_signal)
{
	if (hws_camera_active())
		hws_camera_publish(frame, bytes, w, h, no_signal);
	if (g_v4l2.fd >= 0)
		hws_v4l2out_write(&g_v4l2, frame, w, h);
}

static void *output_thread(void *arg)
{
	uint64_t last_seq = 0, last_standby = 0;

	(void)arg;
	while (!g_quit) {
		struct hws_frame_ref f;

		output_apply_v4l2_config();
		if (hws_capture_acquire(g_cap, CONSUMER_OUTPUT, last_seq, 40, &f) == 0) {
			last_seq = f.seq;
			output_publish(f.data, f.bytes, f.width, f.height, false);
			hws_capture_release(g_cap, CONSUMER_OUTPUT);
			continue;
		}
		/* No live frame: keep consumers fed with the standby card. */
		if (now_us() - last_standby >= NO_SIGNAL_PERIOD_US) {
			struct hws_capture_status st;
			const void *pix;
			size_t bytes;

			hws_capture_get_status(g_cap, &st);
			if (st.signal)
				continue;
			last_standby = now_us();
			if (hws_no_signal_ensure(&g_nosignal, HWS_NO_SIGNAL_DEFAULT_WIDTH,
						 HWS_NO_SIGNAL_DEFAULT_HEIGHT) < 0)
				continue;
			pix = hws_no_signal_frame(&g_nosignal, &bytes);
			if (pix)
				output_publish(pix, bytes, hws_no_signal_width(&g_nosignal),
					       hws_no_signal_height(&g_nosignal), true);
		}
	}
	hws_capture_release(g_cap, CONSUMER_OUTPUT);
	return NULL;
}

/* ---- Control socket ---------------------------------------------------- */

static bool g_mode_switch;	/* leave the current main loop to switch mode */

static void apply_cmd(const struct hws_ipc_cmd *cmd)
{
	char msg[96];

	switch (cmd->kind) {
	case HWS_IPC_CMD_AUDIO_GAIN:
		atomic_store(&g_gain, cmd->f < 0.f ? 0.f : cmd->f > 1.5f ? 1.5f : cmd->f);
		break;
	case HWS_IPC_CMD_AUDIO_MUTE:
		atomic_store(&g_muted, cmd->a != 0);
		break;
	case HWS_IPC_CMD_AUDIO_CHANNELS:
		break;	/* stereo is the only layout */
	case HWS_IPC_CMD_OSD_ENABLE:
		g_osd = cmd->a != 0;
		break;
	case HWS_IPC_CMD_OSD_POSITION:
		g_osd_pos = cmd->a & 3;
		break;
	case HWS_IPC_CMD_OSD_MODE:
		g_osd_mode = cmd->a & 3;
		break;
	case HWS_IPC_CMD_ASPECT:
		g_aspect = cmd->a & 3;
		break;
	case HWS_IPC_CMD_SCALE:
		g_scale_integer = cmd->a != 0;
		break;
	case HWS_IPC_CMD_MODE: {
		bool headless = cmd->a == HWS_IPC_MODE_HEADLESS;

		if (cmd->path[0])
			g_parent_winid = strtoul(cmd->path, NULL, 0);
		if (headless != g_headless || cmd->path[0]) {
			g_headless = headless;
			g_mode_switch = true;
		}
		break;
	}
	case HWS_IPC_CMD_V4L2:
		pthread_mutex_lock(&g_v4l2_lock);
		g_v4l2_enabled = cmd->a != 0;
		if (cmd->path[0])
			snprintf(g_v4l2_path, sizeof(g_v4l2_path), "%s", cmd->path);
		else if (!g_v4l2_path[0])
			snprintf(g_v4l2_path, sizeof(g_v4l2_path), "/dev/video10");
		g_v4l2_dirty = true;
		pthread_mutex_unlock(&g_v4l2_lock);
		snprintf(msg, sizeof(msg), "Virtual camera %s", g_v4l2_enabled ? "on" : "off");
		toast(msg);
		break;
	case HWS_IPC_CMD_TOAST:
		toast(cmd->path);
		break;
	default:
		break;
	}
}

static void poll_ipc(void)
{
	struct hws_ipc_cmd cmd;

	while (hws_ipc_poll_cmd(&cmd))
		apply_cmd(&cmd);
}

/*
 * Status lines for the Console Hub, which follows a local engine through its
 * log (~/.config/everlive/streamer.log).  Same formats as the other EverLive
 * engines: a [VIDEO] line on lock, a [STREAM] line every second, an [AUDIO]
 * line while HDMI audio flows, and "HDMI signal lost" / "Waiting for HDMI
 * signal" without a picture.  Called once per second.
 */
static void log_status(void)
{
	static bool was_live, announced_wait;
	static unsigned int live_w, live_h;
	static uint64_t last_audio_packets;
	struct hws_capture_status cs;

	static uint64_t first_us;

	if (!first_us)
		first_us = now_us();
	hws_capture_get_status(g_cap, &cs);
	if (!cs.signal) {
		/* Capture needs a moment to deliver the first frame after start. */
		if (!was_live && !announced_wait && now_us() - first_us < 2000000)
			return;
		if (was_live)
			printf("[HWS-USER] HDMI signal lost\n");
		else if (!announced_wait)
			printf("[HWS-USER] Waiting for HDMI signal%s\n",
			       cs.present && cs.interlaced ? " (interlaced input is not supported)" :
			       cs.hdcp ? " (HDCP: the card withholds video)" : "");
		announced_wait = true;
		was_live = false;
		fflush(stdout);
		return;
	}
	if (!was_live || cs.width != live_w || cs.height != live_h) {
		printf("[VIDEO] Capturing %ux%up @ %u.00 Hz (HWS UHD, YCbCr 4:2:2, pclk 0 kHz) -> YUYV zero-copy\n",
		       cs.width, cs.height, cs.input_fps);
		was_live = true;
		announced_wait = false;
		live_w = cs.width;
		live_h = cs.height;
	}
	if (cs.fps > 0.5) {
		/* Report the card's rate register unless the measurement shows a
		 * fractional mode (59.94 etc.); per-second jitter is not news. */
		double hz = (cs.input_fps && fabs(cs.fps - cs.input_fps) < 0.05) ?
			    (double)cs.input_fps : cs.fps;
		unsigned int hz100 = (unsigned int)lrint(hz * 100.0);

		printf("[STREAM] %ux%up @ %u.%02u Hz -> %ux%u | FPS: %.1f | Frames: %llu | preview drops: %llu\n",
		       cs.width, cs.height, hz100 / 100, hz100 % 100, cs.width, cs.height, cs.fps,
		       (unsigned long long)cs.frames, (unsigned long long)cs.dropped);
	}
	if (cs.audio_packets != last_audio_packets)
		printf("[AUDIO] %d Hz %d ch 16-bit\n", HWS_AUDIO_RATE, HWS_AUDIO_CHANNELS);
	last_audio_packets = cs.audio_packets;
	fflush(stdout);
}

static void publish_stats(int win_w, int win_h)
{
	struct hws_capture_status cs;
	struct hws_stats st;

	(void)win_w;
	(void)win_h;
	hws_capture_get_status(g_cap, &cs);
	memset(&st, 0, sizeof(st));
	st.timestamp_ms = now_ms();
	st.headless = g_headless;
	st.streaming = cs.signal;
	st.signal_locked = cs.signal;
	st.width = cs.width;
	st.height = cs.height;
	st.interlaced = cs.interlaced;
	st.rate_x100 = cs.signal && cs.fps > 0 ? (uint32_t)lrint(cs.fps * 100.0) : cs.input_fps * 100;
	st.output_width = cs.signal ? cs.width : 0;
	st.output_height = cs.signal ? cs.height : 0;
	st.fps = cs.fps;
	st.frames = cs.frames;
	st.preview_dropped = cs.dropped;
	st.ring_buffers = cs.nslots;
	st.ring_channels = 4;	/* quarters per frame */
	snprintf(st.audio_state, sizeof(st.audio_state), "%s",
		 atomic_load(&g_muted) ? "muted" : cs.audio_packets ? "lpcm" : "none");
	snprintf(st.audio_label, sizeof(st.audio_label), "L-PCM 48kHz stereo");
	st.audio_rate = HWS_AUDIO_RATE;
	st.audio_channels = HWS_AUDIO_CHANNELS;
	st.audio_out_channels = HWS_AUDIO_CHANNELS;
	snprintf(st.audio_layout, sizeof(st.audio_layout), "stereo");
	st.audio_bits = 16;
	st.audio_gain = atomic_load(&g_gain);
	st.audio_muted = atomic_load(&g_muted);
	pthread_mutex_lock(&g_v4l2_lock);
	snprintf(st.v4l2_path, sizeof(st.v4l2_path), "%s", g_v4l2_enabled ? g_v4l2_path : "");
	pthread_mutex_unlock(&g_v4l2_lock);
	st.v4l2_configured = g_v4l2.configured;
	st.v4l2_width = g_v4l2.out_w;
	st.v4l2_height = g_v4l2.out_h;
	st.v4l2_frames = g_v4l2.frames;
	st.v4l2_dropped = g_v4l2.dropped;
	st.osd_enabled = g_osd;
	snprintf(st.osd_position, sizeof(st.osd_position), "%s", kOsdPosNames[g_osd_pos & 3]);
	snprintf(st.aspect, sizeof(st.aspect), "%s", kAspectNames[g_aspect & 3]);
	snprintf(st.scale, sizeof(st.scale), "%s", g_scale_integer ? "integer" : "fit");
	st.vsync = g_vsync;
	st.fullscreen = g_fullscreen;
	snprintf(st.framegen_mode, sizeof(st.framegen_mode), "off");
	st.framegen_multiplier = 2;
	st.framegen_flow_scale = 1.0f;
	hws_ipc_publish_stats(&st);
}

/* ---- Headless loop ----------------------------------------------------- */

static void run_headless(void)
{
	uint64_t last_stats = 0;

	printf("[HWS-USER] headless: frame store%s%s\n",
	       g_v4l2_enabled ? " + " : "", g_v4l2_enabled ? g_v4l2_path : "");
	fflush(stdout);
	g_mode_switch = false;
	while (!g_quit && !g_mode_switch) {
		poll_ipc();
		if (now_ms() - last_stats >= 1000) {
			last_stats = now_ms();
			publish_stats(0, 0);
			log_status();
		}
		usleep(20000);
	}
}

/* ---- SDL presenter ----------------------------------------------------- */

#ifdef HAVE_SDL2

#if defined(HAVE_X11) && defined(SDL_VIDEO_DRIVER_X11)
/*
 * Native child-window embedding for the Console Hub (--parent-winid): the SDL
 * window is reparented into the Hub's X11 widget, so the GPU presents straight
 * to it.  Same approach as the GC573 engine.
 */
static int g_embed_w, g_embed_h;
static uint64_t g_embed_sync_ms;

static int x11_ignore_error(Display *dpy, XErrorEvent *ev)
{
	(void)dpy;
	(void)ev;
	return 0;
}

static bool embed_info(SDL_Window *w, Display **dpy, Window *self)
{
	SDL_SysWMinfo info;

	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_X11)
		return false;
	*dpy = info.info.x11.display;
	*self = info.info.x11.window;
	return true;
}

static void embed_attach(SDL_Window *w)
{
	Display *dpy;
	Window self, parent = (Window)g_parent_winid;
	XWindowAttributes attr;
	int (*old)(Display *, XErrorEvent *);

	if (!g_parent_winid || !embed_info(w, &dpy, &self)) {
		if (g_parent_winid)
			fprintf(stderr, "[HWS-USER][WARN] --parent-winid needs SDL on X11\n");
		return;
	}
	old = XSetErrorHandler(x11_ignore_error);
	memset(&attr, 0, sizeof(attr));
	if (XGetWindowAttributes(dpy, parent, &attr) && attr.width > 1 && attr.height > 1) {
		g_embed_w = attr.width;
		g_embed_h = attr.height;
	} else {
		g_embed_w = DEFAULT_WINDOW_W;
		g_embed_h = DEFAULT_WINDOW_H;
	}
	XAddToSaveSet(dpy, self);
	XReparentWindow(dpy, self, parent, 0, 0);
	XMoveResizeWindow(dpy, self, 0, 0, (unsigned)g_embed_w, (unsigned)g_embed_h);
	SDL_SetWindowSize(w, g_embed_w, g_embed_h);
	/* Keep pointer events on the child so the OSD card stays clickable. */
	SDL_ShowWindow(w);
	XMapRaised(dpy, self);
	XSync(dpy, False);
	XSetErrorHandler(old);
	g_embed_sync_ms = now_ms();
	printf("[HWS-USER] embedded into X11 window 0x%lx (%dx%d)\n", g_parent_winid,
	       g_embed_w, g_embed_h);
}

static void embed_sync(SDL_Window *w)
{
	Display *dpy;
	Window self, parent = (Window)g_parent_winid, root = 0, cur = 0, *kids = NULL;
	unsigned int nkids = 0;
	XWindowAttributes attr;
	int (*old)(Display *, XErrorEvent *);

	if (!g_parent_winid || now_ms() - g_embed_sync_ms < 25 || !embed_info(w, &dpy, &self))
		return;
	g_embed_sync_ms = now_ms();
	old = XSetErrorHandler(x11_ignore_error);
	if (XQueryTree(dpy, self, &root, &cur, &kids, &nkids)) {
		if (kids)
			XFree(kids);
		if (cur != parent) {
			XReparentWindow(dpy, self, parent, 0, 0);
			XMapRaised(dpy, self);
		}
	}
	memset(&attr, 0, sizeof(attr));
	if (XGetWindowAttributes(dpy, parent, &attr) && attr.width > 1 && attr.height > 1 &&
	    (attr.width != g_embed_w || attr.height != g_embed_h)) {
		g_embed_w = attr.width;
		g_embed_h = attr.height;
		XMoveResizeWindow(dpy, self, 0, 0, (unsigned)attr.width, (unsigned)attr.height);
		SDL_SetWindowSize(w, attr.width, attr.height);
		XFlush(dpy);
	}
	XSetErrorHandler(old);
}
#else
static void embed_attach(SDL_Window *w)
{
	(void)w;
	if (g_parent_winid)
		fprintf(stderr, "[HWS-USER][WARN] built without X11: --parent-winid ignored\n");
}

static void embed_sync(SDL_Window *w)
{
	(void)w;
}
#endif

static void draw_text(SDL_Renderer *r, int x, int y, int scale, const char *s)
{
	for (; *s; s++, x += 8 * scale) {
		const uint8_t *g = font8x8_basic[(unsigned char)*s & 0x7F];

		for (int row = 0; row < 8; row++)
			for (int col = 0; col < 8; col++)
				if (g[row] & (1u << col)) {
					SDL_Rect px = { x + col * scale, y + row * scale, scale, scale };

					SDL_RenderFillRect(r, &px);
				}
	}
}

static void draw_box_text(SDL_Renderer *r, int win_w, int win_h, int pos,
			  const char *const *lines, int nlines)
{
	const int scale = win_h >= 1080 ? 2 : 1, pad = 8 * scale, lh = 10 * scale;
	int maxlen = 0, bw, bh, bx, by;

	for (int i = 0; i < nlines; i++)
		if ((int)strlen(lines[i]) > maxlen)
			maxlen = (int)strlen(lines[i]);
	bw = maxlen * 8 * scale + 2 * pad;
	bh = nlines * lh + 2 * pad - 2 * scale;
	bx = (pos == HWS_IPC_OSD_TL || pos == HWS_IPC_OSD_BL) ? 16 : win_w - bw - 16;
	by = (pos == HWS_IPC_OSD_TR || pos == HWS_IPC_OSD_TL) ? 16 : win_h - bh - 16;

	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 10, 12, 18, 190);
	SDL_RenderFillRect(r, &(SDL_Rect){ bx, by, bw, bh });
	SDL_SetRenderDrawColor(r, 34, 211, 238, 255);
	SDL_RenderFillRect(r, &(SDL_Rect){ bx, by, 3 * scale, bh });
	SDL_SetRenderDrawColor(r, 235, 240, 245, 255);
	for (int i = 0; i < nlines; i++)
		draw_text(r, bx + pad, by + pad + i * lh, scale, lines[i]);
}

static void handle_key(SDL_Window *win, SDL_Keycode key);

static bool hws_card_active(SDL_Window *win)
{
	Uint32 flags;

	if (g_osd_mode == HWS_IPC_OSD_MODE_CARD)
		return true;
	if (g_osd_mode == HWS_IPC_OSD_MODE_COMPACT)
		return false;
	flags = SDL_GetWindowFlags(win);
	return g_fullscreen ||
		(flags & (SDL_WINDOW_FULLSCREEN |
			  SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}

static void hws_scale_rect(SDL_Rect *dst, const SDL_Rect *src,
			   SDL_Renderer *renderer, int win_w, int win_h)
{
	int out_w = 0;
	int out_h = 0;

	if (src->w <= 0 || src->h <= 0 || win_w <= 0 || win_h <= 0 ||
	    !renderer || SDL_GetRendererOutputSize(renderer, &out_w, &out_h) < 0 ||
	    out_w <= 0 || out_h <= 0) {
		*dst = *src;
		return;
	}
	dst->x = lroundf((float)src->x * (float)out_w / (float)win_w);
	dst->y = lroundf((float)src->y * (float)out_h / (float)win_h);
	dst->w = lroundf((float)src->w * (float)out_w / (float)win_w);
	dst->h = lroundf((float)src->h * (float)out_h / (float)win_h);
}

static void hws_place_osd(SDL_Rect *dst, int width, int height, float scale,
			  int win_w, int win_h)
{
	int margin = (int)(28.0f * scale + 0.5f);
	bool right = g_osd_pos == HWS_IPC_OSD_TR || g_osd_pos == HWS_IPC_OSD_BR;
	bool bottom = g_osd_pos == HWS_IPC_OSD_BR || g_osd_pos == HWS_IPC_OSD_BL;

	if (margin < 10)
		margin = 10;
	dst->w = width;
	dst->h = height;
	dst->x = right ? win_w - margin - width : margin;
	dst->y = bottom ? win_h - margin - height : margin;
	if (dst->x < 0)
		dst->x = 0;
	if (dst->y < 0)
		dst->y = 0;
}

static bool hws_ensure_osd_fonts(void)
{
	if (!everlive_osd_available()) {
		if (!g_osd_font_attempted) {
			g_osd_font_attempted = true;
			everlive_osd_init();
		}
	}
	return everlive_osd_available();
}

static void hws_card_invalidate(void)
{
	g_card.cache_key[0] = '\0';
}

static void hws_card_update(SDL_Renderer *r, SDL_Window *win,
			    int win_w, int win_h)
{
	struct hws_capture_status cs;
	struct everlive_osd_snapshot snap;
	char title[128];
	char source[48];
	char target[220];
	char aspect_lbl[64];
	char scale_lbl[64];
	float gain = atomic_load(&g_gain);
	bool muted = atomic_load(&g_muted);
	Uint32 flags;

	if (!hws_ensure_osd_fonts())
		return;
	hws_capture_get_status(g_cap, &cs);
	snprintf(title, sizeof(title), "HWS UHD : HDMI (%s)",
		 kAspectNames[g_aspect & 3]);
	if (cs.signal)
		snprintf(source, sizeof(source), "%ux%u", cs.width, cs.height);
	else
		snprintf(source, sizeof(source), "No signal");
	snprintf(target, sizeof(target), "%dx%d YUYV | 2 ch 48000 Hz%s",
		 win_w, win_h, muted ? " | Muted" : "");
	snprintf(aspect_lbl, sizeof(aspect_lbl), "Aspect: %s",
		 kAspectNames[g_aspect & 3]);
	snprintf(scale_lbl, sizeof(scale_lbl), "Scale: %s",
		 g_scale_integer ? "Integer" : "Fit");
	flags = SDL_GetWindowFlags(win);
	memset(&snap, 0, sizeof(snap));
	snap.title = title;
	snap.route_source = source;
	snap.route_target = target;
	snap.input_label = NULL;
	snap.aspect_label = aspect_lbl;
	snap.scale_label = scale_lbl;
	snap.show_input = false;
	snap.pip = false;
	snap.muted = muted;
	snap.fullscreen = g_fullscreen ||
		(flags & (SDL_WINDOW_FULLSCREEN |
			  SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
	snap.interactive = true;
	snap.volume_pct = (int)lrintf(gain * 100.0f);
	everlive_osd_build_card(r, &snap, (float)win_h / 1080.0f,
				win_w, win_h, &g_card);
}

static void hws_popup_build(SDL_Renderer *r, float scale)
{
	if (!g_popup_open)
		return;
	everlive_osd_build_popup(r, EVERLIVE_OSD_POPUP_VOLUME,
				 0, g_popup_volume, scale, &g_popup);
	g_popup_scale = scale;
	g_popup_dirty = false;
}

static void hws_render_popup(SDL_Renderer *r, int win_w, int win_h,
			     float scale)
{
	const struct everlive_osd_hitbox *anchor = NULL;
	SDL_Rect draw;

	if (!g_popup_open)
		return;
	if (!g_popup.texture || g_popup_dirty ||
	    fabsf(g_popup_scale - scale) > 0.001f)
		hws_popup_build(r, scale);
	if (!g_popup.texture)
		return;
	for (int i = 0; i < g_card.hit_count; i++) {
		if (g_card.hits[i].action == EVERLIVE_OSD_ACTION_VOLUME) {
			anchor = &g_card.hits[i];
			break;
		}
	}
	if (!anchor)
		return;
	g_popup_dest.x = g_card_dest.x + anchor->rect.x;
	g_popup_dest.y = g_card_dest.y + anchor->rect.y + anchor->rect.h + 8;
	g_popup_dest.w = g_popup.width;
	g_popup_dest.h = g_popup.height;
	if (g_popup_dest.x + g_popup_dest.w > win_w - 10)
		g_popup_dest.x = win_w - g_popup_dest.w - 10;
	if (g_popup_dest.x < 10)
		g_popup_dest.x = 10;
	if (g_popup_dest.y + g_popup_dest.h > win_h - 10) {
		g_popup_dest.y = g_card_dest.y + anchor->rect.y -
				 g_popup.height - 8;
		if (g_popup_dest.y < 10)
			g_popup_dest.y = 10;
	}
	hws_scale_rect(&draw, &g_popup_dest, r, win_w, win_h);
	SDL_RenderCopy(r, g_popup.texture, NULL, &draw);
}

static void hws_draw_card(SDL_Renderer *r, SDL_Window *win,
			  int out_w, int out_h)
{
	int win_w = 0;
	int win_h = 0;
	float scale;
	SDL_Rect draw;

	if (!hws_card_active(win))
		return;
	SDL_GetWindowSize(win, &win_w, &win_h);
	if (win_w <= 0 || win_h <= 0)
		return;
	scale = (float)win_h / 1080.0f;
	hws_card_update(r, win, win_w, win_h);
	if (!g_card.texture)
		return;
	hws_place_osd(&g_card_dest, g_card.width, g_card.height, scale,
		      win_w, win_h);
	hws_scale_rect(&draw, &g_card_dest, r, win_w, win_h);
	SDL_RenderCopy(r, g_card.texture, NULL, &draw);
	hws_render_popup(r, win_w, win_h, scale);
	(void)out_w;
	(void)out_h;
}

static void hws_apply_volume(SDL_Renderer *r, SDL_Window *win, int pct)
{
	(void)r;
	(void)win;
	if (pct < 0)
		pct = 0;
	if (pct > 150)
		pct = 150;
	atomic_store(&g_gain, (float)pct / 100.0f);
	if (pct > 0)
		atomic_store(&g_muted, false);
	g_popup_volume = pct;
	g_popup_dirty = true;
	hws_card_invalidate();
}

static enum everlive_osd_action hws_hit_card(int x, int y)
{
	SDL_Point point = { x - g_card_dest.x, y - g_card_dest.y };

	for (int i = 0; i < g_card.hit_count; i++)
		if (SDL_PointInRect(&point, &g_card.hits[i].rect))
			return g_card.hits[i].action;
	return EVERLIVE_OSD_ACTION_NONE;
}

static void hws_mouse_track(SDL_Window *win, int x, bool press)
{
	SDL_Rect track = g_popup.volume_track;
	int rel = x - (g_popup_dest.x + track.x);
	int pct;

	if (!g_popup_open || g_popup.kind != EVERLIVE_OSD_POPUP_VOLUME)
		return;
	if (rel < 0)
		rel = 0;
	if (rel > track.w)
		rel = track.w;
	pct = track.w > 0 ? (rel * 150 + track.w / 2) / track.w : 0;
	if (press)
		g_popup_dragging = true;
	if (g_popup_dragging)
		hws_apply_volume(NULL, win, pct);
}

static bool hws_handle_mouse(SDL_Event *event, SDL_Window *win)
{
	enum everlive_osd_action action;
	SDL_Point point;
	int win_w = 0;
	int win_h = 0;
	int x;
	int y;

	if (!g_osd || g_streamer_mode || !hws_card_active(win))
		return false;
	SDL_GetWindowSize(win, &win_w, &win_h);
	if (win_w <= 0 || win_h <= 0)
		return false;
	hws_place_osd(&g_card_dest, g_card.width, g_card.height,
		      (float)win_h / 1080.0f, win_w, win_h);

	if (event->type == SDL_MOUSEBUTTONUP &&
	    event->button.button == SDL_BUTTON_LEFT) {
		g_popup_dragging = false;
		return true;
	}
	if (event->type == SDL_MOUSEMOTION) {
		if (g_popup_dragging)
			hws_mouse_track(win, event->motion.x, false);
		return false;
	}
	if (event->type != SDL_MOUSEBUTTONDOWN ||
	    event->button.button != SDL_BUTTON_LEFT)
		return false;
	x = event->button.x;
	y = event->button.y;

	if (g_popup_open) {
		point.x = x - g_popup_dest.x;
		point.y = y - g_popup_dest.y;
		if (g_popup_dest.w > 0 && SDL_PointInRect(&point, &g_popup_dest)) {
			hws_mouse_track(win, x, true);
			return true;
		}
		g_popup_open = false;
		g_popup_dragging = false;
		everlive_osd_destroy_popup(&g_popup);
	}
	action = hws_hit_card(x, y);
	if (event->button.clicks >= 2 && action == EVERLIVE_OSD_ACTION_NONE) {
		if (g_parent_winid) {
			printf("[HUB] TOGGLE_FULLSCREEN\n");
			fflush(stdout);
		} else {
			handle_key(win, SDLK_f);
		}
		return true;
	}
	switch (action) {
	case EVERLIVE_OSD_ACTION_ASPECT:
		handle_key(win, SDLK_a);
		hws_card_invalidate();
		break;
	case EVERLIVE_OSD_ACTION_SCALE:
		handle_key(win, SDLK_l);
		hws_card_invalidate();
		break;
	case EVERLIVE_OSD_ACTION_VOLUME:
		g_popup_volume = (int)lrintf(atomic_load(&g_gain) * 100.0f);
		g_popup_open = true;
		g_popup_dirty = true;
		everlive_osd_destroy_popup(&g_popup);
		break;
	case EVERLIVE_OSD_ACTION_MUTE:
		handle_key(win, SDLK_m);
		hws_card_invalidate();
		break;
	case EVERLIVE_OSD_ACTION_FULLSCREEN:
		if (g_parent_winid) {
			printf("[HUB] TOGGLE_FULLSCREEN\n");
			fflush(stdout);
		} else {
			handle_key(win, SDLK_f);
		}
		hws_card_invalidate();
		break;
	default:
		break;
	}
	return action != EVERLIVE_OSD_ACTION_NONE;
}

static void draw_compact_osd(SDL_Renderer *r, int win_w, int win_h)
{
	struct hws_capture_status cs;
	char l[6][96];
	const char *lines[6];
	int n = 0;

	hws_capture_get_status(g_cap, &cs);
	if (cs.signal)
		snprintf(l[n++], sizeof(l[0]), "HWS UHD  %ux%u @ %.2f fps", cs.width, cs.height, cs.fps);
	else if (cs.present && cs.interlaced)
		snprintf(l[n++], sizeof(l[0]), "HWS UHD  interlaced input (unsupported)");
	else
		snprintf(l[n++], sizeof(l[0]), "HWS UHD  no signal");
	snprintf(l[n++], sizeof(l[0]), "Frames %llu  lost %llu  restarts %llu",
		 (unsigned long long)cs.frames, (unsigned long long)cs.dropped,
		 (unsigned long long)cs.restarts);
	snprintf(l[n++], sizeof(l[0]), "Audio L-PCM 48k stereo  vol %d%%%s",
		 (int)lrintf(atomic_load(&g_gain) * 100.f), atomic_load(&g_muted) ? " [muted]" : "");
	if (g_v4l2.fd >= 0)
		snprintf(l[n++], sizeof(l[0]), "V4L2 %s %ux%u", g_v4l2.path, g_v4l2.out_w, g_v4l2.out_h);
	if (cs.hdcp)
		snprintf(l[n++], sizeof(l[0]), "HDCP active: card withholds video");
	snprintf(l[n++], sizeof(l[0]), "PCIe %s  FPGA %u  %s %s", cs.link, cs.fpga_version,
		 kAspectNames[g_aspect & 3], g_scale_integer ? "integer" : "fit");
	for (int i = 0; i < n; i++)
		lines[i] = l[i];
	draw_box_text(r, win_w, win_h, g_osd_pos, lines, n);
}


static void draw_osd(SDL_Renderer *r, SDL_Window *win, int out_w, int out_h)
{
	if (!g_osd || g_streamer_mode)
		return;
	if (hws_card_active(win)) {
		hws_draw_card(r, win, out_w, out_h);
		return;
	}
	draw_compact_osd(r, out_w, out_h);
}

static SDL_Rect dest_rect(int win_w, int win_h, unsigned int fw, unsigned int fh)
{
	double ar;
	SDL_Rect d = { 0, 0, win_w, win_h };

	if (!fw || !fh || g_aspect == ASPECT_STRETCH)
		return d;
	if (g_scale_integer && g_aspect == ASPECT_AUTO) {
		int k = (int)fmin((double)win_w / fw, (double)win_h / fh);

		if (k >= 1) {
			d.w = (int)fw * k;
			d.h = (int)fh * k;
			d.x = (win_w - d.w) / 2;
			d.y = (win_h - d.h) / 2;
			return d;
		}
	}
	ar = g_aspect == ASPECT_16_9 ? 16.0 / 9.0 : g_aspect == ASPECT_4_3 ? 4.0 / 3.0 :
	     (double)fw / fh;
	if ((double)win_w / win_h > ar) {
		d.h = win_h;
		d.w = (int)lround(win_h * ar);
	} else {
		d.w = win_w;
		d.h = (int)lround(win_w / ar);
	}
	d.x = (win_w - d.w) / 2;
	d.y = (win_h - d.h) / 2;
	return d;
}

static void handle_key(SDL_Window *win, SDL_Keycode k)
{
	char msg[64];
	float gain;

	switch (k) {
	case SDLK_ESCAPE:
	case SDLK_q:
		if (!g_parent_winid)
			g_quit = 1;
		break;
	case SDLK_f:
	case SDLK_F11:
		if (g_parent_winid)
			break;
		g_fullscreen = !g_fullscreen;
		SDL_SetWindowFullscreen(win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
		break;
	case SDLK_a:
		g_aspect = (g_aspect + 1) & 3;
		snprintf(msg, sizeof(msg), "Aspect: %s", kAspectNames[g_aspect]);
		toast(msg);
		break;
	case SDLK_1:
	case SDLK_l:
		g_scale_integer = !g_scale_integer;
		toast(g_scale_integer ? "Scaling: integer" : "Scaling: fit");
		break;
	case SDLK_UP:
	case SDLK_PLUS:
	case SDLK_EQUALS:
	case SDLK_DOWN:
	case SDLK_MINUS:
		gain = atomic_load(&g_gain) + ((k == SDLK_DOWN || k == SDLK_MINUS) ? -0.05f : 0.05f);
		gain = gain < 0.f ? 0.f : gain > 1.5f ? 1.5f : gain;
		atomic_store(&g_gain, gain);
		snprintf(msg, sizeof(msg), "Volume %d%%", (int)lrintf(gain * 100.f));
		toast(msg);
		break;
	case SDLK_m:
		atomic_store(&g_muted, !atomic_load(&g_muted));
		toast(atomic_load(&g_muted) ? "Muted" : "Unmuted");
		break;
	case SDLK_o:
	case SDLK_i:
	case SDLK_TAB:
		g_osd = !g_osd;
		break;
	case SDLK_p:
		g_osd_pos = (g_osd_pos + 1) & 3;
		break;
	case SDLK_s:
	case SDLK_F10:
		g_streamer_mode = !g_streamer_mode;
		g_osd = !g_streamer_mode;
		SDL_ShowCursor(g_streamer_mode ? SDL_DISABLE : SDL_ENABLE);
		break;
	case SDLK_F1:
	case SDLK_SLASH:
		toast("f fullscreen  a aspect  l scale  up/down vol  m mute  o OSD  p corner  q quit");
		break;
	default:
		break;
	}
}

static void run_window(void)
{
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex = NULL;
	unsigned int tex_w = 0, tex_h = 0;
	bool tex_nosignal = false, tex_integer = false;
	uint64_t last_seq = 0, last_stats = 0;
	Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

	g_mode_switch = false;
	/*
	 * Create the window GL-capable up front, as the GC573 engine does.
	 * Otherwise SDL_CreateRenderer() may recreate it (SDL3/sdl2-compat),
	 * and the replacement is a new top-level window outside the Hub.
	 */
	if (g_parent_winid)
		flags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_OPENGL;
	else
		flags |= SDL_WINDOW_SHOWN | (g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	win = SDL_CreateWindow(WINDOW_TITLE, g_parent_winid ? 0 : SDL_WINDOWPOS_CENTERED,
			       g_parent_winid ? 0 : SDL_WINDOWPOS_CENTERED,
			       DEFAULT_WINDOW_W, DEFAULT_WINDOW_H, flags);
	if (!win) {
		fprintf(stderr, "[HWS-USER][ERROR] SDL_CreateWindow: %s; running headless\n",
			SDL_GetError());
		g_headless = true;
		return;
	}
	embed_attach(win);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
				 (g_vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
	if (!ren) {
		fprintf(stderr, "[HWS-USER][ERROR] SDL_CreateRenderer: %s; running headless\n",
			SDL_GetError());
		SDL_DestroyWindow(win);
		g_headless = true;
		return;
	}
	SDL_ShowCursor(g_streamer_mode ? SDL_DISABLE : SDL_ENABLE);

	while (!g_quit && !g_mode_switch) {
		struct hws_frame_ref f;
		SDL_Event e;
		int win_w, win_h;
		bool have_frame;

		while (SDL_PollEvent(&e)) {
			if (hws_handle_mouse(&e, win))
				continue;
			if (e.type == SDL_KEYDOWN && !e.key.repeat &&
			    g_popup_open &&
			    e.key.keysym.sym == SDLK_ESCAPE) {
				g_popup_open = false;
				g_popup_dragging = false;
				everlive_osd_destroy_popup(&g_popup);
				continue;
			}
			if (e.type == SDL_QUIT)
				g_quit = 1;
			else if (e.type == SDL_KEYDOWN && !e.key.repeat)
				handle_key(win, e.key.keysym.sym);
			else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.clicks == 2 && !g_parent_winid)
				handle_key(win, SDLK_f);
		}
		poll_ipc();
		embed_sync(win);
		SDL_GetRendererOutputSize(ren, &win_w, &win_h);

		have_frame = hws_capture_acquire(g_cap, CONSUMER_PRESENT, last_seq,
						 g_vsync ? 4 : 8, &f) == 0;
		if (have_frame) {
			last_seq = f.seq;
			if (!tex || tex_w != f.width || tex_h != f.height || tex_nosignal ||
			    tex_integer != g_scale_integer) {
				if (tex)
					SDL_DestroyTexture(tex);
				tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_YUY2,
							SDL_TEXTUREACCESS_STREAMING,
							(int)f.width, (int)f.height);
				tex_w = f.width;
				tex_h = f.height;
				tex_integer = g_scale_integer;
				if (tex)
					SDL_SetTextureScaleMode(tex, g_scale_integer ?
								SDL_ScaleModeNearest : SDL_ScaleModeLinear);
			}
			tex_nosignal = false;
			if (tex)
				SDL_UpdateTexture(tex, NULL, f.data, (int)f.width * 2);
			hws_capture_release(g_cap, CONSUMER_PRESENT);
		} else {
			struct hws_capture_status cs;

			hws_capture_get_status(g_cap, &cs);
			if (!cs.signal && !tex_nosignal &&
			    hws_no_signal_ensure(&g_nosignal, HWS_NO_SIGNAL_DEFAULT_WIDTH,
						 HWS_NO_SIGNAL_DEFAULT_HEIGHT) == 0) {
				size_t bytes;
				const void *pix = hws_no_signal_frame(&g_nosignal, &bytes);
				unsigned int w = hws_no_signal_width(&g_nosignal);
				unsigned int h = hws_no_signal_height(&g_nosignal);

				if (tex)
					SDL_DestroyTexture(tex);
				tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_YUY2,
							SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
				if (tex && pix) {
					SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
					SDL_UpdateTexture(tex, NULL, pix, (int)w * 2);
				}
				tex_w = w;
				tex_h = h;
				tex_nosignal = true;
			} else if (cs.signal || tex_nosignal) {
				/* Nothing new; avoid re-presenting the same frame in a busy loop. */
				if (!g_vsync)
					SDL_Delay(1);
			}
		}

		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);
		if (tex) {
			SDL_Rect d = tex_nosignal ? (SDL_Rect){ 0, 0, win_w, win_h } :
				     dest_rect(win_w, win_h, tex_w, tex_h);

			SDL_RenderCopy(ren, tex, NULL, &d);
		}
		draw_osd(ren, win, win_w, win_h);
		if (now_ms() < g_toast_until_ms) {
			const char *line = g_toast;

			draw_box_text(ren, win_w, win_h, HWS_IPC_OSD_BL, &line, 1);
		}
		SDL_RenderPresent(ren);

		if (now_ms() - last_stats >= 1000) {
			last_stats = now_ms();
			publish_stats(win_w, win_h);
			log_status();
		}
	}
	hws_capture_release(g_cap, CONSUMER_PRESENT);
	if (tex)
		SDL_DestroyTexture(tex);
	everlive_osd_destroy_card(&g_card);
	everlive_osd_destroy_popup(&g_popup);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
}

static void audio_open(void)
{
	SDL_AudioSpec want = { 0 }, have;

	if (!g_play_audio || g_headless)
		return;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		fprintf(stderr, "[HWS-USER][WARN] SDL audio: %s\n", SDL_GetError());
		return;
	}
	want.freq = HWS_AUDIO_RATE;
	want.format = AUDIO_S16LSB;
	want.channels = HWS_AUDIO_CHANNELS;
	want.samples = 512;
	g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!g_audio_dev) {
		fprintf(stderr, "[HWS-USER][WARN] SDL audio: %s\n", SDL_GetError());
		return;
	}
	SDL_PauseAudioDevice(g_audio_dev, 0);
	printf("SDL audio: playing %d Hz, 2 ch, 16-bit via %s\n", have.freq,
	       SDL_GetCurrentAudioDriver());
}
#endif /* HAVE_SDL2 */

/* ---- Main -------------------------------------------------------------- */

static void usage(const char *argv0)
{
	printf("Usage: %s [PCI_ADDRESS] [OPTIONS]\n"
	       "  -f, --fullscreen       start fullscreen\n"
	       "  --vsync / --no-vsync   pace presentation to the display (default off)\n"
	       "  --aspect MODE          auto|16:9|4:3|stretch\n"
	       "  --scale MODE           fit|integer (alias --integer-scale)\n"
	       "  -v, --volume GAIN      0.0..1.5 (default 1.0)\n"
	       "  --mute                 start muted\n"
	       "  --osd / --no-osd       stats overlay (default on)\n"
	       "  --osd-pos POS          tr|br|bl|tl\n"
	       "  --streamer-mode        hide overlay and cursor\n"
	       "  --v4l2 DEVICE          write frames to a v4l2loopback node\n"
	       "  --loopback [DEVICE]    same, default /dev/video10\n"
	       "  --headless             no window: frame store, V4L2 and audio tap only\n"
	       "  --audio-tap [NAME]     mirror audio into a PipeWire sink\n"
	       "  --no-audio             no local audio playback\n"
	       "  --no-camera            do not publish /dev/shm%s\n"
	       "  --ipc-socket PATH      control socket (default $XDG_RUNTIME_DIR/everlive.sock)\n"
	       "  --no-ipc               no control socket\n"
	       "  --parent-winid ID      embed into an X11 window (Console Hub)\n"
	       "  --slots N              DMA frame slots (default %d)\n"
	       "Hotkeys: f fullscreen, a aspect, l scale, up/down volume, m mute,\n"
	       "         o OSD, p OSD corner, s streamer mode, F1 help, q quit\n",
	       argv0, HWS_CAMERA_SHM_NAME, HWS_CAPTURE_DEFAULT_SLOTS);
}

static int parse_osd_pos(const char *s)
{
	if (!strcasecmp(s, "br") || !strcasecmp(s, "bottom-right"))
		return HWS_IPC_OSD_BR;
	if (!strcasecmp(s, "bl") || !strcasecmp(s, "bottom-left"))
		return HWS_IPC_OSD_BL;
	if (!strcasecmp(s, "tl") || !strcasecmp(s, "top-left"))
		return HWS_IPC_OSD_TL;
	return HWS_IPC_OSD_TR;
}

static int parse_aspect(const char *s)
{
	if (!strcmp(s, "16:9"))
		return ASPECT_16_9;
	if (!strcmp(s, "4:3"))
		return ASPECT_4_3;
	if (!strcasecmp(s, "stretch") || !strcasecmp(s, "fill"))
		return ASPECT_STRETCH;
	return ASPECT_AUTO;
}

enum {
	OPT_VSYNC = 256, OPT_NO_VSYNC, OPT_ASPECT, OPT_SCALE, OPT_INTEGER, OPT_MUTE,
	OPT_OSD, OPT_NO_OSD, OPT_OSD_POS, OPT_STREAMER, OPT_V4L2, OPT_LOOPBACK,
	OPT_HEADLESS, OPT_AUDIO_TAP, OPT_NO_AUDIO, OPT_NO_CAMERA, OPT_IPC_SOCKET,
	OPT_NO_IPC, OPT_PARENT, OPT_SLOTS, OPT_AUDIO_CHANNELS, OPT_SDL,
	OPT_IGNORE_ARG, OPT_IGNORE,
};

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "fullscreen", no_argument, NULL, 'f' },
		{ "vsync", no_argument, NULL, OPT_VSYNC },
		{ "no-vsync", no_argument, NULL, OPT_NO_VSYNC },
		{ "aspect", required_argument, NULL, OPT_ASPECT },
		{ "scale", required_argument, NULL, OPT_SCALE },
		{ "integer-scale", no_argument, NULL, OPT_INTEGER },
		{ "volume", required_argument, NULL, 'v' },
		{ "mute", no_argument, NULL, OPT_MUTE },
		{ "osd", no_argument, NULL, OPT_OSD },
		{ "no-osd", no_argument, NULL, OPT_NO_OSD },
		{ "osd-pos", required_argument, NULL, OPT_OSD_POS },
		{ "streamer-mode", no_argument, NULL, OPT_STREAMER },
		{ "v4l2", required_argument, NULL, OPT_V4L2 },
		{ "loopback", optional_argument, NULL, OPT_LOOPBACK },
		{ "headless", no_argument, NULL, OPT_HEADLESS },
		{ "audio-tap", optional_argument, NULL, OPT_AUDIO_TAP },
		{ "no-audio", no_argument, NULL, OPT_NO_AUDIO },
		{ "no-camera", no_argument, NULL, OPT_NO_CAMERA },
		{ "ipc-socket", required_argument, NULL, OPT_IPC_SOCKET },
		{ "no-ipc", no_argument, NULL, OPT_NO_IPC },
		{ "parent-winid", required_argument, NULL, OPT_PARENT },
		{ "slots", required_argument, NULL, OPT_SLOTS },
		{ "audio-channels", required_argument, NULL, OPT_AUDIO_CHANNELS },
		{ "sdl", no_argument, NULL, OPT_SDL },
		/* GC573 options for hardware this card does not have. */
		{ "led", required_argument, NULL, OPT_IGNORE_ARG },
		{ "edid", required_argument, NULL, OPT_IGNORE_ARG },
		{ "framegen", required_argument, NULL, OPT_IGNORE_ARG },
		{ "fg-mult", required_argument, NULL, OPT_IGNORE_ARG },
		{ "fg-flow-scale", required_argument, NULL, OPT_IGNORE_ARG },
		{ "fg-backend", required_argument, NULL, OPT_IGNORE_ARG },
		{ "hdr-exposure", required_argument, NULL, OPT_IGNORE_ARG },
		{ "wave", no_argument, NULL, OPT_IGNORE },
		{ "rainbow", no_argument, NULL, OPT_IGNORE },
		{ "raw-edid", no_argument, NULL, OPT_IGNORE },
		{ "no-unify-edid", no_argument, NULL, OPT_IGNORE },
		{ "fg-dedup", no_argument, NULL, OPT_IGNORE },
		{ "fg-perf", no_argument, NULL, OPT_IGNORE },
		{ "no-fg-perf", no_argument, NULL, OPT_IGNORE },
		{ "fg-low-latency", no_argument, NULL, OPT_IGNORE },
		{ "no-fg-low-latency", no_argument, NULL, OPT_IGNORE },
		{ "no-hdr", no_argument, NULL, OPT_IGNORE },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct hws_capture_cfg ccfg = { .audio = true, .audio_cb = audio_packet };
	const char *ipc_socket = NULL, *audio_tap = NULL;
	bool use_ipc = true;
	pthread_t out_thread;
	int c, ret;

	hws_log_init();
	while ((c = getopt_long(argc, argv, "fv:h", opts, NULL)) != -1) {
		switch (c) {
		case 'f': g_fullscreen = true; break;
		case OPT_VSYNC: g_vsync = true; break;
		case OPT_NO_VSYNC: g_vsync = false; break;
		case OPT_ASPECT: g_aspect = parse_aspect(optarg); break;
		case OPT_SCALE: g_scale_integer = !strcasecmp(optarg, "integer"); break;
		case OPT_INTEGER: g_scale_integer = true; break;
		case 'v': {
			float g = strtof(optarg, NULL);

			atomic_store(&g_gain, g < 0.f ? 0.f : g > 1.5f ? 1.5f : g);
			break;
		}
		case OPT_MUTE: atomic_store(&g_muted, true); break;
		case OPT_OSD: g_osd = true; break;
		case OPT_NO_OSD: g_osd = false; break;
		case OPT_OSD_POS: g_osd_pos = parse_osd_pos(optarg); break;
		case OPT_STREAMER: g_streamer_mode = true; g_osd = false; break;
		case OPT_V4L2:
			snprintf(g_v4l2_path, sizeof(g_v4l2_path), "%s", optarg);
			g_v4l2_enabled = g_v4l2_dirty = true;
			break;
		case OPT_LOOPBACK:
			if (!optarg && optind < argc && !strncmp(argv[optind], "/dev/", 5))
				optarg = argv[optind++];
			snprintf(g_v4l2_path, sizeof(g_v4l2_path), "%s", optarg ? optarg : "/dev/video10");
			g_v4l2_enabled = g_v4l2_dirty = true;
			break;
		case OPT_HEADLESS: g_headless = true; break;
		case OPT_AUDIO_TAP:
			if (!optarg && optind < argc && argv[optind][0] != '-' &&
			    strncmp(argv[optind], "0000:", 5))
				optarg = argv[optind++];
			audio_tap = optarg ? optarg : HWS_AUDIO_TAP_DEFAULT;
			break;
		case OPT_NO_AUDIO: g_play_audio = false; break;
		case OPT_NO_CAMERA: g_camera = false; break;
		case OPT_IPC_SOCKET: ipc_socket = optarg; break;
		case OPT_NO_IPC: use_ipc = false; break;
		case OPT_PARENT: g_parent_winid = strtoul(optarg, NULL, 0); break;
		case OPT_SLOTS: ccfg.nslots = (unsigned int)strtoul(optarg, NULL, 0); break;
		case OPT_AUDIO_CHANNELS:
			if (strcasecmp(optarg, "auto") && strcasecmp(optarg, "stereo"))
				fprintf(stderr, "[HWS-USER][WARN] --audio-channels %s: the card is stereo only\n",
					optarg);
			break;
		case OPT_SDL: g_headless = false; break;
		case OPT_IGNORE_ARG:
		case OPT_IGNORE:
			break;	/* not on this card */
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	if (optind < argc)
		ccfg.bdf = argv[optind];

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	printf("[HWS-USER] EverLive HWS UHD engine %s\n", HWS_ENGINE_VERSION);
	ret = hws_capture_start(&g_cap, &ccfg);
	if (ret < 0) {
		fprintf(stderr, "[HWS-USER][ERROR] cannot open the capture card: %s\n", strerror(-ret));
		return 1;
	}
	{
		struct hws_capture_status cs;

		hws_capture_get_status(g_cap, &cs);
		printf("[HWS-USER] card %s, FPGA %u, PCIe %s, %u frame slots\n", cs.bdf,
		       cs.fpga_version, cs.link, cs.nslots);
		if (g_camera)
			hws_camera_start(cs.bdf);
	}
	if (audio_tap) {
		if (hws_audio_tap_start(audio_tap) == 0)
			hws_audio_tap_set_format(HWS_AUDIO_RATE, HWS_AUDIO_CHANNELS);
		else
			fprintf(stderr, "[HWS-USER][WARN] audio tap %s unavailable\n", audio_tap);
	}
	if (use_ipc && hws_ipc_start(HWS_ENGINE_VERSION, ipc_socket) < 0)
		fprintf(stderr, "[HWS-USER][WARN] control socket unavailable (another EverLive engine running?)\n");

#ifdef HAVE_SDL2
	if (SDL_Init(0) < 0) {
		fprintf(stderr, "[HWS-USER][WARN] SDL: %s\n", SDL_GetError());
		g_headless = true;
	}
	audio_open();
#else
	g_headless = true;
#endif
	pthread_create(&out_thread, NULL, output_thread, NULL);

	while (!g_quit) {
#ifdef HAVE_SDL2
		if (!g_headless) {
			if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
				fprintf(stderr, "[HWS-USER][WARN] SDL video: %s; running headless\n",
					SDL_GetError());
				g_headless = true;
				continue;
			}
			run_window();
			continue;
		}
#endif
		run_headless();
	}

	printf("[HWS-USER] shutting down\n");
	pthread_join(out_thread, NULL);
	hws_capture_stop(g_cap);
	hws_camera_stop();
	hws_v4l2out_close(&g_v4l2);
	hws_audio_tap_stop();
	if (use_ipc)
		hws_ipc_stop();
#ifdef HAVE_SDL2
	if (g_audio_dev)
		SDL_CloseAudioDevice(g_audio_dev);
	SDL_Quit();
#endif
	hws_no_signal_destroy(&g_nosignal);
	return 0;
}

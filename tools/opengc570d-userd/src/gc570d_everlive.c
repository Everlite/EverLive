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
#include "everlive_framestore.h"
#include "gc570d_everlive.h"

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
#include <SDL2/SDL_syswm.h>
#include "everlive_osd.h"
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif
#endif

#define EVERLIVE_FRAME_BYTES (1920U * 1080U * 2U)
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
	EVERLIVE_OSD_TOP = 0,
	EVERLIVE_OSD_BOTTOM = 1,
};

enum everlive_osd_mode {
	EVERLIVE_OSD_MODE_AUTO = 0,
	EVERLIVE_OSD_MODE_COMPACT = 1,
	EVERLIVE_OSD_MODE_CARD = 2,
};

static enum everlive_osd_action g_hover_action = EVERLIVE_OSD_ACTION_NONE;

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
	uint64_t last_reader_check_ns;
	uint64_t idle_skips;
	bool reader_active;
	bool configured;
};

struct everlive_state {
	struct everlive_framestore *framestore;
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
	atomic_int osd_mode;
	atomic_bool recording;
	atomic_bool osd_dirty;
	atomic_int aspect;
	atomic_int scale;
	atomic_int volume_pct;
	atomic_bool muted;
	atomic_bool headless;
	atomic_int pending_mode;          /* 0 = none, 1 = gaming, 2 = headless */
	atomic_ulong pending_parent_winid;
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
	.osd_pos = ATOMIC_VAR_INIT(EVERLIVE_OSD_TOP),
	.osd_mode = ATOMIC_VAR_INIT(EVERLIVE_OSD_MODE_CARD),
	.recording = ATOMIC_VAR_INIT(false),
	.osd_dirty = ATOMIC_VAR_INIT(false),
	.aspect = ATOMIC_VAR_INIT(EVERLIVE_ASPECT_AUTO),
	.scale = ATOMIC_VAR_INIT(EVERLIVE_SCALE_FIT),
	.volume_pct = ATOMIC_VAR_INIT(100),
	.muted = ATOMIC_VAR_INIT(false),
	.headless = ATOMIC_VAR_INIT(true),
	.pending_mode = ATOMIC_VAR_INIT(0),
	.pending_parent_winid = ATOMIC_VAR_INIT(0),
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

static int parse_osd_mode(const char *mode)
{
	if (!mode)
		return EVERLIVE_OSD_MODE_AUTO;
	if (strcasecmp(mode, "compact") == 0)
		return EVERLIVE_OSD_MODE_COMPACT;
	if (strcasecmp(mode, "card") == 0 || strcasecmp(mode, "full") == 0)
		return EVERLIVE_OSD_MODE_CARD;
	return EVERLIVE_OSD_MODE_AUTO;
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
		return EVERLIVE_OSD_TOP;
	if (!strcasecmp(pos, "bottom") || !strcasecmp(pos, "br") ||
	    !strcasecmp(pos, "bl") || !strcasecmp(pos, "bottom-right") ||
	    !strcasecmp(pos, "bottom-left"))
		return EVERLIVE_OSD_BOTTOM;
	return EVERLIVE_OSD_TOP;
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

static bool shm_port_source_interlaced(const struct everlive_state *st, int port)
{
	struct gc570d_dev *gc = READ_ONCE(st->gc);

	if (!gc)
		return false;
	return port == 0 ? gc->video0_input_interlaced :
			   gc->video_input_interlaced;
}

static unsigned int shm_port_profile(const struct everlive_state *st, int port)
{
	struct gc570d_dev *gc = READ_ONCE(st->gc);

	if (!gc)
		return GC570D_VIDEO0_HDR_BT2020;
	return port == 0 ? gc->video0_source_profile : gc->video_source_profile;
}

/*
 * Snapshot one port through the core frame store.  The sequence check before
 * the copy keeps the old "only copy when a new frame arrived" behaviour, and
 * the post-copy validation makes a lapping producer produce no frame rather
 * than a torn one.
 */
static bool everlive_snapshot(struct everlive_state *st, int port)
{
	struct everlive_frame *frame = &st->frame[port];
	unsigned int tries;

	if (!st->framestore || !frame->pixels)
		return false;

	for (tries = 0; tries < 8; tries++) {
		struct everlive_framestore_frame view;

		if (!everlive_framestore_read_begin(st->framestore,
						    (unsigned int)port,
						    &view))
			return false;
		if (view.seq == st->last_seen[port])
			return false;
		if (view.width < 2 || view.height < 2 ||
		    view.width > 1920 || view.height > 1080)
			return false;
		if (view.bytes > EVERLIVE_FRAME_BYTES)
			return false;

		memcpy(frame->pixels, view.data, view.bytes);
		if (!everlive_framestore_read_validate(st->framestore,
						       (unsigned int)port,
						       &view))
			continue;

		frame->width = view.width;
		frame->height = view.height;
		frame->no_signal = view.no_signal;
		frame->streaming = view.streaming;
		frame->timestamp_ns = view.timestamp_ns;
		frame->seq = view.seq;
		st->last_seen[port] = view.seq;
		st->last_profile[port] = shm_port_profile(st, port);
		return true;
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

/*
 * v4l2loopback reports whether a capture consumer owns the node through
 * sysfs.  Without a reader, write() only burns a full-frame copy (and the
 * dual-HDMI path does this twice), so skip it while nobody is listening.
 */
static bool everlive_v4l2_reader_active(const char *device)
{
	const char *name;
	char path[160];
	char buf[64];
	ssize_t n;
	int fd;

	if (!device || !*device)
		return false;
	name = strrchr(device, '/');
	name = name ? name + 1 : device;
	if (!*name)
		return false;

	if (snprintf(path, sizeof(path), "/sys/class/video4linux/%s/state",
		     name) < (int)sizeof(path)) {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			const char *p = buf;

			n = read(fd, buf, sizeof(buf) - 1);
			close(fd);
			if (n > 0) {
				buf[n] = '\0';
				while (*p == ' ' || *p == '\t' || *p == '\n')
					p++;
				return strncasecmp(p, "capture", 7) == 0;
			}
		}
	}

	if (snprintf(path, sizeof(path), "/sys/class/video4linux/%s/buffers",
		     name) >= (int)sizeof(path))
		return false;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	return strtoul(buf, NULL, 10) > 0;
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

	/* Re-check the sysfs state at most twice per second. */
	if (now - out->last_reader_check_ns >= 500000000ULL) {
		out->reader_active = everlive_v4l2_reader_active(out->path);
		out->last_reader_check_ns = now;
	}
	if (!out->reader_active) {
		out->idle_skips++;
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
	uint32_t overlay_w;
	uint32_t overlay_h;
	uint64_t overlay_built_ns;
	struct everlive_osd_card card;
	struct everlive_osd_popup popup;
	SDL_Rect card_dest;
	SDL_Rect popup_dest;
	enum everlive_osd_popup_kind popup_kind;
	bool popup_open;
	bool popup_dragging;
	bool popup_dirty;
	float popup_scale;
	int popup_selected;
	int popup_hover;
	int popup_volume;
	SDL_Texture *toast_texture;
	char toast_text[96];
	char toast_built[96];
	uint64_t toast_until_ns;
	int toast_w;
	int toast_h;
	SDL_AudioDeviceID audio_dev;
	bool fullscreen;
	unsigned long parent_winid;
	bool parent_lost;
	int embed_last_w;
	int embed_last_h;
	uint64_t embed_last_sync_ns;
};

static bool g_osd_font_init_attempted;

static void sdl_build_overlay(struct everlive_sdl *sdl, const char *line1,
			      const char *line2, const char *line3)
{
	char text[768];
	SDL_Texture *texture = NULL;
	int width = 0;
	int height = 0;

	if (!sdl->renderer)
		return;
	if (!everlive_osd_available()) {
		if (!g_osd_font_init_attempted) {
			g_osd_font_init_attempted = true;
			everlive_osd_init();
		}
		if (!everlive_osd_available())
			return;
	}
	snprintf(text, sizeof(text), "%s\n%s\n%s", line1, line2, line3);
	if (!everlive_osd_build_text(sdl->renderer, text, 1080, &texture,
				     &width, &height, NULL))
		return;
	if (sdl->overlay)
		SDL_DestroyTexture(sdl->overlay);
	sdl->overlay = texture;
	sdl->overlay_w = (uint32_t)width;
	sdl->overlay_h = (uint32_t)height;
}

#if defined(SDL_VIDEO_DRIVER_X11)
static int (*g_prev_x_error_handler)(Display *, XErrorEvent *);

static int gc570d_x11_ignore_error(Display *dpy, XErrorEvent *ev)
{
	(void)dpy;
	(void)ev;
	return 0;
}

/*
 * Permanent process-wide handler for the X11 embedding path.  When the Hub
 * window disappears (clean close or crash) the SDL child and its renderer are
 * torn down while X may still report BadWindow/BadDrawable for in-flight
 * requests.  Those are expected here; everything else is forwarded to the
 * handler SDL installed.
 */
static int gc570d_x11_tolerant_error(Display *dpy, XErrorEvent *ev)
{
	if (ev && (ev->error_code == BadWindow ||
		   ev->error_code == BadDrawable ||
		   ev->error_code == BadMatch))
		return 0;
	if (g_prev_x_error_handler)
		return g_prev_x_error_handler(dpy, ev);
	return 0;
}

static void sdl_x11_install_tolerant_handler(void)
{
	g_prev_x_error_handler = XSetErrorHandler(gc570d_x11_tolerant_error);
}
#else
static void sdl_x11_install_tolerant_handler(void)
{
}
#endif

static void sdl_embed_attach(struct everlive_sdl *sdl)
{
#if defined(SDL_VIDEO_DRIVER_X11)
	SDL_SysWMinfo wminfo;
	Display *dpy;
	Window sdl_xwin;
	Window parent_xwin;
	XWindowAttributes attr;
	int (*old_handler)(Display *, XErrorEvent *);

	if (!sdl->window || !sdl->parent_winid)
		return;

	sdl->parent_lost = false;

	SDL_VERSION(&wminfo.version);
	if (!SDL_GetWindowWMInfo(sdl->window, &wminfo) ||
	    wminfo.subsystem != SDL_SYSWM_X11)
		return;

	dpy = wminfo.info.x11.display;
	sdl_xwin = wminfo.info.x11.window;
	parent_xwin = (Window)sdl->parent_winid;

	old_handler = XSetErrorHandler(gc570d_x11_ignore_error);
	memset(&attr, 0, sizeof(attr));
	if (XGetWindowAttributes(dpy, parent_xwin, &attr) &&
	    attr.width > 1 && attr.height > 1) {
		sdl->embed_last_w = attr.width;
		sdl->embed_last_h = attr.height;
	} else {
		sdl->embed_last_w = 1280;
		sdl->embed_last_h = 720;
	}

	XAddToSaveSet(dpy, sdl_xwin);
	XReparentWindow(dpy, sdl_xwin, parent_xwin, 0, 0);
	XMoveResizeWindow(dpy, sdl_xwin, 0, 0,
			  (unsigned int)sdl->embed_last_w,
			  (unsigned int)sdl->embed_last_h);
	SDL_SetWindowSize(sdl->window, sdl->embed_last_w, sdl->embed_last_h);

	/* Keep pointer events on the child so the OSD card stays clickable
	 * inside the Hub viewport.  Double-click fullscreen is handled by the
	 * engine and forwarded to the Hub when embedded. */
	SDL_ShowWindow(sdl->window);
	XMoveResizeWindow(dpy, sdl_xwin, 0, 0,
			  (unsigned int)sdl->embed_last_w,
			  (unsigned int)sdl->embed_last_h);
	XMapRaised(dpy, sdl_xwin);
	XSync(dpy, False);
	XSetErrorHandler(old_handler);
	sdl->embed_last_sync_ns = now_ns();
#else
	(void)sdl;
#endif
}

static bool sdl_embed_sync_size(struct everlive_state *st,
				struct everlive_sdl *sdl)
{
#if defined(SDL_VIDEO_DRIVER_X11)
	SDL_SysWMinfo wminfo;
	Display *dpy;
	Window sdl_xwin;
	Window parent_xwin;
	Window root_ret = 0;
	Window cur_parent = 0;
	Window *children = NULL;
	unsigned int nchildren = 0;
	XWindowAttributes attr;
	XWindowAttributes self_attr;
	int (*old_handler)(Display *, XErrorEvent *);
	uint64_t now;

	if (!sdl->window)
		return true;
	if (sdl->parent_lost)
		return false;
	if (!sdl->parent_winid)
		return true;

	now = now_ns();
	if (now - sdl->embed_last_sync_ns < 25000000ULL)
		return true;
	sdl->embed_last_sync_ns = now;

	SDL_VERSION(&wminfo.version);
	if (!SDL_GetWindowWMInfo(sdl->window, &wminfo) ||
	    wminfo.subsystem != SDL_SYSWM_X11)
		return true;

	dpy = wminfo.info.x11.display;
	sdl_xwin = wminfo.info.x11.window;
	parent_xwin = (Window)sdl->parent_winid;

	old_handler = XSetErrorHandler(gc570d_x11_ignore_error);
	if (XQueryTree(dpy, sdl_xwin, &root_ret, &cur_parent,
		       &children, &nchildren)) {
		if (children)
			XFree(children);
		if (cur_parent != parent_xwin) {
			XReparentWindow(dpy, sdl_xwin, parent_xwin, 0, 0);
			XMapRaised(dpy, sdl_xwin);
		}
	}

	memset(&attr, 0, sizeof(attr));
	memset(&self_attr, 0, sizeof(self_attr));
	if (!XGetWindowAttributes(dpy, parent_xwin, &attr) ||
	    attr.width <= 1 || attr.height <= 1) {
		/*
		 * The Hub container window disappeared (closed or crashed).
		 * Keep the SDL window alive but hidden and stop rendering; a
		 * later mode.set can reparent it into a fresh container.  Do
		 * not destroy it here: the renderer may still be waiting on a
		 * present for the dead parent.
		 */
		XSync(dpy, False);
		XSetErrorHandler(old_handler);
		sdl->parent_lost = true;
		/* Asynchronous unmap: never block on the dead parent's renderer. */
		XUnmapWindow(dpy, sdl_xwin);
		XFlush(dpy);
		printf("[MODE] Embedded parent window lost; preview suspended\n");
		fflush(stdout);
		atomic_store_explicit(&st->headless, true, __ATOMIC_RELEASE);
		return false;
	}
	{
		bool self_ok = XGetWindowAttributes(dpy, sdl_xwin, &self_attr) != 0;
		bool geom_mismatch = !self_ok ||
				     self_attr.x != 0 || self_attr.y != 0 ||
				     self_attr.width != attr.width ||
				     self_attr.height != attr.height;

		if (attr.width != sdl->embed_last_w ||
		    attr.height != sdl->embed_last_h || geom_mismatch) {
			sdl->embed_last_w = attr.width;
			sdl->embed_last_h = attr.height;
			XMoveResizeWindow(dpy, sdl_xwin, 0, 0,
					  (unsigned int)attr.width,
					  (unsigned int)attr.height);
			SDL_SetWindowSize(sdl->window, attr.width, attr.height);
			if (self_ok && self_attr.map_state != IsViewable)
				XMapRaised(dpy, sdl_xwin);
			XFlush(dpy);
		}
	}
	XSync(dpy, False);
	XSetErrorHandler(old_handler);
	return true;
#else
	(void)st;
	(void)sdl;
	return true;
#endif
}

static void sdl_embed_detach(struct everlive_sdl *sdl)
{
#if defined(SDL_VIDEO_DRIVER_X11)
	SDL_SysWMinfo wminfo;
	Display *dpy;
	Window sdl_xwin;
	Window root;
	int (*old_handler)(Display *, XErrorEvent *);

	if (!sdl->window)
		return;

	SDL_VERSION(&wminfo.version);
	if (!SDL_GetWindowWMInfo(sdl->window, &wminfo) ||
	    wminfo.subsystem != SDL_SYSWM_X11)
		return;

	dpy = wminfo.info.x11.display;
	sdl_xwin = wminfo.info.x11.window;
	root = DefaultRootWindow(dpy);

	old_handler = XSetErrorHandler(gc570d_x11_ignore_error);
	XReparentWindow(dpy, sdl_xwin, root, 100, 100);
	XMoveResizeWindow(dpy, sdl_xwin, 100, 100, 1280, 720);
	SDL_SetWindowSize(sdl->window, 1280, 720);
	XMapRaised(dpy, sdl_xwin);
	XSync(dpy, False);
	XSetErrorHandler(old_handler);

	sdl->embed_last_w = 0;
	sdl->embed_last_h = 0;
	sdl->embed_last_sync_ns = 0;
	sdl->parent_lost = false;
#else
	(void)sdl;
#endif
}

static void sdl_audio_open(struct everlive_state *st, struct everlive_sdl *sdl)
{
	SDL_AudioSpec want, have;

	if (sdl->audio_dev > 0)
		return;

	SDL_SetHint(SDL_HINT_APP_NAME, "EverLive");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "EverLive");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "HDMI Game Audio");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "game");

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		fprintf(stderr,
			"[gc570d WARN] SDL audio unavailable (%s); PipeWire capture nodes remain active\n",
			SDL_GetError());
		return;
	}

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

static void sdl_audio_close(struct everlive_state *st, struct everlive_sdl *sdl)
{
	atomic_store_explicit(&st->audio_dev, 0, __ATOMIC_RELEASE);
	if (sdl->audio_dev > 0) {
		SDL_CloseAudioDevice(sdl->audio_dev);
		sdl->audio_dev = 0;
	}
}

static void sdl_presentation_close(struct everlive_sdl *sdl)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (sdl->texture[i])
			SDL_DestroyTexture(sdl->texture[i]);
		sdl->texture[i] = NULL;
		sdl->texture_w[i] = 0;
		sdl->texture_h[i] = 0;
		sdl->texture_scale_mode[i] = 0;
	}
	if (sdl->overlay)
		SDL_DestroyTexture(sdl->overlay);
	sdl->overlay = NULL;
	sdl->overlay_w = 0;
	sdl->overlay_h = 0;
	sdl->overlay_built_ns = 0;
	everlive_osd_destroy_card(&sdl->card);
	everlive_osd_destroy_popup(&sdl->popup);
	if (sdl->toast_texture)
		SDL_DestroyTexture(sdl->toast_texture);
	sdl->toast_texture = NULL;
	sdl->toast_text[0] = '\0';
	sdl->toast_built[0] = '\0';
	sdl->toast_until_ns = 0;
	sdl->popup_open = false;
	sdl->popup_dirty = false;
	sdl->popup_hover = -1;
	if (sdl->renderer)
		SDL_DestroyRenderer(sdl->renderer);
	if (sdl->window)
		SDL_DestroyWindow(sdl->window);
	sdl->renderer = NULL;
	sdl->window = NULL;
	sdl->parent_winid = 0;
	sdl->parent_lost = false;
	sdl->embed_last_w = 0;
	sdl->embed_last_h = 0;
	sdl->embed_last_sync_ns = 0;
	sdl->fullscreen = false;
}

static bool sdl_presentation_open(struct everlive_state *st,
				  struct everlive_sdl *sdl,
				  unsigned long parent_winid)
{
	Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
	Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN |
			   SDL_WINDOW_OPENGL;
	int window_w = 1280;
	int window_h = 720;

	sdl->parent_winid = parent_winid;
	if (parent_winid) {
		SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
		(void)setenv("SDL_VIDEODRIVER", "x11", 1);
		win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
			    SDL_WINDOW_BORDERLESS | SDL_WINDOW_OPENGL;
	}

	if (st->opt.vsync)
		renderer_flags |= SDL_RENDERER_PRESENTVSYNC;

	SDL_SetHint(SDL_HINT_APP_NAME, "EverLive");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "EverLive");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "HDMI Game Audio");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "game");

	if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
		fprintf(stderr, "[gc570d WARN] SDL video unavailable: %s\n",
			SDL_GetError());
		return false;
	}

	sdl->window = SDL_CreateWindow(
		st->opt.title ? st->opt.title : "EverLive DUO",
		sdl->parent_winid ? 0 : SDL_WINDOWPOS_CENTERED,
		sdl->parent_winid ? 0 : SDL_WINDOWPOS_CENTERED,
		window_w, window_h, win_flags);
	if (!sdl->window) {
		fprintf(stderr, "[gc570d WARN] SDL window failed: %s\n", SDL_GetError());
		return false;
	}
	if (sdl->parent_winid)
		sdl_embed_attach(sdl);

	sdl->renderer = SDL_CreateRenderer(sdl->window, -1, renderer_flags);
	if (!sdl->renderer) {
		sdl->renderer = SDL_CreateRenderer(sdl->window, -1, 0);
		if (!sdl->renderer) {
			fprintf(stderr, "[gc570d WARN] SDL renderer failed: %s\n",
				SDL_GetError());
			SDL_DestroyWindow(sdl->window);
			sdl->window = NULL;
			return false;
		}
	}
	if (sdl->parent_winid)
		sdl_embed_attach(sdl);
	SDL_SetRenderDrawBlendMode(sdl->renderer, SDL_BLENDMODE_BLEND);

	if (st->opt.fullscreen && !sdl->parent_winid) {
		SDL_SetWindowFullscreen(sdl->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
		sdl->fullscreen = true;
	}

	sdl_x11_install_tolerant_handler();
	return true;
}

static bool sdl_init(struct everlive_state *st, struct everlive_sdl *sdl)
{
	const char *pwid_env = getenv("EVERLIVE_PARENT_WINID");
	unsigned long parent_winid = 0;

	if (pwid_env && *pwid_env)
		parent_winid = strtoul(pwid_env, NULL, 0);

	if (!sdl_presentation_open(st, sdl, parent_winid))
		return false;
	sdl_audio_open(st, sdl);
	return true;
}

static void sdl_destroy(struct everlive_state *st, struct everlive_sdl *sdl)
{
	sdl_audio_close(st, sdl);
	sdl_presentation_close(sdl);
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

static bool sdl_card_mode_active(struct everlive_state *st,
				 struct everlive_sdl *sdl);
static void sdl_place_osd(SDL_Rect *dst, int width, int height, float scale,
			  int win_w, int win_h, int pos);
static void sdl_scale_rect(SDL_Rect *dst, const SDL_Rect *src,
			   SDL_Renderer *renderer, int win_w, int win_h);
static void sdl_render_popup(struct everlive_sdl *sdl, int win_w, int win_h,
			     float scale);
static void sdl_render_tooltip(struct everlive_sdl *sdl, int win_w, int win_h,
			       float scale);
static void sdl_render_toast(struct everlive_sdl *sdl, int win_w, int win_h);

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

	if (atomic_load_explicit(&st->osd, __ATOMIC_RELAXED)) {
		int pos = atomic_load_explicit(&st->osd_pos, __ATOMIC_RELAXED);

		if (sdl_card_mode_active(st, sdl) && sdl->card.texture) {
			int win_w = 0;
			int win_h = 0;
			float scale;
			SDL_Rect draw;

			SDL_GetWindowSize(sdl->window, &win_w, &win_h);
			if (win_w > 0 && win_h > 0) {
				scale = (float)win_h / 1080.0f;
				sdl_place_osd(&sdl->card_dest,
					      sdl->card.width, sdl->card.height,
					      scale, win_w, win_h, pos);
				sdl_scale_rect(&draw, &sdl->card_dest,
					       sdl->renderer, win_w, win_h);
				SDL_RenderCopy(sdl->renderer,
					       sdl->card.texture, NULL, &draw);
				sdl_render_tooltip(sdl, win_w, win_h, scale);
				sdl_render_popup(sdl, win_w, win_h, scale);
			}
		} else if (sdl->overlay) {
			const int margin = 24;
			SDL_Rect overlay_dst = {
				.x = margin,
				.y = margin,
				.w = (int)sdl->overlay_w,
				.h = (int)sdl->overlay_h,
			};

			overlay_dst.x = (out_w - overlay_dst.w) / 2;
			overlay_dst.y = pos == EVERLIVE_OSD_BOTTOM ?
				out_h - overlay_dst.h - margin : margin;
			SDL_RenderCopy(sdl->renderer, sdl->overlay, NULL,
				       &overlay_dst);
		}
	}

	if (atomic_load_explicit(&st->osd, __ATOMIC_RELAXED)) {
		int win_w = 0;
		int win_h = 0;

		SDL_GetWindowSize(sdl->window, &win_w, &win_h);
		if (win_w > 0 && win_h > 0)
			sdl_render_toast(sdl, win_w, win_h);
	}
	SDL_RenderPresent(sdl->renderer);
}

static bool sdl_card_mode_active(struct everlive_state *st,
				 struct everlive_sdl *sdl)
{
	int mode = atomic_load_explicit(&st->osd_mode, __ATOMIC_RELAXED);
	Uint32 flags;

	if (mode == EVERLIVE_OSD_MODE_CARD)
		return true;
	if (mode == EVERLIVE_OSD_MODE_COMPACT)
		return false;
	flags = SDL_GetWindowFlags(sdl->window);
	return sdl->fullscreen ||
		(flags & (SDL_WINDOW_FULLSCREEN |
			  SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}

static void sdl_card_invalidate(struct everlive_sdl *sdl)
{
	sdl->card.cache_key[0] = '\0';
	sdl->overlay_built_ns = 0;
}

static void sdl_card_update(struct everlive_state *st,
			    struct everlive_sdl *sdl, int port, int aspect,
			    int win_w, int win_h)
{
	char title[160];
	char details[220];
	char source_label[40];
	char aspect_lbl[64];
	char scale_lbl[64];
	int scale_mode = atomic_load_explicit(&st->scale, __ATOMIC_RELAXED);
	unsigned int rate = shm_port_rate(st, port);
	unsigned int channels = shm_port_channels(st, port);
	unsigned int src_w = shm_port_source_width(st, port);
	unsigned int src_h = shm_port_source_height(st, port);
	unsigned int src_fps = shm_port_source_fps(st, port);
	bool interlaced = shm_port_source_interlaced(st, port);
	int vol = atomic_load_explicit(&st->volume_pct, __ATOMIC_RELAXED);
	bool muted = atomic_load_explicit(&st->muted, __ATOMIC_RELAXED);
	struct everlive_osd_snapshot snap;

	if (!everlive_osd_available()) {
		if (!g_osd_font_init_attempted) {
			g_osd_font_init_attempted = true;
			everlive_osd_init();
		}
		if (!everlive_osd_available())
			return;
	}
	snprintf(title, sizeof(title), "GC570D");
	if (src_w && src_h)
		snprintf(source_label, sizeof(source_label), "%u%c%u", src_h,
			 interlaced ? 'i' : 'p', src_fps ? src_fps : 60);
	else
		snprintf(source_label, sizeof(source_label), "No signal");
	snprintf(details, sizeof(details), "%s • YUYV • %u ch %u Hz",
		 source_label, channels ? channels : 2,
		 rate ? rate : 48000);

	memset(&snap, 0, sizeof(snap));
	snap.title = title;
	snap.details = details;
	snap.input_label = shm_port_name(port);
	snprintf(aspect_lbl, sizeof(aspect_lbl), "Aspect: %s",
		 aspect_mode_label(aspect));
	snprintf(scale_lbl, sizeof(scale_lbl), "Scale: %s",
		 scale_mode == EVERLIVE_SCALE_INTEGER ? "Integer" : "Fit");
	snap.aspect_label = aspect_lbl;
	snap.scale_label = scale_lbl;
	snap.show_input = true;
	snap.pip = atomic_load_explicit(&st->pip, __ATOMIC_RELAXED);
	snap.muted = muted;
	snap.fullscreen = sdl->fullscreen ||
		(SDL_GetWindowFlags(sdl->window) &
		 (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP));
	snap.recording = atomic_load_explicit(&st->recording, __ATOMIC_RELAXED);
	snap.interactive = true;
	snap.hover_action = g_hover_action;
	snap.volume_pct = vol;
	everlive_osd_build_card(sdl->renderer, &snap,
				(float)win_h / 1080.0f, win_w, win_h,
				&sdl->card);
}

static const struct everlive_osd_hitbox *sdl_card_hit(
	struct everlive_sdl *sdl, enum everlive_osd_action action)
{
	for (int i = 0; i < sdl->card.hit_count; i++) {
		if (sdl->card.hits[i].action == action)
			return &sdl->card.hits[i];
	}
	return NULL;
}

static void sdl_scale_rect(SDL_Rect *dst, const SDL_Rect *src,
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

static void sdl_place_osd(SDL_Rect *dst, int width, int height, float scale,
			  int win_w, int win_h, int pos)
{
	int margin = (int)(34.0f * scale + 0.5f);
	bool bottom = pos == EVERLIVE_OSD_BOTTOM;

	if (margin < 10)
		margin = 10;
	dst->w = width;
	dst->h = height;
	dst->x = (win_w - width) / 2;
	dst->y = bottom ? win_h - margin - height : margin;
	if (dst->x < 0)
		dst->x = 0;
	if (dst->y < 0)
		dst->y = 0;
}

static void sdl_toast(struct everlive_sdl *sdl, const char *message)
{
	if (!message || !*message)
		return;
	snprintf(sdl->toast_text, sizeof(sdl->toast_text), "%s", message);
	sdl->toast_until_ns = now_ns() + 2200000000ULL;
	sdl->toast_built[0] = '\0';
	if (sdl->toast_texture) {
		SDL_DestroyTexture(sdl->toast_texture);
		sdl->toast_texture = NULL;
	}
}

static void sdl_render_toast(struct everlive_sdl *sdl, int win_w, int win_h)
{
	SDL_Rect dst;
	SDL_Rect draw;
	float scale;

	if (!sdl->toast_text[0] || now_ns() >= sdl->toast_until_ns)
		return;
	if (!sdl->toast_texture ||
	    strcmp(sdl->toast_built, sdl->toast_text) != 0) {
		if (!everlive_osd_build_text(sdl->renderer, sdl->toast_text,
					     win_h, &sdl->toast_texture,
					     &sdl->toast_w, &sdl->toast_h,
					     NULL))
			return;
		snprintf(sdl->toast_built, sizeof(sdl->toast_built), "%s",
			 sdl->toast_text);
	}
	scale = (float)win_h / 1080.0f;
	dst.w = sdl->toast_w;
	dst.h = sdl->toast_h;
	dst.x = (win_w - dst.w) / 2;
	dst.y = win_h - dst.h - (int)(24.0f * scale + 0.5f);
	if (dst.x < 8)
		dst.x = 8;
	if (dst.y < 8)
		dst.y = 8;
	sdl_scale_rect(&draw, &dst, sdl->renderer, win_w, win_h);
	SDL_RenderCopy(sdl->renderer, sdl->toast_texture, NULL, &draw);
}

static void sdl_popup_build(struct everlive_sdl *sdl, float scale)
{
	if (!sdl->popup_open)
		return;
	everlive_osd_build_popup(sdl->renderer, sdl->popup_kind,
				 sdl->popup_selected, sdl->popup_hover,
				 sdl->popup_volume, scale, &sdl->popup);
	sdl->popup_scale = scale;
	sdl->popup_dirty = false;
}

static void sdl_render_popup(struct everlive_sdl *sdl, int win_w, int win_h,
			     float scale)
{
	const struct everlive_osd_hitbox *anchor;
	enum everlive_osd_action action =
		sdl->popup_kind == EVERLIVE_OSD_POPUP_INPUT
			? EVERLIVE_OSD_ACTION_INPUT : EVERLIVE_OSD_ACTION_VOLUME;

	if (!sdl->popup_open)
		return;
	if (!sdl->popup.texture || sdl->popup_dirty ||
	    fabsf(sdl->popup_scale - scale) > 0.001f)
		sdl_popup_build(sdl, scale);
	if (!sdl->popup.texture)
		return;
	anchor = sdl_card_hit(sdl, action);
	if (!anchor)
		return;
	sdl->popup_dest.x = sdl->card_dest.x + anchor->rect.x;
	sdl->popup_dest.y = sdl->card_dest.y + anchor->rect.y +
			    anchor->rect.h + 8;
	sdl->popup_dest.w = sdl->popup.width;
	sdl->popup_dest.h = sdl->popup.height;
	if (sdl->popup_dest.x + sdl->popup_dest.w > win_w - 10)
		sdl->popup_dest.x = win_w - sdl->popup_dest.w - 10;
	if (sdl->popup_dest.x < 10)
		sdl->popup_dest.x = 10;
	if (sdl->popup_dest.y + sdl->popup_dest.h > win_h - 10) {
		sdl->popup_dest.y = sdl->card_dest.y + anchor->rect.y -
				    sdl->popup.height - 8;
		if (sdl->popup_dest.y < 10)
			sdl->popup_dest.y = 10;
	}
	{
		SDL_Rect draw;

		sdl_scale_rect(&draw, &sdl->popup_dest, sdl->renderer,
			       win_w, win_h);
		SDL_RenderCopy(sdl->renderer, sdl->popup.texture, NULL, &draw);
	}
}

static void sdl_apply_volume(struct everlive_state *st,
			     struct everlive_sdl *sdl, int pct)
{
	(void)st;
	if (pct < 0)
		pct = 0;
	if (pct > 150)
		pct = 150;
	gc570d_everlive_set_volume(pct);
	sdl->popup_volume = pct;
	sdl->popup_dirty = true;
	sdl_card_invalidate(sdl);
}

static void sdl_apply_mute(struct everlive_state *st,
			   struct everlive_sdl *sdl, bool muted)
{
	(void)st;
	gc570d_everlive_set_mute(muted);
	sdl_toast(sdl, muted ? "Mute: On" : "Mute: Off");
	sdl_card_invalidate(sdl);
}

static void sdl_toggle_fullscreen(struct everlive_sdl *sdl)
{
	if (sdl->parent_winid != 0) {
		printf("[HUB] TOGGLE_FULLSCREEN\n");
		fflush(stdout);
		sdl_toast(sdl, "Fullscreen requested");
		return;
	}
	sdl->fullscreen = !sdl->fullscreen;
	SDL_SetWindowFullscreen(sdl->window, sdl->fullscreen ?
				SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	sdl_toast(sdl, sdl->fullscreen ? "Fullscreen: On" : "Fullscreen: Off");
	sdl_card_invalidate(sdl);
}

static void sdl_cycle_aspect(struct everlive_state *st,
			     struct everlive_sdl *sdl)
{
	int next = (atomic_load_explicit(&st->aspect, __ATOMIC_RELAXED) + 1) & 3;
	char message[64];

	atomic_store_explicit(&st->aspect, next, __ATOMIC_RELAXED);
	snprintf(message, sizeof(message), "Aspect: %s",
		 aspect_mode_label(next));
	sdl_toast(sdl, message);
	sdl_card_invalidate(sdl);
}

static void sdl_cycle_scale(struct everlive_state *st,
			    struct everlive_sdl *sdl)
{
	int next = atomic_load_explicit(&st->scale, __ATOMIC_RELAXED) ==
		EVERLIVE_SCALE_INTEGER ? EVERLIVE_SCALE_FIT :
		EVERLIVE_SCALE_INTEGER;
	char message[64];

	atomic_store_explicit(&st->scale, next, __ATOMIC_RELAXED);
	snprintf(message, sizeof(message), "Scale: %s",
		 next == EVERLIVE_SCALE_INTEGER ? "Integer" : "Fit");
	sdl_toast(sdl, message);
	sdl_card_invalidate(sdl);
}

static void sdl_open_popup(struct everlive_sdl *sdl,
			   enum everlive_osd_popup_kind kind)
{
	if (g_hover_action != EVERLIVE_OSD_ACTION_NONE) {
		g_hover_action = EVERLIVE_OSD_ACTION_NONE;
		sdl_card_invalidate(sdl);
	}
	sdl->popup_hover = -1;
	sdl->popup_kind = kind;
	sdl->popup_open = true;
	sdl->popup_dirty = true;
	everlive_osd_destroy_popup(&sdl->popup);
}

static void sdl_close_popup(struct everlive_sdl *sdl)
{
	sdl->popup_open = false;
	sdl->popup_dragging = false;
	sdl->popup_hover = -1;
	sdl->popup_kind = EVERLIVE_OSD_POPUP_NONE;
	everlive_osd_destroy_popup(&sdl->popup);
}

static enum everlive_osd_action sdl_hit_card_raw(struct everlive_sdl *sdl,
						 int x, int y)
{
	SDL_Point point = { x - sdl->card_dest.x, y - sdl->card_dest.y };

	if (!sdl->card.texture || sdl->card_dest.w <= 0)
		return EVERLIVE_OSD_ACTION_NONE;
	for (int i = 0; i < sdl->card.hit_count; i++) {
		if (SDL_PointInRect(&point, &sdl->card.hits[i].rect))
			return sdl->card.hits[i].action;
	}
	return EVERLIVE_OSD_ACTION_NONE;
}

static enum everlive_osd_action sdl_hit_card(struct everlive_sdl *sdl,
					     int x, int y)
{
	enum everlive_osd_action hit = sdl_hit_card_raw(sdl, x, y);

	if (getenv("EVERLIVE_OSD_DEBUG"))
		fprintf(stderr,
			"[OSD] click %d,%d local=%d,%d dest=%d,%d %dx%d gen=%u hit=%d\n",
			x, y, x - sdl->card_dest.x, y - sdl->card_dest.y,
			sdl->card_dest.x, sdl->card_dest.y,
			sdl->card_dest.w, sdl->card_dest.h,
			sdl->card.generation, hit);
	return hit;
}

static void sdl_set_hover(struct everlive_sdl *sdl,
			  enum everlive_osd_action action)
{
	enum everlive_osd_action old = g_hover_action;
	bool visual_old;
	bool visual_new;

	if (old == action)
		return;
	g_hover_action = action;
	visual_old = old != EVERLIVE_OSD_ACTION_NONE &&
		     old != EVERLIVE_OSD_ACTION_INFO;
	visual_new = action != EVERLIVE_OSD_ACTION_NONE &&
		     action != EVERLIVE_OSD_ACTION_INFO;
	if (visual_old || visual_new)
		sdl_card_invalidate(sdl);
	if (getenv("EVERLIVE_OSD_DEBUG"))
		fprintf(stderr, "[OSD] hover=%d\n", action);
}

static void sdl_update_hover(struct everlive_state *st,
			     struct everlive_sdl *sdl, int x, int y)
{
	int win_w = 0;
	int win_h = 0;
	int pos;

	if (!sdl->window || !atomic_load_explicit(&st->osd, __ATOMIC_RELAXED) ||
	    sdl->popup_open || !sdl_card_mode_active(st, sdl) ||
	    !sdl->card.texture) {
		sdl_set_hover(sdl, EVERLIVE_OSD_ACTION_NONE);
		return;
	}
	SDL_GetWindowSize(sdl->window, &win_w, &win_h);
	if (win_w <= 0 || win_h <= 0 || x < 0 || y < 0 ||
	    x >= win_w || y >= win_h) {
		sdl_set_hover(sdl, EVERLIVE_OSD_ACTION_NONE);
		return;
	}
	pos = atomic_load_explicit(&st->osd_pos, __ATOMIC_RELAXED);
	sdl_place_osd(&sdl->card_dest, sdl->card.width, sdl->card.height,
		      (float)win_h / 1080.0f, win_w, win_h, pos);
	sdl_set_hover(sdl, sdl_hit_card_raw(sdl, x, y));
}

static void sdl_render_tooltip(struct everlive_sdl *sdl, int win_w, int win_h,
			       float scale)
{
	const struct everlive_osd_hitbox *anchor;
	SDL_Rect screen;

	if (sdl->popup_open || g_hover_action == EVERLIVE_OSD_ACTION_NONE)
		return;
	anchor = sdl_card_hit(sdl, g_hover_action);
	if (!anchor)
		return;
	screen.x = sdl->card_dest.x + anchor->rect.x;
	screen.y = sdl->card_dest.y + anchor->rect.y;
	screen.w = anchor->rect.w;
	screen.h = anchor->rect.h;
	if (g_hover_action == EVERLIVE_OSD_ACTION_INFO) {
		if (everlive_osd_build_details(sdl->renderer, &sdl->card,
					       scale))
			everlive_osd_render_details(sdl->renderer,
						    &sdl->card, &screen,
						    win_w, win_h);
		return;
	}
	if (everlive_osd_build_tooltip(sdl->renderer, &sdl->card,
				       g_hover_action, scale))
		everlive_osd_render_tooltip(sdl->renderer, &sdl->card,
					    &screen, win_w, win_h);
}

static void sdl_volume_track_event(struct everlive_state *st,
				   struct everlive_sdl *sdl, int x, bool press)
{
	SDL_Rect track = sdl->popup.volume_track;
	int rel;
	int pct;

	if (!sdl->popup_open || sdl->popup.kind != EVERLIVE_OSD_POPUP_VOLUME)
		return;
	rel = x - (sdl->popup_dest.x + track.x);
	if (rel < 0)
		rel = 0;
	if (rel > track.w)
		rel = track.w;
	pct = track.w > 0 ? (rel * 150 + track.w / 2) / track.w : 0;
	if (press)
		sdl->popup_dragging = true;
	if (sdl->popup_dragging)
		sdl_apply_volume(st, sdl, pct);
}

static void sdl_update_popup_hover(struct everlive_sdl *sdl, int x, int y)
{
	SDL_Point point = { x, y };
	SDL_Point local;
	int hover = -1;

	if (!sdl->popup_open || sdl->popup.kind != EVERLIVE_OSD_POPUP_INPUT) {
		if (sdl->popup_hover != -1)
			sdl->popup_hover = -1;
		return;
	}
	if (sdl->popup_dest.w > 0 && SDL_PointInRect(&point, &sdl->popup_dest)) {
		local.x = x - sdl->popup_dest.x;
		local.y = y - sdl->popup_dest.y;
		if (SDL_PointInRect(&local, &sdl->popup.input_rows[0]))
			hover = 0;
		else if (SDL_PointInRect(&local, &sdl->popup.input_rows[1]))
			hover = 1;
	}
	if (hover != sdl->popup_hover) {
		sdl->popup_hover = hover;
		sdl->popup_dirty = true;
		if (getenv("EVERLIVE_OSD_DEBUG"))
			fprintf(stderr, "[OSD] popup hover=%d\n", hover);
	}
}

static bool sdl_handle_mouse(struct everlive_state *st,
			     struct everlive_sdl *sdl, const SDL_Event *event)
{
	enum everlive_osd_action action;
	SDL_Point point;
	int x;
	int y;

	if (getenv("EVERLIVE_OSD_DEBUG") &&
	    (event->type == SDL_MOUSEBUTTONDOWN ||
	     event->type == SDL_MOUSEBUTTONUP ||
	     event->type == SDL_MOUSEMOTION))
		fprintf(stderr, "[OSD] mouse type=%d at %d,%d card=%d,%d+%dx%d\n",
			event->type,
			event->type == SDL_MOUSEMOTION ? event->motion.x : event->button.x,
			event->type == SDL_MOUSEMOTION ? event->motion.y : event->button.y,
			sdl->card_dest.x, sdl->card_dest.y,
			sdl->card_dest.w, sdl->card_dest.h);
	if (!atomic_load_explicit(&st->osd, __ATOMIC_RELAXED) ||
	    !sdl_card_mode_active(st, sdl))
		return false;

	{
		int win_w = 0;
		int win_h = 0;

		SDL_GetWindowSize(sdl->window, &win_w, &win_h);
		if (win_w > 0 && win_h > 0) {
			sdl_place_osd(&sdl->card_dest, sdl->card.width,
				      sdl->card.height,
				      (float)win_h / 1080.0f,
				      win_w, win_h,
				      atomic_load_explicit(&st->osd_pos,
							   __ATOMIC_RELAXED));
		}
	}
	if (event->type == SDL_MOUSEMOTION)
		sdl_update_hover(st, sdl, event->motion.x, event->motion.y);
	else if (event->type == SDL_MOUSEBUTTONDOWN ||
		 event->type == SDL_MOUSEBUTTONUP)
		sdl_update_hover(st, sdl, event->button.x, event->button.y);
	if (sdl->popup_open) {
		if (event->type == SDL_MOUSEMOTION)
			sdl_update_popup_hover(sdl, event->motion.x,
					       event->motion.y);
		else if (event->type == SDL_MOUSEBUTTONDOWN ||
			 event->type == SDL_MOUSEBUTTONUP)
			sdl_update_popup_hover(sdl, event->button.x,
					       event->button.y);
	}

	if (event->type == SDL_MOUSEBUTTONUP &&
	    event->button.button == SDL_BUTTON_LEFT) {
		sdl->popup_dragging = false;
		return true;
	}
	if (event->type == SDL_MOUSEMOTION) {
		if (sdl->popup_dragging)
			sdl_volume_track_event(st, sdl, event->motion.x, false);
		return false;
	}
	if (event->type != SDL_MOUSEBUTTONDOWN ||
	    event->button.button != SDL_BUTTON_LEFT)
		return false;
	x = event->button.x;
	y = event->button.y;

	if (sdl->popup_open) {
		/* Window-space first; input rows below use popup-local space. */
		point.x = x;
		point.y = y;
		if (getenv("EVERLIVE_OSD_DEBUG"))
			fprintf(stderr,
				"[OSD] popup kind=%d dest=%d,%d %dx%d point=%d,%d inside=%d\n",
				(int)sdl->popup.kind, sdl->popup_dest.x,
				sdl->popup_dest.y, sdl->popup_dest.w,
				sdl->popup_dest.h, point.x, point.y,
				SDL_PointInRect(&point, &sdl->popup_dest));
		if (sdl->popup_dest.w > 0 &&
		    SDL_PointInRect(&point, &sdl->popup_dest)) {
			point.x -= sdl->popup_dest.x;
			point.y -= sdl->popup_dest.y;
			if (sdl->popup.kind == EVERLIVE_OSD_POPUP_VOLUME) {
				sdl_volume_track_event(st, sdl, x, true);
			} else if (sdl->popup.kind == EVERLIVE_OSD_POPUP_INPUT) {
				if (SDL_PointInRect(&point,
						    &sdl->popup.input_rows[0])) {
					gc570d_everlive_set_port(0);
					sdl_toast(sdl, "Input: HDMI 1");
					sdl_card_invalidate(sdl);
				} else if (SDL_PointInRect(&point,
							   &sdl->popup.input_rows[1])) {
					gc570d_everlive_set_port(1);
					sdl_toast(sdl, "Input: HDMI 2");
					sdl_card_invalidate(sdl);
				}
				sdl_close_popup(sdl);
			}
			return true;
		}
		sdl_close_popup(sdl);
	}

	action = sdl_hit_card(sdl, x, y);
	if (event->button.clicks >= 2 &&
	    action == EVERLIVE_OSD_ACTION_NONE) {
		sdl_toggle_fullscreen(sdl);
		return true;
	}
	switch (action) {
	case EVERLIVE_OSD_ACTION_INPUT:
		sdl->popup_selected = atomic_load_explicit(&st->port,
							   __ATOMIC_RELAXED);
		sdl_open_popup(sdl, EVERLIVE_OSD_POPUP_INPUT);
		break;
	case EVERLIVE_OSD_ACTION_ASPECT:
		sdl_cycle_aspect(st, sdl);
		break;
	case EVERLIVE_OSD_ACTION_SCALE:
		sdl_cycle_scale(st, sdl);
		break;
	case EVERLIVE_OSD_ACTION_PIP: {
		bool next = !atomic_load_explicit(&st->pip, __ATOMIC_RELAXED);

		atomic_store_explicit(&st->pip, next, __ATOMIC_RELAXED);
		sdl_toast(sdl, next ? "PiP: On" : "PiP: Off");
		sdl_card_invalidate(sdl);
		break;
	}
	case EVERLIVE_OSD_ACTION_VOLUME:
		sdl->popup_volume = atomic_load_explicit(&st->volume_pct,
							 __ATOMIC_RELAXED);
		sdl_open_popup(sdl, EVERLIVE_OSD_POPUP_VOLUME);
		break;
	case EVERLIVE_OSD_ACTION_MUTE:
		sdl_apply_mute(st, sdl,
			!atomic_load_explicit(&st->muted, __ATOMIC_RELAXED));
		break;
	case EVERLIVE_OSD_ACTION_FULLSCREEN:
		sdl_toggle_fullscreen(sdl);
		break;
	case EVERLIVE_OSD_ACTION_RECORD:
		printf("[HUB] TOGGLE_RECORD\n");
		fflush(stdout);
		break;
	default:
		break;
	}
	return action != EVERLIVE_OSD_ACTION_NONE;
}

static bool g_embed_mouse_down;
static uint64_t g_embed_click_ns;
static int g_embed_click_x;
static int g_embed_click_y;

static void sdl_poll_embedded_pointer(struct everlive_state *st,
				      struct everlive_sdl *sdl)
{
#if defined(SDL_VIDEO_DRIVER_X11)
	SDL_SysWMinfo info;
	Display *dpy;
	Window xwin;
	Window root_ret;
	Window child_ret;
	int root_x = 0;
	int root_y = 0;
	int win_x = 0;
	int win_y = 0;
	int local_x = 0;
	int local_y = 0;
	unsigned int mask = 0;
	bool down;

	if (!sdl->window || sdl->parent_winid == 0) {
		g_embed_mouse_down = false;
		return;
	}
	{
		static uint64_t last_poll_ns;
		uint64_t poll_now = now_ns();

		if (poll_now - last_poll_ns < 8000000ULL)
			return;
		last_poll_ns = poll_now;
	}
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(sdl->window, &info) ||
	    info.subsystem != SDL_SYSWM_X11 || !info.info.x11.display)
		return;
	dpy = info.info.x11.display;
	xwin = info.info.x11.window;
	if (!XQueryPointer(dpy, DefaultRootWindow(dpy), &root_ret, &child_ret,
			   &root_x, &root_y, &win_x, &win_y, &mask))
		return;
	if (!XTranslateCoordinates(dpy, DefaultRootWindow(dpy), xwin,
				   root_x, root_y, &local_x, &local_y,
				   &child_ret))
		return;
	{
		XWindowAttributes xa;
		int ww = 0;
		int wh = 0;

		/* X11 reports physical pixels; SDL mouse events are logical. */
		SDL_GetWindowSize(sdl->window, &ww, &wh);
		if (XGetWindowAttributes(dpy, xwin, &xa) && xa.width > 0 &&
		    xa.height > 0 && ww > 0 && wh > 0) {
			local_x = (local_x * ww + xa.width / 2) / xa.width;
			local_y = (local_y * wh + xa.height / 2) / xa.height;
		}
	}
	(void)win_x;
	(void)win_y;
	sdl_update_hover(st, sdl, local_x, local_y);
	if (sdl->popup_open)
		sdl_update_popup_hover(sdl, local_x, local_y);
	down = (mask & Button1Mask) != 0;
	if (down && !g_embed_mouse_down) {
		SDL_Event event;
		uint64_t now = now_ns();
		int clicks = (now - g_embed_click_ns < 450000000ULL &&
			      abs(local_x - g_embed_click_x) < 8 &&
			      abs(local_y - g_embed_click_y) < 8) ? 2 : 1;

		memset(&event, 0, sizeof(event));
		event.type = SDL_MOUSEBUTTONDOWN;
		event.button.button = SDL_BUTTON_LEFT;
		event.button.state = SDL_PRESSED;
		event.button.clicks = (Uint8)clicks;
		event.button.x = local_x;
		event.button.y = local_y;
		sdl_handle_mouse(st, sdl, &event);
		g_embed_click_ns = now;
		g_embed_click_x = local_x;
		g_embed_click_y = local_y;
	} else if (down && g_embed_mouse_down) {
		SDL_Event event;

		memset(&event, 0, sizeof(event));
		event.type = SDL_MOUSEMOTION;
		event.motion.x = local_x;
		event.motion.y = local_y;
		event.motion.state = SDL_BUTTON_LMASK;
		sdl_handle_mouse(st, sdl, &event);
	} else if (!down && g_embed_mouse_down) {
		SDL_Event event;

		memset(&event, 0, sizeof(event));
		event.type = SDL_MOUSEBUTTONUP;
		event.button.button = SDL_BUTTON_LEFT;
		event.button.state = SDL_RELEASED;
		event.button.x = local_x;
		event.button.y = local_y;
		sdl_handle_mouse(st, sdl, &event);
	}
	g_embed_mouse_down = down;
#else
	(void)st;
	(void)sdl;
#endif
}

static bool sdl_handle_events(struct everlive_state *st,
			      struct everlive_sdl *sdl)
{
	SDL_Event event;

	while (SDL_PollEvent(&event)) {
		if (sdl->parent_winid != 0 &&
		    (event.type == SDL_MOUSEBUTTONDOWN ||
		     event.type == SDL_MOUSEBUTTONUP ||
		     event.type == SDL_MOUSEMOTION))
			continue;
		if (sdl_handle_mouse(st, sdl, &event))
			continue;
		if (event.type == SDL_WINDOWEVENT &&
		    event.window.event == SDL_WINDOWEVENT_LEAVE)
			sdl_set_hover(sdl, EVERLIVE_OSD_ACTION_NONE);
		if (event.type == SDL_QUIT)
			return false;
		if (event.type != SDL_KEYDOWN || event.key.repeat)
			continue;
		if (sdl->popup_open &&
		    event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
			sdl_close_popup(sdl);
			continue;
		}

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
			int next_pos = atomic_load_explicit(&st->osd_pos,
							    __ATOMIC_RELAXED) ^ 1;
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
	unsigned int pclk = gc ? gc->splitter_pclk[port == 0 ? 1 : 2] : 0;
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
	printf("[VIDEO] Capturing %ux%up @ %u.%02u Hz (VIP %u Hz, %s, pclk %u kHz) -> %ux%u\n",
	       src_w, src_h, src_fps, 0U, src_fps,
	       shm_profile_name(profile), pclk, out_w, out_h);
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

#ifdef HAVE_SDL2
/*
 * Apply a mode.set request on the frontend thread.  Only the presentation
 * layer is touched: the capture thread, DMA rings, V4L2 sinks, the shared
 * frame store and the audio device keep running in both directions.
 */
static void everlive_switch_mode(struct everlive_state *st,
				 struct everlive_sdl *sdl, bool *sdl_ready,
				 bool want_gaming, unsigned long parent_winid)
{
	bool headless = atomic_load_explicit(&st->headless, __ATOMIC_RELAXED);

	if (want_gaming) {
		if (!*sdl_ready) {
			if (!sdl_presentation_open(st, sdl, parent_winid)) {
				fprintf(stderr,
					"[gc570d WARN] mode.set: video presentation unavailable; staying headless\n");
				fflush(stdout);
				return;
			}
			*sdl_ready = true;
		} else if (sdl->parent_lost ||
			   parent_winid != sdl->parent_winid) {
			sdl->parent_winid = parent_winid;
			sdl->parent_lost = false;
			sdl->embed_last_w = 0;
			sdl->embed_last_h = 0;
			sdl->embed_last_sync_ns = 0;
			if (parent_winid) {
				SDL_ShowWindow(sdl->window);
				sdl_embed_attach(sdl);
			} else {
				sdl_embed_detach(sdl);
			}
		}
		if (headless)
			printf("[MODE] Gaming window opened; DMA, V4L2, SHM and audio untouched\n");
		if (sdl->audio_dev <= 0)
			sdl_audio_open(st, sdl);
		atomic_store_explicit(&st->headless, false, __ATOMIC_RELEASE);
	} else {
		if (*sdl_ready && !sdl->parent_lost) {
			sdl_presentation_close(sdl);
			*sdl_ready = false;
		}
		if (!headless)
			printf("[MODE] Headless: DMA, V4L2, SHM and audio keep running\n");
		atomic_store_explicit(&st->headless, true, __ATOMIC_RELEASE);
	}
	fflush(stdout);
}
#endif

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
	/*
	 * Desktop audio runs in both modes, including a headless start that is
	 * later docked into the Console Hub through mode.set.
	 */
	if (st->opt.enable_sdl || st->opt.headless)
		sdl_audio_open(st, &sdl);
#endif

	while (!atomic_load_explicit(&st->quit, __ATOMIC_RELAXED)) {
		uint64_t now = now_ns();
		int active_port = atomic_load_explicit(&st->port, __ATOMIC_RELAXED);
		bool updated[2] = { false, false };

#ifdef HAVE_SDL2
		{
			int pending = atomic_exchange_explicit(
				&st->pending_mode, 0, __ATOMIC_ACQ_REL);

			if (pending == 1 || pending == 2) {
				unsigned long pwid = atomic_load_explicit(
					&st->pending_parent_winid,
					__ATOMIC_RELAXED);

				everlive_switch_mode(st, &sdl, &sdl_ready,
						     pending == 1, pwid);
			}
		}
#endif

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
			if (atomic_exchange_explicit(&st->osd_dirty, false,
						     __ATOMIC_RELAXED)) {
				sdl_card_invalidate(&sdl);
				sdl.overlay_built_ns = 0;
			}
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
				char line1[160];
				char line2[160];
				char line3[160];
				int out_w = st->frame[port].width ?
					(int)st->frame[port].width : 1920;
				int out_h = st->frame[port].height ?
					(int)st->frame[port].height : 1080;

				if (sdl_card_mode_active(st, &sdl)) {
					int win_w = 0;
					int win_h = 0;

					SDL_GetWindowSize(sdl.window,
							  &win_w, &win_h);
					sdl_card_update(st, &sdl, port, aspect,
							win_w, win_h);
				} else {
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
						 out_w, out_h,
						 shm_port_rate(st, port),
						 shm_port_channels(st, port),
						 muted ? "Muted/" : "", vol);
					snprintf(line3, sizeof(line3),
						 "1/2 Input  P PiP  A Aspect  L Scale  M Mute  Tab OSD  F Full");
					sdl_build_overlay(&sdl, line1, line2,
							  line3);
				}
				sdl.overlay_built_ns = now;
			}

			if (!sdl_handle_events(st, &sdl)) {
				keep_running = false;
				break;
			}
			if (sdl_embed_sync_size(st, &sdl)) {
				sdl_poll_embedded_pointer(st, &sdl);
				sdl_render_frame(st, &sdl);
			}
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
	if (sdl_ready || sdl.audio_dev > 0)
		sdl_destroy(st, &sdl);
#endif
	atomic_store_explicit(&st->running, false, __ATOMIC_RELEASE);
	return NULL;
}

int gc570d_everlive_start(struct everlive_framestore *framestore,
			  struct gc570d_dev *gc,
			  const struct gc570d_everlive_options *options)
{
	int i;
	int vol;

	if (!framestore || !options)
		return -EINVAL;
	if (!options->enable_sdl && !options->headless && !options->v4l2[0] && !options->v4l2[1])
		return 0;
	if (atomic_load_explicit(&g_everlive.running, __ATOMIC_ACQUIRE))
		return -EBUSY;

	memset(&g_everlive, 0, sizeof(g_everlive));
	g_everlive.framestore = framestore;
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
	atomic_init(&g_everlive.osd_mode, EVERLIVE_OSD_MODE_CARD);
	atomic_init(&g_everlive.recording, false);
	atomic_init(&g_everlive.osd_dirty, false);
	atomic_init(&g_everlive.aspect, parse_aspect_mode(options->aspect));
	atomic_init(&g_everlive.scale, parse_scale_mode(options->scale));
	atomic_init(&g_everlive.volume_pct, vol);
	atomic_init(&g_everlive.muted, options->muted);
	atomic_init(&g_everlive.headless, options->headless);
	atomic_init(&g_everlive.pending_mode, 0);
	atomic_init(&g_everlive.pending_parent_winid, 0);
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
	struct timespec deadline;
	int i;
	bool joined;

	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;

	atomic_store_explicit(&g_everlive.quit, true, __ATOMIC_RELAXED);
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 2;
	joined = pthread_timedjoin_np(g_everlive.thread, NULL, &deadline) == 0;
	if (!joined) {
		/*
		 * A dead embedded container can leave the SDL/X11 thread blocked
		 * in the GPU or Xlib.  The capture side has already been asked to
		 * quit, and the kernel releases VFIO on exit, so do not let a
		 * wedged preview thread hang `systemctl stop` forever.
		 */
		fprintf(stderr,
			"[gc570d WARN] frontend thread did not stop within 2 s; forcing exit\n");
		fflush(stderr);
		_exit(0);
	}
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

void gc570d_everlive_set_osd_mode(const char *mode)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	atomic_store_explicit(&g_everlive.osd_mode, parse_osd_mode(mode),
			      __ATOMIC_RELAXED);
}

void gc570d_everlive_set_recording(bool active)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return;
	atomic_store_explicit(&g_everlive.recording, active, __ATOMIC_RELAXED);
	atomic_store_explicit(&g_everlive.osd_dirty, true, __ATOMIC_RELAXED);
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

/* ------------------------------------------------------------------ */
/* Read-only accessors for the JSON-RPC control plane                  */
/* ------------------------------------------------------------------ */

static const char *everlive_osd_pos_name(int pos)
{
	return pos == EVERLIVE_OSD_BOTTOM ? "bottom" : "top";
}

static const char *everlive_aspect_name(int aspect)
{
	switch (aspect) {
	case EVERLIVE_ASPECT_16_9:
		return "16:9";
	case EVERLIVE_ASPECT_4_3:
		return "4:3";
	case EVERLIVE_ASPECT_STRETCH:
		return "stretch";
	case EVERLIVE_ASPECT_AUTO:
	default:
		return "auto";
	}
}

static const char *everlive_scale_name(int scale)
{
	return scale == EVERLIVE_SCALE_INTEGER ? "integer" : "fit";
}

bool gc570d_everlive_running(void)
{
	return atomic_load_explicit(&g_everlive.running, __ATOMIC_RELAXED);
}

bool gc570d_everlive_headless(void)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_RELAXED))
		return true;
	return atomic_load_explicit(&g_everlive.headless, __ATOMIC_RELAXED);
}

int gc570d_everlive_request_mode(bool want_gaming, unsigned long parent_winid)
{
	if (!atomic_load_explicit(&g_everlive.started, __ATOMIC_ACQUIRE))
		return -ENODEV;
#ifndef HAVE_SDL2
	(void)want_gaming;
	(void)parent_winid;
	return -EOPNOTSUPP;
#else
	atomic_store_explicit(&g_everlive.pending_parent_winid, parent_winid,
			      __ATOMIC_RELAXED);
	atomic_store_explicit(&g_everlive.pending_mode,
			      want_gaming ? 1 : 2, __ATOMIC_RELEASE);
	return 0;
#endif
}

unsigned int gc570d_everlive_pixel_clock(void)
{
	struct gc570d_dev *gc = READ_ONCE(g_everlive.gc);
	int port = atomic_load_explicit(&g_everlive.port, __ATOMIC_RELAXED);

	if (!gc)
		return 0;
	return gc->splitter_pclk[port == 0 ? 1 : 2];
}

int gc570d_everlive_port(void)
{
	return atomic_load_explicit(&g_everlive.port, __ATOMIC_RELAXED);
}

bool gc570d_everlive_pip(void)
{
	return atomic_load_explicit(&g_everlive.pip, __ATOMIC_RELAXED);
}

bool gc570d_everlive_osd(void)
{
	return atomic_load_explicit(&g_everlive.osd, __ATOMIC_RELAXED);
}

const char *gc570d_everlive_osd_pos(void)
{
	return everlive_osd_pos_name(
		atomic_load_explicit(&g_everlive.osd_pos, __ATOMIC_RELAXED));
}

const char *gc570d_everlive_aspect(void)
{
	return everlive_aspect_name(
		atomic_load_explicit(&g_everlive.aspect, __ATOMIC_RELAXED));
}

const char *gc570d_everlive_scale(void)
{
	return everlive_scale_name(
		atomic_load_explicit(&g_everlive.scale, __ATOMIC_RELAXED));
}

int gc570d_everlive_volume(void)
{
	return atomic_load_explicit(&g_everlive.volume_pct, __ATOMIC_RELAXED);
}

bool gc570d_everlive_muted(void)
{
	return atomic_load_explicit(&g_everlive.muted, __ATOMIC_RELAXED);
}

const char *gc570d_everlive_v4l2(int port)
{
	if (port < 0 || port > 1)
		return "";
	return g_everlive.opt.v4l2[port] ? g_everlive.opt.v4l2[port] : "";
}

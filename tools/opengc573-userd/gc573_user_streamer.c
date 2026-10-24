/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_streamer.c — Native Userspace VFIO Video Streamer & Display for GC573
 *
 * Runs 100% in Userspace (Ring 3) via vfio-pci, IOMMU DMA, and SDL2 renderer.
 *
 * Pipeline:
 *   HDMI source -> IT6664 splitter RX -> IT6664 TX1 -> IT6805 port 0
 *   -> FPGA VIP (exact resolution, or scaled when the memlock limit is
 *      too small for a full frame) -> video DMA -> SDL2 texture
 *
 * While streaming, the IT6805 is probed for timing/AVI changes.  A confirmed
 * change stops the DMA, re-measures the signal, reprograms the VIP/scaler and
 * restarts the ring without dropping the LED handoff.
 */

#include "gc573_vfio.h"
#include "gc573_user_i2c.h"
#include "gc573_user_hw.h"
#include "gc573_user_edid.h"
#include "gc573_user_it6664.h"
#include "gc573_user_it6805.h"
#include "gc573_user_fpga.h"
#include "gc573_user_video.h"
#include "gc573_user_audio.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_WINDOW_WIDTH	1280U
#define DEFAULT_WINDOW_HEIGHT	720U
#define VIDEO_BUFFERS		4U

/* Cheap timing/AVI probe cadence while streaming. */
#define STATUS_POLL_STREAM_MS	50U
#define STATUS_POLL_WAIT_MS	200U
/* Splitter service and AVMute release cadence. */
#define STATUS_SERVICE_MS	500U
/*
 * Pixel-clock sampling blocks for ~24 ms and must not run in the render loop
 * at high refresh rates.  It is only used when a geometry/colorspace change
 * is already pending, or as a slow background safety net.
 */
#define FRAME_RATE_PROBE_MS	10000U
/* Bounded DMA stop wait for a hot switch; the following DMA reset is safe. */
#define VIDEO_STOP_HOT_MS	150U
/* Audio mapping plus descriptor pages and allocator slack. */
#define MEMLOCK_RESERVE_BYTES	(512U * 1024U)
#define SIGNAL_CHANGE_HITS	2U

#define AUDIO_IT6805_POLL_MS	100U
#define AUDIO_DMA_POLL_MS	5U
#define AUDIO_QUEUE_LIMIT_MS	80U
#define AUDIO_LOST_STOP_MS	1000U
#define AUDIO_FORMAT_WAIT_MS	3000U
#define AUDIO_RETRY_MS		2000U
#define AUDIO_SCRATCH_BYTES	16384U

static volatile sig_atomic_t g_running = 1;
static bool g_trace_mode;
static bool g_trace_requested;

/*
 * LED colour used while streaming.  Cyan is the verified default; it can be
 * overridden with --led or EVERLIVE_LED so the GUI's dropdown colour is kept.
 */
static uint8_t g_led_r = 0x00;
static uint8_t g_led_g = 0xcc;
static uint8_t g_led_b = 0xff;

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static bool parse_led_color(const char *spec, uint8_t *r, uint8_t *g, uint8_t *b)
{
	static const struct {
		const char *name;
		uint8_t r, g, b;
	} names[] = {
		{ "off",     0x00, 0x00, 0x00 },
		{ "blue",    0x00, 0x00, 0xff },
		{ "green",   0x00, 0xff, 0x00 },
		{ "red",     0xff, 0x00, 0x00 },
		{ "cyan",    0x00, 0xcc, 0xff },
		{ "magenta", 0xff, 0x00, 0xff },
		{ "yellow",  0xff, 0xff, 0x00 },
		{ "white",   0xff, 0xff, 0xff },
	};
	const char *hex;
	unsigned long value;
	char *end;

	if (!spec || !*spec)
		return false;

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcasecmp(spec, names[i].name) == 0) {
			*r = names[i].r;
			*g = names[i].g;
			*b = names[i].b;
			return true;
		}
	}

	if (spec[0] == '#') {
		hex = spec + 1;
	} else if (spec[0] == '0' && (spec[1] == 'x' || spec[1] == 'X')) {
		hex = spec + 2;
	} else if (strlen(spec) == 6) {
		hex = spec;   /* bare RRGGBB is accepted too */
	} else {
		return false;
	}

	if (strlen(hex) != 6 || !isxdigit((unsigned char)hex[0]))
		return false;
	value = strtoul(hex, &end, 16);
	if (*end != '\0')
		return false;

	*r = (uint8_t)((value >> 16) & 0xff);
	*g = (uint8_t)((value >> 8) & 0xff);
	*b = (uint8_t)(value & 0xff);
	return true;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [PCI_ADDRESS] [--led COLOR] [--edid PROFILE|FILE]\n", prog);
	printf("  --led COLOR   off|blue|green|red|cyan|magenta|yellow|white|#RRGGBB\n");
	printf("                LED colour while streaming (default: cyan #00ccff).\n");
	printf("  --edid VALUE  default|1440p120|1080p240|/path/to/custom.edid\n");
	printf("                EDID advertised to the source (default: default).\n");
	printf("  EVERLIVE_LED  fallback used when --led is not given.\n");
}

static void stop_led_daemon(void)
{
	(void)system("systemctl --user stop everlive-userd >/dev/null 2>&1");
	(void)system("pkill -x everlive_userd >/dev/null 2>&1");
}

/*
 * The daemon only needs the inherited VFIO descriptors.  Mark every other
 * inherited descriptor close-on-exec so the long-lived child does not keep
 * SDL, DRM or renderer descriptors alive after the streamer exits.
 */
static void mark_extra_fds_close_on_exec(int keep_a, int keep_b, int keep_c)
{
	DIR *dir = opendir("/proc/self/fd");
	int dir_fd;
	struct dirent *entry;

	if (!dir)
		return;
	dir_fd = dirfd(dir);
	while ((entry = readdir(dir))) {
		char *end;
		long fd = strtol(entry->d_name, &end, 10);
		int flags;

		if (*end != '\0' || fd <= 2 || fd > INT_MAX)
			continue;
		if (fd == dir_fd || fd == keep_a || fd == keep_b ||
		    fd == keep_c)
			continue;
		flags = fcntl((int)fd, F_GETFD);
		if (flags >= 0)
			fcntl((int)fd, F_SETFD, flags | FD_CLOEXEC);
	}
	closedir(dir);
}

static bool start_led_daemon(struct gc573_vfio_ctx *ctx, bool handoff)
{
	char exe[PATH_MAX];
	char container_fd[16], group_fd[16], device_fd[16];
	char bar_offset[32], bar_size[32];
	char led_r[4], led_g[4], led_b[4];
	ssize_t len;
	char *slash;
	pid_t pid;

	len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len < 0)
		return false;
	exe[len] = '\0';
	slash = strrchr(exe, '/');
	if (!slash)
		return false;
	if ((size_t)(slash - exe) + 1 + sizeof("everlive_userd") > sizeof(exe))
		return false;
	memcpy(slash + 1, "everlive_userd", sizeof("everlive_userd"));
	if (access(exe, X_OK) != 0)
		return false;

	/*
	 * handoff=false is the early exit path: the parent keeps its BAR,
	 * IRQ and DMA mappings for another few milliseconds while the daemon
	 * starts refreshing the LED.  handoff=true is the late/diagnostic
	 * path: drop those resources before exec so the child can own them.
	 */
	if (handoff)
		gc573_vfio_prepare_inherit(ctx);
	else
		gc573_vfio_keep_fds(ctx);

	snprintf(container_fd, sizeof(container_fd), "%d", ctx->container_fd);
	snprintf(group_fd, sizeof(group_fd), "%d", ctx->group_fd);
	snprintf(device_fd, sizeof(device_fd), "%d", ctx->device_fd);
	snprintf(bar_offset, sizeof(bar_offset), "%llu",
		 (unsigned long long)ctx->bar0_offset);
	snprintf(bar_size, sizeof(bar_size), "%zu", ctx->bar0_size);
	snprintf(led_r, sizeof(led_r), "%u", g_led_r);
	snprintf(led_g, sizeof(led_g), "%u", g_led_g);
	snprintf(led_b, sizeof(led_b), "%u", g_led_b);

	pid = fork();
	if (pid < 0)
		return false;
	if (pid == 0) {
		setsid();
		mark_extra_fds_close_on_exec(ctx->container_fd, ctx->group_fd,
					     ctx->device_fd);
		execl(exe, "everlive_userd", "--inherit",
		      container_fd, group_fd, device_fd,
		      bar_offset, bar_size, "--color",
		      led_r, led_g, led_b, (char *)NULL);
		_exit(127);
	}

	if (handoff) {
		close(ctx->container_fd);
		close(ctx->group_fd);
		close(ctx->device_fd);
		ctx->container_fd = -1;
		ctx->group_fd = -1;
		ctx->device_fd = -1;
	}
	return true;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *colorspace_name(uint8_t colorspace)
{
	switch (colorspace) {
	case GC573_USER_COLORSPACE_RGB:
		return "RGB";
	case GC573_USER_COLORSPACE_YCBCR422:
		return "YCbCr 4:2:2";
	case GC573_USER_COLORSPACE_YCBCR444:
		return "YCbCr 4:4:4";
	case GC573_USER_COLORSPACE_YCBCR420:
		return "YCbCr 4:2:0";
	default:
		return "unknown";
	}
}

static enum gc573_user_input_class
signal_input_class(const struct gc573_user_video_signal *sig)
{
	if (sig->width >= 1920 && sig->height >= 2160)
		return GC573_USER_INPUT_UHD;
	if (sig->height <= 576)
		return GC573_USER_INPUT_SD;
	if (sig->width > 720 || sig->height > 576)
		return GC573_USER_INPUT_HD;
	return GC573_USER_INPUT_SD;
}

static enum gc573_user_sampling
signal_sampling(const struct gc573_user_video_signal *sig)
{
	switch (sig->colorspace) {
	case GC573_USER_COLORSPACE_RGB:
		return GC573_USER_SAMPLING_RGB;
	case GC573_USER_COLORSPACE_YCBCR422:
		return GC573_USER_SAMPLING_YUV422;
	case GC573_USER_COLORSPACE_YCBCR444:
		return GC573_USER_SAMPLING_YUV444;
	case GC573_USER_COLORSPACE_YCBCR420:
	default:
		return GC573_USER_SAMPLING_YUV420;
	}
}

static enum gc573_user_colorimetry
signal_colorimetry(const struct gc573_user_video_signal *sig)
{
	/* SD, including 240p super-resolution, is always BT.601. */
	if (signal_input_class(sig) == GC573_USER_INPUT_SD)
		return GC573_USER_COLORIMETRY_BT601;
	if (sig->colorimetry == 3 &&
	    sig->extended_colorimetry >= 5 && sig->extended_colorimetry <= 6)
		return GC573_USER_COLORIMETRY_BT2020;
	return GC573_USER_COLORIMETRY_BT709;
}

static enum gc573_user_encoding
signal_encoding(const struct gc573_user_video_signal *sig)
{
	if (sig->colorspace != GC573_USER_COLORSPACE_RGB)
		return GC573_USER_ENCODING_YUV;
	return sig->rgb_quantization == 1 ? GC573_USER_ENCODING_RGB_LIMITED :
					   GC573_USER_ENCODING_RGB_FULL;
}

static void raise_memlock_limit(void)
{
	struct rlimit rl;
	struct rlimit want;

	if (getrlimit(RLIMIT_MEMLOCK, &rl) < 0)
		return;

	if (rl.rlim_cur == RLIM_INFINITY && rl.rlim_max == RLIM_INFINITY) {
		pr_info("RLIMIT_MEMLOCK: unlimited");
		return;
	}

	/*
	 * Prefer a fully unlimited limit.  This succeeds when the process has
	 * CAP_SYS_RESOURCE (for example when launched via a privileged unit or
	 * when the hard limit was already unlimited).  Otherwise raise the soft
	 * limit to the existing hard limit, which never needs privileges.
	 */
	want.rlim_cur = RLIM_INFINITY;
	want.rlim_max = RLIM_INFINITY;
	if (setrlimit(RLIMIT_MEMLOCK, &want) == 0) {
		rl = want;
	} else if (rl.rlim_cur != rl.rlim_max) {
		want = rl;
		want.rlim_cur = rl.rlim_max;
		if (setrlimit(RLIMIT_MEMLOCK, &want) == 0) {
			rl = want;
		} else {
			pr_warn("Cannot raise RLIMIT_MEMLOCK to the hard limit: %s",
				strerror(errno));
		}
	}

	if (rl.rlim_cur == RLIM_INFINITY)
		pr_info("RLIMIT_MEMLOCK: unlimited");
	else
		pr_info("RLIMIT_MEMLOCK: %llu KB (hard limit %llu KB)",
			(unsigned long long)rl.rlim_cur / 1024U,
			(unsigned long long)rl.rlim_max / 1024U);
}

static size_t memlock_budget_bytes(void)
{
	struct rlimit rl;

	if (getrlimit(RLIMIT_MEMLOCK, &rl) < 0)
		return 0;
	if (rl.rlim_cur == RLIM_INFINITY)
		return SIZE_MAX;
	if (rl.rlim_cur <= MEMLOCK_RESERVE_BYTES)
		return 0;
	return (size_t)rl.rlim_cur - MEMLOCK_RESERVE_BYTES;
}

/*
 * Native capture only.  The userspace engine does not wire the FPGA
 * downscaler into the DMA geometry, so an output smaller than the input
 * leaves the VIP bridge producing input-height lines into a short buffer and
 * stalls the DMA.  Always request the native frame size and fail loudly when
 * RLIMIT_MEMLOCK cannot hold it.
 */
static int compute_output_and_buffers(
				const struct gc573_user_video_signal *sig,
				unsigned int *out_width,
				unsigned int *out_height,
				unsigned int *num_buffers)
{
	static uint32_t reported_width;
	static uint32_t reported_height;
	static size_t reported_need;
	size_t budget = memlock_budget_bytes();
	unsigned int width = sig->width;
	unsigned int height = sig->height;
	size_t frame_map;
	unsigned int buffers;

	if (!width || !height || (width & 1U))
		return -EINVAL;

	frame_map = ((size_t)width * height * 2U + 4095U) & ~(size_t)4095U;

	if (budget != SIZE_MAX && frame_map > budget) {
		if (width != reported_width || height != reported_height ||
		    frame_map != reported_need) {
			pr_err("Cannot map native %ux%u YUYV frame (%zu KB): "
			       "RLIMIT_MEMLOCK budget is %zu KB; raise it with "
			       "'ulimit -l unlimited', LimitMEMLOCK=64M or CAP_IPC_LOCK",
			       width, height, frame_map / 1024U,
			       budget / 1024U);
			reported_width = width;
			reported_height = height;
			reported_need = frame_map;
		}
		return -ENOMEM;
	}

	reported_width = 0;
	reported_height = 0;
	reported_need = 0;

	if (budget == SIZE_MAX) {
		buffers = VIDEO_BUFFERS;
	} else {
		buffers = (unsigned int)(budget / frame_map);
		if (!buffers)
			buffers = 1U;
		if (buffers > VIDEO_BUFFERS)
			buffers = VIDEO_BUFFERS;
	}

	/*
	 * One buffer already sustains 1440p120 (the verified configuration),
	 * and a 4K ring would otherwise pin ~66 MiB.  Keep large frames at one
	 * buffer and use the extra ring depth only for 720p-class modes.
	 */
	if (frame_map > 4U * 1024U * 1024U && buffers > 1U)
		buffers = 1U;

	*out_width = width;
	*out_height = height;
	*num_buffers = buffers;
	return 0;
}

static bool signal_usable(const struct gc573_user_video_signal *sig)
{
	return sig->scdt_locked && sig->avi_valid && sig->width && sig->height &&
	       sig->pixel_clock_khz && sig->frame_rate_hz;
}

static bool signal_rate_changed(uint32_t hz_a, uint32_t x100_a,
				uint32_t hz_b, uint32_t x100_b)
{
	uint32_t hz_diff;
	uint32_t x100_diff;

	if (!hz_a || !hz_b)
		return hz_a != hz_b;

	hz_diff = hz_a > hz_b ? hz_a - hz_b : hz_b - hz_a;
	/* Mirror the kernel's 2 Hz frame-rate hysteresis. */
	if (hz_diff >= 2)
		return true;

	/*
	 * A short pixel-clock sample can under-read 1440p120 by several Hz
	 * while normalizing to the same nominal rate.  Only treat a large
	 * x100 difference inside the same nominal bucket as a real change.
	 */
	if (!x100_a || !x100_b)
		return false;
	x100_diff = x100_a > x100_b ? x100_a - x100_b : x100_b - x100_a;
	return x100_diff > 1000;
}

static bool signal_parameters_equal(const struct gc573_user_video_signal *a,
				    const struct gc573_user_video_signal *b,
				    bool compare_rate)
{
	if (a->width != b->width || a->height != b->height ||
	    a->interlaced != b->interlaced ||
	    a->colorspace != b->colorspace)
		return false;

	return !compare_rate ||
	       !signal_rate_changed(a->frame_rate_hz, a->frame_rate_x100,
				    b->frame_rate_hz, b->frame_rate_x100);
}

static float display_aspect_for_signal(
			const struct gc573_user_video_signal *sig)
{
	/* 240p/288p super-resolution and 480i/576i are presented as 4:3. */
	switch (sig->height) {
	case 240:
	case 288:
	case 480:
	case 576:
		return 4.0f / 3.0f;
	default:
		return (float)sig->width / (float)sig->height;
	}
}

static int ensure_texture(SDL_Renderer *renderer, SDL_Texture **texture,
			  unsigned int width, unsigned int height)
{
	SDL_Texture *new_texture;
	int tw = 0, th = 0;

	if (*texture) {
		SDL_QueryTexture(*texture, NULL, NULL, &tw, &th);
		if (tw == (int)width && th == (int)height)
			return 0;
	}

	new_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YUY2,
					SDL_TEXTUREACCESS_STREAMING,
					(int)width, (int)height);
	if (!new_texture) {
		pr_err("SDL_CreateTexture(%ux%u) failed: %s",
		       width, height, SDL_GetError());
		return -EIO;
	}

	if (*texture)
		SDL_DestroyTexture(*texture);
	*texture = new_texture;
	return 0;
}

static int configure_and_start(struct gc573_vfio_ctx *ctx,
			       struct gc573_user_video *video,
			       bool *video_ready,
			       struct gc573_user_video_signal *sig,
			       unsigned int out_width,
			       unsigned int out_height,
			       unsigned int num_buffers)
{
	struct gc573_user_fpga_config cfg = { 0 };
	int ret;

	/*
	 * Program the IT6805 output path first: TTL/LVDS mode, DDR/dual-pixel
	 * selection, colour space and the final tristate release.
	 */
	ret = gc573_user_it6805_apply_signal_output(ctx->bar0, sig);
	if (ret < 0) {
		pr_err("IT6805 output path enable failed: %d", ret);
		return ret;
	}

	cfg.input_width = sig->width;
	cfg.input_height = sig->height;
	cfg.output_width = out_width;
	cfg.output_height = out_height;
	cfg.input_frame_rate_hz = sig->frame_rate_hz;
	cfg.input_frame_rate_x100 = sig->frame_rate_x100;
	cfg.output_frame_rate_hz = sig->frame_rate_hz;
	cfg.input_class = signal_input_class(sig);
	cfg.input_encoding = signal_encoding(sig);
	cfg.input_sampling = signal_sampling(sig);
	cfg.input_colorimetry = signal_colorimetry(sig);
	cfg.interlaced = sig->interlaced;
	cfg.dual_pixel = sig->dual_pixel;
	cfg.ddr = sig->ddr;

	/* Ensure FPGA DDR sampler is armed */
	gc573_user_bridge_wait_for_ddr(ctx->bar0);

	ret = gc573_user_fpga_reset_dma(ctx->bar0);
	if (ret < 0) {
		pr_err("FPGA DMA reset failed: %d", ret);
		return ret;
	}

	ret = gc573_user_fpga_configure(ctx->bar0, &cfg);
	if (ret < 0) {
		pr_err("FPGA VIP configuration failed: %d", ret);
		return ret;
	}

	if (*video_ready &&
	    (video->width != out_width || video->height != out_height)) {
		gc573_user_video_stop_timeout(video, VIDEO_STOP_HOT_MS);
		gc573_user_video_cleanup(video);
		*video_ready = false;
	}

	if (!*video_ready) {
		ret = gc573_user_video_init(video, ctx, ctx->bar0,
					    out_width, out_height,
					    num_buffers);
		if (ret < 0 && num_buffers > 1U &&
		    (ret == -ENOMEM || ret == -EPERM || ret == -EAGAIN)) {
			/*
			 * VFIO pins every mapped frame against RLIMIT_MEMLOCK.
			 * Keep the image alive with one buffer instead of failing
			 * capture completely.
			 */
			pr_warn("VFIO memlock limit reached with %u DMA buffers; retrying with 1 buffer",
				num_buffers);
			ret = gc573_user_video_init(video, ctx, ctx->bar0,
						    out_width, out_height, 1);
		}
		if (ret < 0) {
			pr_err("Video DMA initialization failed: %d", ret);
			return ret;
		}
		*video_ready = true;
	}

	ret = gc573_user_video_start(video);
	if (ret < 0) {
		pr_err("Video DMA start failed: %d", ret);
		return ret;
	}

	/*
	 * The DMA reset can return the LED block to the bitstream default.
	 * Re-arm the selected colour; profile, divider and opcode sequence are
	 * unchanged.
	 */
	gc573_user_led_refresh(ctx->bar0, g_led_r, g_led_g, g_led_b);

	if (!gc573_user_fpga_mode_known(sig->width, sig->height,
					sig->frame_rate_hz,
					sig->frame_rate_x100))
		pr_info("Mode %ux%u%s @ %u.%02u Hz is not in the verified kernel table; attempting anyway",
			sig->width, sig->height, sig->interlaced ? "i" : "p",
			sig->frame_rate_x100 / 100, sig->frame_rate_x100 % 100);

	printf("[VIDEO] Capturing %ux%u%s @ %u.%02u Hz (VIP %u Hz, %s, pclk %u kHz) -> %ux%u YUYV%s\n",
	       sig->width, sig->height, sig->interlaced ? "i" : "p",
	       sig->frame_rate_x100 / 100, sig->frame_rate_x100 % 100,
	       sig->frame_rate_hz,
	       colorspace_name(sig->colorspace), sig->pixel_clock_khz,
	       out_width, out_height,
	       (out_width != sig->width || out_height != sig->height) ?
		" (scaled)" : "");
	printf("[VIDEO] IT6805 output: DDR=%u dual-pixel=%u DVI=%u\n",
	       sig->ddr, sig->dual_pixel, sig->is_dvi);
	if (sig->interlaced) {
		u32 field_height = sig->height / 2U;

		printf("[VIDEO] Interlaced field %ux%u, clip_height=0x%08x\n",
		       sig->width, field_height,
		       (field_height << 16) | field_height);
	}

	u32 vstat = readl((u8 *)ctx->bar0 + 0x1004);
	u32 vwidth = readl((u8 *)ctx->bar0 + 0x1008);
	u32 vheight = readl((u8 *)ctx->bar0 + 0x100c);
	u32 vddr = readl((u8 *)ctx->bar0 + 0x107c);
	u32 gpio = readl((u8 *)ctx->bar0 + 0x40);
	printf("[VIDEO] FPGA Bridge: status=0x%08x size=%ux%u DDR=0x%08x GPIO=0x%08x (Splitter SCDT Pin3=%u)\n",
	       vstat, vwidth & 0xffff, vheight & 0xffff, vddr, gpio, !!(gpio & BIT(3)));
	printf("[VIDEO] VIP regs period=0x%08x rate=0x%08x\n",
	       readl((u8 *)ctx->bar0 + 0x103c),
	       readl((u8 *)ctx->bar0 + 0x1080));

	return 0;
}

static void render_frame(SDL_Window *window, SDL_Renderer *renderer,
			 SDL_Texture *texture, float src_aspect)
{
	SDL_Rect dst;
	int win_w = 0;
	int win_h = 0;
	float win_aspect;

	SDL_GetWindowSize(window, &win_w, &win_h);
	if (win_w <= 0 || win_h <= 0 || src_aspect <= 0.0f)
		return;

	win_aspect = (float)win_w / (float)win_h;

	if (win_aspect > src_aspect) {
		dst.h = win_h;
		dst.w = (int)(win_h * src_aspect);
		dst.x = (win_w - dst.w) / 2;
		dst.y = 0;
	} else {
		dst.w = win_w;
		dst.h = (int)(win_w / src_aspect);
		dst.x = 0;
		dst.y = (win_h - dst.h) / 2;
	}

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, &dst);
	SDL_RenderPresent(renderer);
}

static void render_standby(SDL_Window *window, SDL_Renderer *renderer,
			   const char *message)
{
	SDL_SetRenderDrawColor(renderer, 15, 17, 26, 255);
	SDL_RenderClear(renderer);
	SDL_SetWindowTitle(window, message);
	SDL_RenderPresent(renderer);
}

/*
 * SDL_RenderPresent waits on the monitor. Doing that in the DMA poll loop
 * lets a second frame finish before its channel code is read, the ring
 * sticks, and 120 Hz input is counted at the display rate. This thread
 * only retires DMA buffers.
 */
struct frame_preview {
	pthread_t thread;
	pthread_mutex_t lock;
	bool lock_ready;
	bool started;
	bool stop;
	bool stalled;
	struct gc573_user_video *video;
	uint8_t *shadow;
	uint8_t *local;
	size_t bytes;
	uint64_t seq;
	uint64_t consumed;
};

static struct frame_preview g_preview;

static void *preview_thread(void *arg)
{
	struct frame_preview *preview = arg;

	while (!preview->stop) {
		int index = gc573_user_video_poll(preview->video, 5);

		if (index == -2) {
			preview->stalled = true;
			break;
		}
		if (index < 0)
			continue;

		if (preview->video->frames == 1 ||
		    preview->video->frames == 60 ||
		    preview->video->frames % 300 == 0) {
			const uint8_t *p =
				(const uint8_t *)preview->video->buffers[index].vaddr;

			printf("[FRAME #%lu] Buf %d (vaddr=%p): %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x\n",
			       (unsigned long)preview->video->frames, index,
			       (void *)p,
			       p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
			       p[8], p[9], p[10], p[11], p[12], p[13], p[14],
			       p[15]);
		}

		pthread_mutex_lock(&preview->lock);
		memcpy(preview->shadow,
		       preview->video->buffers[index].vaddr, preview->bytes);
		preview->seq++;
		pthread_mutex_unlock(&preview->lock);
		gc573_user_video_release(preview->video, index);
	}
	return NULL;
}

static void preview_stop(void)
{
	if (!g_preview.started)
		return;
	g_preview.stop = true;
	pthread_join(g_preview.thread, NULL);
	g_preview.started = false;
	free(g_preview.shadow);
	free(g_preview.local);
	g_preview.shadow = NULL;
	g_preview.local = NULL;
}

static int preview_start(struct gc573_user_video *video, size_t bytes)
{
	uint8_t *shadow;
	uint8_t *local;

	preview_stop();
	shadow = malloc(bytes);
	local = malloc(bytes);
	if (!shadow || !local) {
		free(shadow);
		free(local);
		return -ENOMEM;
	}
	if (!g_preview.lock_ready) {
		pthread_mutex_init(&g_preview.lock, NULL);
		g_preview.lock_ready = true;
	}
	g_preview.video = video;
	g_preview.shadow = shadow;
	g_preview.local = local;
	g_preview.bytes = bytes;
	g_preview.seq = 0;
	g_preview.consumed = 0;
	g_preview.stop = false;
	g_preview.stalled = false;
	if (pthread_create(&g_preview.thread, NULL, preview_thread,
			   &g_preview) != 0) {
		free(shadow);
		free(local);
		g_preview.shadow = NULL;
		g_preview.local = NULL;
		return -errno;
	}
	g_preview.started = true;
	return 0;
}

static int preview_take(void **pixels)
{
	if (!g_preview.started)
		return 0;
	pthread_mutex_lock(&g_preview.lock);
	if (g_preview.seq == g_preview.consumed) {
		pthread_mutex_unlock(&g_preview.lock);
		return 0;
	}
	memcpy(g_preview.local, g_preview.shadow, g_preview.bytes);
	g_preview.consumed = g_preview.seq;
	pthread_mutex_unlock(&g_preview.lock);
	*pixels = g_preview.local;
	return 1;
}

struct audio_playback {
	SDL_AudioDeviceID dev;
	int rate;
	int bits;
	unsigned int bytes_per_frame;
	uint8_t *scratch;
	size_t scratch_size;
	bool queue_error_reported;
};

struct audio_stream {
	struct gc573_user_audio dma;
	bool dma_ready;
	bool sdl_ready;
	bool streaming;
	uint32_t format_rate;
	unsigned int format_channels;
	unsigned int format_bits;
	uint64_t format_lost_since;
	uint64_t format_wait_since;
	bool format_wait_reported;
	uint64_t last_it6805_poll;
	uint64_t last_dma_poll;
	uint64_t retry_after;
	uint64_t last_error_ms;
	bool unsupported_reported;
	uint64_t dma_blocks;
	uint64_t queued_blocks;
	uint64_t dropped_blocks;
	uint64_t last_dma_blocks;
	uint64_t last_queued_blocks;
	uint64_t last_dropped_blocks;
	uint32_t peak;
	struct audio_playback playback;
};

static void audio_close_device(struct audio_playback *playback)
{
	if (playback->dev) {
		SDL_PauseAudioDevice(playback->dev, 1);
		SDL_CloseAudioDevice(playback->dev);
	}
	playback->dev = 0;
	playback->rate = 0;
	playback->bits = 0;
	playback->bytes_per_frame = 0;
	playback->queue_error_reported = false;
}

static int audio_open_device(struct audio_playback *playback,
			     unsigned int rate_hz, unsigned int sample_bits)
{
	SDL_AudioSpec want;
	SDL_AudioSpec have;

	if (!playback->scratch || !playback->scratch_size)
		return -ENOMEM;
	if (playback->dev && playback->rate == (int)rate_hz &&
	    playback->bits == (int)sample_bits)
		return 0;

	audio_close_device(playback);
	memset(&want, 0, sizeof(want));
	want.freq = (int)rate_hz;
	want.format = sample_bits == 24 ? AUDIO_S32LSB : AUDIO_S16LSB;
	want.channels = 2;
	want.samples = 512;
	want.callback = NULL;

	playback->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
					    SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
	if (!playback->dev) {
		pr_warn("SDL audio: cannot open %u Hz %u-bit stereo: %s",
			rate_hz, sample_bits, SDL_GetError());
		return -EIO;
	}
	if (have.freq != want.freq || have.channels != 2 ||
	    have.format != want.format) {
		pr_warn("SDL audio: device returned %d Hz, %u ch, format 0x%04x; "
			"need %d Hz stereo 0x%04x",
			have.freq, have.channels, have.format,
			want.freq, want.format);
		audio_close_device(playback);
		return -EIO;
	}

	playback->rate = want.freq;
	playback->bits = (int)sample_bits;
	playback->bytes_per_frame = (sample_bits / 8U) * 2U;
	SDL_PauseAudioDevice(playback->dev, 0);
	pr_info("SDL audio: playing %u Hz, 2 ch, %u-bit via %s",
		rate_hz, sample_bits,
		SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown");
	if (SDL_GetCurrentAudioDriver() &&
	    strcmp(SDL_GetCurrentAudioDriver(), "dummy") == 0)
		pr_warn("SDL audio: dummy driver selected; playback is silent "
			"(fix the audio backend or set SDL_AUDIODRIVER)");
	return 0;
}

/*
 * The FPGA always stores 16- or 24-bit little-endian samples in the same
 * channel layout the kernel driver exposes.  Playback is stereo, so keep
 * the first two HDMI slots (FL/FR); 5.1/7.1 sources therefore downmix to
 * their front pair instead of failing.
 */
static size_t audio_convert_pcm(const struct gc573_user_audio *dma,
				const void *src, size_t bytes,
				uint8_t *dst, size_t dst_size)
{
	const uint8_t *in = src;
	unsigned int in_frame;
	unsigned int out_sample_bytes;
	unsigned int out_frame;
	unsigned int frames;
	size_t out_bytes;

	if (!dma->dma_channels || !dma->sample_bytes)
		return 0;

	in_frame = dma->dma_channels * dma->sample_bytes;
	out_sample_bytes = dma->sample_bits / 8U;
	out_frame = out_sample_bytes * 2U;
	frames = (unsigned int)(bytes / in_frame);
	out_bytes = (size_t)frames * out_frame;
	if (!out_bytes || out_bytes > dst_size)
		return 0;

	for (unsigned int frame = 0; frame < frames; frame++) {
		for (unsigned int channel = 0; channel < 2; channel++) {
			const uint8_t *p = in + (size_t)frame * in_frame +
					   (size_t)channel * dma->sample_bytes;
			uint8_t *o = dst + (size_t)frame * out_frame +
				     (size_t)channel * out_sample_bytes;

			if (out_sample_bytes == 2) {
				o[0] = p[0];
				o[1] = p[1];
			} else {
				/* S24_3LE -> S32LSB. */
				o[0] = 0;
				o[1] = p[0];
				o[2] = p[1];
				o[3] = p[2];
			}
		}
	}

	return out_bytes;
}

static void audio_queue_block(struct audio_stream *as, const void *data,
			      size_t bytes)
{
	struct audio_playback *playback = &as->playback;
	uint32_t max_queued;
	uint32_t queued;
	size_t out_bytes;

	if (!playback->dev)
		return;

	out_bytes = audio_convert_pcm(&as->dma, data, bytes,
				      playback->scratch,
				      playback->scratch_size);
	if (!out_bytes)
		return;

	if (as->dma.sample_bits == 16) {
		for (size_t i = 0; i + 1 < out_bytes; i += 2) {
			int16_t sample = (int16_t)(playback->scratch[i] |
						   playback->scratch[i + 1] << 8);
			uint32_t magnitude = sample < 0 ?
					     (uint32_t)(-(int32_t)sample) :
					     (uint32_t)sample;

			if (magnitude > as->peak)
				as->peak = magnitude;
		}
	} else {
		for (size_t i = 0; i + 3 < out_bytes; i += 4) {
			int32_t sample;
			uint32_t magnitude;

			memcpy(&sample, playback->scratch + i, sizeof(sample));
			magnitude = sample < 0 ?
				    (uint32_t)(-(int64_t)sample) :
				    (uint32_t)sample;
			if (magnitude > as->peak)
				as->peak = magnitude;
		}
	}

	max_queued = (uint32_t)((uint64_t)playback->rate *
				playback->bytes_per_frame *
				AUDIO_QUEUE_LIMIT_MS / 1000U);
	queued = SDL_GetQueuedAudioSize(playback->dev);
	if (queued >= max_queued) {
		as->dropped_blocks++;
		return;
	}

	if (SDL_QueueAudio(playback->dev, playback->scratch,
			   (Uint32)out_bytes) < 0) {
		if (!playback->queue_error_reported) {
			pr_warn("SDL audio: SDL_QueueAudio failed: %s",
				SDL_GetError());
			playback->queue_error_reported = true;
		}
	} else {
		as->queued_blocks++;
	}
}

static void audio_stop_capture(struct audio_stream *as)
{
	if (as->dma_ready && as->dma.streaming)
		gc573_user_audio_stop(&as->dma);
	audio_close_device(&as->playback);
	as->streaming = false;
	as->format_rate = 0;
	as->format_channels = 0;
	as->format_bits = 0;
	as->format_lost_since = 0;
	as->format_wait_since = 0;
	as->format_wait_reported = false;
	as->dma_blocks = 0;
	as->queued_blocks = 0;
	as->dropped_blocks = 0;
	as->last_dma_blocks = 0;
	as->last_queued_blocks = 0;
	as->last_dropped_blocks = 0;
	as->peak = 0;
}

static void audio_handle_format(struct audio_stream *as,
				const struct gc573_user_audio_format *format,
				uint64_t now)
{
	bool channels_ok;
	bool rate_ok;

	if (!format->valid) {
		if (as->streaming) {
			if (!as->format_lost_since)
				as->format_lost_since = now;
			else if (now - as->format_lost_since >=
				 AUDIO_LOST_STOP_MS) {
				pr_info("HDMI audio absent for %u ms; stopping audio capture",
					AUDIO_LOST_STOP_MS);
				audio_stop_capture(as);
			}
		} else if (!as->format_wait_since) {
			as->format_wait_since = now;
		} else if (!as->format_wait_reported &&
			   now - as->format_wait_since >=
				   AUDIO_FORMAT_WAIT_MS) {
			pr_info("HDMI audio: receiver reports no playable stream after %u ms; "
				"check the source's HDMI audio output (Linear PCM, 2 ch)",
				AUDIO_FORMAT_WAIT_MS);
			as->format_wait_reported = true;
		}
		return;
	}

	as->format_wait_since = 0;
	as->format_wait_reported = false;
	as->format_lost_since = 0;
	channels_ok = format->channels == 2 || format->channels == 6 ||
		      format->channels == 8;
	rate_ok = format->rate_hz == 32000 || format->rate_hz == 44100 ||
		  format->rate_hz == 48000;
	if (!channels_ok || !rate_ok || format->transport != 0 ||
	    (format->sample_bits != 16 && format->sample_bits != 24)) {
		if (!as->unsupported_reported) {
			pr_warn("HDMI audio format not playable: %u Hz, %u ch, "
				"%u-bit, transport %u; audio disabled",
				format->rate_hz, format->channels,
				format->sample_bits, format->transport);
			as->unsupported_reported = true;
		}
		if (as->streaming)
			audio_stop_capture(as);
		return;
	}
	as->unsupported_reported = false;

	if (as->streaming && as->format_rate == format->rate_hz &&
	    as->format_channels == format->channels &&
	    as->format_bits == format->sample_bits)
		return;

	if (as->streaming) {
		pr_info("HDMI audio format changed; restarting capture");
		audio_stop_capture(as);
	}

	if (!as->sdl_ready || !as->dma_ready || now < as->retry_after)
		return;

	if (audio_open_device(&as->playback, format->rate_hz,
			      format->sample_bits) < 0) {
		as->retry_after = now + AUDIO_RETRY_MS;
		return;
	}

	if (gc573_user_audio_start(&as->dma, format->rate_hz,
				   format->channels,
				   format->sample_bits) < 0) {
		pr_warn("HDMI audio DMA start failed for %u Hz, %u ch, %u-bit",
			format->rate_hz, format->channels,
			format->sample_bits);
		audio_close_device(&as->playback);
		as->retry_after = now + AUDIO_RETRY_MS;
		return;
	}

	as->streaming = true;
	as->format_rate = format->rate_hz;
	as->format_channels = format->channels;
	as->format_bits = format->sample_bits;
	as->retry_after = 0;
}

static void audio_poll_format(struct audio_stream *as, void *bar0,
			      uint64_t now)
{
	struct gc573_user_audio_format format = { 0 };
	int ret;

	if (!as->sdl_ready)
		return;

	ret = gc573_user_it6805_audio_poll(bar0, &format);
	if (ret < 0) {
		if (now - as->last_error_ms >= 5000) {
			pr_warn("IT6805 audio poll failed: %d", ret);
			as->last_error_ms = now;
		}
		return;
	}
	audio_handle_format(as, &format, now);
}

static void audio_drain_dma(struct audio_stream *as)
{
	const void *data;
	size_t bytes;

	if (!as->dma_ready || !as->streaming)
		return;

	for (unsigned int i = 0; i < 4; i++) {
		int ret = gc573_user_audio_poll(&as->dma, &data, &bytes);

		if (ret <= 0)
			break;
		as->dma_blocks++;
		audio_queue_block(as, data, bytes);
	}
}

static void audio_cleanup(struct audio_stream *as)
{
	audio_stop_capture(as);
	free(as->playback.scratch);
	as->playback.scratch = NULL;
	as->playback.scratch_size = 0;
	if (as->dma_ready) {
		gc573_user_audio_cleanup(&as->dma);
		as->dma_ready = false;
	}
}

static void stream_reset_audio(struct audio_stream *as)
{
	as->last_it6805_poll = 0;
	as->last_dma_poll = 0;
	as->retry_after = 0;
	as->unsupported_reported = false;
	as->format_lost_since = 0;
	as->format_wait_since = 0;
	as->format_wait_reported = false;
	as->last_error_ms = 0;
}

/*
 * A signal change must survive two consecutive probes before the expensive
 * full measurement and DMA reconfiguration run.  Rate-only candidates keep
 * accumulating across cheap timing probes, which carry no rate measurement.
 */
struct signal_watch {
	bool active;
	bool rate_valid;
	bool timing_diff;
	bool interlaced;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_hz;
	uint32_t frame_rate_x100;
	uint8_t colorspace;
	unsigned int hits;
};

static void watch_capture(struct signal_watch *watch,
			  const struct gc573_user_video_signal *probe,
			  bool timing_diff)
{
	watch->active = true;
	watch->width = probe->width;
	watch->height = probe->height;
	watch->interlaced = probe->interlaced;
	watch->colorspace = probe->colorspace;
	watch->frame_rate_hz = probe->frame_rate_hz;
	watch->frame_rate_x100 = probe->frame_rate_x100;
	watch->rate_valid = probe->frame_rate_hz != 0;
	watch->timing_diff = timing_diff;
	watch->hits = 1;
}

static bool watch_update(struct signal_watch *watch,
			 const struct gc573_user_video_signal *current,
			 const struct gc573_user_video_signal *probe)
{
	bool probe_rate_valid = probe->frame_rate_hz != 0;
	bool timing_current = probe->width == current->width &&
			      probe->height == current->height &&
			      probe->interlaced == current->interlaced &&
			      probe->colorspace == current->colorspace;
	bool rate_current = !probe_rate_valid || !current->frame_rate_hz ||
			    !signal_rate_changed(
				    probe->frame_rate_hz,
				    probe->frame_rate_x100,
				    current->frame_rate_hz,
				    current->frame_rate_x100);
	bool timing_candidate;
	bool rate_candidate;

	if (!watch->active) {
		if (timing_current && rate_current)
			return false;
		watch_capture(watch, probe, !timing_current);
		return false;
	}

	timing_candidate = watch->width == probe->width &&
			   watch->height == probe->height &&
			   watch->interlaced == probe->interlaced &&
			   watch->colorspace == probe->colorspace;
	rate_candidate = !probe_rate_valid || !watch->rate_valid ||
			 !signal_rate_changed(probe->frame_rate_hz,
					      probe->frame_rate_x100,
					      watch->frame_rate_hz,
					      watch->frame_rate_x100);

	if (timing_candidate && rate_candidate) {
		/*
		 * A cheap timing probe cannot confirm a rate-only change.
		 * Require two pixel-clock probes for that case, but let a timing
		 * probe confirm a candidate that already differs in geometry or
		 * colorspace.
		 */
		if (!probe_rate_valid && !watch->timing_diff)
			return false;
		if (probe_rate_valid) {
			watch->frame_rate_hz = probe->frame_rate_hz;
			watch->frame_rate_x100 = probe->frame_rate_x100;
			watch->rate_valid = true;
		}
		watch->hits++;
		return watch->hits >= SIGNAL_CHANGE_HITS;
	}

	if (timing_current && rate_current) {
		memset(watch, 0, sizeof(*watch));
		return false;
	}

	watch_capture(watch, probe, !timing_current);
	return false;
}

static void trace_dump(struct gc573_vfio_ctx *ctx, const char *tag)
{
	u8 *b;

	if (!ctx || !ctx->bar0) {
		printf("[TRACE] %-28s BAR not mapped\n", tag);
		fflush(stdout);
		return;
	}

	b = (u8 *)ctx->bar0;
	printf("[TRACE] %-28s "
	       "rst=%08x irq=%08x/%08x ctl=%08x ch=%08x "
	       "leddiv=%08x leden=%08x scr=%08x,%08x,%08x,%08x "
	       "vip=%08x ddr=%08x vrst=%08x\n",
	       tag,
	       readl(b + 0x0000c), readl(b + 0x00010), readl(b + 0x0001c),
	       readl(b + 0x00304), readl(b + 0x00300),
	       readl(b + 0x00800), readl(b + 0x00804),
	       readl(b + 0x02008), readl(b + 0x0200c),
	       readl(b + 0x02010), readl(b + 0x02014),
	       readl(b + 0x01000), readl(b + 0x0107c),
	       readl(b + 0x50000));
	fflush(stdout);
}

static void trace_draw_digit(SDL_Renderer *renderer, int digit,
			     int x, int y, int w, int h)
{
	/* Seven-segment masks for 0..9. */
	static const unsigned int masks[10] = {
		0x3f, 0x06, 0x5b, 0x4f, 0x66,
		0x6d, 0x7d, 0x07, 0x7f, 0x6f
	};
	unsigned int mask = (digit >= 0 && digit <= 9) ? masks[digit] : 0;
	int t = w / 6;
	SDL_Rect seg;

	SDL_SetRenderDrawColor(renderer, 240, 240, 240, 255);
#define SEG(bit, rx, ry, rw, rh) do { \
		if (mask & (1U << (bit))) { \
			seg.x = (rx); seg.y = (ry); \
			seg.w = (rw); seg.h = (rh); \
			SDL_RenderFillRect(renderer, &seg); \
		} \
	} while (0)

	SEG(0, x + t, y, w - 2 * t, t);                 /* top */
	SEG(1, x + w - t, y + t, t, h / 2 - t);         /* upper right */
	SEG(2, x + w - t, y + h / 2, t, h / 2 - t);     /* lower right */
	SEG(3, x + t, y + h - t, w - 2 * t, t);         /* bottom */
	SEG(4, x, y + h / 2, t, h / 2 - t);             /* lower left */
	SEG(5, x, y + t, t, h / 2 - t);                 /* upper left */
	SEG(6, x + t, y + h / 2 - t / 2, w - 2 * t, t); /* middle */
#undef SEG
}

static void trace_show_phase(SDL_Window *window, SDL_Renderer *renderer,
			     int phase, const char *title)
{
	int out_w = 0, out_h = 0;
	int digit_w, digit_h;

	if (window)
		SDL_SetWindowTitle(window, title);
	printf("[TRACE] %s\n", title);
	fflush(stdout);

	if (!renderer || !window)
		return;
	SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
	SDL_SetRenderDrawColor(renderer, 8, 10, 18, 255);
	SDL_RenderClear(renderer);

	digit_h = out_h * 2 / 3;
	digit_w = digit_h / 2;
	if (digit_w > out_w / 2) {
		digit_w = out_w / 2;
		digit_h = digit_w * 2;
	}
	trace_draw_digit(renderer, phase,
			 (out_w - digit_w) / 2, (out_h - digit_h) / 2,
			 digit_w, digit_h);

	/* Progress pips along the bottom. */
	for (int i = 0; i <= 8; i++) {
		SDL_Rect pip = {
			.x = out_w / 2 - 9 * 22 + i * 44,
			.y = out_h - 42,
			.w = 28,
			.h = 14,
		};

		if (i <= phase)
			SDL_SetRenderDrawColor(renderer, 0, 220, 240, 255);
		else
			SDL_SetRenderDrawColor(renderer, 60, 65, 80, 255);
		SDL_RenderFillRect(renderer, &pip);
	}
	SDL_RenderPresent(renderer);
}

static void trace_wait(unsigned int sec)
{
	for (unsigned int i = 0; i < sec * 10; i++) {
		SDL_PumpEvents();
		SDL_Delay(100);
	}
}

static void run_teardown_trace(struct gc573_vfio_ctx *ctx,
			       struct gc573_user_video *video,
			       bool video_ready,
			       SDL_Window *window,
			       SDL_Renderer *renderer,
			       SDL_Texture *texture)
{
	printf("\n[TRACE] Starting teardown phase trace. The window stays open "
	       "and shows a large phase number. Watch the LED and note the "
	       "number when it turns red.\n");
	fflush(stdout);

	/* Keep the title bar and the phase number visible. */
	if (window) {
		SDL_SetWindowFullscreen(window, 0);
		SDL_RaiseWindow(window);
	}

	trace_dump(ctx, "phase 0: streaming");
	trace_show_phase(window, renderer, 0,
			 "TRACE 0/8 — streaming; LED should show the selected colour");
	trace_wait(6);

	preview_stop();
	if (video_ready)
		gc573_user_video_stop(video);
	trace_dump(ctx, "phase 1: DMA stopped");
	trace_show_phase(window, renderer, 1,
			 "TRACE 1/8 — DMA stopped; LED probably red");
	trace_wait(6);

	/*
	 * Candidate fix: keep the VIP pixel source running after the DMA
	 * engine has been shut down, then reload the LED profile while the
	 * video pipeline is alive again.
	 */
	if (ctx->bar0)
		gc573_user_fpga_set_output_enabled(ctx->bar0, true);
	trace_dump(ctx, "phase 2: VIP re-enabled");
	trace_show_phase(window, renderer, 2,
			 "TRACE 2/8 — VIP re-enabled; still red?");
	trace_wait(6);

	trace_show_phase(window, renderer, 3,
			 "TRACE 3/8 — re-arming LED 20x/s; colour back?");
	if (ctx->bar0) {
		for (int i = 0; i < 160; i++) {
			gc573_user_led_refresh(ctx->bar0, g_led_r, g_led_g, g_led_b);
			SDL_PumpEvents();
			SDL_Delay(50);
		}
	}
	trace_dump(ctx, "phase 3: LED re-armed for 8s");

	if (video_ready)
		gc573_user_video_unmap(video);
	trace_dump(ctx, "phase 4: DMA unmapped");
	trace_show_phase(window, renderer, 4,
			 "TRACE 4/8 — DMA unmapped; watch LED");
	trace_wait(6);

	gc573_vfio_release_irq(ctx);
	trace_dump(ctx, "phase 5: IRQ released");
	trace_show_phase(window, renderer, 5,
			 "TRACE 5/8 — IRQ released; watch LED");
	trace_wait(6);

	trace_dump(ctx, "phase 6: before BAR unmap");
	gc573_vfio_unmap_bar(ctx);
	trace_show_phase(window, renderer, 6,
			 "TRACE 6/8 — BAR unmapped; watch LED");
	trace_wait(6);

	start_led_daemon(ctx, true);
	trace_show_phase(window, renderer, 7,
			 "TRACE 7/8 — VFIO handed to daemon; watch LED");
	trace_wait(6);

	if (texture)
		SDL_DestroyTexture(texture);
	if (renderer)
		SDL_DestroyRenderer(renderer);
	if (window)
		SDL_DestroyWindow(window);
	SDL_Quit();

	printf("[TRACE] phase 8: SDL quit; waiting 4 s for final LED state\n");
	fflush(stdout);
	sleep(4);
	printf("[TRACE] done\n");
	fflush(stdout);
}

int main(int argc, char *argv[])
{
	const char *pci_addr = NULL;
	const char *led_spec = NULL;
	const char *edid_spec = NULL;
	struct gc573_vfio_ctx ctx;
	struct gc573_user_video video;
	struct audio_stream audio;
	bool video_ready = false;
	bool streaming = false;
	SDL_Window *window = NULL;
	SDL_Renderer *renderer = NULL;
	SDL_Texture *texture = NULL;
	uint64_t last_status_poll = 0;
	uint64_t last_service = 0;
	uint64_t last_frame_rate_probe = 0;
	uint64_t last_stats = 0;
	uint64_t last_frames = 0;
	uint32_t input_width = 0;
	uint32_t input_height = 0;
	uint32_t input_rate_x100 = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	float video_aspect = 16.0f / 9.0f;
	struct gc573_user_video_signal current_sig;
	struct signal_watch watch;
	bool have_signal = false;
	unsigned int signal_lost_hits = 0;
	bool rate_probe_after_scdt = false;
	int ret;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	g_trace_mode = getenv("GC573_TEARDOWN_TRACE") != NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--led") == 0 && i + 1 < argc) {
			led_spec = argv[++i];
		} else if (strncmp(argv[i], "--led=", 6) == 0) {
			led_spec = argv[i] + 6;
		} else if (strcmp(argv[i], "--edid") == 0 && i + 1 < argc) {
			edid_spec = argv[++i];
		} else if (strncmp(argv[i], "--edid=", 7) == 0) {
			edid_spec = argv[i] + 7;
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		} else if (argv[i][0] != '-') {
			pci_addr = argv[i];
		} else {
			fprintf(stderr, "[LED] Unknown option '%s' ignored\n",
				argv[i]);
		}
	}

	/* --led wins; EVERLIVE_LED is the GUI-friendly fallback. */
	if (!led_spec || !*led_spec)
		led_spec = getenv("EVERLIVE_LED");
	if (led_spec && *led_spec &&
	    !parse_led_color(led_spec, &g_led_r, &g_led_g, &g_led_b)) {
		fprintf(stderr, "[LED] Unknown colour '%s'; using default cyan #00ccff\n",
			led_spec);
		g_led_r = 0x00;
		g_led_g = 0xcc;
		g_led_b = 0xff;
	}
	if (!edid_spec || !*edid_spec)
		edid_spec = GC573_EDID_PROFILE_DEFAULT;

	printf("==================================================================\n");
	printf("  EverLive Userspace VFIO Streamer [INTERN TEST - EXPERIMENTAL]  \n");
	printf("==================================================================\n\n");
	if (g_trace_mode)
		printf("[TRACE] Teardown trace enabled: press T while streaming to start the phase test.\n");

	memset(&video, 0, sizeof(video));
	memset(&audio, 0, sizeof(audio));
	memset(&current_sig, 0, sizeof(current_sig));
	memset(&watch, 0, sizeof(watch));

	/*
	 * Let VFIO pin as much as the process is permitted to.  Native 4K needs
	 * a 15.8 MiB frame, so the launcher also raises the limit before exec;
	 * if the hard limit is still too small, capture fails cleanly instead
	 * of configuring a mismatched scaled pipeline.
	 */
	raise_memlock_limit();

	/* 0. Yield VFIO if background hardware/LED daemon is holding the device */
	stop_led_daemon();
	usleep(150000);

	/* 1. Initialize VFIO interface. */
	ret = gc573_vfio_init(&ctx, pci_addr);
	if (ret < 0) {
		fprintf(stderr, "[ERROR] Failed to initialize VFIO: %d\n", ret);
		fprintf(stderr, "Ensure device is bound to vfio-pci:\n");
		fprintf(stderr, "  sudo ./tools/opengc573-userd/vfio_pci_bind.sh --bind\n");
		return 1;
	}

	/* 2. Initialize FPGA bridge, rails, HDMI link, and assert HPD. */
	ret = gc573_user_hw_init(ctx.bar0);
	if (ret < 0) {
		fprintf(stderr, "[ERROR] FPGA/bridge init failed: %d\n", ret);
		gc573_vfio_cleanup(&ctx);
		return 1;
	}

	/*
	 * 3. Bring up the IT6664 splitter first.  The source only sees the
	 *    splitter's upstream EDID/HPD, so this must happen before the
	 *    IT6805 can lock to TX1.
	 */
	ret = gc573_user_it6664_init(ctx.bar0);
	if (ret < 0) {
		fprintf(stderr, "[ERROR] IT6664 init failed: %d\n", ret);
		gc573_vfio_cleanup(&ctx);
		return 1;
	}

	/*
	 * 3b. Select the EDID profile before the IT6805 init step programs the
	 *     DDC RAM and releases its HPD.  An unusable profile falls back to
	 *     the verified default instead of aborting the stream.
	 */
	if (gc573_user_edid_select(edid_spec, NULL) < 0) {
		fprintf(stderr, "[EDID] Unknown or unusable profile '%s'; using default\n",
			edid_spec);
		gc573_user_edid_select(GC573_EDID_PROFILE_DEFAULT, NULL);
	}
	printf("[EDID] Profile: %s\n", gc573_user_edid_active_name());

	/* 4. Initialize the IT6805 HDMI 2.0 receiver. */
	ret = gc573_user_it6805_init(ctx.bar0);
	if (ret < 0) {
		fprintf(stderr, "[ERROR] IT6805 init failed: %d\n", ret);
		gc573_vfio_cleanup(&ctx);
		return 1;
	}

	/*
	 * 4b. Program the selected profile explicitly before the source-facing
	 *     HDMI handshake is released.  No-op when the init step above already
	 *     latched the same table.
	 */
	ret = gc573_user_program_edid_by_name(ctx.bar0,
					      gc573_user_edid_active_name(), NULL);
	if (ret < 0) {
		fprintf(stderr, "[ERROR] EDID programming failed: %d\n", ret);
		gc573_vfio_cleanup(&ctx);
		return 1;
	}

	/* 5. Power TX1, publish the upstream EDID and release source HPD. */
	ret = gc573_user_it6664_enable_source(ctx.bar0);
	if (ret < 0) {
		fprintf(stderr, "[ERROR] IT6664 source enable failed: %d\n", ret);
		gc573_vfio_cleanup(&ctx);
		return 1;
	}

	/* 6. Keep the selected LED colour to indicate active userspace streaming. */
	gc573_user_set_led_rgb(ctx.bar0, g_led_r, g_led_g, g_led_b);
	printf("[LED] Active colour #%02x%02x%02x\n",
	       g_led_r, g_led_g, g_led_b);

	/* 7. Setup MSI Interrupt via EventFD (video DMA completion). */
	ret = gc573_vfio_setup_msi(&ctx);
	if (ret < 0)
		pr_warn("Interrupt setup failed; falling back to IRQ polling");

	/* 8. Initialize SDL2 display window and YUYV texture. */
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
		fprintf(stderr, "[ERROR] SDL_Init failed: %s\n", SDL_GetError());
		gc573_vfio_cleanup(&ctx);
		return 1;
	}

	/*
	 * Audio is optional.  A missing or broken audio backend must never
	 * prevent the verified video path from running.
	 */
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
		audio.sdl_ready = true;
	} else {
		printf("[AUDIO] SDL audio unavailable (%s); continuing video-only\n",
		       SDL_GetError());
	}

	window = SDL_CreateWindow(
		"EverLive [USERSPACE VFIO ENGINE - EXPERIMENTAL]",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT,
		SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!window) {
		fprintf(stderr, "[ERROR] SDL_CreateWindow failed: %s\n",
			SDL_GetError());
		SDL_Quit();
		gc573_vfio_cleanup(&ctx);
		return 1;
	}

	SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer)
		renderer = SDL_CreateRenderer(window, -1, 0);
	if (!renderer) {
		fprintf(stderr, "[ERROR] SDL_CreateRenderer failed: %s\n",
			SDL_GetError());
		SDL_DestroyWindow(window);
		SDL_Quit();
		gc573_vfio_cleanup(&ctx);
		return 1;
	}
	SDL_RenderSetVSync(renderer, 0);

	texture = NULL;

	/* Map the two HDMI audio ping-pong buffers (96 KB total). */
	if (audio.sdl_ready) {
		audio.playback.scratch = malloc(AUDIO_SCRATCH_BYTES);
		if (audio.playback.scratch) {
			audio.playback.scratch_size = AUDIO_SCRATCH_BYTES;
		} else {
			pr_warn("Audio scratch allocation failed; continuing video-only");
			audio.sdl_ready = false;
		}
	}
	if (gc573_user_audio_init(&audio.dma, &ctx, ctx.bar0) == 0) {
		audio.dma_ready = true;
	} else {
		pr_warn("Audio DMA mapping failed; continuing video-only");
	}

	printf("[*] Userspace streamer active. Press 'q' or ESC to exit, 'f' for fullscreen.\n");
	render_standby(window, renderer,
		       "EverLive [USERSPACE VFIO] — Waiting for HDMI signal...");

	last_stats = now_ms();
	last_status_poll = 0;
	last_service = 0;
	last_frame_rate_probe = 0;

	while (g_running) {
		SDL_Event event;
		uint64_t now;

		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				if (g_trace_mode && streaming)
					g_trace_requested = true;
				g_running = 0;
			} else if (event.type == SDL_KEYDOWN) {
				if (event.key.keysym.sym == SDLK_ESCAPE ||
				    event.key.keysym.sym == SDLK_q) {
					if (g_trace_mode && streaming)
						g_trace_requested = true;
					g_running = 0;
				} else if (event.key.keysym.sym == SDLK_t &&
					   g_trace_mode && streaming) {
					g_trace_requested = true;
					g_running = 0;
				} else if (event.key.keysym.sym == SDLK_f) {
					uint32_t flags = SDL_GetWindowFlags(window);

					SDL_SetWindowFullscreen(window,
						(flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ?
						0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
				}
			}
		}
		if (!g_running)
			break;

		now = now_ms();

		/*
		 * Cheap signal probe: reads status, the latched timing registers and
		 * the AVI InfoFrame.  The 24 ms pixel-clock sample is only used when
		 * a candidate change is already pending, or as a slow 10 s safety
		 * net, so the 120 Hz render loop is never blocked periodically.
		 */
		if (now - last_status_poll >=
		    (streaming ? STATUS_POLL_STREAM_MS : STATUS_POLL_WAIT_MS)) {
			last_status_poll = now;

			/* Keep the splitter RX-lock and AVMute release serviced. */
			if (!streaming || now - last_service >= STATUS_SERVICE_MS) {
				last_service = now;
				gc573_user_it6664_poll(ctx.bar0, NULL);
				if (streaming)
					gc573_user_it6805_unmute(ctx.bar0);
			}

			if (streaming) {
				struct gc573_user_video_signal probe = { 0 };
				enum gc573_user_signal_probe kind =
					GC573_USER_SIGNAL_PROBE_TIMING;

				if (watch.active || rate_probe_after_scdt ||
				    now - last_frame_rate_probe >=
				    FRAME_RATE_PROBE_MS) {
					last_frame_rate_probe = now;
					kind = GC573_USER_SIGNAL_PROBE_RATE;
				}

				/*
				 * The rate probe blocks for ~24 ms.  Pull any completed
				 * audio period into the SDL queue first so playback has
				 * the full 80 ms cushion while the measurement runs.
				 */
				if (kind == GC573_USER_SIGNAL_PROBE_RATE) {
					audio_drain_dma(&audio);
					audio.last_dma_poll = now;
				}

				ret = gc573_user_it6805_poll_signal_ex(ctx.bar0,
								 &probe, kind);
				if (kind == GC573_USER_SIGNAL_PROBE_RATE)
					rate_probe_after_scdt = false;
				if (ret < 0) {
					pr_warn("IT6805 signal probe failed: %d", ret);
				} else if (!probe.scdt_locked) {
					/*
					 * Sources briefly drop SCDT while changing modes.
					 * Keep the current DMA running for one more probe so a
					 * hot switch does not turn into a full stop/restart.
					 */
					signal_lost_hits++;
					rate_probe_after_scdt = true;
					if (signal_lost_hits >= SIGNAL_CHANGE_HITS) {
						printf("[VIDEO] HDMI signal lost; stopping capture.\n");
						audio_stop_capture(&audio);
						ret = gc573_user_it6805_audio_disable(ctx.bar0);
						if (ret < 0)
							pr_warn("IT6805 audio disable failed: %d",
								ret);
						preview_stop();
						gc573_user_video_stop_timeout(&video,
									      VIDEO_STOP_HOT_MS);
						streaming = false;
						have_signal = false;
						signal_lost_hits = 0;
						memset(&current_sig, 0,
						       sizeof(current_sig));
						memset(&watch, 0, sizeof(watch));
						output_width = 0;
						output_height = 0;
						if (texture) {
							SDL_DestroyTexture(texture);
							texture = NULL;
						}
						render_standby(window, renderer,
							       "EverLive [USERSPACE VFIO] — Signal lost, waiting...");
					}
				} else if (!probe.width || !probe.height) {
					/* Timing register snapshot not ready; retry next poll. */
				} else {
					signal_lost_hits = 0;
					if (!probe.avi_valid)
						probe.colorspace = current_sig.colorspace;

					if (watch_update(&watch, &current_sig, &probe)) {
						uint64_t reconfigure_start = now_ms();
						struct gc573_user_video_signal fresh = { 0 };

						memset(&watch, 0, sizeof(watch));

						/*
						 * Silence audio before the full 175 ms measurement.
						 * The preview thread keeps draining video DMA.
						 */
						audio_stop_capture(&audio);
						ret = gc573_user_it6805_poll_signal(ctx.bar0,
									    &fresh);
						if (ret < 0 || !signal_usable(&fresh)) {
							printf("[VIDEO] Signal vanished during re-measurement; waiting.\n");
							preview_stop();
							gc573_user_video_stop_timeout(&video,
										      VIDEO_STOP_HOT_MS);
							streaming = false;
							have_signal = false;
							memset(&current_sig, 0,
							       sizeof(current_sig));
							output_width = 0;
							output_height = 0;
							if (texture) {
								SDL_DestroyTexture(texture);
								texture = NULL;
							}
							render_standby(window, renderer,
								       "EverLive [USERSPACE VFIO] — Signal lost, waiting...");
						} else if (have_signal &&
							   signal_parameters_equal(&fresh,
									   &current_sig,
									   true)) {
							pr_info("Signal probe was transient; continuing current mode");
							stream_reset_audio(&audio);
						} else {
							unsigned int new_out_w = 0;
							unsigned int new_out_h = 0;
							unsigned int new_buffers = 0;

							printf("[VIDEO] Signal change: %ux%u%s @ %u.%02u Hz, %s -> %ux%u%s @ %u.%02u Hz, %s\n",
							       current_sig.width,
							       current_sig.height,
							       current_sig.interlaced ? "i" : "p",
							       current_sig.frame_rate_x100 / 100,
							       current_sig.frame_rate_x100 % 100,
							       colorspace_name(current_sig.colorspace),
							       fresh.width, fresh.height,
							       fresh.interlaced ? "i" : "p",
							       fresh.frame_rate_x100 / 100,
							       fresh.frame_rate_x100 % 100,
							       colorspace_name(fresh.colorspace));

							preview_stop();
							gc573_user_video_stop_timeout(&video,
										      VIDEO_STOP_HOT_MS);
							ret = compute_output_and_buffers(&fresh,
										 &new_out_w,
										 &new_out_h,
										 &new_buffers);

							if (ret == 0)
								ret = configure_and_start(&ctx, &video,
											  &video_ready,
											  &fresh,
											  new_out_w,
											  new_out_h,
											  new_buffers);
							if (ret == 0)
								ret = ensure_texture(renderer,
										     &texture,
										     new_out_w,
										     new_out_h);
							if (ret == 0)
								ret = preview_start(
									&video,
									(size_t)new_out_w *
									new_out_h * 2U);

							if (ret < 0) {
								if (ret != -ENOMEM)
									pr_err("Hot switch failed: %d", ret);
								gc573_user_video_stop_timeout(
									&video,
									VIDEO_STOP_HOT_MS);
								streaming = false;
								have_signal = false;
								memset(&current_sig, 0,
								       sizeof(current_sig));
								output_width = 0;
								output_height = 0;
								if (texture) {
									SDL_DestroyTexture(texture);
									texture = NULL;
								}
								render_standby(window, renderer,
									       ret == -ENOMEM ?
									       "EverLive [USERSPACE VFIO] — Native mode exceeds RLIMIT_MEMLOCK" :
									       "EverLive [USERSPACE VFIO] — Hot switch failed, waiting...");
							} else {
								streaming = true;
								have_signal = true;
								rate_probe_after_scdt = false;
								current_sig = fresh;
								output_width = new_out_w;
								output_height = new_out_h;
								video_aspect = display_aspect_for_signal(
									&fresh);
								input_width = fresh.width;
								input_height = fresh.height;
								input_rate_x100 =
									fresh.frame_rate_x100;
								last_frames = video.frames;
								last_stats = now_ms();
								stream_reset_audio(&audio);
								printf("[VIDEO] Hot switch completed in %llu ms\n",
								       (unsigned long long)(now_ms() -
										reconfigure_start));
							}
						}
					}
				}
			} else {
				struct gc573_user_video_signal light = { 0 };

				ret = gc573_user_it6805_poll_status(ctx.bar0, &light);
				if (ret < 0) {
					pr_warn("IT6805 status poll failed: %d", ret);
				} else if (light.scdt_locked) {
					struct gc573_user_video_signal sig = { 0 };

					printf("[VIDEO] SCDT locked; measuring input timing...\n");
					ret = gc573_user_it6805_poll_signal(ctx.bar0,
									    &sig);
					if (ret == 0 && signal_usable(&sig)) {
						unsigned int new_out_w = 0;
						unsigned int new_out_h = 0;
						unsigned int new_buffers = 0;

						ret = compute_output_and_buffers(&sig,
									 &new_out_w,
									 &new_out_h,
									 &new_buffers);
						if (ret == 0)
							ret = configure_and_start(&ctx, &video,
										  &video_ready,
										  &sig,
										  new_out_w,
										  new_out_h,
										  new_buffers);
						if (ret == 0)
							ret = ensure_texture(renderer,
									     &texture,
									     new_out_w,
									     new_out_h);
						if (ret == 0)
							ret = preview_start(
								&video,
								(size_t)new_out_w *
								new_out_h * 2U);

						if (ret < 0) {
							if (ret != -ENOMEM)
								pr_err("Capture start failed: %d", ret);
							gc573_user_video_stop_timeout(
								&video,
								VIDEO_STOP_HOT_MS);
							render_standby(window, renderer,
								       ret == -ENOMEM ?
								       "EverLive [USERSPACE VFIO] — Native mode exceeds RLIMIT_MEMLOCK" :
								       "EverLive [USERSPACE VFIO] — Waiting for HDMI signal...");
						} else {
							streaming = true;
							have_signal = true;
							signal_lost_hits = 0;
							rate_probe_after_scdt = false;
							current_sig = sig;
							output_width = new_out_w;
							output_height = new_out_h;
							video_aspect =
								display_aspect_for_signal(&sig);
							input_width = sig.width;
							input_height = sig.height;
							input_rate_x100 =
								sig.frame_rate_x100;
							last_frames = video.frames;
							last_stats = now_ms();
							stream_reset_audio(&audio);
						}
					} else if (ret == 0) {
						pr_warn("SCDT set but timing not ready (5V=%u %ux%u %u kHz %u Hz)",
							sig.five_volt_present,
							sig.width, sig.height,
							sig.pixel_clock_khz,
							sig.frame_rate_hz);
					}
				}
			}
		}

		if (streaming && g_preview.stalled) {
			u8 *b = (u8 *)ctx.bar0;

			pr_err("Video DMA stalled; stopping capture. Diagnostic dump:\n"
			       "  FPGA VIP Status: 0x%08x | Res: %u x %u | VIP Ctrl: 0x%08x | DDR: 0x%08x\n"
			       "  DMA Control: 0x%08x | Channel: 0x%08x | IRQ Stat: 0x%08x | IRQ En: 0x%08x",
			       readl(b + 0x01004), readl(b + 0x01008) & 0xffff,
			       readl(b + 0x0100c) & 0xffff,
			       readl(b + 0x01000), readl(b + 0x0107c),
			       readl(b + 0x00304), readl(b + 0x00300),
			       readl(b + 0x00010), readl(b + 0x0001c));
			preview_stop();
			gc573_user_video_stop_timeout(&video, VIDEO_STOP_HOT_MS);
			streaming = false;
			have_signal = false;
			signal_lost_hits = 0;
			memset(&current_sig, 0, sizeof(current_sig));
			memset(&watch, 0, sizeof(watch));
			output_width = 0;
			output_height = 0;
			audio_stop_capture(&audio);
			gc573_user_it6805_audio_disable(ctx.bar0);
			if (texture) {
				SDL_DestroyTexture(texture);
				texture = NULL;
			}
			render_standby(window, renderer,
				       "EverLive [USERSPACE VFIO] — DMA stall, reconnecting...");
		}

		if (streaming) {
			void *pixels = NULL;

			if (preview_take(&pixels) && texture) {
				int uret = SDL_UpdateTexture(texture, NULL,
							     pixels,
							     output_width * 2);

				if (uret < 0) {
					static bool reported = false;

					if (!reported) {
						printf("[SDL-ERR] SDL_UpdateTexture failed: %s\n",
						       SDL_GetError());
						reported = true;
					}
				}
				render_frame(window, renderer, texture,
					     video_aspect);
			} else {
				SDL_Delay(2);
			}
		} else {
			SDL_Delay(20);
		}

		if (streaming) {
			now = now_ms();
			if (audio.sdl_ready && audio.dma_ready &&
			    now - audio.last_it6805_poll >=
				    AUDIO_IT6805_POLL_MS) {
				audio.last_it6805_poll = now;
				audio_poll_format(&audio, ctx.bar0, now);
			}
			if (now - audio.last_dma_poll >= AUDIO_DMA_POLL_MS) {
				audio.last_dma_poll = now;
				audio_drain_dma(&audio);
			}
		}

		/* Update title stats once per second. */
		now = now_ms();
		if (now - last_stats >= 1000) {
			uint64_t frames = video.frames;
			double fps = (double)(frames - last_frames) * 1000.0 /
				     (double)(now - last_stats);
			char title[256];

			if (streaming) {
				snprintf(title, sizeof(title),
					 "EverLive [USERSPACE VFIO] — %ux%u%s @ %u.%02u Hz -> %ux%u YUYV @ %.1f FPS",
					 input_width, input_height,
					 current_sig.interlaced ? "i" : "p",
					 input_rate_x100 / 100,
					 input_rate_x100 % 100,
					 output_width, output_height, fps);
				printf("[STREAM] %ux%u%s @ %u.%02u Hz -> %ux%u | FPS: %.1f | Frames: %lu\n",
				       input_width, input_height,
				       current_sig.interlaced ? "i" : "p",
				       input_rate_x100 / 100, input_rate_x100 % 100,
				       output_width, output_height,
				       fps, (unsigned long)frames);
				if (audio.streaming && audio.playback.dev) {
					uint64_t dma_delta = audio.dma_blocks -
							     audio.last_dma_blocks;
					uint64_t queued_delta = audio.queued_blocks -
								audio.last_queued_blocks;
					uint64_t dropped_delta = audio.dropped_blocks -
								 audio.last_dropped_blocks;

					printf("[AUDIO] %u Hz %u ch %u-bit | DMA %lu blocks/s | queued %lu/s | dropped %lu/s | SDL %u B | peak %u\n",
					       audio.format_rate,
					       audio.format_channels,
					       audio.format_bits,
					       (unsigned long)dma_delta,
					       (unsigned long)queued_delta,
					       (unsigned long)dropped_delta,
					       SDL_GetQueuedAudioSize(audio.playback.dev),
					       audio.peak);
					audio.last_dma_blocks = audio.dma_blocks;
					audio.last_queued_blocks =
						audio.queued_blocks;
					audio.last_dropped_blocks =
						audio.dropped_blocks;
					audio.peak = 0;
				}
			} else {
				snprintf(title, sizeof(title),
					 "EverLive [USERSPACE VFIO] — Waiting for HDMI signal...");
			}
			if (g_trace_mode)
				strncat(title, " | T=teardown trace",
					sizeof(title) - strlen(title) - 1);
			SDL_SetWindowTitle(window, title);
			last_frames = frames;
			last_stats = now;
		}
	}

	if (g_trace_requested) {
		audio_cleanup(&audio);
		gc573_user_it6805_audio_disable(ctx.bar0);
		run_teardown_trace(&ctx, &video, video_ready, window, renderer,
				   texture);
		return 0;
	}

	printf("\n[*] Stopping Userspace Streamer...\n");

	/*
	 * Start the daemon before stopping the video DMA.  It re-arms the
	 * selected colour at 20 Hz, so the LED never has to fall back
	 * to the red blink while we tear the stream down.
	 */
	bool daemon_started = start_led_daemon(&ctx, false);

	audio_cleanup(&audio);
	gc573_user_it6805_audio_disable(ctx.bar0);
	preview_stop();
	if (video_ready)
		gc573_user_video_cleanup(&video);
	if (texture)
		SDL_DestroyTexture(texture);
	if (renderer)
		SDL_DestroyRenderer(renderer);
	if (window)
		SDL_DestroyWindow(window);
	SDL_Quit();

	/*
	 * If the early handoff failed, fall back to the old late handoff.
	 * Otherwise the daemon already owns the inherited VFIO descriptors;
	 * closing ours is safe because it holds the last references.
	 */
	if (!daemon_started)
		start_led_daemon(&ctx, true);
	gc573_vfio_cleanup(&ctx);

	printf("[✓] EverLive Userspace Streamer closed cleanly.\n");
	return 0;
}

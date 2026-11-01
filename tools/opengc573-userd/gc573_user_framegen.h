/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_framegen.h — lsfg-vk frame generation + PS5 phase-locked dedup
 *
 * EverLive Experimental no longer ships a custom optical-flow shader.  Frame
 * generation is delegated to the system's native lsfg-vk implicit Vulkan layer
 * (Lossless Scaling Frame Generation 2x on Linux), which the SDL2 "vulkan"
 * renderer feeds directly.  This module is the glue:
 *
 *   gc573_fg_probe_system()   locate the layer + lsfg-vk.dll, build the env
 *   gc573_fg_apply_env()      export LSFGVK_* before the Vulkan instance exists
 *   gc573_fg_write_toml()     write ~/.config/everlive-experimental/lsfg.toml
 *   gc573_fg_cadence_*        PS5 30-in-60 phase-locked 2:1 deduplicator
 *   gc573_framegen_*          capture-side ring + present-side take/release
 *
 * Three explicit modes, strictly 2x:
 *
 *   OFF         presentation runs on SDL's "opengl" renderer; no Vulkan
 *               instance is ever created, so lsfg-vk is 100% bypassed.
 *   60_TO_120   every captured 60 Hz frame is presented exactly once;
 *               lsfg-vk turns each present into two (LSFGVK_MULTIPLIER=2).
 *   30_TO_60    the 2:1 HDMI parity deduplicator keeps ~30 of the 60 frames
 *               per second, so lsfg-vk's 2x lands exactly on 60.0 FPS.
 *
 * The module has no SDL, GL or Vulkan dependency of its own: the capture side
 * is pure CPU and the presentation side is driven by the streamer.
 */

#ifndef GC573_USER_FRAMEGEN_H
#define GC573_USER_FRAMEGEN_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Frame generation modes.  The values are shared with the IPC wire format. */
enum gc573_fg_mode {
	GC573_FG_OFF = 0,
	GC573_FG_60_TO_120 = 1,
	GC573_FG_30_TO_60 = 2,
};

/* Cadence classification result of one captured YUYV frame. */
enum gc573_fg_frame_class {
	GC573_FG_DUPLICATE = 0,	/* HDMI repeat inside the 60 Hz container */
	GC573_FG_UNIQUE = 1,	/* genuine new game frame */
	GC573_FG_SCENE_CUT = 2,	/* reserved; the parity lock needs no cut */
};

/*
 * Deduplicator tuning.  mad values are Mean Absolute Luma Differences in 8-bit
 * code values over the 4x2 box-filtered 64x36 grid (0 .. 255).
 */
#define GC573_FG_GRID_W			64
#define GC573_FG_GRID_H			36
#define GC573_FG_GRID_MARGIN_PCT	12	/* skip letterbox/ticker bars */

/* A source faster than this is treated as a duplicated 60 Hz container. */
#define GC573_FG_DEDUP_MIN_HZ_X100	4500U
/* Parity EMA smoothing and the lock separation. */
#define GC573_FG_MAD_EMA_ALPHA		0.20f
#define GC573_FG_LOCK_RATIO		1.60f
#define GC573_FG_LOCK_BIAS		0.15f
/* Instantaneous phase-slip recovery (A, A, A, B, B). */
#define GC573_FG_SLIP_MAD_FACTOR	2.0f
#define GC573_FG_SLIP_MAD_MIN		2.0f

struct gc573_fg_cadence {
	uint32_t width;
	uint32_t height;
	uint8_t *prev_raw;	/* 4x2-filtered grid of raw HDMI frame k-1 */
	uint8_t *scratch;	/* grid of the frame being classified */
	bool have_prev;

	uint32_t raw_frames;
	uint32_t unique_frames;
	uint32_t duplicates;
	uint32_t parity_flips;

	/* EMA of the transition MAD by raw-frame parity. */
	float even_mad_ema;
	float odd_mad_ema;
	/* Parity on which genuine new frames arrive (0 or 1). */
	int active_parity;
	bool locked;

	float last_mad;
	float prev_raw_mad;

	uint64_t last_unique_us;
	uint64_t dt_unique_us;
	double smoothed_dt_us;
};

struct gc573_fg_config {
	int mode;		/* enum gc573_fg_mode */
	float flow_scale;	/* 0.50 .. 1.00 (LSFGVK_FLOW_SCALE) */
	bool perf_mode;		/* LSFGVK_PERFORMANCE_MODE */
	/*
	 * Keep lsfg-vk from enlarging the Vulkan swapchain.  EverLive only
	 * uploads a 2D capture texture plus OSD, so the default 1-2 extra
	 * buffered frames are pure input latency.  Maps to
	 * LSFGVK_PRESERVE_SWAPCHAIN_IMAGE_COUNT=1.
	 */
	bool low_latency;
};

/* Result of the lsfg-vk installation probe. */
struct gc573_fg_probe {
	bool layer_found;
	bool dll_found;
	char layer_dir[PATH_MAX];
	char dll_path[PATH_MAX];
};

struct gc573_fg_stats {
	int mode;
	bool layer_found;
	bool dll_found;
	char dll_path[PATH_MAX];
	bool vulkan_active;	/* renderer really is the Vulkan/lsfg-vk path */
	bool deduplicator_active;
	float flow_scale;
	bool perf_mode;
	bool low_latency;

	double present_fps;	/* real SDL_RenderPresent calls per second */
	double output_fps;	/* present_fps * 2 when lsfg-vk is usable */
	double raw_fps;		/* captured HDMI frames per second */
	double unique_fps;	/* deduplicated game frames per second */

	uint32_t unique_frames;
	uint32_t duplicates;
	uint32_t parity_flips;
	uint32_t input_dropped;
	unsigned int queued;
};

/* ------------------------------------------------------------------ */
/* Installation probe & environment                                    */
/* ------------------------------------------------------------------ */

/**
 * gc573_fg_probe_system - Locate the lsfg-vk layer and lsfg-vk.dll
 * @out: Receives the result (never NULL)
 *
 * Scans the Vulkan implicit-layer directories for an lsfg JSON manifest and
 * resolves lsfg-vk.dll through LSFGVK_DLL_PATH, ~/.config/lsfg-vk/conf.toml,
 * the two default Steam roots and every extra library in libraryfolders.vdf.
 * Pure filesystem work: no external binaries are executed.
 */
void gc573_fg_probe_system(struct gc573_fg_probe *out);

/**
 * gc573_fg_apply_env - Export LSFGVK_* for the next Vulkan instance
 * @cfg: Requested mode/flow/perf settings
 * @out: Probe result; refreshed by this call
 *
 * Re-runs the probe, then sets LSFGVK_ENV/LSFGVK_MULTIPLIER/LSFGVK_FLOW_SCALE
 * (locale-independent) and, when available, LSFGVK_DLL_PATH.  Returns true
 * when both the layer and the DLL are present, i.e. lsfg-vk will actually work.
 */
bool gc573_fg_apply_env(const struct gc573_fg_config *cfg,
			struct gc573_fg_probe *out);

/**
 * gc573_fg_write_toml - Write the diagnostic lsfg.toml profile
 *
 * Mirrors the exported environment into
 * ~/.config/everlive-experimental/lsfg.toml (version 2, [global] allow_fp16,
 * dll path when known, [[profile]] name = "everlive").
 */
void gc573_fg_write_toml(const struct gc573_fg_config *cfg,
			 const struct gc573_fg_probe *probe);

/** Locale-independent "0.85" formatting used for the env and TOML. */
void gc573_fg_format_scale(float flow_scale, char *buf, size_t len);

/** "off" | "60to120" | "30to60" */
const char *gc573_fg_mode_name(int mode);
/** Parses the mode plus the "2x" alias; returns -1 when unknown. */
int gc573_fg_mode_parse(const char *name);

/* ------------------------------------------------------------------ */
/* Phase-locked 2:1 HDMI parity deduplicator                           */
/* ------------------------------------------------------------------ */

void gc573_fg_cadence_init(struct gc573_fg_cadence *cad,
			   uint32_t width, uint32_t height);
void gc573_fg_cadence_reset(struct gc573_fg_cadence *cad);
void gc573_fg_cadence_free(struct gc573_fg_cadence *cad);

/**
 * gc573_fg_cadence_classify - Decide whether a raw HDMI frame is new
 * @cad: Detector state
 * @yuyv: Full packed YUYV frame (width * height * 2 bytes)
 * @now_us: CLOCK_MONOTONIC capture completion time in microseconds
 *
 * Compares against the immediately preceding raw frame, keeps per-parity
 * motion EMAs and emits on the locked parity only.  Returns
 * GC573_FG_UNIQUE or GC573_FG_DUPLICATE.
 */
int gc573_fg_cadence_classify(struct gc573_fg_cadence *cad,
			      const uint8_t *yuyv, uint64_t now_us);

/** Smoothed unique (game) frame rate; 0.0 until two frames were accepted. */
double gc573_fg_cadence_unique_fps(const struct gc573_fg_cadence *cad);

/* ------------------------------------------------------------------ */
/* Capture ring & presentation hand-off                                */
/* ------------------------------------------------------------------ */

/*
 * Per-slot frame filter, called once per accepted frame *after* the copy into
 * the ring.  The streamer uses it to run the HDR tonemapper on the ring copy.
 */
typedef void (*gc573_fg_frame_filter)(uint8_t *yuyv, uint32_t width,
				      uint32_t height, void *opaque);

struct gc573_framegen;

/** Build the ring for @cfg.  Never touches Vulkan; never fails for GL. */
struct gc573_framegen *gc573_framegen_create(const struct gc573_fg_config *cfg);
void gc573_framegen_destroy(struct gc573_framegen *fg);

void gc573_framegen_set_config(struct gc573_framegen *fg,
			       const struct gc573_fg_config *cfg);
void gc573_framegen_get_config(const struct gc573_framegen *fg,
			       struct gc573_fg_config *cfg);
void gc573_framegen_set_frame_filter(struct gc573_framegen *fg,
				     gc573_fg_frame_filter fn, void *opaque);

/**
 * gc573_framegen_resize - Match the capture geometry
 *
 * Reallocates the 3-slot ring; cheap no-op when unchanged.
 */
int gc573_framegen_resize(struct gc573_framegen *fg,
			  uint32_t width, uint32_t height);

/**
 * gc573_framegen_set_source_hz - Tell the module the HDMI input rate
 * @hz_x100: Signal rate in 1/100 Hz (0 = unknown)
 *
 * 30to60 deduplicates only while the input is faster than 45 Hz; a native
 * 30 Hz signal is passed through 1:1.
 */
void gc573_framegen_set_source_hz(struct gc573_framegen *fg,
				  unsigned int hz_x100);

void gc573_framegen_set_probe(struct gc573_framegen *fg,
			      const struct gc573_fg_probe *probe);
void gc573_framegen_set_vulkan_active(struct gc573_framegen *fg, bool active);

/**
 * gc573_framegen_input - Feed one captured frame from the DMA thread
 *
 * Accepts every frame in 60to120 (and in 30to60 when the source is native
 * <=45 Hz), otherwise runs the parity deduplicator.  Thread-safe.
 */
void gc573_framegen_input(struct gc573_framegen *fg, const uint8_t *yuyv,
			  uint64_t frame_us);

/**
 * gc573_framegen_take - Pop the newest queued frame for presentation
 * @pixels: Receives the YUYV buffer
 *
 * Drains older queued frames so latency stays minimal.  Returns true only when
 * a genuinely new frame is available - the caller must then upload, present
 * ONCE and call gc573_framegen_release().  Returning false means "do not call
 * SDL_RenderPresent", which is what keeps lsfg-vk's swapchain cadence true.
 */
bool gc573_framegen_take(struct gc573_framegen *fg, const uint8_t **pixels);

/** Release the slot returned by the last successful take. */
void gc573_framegen_release(struct gc573_framegen *fg);

/**
 * gc573_framegen_wait - Block until an unread frame exists (or timeout)
 * @fg: Capture ring
 * @timeout_us: Maximum wait in microseconds
 *
 * The DMA thread signals an internal condition variable as soon as an accepted
 * frame is queued, so the render loop can wake within microseconds of the PCIe
 * MSI completion instead of sleeping through a 120 Hz VBlank with SDL_Delay().
 * Returns true when a frame is already waiting (or arrived during the wait),
 * false on timeout or when a presentation is still in flight.
 */
bool gc573_framegen_wait(struct gc573_framegen *fg, unsigned int timeout_us);

/** Snapshot of the live counters (thread-safe). */
void gc573_framegen_get_stats(struct gc573_framegen *fg,
			      struct gc573_fg_stats *out);

#endif /* GC573_USER_FRAMEGEN_H */

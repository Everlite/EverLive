/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_ipc.h — Control socket for the EverLive userspace streamer
 *
 * The streamer owns the VFIO container and the BAR0 mapping exclusively, so
 * the Control Hub cannot open the card a second time while a session runs.
 * Instead of a second process instance it talks to the running engine over a
 * Unix domain socket using newline-delimited JSON-RPC 2.0:
 *
 *     {"jsonrpc":"2.0","id":1,"method":"status.get"}
 *     {"jsonrpc":"2.0","id":1,"result":{...}}
 *
 * Phase 1 is read-only (hello / ping / status.get).  The main loop publishes a
 * snapshot once per second through a seqlock, so the control thread never
 * blocks the DMA or render path and never touches a shared mutex.
 *
 * The socket lives at $XDG_RUNTIME_DIR/everlive.sock (fallback
 * /run/user/<uid>, then /tmp/everlive-<uid>) and is protected by a lock file.
 * The lock file — not the socket — is the authoritative "is a streamer
 * running?" signal: a crashed process leaves a stale socket behind, and only
 * flock() reliably distinguishes that from a live owner.
 */

#ifndef GC573_USER_IPC_H
#define GC573_USER_IPC_H

#include <stdbool.h>
#include <stdint.h>

#define GC573_IPC_PROTO_VERSION	1
#define GC573_IPC_MAX_CLIENTS	8
#define GC573_IPC_MAX_FRAME	65536U
#define GC573_IPC_CMD_RING	32U

/* Neutral codes on the wire; the streamer maps them onto its own enums. */
enum gc573_ipc_led_mode {
	GC573_IPC_LED_STATIC = 0,
	GC573_IPC_LED_WAVE = 1,
	GC573_IPC_LED_RAINBOW = 2,
	GC573_IPC_LED_OFF = 3,
};

enum gc573_ipc_osd_pos {
	GC573_IPC_OSD_TR = 0,
	GC573_IPC_OSD_BR = 1,
	GC573_IPC_OSD_BL = 2,
	GC573_IPC_OSD_TL = 3,
};

enum gc573_ipc_aspect {
	GC573_IPC_ASPECT_AUTO = 0,
	GC573_IPC_ASPECT_16_9 = 1,
	GC573_IPC_ASPECT_4_3 = 2,
	GC573_IPC_ASPECT_STRETCH = 3,
};

/* Output mode of the running engine. */
enum gc573_ipc_mode {
	GC573_IPC_MODE_HEADLESS = 0,
	GC573_IPC_MODE_GAMING = 1,
};

enum gc573_ipc_cmd_kind {
	GC573_IPC_CMD_LED_SET = 0,
	GC573_IPC_CMD_LED_MODE,
	GC573_IPC_CMD_AUDIO_GAIN,
	GC573_IPC_CMD_AUDIO_MUTE,
	GC573_IPC_CMD_OSD_ENABLE,
	GC573_IPC_CMD_OSD_POSITION,
	GC573_IPC_CMD_ASPECT,
	GC573_IPC_CMD_MODE,
	GC573_IPC_CMD_V4L2,
	GC573_IPC_CMD_HDR,
};

/*
 * One queued mutation.  Deliberately POD: it travels from the control thread
 * to the render loop through a lock-free single-producer/single-consumer ring.
 */
struct gc573_ipc_cmd {
	int32_t kind;
	int32_t a;
	int32_t b;
	int32_t c;
	float f;
	/* Optional device path for GC573_IPC_CMD_V4L2 (empty = streamer default). */
	char path[64];
};

/*
 * Snapshot of everything the Control Hub may display.  It is copied as a
 * whole under the seqlock, so every field belongs to the same instant.
 */
struct gc573_stats {
	uint64_t timestamp_ms;

	bool headless;
	bool streaming;		/* signal locked and DMA running */

	/* Input signal (IT6805) */
	bool signal_locked;
	uint32_t width;
	uint32_t height;
	bool interlaced;
	uint32_t rate_x100;
	uint32_t pixel_clock_khz;

	/* Capture engine (FPGA DMA) */
	uint32_t output_width;
	uint32_t output_height;
	double fps;
	uint64_t frames;
	unsigned int ring_buffers;
	unsigned int ring_channels;

	/* Audio pipeline */
	char audio_state[16];	/* "lpcm" | "bitstream" | "muted" | "none" */
	char audio_label[48];	/* e.g. "L-PCM 48kHz", "Dolby Digital 5.1" */
	uint32_t audio_rate;
	uint32_t audio_channels;
	uint32_t audio_bits;
	float audio_gain;
	bool audio_muted;

	/* V4L2 loopback sink */
	char v4l2_path[64];
	bool v4l2_configured;
	uint32_t v4l2_width;
	uint32_t v4l2_height;
	uint64_t v4l2_frames;
	uint64_t v4l2_dropped;

	/* Presentation options */
	bool osd_enabled;
	char osd_position[16];
	char aspect[12];
	bool vsync;
	bool fullscreen;

	/* LED state while streaming */
	uint8_t led_r;
	uint8_t led_g;
	uint8_t led_b;
};

/**
 * gc573_ipc_start - Bind the control socket and spawn the control thread
 * @version: Streamer version reported by hello/status
 * @socket_override: Explicit socket path, or NULL for the default location
 *
 * Returns 0 on success or a negative errno.  A failure is never fatal: the
 * streamer keeps capturing, it just cannot be remote controlled.  Starting a
 * second server in the same process is a no-op.
 */
int gc573_ipc_start(const char *version, const char *socket_override);

/**
 * gc573_ipc_stop - Tear down the control thread and remove the socket
 *
 * Safe to call when the server was never started and safe to call twice.
 */
void gc573_ipc_stop(void);

/**
 * gc573_ipc_publish_stats - Publish a new snapshot from the main loop
 *
 * Lock-free (seqlock).  Call it from one thread only — the render/DMA loop.
 */
void gc573_ipc_publish_stats(const struct gc573_stats *stats);

/**
 * gc573_ipc_poll_cmd - Dequeue one pending mutation
 * @out: Receives the command
 *
 * Called by the render loop.  Lock-free: the control thread is the only
 * producer and this function is the only consumer.
 *
 * Returns true when a command was dequeued.
 */
bool gc573_ipc_poll_cmd(struct gc573_ipc_cmd *out);

/**
 * gc573_ipc_has_clients - True when at least one client is attached
 */
bool gc573_ipc_has_clients(void);

/**
 * gc573_ipc_socket_path - Path of the bound socket, or "" when not running
 *
 * The returned pointer stays valid until the next gc573_ipc_start().
 */
const char *gc573_ipc_socket_path(void);

#endif /* GC573_USER_IPC_H */

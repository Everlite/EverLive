/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc570d-jsonrpc.h — Unified JSON-RPC 2.0 control socket for the GC570D
 *
 * The daemon keeps serving its legacy plain-text command socket for
 * gc570d-ctl, and additionally binds the shared everlive.sock so EverLive's
 * CLI and desktop hub can talk to both capture cards through one protocol.
 */
#ifndef GC570D_JSONRPC_H
#define GC570D_JSONRPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <poll.h>

struct gc570d_dev;

#define GC570D_JSONRPC_MAX_CLIENTS 8

struct gc570d_jsonrpc_stats {
	uint64_t video_seq[2];
	uint64_t audio_periods[2];
	uint64_t irq_dma0;
	uint64_t irq_dma1;
	bool v4l2_configured[2];
	uint64_t v4l2_frames[2];
	uint64_t v4l2_dropped[2];
};

/*
 * Bind the unified runtime socket (respects $EVERLIVE_SOCKET and the legacy
 * $EVERLIVE_CONTROL_SOCKET; otherwise $XDG_RUNTIME_DIR/everlive.sock).
 * Returns 0 on success or a negative errno.
 */
int gc570d_jsonrpc_init(const char *version);

/* Close the listener and unlink the socket + lock file.  Idempotent. */
void gc570d_jsonrpc_shutdown(void);

/* For the daemon poll() loop; -1 when not initialised. */
int gc570d_jsonrpc_listen_fd(void);
/* Path of the bound socket, or "" when not running. */
const char *gc570d_jsonrpc_socket_path(void);

/*
 * Add the listener and every connected client to @fds.  Returns the number of
 * descriptors written; the daemon poll() loop can then call
 * gc570d_jsonrpc_service() when any of them reports activity.
 */
int gc570d_jsonrpc_fill_pollfds(struct pollfd *fds, int max);

/* Accept new clients and drain every readable request line.  Non-blocking. */
void gc570d_jsonrpc_service(void);

/* Bind the pipeline snapshot callback (implemented in gc570d-userspace.c). */
void gc570d_jsonrpc_set_stats_fn(void (*fn)(struct gc570d_jsonrpc_stats *out));

/* Bind the active device pointer; re-arm after a hardware re-init. */
void gc570d_jsonrpc_set_dev(struct gc570d_dev *gc);

#endif /* GC570D_JSONRPC_H */

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_ipc.h — Shared control-socket path resolution for EverLive
 *
 * Both engines bind and every client (everlive-ctl, the Python hub, shell
 * helpers) connects through the same deterministic path.  The resolver never
 * probes the socket itself: a path must be valid before the socket exists.
 */

#ifndef EVERLIVE_IPC_H
#define EVERLIVE_IPC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVERLIVE_IPC_SOCKET_NAME	"everlive.sock"
#define EVERLIVE_IPC_PATH_MAX		108	/* sizeof sun_path */

/*
 * Runtime directory, without touching the socket:
 *   $XDG_RUNTIME_DIR   (if set and writable)
 *   /run/user/<uid>    (if writable)
 *   /tmp/everlive-<uid>
 *
 * Returns 0, -EINVAL for bad arguments, or -ENAMETOOLONG when it does not
 * fit in @buf.
 */
int everlive_ipc_runtime_dir(char *buf, size_t size);

/*
 * Pure path resolution:
 *   1. @override_path
 *   2. $EVERLIVE_SOCKET          (full path; used by everlive_client.py)
 *   3. $EVERLIVE_CONTROL_SOCKET  (legacy full path)
 *   4. <runtime_dir>/everlive.sock
 *
 * Returns 0, -EINVAL on bad arguments, or -ENAMETOOLONG when it does not fit.
 */
int everlive_ipc_resolve_socket_path(char *buf, size_t size,
				     const char *override_path);

/*
 * Same as resolve, then ensures the parent directory exists (mkdir 0700,
 * EEXIST is success).  Intended for the server side before bind(); clients
 * must use the pure resolver so they never create directories.
 */
int everlive_ipc_prepare_socket_path(char *buf, size_t size,
				     const char *override_path);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_IPC_H */

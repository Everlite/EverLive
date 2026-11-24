/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_ipc.h — Shared control-socket path resolution for EverLive
 *
 * Both engines bind and every client (everlive-ctl, the Python hub, shell
 * helpers) connects through the same deterministic path.  The resolver never
 * probes the socket itself: a path must be valid before the socket exists.
 *
 * Runtime directories are private security boundaries.  An existing directory
 * must be a real directory (not a symlink), owned by geteuid(), with mode
 * 0700.  A fallback directory is created 0700 under umask 077 and then
 * validated.  Callers must fail rather than silently use a foreign directory.
 */

#ifndef EVERLIVE_IPC_H
#define EVERLIVE_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVERLIVE_IPC_SOCKET_NAME	"everlive.sock"
#define EVERLIVE_IPC_PATH_MAX		108	/* sizeof sun_path */

/*
 * Runtime directory, without touching the socket:
 *   $XDG_RUNTIME_DIR   (must be a private 0700 directory)
 *   /run/user/<euid>   (if it exists and is private)
 *   /tmp/everlive-<euid> (validated if it exists)
 *
 * Returns 0, -EINVAL for bad arguments, -ENOENT when an explicitly selected
 * directory is missing, -ELOOP for a symlink, -ENOTDIR for a non-directory,
 * -EACCES for a foreign or permissive directory, or another negative errno.
 */
int everlive_ipc_runtime_dir(char *buf, size_t size);

/*
 * Same as runtime_dir(), but create the fallback directory when missing.
 * Returns the validated path in @buf.
 */
int everlive_ipc_prepare_runtime_dir(char *buf, size_t size);

/*
 * Engine-specific subdirectory:
 *   $XDG_RUNTIME_DIR/<name>
 *   /run/user/<euid>/<name>
 *   /tmp/<name>-<euid>
 *
 * The pure resolver validates an existing directory but allows it to be
 * missing.  prepare_runtime_subdir() creates and validates it.
 */
int everlive_ipc_runtime_subdir(const char *name, char *buf, size_t size);
int everlive_ipc_prepare_runtime_subdir(const char *name, char *buf,
					size_t size);

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
 * Same as resolve, then ensures the parent directory exists.  Auto-resolved
 * runtime directories are validated as private; explicit override paths are
 * operator-controlled and only have a missing parent created 0700.
 */
int everlive_ipc_prepare_socket_path(char *buf, size_t size,
				     const char *override_path);

/*
 * Open and lock <socket_path>.lock with O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW.
 * The lock file must be a regular file owned by geteuid(); it is chmod'd 0600.
 * Returns 0, -EADDRINUSE when another process owns the lock, or another
 * negative errno.
 */
int everlive_ipc_acquire_socket_lock(const char *socket_path, int *lock_fd);

/*
 * Read SO_PEERCRED from a connected Unix socket.  Either output pointer may be
 * NULL.  Returns 0 or a negative errno.
 */
int everlive_ipc_peer_identity(int fd, uid_t *uid, pid_t *pid);

/*
 * True only when EVERLIVE_DEBUG_CONTROL=1.  This is intended for the daemon's
 * environment, not the client's.
 */
bool everlive_ipc_debug_control_enabled(void);

/*
 * True for errors that mean the runtime directory is unsafe or misconfigured
 * (-EACCES, -EPERM, -ELOOP, -ENOTDIR, -ENOENT).  Engine startup should treat
 * these as fatal rather than continuing without a control socket.
 */
bool everlive_ipc_is_security_error(int err);

/*
 * Format an actionable diagnostic for a failed runtime-dir operation.
 * Returns 0, or -EINVAL for bad arguments / formatting failure.
 */
int everlive_ipc_describe_error(int err, const char *path, char *buf,
				size_t size);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_IPC_H */

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_ipc.c — Shared control-socket path resolution for EverLive
 */

#include "everlive_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define IPC_LOCK_PATH_MAX	(EVERLIVE_IPC_PATH_MAX + 8)

static int ipc_copy(char *buf, size_t size, const char *value)
{
	int n;

	if (!buf || size == 0)
		return -EINVAL;

	n = snprintf(buf, size, "%s", value);
	if (n < 0 || (size_t)n >= size || (size_t)n >= EVERLIVE_IPC_PATH_MAX)
		return -ENAMETOOLONG;

	return 0;
}

/*
 * A runtime directory is private only when it is a real directory, owned by
 * this process, and inaccessible to everyone else.  lstat() rejects a symlink;
 * the follow-up open/fstat closes the race where the path is swapped after the
 * check.
 */
static int ipc_check_private_dir(const char *path)
{
	struct stat lst;
	struct stat st;
	int fd;
	int ret;

	if (!path || !*path)
		return -EINVAL;

	if (lstat(path, &lst) < 0)
		return -errno;
	if (S_ISLNK(lst.st_mode))
		return -ELOOP;
	if (!S_ISDIR(lst.st_mode))
		return -ENOTDIR;
	if (lst.st_uid != geteuid() || (lst.st_mode & 07777) != 0700)
		return -EACCES;

	fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0) {
		ret = -errno;
		close(fd);
		return ret;
	}
	close(fd);

	if (st.st_dev != lst.st_dev || st.st_ino != lst.st_ino)
		return -EAGAIN;
	if (!S_ISDIR(st.st_mode))
		return -ENOTDIR;
	if (st.st_uid != geteuid() || (st.st_mode & 07777) != 0700)
		return -EACCES;

	return 0;
}

/*
 * Resolve one runtime directory.  @subdir is optional.  A missing fallback
 * directory is allowed because the caller may create it; a missing explicit
 * XDG_RUNTIME_DIR is not.
 */
static int ipc_resolve_dir(const char *subdir, char *buf, size_t size)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	char candidate[EVERLIVE_IPC_PATH_MAX];
	int ret;
	int n;

	if (subdir && (!*subdir || strchr(subdir, '/') ||
		       strcmp(subdir, ".") == 0 || strcmp(subdir, "..") == 0))
		return -EINVAL;

	if (xdg && *xdg) {
		ret = ipc_check_private_dir(xdg);
		if (ret < 0)
			return ret;
		if (!subdir)
			return ipc_copy(buf, size, xdg);

		n = snprintf(candidate, sizeof(candidate), "%s/%s", xdg, subdir);
		if (n < 0 || (size_t)n >= sizeof(candidate))
			return -ENAMETOOLONG;
		ret = ipc_check_private_dir(candidate);
		if (ret < 0 && ret != -ENOENT)
			return ret;
		return ipc_copy(buf, size, candidate);
	}

	n = snprintf(candidate, sizeof(candidate), "/run/user/%u",
		     (unsigned int)geteuid());
	if (n < 0 || (size_t)n >= sizeof(candidate))
		return -ENAMETOOLONG;

	ret = ipc_check_private_dir(candidate);
	if (ret == 0) {
		if (!subdir)
			return ipc_copy(buf, size, candidate);

		n = snprintf(candidate, sizeof(candidate), "/run/user/%u/%s",
			     (unsigned int)geteuid(), subdir);
		if (n < 0 || (size_t)n >= sizeof(candidate))
			return -ENAMETOOLONG;
		ret = ipc_check_private_dir(candidate);
		if (ret < 0 && ret != -ENOENT)
			return ret;
		return ipc_copy(buf, size, candidate);
	}
	if (ret != -ENOENT)
		return ret;

	if (subdir)
		n = snprintf(candidate, sizeof(candidate), "/tmp/%s-%u",
			     subdir, (unsigned int)geteuid());
	else
		n = snprintf(candidate, sizeof(candidate), "/tmp/everlive-%u",
			     (unsigned int)geteuid());
	if (n < 0 || (size_t)n >= sizeof(candidate))
		return -ENAMETOOLONG;

	ret = ipc_check_private_dir(candidate);
	if (ret < 0 && ret != -ENOENT)
		return ret;

	return ipc_copy(buf, size, candidate);
}

static int ipc_ensure_private_dir(const char *path)
{
	mode_t old_umask;
	int ret;
	int err;

	ret = ipc_check_private_dir(path);
	if (ret == 0)
		return 0;
	if (ret != -ENOENT)
		return ret;

	old_umask = umask(077);
	ret = mkdir(path, 0700);
	err = errno;
	(void)umask(old_umask);
	if (ret < 0 && err != EEXIST)
		return -err;

	return ipc_check_private_dir(path);
}

static int ipc_mkdir_parent_if_missing(const char *path)
{
	struct stat st;
	mode_t old_umask;
	int ret;

	if (lstat(path, &st) == 0)
		return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;
	if (errno != ENOENT)
		return -errno;

	old_umask = umask(077);
	ret = mkdir(path, 0700);
	(void)umask(old_umask);
	if (ret < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

int everlive_ipc_runtime_dir(char *buf, size_t size)
{
	if (!buf || size == 0)
		return -EINVAL;
	return ipc_resolve_dir(NULL, buf, size);
}

int everlive_ipc_prepare_runtime_dir(char *buf, size_t size)
{
	int ret;

	ret = everlive_ipc_runtime_dir(buf, size);
	if (ret < 0)
		return ret;
	return ipc_ensure_private_dir(buf);
}

int everlive_ipc_runtime_subdir(const char *name, char *buf, size_t size)
{
	if (!buf || size == 0 || !name || !*name)
		return -EINVAL;
	return ipc_resolve_dir(name, buf, size);
}

int everlive_ipc_prepare_runtime_subdir(const char *name, char *buf, size_t size)
{
	int ret;

	ret = everlive_ipc_runtime_subdir(name, buf, size);
	if (ret < 0)
		return ret;
	return ipc_ensure_private_dir(buf);
}

static int ipc_resolve_socket_path(char *buf, size_t size,
				   const char *override_path, bool *explicit_path)
{
	const char *env;
	char dir[EVERLIVE_IPC_PATH_MAX];
	int n;
	int ret;

	if (!buf || size == 0)
		return -EINVAL;

	if (explicit_path)
		*explicit_path = false;

	if (override_path && *override_path) {
		if (explicit_path)
			*explicit_path = true;
		return ipc_copy(buf, size, override_path);
	}

	env = getenv("EVERLIVE_SOCKET");
	if (env && *env) {
		if (explicit_path)
			*explicit_path = true;
		return ipc_copy(buf, size, env);
	}

	env = getenv("EVERLIVE_CONTROL_SOCKET");
	if (env && *env) {
		if (explicit_path)
			*explicit_path = true;
		return ipc_copy(buf, size, env);
	}

	ret = everlive_ipc_runtime_dir(dir, sizeof(dir));
	if (ret < 0)
		return ret;

	n = snprintf(buf, size, "%s/%s", dir, EVERLIVE_IPC_SOCKET_NAME);
	if (n < 0 || (size_t)n >= size || (size_t)n >= EVERLIVE_IPC_PATH_MAX)
		return -ENAMETOOLONG;

	return 0;
}

int everlive_ipc_resolve_socket_path(char *buf, size_t size,
				     const char *override_path)
{
	return ipc_resolve_socket_path(buf, size, override_path, NULL);
}

int everlive_ipc_prepare_socket_path(char *buf, size_t size,
				     const char *override_path)
{
	char dir[EVERLIVE_IPC_PATH_MAX];
	char *slash;
	bool explicit_path;
	int ret;

	ret = ipc_resolve_socket_path(buf, size, override_path, &explicit_path);
	if (ret < 0)
		return ret;

	if (snprintf(dir, sizeof(dir), "%s", buf) < 0)
		return -EINVAL;

	slash = strrchr(dir, '/');
	if (!slash || slash == dir)
		return 0;

	*slash = '\0';
	if (explicit_path)
		return ipc_mkdir_parent_if_missing(dir);
	return ipc_ensure_private_dir(dir);
}

int everlive_ipc_acquire_socket_lock(const char *socket_path, int *lock_fd)
{
	char lock_path[IPC_LOCK_PATH_MAX];
	struct stat st;
	int fd;
	int n;
	int ret;

	if (!socket_path || !*socket_path || !lock_fd)
		return -EINVAL;

	n = snprintf(lock_path, sizeof(lock_path), "%.*s.lock",
		     EVERLIVE_IPC_PATH_MAX - 1, socket_path);
	if (n < 0 || (size_t)n >= sizeof(lock_path))
		return -ENAMETOOLONG;

	fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -errno;

	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
	    st.st_uid != geteuid()) {
		ret = -EACCES;
		goto fail;
	}
	if (fchmod(fd, 0600) != 0) {
		ret = -errno;
		goto fail;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		ret = (errno == EWOULDBLOCK) ? -EADDRINUSE : -errno;
		goto fail;
	}

	*lock_fd = fd;
	return 0;

fail:
	close(fd);
	return ret;
}

int everlive_ipc_peer_identity(int fd, uid_t *uid, pid_t *pid)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);

	if (fd < 0)
		return -EINVAL;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
		return -errno;

	if (uid)
		*uid = cred.uid;
	if (pid)
		*pid = cred.pid;
	return 0;
}

bool everlive_ipc_debug_control_enabled(void)
{
	const char *value = getenv("EVERLIVE_DEBUG_CONTROL");

	return value && strcmp(value, "1") == 0;
}

bool everlive_ipc_is_security_error(int err)
{
	switch (err) {
	case -EACCES:
	case -EPERM:
	case -ELOOP:
	case -ENOTDIR:
	case -ENOENT:
		return true;
	default:
		return false;
	}
}

int everlive_ipc_describe_error(int err, const char *path, char *buf,
				size_t size)
{
	const char *what = path && *path ? path : "(runtime directory)";
	unsigned int uid = (unsigned int)geteuid();

	if (!buf || size == 0)
		return -EINVAL;

	switch (err) {
	case -ELOOP:
		return snprintf(buf, size,
				"%s is a symbolic link; expected a private 0700 directory owned by uid %u",
				what, uid) >= 0 ? 0 : -EINVAL;
	case -ENOTDIR:
		return snprintf(buf, size,
				"%s is not a directory; expected a private 0700 directory owned by uid %u",
				what, uid) >= 0 ? 0 : -EINVAL;
	case -EACCES:
	case -EPERM:
		return snprintf(buf, size,
				"%s is not a private runtime directory (expected owner uid %u and mode 0700); remove it or set XDG_RUNTIME_DIR to a safe directory",
				what, uid) >= 0 ? 0 : -EINVAL;
	case -ENOENT:
		return snprintf(buf, size,
				"%s does not exist; expected an existing private 0700 directory owned by uid %u",
				what, uid) >= 0 ? 0 : -EINVAL;
	default:
		return snprintf(buf, size, "%s: %s", what,
				strerror(err < 0 ? -err : err)) >= 0 ? 0 : -EINVAL;
	}
}

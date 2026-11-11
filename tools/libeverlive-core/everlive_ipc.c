/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_ipc.c — Shared control-socket path resolution for EverLive
 */

#include "everlive_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

int everlive_ipc_runtime_dir(char *buf, size_t size)
{
	const char *xdg;
	char candidate[EVERLIVE_IPC_PATH_MAX];
	int n;

	if (!buf || size == 0)
		return -EINVAL;

	xdg = getenv("XDG_RUNTIME_DIR");
	if (xdg && *xdg && access(xdg, W_OK) == 0)
		return ipc_copy(buf, size, xdg);

	n = snprintf(candidate, sizeof(candidate), "/run/user/%u",
		     (unsigned int)getuid());
	if (n > 0 && (size_t)n < sizeof(candidate) &&
	    access(candidate, W_OK) == 0)
		return ipc_copy(buf, size, candidate);

	n = snprintf(candidate, sizeof(candidate), "/tmp/everlive-%u",
		     (unsigned int)getuid());
	if (n < 0 || (size_t)n >= sizeof(candidate))
		return -ENAMETOOLONG;

	return ipc_copy(buf, size, candidate);
}

int everlive_ipc_resolve_socket_path(char *buf, size_t size,
				     const char *override_path)
{
	const char *env;
	char dir[EVERLIVE_IPC_PATH_MAX];
	int n;
	int ret;

	if (!buf || size == 0)
		return -EINVAL;

	if (override_path && *override_path)
		return ipc_copy(buf, size, override_path);

	env = getenv("EVERLIVE_SOCKET");
	if (env && *env)
		return ipc_copy(buf, size, env);

	env = getenv("EVERLIVE_CONTROL_SOCKET");
	if (env && *env)
		return ipc_copy(buf, size, env);

	ret = everlive_ipc_runtime_dir(dir, sizeof(dir));
	if (ret < 0)
		return ret;

	n = snprintf(buf, size, "%s/%s", dir, EVERLIVE_IPC_SOCKET_NAME);
	if (n < 0 || (size_t)n >= size || (size_t)n >= EVERLIVE_IPC_PATH_MAX)
		return -ENAMETOOLONG;

	return 0;
}

int everlive_ipc_prepare_socket_path(char *buf, size_t size,
				     const char *override_path)
{
	char dir[EVERLIVE_IPC_PATH_MAX];
	char *slash;
	int ret;

	ret = everlive_ipc_resolve_socket_path(buf, size, override_path);
	if (ret < 0)
		return ret;

	if (snprintf(dir, sizeof(dir), "%s", buf) < 0)
		return -EINVAL;

	slash = strrchr(dir, '/');
	if (!slash || slash == dir)
		return 0;

	*slash = '\0';
	if (mkdir(dir, 0700) < 0 && errno != EEXIST)
		return -errno;

	return 0;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * everlive_v4l2_preload.c — libc/libv4l2 interposition for the shared V4L2
 * virtual-camera engine.
 *
 * The per-card DSO links this file, everlive_v4l2_state.c and a small
 * everlive_v4l2_card_descriptor().  This file owns only the interposed
 * symbols, path/sysfs injection and the lock around the state machine; all
 * V4L2 semantics live in everlive_v4l2_state.c.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <linux/videodev2.h>

#include "everlive_v4l2_state.h"

#define EVERLIVE_V4L2_MAX_DIRS	16U

struct everlive_dir_track {
	DIR *dirp;
	int step;
	bool is_sysfs;
};

static struct everlive_dir_track g_dev_dirs[EVERLIVE_V4L2_MAX_DIRS];
static pthread_mutex_t g_v4l2_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Optional per-process opt-in.  LD_PRELOAD is inherited by every child of a
 * launched application, but this shim is only meaningful in the process that
 * actually opens the virtual camera.  When EVERLIVE_V4L2_TARGET is set to a
 * comma-separated list of executable basenames (or "*"), stay dormant in
 * every other child so crash reporters, shells and helpers are untouched.
 * Unset = legacy behaviour (active everywhere).
 */
static int g_target_allowed = -1;

static bool shim_target_allowed(void)
{
	int cached = __atomic_load_n(&g_target_allowed, __ATOMIC_RELAXED);

	if (cached >= 0)
		return cached != 0;

	const char *list = getenv("EVERLIVE_V4L2_TARGET");
	if (!list || !*list) {
		__atomic_store_n(&g_target_allowed, 1, __ATOMIC_RELAXED);
		return true;
	}

	const char *prog = program_invocation_short_name;
	bool match = false;
	const char *p = list;

	if (!prog)
		prog = "";
	while (*p) {
		const char *start;

		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		start = p;
		while (*p && *p != ',' && *p != ' ' && *p != '\t')
			p++;
		if ((size_t)(p - start) == 1 && start[0] == '*') {
			match = true;
			break;
		}
		if ((size_t)(p - start) == strlen(prog) &&
		    strncmp(start, prog, (size_t)(p - start)) == 0) {
			match = true;
			break;
		}
	}

	__atomic_store_n(&g_target_allowed, match ? 1 : 0, __ATOMIC_RELAXED);
	return match;
}

/* Real libc & libv4l2 function pointers */
static int (*real_open)(const char *, int, ...) = NULL;
static int (*real_open64)(const char *, int, ...) = NULL;
static int (*real_openat)(int, const char *, int, ...) = NULL;
static int (*real_openat64)(int, const char *, int, ...) = NULL;
static int (*real_close)(int) = NULL;
static int (*real_dup)(int) = NULL;
static int (*real_dup2)(int, int) = NULL;
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static void *(*real_mmap64)(void *, size_t, int, int, int, off_t) = NULL;
static int (*real_munmap)(void *, size_t) = NULL;
static int (*real_stat)(const char *, struct stat *) = NULL;
static int (*real_lstat)(const char *, struct stat *) = NULL;
static int (*real_fstat)(int, struct stat *) = NULL;
static int (*real_fstatat)(int, const char *, struct stat *, int) = NULL;
static int (*real_access)(const char *, int) = NULL;
static DIR *(*real_opendir)(const char *) = NULL;
static struct dirent *(*real_readdir)(DIR *) = NULL;
static int (*real_closedir)(DIR *) = NULL;
static FILE *(*real_fopen)(const char *, const char *) = NULL;
static FILE *(*real_fopen64)(const char *, const char *) = NULL;

static int (*real_v4l2_open)(const char *, int, ...) = NULL;
static int (*real_v4l2_fd_open)(int, int) = NULL;
static int (*real_v4l2_close)(int) = NULL;
static int (*real_v4l2_dup)(int) = NULL;
static int (*real_v4l2_ioctl)(int, unsigned long, ...) = NULL;
static ssize_t (*real_v4l2_read)(int, void *, size_t) = NULL;
static void *(*real_v4l2_mmap)(void *, size_t, int, int, int, int64_t) = NULL;
static int (*real_v4l2_munmap)(void *, size_t) = NULL;

static bool g_libc_funcs_ready = false;
static bool g_v4l2_funcs_ready = false;

static void init_real_funcs(void)
{
	if (g_libc_funcs_ready)
		return;
	if (!real_open)     real_open     = dlsym(RTLD_NEXT, "open");
	if (!real_open64)   real_open64   = dlsym(RTLD_NEXT, "open64");
	if (!real_openat)   real_openat   = dlsym(RTLD_NEXT, "openat");
	if (!real_openat64) real_openat64 = dlsym(RTLD_NEXT, "openat64");
	if (!real_close)    real_close    = dlsym(RTLD_NEXT, "close");
	if (!real_dup)      real_dup      = dlsym(RTLD_NEXT, "dup");
	if (!real_dup2)     real_dup2     = dlsym(RTLD_NEXT, "dup2");
	if (!real_ioctl)    real_ioctl    = dlsym(RTLD_NEXT, "ioctl");
	if (!real_mmap)     real_mmap     = dlsym(RTLD_NEXT, "mmap");
	if (!real_mmap64)   real_mmap64   = dlsym(RTLD_NEXT, "mmap64");
	if (!real_munmap)   real_munmap   = dlsym(RTLD_NEXT, "munmap");
	if (!real_stat)     real_stat     = dlsym(RTLD_NEXT, "stat");
	if (!real_lstat)    real_lstat    = dlsym(RTLD_NEXT, "lstat");
	if (!real_fstat)    real_fstat    = dlsym(RTLD_NEXT, "fstat");
	if (!real_fstatat)  real_fstatat  = dlsym(RTLD_NEXT, "fstatat");
	if (!real_access)   real_access   = dlsym(RTLD_NEXT, "access");
	if (!real_opendir)  real_opendir  = dlsym(RTLD_NEXT, "opendir");
	if (!real_readdir)  real_readdir  = dlsym(RTLD_NEXT, "readdir");
	if (!real_closedir) real_closedir = dlsym(RTLD_NEXT, "closedir");
	if (!real_fopen)    real_fopen    = dlsym(RTLD_NEXT, "fopen");
	if (!real_fopen64)  real_fopen64  = dlsym(RTLD_NEXT, "fopen64");
	g_libc_funcs_ready = true;
}

static void init_real_v4l2_funcs(void)
{
	init_real_funcs();
	if (g_v4l2_funcs_ready)
		return;
	g_v4l2_funcs_ready = true;
	real_v4l2_open    = dlsym(RTLD_NEXT, "v4l2_open");
	real_v4l2_fd_open = dlsym(RTLD_NEXT, "v4l2_fd_open");
	real_v4l2_close   = dlsym(RTLD_NEXT, "v4l2_close");
	real_v4l2_dup     = dlsym(RTLD_NEXT, "v4l2_dup");
	real_v4l2_ioctl   = dlsym(RTLD_NEXT, "v4l2_ioctl");
	real_v4l2_read    = dlsym(RTLD_NEXT, "v4l2_read");
	real_v4l2_mmap    = dlsym(RTLD_NEXT, "v4l2_mmap");
	real_v4l2_munmap  = dlsym(RTLD_NEXT, "v4l2_munmap");
}

static const struct everlive_v4l2_card *card(void)
{
	return everlive_v4l2_card_descriptor();
}

static void *direct_mmap(void *addr, size_t length, int prot, int flags,
			 int fd, off_t offset)
{
#if defined(SYS_mmap)
	return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd,
			       offset);
#elif defined(SYS_mmap2)
	return (void *)syscall(SYS_mmap2, addr, length, prot, flags, fd,
			       (unsigned long)(offset / 4096));
#else
	errno = ENOSYS;
	return MAP_FAILED;
#endif
}

__attribute__((constructor))
static void everlive_v4l2_preload_init(void)
{
	(void)everlive_v4l2_state_init(card());
}

static int virtual_node_path(unsigned int port, char *buf, size_t size)
{
	const struct everlive_v4l2_card *c = card();

	if (!c || port >= c->port_count)
		return -1;
	return snprintf(buf, size, "/dev/video%u", c->node_base + port);
}

static int virtual_node_name(unsigned int port, char *buf, size_t size)
{
	const struct everlive_v4l2_card *c = card();

	if (!c || port >= c->port_count)
		return -1;
	return snprintf(buf, size, "video%u", c->node_base + port);
}

static bool is_virtual_node_name(const char *name)
{
	const struct everlive_v4l2_card *c = card();
	char buf[32];

	if (!c || !name)
		return false;
	for (unsigned int p = 0; p < c->port_count; p++) {
		if (virtual_node_name(p, buf, sizeof(buf)) < 0)
			continue;
		if (strcmp(name, buf) == 0)
			return true;
	}
	return false;
}

static int match_virtual_video_path(const char *path)
{
	const struct everlive_v4l2_card *c = card();
	char dev[32];
	char node[32];

	if (!path || !c || !shim_target_allowed())
		return -1;
	for (unsigned int p = 0; p < c->port_count; p++) {
		if (!virtual_node_path(p, dev, sizeof(dev)) ||
		    !virtual_node_name(p, node, sizeof(node)))
			continue;
		if (strcmp(path, dev) == 0 || strcmp(path, node) == 0)
			return (int)p;
	}
	return -1;
}

static struct everlive_dir_track *find_dir_track(DIR *dirp)
{
	if (!dirp)
		return NULL;
	for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_DIRS; i++) {
		if (g_dev_dirs[i].dirp == dirp)
			return &g_dev_dirs[i];
	}
	return NULL;
}

static bool is_sysfs_video_dir(const char *path)
{
	return strcmp(path, "/sys/class/video4linux") == 0 ||
	       strcmp(path, "/sys/class/video4linux/") == 0 ||
	       strcmp(path, "/sys/subsystem/video4linux/devices") == 0 ||
	       strcmp(path, "/sys/subsystem/video4linux/devices/") == 0;
}

static int match_virtual_sysfs_stat(const char *path, struct stat *st)
{
	const struct everlive_v4l2_card *c = card();
	char node[128];
	char prefix[160];

	if (!path || !c || !shim_target_allowed())
		return -1;
	while (*path == '.' && *(path + 1) == '/')
		path += 2;
	if (is_sysfs_video_dir(path)) {
		if (st) {
			memset(st, 0, sizeof(*st));
			st->st_mode = S_IFDIR | 0755;
			st->st_nlink = 4;
		}
		return 0;
	}
	for (unsigned int p = 0; p < c->port_count; p++) {
		snprintf(node, sizeof(node), "/sys/class/video4linux/video%u",
			 c->node_base + p);
		snprintf(prefix, sizeof(prefix), "%s/", node);
		if (strcmp(path, node) == 0 || strcmp(path, prefix) == 0) {
			if (st) {
				memset(st, 0, sizeof(*st));
				st->st_mode = S_IFDIR | 0755;
				st->st_nlink = 2;
			}
			return 0;
		}
		if (strncmp(path, prefix, strlen(prefix)) == 0) {
			if (st) {
				memset(st, 0, sizeof(*st));
				st->st_mode = S_IFREG | 0444;
				st->st_nlink = 1;
				st->st_size = 64;
			}
			return 0;
		}

		/*
		 * libv4l2's media-info helper detects the device type through
		 * /sys/dev/char/<major>:<minor>/uevent.  Expose that alias as
		 * well so tools such as v4l2-ctl --all work.
		 */
		snprintf(node, sizeof(node), "/sys/dev/char/81:%u",
			 c->node_base + p);
		snprintf(prefix, sizeof(prefix), "%s/", node);
		if (strcmp(path, node) == 0 || strcmp(path, prefix) == 0) {
			if (st) {
				memset(st, 0, sizeof(*st));
				st->st_mode = S_IFDIR | 0755;
				st->st_nlink = 2;
			}
			return 0;
		}
		if (strncmp(path, prefix, strlen(prefix)) == 0) {
			if (st) {
				memset(st, 0, sizeof(*st));
				st->st_mode = S_IFREG | 0444;
				st->st_nlink = 1;
				st->st_size = 64;
			}
			return 0;
		}
	}
	return -1;
}

static int open_virtual_sysfs(const char *path)
{
	const struct everlive_v4l2_card *c = card();
	char prefix[160];
	char content[256] = { 0 };
	const char *attr = NULL;
	int port = -1;
	int fd;
	size_t len;

	if (!c || !shim_target_allowed())
		return -1;
	for (unsigned int p = 0; p < c->port_count; p++) {
		snprintf(prefix, sizeof(prefix),
			 "/sys/class/video4linux/video%u/", c->node_base + p);
		if (strncmp(path, prefix, strlen(prefix)) == 0) {
			port = (int)p;
			attr = path + strlen(prefix);
			break;
		}
	}
	if (port < 0) {
		for (unsigned int p = 0; p < c->port_count; p++) {
			snprintf(prefix, sizeof(prefix),
				 "/sys/dev/char/81:%u/", c->node_base + p);
			if (strncmp(path, prefix, strlen(prefix)) == 0) {
				port = (int)p;
				attr = path + strlen(prefix);
				break;
			}
		}
	}
	if (port < 0 || !attr)
		return -1;

	if (strcmp(attr, "uevent") == 0) {
		snprintf(content, sizeof(content),
			 "MAJOR=81\nMINOR=%u\nDEVNAME=video%u\nSUBSYSTEM=video4linux\n",
			 c->node_base + (unsigned int)port,
			 c->node_base + (unsigned int)port);
	} else if (strcmp(attr, "name") == 0) {
		const char *name = c->port_names[port] ?
				   c->port_names[port] : c->card_name;

		snprintf(content, sizeof(content), "%s\n", name ? name : "");
	} else if (strcmp(attr, "dev") == 0) {
		snprintf(content, sizeof(content), "81:%u\n",
			 c->node_base + (unsigned int)port);
	} else if (strcmp(attr, "index") == 0) {
		snprintf(content, sizeof(content), "%d\n", port);
	} else {
		return -1;
	}

	fd = memfd_create("everlive_v4l2_sysfs", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	len = strlen(content);
	if (write(fd, content, len) != (ssize_t)len) {
		close(fd);
		return -1;
	}
	lseek(fd, 0, SEEK_SET);
	return fd;
}

DIR *opendir(const char *name)
{
	init_real_funcs();
	bool is_sysfs = is_sysfs_video_dir(name);
	bool is_dev = (strcmp(name, "/dev") == 0 || strcmp(name, "/dev/") == 0);
	DIR *d = real_opendir(name);

	if (!d && is_sysfs)
		d = real_opendir("/tmp");
	if (d && (is_dev || is_sysfs)) {
		pthread_mutex_lock(&g_v4l2_lock);
		for (unsigned int i = 0; i < EVERLIVE_V4L2_MAX_DIRS; i++) {
			if (!g_dev_dirs[i].dirp) {
				g_dev_dirs[i].dirp = d;
				g_dev_dirs[i].step = 0;
				g_dev_dirs[i].is_sysfs = is_sysfs;
				break;
			}
		}
		pthread_mutex_unlock(&g_v4l2_lock);
	}
	return d;
}

struct dirent *readdir(DIR *dirp)
{
	static struct dirent tls_fake_dirent;
	const struct everlive_v4l2_card *c = card();
	struct everlive_dir_track *tr;
	struct dirent *ent;

	init_real_funcs();
	if (!find_dir_track(dirp))
		return real_readdir(dirp);

	ent = real_readdir(dirp);
	pthread_mutex_lock(&g_v4l2_lock);
	tr = find_dir_track(dirp);
	if (!tr || !c) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return ent;
	}

	if (tr->is_sysfs) {
		if ((unsigned int)tr->step < c->port_count) {
			unsigned int idx = (unsigned int)tr->step++;

			pthread_mutex_unlock(&g_v4l2_lock);
			memset(&tls_fake_dirent, 0, sizeof(tls_fake_dirent));
			tls_fake_dirent.d_ino = 7000 + idx;
			tls_fake_dirent.d_type = DT_DIR;
			snprintf(tls_fake_dirent.d_name,
				 sizeof(tls_fake_dirent.d_name),
				 "video%u", c->node_base + idx);
			return &tls_fake_dirent;
		}
		pthread_mutex_unlock(&g_v4l2_lock);
		return NULL;
	}

	if (ent) {
		if (is_virtual_node_name(ent->d_name)) {
			pthread_mutex_unlock(&g_v4l2_lock);
			return readdir(dirp);
		}
		pthread_mutex_unlock(&g_v4l2_lock);
		return ent;
	}
	if ((unsigned int)tr->step < c->port_count) {
		unsigned int idx = (unsigned int)tr->step++;

		pthread_mutex_unlock(&g_v4l2_lock);
		memset(&tls_fake_dirent, 0, sizeof(tls_fake_dirent));
		tls_fake_dirent.d_ino = 6000 + idx;
		tls_fake_dirent.d_type = DT_CHR;
		snprintf(tls_fake_dirent.d_name, sizeof(tls_fake_dirent.d_name),
			 "video%u", c->node_base + idx);
		return &tls_fake_dirent;
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	return NULL;
}

int closedir(DIR *dirp)
{
	init_real_funcs();
	if (find_dir_track(dirp)) {
		pthread_mutex_lock(&g_v4l2_lock);
		struct everlive_dir_track *tr = find_dir_track(dirp);

		if (tr) {
			tr->dirp = NULL;
			tr->step = 0;
			tr->is_sysfs = false;
		}
		pthread_mutex_unlock(&g_v4l2_lock);
	}
	return real_closedir(dirp);
}

static int fill_fake_video_stat(unsigned int port, struct stat *st)
{
	const struct everlive_v4l2_card *c = card();

	if ((uintptr_t)st == 0) {
		errno = EFAULT;
		return -1;
	}
	if (!c || port >= c->port_count) {
		errno = ENOENT;
		return -1;
	}
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | 0666;
	st->st_rdev = makedev(81, c->node_base + port);
	st->st_nlink = 1;
	return 0;
}

int stat(const char *path, struct stat *buf)
{
	init_real_funcs();
	int port = match_virtual_video_path(path);

	if (port >= 0)
		return fill_fake_video_stat((unsigned int)port, buf);
	if (match_virtual_sysfs_stat(path, buf) == 0)
		return 0;
	return real_stat(path, buf);
}

int lstat(const char *path, struct stat *buf)
{
	init_real_funcs();
	int port = match_virtual_video_path(path);

	if (port >= 0)
		return fill_fake_video_stat((unsigned int)port, buf);
	if (match_virtual_sysfs_stat(path, buf) == 0)
		return 0;
	return real_lstat(path, buf);
}

int fstat(int fd, struct stat *buf)
{
	struct everlive_v4l2_handle *h;
	int ret;

	init_real_funcs();
	if (!everlive_v4l2_is_virtual_fd(fd))
		return real_fstat(fd, buf);
	pthread_mutex_lock(&g_v4l2_lock);
	h = everlive_v4l2_find_handle(fd);
	if (!h) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return real_fstat(fd, buf);
	}
	{
		unsigned int port = h->port;

		pthread_mutex_unlock(&g_v4l2_lock);
		ret = fill_fake_video_stat(port, buf);
		return ret;
	}
}

int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags)
{
	init_real_funcs();
	int port = match_virtual_video_path(pathname);

	if (port >= 0)
		return fill_fake_video_stat((unsigned int)port, buf);
	if (match_virtual_sysfs_stat(pathname, buf) == 0)
		return 0;
	return real_fstatat(dirfd, pathname, buf, flags);
}

int access(const char *pathname, int mode)
{
	init_real_funcs();
	if (match_virtual_video_path(pathname) >= 0)
		return 0;
	if (match_virtual_sysfs_stat(pathname, NULL) == 0)
		return 0;
	return real_access(pathname, mode);
}

static int open_virtual_video(unsigned int port)
{
	int fd;

	pthread_mutex_lock(&g_v4l2_lock);
	fd = everlive_v4l2_open_virtual(port);
	pthread_mutex_unlock(&g_v4l2_lock);
	return fd;
}

FILE *fopen(const char *pathname, const char *mode)
{
	FILE *file;
	int fd;
	int port;

	init_real_funcs();
	port = match_virtual_video_path(pathname);
	if (port >= 0) {
		fd = open_virtual_video((unsigned int)port);
		if (fd < 0)
			return NULL;
		file = fdopen(fd, mode);
		if (!file)
			close(fd);
		return file;
	}
	fd = open_virtual_sysfs(pathname);
	if (fd >= 0) {
		file = fdopen(fd, mode);
		if (!file)
			close(fd);
		return file;
	}
	if (real_fopen)
		return real_fopen(pathname, mode);
	return NULL;
}

FILE *fopen64(const char *pathname, const char *mode)
{
	FILE *file;
	int fd;
	int port;

	init_real_funcs();
	port = match_virtual_video_path(pathname);
	if (port >= 0) {
		fd = open_virtual_video((unsigned int)port);
		if (fd < 0)
			return NULL;
		file = fdopen(fd, mode);
		if (!file)
			close(fd);
		return file;
	}
	fd = open_virtual_sysfs(pathname);
	if (fd >= 0) {
		file = fdopen(fd, mode);
		if (!file)
			close(fd);
		return file;
	}
	if (real_fopen64)
		return real_fopen64(pathname, mode);
	if (real_fopen)
		return real_fopen(pathname, mode);
	return NULL;
}

int open(const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	int port;
	int sfd;

	init_real_funcs();
	port = match_virtual_video_path(pathname);
	if (port >= 0)
		return open_virtual_video((unsigned int)port);
	sfd = open_virtual_sysfs(pathname);
	if (sfd >= 0)
		return sfd;
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	return real_open(pathname, flags, mode);
}

int open64(const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	int port;
	int sfd;

	init_real_funcs();
	port = match_virtual_video_path(pathname);
	if (port >= 0)
		return open_virtual_video((unsigned int)port);
	sfd = open_virtual_sysfs(pathname);
	if (sfd >= 0)
		return sfd;
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	return real_open64(pathname, flags, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	int port;
	int sfd;

	init_real_funcs();
	port = match_virtual_video_path(pathname);
	if (port >= 0)
		return open_virtual_video((unsigned int)port);
	sfd = open_virtual_sysfs(pathname);
	if (sfd >= 0)
		return sfd;
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	return real_openat(dirfd, pathname, flags, mode);
}

int openat64(int dirfd, const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	int port;
	int sfd;

	init_real_funcs();
	port = match_virtual_video_path(pathname);
	if (port >= 0)
		return open_virtual_video((unsigned int)port);
	sfd = open_virtual_sysfs(pathname);
	if (sfd >= 0)
		return sfd;
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	return real_openat64(dirfd, pathname, flags, mode);
}

int close(int fd)
{
	int ret;

	init_real_funcs();
	if (!everlive_v4l2_is_virtual_fd(fd))
		return real_close(fd);
	pthread_mutex_lock(&g_v4l2_lock);
	ret = everlive_v4l2_close_virtual(fd);
	pthread_mutex_unlock(&g_v4l2_lock);
	return ret;
}

int ioctl(int fd, unsigned long request, ...)
{
	struct everlive_v4l2_handle *h;
	void *arg;
	va_list ap;
	int ret;

	init_real_funcs();
	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!everlive_v4l2_is_virtual_fd(fd))
		return real_ioctl(fd, request, arg);

	pthread_mutex_lock(&g_v4l2_lock);
	h = everlive_v4l2_find_handle(fd);
	if (!h) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return real_ioctl(fd, request, arg);
	}
	ret = everlive_v4l2_ioctl(h, request, arg);
	pthread_mutex_unlock(&g_v4l2_lock);
	return ret;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	void *p;

	init_real_funcs();
	if (fd >= 0 && everlive_v4l2_is_virtual_fd(fd)) {
		pthread_mutex_lock(&g_v4l2_lock);
		p = everlive_v4l2_mmap_buffer(fd, (uint64_t)offset);
		pthread_mutex_unlock(&g_v4l2_lock);
		if (p)
			return p;
	}
	if (!real_mmap)
		return direct_mmap(addr, length, prot, flags, fd, offset);
	return real_mmap(addr, length, prot, flags, fd, offset);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	return mmap(addr, length, prot, flags, fd, offset);
}

int munmap(void *addr, size_t length)
{
	init_real_funcs();
	if (everlive_v4l2_open_count() > 0) {
		bool ours;

		pthread_mutex_lock(&g_v4l2_lock);
		ours = everlive_v4l2_munmap_buffer(addr);
		pthread_mutex_unlock(&g_v4l2_lock);
		if (ours)
			return 0;
	}
	if (!real_munmap)
		return (int)syscall(SYS_munmap, addr, length);
	return real_munmap(addr, length);
}

/* =========================================================================
 * libv4l2.so.0 wrapper interceptors (required for OBS Studio linux-v4l2)
 * ========================================================================= */

int v4l2_open(const char *file, int oflag, ...)
{
	mode_t mode = 0;
	int port;

	init_real_v4l2_funcs();
	port = match_virtual_video_path(file);
	if (port >= 0)
		return open_virtual_video((unsigned int)port);
	if (oflag & O_CREAT) {
		va_list ap;

		va_start(ap, oflag);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	if (real_v4l2_open)
		return real_v4l2_open(file, oflag, mode);
	return real_open(file, oflag, mode);
}

int v4l2_fd_open(int fd, int v4l2_flags)
{
	int ret;

	init_real_v4l2_funcs();
	pthread_mutex_lock(&g_v4l2_lock);
	ret = everlive_v4l2_is_virtual_fd(fd);
	pthread_mutex_unlock(&g_v4l2_lock);
	if (ret)
		return fd;
	if (real_v4l2_fd_open)
		return real_v4l2_fd_open(fd, v4l2_flags);
	return fd;
}

int v4l2_close(int fd)
{
	init_real_v4l2_funcs();
	if (everlive_v4l2_is_virtual_fd(fd))
		return close(fd);
	if (real_v4l2_close)
		return real_v4l2_close(fd);
	return real_close(fd);
}

int v4l2_dup(int fd)
{
	int newfd;

	init_real_v4l2_funcs();
	if (!everlive_v4l2_is_virtual_fd(fd)) {
		if (real_v4l2_dup)
			return real_v4l2_dup(fd);
		return real_dup(fd);
	}
	newfd = real_dup(fd);
	if (newfd < 0)
		return newfd;
	pthread_mutex_lock(&g_v4l2_lock);
	everlive_v4l2_dup_virtual(fd, newfd);
	pthread_mutex_unlock(&g_v4l2_lock);
	return newfd;
}

int v4l2_ioctl(int fd, unsigned long int request, ...)
{
	struct everlive_v4l2_handle *h;
	void *arg;
	va_list ap;
	int ret;

	init_real_v4l2_funcs();
	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!everlive_v4l2_is_virtual_fd(fd)) {
		if (real_v4l2_ioctl)
			return real_v4l2_ioctl(fd, request, arg);
		return real_ioctl(fd, request, arg);
	}
	pthread_mutex_lock(&g_v4l2_lock);
	h = everlive_v4l2_find_handle(fd);
	if (h) {
		ret = everlive_v4l2_ioctl(h, request, arg);
		pthread_mutex_unlock(&g_v4l2_lock);
		return ret;
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	if (real_v4l2_ioctl)
		return real_v4l2_ioctl(fd, request, arg);
	return real_ioctl(fd, request, arg);
}

ssize_t v4l2_read(int fd, void *buffer, size_t n)
{
	ssize_t ret;

	init_real_v4l2_funcs();
	if (!everlive_v4l2_is_virtual_fd(fd)) {
		if (real_v4l2_read)
			return real_v4l2_read(fd, buffer, n);
		return read(fd, buffer, n);
	}
	pthread_mutex_lock(&g_v4l2_lock);
	ret = everlive_v4l2_read_frame(fd, buffer, n);
	pthread_mutex_unlock(&g_v4l2_lock);
	return ret;
}

void *v4l2_mmap(void *start, size_t length, int prot, int flags, int fd,
		int64_t offset)
{
	void *p;

	init_real_v4l2_funcs();
	if (fd >= 0 && everlive_v4l2_is_virtual_fd(fd)) {
		pthread_mutex_lock(&g_v4l2_lock);
		p = everlive_v4l2_mmap_buffer(fd, (uint64_t)offset);
		pthread_mutex_unlock(&g_v4l2_lock);
		if (p)
			return p;
	}
	if (real_v4l2_mmap)
		return real_v4l2_mmap(start, length, prot, flags, fd, offset);
	if (!real_mmap)
		return direct_mmap(start, length, prot, flags, fd,
				   (off_t)offset);
	return real_mmap(start, length, prot, flags, fd, (off_t)offset);
}

int v4l2_munmap(void *_start, size_t length)
{
	bool ours;

	init_real_v4l2_funcs();
	pthread_mutex_lock(&g_v4l2_lock);
	ours = everlive_v4l2_munmap_buffer(_start);
	pthread_mutex_unlock(&g_v4l2_lock);
	if (ours)
		return 0;
	if (real_v4l2_munmap)
		return real_v4l2_munmap(_start, length);
	if (!real_munmap)
		return (int)syscall(SYS_munmap, _start, length);
	return real_munmap(_start, length);
}

struct dirent64 *readdir64(DIR *dirp)
{
	return (struct dirent64 *)readdir(dirp);
}

int stat64(const char *path, struct stat64 *buf)
{
	return stat(path, (struct stat *)buf);
}

int lstat64(const char *path, struct stat64 *buf)
{
	return lstat(path, (struct stat *)buf);
}

int fstat64(int fd, struct stat64 *buf)
{
	return fstat(fd, (struct stat *)buf);
}

int fstatat64(int dirfd, const char *pathname, struct stat64 *buf, int flags)
{
	return fstatat(dirfd, pathname, (struct stat *)buf, flags);
}

int __xstat(int ver, const char *path, struct stat *buf)
{
	(void)ver;
	return stat(path, buf);
}

int __xstat64(int ver, const char *path, struct stat64 *buf)
{
	(void)ver;
	return stat(path, (struct stat *)buf);
}

int __lxstat(int ver, const char *path, struct stat *buf)
{
	(void)ver;
	return lstat(path, buf);
}

int __lxstat64(int ver, const char *path, struct stat64 *buf)
{
	(void)ver;
	return lstat(path, (struct stat *)buf);
}

int __fxstat(int ver, int fd, struct stat *buf)
{
	(void)ver;
	return fstat(fd, buf);
}

int __fxstat64(int ver, int fd, struct stat64 *buf)
{
	(void)ver;
	return fstat(fd, (struct stat *)buf);
}

int __fxstatat(int ver, int dirfd, const char *pathname, struct stat *buf,
	       int flags)
{
	(void)ver;
	return fstatat(dirfd, pathname, buf, flags);
}

int __fxstatat64(int ver, int dirfd, const char *pathname,
		 struct stat64 *buf, int flags)
{
	(void)ver;
	return fstatat(dirfd, pathname, (struct stat *)buf, flags);
}

int __open_2(const char *pathname, int flags)
{
	return open(pathname, flags);
}

int __open64_2(const char *pathname, int flags)
{
	return open64(pathname, flags);
}

int __openat_2(int dirfd, const char *pathname, int flags)
{
	return openat(dirfd, pathname, flags);
}

int __openat64_2(int dirfd, const char *pathname, int flags)
{
	return openat64(dirfd, pathname, flags);
}

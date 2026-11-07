// SPDX-License-Identifier: GPL-2.0-only
/*
 * libgc573-v4l2.so — Userspace V4L2 LD_PRELOAD bridge for EverLive GC573
 *
 * Exposes virtual V4L2 capture device in pure userspace (no kernel modules needed):
 *   - /dev/video63 : "EverLive GC573 Live Gamer 4K" (bus_info: PCI:everlive:gc573)
 *
 * Intercepts POSIX file/ioctl/mmap APIs and libv4l2.so.0 wrapper APIs.
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
#include <linux/version.h>
#include <stdatomic.h>

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
#endif

#include "gc573_shm.h"

#define GC573_V4L2_MAX_FDS     64
#define GC573_V4L2_MAX_BUFS    8
#define GC573_V4L2_MAX_DIRS    16
#define GC573_V4L2_MMAP_BASE   0x50000000UL
#define GC573_V4L2_MMAP_STRIDE 0x02000000UL /* 32MB stride for 4K frames */

struct gc573_v4l2_buf {
	void *addr;
	size_t size;
	bool queued;
	bool done;
};

struct gc573_v4l2_handle {
	bool in_use;
	int fd;
	uint32_t width;
	uint32_t height;
	uint32_t pixfmt;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t req_count;
	bool streaming;
	uint32_t sequence;
	struct gc573_v4l2_buf bufs[GC573_V4L2_MAX_BUFS];
};

struct gc573_dir_track {
	DIR *dirp;
	int step;
	bool is_sysfs;
};

static struct gc573_v4l2_handle g_handles[GC573_V4L2_MAX_FDS];
static struct gc573_dir_track g_dev_dirs[GC573_V4L2_MAX_DIRS];
static pthread_mutex_t g_v4l2_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_open_handle_count = 0;
static struct gc573_shm_region *g_shm_ptr = NULL;

/*
 * Optional per-process opt-in.  Set EVERLIVE_V4L2_TARGET to a comma-separated
 * list of executable basenames (or "*") to keep this shim dormant in children
 * that inherit LD_PRELOAD.  Unset preserves the legacy global behaviour.
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

static uint64_t shim_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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

static struct gc573_shm_region *get_shm(void)
{
	if (g_shm_ptr)
		return g_shm_ptr;
	int fd = shm_open(GC573_SHM_NAME, O_RDONLY, 0);
	if (fd < 0)
		return NULL;
	void *p = real_mmap ? real_mmap(NULL, sizeof(struct gc573_shm_region),
					PROT_READ, MAP_SHARED, fd, 0) : NULL;
	if (real_close)
		real_close(fd);
	if (!p || p == MAP_FAILED)
		return NULL;
	g_shm_ptr = (struct gc573_shm_region *)p;
	return g_shm_ptr;
}

static uint32_t calc_bytesperline(uint32_t w, uint32_t pixfmt)
{
	if (pixfmt == V4L2_PIX_FMT_YUYV || pixfmt == V4L2_PIX_FMT_UYVY)
		return w * 2;
	if (pixfmt == V4L2_PIX_FMT_NV12 || pixfmt == V4L2_PIX_FMT_YUV420)
		return w;
	if (pixfmt == V4L2_PIX_FMT_RGB24 || pixfmt == V4L2_PIX_FMT_BGR24)
		return w * 3;
	return w * 4;
}

static uint32_t calc_sizeimage(uint32_t w, uint32_t h, uint32_t pixfmt)
{
	if (pixfmt == V4L2_PIX_FMT_YUYV || pixfmt == V4L2_PIX_FMT_UYVY)
		return w * h * 2;
	if (pixfmt == V4L2_PIX_FMT_NV12 || pixfmt == V4L2_PIX_FMT_YUV420)
		return w * h * 3 / 2;
	if (pixfmt == V4L2_PIX_FMT_RGB24 || pixfmt == V4L2_PIX_FMT_BGR24)
		return w * h * 3;
	return w * h * 4;
}

static inline uint8_t clamp_byte(int v)
{
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static inline void yuv_to_rgb(int y, int u, int v, uint8_t *r, uint8_t *g, uint8_t *b)
{
	int c = y - 16;
	int d = u - 128;
	int e = v - 128;
	if (c < 0) c = 0;
	*r = clamp_byte((298 * c + 459 * e + 128) >> 8);
	*g = clamp_byte((298 * c - 55 * d - 136 * e + 128) >> 8);
	*b = clamp_byte((298 * c + 541 * d + 128) >> 8);
}

static void convert_yuy2_frame(const uint8_t *src, uint32_t src_w, uint32_t src_h,
			       uint8_t *dst, size_t dst_max,
			       uint32_t dst_w, uint32_t dst_h, uint32_t pixfmt)
{
	if (src_w < 2 || src_w > GC573_SHM_MAX_WIDTH)
		src_w = 1920;
	if (src_h < 2 || src_h > GC573_SHM_MAX_HEIGHT)
		src_h = 1080;
	src_w &= ~1U;
	src_h &= ~1U;
	if (dst_w & 1U)
		dst_w &= ~1U;
	if (dst_h & 1U)
		dst_h &= ~1U;
	size_t needed = calc_sizeimage(dst_w, dst_h, pixfmt);
	if (!dst || dst_max < needed)
		return;
	if (!src) {
		memset(dst, 0, needed);
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_YUYV) {
		if (dst_w == src_w && dst_h == src_h) {
			memcpy(dst, src, needed);
			return;
		}
		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 2;
			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				memcpy(drow + x * 2, srow + sx * 2, 4);
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_UYVY) {
		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 2;
			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;
				drow[x * 2 + 0] = p[1];
				drow[x * 2 + 1] = p[0];
				drow[x * 2 + 2] = p[3];
				drow[x * 2 + 3] = p[2];
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_YUV420 || pixfmt == V4L2_PIX_FMT_NV12) {
		size_t y_size = (size_t)dst_w * dst_h;
		uint8_t *y_plane = dst;
		uint8_t *u_plane = dst + y_size;
		uint8_t *v_plane = dst + y_size + y_size / 4;

		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *yd = y_plane + (size_t)y * dst_w;
			bool even_y = (y & 1) == 0;

			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;
				yd[x]     = p[0];
				yd[x + 1] = p[2];

				if (even_y) {
					if (pixfmt == V4L2_PIX_FMT_NV12) {
						uint8_t *uvd = u_plane + ((size_t)(y / 2) * dst_w) + x;
						uvd[0] = p[1];
						uvd[1] = p[3];
					} else {
						u_plane[((size_t)(y / 2) * (dst_w / 2)) + (x / 2)] = p[1];
						v_plane[((size_t)(y / 2) * (dst_w / 2)) + (x / 2)] = p[3];
					}
				}
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_BGR24) {
		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 3;
			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;
				uint8_t r0, g0, b0, r1, g1, b1;
				yuv_to_rgb(p[0], p[1], p[3], &r0, &g0, &b0);
				yuv_to_rgb(p[2], p[1], p[3], &r1, &g1, &b1);
				drow[x * 3 + 0] = b0; drow[x * 3 + 1] = g0; drow[x * 3 + 2] = r0;
				drow[(x+1)*3+0] = b1; drow[(x+1)*3+1] = g1; drow[(x+1)*3+2] = r1;
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_RGB24) {
		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 3;
			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;
				uint8_t r0, g0, b0, r1, g1, b1;
				yuv_to_rgb(p[0], p[1], p[3], &r0, &g0, &b0);
				yuv_to_rgb(p[2], p[1], p[3], &r1, &g1, &b1);
				drow[x * 3 + 0] = r0; drow[x * 3 + 1] = g0; drow[x * 3 + 2] = b0;
				drow[(x+1)*3+0] = r1; drow[(x+1)*3+1] = g1; drow[(x+1)*3+2] = b1;
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_XBGR32 || pixfmt == V4L2_PIX_FMT_BGR32) {
		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 4;
			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;
				uint8_t r0, g0, b0, r1, g1, b1;
				yuv_to_rgb(p[0], p[1], p[3], &r0, &g0, &b0);
				yuv_to_rgb(p[2], p[1], p[3], &r1, &g1, &b1);
				drow[x * 4 + 0] = b0; drow[x * 4 + 1] = g0; drow[x * 4 + 2] = r0; drow[x * 4 + 3] = 255;
				drow[(x+1)*4+0] = b1; drow[(x+1)*4+1] = g1; drow[(x+1)*4+2] = r1; drow[(x+1)*4+3] = 255;
			}
		}
	}
}

static void shm_convert_frame(uint8_t *dst, size_t dst_max,
			      uint32_t dst_w, uint32_t dst_h, uint32_t pixfmt)
{
	struct gc573_shm_region *shm = get_shm();
	uint32_t src_w, src_h;
	uint64_t seq;

	if (!shm || shm->magic != GC573_SHM_MAGIC) {
		convert_yuy2_frame(NULL, 0, 0, dst, dst_max, dst_w, dst_h, pixfmt);
		return;
	}

	/* Stale publisher (crash/SIGKILL): emit black instead of a frozen frame. */
	{
		uint64_t heartbeat = __atomic_load_n(&shm->heartbeat_ns,
						     __ATOMIC_ACQUIRE);
		if (heartbeat && shim_now_ns() > heartbeat + 1000000000ULL) {
			convert_yuy2_frame(NULL, 0, 0, dst, dst_max,
					   dst_w, dst_h, pixfmt);
			return;
		}
	}

	for (int tries = 0; tries < 8; tries++) {
		seq = __atomic_load_n(&shm->port.seq, __ATOMIC_ACQUIRE);
		if (!seq || (seq & 1)) {
			usleep(100);
			continue;
		}

		src_w = __atomic_load_n(&shm->port.width, __ATOMIC_ACQUIRE);
		src_h = __atomic_load_n(&shm->port.height, __ATOMIC_ACQUIRE);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);

		convert_yuy2_frame(shm->port.yuy2, src_w, src_h,
				   dst, dst_max, dst_w, dst_h, pixfmt);

		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&shm->port.seq, __ATOMIC_ACQUIRE) == seq)
			return;
		usleep(100);
	}

	convert_yuy2_frame(NULL, 0, 0, dst, dst_max, dst_w, dst_h, pixfmt);
}

static int match_virtual_video_path(const char *path)
{
	if (!path || !shim_target_allowed())
		return -1;
	const char *env_dev = getenv("EVERLIVE_V4L2_DEVICE");
	if (env_dev && strcmp(path, env_dev) == 0)
		return 0;
	if (strcmp(path, "/dev/video63") == 0 || strcmp(path, "video63") == 0)
		return 0;
	return -1;
}

static struct gc573_v4l2_handle *find_handle(int fd)
{
	if (fd < 0 || atomic_load_explicit(&g_open_handle_count, memory_order_relaxed) <= 0)
		return NULL;
	for (int i = 0; i < GC573_V4L2_MAX_FDS; i++) {
		if (g_handles[i].in_use && g_handles[i].fd == fd)
			return &g_handles[i];
	}
	return NULL;
}

static struct dirent tls_fake_dirent;

static struct gc573_dir_track *find_dir_track(DIR *dirp)
{
	if (!dirp)
		return NULL;
	for (int i = 0; i < GC573_V4L2_MAX_DIRS; i++) {
		if (g_dev_dirs[i].dirp == dirp)
			return &g_dev_dirs[i];
	}
	return NULL;
}

static int match_virtual_sysfs_stat(const char *path, struct stat *st)
{
	if (!path || !shim_target_allowed())
		return -1;
	while (*path == '.' && *(path + 1) == '/')
		path += 2;
	if (strcmp(path, "/sys/class/video4linux") == 0 ||
	    strcmp(path, "/sys/class/video4linux/") == 0 ||
	    strcmp(path, "/sys/subsystem/video4linux/devices") == 0 ||
	    strcmp(path, "/sys/subsystem/video4linux/devices/") == 0) {
		if (st) {
			memset(st, 0, sizeof(*st));
			st->st_mode = S_IFDIR | 0755;
			st->st_nlink = 3;
		}
		return 0;
	}
	if (strcmp(path, "/sys/class/video4linux/video63") == 0 ||
	    strcmp(path, "/sys/class/video4linux/video63/") == 0) {
		if (st) {
			memset(st, 0, sizeof(*st));
			st->st_mode = S_IFDIR | 0755;
			st->st_nlink = 2;
		}
		return 0;
	}
	if (strncmp(path, "/sys/class/video4linux/video63/", 31) == 0) {
		if (st) {
			memset(st, 0, sizeof(*st));
			st->st_mode = S_IFREG | 0444;
			st->st_nlink = 1;
			st->st_size = 64;
		}
		return 0;
	}
	return -1;
}

static int open_virtual_sysfs(const char *path)
{
	if (!shim_target_allowed())
		return -1;
	if (strncmp(path, "/sys/class/video4linux/video63/", 31) != 0)
		return -1;
	const char *attr = path + 31;
	char content[256] = { 0 };

	if (strcmp(attr, "uevent") == 0) {
		snprintf(content, sizeof(content),
			 "MAJOR=81\nMINOR=63\nDEVNAME=video63\nSUBSYSTEM=video4linux\n");
	} else if (strcmp(attr, "name") == 0) {
		snprintf(content, sizeof(content), "EverLive GC573 Live Gamer 4K\n");
	} else if (strcmp(attr, "dev") == 0) {
		snprintf(content, sizeof(content), "81:63\n");
	} else if (strcmp(attr, "index") == 0) {
		snprintf(content, sizeof(content), "0\n");
	} else {
		return -1;
	}

	int fd = memfd_create("gc573_sysfs", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	size_t len = strlen(content);
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
	bool is_sysfs = (strcmp(name, "/sys/class/video4linux") == 0 ||
	                 strcmp(name, "/sys/class/video4linux/") == 0 ||
	                 strcmp(name, "/sys/subsystem/video4linux/devices") == 0 ||
	                 strcmp(name, "/sys/subsystem/video4linux/devices/") == 0);
	bool is_dev = (strcmp(name, "/dev") == 0 || strcmp(name, "/dev/") == 0);

	DIR *d = real_opendir(name);
	if (!d && is_sysfs)
		d = real_opendir("/tmp");

	if (d && (is_dev || is_sysfs)) {
		pthread_mutex_lock(&g_v4l2_lock);
		for (int i = 0; i < GC573_V4L2_MAX_DIRS; i++) {
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
	init_real_funcs();
	struct dirent *ent = real_readdir(dirp);
	if (!find_dir_track(dirp))
		return ent;
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc573_dir_track *tr = find_dir_track(dirp);
	if (!tr) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return ent;
	}

	if (tr->is_sysfs) {
		if (tr->step == 0) {
			tr->step++;
			pthread_mutex_unlock(&g_v4l2_lock);
			memset(&tls_fake_dirent, 0, sizeof(tls_fake_dirent));
			tls_fake_dirent.d_ino = 7063;
			tls_fake_dirent.d_type = DT_DIR;
			snprintf(tls_fake_dirent.d_name, sizeof(tls_fake_dirent.d_name), "video63");
			return &tls_fake_dirent;
		}
		pthread_mutex_unlock(&g_v4l2_lock);
		return NULL;
	}

	if (ent) {
		if (strcmp(ent->d_name, "video63") == 0) {
			pthread_mutex_unlock(&g_v4l2_lock);
			return readdir(dirp);
		}
		pthread_mutex_unlock(&g_v4l2_lock);
		return ent;
	}
	if (tr->step == 0) {
		tr->step++;
		pthread_mutex_unlock(&g_v4l2_lock);
		memset(&tls_fake_dirent, 0, sizeof(tls_fake_dirent));
		tls_fake_dirent.d_ino = 6063;
		tls_fake_dirent.d_type = DT_CHR;
		snprintf(tls_fake_dirent.d_name, sizeof(tls_fake_dirent.d_name), "video63");
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
		struct gc573_dir_track *tr = find_dir_track(dirp);
		if (tr) {
			tr->dirp = NULL;
			tr->step = 0;
			tr->is_sysfs = false;
		}
		pthread_mutex_unlock(&g_v4l2_lock);
	}
	return real_closedir(dirp);
}

static int fill_fake_video_stat(struct stat *st)
{
	if ((uintptr_t)st == 0) {
		errno = EFAULT;
		return -1;
	}
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | 0666;
	st->st_rdev = makedev(81, 63);
	st->st_nlink = 1;
	return 0;
}

int stat(const char *path, struct stat *buf)
{
	init_real_funcs();
	if (match_virtual_video_path(path) >= 0)
		return fill_fake_video_stat(buf);
	if (match_virtual_sysfs_stat(path, buf) == 0)
		return 0;
	return real_stat(path, buf);
}

int lstat(const char *path, struct stat *buf)
{
	init_real_funcs();
	if (match_virtual_video_path(path) >= 0)
		return fill_fake_video_stat(buf);
	if (match_virtual_sysfs_stat(path, buf) == 0)
		return 0;
	return real_lstat(path, buf);
}

int fstat(int fd, struct stat *buf)
{
	init_real_funcs();
	if (!find_handle(fd))
		return real_fstat(fd, buf);
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc573_v4l2_handle *h = find_handle(fd);
	if (h) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return fill_fake_video_stat(buf);
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	return real_fstat(fd, buf);
}

int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags)
{
	init_real_funcs();
	if (match_virtual_video_path(pathname) >= 0)
		return fill_fake_video_stat(buf);
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

static int open_virtual_video(int flags)
{
	(void)flags;
	pthread_mutex_lock(&g_v4l2_lock);
	int slot = -1;
	for (int i = 0; i < GC573_V4L2_MAX_FDS; i++) {
		if (!g_handles[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_v4l2_lock);
		errno = EMFILE;
		return -1;
	}
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (tfd < 0) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return -1;
	}

	struct gc573_v4l2_handle *h = &g_handles[slot];
	memset(h, 0, sizeof(*h));
	h->in_use = true;
	h->fd = tfd;
	h->width = 1920;
	h->height = 1080;
	h->pixfmt = V4L2_PIX_FMT_YUYV;
	h->fps_num = 1;
	h->fps_den = 60;
	atomic_fetch_add_explicit(&g_open_handle_count, 1, memory_order_relaxed);
	pthread_mutex_unlock(&g_v4l2_lock);
	return tfd;
}

int open(const char *path, int flags, ...)
{
	init_real_funcs();
	if (match_virtual_video_path(path) >= 0)
		return open_virtual_video(flags);
	if (match_virtual_sysfs_stat(path, NULL) == 0) {
		int fd = open_virtual_sysfs(path);
		if (fd >= 0)
			return fd;
	}
	va_list ap;
	va_start(ap, flags);
	mode_t mode = va_arg(ap, mode_t);
	va_end(ap);
	return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
	init_real_funcs();
	if (match_virtual_video_path(path) >= 0)
		return open_virtual_video(flags);
	if (match_virtual_sysfs_stat(path, NULL) == 0) {
		int fd = open_virtual_sysfs(path);
		if (fd >= 0)
			return fd;
	}
	va_list ap;
	va_start(ap, flags);
	mode_t mode = va_arg(ap, mode_t);
	va_end(ap);
	return real_open64(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
	init_real_funcs();
	if (match_virtual_video_path(path) >= 0)
		return open_virtual_video(flags);
	if (match_virtual_sysfs_stat(path, NULL) == 0) {
		int fd = open_virtual_sysfs(path);
		if (fd >= 0)
			return fd;
	}
	va_list ap;
	va_start(ap, flags);
	mode_t mode = va_arg(ap, mode_t);
	va_end(ap);
	return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
	init_real_funcs();
	if (match_virtual_video_path(path) >= 0)
		return open_virtual_video(flags);
	if (match_virtual_sysfs_stat(path, NULL) == 0) {
		int fd = open_virtual_sysfs(path);
		if (fd >= 0)
			return fd;
	}
	va_list ap;
	va_start(ap, flags);
	mode_t mode = va_arg(ap, mode_t);
	va_end(ap);
	return real_openat64(dirfd, path, flags, mode);
}

int close(int fd)
{
	init_real_funcs();
	if (!find_handle(fd))
		return real_close(fd);
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc573_v4l2_handle *h = find_handle(fd);
	if (h) {
		for (int i = 0; i < GC573_V4L2_MAX_BUFS; i++) {
			if (h->bufs[i].addr) {
				munmap(h->bufs[i].addr, h->bufs[i].size);
				h->bufs[i].addr = NULL;
			}
		}
		h->in_use = false;
		atomic_fetch_sub_explicit(&g_open_handle_count, 1, memory_order_relaxed);
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	return real_close(fd);
}

int dup(int oldfd)
{
	init_real_funcs();
	if (!find_handle(oldfd))
		return real_dup(oldfd);
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc573_v4l2_handle *oldh = find_handle(oldfd);
	if (!oldh) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return real_dup(oldfd);
	}
	int newfd = real_dup(oldfd);
	if (newfd >= 0) {
		int slot = -1;
		for (int i = 0; i < GC573_V4L2_MAX_FDS; i++) {
			if (!g_handles[i].in_use) {
				slot = i;
				break;
			}
		}
		if (slot >= 0) {
			g_handles[slot] = *oldh;
			g_handles[slot].fd = newfd;
			atomic_fetch_add_explicit(&g_open_handle_count, 1, memory_order_relaxed);
		}
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	return newfd;
}

int dup2(int oldfd, int newfd)
{
	init_real_funcs();
	if (!find_handle(oldfd))
		return real_dup2(oldfd, newfd);
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc573_v4l2_handle *oldh = find_handle(oldfd);
	if (!oldh) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return real_dup2(oldfd, newfd);
	}
	int retfd = real_dup2(oldfd, newfd);
	if (retfd >= 0) {
		int slot = -1;
		for (int i = 0; i < GC573_V4L2_MAX_FDS; i++) {
			if (g_handles[i].in_use && g_handles[i].fd == newfd) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			for (int i = 0; i < GC573_V4L2_MAX_FDS; i++) {
				if (!g_handles[i].in_use) {
					slot = i;
					break;
				}
			}
			if (slot >= 0)
				atomic_fetch_add_explicit(&g_open_handle_count, 1, memory_order_relaxed);
		}
		if (slot >= 0) {
			g_handles[slot] = *oldh;
			g_handles[slot].fd = newfd;
		}
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	return retfd;
}

static int handle_gc573_ioctl(struct gc573_v4l2_handle *h, unsigned long request, void *arg)
{
	int ret = 0;
	switch (request) {
	case VIDIOC_QUERYCAP: {
		struct v4l2_capability *cap = arg;
		if (!cap) { errno = EINVAL; ret = -1; break; }
		memset(cap, 0, sizeof(*cap));
		snprintf((char *)cap->driver, sizeof(cap->driver), "gc573-user");
		snprintf((char *)cap->card, sizeof(cap->card), "EverLive GC573 Live Gamer 4K");
		snprintf((char *)cap->bus_info, sizeof(cap->bus_info), "PCI:everlive:gc573");
		cap->version = KERNEL_VERSION(6, 6, 0);
		cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
				    V4L2_CAP_READWRITE | V4L2_CAP_DEVICE_CAPS;
		cap->device_caps = cap->capabilities;
		break;
	}
	case VIDIOC_ENUM_FMT: {
		struct v4l2_fmtdesc *f = arg;
		if (!f || f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL; ret = -1; break;
		}
		static const uint32_t fmts[] = {
			V4L2_PIX_FMT_YUYV,
			V4L2_PIX_FMT_UYVY,
			V4L2_PIX_FMT_BGR24,
			V4L2_PIX_FMT_XBGR32,
			V4L2_PIX_FMT_NV12,
			V4L2_PIX_FMT_YUV420,
			V4L2_PIX_FMT_RGB24,
		};
		static const char *names[] = {
			"YUYV 4:2:2",
			"UYVY 4:2:2",
			"24-bit BGR",
			"32-bit BGRx",
			"Y/UV 4:2:0 (NV12)",
			"Planar YUV 4:2:0 (I420)",
			"24-bit RGB",
		};
		if (f->index >= 7) {
			errno = EINVAL; ret = -1; break;
		}
		f->flags = 0;
		f->pixelformat = fmts[f->index];
		snprintf((char *)f->description, sizeof(f->description), "%s", names[f->index]);
		break;
	}
	case VIDIOC_G_FMT: {
		struct v4l2_format *f = arg;
		if (!f || f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL; ret = -1; break;
		}
		memset(&f->fmt.pix, 0, sizeof(f->fmt.pix));
		f->fmt.pix.width = h->width;
		f->fmt.pix.height = h->height;
		f->fmt.pix.pixelformat = h->pixfmt;
		f->fmt.pix.field = V4L2_FIELD_NONE;
		f->fmt.pix.bytesperline = calc_bytesperline(h->width, h->pixfmt);
		f->fmt.pix.sizeimage = calc_sizeimage(h->width, h->height, h->pixfmt);
		f->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
		f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_709;
		f->fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
		f->fmt.pix.xfer_func = V4L2_XFER_FUNC_709;
		break;
	}
	case VIDIOC_TRY_FMT:
	case VIDIOC_S_FMT: {
		struct v4l2_format *f = arg;
		if (!f || f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL; ret = -1; break;
		}
		uint32_t w = f->fmt.pix.width;
		uint32_t ht = f->fmt.pix.height;
		if (w > 2560 || ht > 1440) { w = 3840; ht = 2160; }
		else if (w > 1920 || ht > 1080) { w = 2560; ht = 1440; }
		else if (w > 1280 || ht > 720) { w = 1920; ht = 1080; }
		else if (w > 640 || ht > 480) { w = 1280; ht = 720; }
		else { w = 640; ht = 360; }
		w &= ~1U;
		ht &= ~1U;

		uint32_t pf = f->fmt.pix.pixelformat;
		if (pf != V4L2_PIX_FMT_YUYV && pf != V4L2_PIX_FMT_UYVY &&
		    pf != V4L2_PIX_FMT_NV12 && pf != V4L2_PIX_FMT_YUV420 &&
		    pf != V4L2_PIX_FMT_RGB24 && pf != V4L2_PIX_FMT_BGR24 &&
		    pf != V4L2_PIX_FMT_XBGR32 && pf != V4L2_PIX_FMT_BGR32)
			pf = V4L2_PIX_FMT_YUYV;

		f->fmt.pix.width = w;
		f->fmt.pix.height = ht;
		f->fmt.pix.pixelformat = pf;
		f->fmt.pix.field = V4L2_FIELD_NONE;
		f->fmt.pix.bytesperline = calc_bytesperline(w, pf);
		f->fmt.pix.sizeimage = calc_sizeimage(w, ht, pf);
		f->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
		f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_709;
		f->fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
		f->fmt.pix.xfer_func = V4L2_XFER_FUNC_709;
		if (request == VIDIOC_S_FMT) {
			if (h->req_count > 0 &&
			    (w != h->width || ht != h->height || pf != h->pixfmt)) {
				errno = EBUSY;
				ret = -1;
				break;
			}
			h->width = w;
			h->height = ht;
			h->pixfmt = pf;
		}
		break;
	}
	case VIDIOC_ENUM_FRAMESIZES: {
		struct v4l2_frmsizeenum *fs = arg;
		static const uint32_t sizes[5][2] = {
			{ 3840, 2160 }, { 2560, 1440 }, { 1920, 1080 }, { 1280, 720 }, { 640, 360 }
		};
		if (!fs || fs->index >= 5) {
			errno = EINVAL; ret = -1; break;
		}
		fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fs->discrete.width = sizes[fs->index][0];
		fs->discrete.height = sizes[fs->index][1];
		break;
	}
	case VIDIOC_ENUM_FRAMEINTERVALS: {
		struct v4l2_frmivalenum *fi = arg;
		static const uint32_t fps_list[6] = { 60, 120, 240, 144, 30, 50 };
		if (!fi || fi->index >= 6) {
			errno = EINVAL; ret = -1; break;
		}
		fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		fi->discrete.numerator = 1;
		fi->discrete.denominator = fps_list[fi->index];
		break;
	}
	case VIDIOC_ENUMINPUT: {
		struct v4l2_input *inp = arg;
		if (!inp || inp->index > 0) {
			errno = EINVAL; ret = -1; break;
		}
		memset(inp, 0, sizeof(*inp));
		inp->index = 0;
		snprintf((char *)inp->name, sizeof(inp->name), "HDMI IN");
		inp->type = V4L2_INPUT_TYPE_CAMERA;
		break;
	}
	case VIDIOC_G_INPUT: {
		int *i = arg;
		if (!i) { errno = EINVAL; ret = -1; break; }
		*i = 0;
		break;
	}
	case VIDIOC_S_INPUT: {
		int *i = arg;
		if (!i || *i != 0) { errno = EINVAL; ret = -1; }
		break;
	}
	case VIDIOC_G_PARM:
	case VIDIOC_S_PARM: {
		struct v4l2_streamparm *sp = arg;
		if (!sp || sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL; ret = -1; break;
		}
		if (request == VIDIOC_S_PARM &&
		    sp->parm.capture.timeperframe.numerator > 0 &&
		    sp->parm.capture.timeperframe.denominator > 0) {
			h->fps_num = sp->parm.capture.timeperframe.numerator;
			h->fps_den = sp->parm.capture.timeperframe.denominator;
		}
		memset(&sp->parm.capture, 0, sizeof(sp->parm.capture));
		sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
		sp->parm.capture.timeperframe.numerator = h->fps_num;
		sp->parm.capture.timeperframe.denominator = h->fps_den;
		sp->parm.capture.readbuffers = 4;
		break;
	}
	case VIDIOC_REQBUFS: {
		struct v4l2_requestbuffers *rb = arg;
		if (!rb || rb->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || rb->memory != V4L2_MEMORY_MMAP) {
			errno = EINVAL; ret = -1; break;
		}
		for (int i = 0; i < GC573_V4L2_MAX_BUFS; i++) {
			if (h->bufs[i].addr) {
				munmap(h->bufs[i].addr, h->bufs[i].size);
				h->bufs[i].addr = NULL;
			}
			h->bufs[i].queued = false;
			h->bufs[i].done = false;
		}
		if (rb->count > GC573_V4L2_MAX_BUFS)
			rb->count = GC573_V4L2_MAX_BUFS;
		h->req_count = rb->count;
		size_t sz = calc_sizeimage(h->width, h->height, h->pixfmt);
		for (uint32_t i = 0; i < h->req_count; i++) {
			h->bufs[i].size = sz;
			h->bufs[i].addr = mmap(NULL, sz, PROT_READ | PROT_WRITE,
					       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		}
		break;
	}
	case VIDIOC_QUERYBUF: {
		struct v4l2_buffer *b = arg;
		if (!b || b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || b->index >= h->req_count) {
			errno = EINVAL; ret = -1; break;
		}
		uint32_t idx = b->index;
		memset(b, 0, sizeof(*b));
		b->index = idx;
		b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b->memory = V4L2_MEMORY_MMAP;
		b->length = h->bufs[idx].size;
		b->m.offset = GC573_V4L2_MMAP_BASE + (idx * GC573_V4L2_MMAP_STRIDE);
		b->flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		if (h->bufs[idx].queued) b->flags |= V4L2_BUF_FLAG_QUEUED;
		if (h->bufs[idx].done)   b->flags |= V4L2_BUF_FLAG_DONE;
		break;
	}
	case VIDIOC_QBUF: {
		struct v4l2_buffer *b = arg;
		if (!b || b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || b->index >= h->req_count) {
			errno = EINVAL; ret = -1; break;
		}
		h->bufs[b->index].queued = true;
		h->bufs[b->index].done = false;
		b->flags = V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		break;
	}
	case VIDIOC_DQBUF: {
		struct v4l2_buffer *b = arg;
		if (!b || b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL; ret = -1; break;
		}
		uint64_t exp = 0;
		ssize_t s = read(h->fd, &exp, sizeof(exp));
		if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			ret = -1;
			break;
		}
		int target = -1;
		for (uint32_t i = 0; i < h->req_count; i++) {
			if (h->bufs[i].queued) {
				target = i;
				break;
			}
		}
		if (target < 0) {
			errno = EAGAIN;
			ret = -1;
			break;
		}

		shm_convert_frame(h->bufs[target].addr, h->bufs[target].size,
				  h->width, h->height, h->pixfmt);

		h->bufs[target].queued = false;
		h->bufs[target].done = true;
		memset(b, 0, sizeof(*b));
		b->index = target;
		b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b->bytesused = h->bufs[target].size;
		b->length = h->bufs[target].size;
		b->memory = V4L2_MEMORY_MMAP;
		b->m.offset = GC573_V4L2_MMAP_BASE + (target * GC573_V4L2_MMAP_STRIDE);
		b->sequence = h->sequence++;
		b->flags = V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		clock_gettime(CLOCK_MONOTONIC, (struct timespec *)&b->timestamp);
		break;
	}
	case VIDIOC_STREAMON: {
		int *type = arg;
		if (!type || *type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL; ret = -1; break;
		}
		h->streaming = true;
		struct itimerspec its;
		uint64_t interval_ns = (1000000000ULL * h->fps_num) / (h->fps_den ? h->fps_den : 60);
		if (interval_ns < 4166666ULL) interval_ns = 4166666ULL; /* Max 240 fps */
		its.it_interval.tv_sec = interval_ns / 1000000000ULL;
		its.it_interval.tv_nsec = interval_ns % 1000000000ULL;
		its.it_value = its.it_interval;
		timerfd_settime(h->fd, 0, &its, NULL);
		break;
	}
	case VIDIOC_STREAMOFF: {
		int *type = arg;
		if (!type || *type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL; ret = -1; break;
		}
		h->streaming = false;
		struct itimerspec its = { 0 };
		timerfd_settime(h->fd, 0, &its, NULL);
		for (uint32_t i = 0; i < h->req_count; i++) {
			h->bufs[i].queued = false;
			h->bufs[i].done = false;
		}
		break;
	}
	case VIDIOC_CROPCAP: {
		struct v4l2_cropcap *cc = arg;
		if (!cc || cc->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL; ret = -1; break;
		}
		memset(cc, 0, sizeof(*cc));
		cc->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		cc->bounds.width = h->width;
		cc->bounds.height = h->height;
		cc->defrect = cc->bounds;
		cc->pixelaspect.numerator = 1;
		cc->pixelaspect.denominator = 1;
		break;
	}
	case VIDIOC_G_SELECTION:
	case VIDIOC_S_SELECTION: {
		struct v4l2_selection *sel = arg;
		if (!sel || sel->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			errno = EINVAL; ret = -1; break;
		}
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = h->width;
		sel->r.height = h->height;
		break;
	}
	default:
		errno = ENOTTY;
		ret = -1;
		break;
	}
	return ret;
}

int ioctl(int fd, unsigned long request, ...)
{
	init_real_funcs();
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	if (!find_handle(fd))
		return real_ioctl(fd, request, arg);

	pthread_mutex_lock(&g_v4l2_lock);
	struct gc573_v4l2_handle *h = find_handle(fd);
	if (!h) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return real_ioctl(fd, request, arg);
	}
	int ret = handle_gc573_ioctl(h, request, arg);
	pthread_mutex_unlock(&g_v4l2_lock);
	return ret;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	init_real_funcs();
	if (!find_handle(fd))
		return real_mmap(addr, length, prot, flags, fd, offset);

	pthread_mutex_lock(&g_v4l2_lock);
	struct gc573_v4l2_handle *h = find_handle(fd);
	if (!h) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return real_mmap(addr, length, prot, flags, fd, offset);
	}

	if (offset >= (off_t)GC573_V4L2_MMAP_BASE) {
		uint32_t idx = (offset - GC573_V4L2_MMAP_BASE) / GC573_V4L2_MMAP_STRIDE;
		if (idx < h->req_count && h->bufs[idx].addr) {
			void *ptr = h->bufs[idx].addr;
			pthread_mutex_unlock(&g_v4l2_lock);
			return ptr;
		}
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	errno = EINVAL;
	return MAP_FAILED;
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	return mmap(addr, length, prot, flags, fd, offset);
}

int munmap(void *addr, size_t length)
{
	init_real_funcs();
	if (atomic_load_explicit(&g_open_handle_count, memory_order_relaxed) > 0) {
		pthread_mutex_lock(&g_v4l2_lock);
		for (int i = 0; i < GC573_V4L2_MAX_FDS; i++) {
			if (!g_handles[i].in_use)
				continue;
			for (int b = 0; b < GC573_V4L2_MAX_BUFS; b++) {
				if (g_handles[i].bufs[b].addr == addr) {
					pthread_mutex_unlock(&g_v4l2_lock);
					return 0;
				}
			}
		}
		pthread_mutex_unlock(&g_v4l2_lock);
	}
	return real_munmap(addr, length);
}

/* libv4l2 wrapper interceptions */
int v4l2_open(const char *path, int flags, ...)
{
	init_real_v4l2_funcs();
	if (match_virtual_video_path(path) >= 0)
		return open_virtual_video(flags);
	va_list ap;
	va_start(ap, flags);
	mode_t mode = va_arg(ap, mode_t);
	va_end(ap);
	if (real_v4l2_open)
		return real_v4l2_open(path, flags, mode);
	return open(path, flags, mode);
}

int v4l2_fd_open(int fd, int flags)
{
	init_real_v4l2_funcs();
	if (find_handle(fd))
		return fd;
	if (real_v4l2_fd_open)
		return real_v4l2_fd_open(fd, flags);
	return fd;
}

int v4l2_close(int fd)
{
	init_real_v4l2_funcs();
	if (find_handle(fd))
		return close(fd);
	if (real_v4l2_close)
		return real_v4l2_close(fd);
	return close(fd);
}

int v4l2_dup(int fd)
{
	init_real_v4l2_funcs();
	if (find_handle(fd))
		return dup(fd);
	if (real_v4l2_dup)
		return real_v4l2_dup(fd);
	return dup(fd);
}

int v4l2_ioctl(int fd, unsigned long request, ...)
{
	init_real_v4l2_funcs();
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (find_handle(fd))
		return ioctl(fd, request, arg);
	if (real_v4l2_ioctl)
		return real_v4l2_ioctl(fd, request, arg);
	return ioctl(fd, request, arg);
}

ssize_t v4l2_read(int fd, void *buffer, size_t n)
{
	init_real_v4l2_funcs();
	if (find_handle(fd)) {
		pthread_mutex_lock(&g_v4l2_lock);
		struct gc573_v4l2_handle *h = find_handle(fd);
		if (!h) {
			pthread_mutex_unlock(&g_v4l2_lock);
			errno = EBADF;
			return -1;
		}
		shm_convert_frame((uint8_t *)buffer, n, h->width, h->height, h->pixfmt);
		pthread_mutex_unlock(&g_v4l2_lock);
		return n;
	}
	if (real_v4l2_read)
		return real_v4l2_read(fd, buffer, n);
	return read(fd, buffer, n);
}

void *v4l2_mmap(void *start, size_t length, int prot, int flags, int fd, int64_t offset)
{
	init_real_v4l2_funcs();
	if (find_handle(fd))
		return mmap(start, length, prot, flags, fd, (off_t)offset);
	if (real_v4l2_mmap)
		return real_v4l2_mmap(start, length, prot, flags, fd, offset);
	return mmap(start, length, prot, flags, fd, (off_t)offset);
}

int v4l2_munmap(void *start, size_t length)
{
	init_real_v4l2_funcs();
	if (atomic_load_explicit(&g_open_handle_count, memory_order_relaxed) > 0) {
		pthread_mutex_lock(&g_v4l2_lock);
		for (int i = 0; i < GC573_V4L2_MAX_FDS; i++) {
			if (!g_handles[i].in_use)
				continue;
			for (int b = 0; b < GC573_V4L2_MAX_BUFS; b++) {
				if (g_handles[i].bufs[b].addr == start) {
					pthread_mutex_unlock(&g_v4l2_lock);
					return 0;
				}
			}
		}
		pthread_mutex_unlock(&g_v4l2_lock);
	}
	if (real_v4l2_munmap)
		return real_v4l2_munmap(start, length);
	return munmap(start, length);
}

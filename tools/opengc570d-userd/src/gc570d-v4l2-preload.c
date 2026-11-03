// SPDX-License-Identifier: GPL-2.0-only
/*
 * libgc570d-v4l2.so — Userspace V4L2 LD_PRELOAD bridge for OpenGC570D
 *
 * Exposes two virtual V4L2 capture devices in pure userspace (with zero kernel
 * modules, satisfying Secure Boot + Lockdown Integrity + Epic Games EAC):
 *   - /dev/video60 : "GC570D HDMI 1 (Passthrough)" (bus_info: PCI:0000:06:00.0:hdmi1)
 *   - /dev/video61 : "GC570D HDMI 2 (Capture)"     (bus_info: PCI:0000:06:00.0:hdmi2)
 *
 * Intercepts both glibc's POSIX file/ioctl/mmap APIs (used by v4l2-ctl,
 * Chromium/Discord/Electron, FFmpeg, GStreamer) AND libv4l2.so.0's wrapper APIs
 * (v4l2_open, v4l2_fd_open, v4l2_close, v4l2_dup, v4l2_ioctl, v4l2_read,
 * v4l2_mmap, v4l2_munmap) used by OBS Studio's linux-v4l2 plugin so libv4l2
 * never issues raw kernel syscalls on our virtual timerfd descriptors.
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
#include <linux/videodev2.h>

#include "gc570d-shm.h"

#define GC570D_V4L2_MAX_FDS     64
#define GC570D_V4L2_MAX_BUFS    8
#define GC570D_V4L2_MAX_DIRS    16
#define GC570D_V4L2_MMAP_BASE   0x50000000UL
#define GC570D_V4L2_MMAP_STRIDE 0x00800000UL

struct gc570d_v4l2_buf {
	void *addr;
	size_t size;
	bool queued;
	bool done;
};

struct gc570d_v4l2_handle {
	bool in_use;
	int fd;
	int port; /* 0 = /dev/video60 (HDMI 1), 1 = /dev/video61 (HDMI 2) */
	uint32_t width;
	uint32_t height;
	uint32_t pixfmt;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t req_count;
	bool streaming;
	uint32_t sequence;
	struct gc570d_v4l2_buf bufs[GC570D_V4L2_MAX_BUFS];
};

struct gc570d_dir_track {
	DIR *dirp;
	int step;
};

#include <stdatomic.h>

static struct gc570d_v4l2_handle g_handles[GC570D_V4L2_MAX_FDS];
static struct gc570d_dir_track g_dev_dirs[GC570D_V4L2_MAX_DIRS];
static pthread_mutex_t g_v4l2_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_open_handle_count = 0;
static struct gc570d_shm_region *g_shm_ptr = NULL;



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

static struct gc570d_shm_region *get_shm(void)
{
	if (g_shm_ptr)
		return g_shm_ptr;
	int fd = shm_open(GC570D_SHM_NAME, O_RDONLY, 0);
	if (fd < 0)
		return NULL;
	void *p = real_mmap ? real_mmap(NULL, sizeof(struct gc570d_shm_region),
					PROT_READ, MAP_SHARED, fd, 0) : NULL;
	if (real_close)
		real_close(fd);
	if (!p || p == MAP_FAILED)
		return NULL;
	g_shm_ptr = (struct gc570d_shm_region *)p;
	return g_shm_ptr;
}

static int match_virtual_video_path(const char *path)
{
	if (!path)
		return -1;
	if (strcmp(path, "/dev/video60") == 0 || strcmp(path, "video60") == 0)
		return 0;
	if (strcmp(path, "/dev/video61") == 0 || strcmp(path, "video61") == 0)
		return 1;
	return -1;
}

static struct gc570d_v4l2_handle *find_handle(int fd)
{
	if (fd < 0 || atomic_load_explicit(&g_open_handle_count, memory_order_relaxed) <= 0)
		return NULL;
	for (int i = 0; i < GC570D_V4L2_MAX_FDS; i++) {
		if (g_handles[i].in_use && g_handles[i].fd == fd)
			return &g_handles[i];
	}
	return NULL;
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
	if (src_w < 2 || src_w > 1920)
		src_w = 1920;
	if (src_h < 2 || src_h > 1080)
		src_h = 1080;
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
						uint8_t *uv = u_plane + (size_t)(y / 2) * dst_w + x;
						uv[0] = p[1];
						uv[1] = p[3];
					} else {
						u_plane[(y / 2) * (dst_w / 2) + (x / 2)] = p[1];
						v_plane[(y / 2) * (dst_w / 2) + (x / 2)] = p[3];
					}
				}
			}
		}
		return;
	}

	if (pixfmt == V4L2_PIX_FMT_RGB24 || pixfmt == V4L2_PIX_FMT_BGR24) {
		bool bgr = (pixfmt == V4L2_PIX_FMT_BGR24);
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
				if (bgr) {
					drow[x * 3 + 0] = b0; drow[x * 3 + 1] = g0; drow[x * 3 + 2] = r0;
					drow[(x+1)*3+0] = b1; drow[(x+1)*3+1] = g1; drow[(x+1)*3+2] = r1;
				} else {
					drow[x * 3 + 0] = r0; drow[x * 3 + 1] = g0; drow[x * 3 + 2] = b0;
					drow[(x+1)*3+0] = r1; drow[(x+1)*3+1] = g1; drow[(x+1)*3+2] = b1;
				}
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

/* Directory injection for /dev so OBS, v4l2-ctl --list-devices, and Discord find /dev/video60 and /dev/video61 */
static struct dirent tls_fake_dirent;

static struct gc570d_dir_track *find_dir_track(DIR *dirp)
{
	if (!dirp)
		return NULL;
	for (int i = 0; i < GC570D_V4L2_MAX_DIRS; i++) {
		if (g_dev_dirs[i].dirp == dirp)
			return &g_dev_dirs[i];
	}
	return NULL;
}

DIR *opendir(const char *name)
{
	init_real_funcs();
	DIR *d = real_opendir(name);
	if (d && name && (strcmp(name, "/dev") == 0 || strcmp(name, "/dev/") == 0)) {
		pthread_mutex_lock(&g_v4l2_lock);
		for (int i = 0; i < GC570D_V4L2_MAX_DIRS; i++) {
			if (!g_dev_dirs[i].dirp) {
				g_dev_dirs[i].dirp = d;
				g_dev_dirs[i].step = 0;
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
	struct gc570d_dir_track *tr = find_dir_track(dirp);
	if (ent) {
		if (tr && (strcmp(ent->d_name, "video60") == 0 ||
			   strcmp(ent->d_name, "video61") == 0)) {
			pthread_mutex_unlock(&g_v4l2_lock);
			return readdir(dirp);
		}
		pthread_mutex_unlock(&g_v4l2_lock);
		return ent;
	}
	if (tr && tr->step < 2) {
		int idx = tr->step++;
		pthread_mutex_unlock(&g_v4l2_lock);
		memset(&tls_fake_dirent, 0, sizeof(tls_fake_dirent));
		tls_fake_dirent.d_ino = 6000 + idx;
		tls_fake_dirent.d_type = DT_CHR;
		snprintf(tls_fake_dirent.d_name, sizeof(tls_fake_dirent.d_name),
			 "video%d", 60 + idx);
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
		struct gc570d_dir_track *tr = find_dir_track(dirp);
		if (tr) {
			tr->dirp = NULL;
			tr->step = 0;
		}
		pthread_mutex_unlock(&g_v4l2_lock);
	}
	return real_closedir(dirp);
}

static void fill_fake_video_stat(int port, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | 0666;
	st->st_rdev = makedev(81, 60 + port);
	st->st_nlink = 1;
}

int stat(const char *path, struct stat *buf)
{
	init_real_funcs();
	int port = match_virtual_video_path(path);
	if (port >= 0 && buf) {
		fill_fake_video_stat(port, buf);
		return 0;
	}
	return real_stat(path, buf);
}

int lstat(const char *path, struct stat *buf)
{
	init_real_funcs();
	int port = match_virtual_video_path(path);
	if (port >= 0 && buf) {
		fill_fake_video_stat(port, buf);
		return 0;
	}
	return real_lstat(path, buf);
}

int fstat(int fd, struct stat *buf)
{
	init_real_funcs();
	if (!find_handle(fd))
		return real_fstat(fd, buf);
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc570d_v4l2_handle *h = find_handle(fd);
	if (h && buf) {
		int port = h->port;
		pthread_mutex_unlock(&g_v4l2_lock);
		fill_fake_video_stat(port, buf);
		return 0;
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	return real_fstat(fd, buf);
}

int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags)
{
	init_real_funcs();
	int port = match_virtual_video_path(pathname);
	if (port >= 0 && buf) {
		fill_fake_video_stat(port, buf);
		return 0;
	}
	return real_fstatat(dirfd, pathname, buf, flags);
}

int access(const char *pathname, int mode)
{
	init_real_funcs();
	if (match_virtual_video_path(pathname) >= 0)
		return 0;
	return real_access(pathname, mode);
}

static int open_virtual_video(int port, int flags)
{
	pthread_mutex_lock(&g_v4l2_lock);
	int slot = -1;
	for (int i = 0; i < GC570D_V4L2_MAX_FDS; i++) {
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
	int tfd = timerfd_create(CLOCK_MONOTONIC,
		TFD_CLOEXEC | ((flags & O_NONBLOCK) ? TFD_NONBLOCK : 0));
	if (tfd < 0) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return -1;
	}
	struct gc570d_v4l2_handle *h = &g_handles[slot];
	memset(h, 0, sizeof(*h));
	h->in_use = true;
	h->fd = tfd;
	h->port = port;
	h->width = 1920;
	h->height = 1080;
	h->pixfmt = V4L2_PIX_FMT_YUYV;
	h->fps_num = 1;
	h->fps_den = 60;
	atomic_fetch_add_explicit(&g_open_handle_count, 1, memory_order_relaxed);
	pthread_mutex_unlock(&g_v4l2_lock);
	return tfd;
}

int open(const char *pathname, int flags, ...)
{
	init_real_funcs();
	int port = match_virtual_video_path(pathname);
	if (port >= 0)
		return open_virtual_video(port, flags);
	mode_t mode = 0;
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
	init_real_funcs();
	int port = match_virtual_video_path(pathname);
	if (port >= 0)
		return open_virtual_video(port, flags);
	mode_t mode = 0;
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
	init_real_funcs();
	int port = match_virtual_video_path(pathname);
	if (port >= 0)
		return open_virtual_video(port, flags);
	mode_t mode = 0;
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
	init_real_funcs();
	int port = match_virtual_video_path(pathname);
	if (port >= 0)
		return open_virtual_video(port, flags);
	mode_t mode = 0;
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
	init_real_funcs();
	if (!find_handle(fd))
		return real_close(fd);
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc570d_v4l2_handle *h = find_handle(fd);
	if (h) {
		for (int i = 0; i < GC570D_V4L2_MAX_BUFS; i++) {
			if (h->bufs[i].addr && h->bufs[i].size)
				real_munmap(h->bufs[i].addr, h->bufs[i].size);
		}
		memset(h, 0, sizeof(*h));
		atomic_fetch_sub_explicit(&g_open_handle_count, 1, memory_order_relaxed);
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	return real_close(fd);
}

static int handle_virtual_ioctl(struct gc570d_v4l2_handle *h,
				unsigned long request, void *arg)
{
	int ret = 0;

	switch (request) {
	case VIDIOC_QUERYCAP: {
		struct v4l2_capability *cap = arg;
		if (!cap) { errno = EINVAL; ret = -1; break; }
		memset(cap, 0, sizeof(*cap));
		snprintf((char *)cap->driver, sizeof(cap->driver), "gc570d-user");
		snprintf((char *)cap->card, sizeof(cap->card),
			 h->port == 0 ? "GC570D HDMI 1 (Passthrough)" : "GC570D HDMI 2 (Capture)");
		snprintf((char *)cap->bus_info, sizeof(cap->bus_info),
			 h->port == 0 ? "PCI:0000:06:00.0-hdmi1" : "PCI:0000:06:00.0-hdmi2");
		cap->version = (6 << 16) | (12 << 8) | 0;
		cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
		cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
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
		if (w <= 640 && ht <= 480) { w = 640; ht = 360; }
		else if (w <= 1280 && ht <= 720) { w = 1280; ht = 720; }
		else { w = 1920; ht = 1080; }

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
			h->width = w;
			h->height = ht;
			h->pixfmt = pf;
		}
		break;
	}
	case VIDIOC_ENUM_FRAMESIZES: {
		struct v4l2_frmsizeenum *fs = arg;
		static const uint32_t sizes[3][2] = {
			{ 1920, 1080 }, { 1280, 720 }, { 640, 360 }
		};
		if (!fs || fs->index >= 3) {
			errno = EINVAL; ret = -1; break;
		}
		fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fs->discrete.width = sizes[fs->index][0];
		fs->discrete.height = sizes[fs->index][1];
		break;
	}
	case VIDIOC_ENUM_FRAMEINTERVALS: {
		struct v4l2_frmivalenum *fi = arg;
		static const uint32_t fps_list[3] = { 60, 30, 50 };
		if (!fi || fi->index >= 3) {
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
		uint32_t idx = inp->index;
		memset(inp, 0, sizeof(*inp));
		inp->index = idx;
		snprintf((char *)inp->name, sizeof(inp->name), "HDMI IN %d", h->port + 1);
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
		for (int i = 0; i < GC570D_V4L2_MAX_BUFS; i++) {
			if (h->bufs[i].addr && h->bufs[i].size)
				real_munmap(h->bufs[i].addr, h->bufs[i].size);
			memset(&h->bufs[i], 0, sizeof(h->bufs[i]));
		}
		if (rb->count == 0) {
			h->req_count = 0;
			break;
		}
		if (rb->count < 2) rb->count = 2;
		if (rb->count > GC570D_V4L2_MAX_BUFS) rb->count = GC570D_V4L2_MAX_BUFS;
		size_t sz = calc_sizeimage(h->width, h->height, h->pixfmt);
		for (uint32_t i = 0; i < rb->count; i++) {
			void *p = real_mmap(NULL, sz, PROT_READ | PROT_WRITE,
					    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (p == MAP_FAILED) {
				errno = ENOMEM; ret = -1; break;
			}
			h->bufs[i].addr = p;
			h->bufs[i].size = sz;
		}
		h->req_count = rb->count;
		break;
	}
	case VIDIOC_QUERYBUF: {
		struct v4l2_buffer *b = arg;
		if (!b || b->index >= h->req_count) {
			errno = EINVAL; ret = -1; break;
		}
		size_t sz = h->bufs[b->index].size;
		b->bytesused = sz;
		b->length = sz;
		b->field = V4L2_FIELD_NONE;
		b->memory = V4L2_MEMORY_MMAP;
		b->m.offset = GC570D_V4L2_MMAP_BASE + b->index * GC570D_V4L2_MMAP_STRIDE;
		b->flags = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
			   (h->bufs[b->index].queued ? V4L2_BUF_FLAG_QUEUED : 0) |
			   (h->bufs[b->index].done ? V4L2_BUF_FLAG_DONE : 0);
		break;
	}
	case VIDIOC_QBUF: {
		struct v4l2_buffer *b = arg;
		if (!b || b->index >= h->req_count) {
			errno = EINVAL; ret = -1; break;
		}
		h->bufs[b->index].queued = true;
		h->bufs[b->index].done = false;
		b->flags = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
			   V4L2_BUF_FLAG_QUEUED;
		break;
	}
	case VIDIOC_DQBUF: {
		struct v4l2_buffer *b = arg;
		if (!b) {
			errno = EINVAL; ret = -1; break;
		}
		int found = -1;
		for (uint32_t i = 0; i < h->req_count; i++) {
			if (h->bufs[i].queued) {
				found = (int)i;
				break;
			}
		}
		if (found < 0) {
			errno = EAGAIN; ret = -1; break;
		}
		uint64_t expirations = 0;
		if (read(h->fd, &expirations, sizeof(expirations)) < 0 && errno == EAGAIN) {
			errno = EAGAIN; ret = -1; break;
		}

		struct gc570d_shm_region *shm = get_shm();
		const uint8_t *src_yuy2 = NULL;
		uint32_t src_w = 1920, src_h = 1080;
		if (shm && shm->magic == GC570D_SHM_MAGIC) {
			src_yuy2 = shm->ports[h->port].yuy2;
			src_w = __atomic_load_n(&shm->ports[h->port].width, __ATOMIC_RELAXED);
			src_h = __atomic_load_n(&shm->ports[h->port].height, __ATOMIC_RELAXED);
		}
		convert_yuy2_frame(src_yuy2, src_w, src_h,
				   h->bufs[found].addr, h->bufs[found].size,
				   h->width, h->height, h->pixfmt);

		h->bufs[found].queued = false;
		h->bufs[found].done = true;
		b->index = found;
		b->bytesused = h->bufs[found].size;
		b->length = h->bufs[found].size;
		b->field = V4L2_FIELD_NONE;
		b->sequence = h->sequence++;
		b->memory = V4L2_MEMORY_MMAP;
		b->m.offset = GC570D_V4L2_MMAP_BASE + found * GC570D_V4L2_MMAP_STRIDE;
		b->flags = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		b->timestamp.tv_sec = ts.tv_sec;
		b->timestamp.tv_usec = ts.tv_nsec / 1000;
		break;
	}
	case VIDIOC_STREAMON: {
		h->streaming = true;
		uint64_t interval_ns = (1000000000ULL * h->fps_num) / (h->fps_den ? h->fps_den : 60);
		if (interval_ns < 4000000ULL)
			interval_ns = 16666667ULL;
		struct itimerspec its = {
			.it_interval = {
				.tv_sec  = (time_t)(interval_ns / 1000000000ULL),
				.tv_nsec = (long)(interval_ns % 1000000000ULL),
			},
			.it_value = { .tv_sec = 0, .tv_nsec = 1000000L },
		};
		timerfd_settime(h->fd, 0, &its, NULL);
		break;
	}
	case VIDIOC_STREAMOFF: {
		h->streaming = false;
		for (int i = 0; i < GC570D_V4L2_MAX_BUFS; i++) {
			h->bufs[i].queued = false;
			h->bufs[i].done = false;
		}
		struct itimerspec its;
		memset(&its, 0, sizeof(its));
		timerfd_settime(h->fd, 0, &its, NULL);
		break;
	}
	case VIDIOC_QUERYCTRL:
	case VIDIOC_QUERY_EXT_CTRL:
	case VIDIOC_QUERYMENU:
	case VIDIOC_G_CTRL:
	case VIDIOC_S_CTRL:
	case VIDIOC_G_EXT_CTRLS:
	case VIDIOC_S_EXT_CTRLS:
	case VIDIOC_TRY_EXT_CTRLS:
		errno = EINVAL;
		ret = -1;
		break;
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
	struct gc570d_v4l2_handle *h = find_handle(fd);
	if (!h) {
		pthread_mutex_unlock(&g_v4l2_lock);
		return real_ioctl(fd, request, arg);
	}
	int ret = handle_virtual_ioctl(h, request, arg);
	pthread_mutex_unlock(&g_v4l2_lock);
	return ret;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	init_real_funcs();
	if (fd >= 0 && (unsigned long)offset >= GC570D_V4L2_MMAP_BASE && find_handle(fd)) {
		pthread_mutex_lock(&g_v4l2_lock);
		struct gc570d_v4l2_handle *h = find_handle(fd);
		if (h) {
			unsigned int idx = ((unsigned long)offset - GC570D_V4L2_MMAP_BASE) / GC570D_V4L2_MMAP_STRIDE;
			if (idx < h->req_count && h->bufs[idx].addr) {
				void *p = h->bufs[idx].addr;
				pthread_mutex_unlock(&g_v4l2_lock);
				return p;
			}
		}
		pthread_mutex_unlock(&g_v4l2_lock);
	}
	if (!real_mmap)
		return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
	return real_mmap(addr, length, prot, flags, fd, offset);
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
		for (int i = 0; i < GC570D_V4L2_MAX_FDS; i++) {
			if (!g_handles[i].in_use)
				continue;
			for (int j = 0; j < GC570D_V4L2_MAX_BUFS; j++) {
				if (g_handles[i].bufs[j].addr == addr) {
					pthread_mutex_unlock(&g_v4l2_lock);
					return 0;
				}
			}
		}
		pthread_mutex_unlock(&g_v4l2_lock);
	}
	if (!real_munmap)
		return (int)syscall(SYS_munmap, addr, length);
	return real_munmap(addr, length);
}

/* =========================================================================
 * libv4l2.so.0 Wrapper Interceptors (Required for OBS Studio linux-v4l2)
 * ========================================================================= */

int v4l2_open(const char *file, int oflag, ...)
{
	init_real_v4l2_funcs();
	int port = match_virtual_video_path(file);
	if (port >= 0)
		return open_virtual_video(port, oflag);
	mode_t mode = 0;
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
	init_real_v4l2_funcs();
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc570d_v4l2_handle *h = find_handle(fd);
	pthread_mutex_unlock(&g_v4l2_lock);
	if (h)
		return fd;
	if (real_v4l2_fd_open)
		return real_v4l2_fd_open(fd, v4l2_flags);
	return fd;
}

int v4l2_close(int fd)
{
	init_real_v4l2_funcs();
	if (find_handle(fd))
		return close(fd);
	if (real_v4l2_close)
		return real_v4l2_close(fd);
	return real_close(fd);
}

int v4l2_dup(int fd)
{
	init_real_v4l2_funcs();
	if (!find_handle(fd)) {
		if (real_v4l2_dup)
			return real_v4l2_dup(fd);
		return real_dup(fd);
	}
	int newfd = real_dup(fd);
	if (newfd < 0)
		return newfd;
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc570d_v4l2_handle *h = find_handle(fd);
	if (h) {
		for (int i = 0; i < GC570D_V4L2_MAX_FDS; i++) {
			if (!g_handles[i].in_use) {
				g_handles[i] = *h;
				g_handles[i].fd = newfd;
				memset(g_handles[i].bufs, 0, sizeof(g_handles[i].bufs));
				g_handles[i].req_count = 0;
				atomic_fetch_add_explicit(&g_open_handle_count, 1, memory_order_relaxed);
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	return newfd;
}

int v4l2_ioctl(int fd, unsigned long int request, ...)
{
	init_real_v4l2_funcs();
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	pthread_mutex_lock(&g_v4l2_lock);
	struct gc570d_v4l2_handle *h = find_handle(fd);
	if (h) {
		int ret = handle_virtual_ioctl(h, request, arg);
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
	init_real_v4l2_funcs();
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc570d_v4l2_handle *h = find_handle(fd);
	if (h) {
		size_t sz = calc_sizeimage(h->width, h->height, h->pixfmt);
		if (n < sz) {
			pthread_mutex_unlock(&g_v4l2_lock);
			errno = EINVAL;
			return -1;
		}
		struct gc570d_shm_region *shm = get_shm();
		const uint8_t *src_yuy2 = NULL;
		uint32_t src_w = 1920, src_h = 1080;
		if (shm && shm->magic == GC570D_SHM_MAGIC) {
			src_yuy2 = shm->ports[h->port].yuy2;
			src_w = __atomic_load_n(&shm->ports[h->port].width, __ATOMIC_RELAXED);
			src_h = __atomic_load_n(&shm->ports[h->port].height, __ATOMIC_RELAXED);
		}
		convert_yuy2_frame(src_yuy2, src_w, src_h, buffer, n,
				   h->width, h->height, h->pixfmt);
		pthread_mutex_unlock(&g_v4l2_lock);
		return (ssize_t)sz;
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	if (real_v4l2_read)
		return real_v4l2_read(fd, buffer, n);
	return read(fd, buffer, n);
}

void *v4l2_mmap(void *start, size_t length, int prot, int flags, int fd, int64_t offset)
{
	init_real_v4l2_funcs();
	pthread_mutex_lock(&g_v4l2_lock);
	struct gc570d_v4l2_handle *h = find_handle(fd);
	if (h && (uint64_t)offset >= GC570D_V4L2_MMAP_BASE) {
		unsigned int idx = ((uint64_t)offset - GC570D_V4L2_MMAP_BASE) / GC570D_V4L2_MMAP_STRIDE;
		if (idx < h->req_count && h->bufs[idx].addr) {
			void *p = h->bufs[idx].addr;
			pthread_mutex_unlock(&g_v4l2_lock);
			return p;
		}
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	if (real_v4l2_mmap)
		return real_v4l2_mmap(start, length, prot, flags, fd, offset);
	return real_mmap(start, length, prot, flags, fd, (off_t)offset);
}

int v4l2_munmap(void *_start, size_t length)
{
	init_real_v4l2_funcs();
	pthread_mutex_lock(&g_v4l2_lock);
	for (int i = 0; i < GC570D_V4L2_MAX_FDS; i++) {
		if (!g_handles[i].in_use)
			continue;
		for (int j = 0; j < GC570D_V4L2_MAX_BUFS; j++) {
			if (g_handles[i].bufs[j].addr == _start) {
				pthread_mutex_unlock(&g_v4l2_lock);
				return 0;
			}
		}
	}
	pthread_mutex_unlock(&g_v4l2_lock);
	if (real_v4l2_munmap)
		return real_v4l2_munmap(_start, length);
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
int __fxstatat(int ver, int dirfd, const char *pathname, struct stat *buf, int flags)
{
	(void)ver;
	return fstatat(dirfd, pathname, buf, flags);
}
int __fxstatat64(int ver, int dirfd, const char *pathname, struct stat64 *buf, int flags)
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

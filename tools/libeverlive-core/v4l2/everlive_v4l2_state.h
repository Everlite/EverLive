/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_v4l2_state.h — Reusable V4L2 virtual-camera state machine
 *
 * This module owns the virtual file descriptors, V4L2 ioctl semantics, buffer
 * allocation, mmap offset resolution and frame conversion.  It does not
 * define or call any interposed libc/libv4l2 symbol; the per-card preload DSO
 * supplies the libc wrappers and serialises calls with its own lock.
 *
 * All functions except the init/shutdown pair assume the caller serialises
 * access (the preload DSO holds its global mutex).  Internal buffer mappings
 * and timerfd closes use raw syscalls so they remain safe while that mutex is
 * held.
 */

#ifndef EVERLIVE_V4L2_STATE_H
#define EVERLIVE_V4L2_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "everlive_v4l2_card.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVERLIVE_V4L2_MAX_FDS	64U
#define EVERLIVE_V4L2_MAX_BUFS	8U

struct everlive_v4l2_buffer {
	void *addr;
	size_t size;
	bool queued;
	bool done;
};

struct everlive_v4l2_handle {
	bool in_use;
	int fd;
	unsigned int port;
	uint32_t width;
	uint32_t height;
	uint32_t pixfmt;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t req_count;
	bool streaming;
	uint32_t sequence;
	struct everlive_v4l2_buffer bufs[EVERLIVE_V4L2_MAX_BUFS];
};

/* Reset the state and bind the card descriptor.  Safe to call twice. */
int everlive_v4l2_state_init(const struct everlive_v4l2_card *card);

/* Close virtual handles and release the cached frame-store mapping. */
void everlive_v4l2_state_shutdown(void);

struct everlive_v4l2_handle *everlive_v4l2_find_handle(int fd);
bool everlive_v4l2_is_virtual_fd(int fd);
int everlive_v4l2_open_count(void);

/* Open/close/dup a virtual handle.  Return 0/-1 (open/dup return fd). */
int everlive_v4l2_open_virtual(unsigned int port);
int everlive_v4l2_close_virtual(int fd);
int everlive_v4l2_dup_virtual(int oldfd, int newfd);

int everlive_v4l2_ioctl(struct everlive_v4l2_handle *h,
			unsigned long request, void *arg);

/* mmap offset resolution.  NULL means "not a virtual buffer". */
void *everlive_v4l2_mmap_buffer(int fd, uint64_t offset);

/* True when @addr belongs to a virtual buffer. */
bool everlive_v4l2_munmap_buffer(void *addr);

ssize_t everlive_v4l2_read_frame(int fd, void *buffer, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_V4L2_STATE_H */

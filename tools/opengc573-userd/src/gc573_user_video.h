/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_video.h — Userspace VFIO video DMA ring for the GC573
 *
 * Allocates page-aligned userspace frame buffers, maps them into the VFIO
 * IOMMU and drives the FPGA video DMA channels with one-descriptor frames.
 */

#ifndef GC573_USER_VIDEO_H
#define GC573_USER_VIDEO_H

#include "gc573_vfio.h"
#include "gc573_user_fpga.h"

#define GC573_USER_VIDEO_MAX_BUFFERS	8
#define GC573_USER_VIDEO_MAX_CHANNELS	4

#define GC573_USER_VIDEO_DESC_IOVA	0x10000000ULL
#define GC573_USER_VIDEO_FRAME_IOVA	0x20000000ULL
/* One descriptor page per frame buffer, also pinned against RLIMIT_MEMLOCK. */
#define GC573_USER_VIDEO_DESC_TABLE_SIZE	(32U * 1024U)

struct gc573_user_dma_desc {
	uint32_t address_low;
	uint32_t address_high;
	uint32_t length_dwords;
	uint32_t control;
} __attribute__((packed));

struct gc573_user_video_buffer {
	void *vaddr;
	size_t map_size;
	uint64_t iova;
	uint64_t desc_iova;
	struct gc573_user_dma_desc *desc;
	int channel;
	bool mapped;
	bool desc_mapped;
};

struct gc573_user_video {
	struct gc573_vfio_ctx *vfio;
	void *bar0;

	unsigned int width;
	unsigned int height;
	unsigned int frame_bytes;
	unsigned int num_buffers;
	unsigned int num_channels;

	struct gc573_user_video_buffer buffers[GC573_USER_VIDEO_MAX_BUFFERS];
	int channel_buf[GC573_USER_VIDEO_MAX_CHANNELS];

	int free_stack[GC573_USER_VIDEO_MAX_BUFFERS];
	unsigned int free_count;

	int ready_queue[GC573_USER_VIDEO_MAX_BUFFERS];
	unsigned int ready_head;
	unsigned int ready_tail;
	unsigned int ready_count;

	bool streaming;
	uint64_t frames;
	uint64_t last_complete_ms;
	/* Monotonic completion time of the frame currently in each buffer. */
	uint64_t frame_ready_us[GC573_USER_VIDEO_MAX_BUFFERS];
};

/**
 * gc573_user_video_init - Allocate and map frame buffers and descriptor pages
 * @video: Video context
 * @vfio: Initialized VFIO context
 * @bar0: Mapped BAR0
 * @width: Output frame width in pixels (must be even)
 * @height: Output frame height in pixels
 * @num_buffers: Number of frame buffers (1..GC573_USER_VIDEO_MAX_BUFFERS)
 *
 * The buffers are packed YUYV (2 bytes/pixel), matching the VIP output.
 */
int gc573_user_video_init(struct gc573_user_video *video,
			  struct gc573_vfio_ctx *vfio, void *bar0,
			  unsigned int width, unsigned int height,
			  unsigned int num_buffers);

/**
 * gc573_user_video_start - Arm the DMA channels and enable VIP output
 *
 * The VIP must already be configured with gc573_user_fpga_configure().
 */
int gc573_user_video_start(struct gc573_user_video *video);

/**
 * gc573_user_video_poll - Wait for a completed frame
 * @video: Video context
 * @timeout_ms: Maximum wait in milliseconds
 *
 * Returns a buffer index (>= 0), -1 when no frame arrived before the
 * timeout, or -2 when the DMA engine appears stalled.
 */
int gc573_user_video_poll(struct gc573_user_video *video, int timeout_ms);

/**
 * gc573_user_video_release - Return a displayed buffer to the DMA pool
 */
void gc573_user_video_release(struct gc573_user_video *video, int index);

/**
 * gc573_user_video_frame_time_us - Completion timestamp of a ready frame
 * @video: Video context
 * @index: Buffer index returned by gc573_user_video_poll()
 *
 * Returns the CLOCK_MONOTONIC completion time in microseconds, or 0 when the
 * index is invalid or the buffer has not completed yet.  Consumers use this
 * to measure how stale a frame is without adding a second timestamp source.
 */
uint64_t gc573_user_video_frame_time_us(const struct gc573_user_video *video,
					int index);

void gc573_user_video_stop(struct gc573_user_video *video);

/**
 * gc573_user_video_stop_timeout - Stop DMA with a bounded termination wait
 * @video: Video context
 * @timeout_ms: Maximum time to wait for the TERMINATED status bit
 *
 * The hot-switch path uses a short timeout because the DMA reset that follows
 * a reconfiguration quiesces the engine anyway.  The plain stop keeps the
 * long diagnostic wait for normal teardown.
 */
void gc573_user_video_stop_timeout(struct gc573_user_video *video,
				   unsigned int timeout_ms);

/**
 * gc573_user_video_unmap - Release frame buffers from the IOMMU
 *
 * Separate from gc573_user_video_stop() so diagnostics can distinguish
 * stopping the engine from tearing down its mappings.
 */
void gc573_user_video_unmap(struct gc573_user_video *video);

void gc573_user_video_cleanup(struct gc573_user_video *video);

#endif /* GC573_USER_VIDEO_H */

// SPDX-License-Identifier: GPL-2.0-only
#ifndef GC570D_SHM_H
#define GC570D_SHM_H

#include <stdint.h>
#include <stdbool.h>

#define GC570D_SHM_NAME "/gc570d_v4l2_shm"
#define GC570D_SHM_MAGIC 0x47433537U /* "GC57" */
#define GC570D_SHM_FRAME_BYTES (1920 * 1080 * 2)
#define GC570D_SHM_SLOTS 3

/*
 * Triple-buffered frame slot.  The DMA publisher always writes to the free
 * back buffer, then atomically publishes ready_idx.  A slow consumer therefore
 * never stalls the capture thread and, because it re-checks seq after copying,
 * never observes a torn frame either.
 *
 * Layout note: seq/timestamp/width/height/no_signal/streaming stay at the
 * same offsets as the old single-slot layout; only the frame area moved.
 */
struct gc570d_shm_port {
	uint64_t seq;          /* increments once per published frame */
	uint64_t timestamp_ns;
	uint32_t width;
	uint32_t height;
	uint32_t no_signal;
	uint32_t streaming;

	uint32_t write_idx;    /* producer-private back buffer */
	uint32_t ready_idx;    /* atomically published front buffer */
	uint32_t read_idx;     /* informational: last slot a consumer took */
	uint32_t reserved;

	uint8_t  yuy2[GC570D_SHM_SLOTS][GC570D_SHM_FRAME_BYTES];
};

struct gc570d_shm_region {
	uint32_t magic;
	uint32_t version;
	char pci_bdf[32];
	struct gc570d_shm_port ports[2];

	/* Same liveness beacon as gc573_shm.h; appended to keep offsets stable. */
	uint32_t publisher_pid;
	uint32_t reserved;
	uint64_t heartbeat_ns;
};

#endif /* GC570D_SHM_H */

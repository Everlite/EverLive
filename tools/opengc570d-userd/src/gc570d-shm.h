// SPDX-License-Identifier: GPL-2.0-only
#ifndef GC570D_SHM_H
#define GC570D_SHM_H

#include <stdint.h>
#include <stdbool.h>

#define GC570D_SHM_NAME "/gc570d_v4l2_shm"
#define GC570D_SHM_MAGIC 0x47433537U /* "GC57" */
#define GC570D_SHM_FRAME_BYTES (1920 * 1080 * 2)

struct gc570d_shm_port {
	uint64_t seq;
	uint64_t timestamp_ns;
	uint32_t width;
	uint32_t height;
	uint32_t no_signal;
	uint32_t streaming;
	uint8_t  yuy2[GC570D_SHM_FRAME_BYTES];
};

struct gc570d_shm_region {
	uint32_t magic;
	uint32_t version;
	char pci_bdf[32];
	struct gc570d_shm_port ports[2];
};

#endif /* GC570D_SHM_H */

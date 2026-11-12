/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_audio.h — Userspace VFIO HDMI audio DMA for the GC573
 *
 * Mirrors the ping-pong buffer layout of open_gc573/gc573-dma.c.  The
 * IT6805 feeds L-PCM into the FPGA audio engine, which writes completed
 * 20 ms periods into one of two host buffers.  This module only captures
 * those periods; SDL playback is handled by the streamer.
 */

#ifndef GC573_USER_AUDIO_H
#define GC573_USER_AUDIO_H

#include "gc573_vfio.h"
#include "gc573_user_fpga.h"

#define GC573_USER_AUDIO_BUFFER_COUNT		2U
/* Same capacity as GC555_AUDIO_BUFFER_CAPACITY in the kernel driver. */
#define GC573_USER_AUDIO_BUFFER_CAPACITY	0xc000U
#define GC573_USER_AUDIO_MAP_SIZE \
	(GC573_USER_AUDIO_BUFFER_COUNT * GC573_USER_AUDIO_BUFFER_CAPACITY)
#define GC573_USER_AUDIO_IOVA			0x30000000ULL

struct gc573_user_audio {
	struct gc573_vfio_ctx *vfio;
	void *bar0;

	void *vaddr;
	size_t map_size;
	uint64_t iova;
	bool mapped;

	unsigned int rate_hz;
	unsigned int channels;
	unsigned int dma_channels;
	unsigned int sample_bits;
	unsigned int sample_bytes;
	unsigned int buffer_bytes;
	unsigned int frames_per_block;

	int last_buffer;
	uint64_t blocks;
	bool streaming;
};

/**
 * gc573_user_audio_init - Allocate and map the two audio ping-pong buffers
 * @audio: Audio context
 * @vfio: Initialized VFIO context
 * @bar0: Mapped BAR0
 *
 * The engine is left stopped.  Call gc573_user_audio_start() once the
 * IT6805 reports a valid audio format.
 */
int gc573_user_audio_init(struct gc573_user_audio *audio,
			  struct gc573_vfio_ctx *vfio, void *bar0);

/**
 * gc573_user_audio_start - Program the FPGA audio engine and start capture
 * @audio: Audio context
 * @rate_hz: Sample rate (32000, 44100 or 48000)
 * @channels: HDMI channel count (2, 6 or 8)
 * @sample_bits: Container width (16 or 24)
 *
 * Programs the same registers and ordering as gc555_dma_start_audio():
 * two 20 ms periods, two fixed buffers, map slots 0..3, audio completion
 * IRQ enable, then stream enable.
 */
int gc573_user_audio_start(struct gc573_user_audio *audio,
			   unsigned int rate_hz, unsigned int channels,
			   unsigned int sample_bits);

/**
 * gc573_user_audio_poll - Drain one completed audio period
 * @audio: Audio context
 * @data: Receives a pointer to the completed ping-pong buffer
 * @bytes: Receives the number of valid PCM bytes
 *
 * Returns 1 when a block was completed, 0 when no new block is available,
 * and a negative errno on invalid arguments.  The returned memory is owned
 * by the audio context and remains valid until the next start/cleanup.
 */
int gc573_user_audio_poll(struct gc573_user_audio *audio,
			  const void **data, size_t *bytes);

/**
 * gc573_user_audio_stop - Quiesce the audio engine and reset it
 */
void gc573_user_audio_stop(struct gc573_user_audio *audio);

/**
 * gc573_user_audio_cleanup - Stop the engine and unmap its buffers
 */
void gc573_user_audio_cleanup(struct gc573_user_audio *audio);

#endif /* GC573_USER_AUDIO_H */

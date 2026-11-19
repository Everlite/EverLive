/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hws_capture.h — capture thread for the HWS UHD engine.
 *
 * The thread owns the card: it services interrupts, rotates the zero-copy
 * frame slots, follows the input (signal loss, mode changes) and forwards
 * HDMI audio packets.  Consumers (the presenter and the output thread) take
 * the newest complete frame by reference; a held slot is never handed back
 * to the card until it is released.
 */

#ifndef HWS_CAPTURE_H
#define HWS_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HWS_CAPTURE_MAX_CONSUMERS	2
#define HWS_CAPTURE_DEFAULT_SLOTS	5	/* 2 card-side + newest + 2 held */

struct hws_capture;

struct hws_frame_ref {
	int slot;
	uint64_t seq;
	unsigned int width;
	unsigned int height;
	size_t bytes;
	uint64_t ts_us;		/* CLOCK_MONOTONIC at frame completion */
	const uint8_t *data;
};

/* Called from the capture thread with one 4 KiB S16LE stereo packet. */
typedef void (*hws_audio_cb)(const int16_t *pcm, size_t bytes, void *user);

struct hws_capture_cfg {
	const char *bdf;		/* NULL: first card found */
	unsigned int nslots;
	bool audio;
	hws_audio_cb audio_cb;
	void *user;
};

struct hws_capture_status {
	char bdf[32];
	unsigned int fpga_version;
	char link[32];
	bool signal;			/* usable progressive input, DMA running */
	bool present;			/* card sees a signal (may be unsupported) */
	bool interlaced;
	bool hdcp;
	unsigned int width;
	unsigned int height;
	unsigned int input_fps;		/* card's whole-fps register */
	double fps;			/* measured completed frames per second */
	uint64_t frames;
	uint64_t dropped;		/* lost or torn frames */
	uint64_t audio_packets;
	uint64_t restarts;		/* video engine restarts (mode change, stall) */
	unsigned int nslots;
	bool failed;			/* card stopped answering */
};

int hws_capture_start(struct hws_capture **out, const struct hws_capture_cfg *cfg);
void hws_capture_stop(struct hws_capture *cap);

/*
 * Wait up to @timeout_ms for a frame newer than @after_seq and hold it for
 * @consumer (0 .. HWS_CAPTURE_MAX_CONSUMERS-1).  A consumer holds at most one
 * frame; a previous hold is released first.  Returns 0 or -ETIMEDOUT.
 */
int hws_capture_acquire(struct hws_capture *cap, int consumer, uint64_t after_seq,
			int timeout_ms, struct hws_frame_ref *out);
void hws_capture_release(struct hws_capture *cap, int consumer);

void hws_capture_get_status(struct hws_capture *cap, struct hws_capture_status *st);

#endif /* HWS_CAPTURE_H */

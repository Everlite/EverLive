/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_it6805.h — Userspace IT6805 HDMI 2.0 Receiver Driver for GC573
 */

#ifndef GC573_USER_IT6805_H
#define GC573_USER_IT6805_H

#include "gc573-user-compat.h"
#include <stdbool.h>

/* AVI InfoFrame colour space (IT6805 bank 2, byte 1 bits 6:5). */
enum gc573_user_colorspace {
	GC573_USER_COLORSPACE_RGB = 0,
	GC573_USER_COLORSPACE_YCBCR422 = 1,
	GC573_USER_COLORSPACE_YCBCR444 = 2,
	GC573_USER_COLORSPACE_YCBCR420 = 3,
};

struct gc573_user_video_signal {
	uint32_t width;
	uint32_t height;
	uint32_t pixel_clock_khz;
	uint32_t frame_rate_hz;
	uint32_t frame_rate_x100;
	bool scdt_locked;
	bool five_volt_present;
	bool interlaced;
	bool avi_valid;
	bool is_dvi;
	bool dual_pixel;	/* LVDS output uses dual-pixel mode */
	bool ddr;		/* TTL output uses DDR mode */
	uint8_t vic;
	uint8_t colorspace;	/* enum gc573_user_colorspace */
	uint8_t colorimetry;
	uint8_t extended_colorimetry;
	uint8_t rgb_quantization;
	uint8_t ycc_quantization;
	uint8_t status15;
	uint8_t avi_raw[6];
};

/**
 * gc573_user_it6805_set_bank - Select an IT6805 register bank
 * @bar0: Pointer to mapped FPGA BAR0 MMIO
 * @bank: Bank number (0..7)
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_it6805_set_bank(void *bar0, u8 bank);

/**
 * gc573_user_it6805_init - Perform full IT6805 PLL, EQ, and Port-0 initialization
 * @bar0: Pointer to mapped FPGA BAR0 MMIO
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_it6805_init(void *bar0);

/**
 * gc573_user_it6805_poll_status - Light status poll without timing measurement
 * @bar0: Pointer to mapped FPGA BAR0 MMIO
 * @sig: Pointer to signal struct receiving 5V/SCDT/PHY state
 *
 * Only reads the cheap status registers (bank 0 0x13/0x14/0x15/0x19) and
 * follows the HDMI 2.0 PHY selector.  This is safe to call from a streaming
 * loop: unlike gc573_user_it6805_poll_signal() it does not spend ~175 ms on
 * the TMDS/pixel-clock sampling.
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_it6805_poll_status(void *bar0,
				  struct gc573_user_video_signal *sig);

/**
 * enum gc573_user_signal_probe - Timing measurement depth
 * @GC573_USER_SIGNAL_PROBE_TIMING: status, timing registers and AVI only
 * @GC573_USER_SIGNAL_PROBE_RATE: timing/AVI plus a short pixel-clock sample
 * @GC573_USER_SIGNAL_PROBE_FULL: full TMDS and pixel-clock measurement
 */
enum gc573_user_signal_probe {
	GC573_USER_SIGNAL_PROBE_TIMING = 0,
	GC573_USER_SIGNAL_PROBE_RATE,
	GC573_USER_SIGNAL_PROBE_FULL,
};

/**
 * gc573_user_it6805_poll_signal_ex - Read the signal with a chosen depth
 * @bar0: Pointer to mapped FPGA BAR0 MMIO
 * @sig: Pointer to signal struct to receive status
 * @probe: Measurement depth
 *
 * TIMING is cheap enough for the streaming hot path: it reads the latched
 * timing registers and AVI InfoFrame but does not sample the pixel clock,
 * so frame_rate_hz/frame_rate_x100 are left zero.  RATE adds a short
 * pixel-clock sample and fills the frame rate.  FULL matches
 * gc573_user_it6805_poll_signal() and blocks for roughly 175 ms.
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_it6805_poll_signal_ex(void *bar0,
				     struct gc573_user_video_signal *sig,
				     enum gc573_user_signal_probe probe);

/**
 * gc573_user_it6805_poll_signal - Read SCDT, timing, and AVI InfoFrame
 * @bar0: Pointer to mapped FPGA BAR0 MMIO
 * @sig: Pointer to signal struct to receive status
 *
 * Performs the full timing measurement and AVI InfoFrame read.  This blocks
 * for roughly 175 ms because the IT6805 timing sampler needs that long.
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_it6805_poll_signal(void *bar0, struct gc573_user_video_signal *sig);

/**
 * gc573_user_it6805_apply_signal_output - Configure and enable the video path
 * @bar0: Pointer to mapped FPGA BAR0 MMIO
 * @sig: Signal description from gc573_user_it6805_poll_signal()
 *
 * Ports the kernel's configure/enable output path: TTL and LVDS path setup,
 * high-bandwidth DDR/dual-pixel selection, colour space, pixel-clock gate,
 * colour depth and the final video-tristate release.  On success the signal
 * struct is updated with the chosen @dual_pixel/@ddr/@is_dvi state.
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_it6805_apply_signal_output(void *bar0,
					  struct gc573_user_video_signal *sig);

/**
 * gc573_user_it6805_unmute - Unmute video output so pixel data flows into FPGA
 * @bar0: Pointer to mapped FPGA BAR0 MMIO
 *
 * Returns 0 on success, -EAGAIN when the audio clock is not ready yet.
 */
int gc573_user_it6805_unmute(void *bar0);

/**
 * struct gc573_user_audio_format - Snapshot of the received HDMI audio stream
 */
struct gc573_user_audio_format {
	uint32_t rate_hz;
	uint8_t channels;
	uint8_t sample_bits;	/* 16 or 24, derived from channel status */
	uint8_t channel_allocation;
	uint8_t transport;	/* 0 = L-PCM samples */
	bool valid;
};

/**
 * gc573_user_it6805_audio_poll - Advance the HDMI audio state machine
 * @bar0: Pointer to mapped FPGA BAR0 MMIO
 * @format: Receives the current audio format snapshot
 *
 * Ports the audio request/ready/start/monitor states from
 * it6805_service_audio_locked().  Call this about every 100 ms while
 * streaming; it performs the I2C traffic required by the IT6805 audio
 * path and is independent of the FPGA audio DMA.
 *
 * Returns 0 on success (check @format->valid), negative errno on I2C
 * failure.  The video path must keep running when this fails.
 */
int gc573_user_it6805_audio_poll(void *bar0,
				 struct gc573_user_audio_format *format);

/**
 * gc573_user_it6805_audio_disable - Mute and release the HDMI audio output
 * @bar0: Pointer to mapped FPGA BAR0 MMIO
 *
 * Mirrors it6805_clear_audio_output_locked(): drops the rate-mismatch
 * override and tristates the audio output pins.  Safe to call when audio
 * was never enabled.
 */
int gc573_user_it6805_audio_disable(void *bar0);

#endif /* GC573_USER_IT6805_H */

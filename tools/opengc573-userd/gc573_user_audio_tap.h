/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_audio_tap.h — PipeWire capture tap for the EverLive streamer
 *
 * Mirrors the mixed HDMI audio (L-PCM after gain, or the decoded AC-3/DTS
 * stereo downmix) into a dedicated PipeWire node so OBS/Discord/FFmpeg can
 * record it as its own track, independent of the desktop mix.
 *
 * The tap publishes S16_LE at the HDMI channel count (2, 6 or 8) so OBS and
 * FFmpeg can record real 5.1/7.1 instead of a stereo fold-down. 16-bit
 * carriers are forwarded as-is; S24_3LE and left-aligned S32LSB carriers are
 * re-quantised to S16 without touching the channel order.
 */

#ifndef GC573_USER_AUDIO_TAP_H
#define GC573_USER_AUDIO_TAP_H

#include <stdbool.h>
#include <stddef.h>

#define GC573_AUDIO_TAP_DEFAULT	"everlive-capture-audio"
#define GC573_AUDIO_TAP_MAX_CHANNELS 8U

/**
 * gc573_audio_tap_start - Create the PipeWire client and locate the sink
 * @node_name: Sink node name to publish into (NULL/"" for the default)
 *
 * The sink is normally created by a config drop-in installed by install.sh.
 * If it does not exist, the adapter module is loaded at runtime as a fallback.
 * The audio stream itself is only connected once the HDMI sample rate is known
 * (see gc573_audio_tap_set_format()), so a 44.1 kHz source never plays at the
 * wrong pitch.
 *
 * Returns 0 on success, a negative errno otherwise.  Safe to call twice.
 */
int gc573_audio_tap_start(const char *node_name);

/**
 * gc573_audio_tap_set_format - (Re)connect the stream for rate + channels
 * @rate_hz: HDMI sample rate (32/44.1/48 kHz)
 * @channels: 2, 6 or 8 interleaved channels
 *
 * No-op when the tap is disabled or the format did not change.
 */
void gc573_audio_tap_set_format(unsigned int rate_hz, unsigned int channels);

/**
 * gc573_audio_tap_write - Queue PCM for the tap
 * @data: Interleaved PCM block (S16LE, S24_3LE or left-aligned S32LSB)
 * @bytes: Size of the block
 * @sample_bytes: 2, 3 or 4 bytes per sample
 * @source_channels: Interleaved stride of the input (for 5.1 this is the
 *                   FPGA's 8-slot layout while the source carries 6 channels)
 * @output_channels: 2, 6 or 8 channels to publish
 *
 * Lock-free: called from the audio worker, read from the PipeWire RT thread.
 */
void gc573_audio_tap_write(const void *data, size_t bytes,
			   unsigned int sample_bytes,
			   unsigned int source_channels,
			   unsigned int output_channels);

/**
 * gc573_audio_tap_stop - Tear the PipeWire client down
 */
void gc573_audio_tap_stop(void);

/**
 * gc573_audio_tap_active - True when the tap is running
 */
bool gc573_audio_tap_active(void);

#endif /* GC573_USER_AUDIO_TAP_H */

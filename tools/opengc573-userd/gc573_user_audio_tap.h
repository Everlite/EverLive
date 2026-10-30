/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_audio_tap.h — PipeWire capture tap for the EverLive streamer
 *
 * Mirrors the mixed HDMI audio (L-PCM after gain, or the decoded AC-3/DTS
 * stereo downmix) into a dedicated PipeWire node so OBS/Discord/FFmpeg can
 * record it as its own track, independent of the desktop mix.
 *
 * The tap always publishes S16_LE stereo.  16-bit carriers are forwarded
 * as-is; 24-bit carriers reach this layer as left-aligned S32LSB (the
 * streamer already widened them for SDL) and are re-quantised to S16.
 */

#ifndef GC573_USER_AUDIO_TAP_H
#define GC573_USER_AUDIO_TAP_H

#include <stdbool.h>
#include <stddef.h>

#define GC573_AUDIO_TAP_DEFAULT	"everlive-capture-audio"

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
 * gc573_audio_tap_set_format - (Re)connect the stream for a sample rate
 * @rate_hz: HDMI sample rate (32/44.1/48 kHz)
 *
 * No-op when the tap is disabled or the rate did not change.
 */
void gc573_audio_tap_set_format(unsigned int rate_hz);

/**
 * gc573_audio_tap_write - Queue PCM for the tap
 * @data: Stereo PCM block (S16LE or left-aligned S32LSB)
 * @bytes: Size of the block
 * @sample_bits: 16 or 24, matching the streamer's playback format
 *
 * Lock-free: called from the audio worker, read from the PipeWire RT thread.
 */
void gc573_audio_tap_write(const void *data, size_t bytes,
			   unsigned int sample_bits);

/**
 * gc573_audio_tap_stop - Tear the PipeWire client down
 */
void gc573_audio_tap_stop(void);

/**
 * gc573_audio_tap_active - True when the tap is running
 */
bool gc573_audio_tap_active(void);

#endif /* GC573_USER_AUDIO_TAP_H */

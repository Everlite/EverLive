/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_bitstream.h — IEC 61937 (S/PDIF) AC-3/DTS decoder
 *
 * The GC573 audio DMA delivers the raw HDMI audio subframes.  For compressed
 * sources these are IEC 61937 bursts (Pa/Pb preamble, Pc burst info, Pd
 * payload length) carrying AC-3 or DTS frames.  This module extracts the
 * bursts, decodes them with libavcodec and resamples the result to stereo
 * S16_LE with libswresample.
 */

#ifndef GC573_USER_BITSTREAM_H
#define GC573_USER_BITSTREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

enum gc573_bitstream_codec {
	GC573_BITSTREAM_CODEC_NONE = 0,
	GC573_BITSTREAM_CODEC_AC3,
	GC573_BITSTREAM_CODEC_DTS,
};

/* How the 16-bit IEC word sits inside a 24-bit HDMI subframe. */
enum gc573_bitstream_word_align {
	GC573_BITSTREAM_WORD_MSB,   /* S24 sample = word << 8 (byte 0 padding) */
	GC573_BITSTREAM_WORD_LSB,   /* S24 sample = word        (byte 2 padding) */
};

/* Sanity limits for the IEC 61937 reassembly buffer. */
#define GC573_BITSTREAM_MAX_PENDING_BYTES	(256U * 1024U)
#define GC573_BITSTREAM_MAX_BURST_BYTES		32768U

struct gc573_bitstream_decoder {
	enum gc573_bitstream_codec codec;
	AVCodecContext *codec_ctx;
	AVFrame *frame;
	AVPacket *pkt;
	SwrContext *swr_ctx;

	int out_rate_hz;
	int out_channels;
	int in_rate_hz;
	int in_format;
	AVChannelLayout in_layout;

	uint8_t *pending;
	size_t pending_len;
	size_t pending_cap;

	uint64_t bursts;
	uint64_t frames_decoded;
	uint64_t decode_errors;
};

/**
 * gc573_bitstream_decoder_init - Open the AC-3 or DTS decoder
 * @dec: Decoder context (zero-initialised by the caller)
 * @codec: GC573_BITSTREAM_CODEC_AC3 or GC573_BITSTREAM_CODEC_DTS
 * @out_rate_hz: Output sample rate (typically 48000)
 * @out_channels: Output channel count (2 for stereo downmix)
 *
 * Returns 0 on success, -ENOSYS when libavcodec has no such decoder, or
 * another negative errno on allocation/open failure.
 */
int gc573_bitstream_decoder_init(struct gc573_bitstream_decoder *dec,
				 enum gc573_bitstream_codec codec,
				 int out_rate_hz, int out_channels);

/**
 * gc573_bitstream_decoder_close - Release all decoder resources
 */
void gc573_bitstream_decoder_close(struct gc573_bitstream_decoder *dec);

/**
 * gc573_bitstream_unpack_iec_words - Compact 16-bit IEC words from HDMI samples
 * @src: Raw audio DMA block
 * @bytes: Number of bytes in @src
 * @frame_bytes: Bytes per multichannel frame (channels * sample_bytes)
 * @sample_bytes: Container width (2 for S16, 3 for S24_3LE)
 * @align: MSB/LSB alignment of the 16-bit word inside a 24-bit sample
 * @dst: Output buffer receiving 4 bytes per stereo frame (L word, R word)
 * @dst_size: Capacity of @dst
 *
 * HDMI carries IEC 61937 in the first two channels.  In S24_3LE the 16-bit
 * word is normally MSB-aligned (00 72 F8 / 00 1F 4E), which would otherwise
 * hide the Pa/Pb pair from a byte scan.  This helper extracts the two words
 * per frame into a compact stream where Pa/Pb are adjacent.
 *
 * Returns the number of bytes written to @dst.
 */
size_t gc573_bitstream_unpack_iec_words(const uint8_t *src, size_t bytes,
					unsigned int frame_bytes,
					unsigned int sample_bytes,
					enum gc573_bitstream_word_align align,
					uint8_t *dst, size_t dst_size);

/**
 * gc573_bitstream_has_preamble - Check for a Pa/Pb sync pair
 */
bool gc573_bitstream_has_preamble(const uint8_t *data, size_t bytes);

/**
 * gc573_bitstream_decoder_feed - Feed raw IEC 61937 bytes and emit PCM
 * @dec: Decoder context
 * @data: Raw audio DMA block (IEC 61937 byte stream)
 * @bytes: Number of bytes in @data
 * @out_buf: Output buffer for interleaved S16_LE PCM
 * @out_bytes: In: capacity of @out_buf.  Out: bytes of PCM produced.
 *
 * Bursts may span DMA blocks; incomplete data is retained internally until
 * the next call.  Returns 0 on success (even when no complete burst was
 * available yet), or a negative errno on a hard allocation/decoder failure.
 * Individual corrupt bursts are counted in decode_errors and skipped.
 */
int gc573_bitstream_decoder_feed(struct gc573_bitstream_decoder *dec,
				 const uint8_t *data, size_t bytes,
				 void *out_buf, size_t *out_bytes);

#endif /* GC573_USER_BITSTREAM_H */

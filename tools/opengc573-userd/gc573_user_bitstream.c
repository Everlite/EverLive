/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_bitstream.c — IEC 61937 AC-3/DTS decode and stereo downmix
 *
 * IEC 61937 burst layout (16-bit little-endian words):
 *   Pa = 0xF872, Pb = 0x4E1F,
 *   Pc bits 4:0 = data type (0x01 AC-3, 0x0b DTS),
 *   Pd = payload length in bits,
 *   payload follows the 8-byte preamble and is 16-bit-word byte-swapped.
 *
 * A burst can span two audio DMA blocks (20 ms each), so incomplete data is
 * kept in a small reassembly buffer.
 */

#include "gc573_user_bitstream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/mathematics.h>

#define IEC61937_PA_0		0x72
#define IEC61937_PA_1		0xf8
#define IEC61937_PB_0		0x1f
#define IEC61937_PB_1		0x4e
#define IEC61937_HEADER_BYTES	8U
#define IEC61937_TYPE_AC3	0x01
#define IEC61937_TYPE_DTS	0x0b

static int bitstream_reserve(struct gc573_bitstream_decoder *dec, size_t extra)
{
	size_t need = dec->pending_len + extra;
	size_t cap;
	uint8_t *buf;

	if (need > GC573_BITSTREAM_MAX_PENDING_BYTES)
		return -EOVERFLOW;
	if (need <= dec->pending_cap)
		return 0;

	cap = dec->pending_cap ? dec->pending_cap : 16384U;
	while (cap < need)
		cap <<= 1;
	if (cap > GC573_BITSTREAM_MAX_PENDING_BYTES)
		cap = GC573_BITSTREAM_MAX_PENDING_BYTES;

	buf = realloc(dec->pending, cap);
	if (!buf)
		return -ENOMEM;

	dec->pending = buf;
	dec->pending_cap = cap;
	return 0;
}

static void bitstream_consume(struct gc573_bitstream_decoder *dec, size_t bytes)
{
	if (bytes >= dec->pending_len) {
		dec->pending_len = 0;
		return;
	}
	memmove(dec->pending, dec->pending + bytes, dec->pending_len - bytes);
	dec->pending_len -= bytes;
}

static size_t bitstream_find_preamble(const uint8_t *data, size_t bytes)
{
	for (size_t i = 0; i + IEC61937_HEADER_BYTES <= bytes; i++) {
		if (data[i] == IEC61937_PA_0 && data[i + 1] == IEC61937_PA_1 &&
		    data[i + 2] == IEC61937_PB_0 && data[i + 3] == IEC61937_PB_1)
			return i;
	}
	return SIZE_MAX;
}

static uint8_t bitstream_data_type(uint16_t pc)
{
	uint8_t type = (uint8_t)(pc & 0x1f);

	/* Some implementations put the type in Pc[12:8] instead. */
	if (type != IEC61937_TYPE_AC3 && type != IEC61937_TYPE_DTS) {
		uint8_t alt = (uint8_t)((pc >> 8) & 0x1f);

		if (alt == IEC61937_TYPE_AC3 || alt == IEC61937_TYPE_DTS)
			type = alt;
	}

	return type;
}

size_t gc573_bitstream_unpack_iec_words(const uint8_t *src, size_t bytes,
					unsigned int frame_bytes,
					unsigned int sample_bytes,
					enum gc573_bitstream_word_align align,
					uint8_t *dst, size_t dst_size)
{
	size_t frames;

	if (!src || !dst || !frame_bytes || sample_bytes < 2 ||
	    frame_bytes < 2U * sample_bytes)
		return 0;

	frames = bytes / frame_bytes;
	if (frames > dst_size / 4U)
		frames = dst_size / 4U;

	for (size_t i = 0; i < frames; i++) {
		const uint8_t *p = src + i * frame_bytes;
		uint16_t left;
		uint16_t right;

		if (sample_bytes == 2) {
			/* S16: the two channel words are already adjacent. */
			left = (uint16_t)(p[0] | (p[1] << 8));
			right = (uint16_t)(p[2] | (p[3] << 8));
		} else if (align == GC573_BITSTREAM_WORD_LSB) {
			left = (uint16_t)(p[0] | (p[1] << 8));
			right = (uint16_t)(p[sample_bytes] |
					   (p[sample_bytes + 1] << 8));
		} else {
			/* S24_3LE, MSB-aligned: upper 16 bits of each sample. */
			left = (uint16_t)(p[1] | (p[2] << 8));
			right = (uint16_t)(p[sample_bytes + 1] |
					   (p[sample_bytes + 2] << 8));
		}

		dst[4 * i + 0] = (uint8_t)(left & 0xff);
		dst[4 * i + 1] = (uint8_t)(left >> 8);
		dst[4 * i + 2] = (uint8_t)(right & 0xff);
		dst[4 * i + 3] = (uint8_t)(right >> 8);
	}

	return frames * 4U;
}

bool gc573_bitstream_has_preamble(const uint8_t *data, size_t bytes)
{
	if (!data)
		return false;

	for (size_t i = 0; i + 4 <= bytes; i++) {
		if (data[i] == IEC61937_PA_0 && data[i + 1] == IEC61937_PA_1 &&
		    data[i + 2] == IEC61937_PB_0 && data[i + 3] == IEC61937_PB_1)
			return true;
	}

	return false;
}

/*
 * The payload is carried LSB-first in 16-bit S/PDIF words; libavcodec wants
 * the codec's normal MSB-first elementary stream.  Swap every word.
 */
static void bitstream_swap_words(uint8_t *dst, const uint8_t *src, size_t bytes)
{
	size_t i;

	for (i = 0; i + 1 < bytes; i += 2) {
		dst[i] = src[i + 1];
		dst[i + 1] = src[i];
	}
	if (i < bytes)
		dst[i] = src[i];
}

static int bitstream_ensure_swr(struct gc573_bitstream_decoder *dec,
				const AVFrame *frame)
{
	AVChannelLayout frame_layout;
	AVChannelLayout out_layout;
	bool owned_layout = false;
	int ret;

	if (frame->ch_layout.nb_channels > 0) {
		frame_layout = frame->ch_layout;
	} else {
		av_channel_layout_default(&frame_layout, 2);
		owned_layout = true;
	}

	if (dec->swr_ctx && dec->in_rate_hz == frame->sample_rate &&
	    dec->in_format == frame->format &&
	    dec->in_layout.nb_channels > 0 &&
	    av_channel_layout_compare(&dec->in_layout, &frame_layout) == 0) {
		if (owned_layout)
			av_channel_layout_uninit(&frame_layout);
		return 0;
	}

	swr_free(&dec->swr_ctx);
	av_channel_layout_uninit(&dec->in_layout);

	av_channel_layout_default(&out_layout, dec->out_channels);
	ret = swr_alloc_set_opts2(&dec->swr_ctx, &out_layout, AV_SAMPLE_FMT_S16,
				  dec->out_rate_hz, &frame_layout,
				  (enum AVSampleFormat)frame->format,
				  frame->sample_rate, 0, NULL);
	av_channel_layout_uninit(&out_layout);
	if (owned_layout)
		av_channel_layout_uninit(&frame_layout);
	if (ret < 0)
		return ret;

	ret = swr_init(dec->swr_ctx);
	if (ret < 0) {
		swr_free(&dec->swr_ctx);
		return ret;
	}

	ret = av_channel_layout_copy(&dec->in_layout, &frame->ch_layout);
	if (ret < 0)
		return ret;

	dec->in_rate_hz = frame->sample_rate;
	dec->in_format = frame->format;
	return 0;
}

static int bitstream_convert_frame(struct gc573_bitstream_decoder *dec,
				   const AVFrame *frame, uint8_t *out,
				   size_t cap, size_t *written)
{
	const uint8_t *const *in;
	uint8_t *dst;
	size_t available;
	size_t needed;
	int out_samples;
	int got;
	int ret;

	if (frame->nb_samples <= 0 || frame->sample_rate <= 0)
		return 0;

	ret = bitstream_ensure_swr(dec, frame);
	if (ret < 0)
		return ret;

	out_samples = (int)av_rescale_rnd(
		swr_get_delay(dec->swr_ctx, frame->sample_rate) +
			frame->nb_samples,
		dec->out_rate_hz, frame->sample_rate, AV_ROUND_UP);
	if (out_samples <= 0)
		return 0;

	available = cap - *written;
	needed = (size_t)out_samples * (size_t)dec->out_channels *
		 sizeof(int16_t);
	if (needed > available) {
		out_samples = (int)(available /
				    ((size_t)dec->out_channels *
				     sizeof(int16_t)));
		if (out_samples <= 0)
			return -ENOSPC;
	}

	dst = out + *written;
	in = (const uint8_t *const *)frame->extended_data;
	got = swr_convert(dec->swr_ctx, &dst, out_samples, in,
			  frame->nb_samples);
	if (got < 0)
		return got;

	*written += (size_t)got * (size_t)dec->out_channels * sizeof(int16_t);
	dec->frames_decoded++;
	return 0;
}

static int bitstream_drain_frames(struct gc573_bitstream_decoder *dec,
				  uint8_t *out, size_t cap, size_t *written)
{
	int ret;

	for (;;) {
		ret = avcodec_receive_frame(dec->codec_ctx, dec->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		if (ret < 0)
			return ret;

		ret = bitstream_convert_frame(dec, dec->frame, out, cap,
					      written);
		av_frame_unref(dec->frame);
		if (ret < 0)
			return ret;
	}
}

static int bitstream_decode_packet(struct gc573_bitstream_decoder *dec,
				   const uint8_t *data, size_t bytes,
				   uint8_t *out, size_t cap, size_t *written)
{
	int ret;

	av_packet_unref(dec->pkt);
	ret = av_new_packet(dec->pkt, (int)bytes);
	if (ret < 0)
		return ret;
	bitstream_swap_words(dec->pkt->data, data, bytes);

	ret = avcodec_send_packet(dec->codec_ctx, dec->pkt);
	if (ret == AVERROR(EAGAIN)) {
		ret = bitstream_drain_frames(dec, out, cap, written);
		if (ret < 0) {
			av_packet_unref(dec->pkt);
			return ret;
		}
		ret = avcodec_send_packet(dec->codec_ctx, dec->pkt);
	}
	av_packet_unref(dec->pkt);
	if (ret < 0 && ret != AVERROR_EOF)
		return ret;

	return bitstream_drain_frames(dec, out, cap, written);
}

int gc573_bitstream_decoder_init(struct gc573_bitstream_decoder *dec,
				 enum gc573_bitstream_codec codec,
				 int out_rate_hz, int out_channels)
{
	enum AVCodecID codec_id;
	const AVCodec *impl;

	if (!dec || out_rate_hz <= 0 || out_channels <= 0)
		return -EINVAL;

	memset(dec, 0, sizeof(*dec));
	dec->codec = codec;
	dec->out_rate_hz = out_rate_hz;
	dec->out_channels = out_channels;

	switch (codec) {
	case GC573_BITSTREAM_CODEC_AC3:
		codec_id = AV_CODEC_ID_AC3;
		break;
	case GC573_BITSTREAM_CODEC_DTS:
		codec_id = AV_CODEC_ID_DTS;
		break;
	default:
		return -EINVAL;
	}

	impl = avcodec_find_decoder(codec_id);
	if (!impl)
		return -ENOSYS;

	dec->codec_ctx = avcodec_alloc_context3(impl);
	if (!dec->codec_ctx)
		goto nomem;

	if (avcodec_open2(dec->codec_ctx, impl, NULL) < 0)
		goto fail;

	dec->frame = av_frame_alloc();
	dec->pkt = av_packet_alloc();
	if (!dec->frame || !dec->pkt)
		goto nomem;

	return 0;

nomem:
	gc573_bitstream_decoder_close(dec);
	return -ENOMEM;

fail:
	gc573_bitstream_decoder_close(dec);
	return -EIO;
}

void gc573_bitstream_decoder_close(struct gc573_bitstream_decoder *dec)
{
	if (!dec)
		return;

	swr_free(&dec->swr_ctx);
	av_frame_free(&dec->frame);
	av_packet_free(&dec->pkt);
	avcodec_free_context(&dec->codec_ctx);
	av_channel_layout_uninit(&dec->in_layout);

	free(dec->pending);
	dec->pending = NULL;
	dec->pending_len = 0;
	dec->pending_cap = 0;
	dec->in_rate_hz = 0;
	dec->in_format = 0;
}

int gc573_bitstream_decoder_feed(struct gc573_bitstream_decoder *dec,
				 const uint8_t *data, size_t bytes,
				 void *out_buf, size_t *out_bytes)
{
	size_t cap;
	size_t written = 0;
	int ret;

	if (!dec || !dec->codec_ctx || !data || !out_buf || !out_bytes)
		return -EINVAL;

	cap = *out_bytes;
	*out_bytes = 0;
	if (!cap)
		return -EINVAL;

	ret = bitstream_reserve(dec, bytes);
	if (ret < 0) {
		dec->pending_len = 0;
		return ret;
	}
	memcpy(dec->pending + dec->pending_len, data, bytes);
	dec->pending_len += bytes;

	for (;;) {
		size_t off;
		size_t payload;
		size_t need;
		size_t produced = 0;
		uint16_t pc;
		uint8_t type;

		off = bitstream_find_preamble(dec->pending, dec->pending_len);
		if (off == SIZE_MAX) {
			/* Keep a possible partial Pa/Pb header for the next block. */
			if (dec->pending_len > IEC61937_HEADER_BYTES - 1)
				bitstream_consume(dec, dec->pending_len -
						     (IEC61937_HEADER_BYTES - 1));
			break;
		}
		if (off)
			bitstream_consume(dec, off);
		if (dec->pending_len < IEC61937_HEADER_BYTES)
			break;

		pc = (uint16_t)(dec->pending[4] |
				(dec->pending[5] << 8));
		/* Pd is the payload length in bits (IEC 61937-1). */
		payload = (size_t)(((dec->pending[6] |
				    (dec->pending[7] << 8)) + 7U) / 8U);
		type = bitstream_data_type(pc);

		if (payload > GC573_BITSTREAM_MAX_BURST_BYTES) {
			/* Desynchronised length: resync one byte later. */
			bitstream_consume(dec, 1);
			continue;
		}

		need = IEC61937_HEADER_BYTES + payload;
		if (dec->pending_len < need)
			break;	/* wait for the rest of the burst */

		if (type == 0x00 || payload == 0) {
			/* NULL/padding burst, or nothing to decode. */
			bitstream_consume(dec, need);
			continue;
		}

		if ((type == IEC61937_TYPE_AC3 &&
		     dec->codec != GC573_BITSTREAM_CODEC_AC3) ||
		    (type == IEC61937_TYPE_DTS &&
		     dec->codec != GC573_BITSTREAM_CODEC_DTS) ||
		    (type != IEC61937_TYPE_AC3 &&
		     type != IEC61937_TYPE_DTS)) {
			bitstream_consume(dec, need);
			continue;
		}

		dec->bursts++;
		ret = bitstream_decode_packet(dec,
					      dec->pending +
						      IEC61937_HEADER_BYTES,
					      payload,
					      (uint8_t *)out_buf + written,
					      cap - written, &produced);
		bitstream_consume(dec, need);
		if (ret < 0) {
			dec->decode_errors++;
			continue;
		}

		written += produced;
		if (written >= cap)
			break;	/* output full; keep the rest for next call */
	}

	*out_bytes = written;
	return 0;
}

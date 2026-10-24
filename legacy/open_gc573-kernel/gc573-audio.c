// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "gc573.h"

#define GC555_AUDIO_CHANNELS_5_1		6U
#define GC555_AUDIO_BUFFER_FRAMES(rate_hz)	((rate_hz) / 100U)
#define GC555_AUDIO_DMA_BYTES(rate_hz, channels, sample_bytes) \
	(GC555_AUDIO_BUFFER_FRAMES(rate_hz) * (channels) * (sample_bytes))
#define GC555_AUDIO_MIN_PERIOD_BYTES \
	GC555_AUDIO_DMA_BYTES(GC555_AUDIO_RATE_32000_HZ, \
			      GC555_AUDIO_CHANNELS_STEREO, sizeof(__le16))
#define GC555_AUDIO_MAX_DMA_BYTES \
	GC555_AUDIO_DMA_BYTES(GC555_AUDIO_RATE_48000_HZ, \
			      GC555_AUDIO_CHANNELS_7_1, 3U)
#define GC555_AUDIO_MAX_PERIOD_BYTES	(GC555_AUDIO_MAX_DMA_BYTES * 32U)
#define GC555_AUDIO_MAX_BUFFER_BYTES	(GC555_AUDIO_MAX_DMA_BYTES * 64U)
#define GC555_LINE_AUDIO_DMA_BYTES \
	GC555_AUDIO_DMA_BYTES(GC555_AUDIO_RATE_48000_HZ, \
			      GC555_AUDIO_CHANNELS_STEREO, sizeof(__le16))
#define GC555_LINE_AUDIO_MAX_PERIOD_BYTES \
	(GC555_LINE_AUDIO_DMA_BYTES * 32U)
#define GC555_LINE_AUDIO_MAX_BUFFER_BYTES \
	(GC555_LINE_AUDIO_DMA_BYTES * 64U)

enum gc555_audio_source {
	GC555_AUDIO_SOURCE_HDMI,
	GC555_AUDIO_SOURCE_LINE_IN,
	GC555_AUDIO_SOURCE_COUNT,
};

struct gc555_audio;

struct gc555_audio_stream {
	struct gc555_audio *audio;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *substream;
	snd_pcm_uframes_t hw_ptr;
	snd_pcm_uframes_t period_progress;
	enum gc555_audio_source source;
	struct gc555_hdmi_audio_format prepared_format;
	bool capture_enabled;
	bool dma_running;
	bool format_invalidated;
	bool format_monitoring;
};

struct gc555_audio {
	struct gc555_dev *gc555;
	struct snd_card *card;
	struct gc555_audio_stream stream[GC555_AUDIO_SOURCE_COUNT];
	/* Serializes DMA start and stop across both capture streams. */
	struct mutex control_lock;
	/* Protects stream callbacks, PCM positions, and disconnect state. */
	spinlock_t state_lock;
	struct work_struct format_work;
	bool disconnected;
	bool suspended;
};

static const struct snd_pcm_hardware gc555_audio_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
		 SNDRV_PCM_RATE_48000,
	.rate_min = GC555_AUDIO_RATE_32000_HZ,
	.rate_max = GC555_AUDIO_RATE_48000_HZ,
	.channels_min = GC555_AUDIO_CHANNELS_STEREO,
	.channels_max = GC555_AUDIO_CHANNELS_7_1,
	.buffer_bytes_max = GC555_AUDIO_MAX_BUFFER_BYTES,
	.period_bytes_min = GC555_AUDIO_MIN_PERIOD_BYTES,
	.period_bytes_max = GC555_AUDIO_MAX_PERIOD_BYTES,
	.periods_min = 2,
	.periods_max = 64,
};

static const struct snd_pcm_hardware gc555_line_audio_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = GC555_AUDIO_RATE_48000_HZ,
	.rate_max = GC555_AUDIO_RATE_48000_HZ,
	.channels_min = GC555_AUDIO_CHANNELS_STEREO,
	.channels_max = GC555_AUDIO_CHANNELS_STEREO,
	.buffer_bytes_max = GC555_LINE_AUDIO_MAX_BUFFER_BYTES,
	.period_bytes_min = GC555_LINE_AUDIO_DMA_BYTES,
	.period_bytes_max = GC555_LINE_AUDIO_MAX_PERIOD_BYTES,
	.periods_min = 2,
	.periods_max = 64,
};

static const unsigned int gc555_audio_channel_counts[] = {
	GC555_AUDIO_CHANNELS_STEREO,
	GC555_AUDIO_CHANNELS_5_1,
	GC555_AUDIO_CHANNELS_7_1,
};

static const struct snd_pcm_hw_constraint_list gc555_audio_channel_list = {
	.count = ARRAY_SIZE(gc555_audio_channel_counts),
	.list = gc555_audio_channel_counts,
};

static const u8 gc555_audio_7_1_alsa_from_hdmi[] = {
	/* Map FPGA slots to ALSA's 7.1 channel order. */
	0, 1, 2, 3, 6, 7, 4, 5,
};

static const u8 gc555_audio_5_1_alsa_from_hdmi[] = {
	/* Map the six populated FPGA slots to ALSA's 5.1 channel order. */
	0, 1, 4, 5, 2, 3,
};

static unsigned int gc555_audio_dma_channels(unsigned int channels)
{
	return channels == GC555_AUDIO_CHANNELS_5_1 ?
	       GC555_AUDIO_CHANNELS_7_1 : channels;
}

static bool gc555_audio_is_disconnected(struct gc555_audio *audio)
{
	unsigned long flags;
	bool disconnected;

	spin_lock_irqsave(&audio->state_lock, flags);
	disconnected = audio->disconnected;
	spin_unlock_irqrestore(&audio->state_lock, flags);

	return disconnected;
}

static void gc555_audio_set_disconnected(struct gc555_audio *audio)
{
	unsigned long flags;
	unsigned int source;

	spin_lock_irqsave(&audio->state_lock, flags);
	audio->disconnected = true;
	for (source = 0; source < GC555_AUDIO_SOURCE_COUNT; source++)
		audio->stream[source].capture_enabled = false;
	spin_unlock_irqrestore(&audio->state_lock, flags);
}

static int gc555_audio_set_capture_enabled(struct gc555_audio_stream *stream,
					   bool enabled)
{
	struct gc555_audio *audio = stream->audio;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&audio->state_lock, flags);
	if (enabled && (audio->disconnected || !stream->dma_running))
		ret = -ENODEV;
	else
		stream->capture_enabled = enabled;
	spin_unlock_irqrestore(&audio->state_lock, flags);

	return ret;
}

static void gc555_audio_copy_frames(struct snd_pcm_runtime *runtime,
				    void *destination, const void *source,
				    snd_pcm_uframes_t frames)
{
	const u8 *channel_map;
	const u8 *source_samples = source;
	u8 *destination_samples = destination;
	snd_pcm_uframes_t frame;
	unsigned int sample_bytes;
	unsigned int channel;

	if (runtime->channels == GC555_AUDIO_CHANNELS_STEREO) {
		memcpy(destination, source, frames_to_bytes(runtime, frames));
		return;
	}
	if (runtime->channels == GC555_AUDIO_CHANNELS_5_1)
		channel_map = gc555_audio_5_1_alsa_from_hdmi;
	else
		channel_map = gc555_audio_7_1_alsa_from_hdmi;
	sample_bytes = snd_pcm_format_physical_width(runtime->format) / 8U;

	for (frame = 0; frame < frames; frame++) {
		for (channel = 0; channel < runtime->channels; channel++) {
			unsigned int destination_offset;
			unsigned int source_offset;

			destination_offset =
				(frame * runtime->channels + channel) *
				sample_bytes;
			source_offset =
				(frame * GC555_AUDIO_CHANNELS_7_1 +
				 channel_map[channel]) *
				sample_bytes;
			memcpy(destination_samples + destination_offset,
			       source_samples + source_offset, sample_bytes);
		}
	}
}

static void gc555_audio_receive(void *context, const void *data, size_t bytes)
{
	struct gc555_audio_stream *stream = context;
	struct gc555_audio *audio;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	snd_pcm_uframes_t first_frames;
	snd_pcm_uframes_t frames;
	snd_pcm_uframes_t old_ptr;
	unsigned long flags;
	unsigned int dma_channels;
	unsigned int dma_frame_bytes;
	unsigned int sample_bytes;
	unsigned int elapsed = 0;

	if (!stream || !stream->audio || !data || !bytes)
		return;
	audio = stream->audio;

	spin_lock_irqsave(&audio->state_lock, flags);
	substream = stream->substream;
	if (!stream->capture_enabled || !substream) {
		spin_unlock_irqrestore(&audio->state_lock, flags);
		return;
	}

	runtime = substream->runtime;
	if (!runtime || !runtime->dma_area || !runtime->buffer_size ||
	    !runtime->period_size) {
		spin_unlock_irqrestore(&audio->state_lock, flags);
		return;
	}

	sample_bytes = snd_pcm_format_physical_width(runtime->format) / 8U;
	dma_channels = gc555_audio_dma_channels(runtime->channels);
	dma_frame_bytes = dma_channels * sample_bytes;
	if (!dma_frame_bytes || bytes % dma_frame_bytes) {
		spin_unlock_irqrestore(&audio->state_lock, flags);
		return;
	}
	frames = bytes / dma_frame_bytes;
	if (!frames || frames > runtime->buffer_size) {
		spin_unlock_irqrestore(&audio->state_lock, flags);
		return;
	}

	old_ptr = stream->hw_ptr;
	first_frames = min(frames, runtime->buffer_size - old_ptr);
	gc555_audio_copy_frames(
		runtime, runtime->dma_area + frames_to_bytes(runtime, old_ptr),
		data, first_frames);
	if (first_frames < frames)
		gc555_audio_copy_frames(
			runtime, runtime->dma_area,
			(const u8 *)data + first_frames * dma_frame_bytes,
			frames - first_frames);

	stream->hw_ptr = (old_ptr + frames) % runtime->buffer_size;
	stream->period_progress += frames;
	while (stream->period_progress >= runtime->period_size) {
		stream->period_progress -= runtime->period_size;
		elapsed++;
	}
	spin_unlock_irqrestore(&audio->state_lock, flags);

	while (elapsed--)
		snd_pcm_period_elapsed(substream);
}

static void gc555_audio_stop_sync(struct gc555_audio_stream *stream)
{
	struct gc555_audio *audio = stream->audio;
	unsigned long flags;
	bool running;

	gc555_audio_set_capture_enabled(stream, false);

	mutex_lock(&audio->control_lock);
	spin_lock_irqsave(&audio->state_lock, flags);
	running = stream->dma_running;
	stream->dma_running = false;
	spin_unlock_irqrestore(&audio->state_lock, flags);
	if (running) {
		if (stream->source == GC555_AUDIO_SOURCE_LINE_IN)
			gc555_dma_stop_line_audio(audio->gc555, stream);
		else
			gc555_dma_stop_audio(audio->gc555, stream);
	}
	mutex_unlock(&audio->control_lock);
}

static bool
gc555_audio_format_equal(const struct gc555_hdmi_audio_format *left,
			 const struct gc555_hdmi_audio_format *right)
{
	return left->valid == right->valid &&
	       left->generation == right->generation &&
	       (!left->valid ||
		(left->rate_hz == right->rate_hz &&
		 left->channels == right->channels &&
		 left->channel_allocation == right->channel_allocation &&
		 left->transport == right->transport));
}

static void gc555_audio_format_work(struct work_struct *work)
{
	struct gc555_audio *audio = container_of(work, struct gc555_audio,
						 format_work);
	struct gc555_audio_stream *stream =
		&audio->stream[GC555_AUDIO_SOURCE_HDMI];
	struct gc555_hdmi_audio_format format;
	struct snd_pcm_substream *substream;
	unsigned long flags;
	bool xrun;
	int ret;

	mutex_lock(&audio->control_lock);
	ret = gc555_it6805_get_audio_format(audio->gc555->it6805, &format);
	if (ret && ret != -ENODATA)
		format = (struct gc555_hdmi_audio_format){};

	spin_lock_irqsave(&audio->state_lock, flags);
	/*
	 * Do not stop stream on transient clock jitter or packet gap (!format.valid).
	 * If sample rate matches, update prepared format without stopping DMA or xrun.
	 */
	if (audio->disconnected || audio->suspended ||
	    !stream->format_monitoring || !stream->dma_running ||
	    stream->format_invalidated || ret || !format.valid ||
	    gc555_audio_format_equal(&format, &stream->prepared_format)) {
		spin_unlock_irqrestore(&audio->state_lock, flags);
		mutex_unlock(&audio->control_lock);
		return;
	}

	if (format.rate_hz == stream->prepared_format.rate_hz) {
		stream->prepared_format = format;
		spin_unlock_irqrestore(&audio->state_lock, flags);
		mutex_unlock(&audio->control_lock);
		return;
	}

	xrun = stream->capture_enabled;
	stream->capture_enabled = false;
	stream->format_invalidated = true;
	stream->format_monitoring = false;
	stream->dma_running = false;
	substream = stream->substream;
	spin_unlock_irqrestore(&audio->state_lock, flags);

	if (xrun && substream)
		snd_pcm_stop_xrun(substream);
	gc555_dma_stop_audio(audio->gc555, stream);
	mutex_unlock(&audio->control_lock);
}

void gc555_audio_hdmi_format_changed(struct gc555_dev *gc555)
{
	struct gc555_audio *audio;
	unsigned long flags;

	if (!gc555)
		return;
	audio = READ_ONCE(gc555->audio);
	if (!audio)
		return;

	spin_lock_irqsave(&audio->state_lock, flags);
	if (!audio->disconnected && !audio->suspended &&
	    audio->stream[GC555_AUDIO_SOURCE_HDMI].format_monitoring)
		schedule_work(&audio->format_work);
	spin_unlock_irqrestore(&audio->state_lock, flags);
}

static int gc555_audio_pcm_open(struct snd_pcm_substream *substream)
{
	struct gc555_audio_stream *stream = snd_pcm_substream_chip(substream);
	struct gc555_audio *audio = stream->audio;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&audio->state_lock, flags);
	if (audio->disconnected)
		ret = -ENODEV;
	else if (stream->substream)
		ret = -EBUSY;
	else {
		stream->substream = substream;
		ret = 0;
	}
	spin_unlock_irqrestore(&audio->state_lock, flags);
	if (ret)
		return ret;

	if (stream->source == GC555_AUDIO_SOURCE_LINE_IN) {
		substream->runtime->hw = gc555_line_audio_hardware;
	} else {
		substream->runtime->hw = gc555_audio_hardware;
		ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
						 SNDRV_PCM_HW_PARAM_CHANNELS,
						 &gc555_audio_channel_list);
		if (ret < 0)
			goto fail;
	}
	ret = snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		goto fail;
	return 0;

fail:
	spin_lock_irqsave(&audio->state_lock, flags);
	if (stream->substream == substream)
		stream->substream = NULL;
	spin_unlock_irqrestore(&audio->state_lock, flags);
	return ret;
}

static int gc555_audio_pcm_close(struct snd_pcm_substream *substream)
{
	struct gc555_audio_stream *stream = snd_pcm_substream_chip(substream);
	struct gc555_audio *audio = stream->audio;
	unsigned long flags;

	spin_lock_irqsave(&audio->state_lock, flags);
	stream->capture_enabled = false;
	if (stream->source == GC555_AUDIO_SOURCE_HDMI)
		stream->format_monitoring = false;
	spin_unlock_irqrestore(&audio->state_lock, flags);
	if (stream->source == GC555_AUDIO_SOURCE_HDMI)
		cancel_work_sync(&audio->format_work);
	gc555_audio_stop_sync(stream);
	spin_lock_irqsave(&audio->state_lock, flags);
	if (stream->substream == substream)
		stream->substream = NULL;
	spin_unlock_irqrestore(&audio->state_lock, flags);

	return 0;
}

static int gc555_audio_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct gc555_audio_stream *stream = snd_pcm_substream_chip(substream);
	struct gc555_audio *audio = stream->audio;
	struct gc555_it6805 *it6805 = audio->gc555->it6805;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct gc555_hdmi_audio_format format = {};
	unsigned long flags;
	unsigned int dma_channels;
	unsigned int sample_bits;
	bool restart_dma = false;
	bool running;
	int ret = 0;

	spin_lock_irqsave(&audio->state_lock, flags);
	stream->capture_enabled = false;
	stream->hw_ptr = 0;
	stream->period_progress = 0;
	spin_unlock_irqrestore(&audio->state_lock, flags);

	/* Prepare primes DMA; trigger controls whether samples reach ALSA. */
	mutex_lock(&audio->control_lock);
	spin_lock_irqsave(&audio->state_lock, flags);
	running = stream->dma_running;
	spin_unlock_irqrestore(&audio->state_lock, flags);
	if (gc555_audio_is_disconnected(audio)) {
		ret = -ENODEV;
	} else if (stream->source == GC555_AUDIO_SOURCE_HDMI) {
		ret = gc555_it6805_get_audio_format(it6805, &format);
		if (ret || !format.valid) {
			/* HDMI audio clock not yet locked; prime DMA with requested runtime parameters */
			format.rate_hz = runtime->rate;
			format.channels = runtime->channels;
			format.transport = GC555_HDMI_AUDIO_SAMPLES;
			format.valid = true;
			ret = 0;
		}

		if (running) {
			spin_lock_irqsave(&audio->state_lock, flags);
			restart_dma = stream->format_invalidated;
			if (!restart_dma && format.rate_hz != stream->prepared_format.rate_hz)
				restart_dma = true;
			if (restart_dma)
				stream->dma_running = false;
			spin_unlock_irqrestore(&audio->state_lock, flags);
			if (restart_dma) {
				gc555_dma_stop_audio(audio->gc555, stream);
				running = false;
			}
		}
	}
	if (!ret && !running) {
		if (stream->source == GC555_AUDIO_SOURCE_LINE_IN) {
			ret = gc555_dma_start_line_audio(audio->gc555,
							 gc555_audio_receive,
							 stream);
		} else {
			sample_bits = snd_pcm_format_physical_width(runtime->format);
			dma_channels = gc555_audio_dma_channels(runtime->channels);
			ret = gc555_dma_start_audio(audio->gc555,
						    runtime->rate,
						    dma_channels,
						    sample_bits,
						    gc555_audio_receive,
						    stream);
		}
		if (!ret) {
			spin_lock_irqsave(&audio->state_lock, flags);
			stream->dma_running = true;
			stream->format_invalidated = false;
			if (stream->source == GC555_AUDIO_SOURCE_HDMI) {
				stream->prepared_format = format;
				stream->format_monitoring = true;
			}
			spin_unlock_irqrestore(&audio->state_lock, flags);
		}
	} else if (!ret && stream->source == GC555_AUDIO_SOURCE_HDMI) {
		spin_lock_irqsave(&audio->state_lock, flags);
		stream->format_invalidated = false;
		stream->prepared_format = format;
		stream->format_monitoring = true;
		spin_unlock_irqrestore(&audio->state_lock, flags);
	}
	mutex_unlock(&audio->control_lock);

	return ret;
}

static int gc555_audio_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct gc555_audio_stream *stream = snd_pcm_substream_chip(substream);
	struct gc555_audio *audio = stream->audio;
	unsigned long flags;

	spin_lock_irqsave(&audio->state_lock, flags);
	stream->capture_enabled = false;
	if (stream->source == GC555_AUDIO_SOURCE_HDMI)
		stream->format_monitoring = false;
	spin_unlock_irqrestore(&audio->state_lock, flags);
	if (stream->source == GC555_AUDIO_SOURCE_HDMI)
		cancel_work_sync(&audio->format_work);
	gc555_audio_stop_sync(stream);
	spin_lock_irqsave(&audio->state_lock, flags);
	stream->format_invalidated = false;
	stream->prepared_format = (struct gc555_hdmi_audio_format){};
	spin_unlock_irqrestore(&audio->state_lock, flags);
	return 0;
}

static int gc555_audio_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct gc555_audio_stream *stream = snd_pcm_substream_chip(substream);
	struct gc555_audio *audio = stream->audio;
	unsigned long flags;
	bool start;
	bool invalidated;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		start = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		start = false;
		break;
	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&audio->state_lock, flags);
	invalidated = stream->format_invalidated;
	spin_unlock_irqrestore(&audio->state_lock, flags);
	if (start && invalidated)
		return -EPIPE;

	return gc555_audio_set_capture_enabled(stream, start);
}

static snd_pcm_uframes_t
gc555_audio_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct gc555_audio_stream *stream = snd_pcm_substream_chip(substream);
	struct gc555_audio *audio = stream->audio;
	unsigned long flags;
	snd_pcm_uframes_t pointer;

	spin_lock_irqsave(&audio->state_lock, flags);
	pointer = stream->hw_ptr;
	spin_unlock_irqrestore(&audio->state_lock, flags);

	return pointer;
}

static const struct snd_pcm_ops gc555_audio_pcm_ops = {
	.open = gc555_audio_pcm_open,
	.close = gc555_audio_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_free = gc555_audio_pcm_hw_free,
	.prepare = gc555_audio_pcm_prepare,
	.trigger = gc555_audio_pcm_trigger,
	.pointer = gc555_audio_pcm_pointer,
};

static int gc555_audio_init_stream(struct gc555_audio *audio,
				   enum gc555_audio_source source,
				   unsigned int device, const char *name)
{
	struct gc555_audio_stream *stream = &audio->stream[source];
	int ret;

	stream->audio = audio;
	stream->source = source;

	ret = snd_pcm_new(audio->card, name, device, 0, 1, &stream->pcm);
	if (ret < 0)
		return ret;

	stream->pcm->private_data = stream;
	strscpy(stream->pcm->name, name, sizeof(stream->pcm->name));
	snd_pcm_set_ops(stream->pcm, SNDRV_PCM_STREAM_CAPTURE,
			&gc555_audio_pcm_ops);

	return snd_pcm_set_managed_buffer_all(stream->pcm,
					      SNDRV_DMA_TYPE_VMALLOC, NULL, 0,
					      GC555_AUDIO_MAX_BUFFER_BYTES);
}

int gc555_audio_init(struct gc555_dev *gc555)
{
	struct gc555_audio *audio;
	struct snd_card *card;
	int ret;

	if (!gc555 || !gc555->dma)
		return -EINVAL;
	if (gc555->audio)
		return 0;

	const char *card_id = (gc555->pdev->subsystem_device == 0x5730) ? "A4K" : "GC555";

	ret = snd_card_new(gc555->dev, -1, card_id, THIS_MODULE,
			   sizeof(*audio), &card);
	if (ret < 0)
		return dev_err_probe(gc555->dev, ret,
				     "failed to allocate ALSA card\n");

	audio = card->private_data;
	audio->gc555 = gc555;
	audio->card = card;
	mutex_init(&audio->control_lock);
	spin_lock_init(&audio->state_lock);
	INIT_WORK(&audio->format_work, gc555_audio_format_work);

	if (gc555->pdev->subsystem_device == 0x5730) {
		strscpy(card->driver, "GC573", sizeof(card->driver));
		strscpy(card->shortname, "AVerMedia Live Gamer 4K",
			sizeof(card->shortname));
		strscpy(card->longname, "AVerMedia Live Gamer 4K Audio Capture",
			sizeof(card->longname));
	} else {
		strscpy(card->driver, "GC555", sizeof(card->driver));
		strscpy(card->shortname, "AVerMedia Live Gamer BOLT",
			sizeof(card->shortname));
		strscpy(card->longname, "AVerMedia Live Gamer BOLT Audio Capture",
			sizeof(card->longname));
	}

	ret = gc555_audio_init_stream(audio, GC555_AUDIO_SOURCE_HDMI, 0,
				      (gc555->pdev->subsystem_device == 0x5730) ?
				      "GC573 HDMI Capture" : "GC555 HDMI Capture");
	if (ret < 0)
		goto free_card;

	ret = gc555_audio_init_stream(audio, GC555_AUDIO_SOURCE_LINE_IN, 1,
				      (gc555->pdev->subsystem_device == 0x5730) ?
				      "GC573 Line-In Capture" : "GC555 Line-In Capture");
	if (ret < 0)
		goto free_card;

	ret = snd_card_register(card);
	if (ret < 0)
		goto free_card;

	gc555->audio = audio;
	return 0;

free_card:
	snd_card_free(card);
	return dev_err_probe(gc555->dev, ret,
			     "failed to register ALSA capture device\n");
}

void gc555_audio_cleanup(struct gc555_dev *gc555)
{
	struct gc555_audio *audio;
	struct snd_card *card;
	unsigned int source;

	if (!gc555 || !gc555->audio)
		return;

	audio = gc555->audio;
	card = audio->card;
	gc555_audio_set_disconnected(audio);
	cancel_work_sync(&audio->format_work);
	snd_card_disconnect(card);
	for (source = 0; source < GC555_AUDIO_SOURCE_COUNT; source++)
		gc555_audio_stop_sync(&audio->stream[source]);
	snd_card_disconnect_sync(card);
	gc555->audio = NULL;
	snd_card_free_when_closed(card);
}

void gc555_audio_suspend(struct gc555_dev *gc555)
{
	struct gc555_audio *audio;
	unsigned int source;

	if (!gc555 || !gc555->audio)
		return;

	audio = gc555->audio;
	spin_lock_irq(&audio->state_lock);
	audio->suspended = true;
	spin_unlock_irq(&audio->state_lock);
	cancel_work_sync(&audio->format_work);
	for (source = 0; source < GC555_AUDIO_SOURCE_COUNT; source++) {
		snd_pcm_suspend_all(audio->stream[source].pcm);
		gc555_audio_stop_sync(&audio->stream[source]);
	}
	snd_power_change_state(audio->card, SNDRV_CTL_POWER_D3hot);
}

void gc555_audio_resume(struct gc555_dev *gc555)
{
	struct gc555_audio *audio;
	unsigned long flags;

	if (!gc555 || !gc555->audio ||
	    gc555_audio_is_disconnected(gc555->audio))
		return;

	audio = gc555->audio;
	snd_power_change_state(audio->card, SNDRV_CTL_POWER_D0);
	spin_lock_irqsave(&audio->state_lock, flags);
	audio->suspended = false;
	spin_unlock_irqrestore(&audio->state_lock, flags);
}

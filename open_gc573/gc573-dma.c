// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "gc573.h"

#define GC555_REG_AUDIO_STREAM_CONTROL	0x0008
#define GC555_REG_RESET			0x000c
#define GC555_REG_INTERRUPT_STATUS	0x0010
#define GC555_REG_AUDIO_CHANNEL_STATUS	0x0014
#define GC555_REG_INTERRUPT_ENABLE	0x001c
#define GC555_REG_AUDIO_FORMAT		0x0200
#define GC555_REG_AUDIO_BUFFER_DWORDS	0x0204
#define GC555_REG_AUDIO_BUFFER0_LOW	0x0208
#define GC555_REG_AUDIO_BUFFER0_HIGH	0x020c
#define GC555_REG_AUDIO_BUFFER1_LOW	0x0210
#define GC555_REG_AUDIO_BUFFER1_HIGH	0x0214
#define GC555_REG_AUDIO_BUFFER_CONTROL	0x0218
#define GC555_REG_AUDIO_RATE		0x021c
#define GC555_REG_AUDIO_MAP0		0x02b4
#define GC555_REG_AUDIO_MAP1		0x02b8
#define GC555_REG_AUDIO_MAP2		0x02bc
#define GC555_REG_AUDIO_MAP3		0x02c0
#define GC555_REG_LINE_AUDIO_FORMAT	0x02d0
#define GC555_REG_LINE_AUDIO_CHANNELS	0x02d4
#define GC555_REG_LINE_AUDIO_SOURCE	0x02d8
#define GC555_REG_LINE_AUDIO_DWORDS	0x02e0
#define GC555_REG_LINE_AUDIO_BUFFER0_LOW	0x02e4
#define GC555_REG_LINE_AUDIO_BUFFER0_HIGH	0x02e8
#define GC555_REG_LINE_AUDIO_BUFFER1_LOW	0x02ec
#define GC555_REG_LINE_AUDIO_BUFFER1_HIGH	0x02f0
#define GC555_REG_LINE_AUDIO_MAP		0x02f4
#define GC555_REG_LINE_AUDIO_RATE	0x02fc
#define GC555_REG_VIDEO_CHANNEL		0x0300

#define GC555_IRQ_VIDEO_TERMINATED	BIT(0)
#define GC555_IRQ_VIDEO_COMPLETE		BIT(1)
#define GC555_IRQ_AUDIO_COMPLETE	BIT(5)
#define GC555_IRQ_LINE_AUDIO_COMPLETE	BIT(9)
#define GC555_IRQ_VIDEO_MASK		(GC555_IRQ_VIDEO_TERMINATED | \
					 GC555_IRQ_VIDEO_COMPLETE)
#define GC555_IRQ_ACK_MASK		(0x80001fffU | BIT(13))
#define GC555_AUDIO_ENGINE_RESET	BIT(8)
#define GC555_LINE_AUDIO_ENGINE_RESET	BIT(9)
#define GC555_AUDIO_STREAM_ENABLE	BIT(1)
#define GC555_LINE_AUDIO_STREAM_ENABLE	BIT(5)

#define GC555_AUDIO_BUFFER_COUNT	2U
#define GC555_AUDIO_BUFFER_CAPACITY	0xc000U
#define GC555_AUDIO_DONE_QUEUE_LENGTH	64U
#define GC555_AUDIO_SAMPLE_BITS_16	16U
#define GC555_AUDIO_SAMPLE_BITS_24	24U
#define GC555_LINE_AUDIO_PERIODS		4U

struct gc555_dma;

struct gc555_audio_dma {
	struct gc555_dma *dma;
	struct work_struct work;
	void *buffer[GC555_AUDIO_BUFFER_COUNT];
	dma_addr_t buffer_dma[GC555_AUDIO_BUFFER_COUNT];
	u8 done_queue[GC555_AUDIO_DONE_QUEUE_LENGTH];
	unsigned int done_head;
	unsigned int done_tail;
	unsigned int buffer_bytes;
	gc555_audio_data_t data;
	void *data_context;
	const char *name;
	u32 irq_bit;
	u32 reset_bit;
	u32 stream_enable_bit;
	unsigned int channel_shift;
	bool work_scheduled;
	bool streaming;
};

struct gc555_dma {
	struct gc555_dev *gc555;
	/* Serializes audio setup and DMA engine resets. */
	struct mutex control_lock;
	/* Serializes register updates across process and hard-IRQ contexts. */
	spinlock_t register_lock;
	/* Protects callbacks, audio ownership, and completion queue state. */
	spinlock_t state_lock;
	struct gc555_audio_dma hdmi_audio;
	struct gc555_audio_dma line_audio;
	gc555_dma_video_irq_t video_irq;
	void *video_irq_context;
	int irq;
	bool irq_requested;
	bool irq_vectors_allocated;
	bool device_lost;
};

static int gc555_dma_read(struct gc555_dma *dma, u32 offset, u32 *value)
{
	int ret;

	if (READ_ONCE(dma->device_lost))
		return -ENODEV;

	ret = gc555_bridge_read(dma->gc555, offset, value);
	if (ret)
		return ret;
	if (*value != U32_MAX)
		return 0;

	WRITE_ONCE(dma->device_lost, true);
	dev_err_once(dma->gc555->dev, "FPGA register space is inaccessible\n");
	return -ENODEV;
}

static int gc555_dma_write(struct gc555_dma *dma, u32 offset, u32 value)
{
	if (READ_ONCE(dma->device_lost))
		return -ENODEV;

	return gc555_bridge_write(dma->gc555, offset, value);
}

static int gc555_dma_update_bits(struct gc555_dma *dma, u32 offset, u32 mask,
				 u32 value)
{
	unsigned long flags;
	u32 reg;
	int ret;

	spin_lock_irqsave(&dma->register_lock, flags);
	ret = gc555_dma_read(dma, offset, &reg);
	if (!ret) {
		reg = (reg & ~mask) | (value & mask);
		ret = gc555_dma_write(dma, offset, reg);
	}
	spin_unlock_irqrestore(&dma->register_lock, flags);

	return ret;
}

int gc555_dma_read_register(struct gc555_dev *gc555, u32 offset, u32 *value)
{
	if (!gc555 || !gc555->dma)
		return -ENODEV;

	return gc555_dma_read(gc555->dma, offset, value);
}

int gc555_dma_write_register(struct gc555_dev *gc555, u32 offset, u32 value)
{
	if (!gc555 || !gc555->dma)
		return -ENODEV;

	return gc555_dma_write(gc555->dma, offset, value);
}

int gc555_dma_update_register_bits(struct gc555_dev *gc555, u32 offset,
				   u32 mask, u32 value)
{
	if (!gc555 || !gc555->dma)
		return -ENODEV;

	return gc555_dma_update_bits(gc555->dma, offset, mask, value);
}

bool gc555_dma_device_lost(struct gc555_dev *gc555)
{
	return !gc555 || !gc555->dma || READ_ONCE(gc555->dma->device_lost);
}

void gc555_dma_mark_device_lost(struct gc555_dev *gc555)
{
	if (gc555 && gc555->dma)
		WRITE_ONCE(gc555->dma->device_lost, true);
}

static void gc555_dma_free_audio_buffers(struct gc555_audio_dma *audio)
{
	unsigned int index;

	for (index = 0; index < GC555_AUDIO_BUFFER_COUNT; index++) {
		if (!audio->buffer[index])
			continue;

		dma_free_coherent(audio->dma->gc555->dev,
				  GC555_AUDIO_BUFFER_CAPACITY,
				  audio->buffer[index],
				  audio->buffer_dma[index]);
		audio->buffer[index] = NULL;
		audio->buffer_dma[index] = 0;
	}
}

static int gc555_dma_alloc_audio_buffers(struct gc555_audio_dma *audio)
{
	unsigned int index;

	if (audio->buffer[0] && audio->buffer[1])
		return 0;

	gc555_dma_free_audio_buffers(audio);
	for (index = 0; index < GC555_AUDIO_BUFFER_COUNT; index++) {
		audio->buffer[index] =
			dma_alloc_coherent(audio->dma->gc555->dev,
					   GC555_AUDIO_BUFFER_CAPACITY,
					   &audio->buffer_dma[index],
					   GFP_KERNEL);
		if (!audio->buffer[index]) {
			gc555_dma_free_audio_buffers(audio);
			return -ENOMEM;
		}
	}

	return 0;
}

static int gc555_dma_reset_engine(struct gc555_dma *dma, u32 reset_bit)
{
	bool completed = false;
	u32 status;
	int attempt;
	int ret;

	ret = gc555_dma_write(dma, GC555_REG_RESET, reset_bit);
	if (ret)
		return ret;

	for (attempt = 0; attempt < 11; attempt++) {
		usleep_range(1000, 2000);
		ret = gc555_dma_read(dma, GC555_REG_RESET, &status);
		if (ret)
			return ret;
		if (status & reset_bit) {
			completed = true;
			break;
		}
	}

	msleep(30);
	ret = gc555_dma_read(dma, GC555_REG_RESET, &status);
	if (ret)
		return ret;

	if (!completed)
		dev_dbg(dma->gc555->dev,
			"DMA reset bit %#x was not observed, settled status %#x\n",
			reset_bit, status);

	return 0;
}

int gc555_dma_reset_video(struct gc555_dev *gc555)
{
	struct gc555_dma *dma;
	int ret;

	if (!gc555 || !gc555->dma)
		return -ENODEV;

	dma = gc555->dma;
	mutex_lock(&dma->control_lock);
	ret = gc555_dma_reset_engine(dma, BIT(0));
	mutex_unlock(&dma->control_lock);

	return ret;
}

static bool gc555_dma_queue_audio_locked(struct gc555_audio_dma *audio,
					 unsigned int index)
{
	unsigned int next;

	next = (audio->done_head + 1) % GC555_AUDIO_DONE_QUEUE_LENGTH;
	if (next == audio->done_tail)
		return false;

	audio->done_queue[audio->done_head] = index;
	audio->done_head = next;
	if (audio->work_scheduled)
		return false;

	audio->work_scheduled = true;
	return true;
}

static bool gc555_dma_audio_done_locked(struct gc555_audio_dma *audio,
					u32 status, u32 channel_status)
{
	unsigned int channel_code;

	if (!(status & audio->irq_bit) || !audio->streaming)
		return false;

	channel_code = (channel_status >> audio->channel_shift) & GENMASK(1, 0);
	if (channel_code != 1 && channel_code != 2)
		return false;

	return gc555_dma_queue_audio_locked(audio, channel_code - 1);
}

static irqreturn_t gc555_dma_irq(int irq, void *data)
{
	struct gc555_dma *dma = data;
	struct gc555_audio_dma *audio;
	gc555_dma_video_irq_t video_irq;
	void *video_irq_context;
	unsigned long flags;
	u32 video_channel_status = 0;
	u32 channel_status;
	u32 i2c_acknowledged;
	u32 status;
	bool schedule_hdmi_audio = false;
	bool schedule_line_audio = false;
	int ret;

	ret = gc555_dma_read(dma, GC555_REG_INTERRUPT_STATUS, &status);
	if (ret || !(status & GC555_IRQ_ACK_MASK))
		return IRQ_NONE;

	if (status & GC555_IRQ_VIDEO_COMPLETE) {
		ret = gc555_dma_read(dma, GC555_REG_VIDEO_CHANNEL,
				     &video_channel_status);
		if (ret)
			video_channel_status = 0;
	}
	spin_lock_irqsave(&dma->state_lock, flags);
	video_irq = dma->video_irq;
	video_irq_context = dma->video_irq_context;
	spin_unlock_irqrestore(&dma->state_lock, flags);
	if ((status & GC555_IRQ_VIDEO_MASK) && video_irq)
		video_irq(video_irq_context, status, video_channel_status);

	if (status & (GC555_IRQ_AUDIO_COMPLETE |
		      GC555_IRQ_LINE_AUDIO_COMPLETE)) {
		ret = gc555_dma_read(dma, GC555_REG_AUDIO_CHANNEL_STATUS,
					&channel_status);
		if (!ret) {
			spin_lock_irqsave(&dma->state_lock, flags);
			audio = &dma->hdmi_audio;
			schedule_hdmi_audio =
				gc555_dma_audio_done_locked(audio, status,
							    channel_status);
			audio = &dma->line_audio;
			schedule_line_audio =
				gc555_dma_audio_done_locked(audio, status,
							    channel_status);
			spin_unlock_irqrestore(&dma->state_lock, flags);
		}
	}

	i2c_acknowledged = gc555_i2c_irq(dma->gc555, status);
	gc555_dma_write(dma, GC555_REG_INTERRUPT_STATUS,
			 status & GC555_IRQ_ACK_MASK & ~i2c_acknowledged);
	if (schedule_hdmi_audio)
		schedule_work(&dma->hdmi_audio.work);
	if (schedule_line_audio)
		schedule_work(&dma->line_audio.work);

	return IRQ_HANDLED;
}

static void gc555_dma_audio_work(struct work_struct *work)
{
	struct gc555_audio_dma *audio =
		container_of(work, struct gc555_audio_dma, work);
	struct gc555_dma *dma = audio->dma;

	for (;;) {
		gc555_audio_data_t audio_data;
		void *audio_data_context;
		unsigned long flags;
		unsigned int bytes;
		unsigned int index;
		bool streaming;

		spin_lock_irqsave(&dma->state_lock, flags);
		if (audio->done_tail == audio->done_head) {
			audio->work_scheduled = false;
			spin_unlock_irqrestore(&dma->state_lock, flags);
			return;
		}

		index = audio->done_queue[audio->done_tail];
		audio->done_tail = (audio->done_tail + 1) %
				   GC555_AUDIO_DONE_QUEUE_LENGTH;
		streaming = audio->streaming;
		bytes = audio->buffer_bytes;
		audio_data = audio->data;
		audio_data_context = audio->data_context;
		spin_unlock_irqrestore(&dma->state_lock, flags);

		if (!streaming || !audio_data ||
		    index >= GC555_AUDIO_BUFFER_COUNT ||
		    !audio->buffer[index])
			continue;

		dma_rmb();
		audio_data(audio_data_context, audio->buffer[index], bytes);
	}
}

static int gc555_dma_disable_audio_locked(struct gc555_audio_dma *audio)
{
	struct gc555_dma *dma = audio->dma;
	unsigned long flags;
	u32 pending;
	int first_error = 0;
	int ret;

	spin_lock_irqsave(&dma->state_lock, flags);
	audio->streaming = false;
	spin_unlock_irqrestore(&dma->state_lock, flags);

	ret = gc555_dma_update_bits(dma, GC555_REG_AUDIO_STREAM_CONTROL,
				    audio->stream_enable_bit, 0);
	if (ret)
		first_error = ret;
	ret = gc555_dma_update_bits(dma, GC555_REG_INTERRUPT_ENABLE,
				    audio->irq_bit, 0);
	if (ret && !first_error)
		first_error = ret;
	ret = gc555_dma_read(dma, GC555_REG_INTERRUPT_STATUS, &pending);
	if (ret && !first_error)
		first_error = ret;
	if (!ret && (pending & audio->irq_bit)) {
		ret = gc555_dma_write(dma, GC555_REG_INTERRUPT_STATUS,
				      audio->irq_bit);
		if (ret && !first_error)
			first_error = ret;
	}

	return first_error;
}

static void gc555_dma_clear_audio_state_locked(struct gc555_audio_dma *audio)
{
	struct gc555_dma *dma = audio->dma;
	unsigned long flags;

	spin_lock_irqsave(&dma->state_lock, flags);
	audio->done_head = 0;
	audio->done_tail = 0;
	audio->work_scheduled = false;
	audio->streaming = false;
	audio->data = NULL;
	audio->data_context = NULL;
	spin_unlock_irqrestore(&dma->state_lock, flags);
}

static void gc555_dma_quiesce_audio_locked(struct gc555_audio_dma *audio,
					   bool reset_engine)
{
	struct gc555_dma *dma = audio->dma;
	int ret;

	ret = gc555_dma_disable_audio_locked(audio);
	if (ret && ret != -ENODEV)
		dev_warn(dma->gc555->dev,
			 "failed to disable %s DMA: %d\n", audio->name, ret);

	if (dma->irq_requested)
		synchronize_irq(dma->irq);
	cancel_work_sync(&audio->work);

	if (reset_engine && !READ_ONCE(dma->device_lost)) {
		ret = gc555_dma_reset_engine(dma, audio->reset_bit);
		if (ret && ret != -ENODEV)
			dev_warn(dma->gc555->dev,
				 "%s DMA reset did not complete: %d\n",
				 audio->name, ret);
	}

	gc555_dma_clear_audio_state_locked(audio);
}

static bool gc555_dma_audio_format_valid(unsigned int rate_hz,
					 unsigned int channels,
					 unsigned int sample_bits)
{
	bool rate_valid;

	rate_valid = rate_hz == GC555_AUDIO_RATE_32000_HZ ||
		     rate_hz == GC555_AUDIO_RATE_44100_HZ ||
		     rate_hz == GC555_AUDIO_RATE_48000_HZ;

	return rate_valid &&
	       (sample_bits == GC555_AUDIO_SAMPLE_BITS_16 ||
		sample_bits == GC555_AUDIO_SAMPLE_BITS_24) &&
	       (channels == GC555_AUDIO_CHANNELS_STEREO ||
		channels == GC555_AUDIO_CHANNELS_7_1);
}

int gc555_dma_start_audio(struct gc555_dev *gc555, unsigned int rate_hz,
			  unsigned int channels, unsigned int sample_bits,
			  gc555_audio_data_t data,
			  void *data_context)
{
	struct gc555_audio_dma *audio;
	struct gc555_dma *dma;
	unsigned long flags;
	unsigned int buffer_bytes;
	unsigned int period_units;
	unsigned int sample_bytes;
	u32 observed_rate;
	u32 pending;
	bool setup_started = false;
	int ret;

	if (!gc555 || !gc555->dma)
		return -ENODEV;
	if (!data || !data_context ||
	    !gc555_dma_audio_format_valid(rate_hz, channels, sample_bits))
		return -EINVAL;
	ret = gc555_bridge_get_audio_rate(gc555, &observed_rate);
	if (!ret && observed_rate != rate_hz)
		dev_dbg(gc555->dev,
			"observed rate %u does not match requested audio rate %u\n",
			observed_rate, rate_hz);
	if (ret && ret != -EAGAIN)
		return ret;

	dma = gc555->dma;
	audio = &dma->hdmi_audio;
	sample_bytes = sample_bits / 8U;
	/* Packed 44.1 kHz needs two 10 ms units for dword alignment. */
	period_units = sample_bits == GC555_AUDIO_SAMPLE_BITS_24 &&
		       rate_hz == GC555_AUDIO_RATE_44100_HZ ? 2U : 1U;
	buffer_bytes = rate_hz / 100U * period_units * channels *
		       sample_bytes;
	if (!IS_ALIGNED(buffer_bytes, sizeof(u32)) ||
	    buffer_bytes > GC555_AUDIO_BUFFER_CAPACITY)
		return -EINVAL;

	mutex_lock(&dma->control_lock);
	spin_lock_irqsave(&dma->state_lock, flags);
	if (audio->streaming) {
		spin_unlock_irqrestore(&dma->state_lock, flags);
		ret = -EBUSY;
		goto unlock;
	}
	spin_unlock_irqrestore(&dma->state_lock, flags);

	ret = gc555_dma_alloc_audio_buffers(audio);
	if (ret)
		goto unlock;

	setup_started = true;
	ret = gc555_dma_update_bits(dma, GC555_REG_AUDIO_STREAM_CONTROL,
				    GC555_AUDIO_STREAM_ENABLE, 0);
	if (ret)
		goto rollback;
	ret = gc555_dma_update_bits(dma, GC555_REG_INTERRUPT_ENABLE,
				    GC555_IRQ_AUDIO_COMPLETE, 0);
	if (ret)
		goto rollback;
	ret = gc555_dma_read(dma, GC555_REG_INTERRUPT_STATUS, &pending);
	if (ret)
		goto rollback;
	if (pending & GC555_IRQ_AUDIO_COMPLETE) {
		ret = gc555_dma_write(dma, GC555_REG_INTERRUPT_STATUS,
					GC555_IRQ_AUDIO_COMPLETE);
		if (ret)
			goto rollback;
	}

	memset(audio->buffer[0], 0, GC555_AUDIO_BUFFER_CAPACITY);
	memset(audio->buffer[1], 0, GC555_AUDIO_BUFFER_CAPACITY);
	audio->buffer_bytes = buffer_bytes;

	ret = gc555_dma_write(dma, GC555_REG_AUDIO_FORMAT,
			      (channels == GC555_AUDIO_CHANNELS_7_1 ? 2 : 0) |
			      (sample_bits == GC555_AUDIO_SAMPLE_BITS_24 ? 1 : 0));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_MAP0, 0);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_MAP1, 1);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_MAP2, 2);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_MAP3, 3);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_BUFFER_DWORDS,
			      buffer_bytes / sizeof(u32));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_RATE,
			      rate_hz / 100U * period_units);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_BUFFER0_LOW,
			      lower_32_bits(audio->buffer_dma[0]));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_BUFFER0_HIGH,
			      upper_32_bits(audio->buffer_dma[0]));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_BUFFER1_LOW,
			      lower_32_bits(audio->buffer_dma[1]));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_BUFFER1_HIGH,
			      upper_32_bits(audio->buffer_dma[1]));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_AUDIO_BUFFER_CONTROL, 0);
	if (ret)
		goto rollback;

	dma_wmb();
	ret = gc555_dma_update_bits(dma, GC555_REG_INTERRUPT_ENABLE,
				    GC555_IRQ_AUDIO_COMPLETE,
				    GC555_IRQ_AUDIO_COMPLETE);
	if (ret)
		goto rollback;

	/* Completion IRQ must remain armed for 100 ms before stream enable. */
	msleep(100);
	spin_lock_irqsave(&dma->state_lock, flags);
	audio->done_head = 0;
	audio->done_tail = 0;
	audio->work_scheduled = false;
	audio->data = data;
	audio->data_context = data_context;
	audio->streaming = true;
	spin_unlock_irqrestore(&dma->state_lock, flags);

	ret = gc555_dma_update_bits(dma, GC555_REG_AUDIO_STREAM_CONTROL,
				    GC555_AUDIO_STREAM_ENABLE,
				    GC555_AUDIO_STREAM_ENABLE);
	if (ret)
		goto rollback;

	mutex_unlock(&dma->control_lock);
	return 0;

rollback:
	if (setup_started)
		gc555_dma_quiesce_audio_locked(audio, true);
unlock:
	mutex_unlock(&dma->control_lock);
	return ret;
}

void gc555_dma_stop_audio(struct gc555_dev *gc555, void *data_context)
{
	struct gc555_audio_dma *audio;
	struct gc555_dma *dma;
	unsigned long flags;
	bool owned;

	if (!gc555 || !gc555->dma || !data_context)
		return;

	dma = gc555->dma;
	audio = &dma->hdmi_audio;
	mutex_lock(&dma->control_lock);
	spin_lock_irqsave(&dma->state_lock, flags);
	owned = audio->streaming && audio->data_context == data_context;
	spin_unlock_irqrestore(&dma->state_lock, flags);
	if (owned)
		gc555_dma_quiesce_audio_locked(audio, true);
	mutex_unlock(&dma->control_lock);
}

int gc555_dma_start_line_audio(struct gc555_dev *gc555,
			       gc555_audio_data_t data, void *data_context)
{
	struct gc555_audio_dma *audio;
	struct gc555_dma *dma;
	unsigned long flags;
	u32 pending;
	bool setup_started = false;
	int ret;

	if (!gc555 || !gc555->dma)
		return -ENODEV;
	if (!data || !data_context)
		return -EINVAL;

	dma = gc555->dma;
	audio = &dma->line_audio;

	mutex_lock(&dma->control_lock);
	spin_lock_irqsave(&dma->state_lock, flags);
	if (audio->streaming) {
		spin_unlock_irqrestore(&dma->state_lock, flags);
		ret = -EBUSY;
		goto unlock;
	}
	spin_unlock_irqrestore(&dma->state_lock, flags);

	ret = gc555_dma_alloc_audio_buffers(audio);
	if (ret)
		goto unlock;

	setup_started = true;
	ret = gc555_dma_update_bits(dma, GC555_REG_AUDIO_STREAM_CONTROL,
				    audio->stream_enable_bit, 0);
	if (ret)
		goto rollback;
	ret = gc555_dma_update_bits(dma, GC555_REG_INTERRUPT_ENABLE,
				    audio->irq_bit, 0);
	if (ret)
		goto rollback;
	ret = gc555_dma_read(dma, GC555_REG_INTERRUPT_STATUS, &pending);
	if (ret)
		goto rollback;
	if (pending & audio->irq_bit) {
		ret = gc555_dma_write(dma, GC555_REG_INTERRUPT_STATUS,
				      audio->irq_bit);
		if (ret)
			goto rollback;
	}

	memset(audio->buffer[0], 0, GC555_AUDIO_BUFFER_CAPACITY);
	memset(audio->buffer[1], 0, GC555_AUDIO_BUFFER_CAPACITY);
	audio->buffer_bytes =
		GC555_AUDIO_RATE_48000_HZ / 100U *
		GC555_AUDIO_CHANNELS_STEREO * sizeof(__le16) *
		GC555_LINE_AUDIO_PERIODS;

	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_FORMAT, 0);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_CHANNELS, 0);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_SOURCE, 0x3ff);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_MAP, 0x76542310);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_DWORDS,
			      audio->buffer_bytes / sizeof(u32));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_RATE,
			      GC555_AUDIO_RATE_48000_HZ / 100U *
			      GC555_LINE_AUDIO_PERIODS);
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_BUFFER0_LOW,
			      lower_32_bits(audio->buffer_dma[0]));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_BUFFER0_HIGH,
			      upper_32_bits(audio->buffer_dma[0]));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_BUFFER1_LOW,
			      lower_32_bits(audio->buffer_dma[1]));
	if (ret)
		goto rollback;
	ret = gc555_dma_write(dma, GC555_REG_LINE_AUDIO_BUFFER1_HIGH,
			      upper_32_bits(audio->buffer_dma[1]));
	if (ret)
		goto rollback;

	dma_wmb();
	ret = gc555_dma_update_bits(dma, GC555_REG_INTERRUPT_ENABLE,
				    audio->irq_bit, audio->irq_bit);
	if (ret)
		goto rollback;

	/* Completion IRQ must remain armed for 100 ms before stream enable. */
	msleep(100);
	spin_lock_irqsave(&dma->state_lock, flags);
	audio->done_head = 0;
	audio->done_tail = 0;
	audio->work_scheduled = false;
	audio->data = data;
	audio->data_context = data_context;
	audio->streaming = true;
	spin_unlock_irqrestore(&dma->state_lock, flags);

	ret = gc555_dma_update_bits(dma, GC555_REG_AUDIO_STREAM_CONTROL,
				    audio->stream_enable_bit,
				    audio->stream_enable_bit);
	if (ret)
		goto rollback;

	mutex_unlock(&dma->control_lock);
	return 0;

rollback:
	if (setup_started)
		gc555_dma_quiesce_audio_locked(audio, true);
unlock:
	mutex_unlock(&dma->control_lock);
	return ret;
}

void gc555_dma_stop_line_audio(struct gc555_dev *gc555, void *data_context)
{
	struct gc555_audio_dma *audio;
	struct gc555_dma *dma;
	unsigned long flags;
	bool owned;

	if (!gc555 || !gc555->dma || !data_context)
		return;

	dma = gc555->dma;
	audio = &dma->line_audio;
	mutex_lock(&dma->control_lock);
	spin_lock_irqsave(&dma->state_lock, flags);
	owned = audio->streaming && audio->data_context == data_context;
	spin_unlock_irqrestore(&dma->state_lock, flags);
	if (owned)
		gc555_dma_quiesce_audio_locked(audio, true);
	mutex_unlock(&dma->control_lock);
}

int gc555_dma_register_video_irq(struct gc555_dev *gc555,
				 gc555_dma_video_irq_t handler,
				 void *context)
{
	struct gc555_dma *dma;
	unsigned long flags;
	int ret = 0;

	if (!gc555 || !gc555->dma)
		return -ENODEV;
	if (!handler || !context)
		return -EINVAL;

	dma = gc555->dma;
	mutex_lock(&dma->control_lock);
	spin_lock_irqsave(&dma->state_lock, flags);
	if (dma->video_irq) {
		ret = -EBUSY;
	} else {
		dma->video_irq = handler;
		dma->video_irq_context = context;
	}
	spin_unlock_irqrestore(&dma->state_lock, flags);
	mutex_unlock(&dma->control_lock);

	return ret;
}

void gc555_dma_unregister_video_irq(struct gc555_dev *gc555, void *context)
{
	struct gc555_dma *dma;
	unsigned long flags;
	bool removed = false;

	if (!gc555 || !gc555->dma || !context)
		return;

	dma = gc555->dma;
	mutex_lock(&dma->control_lock);
	spin_lock_irqsave(&dma->state_lock, flags);
	if (dma->video_irq_context == context) {
		dma->video_irq = NULL;
		dma->video_irq_context = NULL;
		removed = true;
	}
	spin_unlock_irqrestore(&dma->state_lock, flags);
	mutex_unlock(&dma->control_lock);

	if (removed && dma->irq_requested)
		synchronize_irq(dma->irq);
}

void gc555_dma_synchronize_irq(struct gc555_dev *gc555)
{
	if (gc555 && gc555->dma && gc555->dma->irq_requested)
		synchronize_irq(gc555->dma->irq);
}

static void gc555_dma_init_audio_engine(struct gc555_dma *dma,
					struct gc555_audio_dma *audio,
					const char *name, u32 irq_bit,
					u32 reset_bit, u32 stream_enable_bit,
					unsigned int channel_shift)
{
	audio->dma = dma;
	audio->name = name;
	audio->irq_bit = irq_bit;
	audio->reset_bit = reset_bit;
	audio->stream_enable_bit = stream_enable_bit;
	audio->channel_shift = channel_shift;
	INIT_WORK(&audio->work, gc555_dma_audio_work);
}

int gc555_dma_init(struct gc555_dev *gc555)
{
	struct gc555_dma *dma;
	unsigned long irq_flags;
	int ret;

	if (!gc555 || !gc555->pdev || !gc555_bridge_is_ready(gc555))
		return -EINVAL;
	if (gc555->dma)
		return 0;

	dma = kzalloc(sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;

	dma->gc555 = gc555;
	dma->irq = -1;
	mutex_init(&dma->control_lock);
	spin_lock_init(&dma->register_lock);
	spin_lock_init(&dma->state_lock);
	gc555_dma_init_audio_engine(dma, &dma->hdmi_audio, "HDMI audio",
				    GC555_IRQ_AUDIO_COMPLETE,
				    GC555_AUDIO_ENGINE_RESET,
				    GC555_AUDIO_STREAM_ENABLE, 0);
	gc555_dma_init_audio_engine(dma, &dma->line_audio, "line-in audio",
				    GC555_IRQ_LINE_AUDIO_COMPLETE,
				    GC555_LINE_AUDIO_ENGINE_RESET,
				    GC555_LINE_AUDIO_STREAM_ENABLE, 2);

	ret = pci_alloc_irq_vectors(gc555->pdev, 1, 1,
				    PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (ret < 0)
		goto free_dma;
	dma->irq_vectors_allocated = true;
	dma->irq = pci_irq_vector(gc555->pdev, 0);
	if (dma->irq < 0) {
		ret = dma->irq;
		goto free_vectors;
	}

	irq_flags = pci_dev_msi_enabled(gc555->pdev) ? 0 : IRQF_SHARED;
	ret = request_irq(dma->irq, gc555_dma_irq, irq_flags, "gc555", dma);
	if (ret)
		goto free_vectors;
	dma->irq_requested = true;
	gc555->dma = dma;

	dev_dbg(gc555->dev, "using %s interrupt vector %d\n",
		pci_dev_msi_enabled(gc555->pdev) ? "MSI" : "INTx", dma->irq);
	return 0;

free_vectors:
	pci_free_irq_vectors(gc555->pdev);
free_dma:
	kfree(dma);
	return dev_err_probe(gc555->dev, ret,
			     "failed to initialize DMA interrupt\n");
}

void gc555_dma_cleanup(struct gc555_dev *gc555)
{
	struct gc555_dma *dma;

	if (!gc555 || !gc555->dma)
		return;

	dma = gc555->dma;
	mutex_lock(&dma->control_lock);
	if (dma->hdmi_audio.buffer[0] || dma->hdmi_audio.buffer[1] ||
	    dma->hdmi_audio.streaming)
		gc555_dma_quiesce_audio_locked(&dma->hdmi_audio, true);
	if (dma->line_audio.buffer[0] || dma->line_audio.buffer[1] ||
	    dma->line_audio.streaming)
		gc555_dma_quiesce_audio_locked(&dma->line_audio, true);
	mutex_unlock(&dma->control_lock);

	if (dma->irq_requested)
		free_irq(dma->irq, dma);
	if (dma->irq_vectors_allocated)
		pci_free_irq_vectors(gc555->pdev);

	gc555_dma_free_audio_buffers(&dma->hdmi_audio);
	gc555_dma_free_audio_buffers(&dma->line_audio);
	gc555->dma = NULL;
	kfree(dma);
}

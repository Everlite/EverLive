// SPDX-License-Identifier: GPL-2.0-only

#include <linux/build_bug.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#include "gc573.h"

#define GC555_VIDEO_DMA_CHANNELS		4
#define GC555_VIDEO_DMA_MAX_DESCRIPTORS	2048U
#define GC555_VIDEO_DMA_DESCRIPTOR_BYTES	16U
#define GC555_VIDEO_DMA_TABLE_BYTES		\
	(GC555_VIDEO_DMA_MAX_DESCRIPTORS * GC555_VIDEO_DMA_DESCRIPTOR_BYTES)

#define GC555_VIDEO_DMA_INTERRUPT_STATUS	0x0010
#define GC555_VIDEO_DMA_INTERRUPT_ENABLE	0x001c
#define GC555_VIDEO_DMA_CONTROL			0x0304
#define GC555_VIDEO_DMA_TABLE_BASE		0x0308
#define GC555_VIDEO_DMA_CHROMA_CONTROL		0x0504
#define GC555_VIDEO_DMA_CHROMA_TABLE_BASE	0x0508
#define GC555_VIDEO_DMA_THIRD_CONTROL		0x0604
#define GC555_VIDEO_DMA_STREAM_CONTROL		0x1000

#define GC555_VIDEO_DMA_IRQ_TERMINATED	BIT(0)
#define GC555_VIDEO_DMA_IRQ_COMPLETE	BIT(1)
#define GC555_VIDEO_DMA_IRQ_MASK		\
	(GC555_VIDEO_DMA_IRQ_TERMINATED | GC555_VIDEO_DMA_IRQ_COMPLETE)
#define GC555_VIDEO_DMA_DESCRIPTOR_CONTROL	0x80008000U
#define GC555_VIDEO_DMA_STOP_TIMEOUT_MS		2000U
#define GC555_VIDEO_DMA_WATCHDOG_INTERVAL_MS	250U
/* Even a 24 Hz stream should make substantial progress inside this bound. */
#define GC555_VIDEO_DMA_STALL_TIMEOUT_MS		1000U

struct gc555_video_dma_descriptor {
	__le32 address_low;
	__le32 address_high;
	__le32 length_dwords;
	__le32 control;
};

static_assert(sizeof(struct gc555_video_dma_descriptor) ==
	      GC555_VIDEO_DMA_DESCRIPTOR_BYTES);

enum gc555_video_dma_buffer_state {
	GC555_VIDEO_DMA_BUFFER_PREPARED,
	GC555_VIDEO_DMA_BUFFER_PREPARING,
	GC555_VIDEO_DMA_BUFFER_READY,
	GC555_VIDEO_DMA_BUFFER_ACTIVE,
	GC555_VIDEO_DMA_BUFFER_DONE,
};

struct gc555_video_dma_buffer {
	struct list_head all_node;
	struct list_head queue_node;
	refcount_t references;
	void *cookie;
	struct gc555_video_dma_descriptor *descriptors;
	dma_addr_t descriptor_dma;
	unsigned int descriptor_count;
	struct gc555_video_dma_descriptor *chroma_descriptors;
	dma_addr_t chroma_descriptor_dma;
	unsigned int chroma_descriptor_count;
	enum gc555_video_format format;
	enum gc555_video_dma_buffer_state state;
	int channel;
	u64 completion_ns;
	enum gc555_video_dma_completion
	(*complete)(void *buffer, u64 completion_ns, void *context);
	void *complete_context;
	bool registered;
};

struct gc555_video_dma {
	struct gc555_dev *gc555;
	/* Serializes preparation and stream lifecycle operations. */
	struct mutex control_lock;
	/* Protects channel, queue, and completion state in IRQ context. */
	spinlock_t state_lock;
	struct list_head all_buffers;
	struct list_head ready_buffers;
	struct list_head done_buffers;
	struct gc555_video_dma_buffer *active[GC555_VIDEO_DMA_CHANNELS];
	struct workqueue_struct *completion_wq;
	struct work_struct completion_work;
	struct delayed_work watchdog_work;
	struct completion termination;
	gc555_video_dma_error_t error_handler;
	void *error_context;
	unsigned long last_progress;
	int last_channel;
	bool streaming;
	bool stopping;
	bool failed;
	bool recovery_requested;
	bool work_scheduled;
};

static bool gc555_video_dma_format_is_packed(enum gc555_video_format format)
{
	return format == GC555_VIDEO_FORMAT_YUYV ||
	       format == GC555_VIDEO_FORMAT_BGR24 ||
	       format == GC555_VIDEO_FORMAT_RGB32;
}

static bool gc555_video_dma_format_has_chroma(
					 enum gc555_video_format format)
{
	return format == GC555_VIDEO_FORMAT_NV12 ||
	       format == GC555_VIDEO_FORMAT_P010;
}

static struct gc555_video_dma_buffer *
gc555_video_dma_find_buffer_locked(struct gc555_video_dma *video_dma,
				   void *cookie)
{
	struct gc555_video_dma_buffer *buffer;

	list_for_each_entry(buffer, &video_dma->all_buffers, all_node) {
		if (buffer->cookie == cookie)
			return buffer;
	}

	return NULL;
}

static void gc555_video_dma_free_buffer(
				struct gc555_video_dma *video_dma,
				struct gc555_video_dma_buffer *buffer)
{
	if (!buffer)
		return;
	if (buffer->descriptors)
		dma_free_coherent(video_dma->gc555->dev,
				  GC555_VIDEO_DMA_TABLE_BYTES,
				  buffer->descriptors,
				  buffer->descriptor_dma);
	if (buffer->chroma_descriptors)
		dma_free_coherent(video_dma->gc555->dev,
				  GC555_VIDEO_DMA_TABLE_BYTES,
				  buffer->chroma_descriptors,
				  buffer->chroma_descriptor_dma);
	kfree(buffer);
}

static void gc555_video_dma_put_buffer(
				struct gc555_video_dma *video_dma,
				struct gc555_video_dma_buffer *buffer)
{
	if (refcount_dec_and_test(&buffer->references))
		gc555_video_dma_free_buffer(video_dma, buffer);
}

static int gc555_video_dma_build_table(
				struct gc555_video_dma_descriptor *descriptors,
				struct sg_table *sgt, u64 span_offset,
				u64 span_size, unsigned int *count)
{
	struct scatterlist *sg;
	u64 remaining = span_size;
	u64 skip = span_offset;
	unsigned int descriptor_count = 0;
	unsigned int index;

	if (!descriptors || !sgt || !sgt->sgl || !sgt->nents || !count ||
	    !span_size || (span_offset & 0x7) || (span_size & 0x7))
		return -EINVAL;

	memset(descriptors, 0, GC555_VIDEO_DMA_TABLE_BYTES);
	for_each_sgtable_dma_sg(sgt, sg, index) {
		u64 segment_size = sg_dma_len(sg);
		u64 address;
		u64 length;

		if (!segment_size)
			return -EINVAL;
		if (skip >= segment_size) {
			skip -= segment_size;
			continue;
		}

		address = sg_dma_address(sg) + skip;
		length = min(segment_size - skip, remaining);
		skip = 0;
		if (!length || (address & 0x3) || (length & 0x7))
			return -EINVAL;
		if (descriptor_count < GC555_VIDEO_DMA_MAX_DESCRIPTORS) {
			struct gc555_video_dma_descriptor *descriptor;

			descriptor = &descriptors[descriptor_count];
			descriptor->address_low =
				cpu_to_le32(lower_32_bits(address));
			descriptor->address_high =
				cpu_to_le32(upper_32_bits(address));
			descriptor->length_dwords =
				cpu_to_le32(length / sizeof(u32));
			descriptor->control =
				cpu_to_le32(GC555_VIDEO_DMA_DESCRIPTOR_CONTROL);
		}
		descriptor_count++;
		remaining -= length;
		if (!remaining)
			break;
	}
	if (remaining)
		return -EMSGSIZE;

	*count = descriptor_count;
	if (descriptor_count > GC555_VIDEO_DMA_MAX_DESCRIPTORS)
		return -E2BIG;

	return 0;
}

static int gc555_video_dma_next_channel_locked(
					struct gc555_video_dma *video_dma)
{
	int channel = (video_dma->last_channel + 1) %
		      GC555_VIDEO_DMA_CHANNELS;

	return video_dma->active[channel] ? -1 : channel;
}

static bool gc555_video_dma_has_active_locked(
					struct gc555_video_dma *video_dma)
{
	unsigned int channel;

	for (channel = 0; channel < GC555_VIDEO_DMA_CHANNELS; channel++) {
		if (video_dma->active[channel])
			return true;
	}

	return false;
}

static int gc555_video_dma_write_table(struct gc555_video_dma *video_dma,
				       u32 table_base, int channel,
				       dma_addr_t table_dma,
				       unsigned int descriptor_count)
{
	u32 table_reg = table_base + channel * 0x0c;
	int ret;

	ret = gc555_dma_write_register(video_dma->gc555, table_reg,
				       lower_32_bits(table_dma));
	if (!ret)
		ret = gc555_dma_write_register(video_dma->gc555,
					       table_reg + sizeof(u32),
					       upper_32_bits(table_dma));
	if (!ret)
		ret = gc555_dma_write_register(video_dma->gc555,
					       table_reg + 2 * sizeof(u32),
					       descriptor_count);

	return ret;
}

static void gc555_video_dma_clear_channel(struct gc555_video_dma *video_dma,
					  int channel, bool chroma)
{
	u32 control = chroma ? GC555_VIDEO_DMA_CHROMA_CONTROL :
			       GC555_VIDEO_DMA_CONTROL;

	gc555_dma_update_register_bits(video_dma->gc555, control,
				       BIT(channel + 1), 0);
}

static bool gc555_video_dma_submit_one_locked(
					struct gc555_video_dma *video_dma)
{
	struct gc555_video_dma_buffer *buffer;
	bool has_chroma;
	int previous_last_channel;
	int channel;
	int ret;

	if (list_empty(&video_dma->ready_buffers) ||
	    gc555_dma_device_lost(video_dma->gc555))
		return false;

	channel = gc555_video_dma_next_channel_locked(video_dma);
	if (channel < 0)
		return false;

	buffer = list_first_entry(&video_dma->ready_buffers,
				  struct gc555_video_dma_buffer, queue_node);
	list_del_init(&buffer->queue_node);
	buffer->state = GC555_VIDEO_DMA_BUFFER_ACTIVE;
	buffer->channel = channel;
	buffer->completion_ns = 0;
	video_dma->active[channel] = buffer;
	previous_last_channel = video_dma->last_channel;
	video_dma->last_channel = channel;
	has_chroma = gc555_video_dma_format_has_chroma(buffer->format);

	/* Publish descriptor contents before handing their addresses to DMA. */
	dma_wmb();
	ret = gc555_video_dma_write_table(video_dma,
					  GC555_VIDEO_DMA_TABLE_BASE, channel,
					  buffer->descriptor_dma,
					  buffer->descriptor_count);
	if (!ret && has_chroma)
		ret = gc555_video_dma_write_table(
			video_dma, GC555_VIDEO_DMA_CHROMA_TABLE_BASE, channel,
			buffer->chroma_descriptor_dma,
			buffer->chroma_descriptor_count);
	if (!ret)
		ret = gc555_dma_update_register_bits(
			video_dma->gc555, GC555_VIDEO_DMA_CONTROL,
			BIT(channel + 1), BIT(channel + 1));
	if (!ret && has_chroma)
		ret = gc555_dma_update_register_bits(
			video_dma->gc555, GC555_VIDEO_DMA_CHROMA_CONTROL,
			BIT(channel + 1), BIT(channel + 1));
	if (!ret)
		return true;

	if (!gc555_dma_device_lost(video_dma->gc555)) {
		gc555_dma_update_register_bits(video_dma->gc555,
					       GC555_VIDEO_DMA_CONTROL,
					       BIT(channel + 1), 0);
		if (has_chroma)
			gc555_dma_update_register_bits(
				video_dma->gc555,
				GC555_VIDEO_DMA_CHROMA_CONTROL,
				BIT(channel + 1), 0);
	}
	video_dma->active[channel] = NULL;
	video_dma->last_channel = previous_last_channel;
	buffer->state = GC555_VIDEO_DMA_BUFFER_READY;
	buffer->channel = -1;
	list_add(&buffer->queue_node, &video_dma->ready_buffers);
	return false;
}

static void gc555_video_dma_submit_ready_locked(
					struct gc555_video_dma *video_dma)
{
	while (gc555_video_dma_submit_one_locked(video_dma))
		;
}

static bool gc555_video_dma_request_work_locked(
					struct gc555_video_dma *video_dma)
{
	if (video_dma->stopping || video_dma->work_scheduled ||
	    gc555_dma_device_lost(video_dma->gc555))
		return false;

	video_dma->work_scheduled = true;
	return true;
}

static void
gc555_video_dma_schedule_watchdog(struct gc555_video_dma *video_dma)
{
	mod_delayed_work(system_wq, &video_dma->watchdog_work,
			 msecs_to_jiffies(GC555_VIDEO_DMA_WATCHDOG_INTERVAL_MS));
}

static void gc555_video_dma_completion_work(struct work_struct *work)
{
	struct gc555_video_dma *video_dma =
		container_of(work, struct gc555_video_dma, completion_work);

	for (;;) {
		struct gc555_video_dma_buffer *buffer;
		enum gc555_video_dma_completion
		(*complete_cb)(void *buffer, u64 completion_ns,
			       void *context);
		void *complete_context;
		u64 completion_ns;
		unsigned long flags;

		spin_lock_irqsave(&video_dma->state_lock, flags);
		if (list_empty(&video_dma->done_buffers)) {
			if (video_dma->streaming && !video_dma->stopping &&
			    !gc555_dma_device_lost(video_dma->gc555))
				gc555_video_dma_submit_ready_locked(video_dma);
			video_dma->work_scheduled = false;
			spin_unlock_irqrestore(&video_dma->state_lock, flags);
			return;
		}

		buffer = list_first_entry(&video_dma->done_buffers,
					  struct gc555_video_dma_buffer,
					  queue_node);
		list_del_init(&buffer->queue_node);
		buffer->state = GC555_VIDEO_DMA_BUFFER_PREPARED;
		buffer->channel = -1;
		completion_ns = buffer->completion_ns;
		buffer->completion_ns = 0;
		complete_cb = video_dma->stopping ? NULL : buffer->complete;
		complete_context = buffer->complete_context;
		buffer->complete = NULL;
		buffer->complete_context = NULL;
		/* The callback runs unlocked and may race buffer cleanup. */
		if (complete_cb)
			refcount_inc(&buffer->references);
		spin_unlock_irqrestore(&video_dma->state_lock, flags);

		if (complete_cb) {
			enum gc555_video_dma_completion action;

			action = complete_cb(buffer->cookie, completion_ns,
					     complete_context);
			spin_lock_irqsave(&video_dma->state_lock, flags);
			if (action == GC555_VIDEO_DMA_RECYCLE &&
			    buffer->registered && video_dma->streaming &&
			    !video_dma->stopping &&
			    !gc555_dma_device_lost(video_dma->gc555) &&
			    buffer->state == GC555_VIDEO_DMA_BUFFER_PREPARED &&
			    !buffer->complete &&
			    list_empty(&buffer->queue_node)) {
				buffer->complete = complete_cb;
				buffer->complete_context = complete_context;
				buffer->state = GC555_VIDEO_DMA_BUFFER_READY;
				list_add_tail(&buffer->queue_node,
					      &video_dma->ready_buffers);
				gc555_video_dma_submit_ready_locked(video_dma);
			}
			spin_unlock_irqrestore(&video_dma->state_lock, flags);
			gc555_video_dma_put_buffer(video_dma, buffer);
		}
	}
}

static void gc555_video_dma_irq(void *context, u32 status,
				u32 channel_status)
{
	struct gc555_video_dma *video_dma = context;
	unsigned long flags;
	bool schedule_completion = false;
	bool schedule_recovery = false;

	if (status & GC555_VIDEO_DMA_IRQ_COMPLETE) {
		u64 completion_ns = ktime_get_ns();
		u32 channel_code = channel_status & 0x7;
		int channel = channel_code ? channel_code - 1 : -1;

		spin_lock_irqsave(&video_dma->state_lock, flags);
		if (channel >= 0 && channel < GC555_VIDEO_DMA_CHANNELS) {
			struct gc555_video_dma_buffer *buffer =
				video_dma->active[channel];

			if (buffer) {
				video_dma->last_progress = jiffies;
				video_dma->active[channel] = NULL;
				gc555_video_dma_clear_channel(video_dma, channel, false);
				if (gc555_video_dma_format_has_chroma(buffer->format))
					gc555_video_dma_clear_channel(video_dma, channel, true);
				buffer->completion_ns = completion_ns;
				buffer->state = GC555_VIDEO_DMA_BUFFER_DONE;
				list_add_tail(&buffer->queue_node,
						      &video_dma->done_buffers);
				if (gc555_video_dma_request_work_locked(
								video_dma))
					schedule_completion = true;
				if (video_dma->streaming && !video_dma->stopping)
					gc555_video_dma_submit_ready_locked(video_dma);
			} else if (video_dma->streaming &&
				   !video_dma->stopping) {
				video_dma->recovery_requested = true;
				schedule_recovery = true;
			}
		} else if (video_dma->streaming && !video_dma->stopping) {
			video_dma->recovery_requested = true;
			schedule_recovery = true;
		}
		spin_unlock_irqrestore(&video_dma->state_lock, flags);
	}

	if (status & GC555_VIDEO_DMA_IRQ_TERMINATED)
		complete(&video_dma->termination);
	if (schedule_completion)
		queue_work(video_dma->completion_wq,
			   &video_dma->completion_work);
	if (schedule_recovery)
		mod_delayed_work(system_wq, &video_dma->watchdog_work, 0);
}

static void gc555_video_dma_return_buffers_locked(
					struct gc555_video_dma *video_dma)
{
	struct gc555_video_dma_buffer *buffer;
	unsigned int channel;

	for (channel = 0; channel < GC555_VIDEO_DMA_CHANNELS; channel++) {
		buffer = video_dma->active[channel];
		if (!buffer)
			continue;
		video_dma->active[channel] = NULL;
		buffer->state = GC555_VIDEO_DMA_BUFFER_PREPARED;
		buffer->channel = -1;
		buffer->completion_ns = 0;
		buffer->complete = NULL;
		buffer->complete_context = NULL;
	}

	while (!list_empty(&video_dma->ready_buffers)) {
		buffer = list_first_entry(&video_dma->ready_buffers,
					  struct gc555_video_dma_buffer,
					  queue_node);
		list_del_init(&buffer->queue_node);
		buffer->state = GC555_VIDEO_DMA_BUFFER_PREPARED;
		buffer->channel = -1;
		buffer->completion_ns = 0;
		buffer->complete = NULL;
		buffer->complete_context = NULL;
	}

	while (!list_empty(&video_dma->done_buffers)) {
		buffer = list_first_entry(&video_dma->done_buffers,
					  struct gc555_video_dma_buffer,
					  queue_node);
		list_del_init(&buffer->queue_node);
		buffer->state = GC555_VIDEO_DMA_BUFFER_PREPARED;
		buffer->channel = -1;
		buffer->completion_ns = 0;
		buffer->complete = NULL;
		buffer->complete_context = NULL;
	}
}

static void gc555_video_dma_clear_hardware(struct gc555_video_dma *video_dma)
{
	if (gc555_dma_device_lost(video_dma->gc555))
		return;

	gc555_dma_update_register_bits(video_dma->gc555,
				       GC555_VIDEO_DMA_CONTROL, 0x1f, 0);
	gc555_dma_update_register_bits(video_dma->gc555,
				       GC555_VIDEO_DMA_CHROMA_CONTROL, 0x1f, 0);
	gc555_dma_update_register_bits(video_dma->gc555,
				       GC555_VIDEO_DMA_THIRD_CONTROL, 0x1f, 0);
}

static bool
gc555_video_dma_wait_for_termination(struct gc555_video_dma *video_dma)
{
	struct gc555_dev *gc555 = video_dma->gc555;
	unsigned long timeout;
	const u32 status_reg = GC555_VIDEO_DMA_INTERRUPT_STATUS;
	u32 status;
	int ret;

	if (gc555_bridge_host_irq_routing_enabled(gc555))
		return wait_for_completion_timeout(&video_dma->termination,
			msecs_to_jiffies(GC555_VIDEO_DMA_STOP_TIMEOUT_MS));

	timeout = jiffies + msecs_to_jiffies(GC555_VIDEO_DMA_STOP_TIMEOUT_MS);
	do {
		ret = gc555_dma_read_register(gc555, status_reg, &status);
		if (ret)
			return false;
		if (status & GC555_VIDEO_DMA_IRQ_TERMINATED) {
			gc555_dma_write_register(gc555,
						 GC555_VIDEO_DMA_INTERRUPT_STATUS,
						 GC555_VIDEO_DMA_IRQ_TERMINATED);
			return true;
		}
		usleep_range(1000, 2000);
	} while (time_before(jiffies, timeout));

	return false;
}

static void gc555_video_dma_stop_locked(struct gc555_video_dma *video_dma,
					bool cancel_watchdog)
{
	unsigned long flags;
	u32 stream_control = 0;
	bool hardware_accessible;
	bool output_active = false;
	bool was_streaming;
	int ret;

	reinit_completion(&video_dma->termination);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	was_streaming = video_dma->streaming;
	video_dma->streaming = false;
	video_dma->stopping = true;
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	if (cancel_watchdog)
		cancel_delayed_work_sync(&video_dma->watchdog_work);

	hardware_accessible =
		!gc555_dma_read_register(video_dma->gc555,
					 GC555_VIDEO_DMA_STREAM_CONTROL,
					 &stream_control);
	if (was_streaming && hardware_accessible)
		output_active = stream_control & BIT(0);
	ret = hardware_accessible ?
		gc555_fpga_set_output_enabled(video_dma->gc555, false) :
		-ENODEV;
	if (was_streaming && output_active && !ret &&
	    !gc555_video_dma_wait_for_termination(video_dma))
		dev_warn(video_dma->gc555->dev,
			 "video DMA termination timed out\n");

	/*
	 * Watchdog stop: the VIP may already have been reset under a live
	 * DMA. Pulse it back out of reset so the next STREAMON can arm it.
	 */
	if (!cancel_watchdog && was_streaming &&
	    !gc555_dma_device_lost(video_dma->gc555))
		gc555_fpga_reset_vip(video_dma->gc555);

	if (hardware_accessible)
		gc555_dma_update_register_bits(
			video_dma->gc555, GC555_VIDEO_DMA_INTERRUPT_ENABLE,
			GC555_VIDEO_DMA_IRQ_MASK, 0);
	gc555_dma_synchronize_irq(video_dma->gc555);
	cancel_work_sync(&video_dma->completion_work);
	gc555_video_dma_clear_hardware(video_dma);
	if (was_streaming && !gc555_dma_device_lost(video_dma->gc555)) {
		ret = gc555_dma_reset_video(video_dma->gc555);
		if (ret && ret != -ENODEV)
			dev_warn(video_dma->gc555->dev,
				 "video DMA reset did not complete: %d\n", ret);
	}

	spin_lock_irqsave(&video_dma->state_lock, flags);
	video_dma->work_scheduled = false;
	gc555_video_dma_return_buffers_locked(video_dma);
	video_dma->last_channel = -1;
	video_dma->recovery_requested = false;
	video_dma->stopping = false;
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
}

static void gc555_video_dma_watchdog_work(struct work_struct *work)
{
	struct gc555_video_dma *video_dma;
	gc555_video_dma_error_t error_handler;
	void *error_context;
	unsigned long flags;
	bool recover;
	bool reschedule;

	video_dma = container_of(to_delayed_work(work),
				 struct gc555_video_dma, watchdog_work);
	if (!mutex_trylock(&video_dma->control_lock)) {
		gc555_video_dma_schedule_watchdog(video_dma);
		return;
	}

	spin_lock_irqsave(&video_dma->state_lock, flags);
	reschedule = video_dma->streaming && !video_dma->stopping &&
		     !gc555_dma_device_lost(video_dma->gc555);
	recover = reschedule &&
		  (video_dma->recovery_requested ||
		   (gc555_video_dma_has_active_locked(video_dma) &&
		    time_is_before_jiffies(video_dma->last_progress +
			msecs_to_jiffies(GC555_VIDEO_DMA_STALL_TIMEOUT_MS))));
	spin_unlock_irqrestore(&video_dma->state_lock, flags);

	if (!recover) {
		mutex_unlock(&video_dma->control_lock);
		if (reschedule)
			gc555_video_dma_schedule_watchdog(video_dma);
		return;
	}

	dev_warn(video_dma->gc555->dev,
		 "video DMA stopped after completion state became unreliable\n");
	gc555_video_dma_stop_locked(video_dma, false);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	video_dma->failed = true;
	error_handler = video_dma->error_handler;
	error_context = video_dma->error_context;
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	if (error_handler)
		error_handler(error_context);
	mutex_unlock(&video_dma->control_lock);
}

int gc555_video_dma_set_error_handler(struct gc555_dev *gc555,
				      gc555_video_dma_error_t handler,
				      void *context)
{
	struct gc555_video_dma *video_dma;
	unsigned long flags;

	if (!gc555 || !gc555->video_dma)
		return -ENODEV;
	if (!!handler != !!context)
		return -EINVAL;

	video_dma = gc555->video_dma;
	mutex_lock(&video_dma->control_lock);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	video_dma->error_handler = handler;
	video_dma->error_context = context;
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	mutex_unlock(&video_dma->control_lock);

	return 0;
}

int gc555_video_dma_prepare(struct gc555_dev *gc555, void *cookie,
			    struct sg_table *sgt,
			    enum gc555_video_format format,
			    size_t luma_size, size_t chroma_size)
{
	struct gc555_video_dma_buffer *buffer;
	struct gc555_video_dma *video_dma;
	unsigned long flags;
	size_t luma_transfer_size = luma_size;
	unsigned int luma_count = 0;
	unsigned int chroma_count = 0;
	bool new_buffer = false;
	int ret;

	if (!gc555 || !gc555->video_dma)
		return -ENODEV;
	if (!cookie || !sgt || !luma_size ||
	    (gc555_video_dma_format_has_chroma(format) && !chroma_size) ||
	    (gc555_video_dma_format_is_packed(format) && chroma_size) ||
	    (!gc555_video_dma_format_is_packed(format) &&
	     !gc555_video_dma_format_has_chroma(format)))
		return -EINVAL;
	if (format == GC555_VIDEO_FORMAT_P010) {
		if (luma_size & 0x3)
			return -EINVAL;
		/* Luma arrives as two packed 12-bit samples per three bytes. */
		luma_transfer_size = (luma_size / 4U) * 3U;
	}
	if (gc555_dma_device_lost(gc555))
		return -ENODEV;

	video_dma = gc555->video_dma;
	mutex_lock(&video_dma->control_lock);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	buffer = gc555_video_dma_find_buffer_locked(video_dma, cookie);
	if (buffer && buffer->state != GC555_VIDEO_DMA_BUFFER_PREPARED) {
		spin_unlock_irqrestore(&video_dma->state_lock, flags);
		ret = -EBUSY;
		goto unlock;
	}
	spin_unlock_irqrestore(&video_dma->state_lock, flags);

	if (!buffer) {
		buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
		if (!buffer) {
			ret = -ENOMEM;
			goto unlock;
		}
		INIT_LIST_HEAD(&buffer->all_node);
		INIT_LIST_HEAD(&buffer->queue_node);
		refcount_set(&buffer->references, 1);
		buffer->cookie = cookie;
		buffer->channel = -1;
		buffer->state = GC555_VIDEO_DMA_BUFFER_PREPARED;
		buffer->descriptors = dma_alloc_coherent(
			gc555->dev, GC555_VIDEO_DMA_TABLE_BYTES,
			&buffer->descriptor_dma, GFP_KERNEL);
		if (!buffer->descriptors) {
			kfree(buffer);
			ret = -ENOMEM;
			goto unlock;
		}
		new_buffer = true;
	}
	if (gc555_video_dma_format_has_chroma(format) &&
	    !buffer->chroma_descriptors) {
		buffer->chroma_descriptors = dma_alloc_coherent(
			gc555->dev, GC555_VIDEO_DMA_TABLE_BYTES,
			&buffer->chroma_descriptor_dma, GFP_KERNEL);
		if (!buffer->chroma_descriptors) {
			if (new_buffer)
				gc555_video_dma_free_buffer(video_dma, buffer);
			ret = -ENOMEM;
			goto unlock;
		}
	}
	if (new_buffer) {
		spin_lock_irqsave(&video_dma->state_lock, flags);
		buffer->registered = true;
		list_add_tail(&buffer->all_node, &video_dma->all_buffers);
		spin_unlock_irqrestore(&video_dma->state_lock, flags);
	}

	spin_lock_irqsave(&video_dma->state_lock, flags);
	buffer->state = GC555_VIDEO_DMA_BUFFER_PREPARING;
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	ret = gc555_video_dma_build_table(buffer->descriptors, sgt, 0,
					  luma_transfer_size, &luma_count);
	if (!ret && gc555_video_dma_format_has_chroma(format))
		ret = gc555_video_dma_build_table(
			buffer->chroma_descriptors, sgt, luma_size,
			chroma_size, &chroma_count);
	if (ret == -E2BIG) {
		if (luma_count > GC555_VIDEO_DMA_MAX_DESCRIPTORS)
			dev_warn_ratelimited(gc555->dev,
					     "luma DMA table needs %u descriptors; maximum is %u\n",
					     luma_count,
					     GC555_VIDEO_DMA_MAX_DESCRIPTORS);
		else
			dev_warn_ratelimited(gc555->dev,
					     "chroma DMA table needs %u descriptors; maximum is %u\n",
					     chroma_count,
					     GC555_VIDEO_DMA_MAX_DESCRIPTORS);
	}
	/* Publish coherent tables before exposing the prepared state. */
	if (!ret)
		dma_wmb();

	spin_lock_irqsave(&video_dma->state_lock, flags);
	buffer->state = GC555_VIDEO_DMA_BUFFER_PREPARED;
	if (ret) {
		buffer->descriptor_count = 0;
		buffer->chroma_descriptor_count = 0;
	} else {
		buffer->format = format;
		buffer->descriptor_count = luma_count;
		buffer->chroma_descriptor_count = chroma_count;
	}
	spin_unlock_irqrestore(&video_dma->state_lock, flags);

unlock:
	mutex_unlock(&video_dma->control_lock);
	return ret;
}

int gc555_video_dma_queue(struct gc555_dev *gc555, void *cookie,
			  enum gc555_video_dma_completion
			  (*complete_cb)(void *buffer, u64 completion_ns,
					 void *context),
			  void *complete_context)
{
	struct gc555_video_dma_buffer *buffer;
	struct gc555_video_dma *video_dma;
	unsigned long flags;
	bool schedule_completion = false;
	int ret = 0;

	if (!gc555 || !gc555->video_dma)
		return -ENODEV;
	if (!cookie || !complete_cb)
		return -EINVAL;

	video_dma = gc555->video_dma;
	mutex_lock(&video_dma->control_lock);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	buffer = gc555_video_dma_find_buffer_locked(video_dma, cookie);
	if (gc555_dma_device_lost(gc555)) {
		ret = -ENODEV;
	} else if (!buffer || !buffer->descriptor_count) {
		ret = -ENODATA;
	} else if (video_dma->failed) {
		ret = -EPIPE;
	} else if (buffer->state != GC555_VIDEO_DMA_BUFFER_PREPARED) {
		ret = -EBUSY;
	} else {
		buffer->complete = complete_cb;
		buffer->complete_context = complete_context;
		buffer->completion_ns = 0;
		buffer->state = GC555_VIDEO_DMA_BUFFER_READY;
		list_add_tail(&buffer->queue_node, &video_dma->ready_buffers);
		if (video_dma->streaming && !video_dma->stopping)
			schedule_completion =
				gc555_video_dma_request_work_locked(video_dma);
	}
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	mutex_unlock(&video_dma->control_lock);

	if (schedule_completion)
		queue_work(video_dma->completion_wq,
			   &video_dma->completion_work);
	return ret;
}

int gc555_video_dma_cleanup_buffer(struct gc555_dev *gc555, void *cookie)
{
	struct gc555_video_dma_buffer *buffer;
	struct gc555_video_dma *video_dma;
	unsigned long flags;
	int ret = 0;

	if (!gc555 || !gc555->video_dma)
		return -ENODEV;
	if (!cookie)
		return -EINVAL;

	video_dma = gc555->video_dma;
	mutex_lock(&video_dma->control_lock);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	buffer = gc555_video_dma_find_buffer_locked(video_dma, cookie);
	if (!buffer) {
		ret = -ENOENT;
	} else if (buffer->state != GC555_VIDEO_DMA_BUFFER_PREPARED) {
		ret = -EBUSY;
	} else {
		buffer->registered = false;
		list_del_init(&buffer->all_node);
	}
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	if (!ret)
		gc555_video_dma_put_buffer(video_dma, buffer);
	mutex_unlock(&video_dma->control_lock);

	return ret;
}

int gc555_video_dma_reset(struct gc555_dev *gc555)
{
	struct gc555_video_dma *video_dma;
	unsigned long flags;
	bool busy;
	int ret;

	if (!gc555 || !gc555->video_dma)
		return -ENODEV;

	video_dma = gc555->video_dma;
	mutex_lock(&video_dma->control_lock);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	busy = video_dma->streaming || video_dma->stopping;
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	ret = busy ? -EBUSY : gc555_dma_reset_video(gc555);
	mutex_unlock(&video_dma->control_lock);

	return ret;
}

int gc555_video_dma_start(struct gc555_dev *gc555)
{
	struct gc555_video_dma *video_dma;
	unsigned long flags;
	u32 pending;
	bool have_ready;
	int reset_ret;
	int ret;

	if (!gc555 || !gc555->video_dma)
		return -ENODEV;
	if (gc555_dma_device_lost(gc555))
		return -ENODEV;

	video_dma = gc555->video_dma;
	mutex_lock(&video_dma->control_lock);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	if (video_dma->streaming) {
		spin_unlock_irqrestore(&video_dma->state_lock, flags);
		mutex_unlock(&video_dma->control_lock);
		return 0;
	}
	/*
	 * start_streaming already reset the DMA engine and reprogrammed
	 * the VIP. The flag only recorded the previous stall.
	 */
	video_dma->failed = false;
	have_ready = !list_empty(&video_dma->ready_buffers);
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	if (!have_ready) {
		ret = -ENOBUFS;
		goto unlock;
	}

	ret = gc555_dma_update_register_bits(gc555,
					     GC555_VIDEO_DMA_INTERRUPT_ENABLE,
					     GC555_VIDEO_DMA_IRQ_MASK, 0);
	if (!ret)
		ret = gc555_dma_read_register(
			gc555, GC555_VIDEO_DMA_INTERRUPT_STATUS, &pending);
	if (!ret && (pending & GC555_VIDEO_DMA_IRQ_MASK))
		ret = gc555_dma_write_register(
			gc555, GC555_VIDEO_DMA_INTERRUPT_STATUS,
			pending & GC555_VIDEO_DMA_IRQ_MASK);
	if (!ret)
		ret = gc555_fpga_set_output_enabled(gc555, false);
	if (!ret)
		ret = gc555_dma_update_register_bits(
			gc555, GC555_VIDEO_DMA_CONTROL, 0x1f, 0);
	if (!ret)
		ret = gc555_dma_update_register_bits(
			gc555, GC555_VIDEO_DMA_CHROMA_CONTROL, 0x1f, 0);
	if (!ret)
		ret = gc555_dma_update_register_bits(
			gc555, GC555_VIDEO_DMA_THIRD_CONTROL, 0x1f, 0);
	if (!ret)
		ret = gc555_dma_update_register_bits(
			gc555, GC555_VIDEO_DMA_INTERRUPT_ENABLE,
			GC555_VIDEO_DMA_IRQ_MASK, GC555_VIDEO_DMA_IRQ_MASK);
	if (ret)
		goto rollback;

	reinit_completion(&video_dma->termination);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	video_dma->stopping = false;
	video_dma->streaming = true;
	video_dma->last_progress = jiffies;
	gc555_video_dma_submit_ready_locked(video_dma);
	if (gc555_dma_device_lost(gc555) ||
	    !gc555_video_dma_has_active_locked(video_dma)) {
		video_dma->streaming = false;
		gc555_video_dma_return_buffers_locked(video_dma);
		spin_unlock_irqrestore(&video_dma->state_lock, flags);
		ret = gc555_dma_device_lost(gc555) ? -ENODEV : -EIO;
		goto rollback;
	}
	spin_unlock_irqrestore(&video_dma->state_lock, flags);

	ret = gc555_dma_update_register_bits(gc555,
					     GC555_VIDEO_DMA_CONTROL,
					     BIT(0), BIT(0));
	if (!ret)
		ret = gc555_fpga_set_output_enabled(gc555, true);
	if (!ret) {
		gc555_video_dma_schedule_watchdog(video_dma);
		goto unlock;
	}

rollback:
	spin_lock_irqsave(&video_dma->state_lock, flags);
	video_dma->streaming = false;
	video_dma->stopping = true;
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	if (!gc555_dma_device_lost(gc555))
		gc555_fpga_set_output_enabled(gc555, false);
	gc555_dma_update_register_bits(gc555,
				       GC555_VIDEO_DMA_INTERRUPT_ENABLE,
				       GC555_VIDEO_DMA_IRQ_MASK, 0);
	gc555_dma_synchronize_irq(gc555);
	cancel_work_sync(&video_dma->completion_work);
	gc555_video_dma_clear_hardware(video_dma);
	if (!gc555_dma_device_lost(gc555)) {
		reset_ret = gc555_dma_reset_video(gc555);
		if (reset_ret && reset_ret != -ENODEV)
			dev_warn(gc555->dev,
				 "video DMA rollback reset failed: %d\n",
				 reset_ret);
	}
	spin_lock_irqsave(&video_dma->state_lock, flags);
	video_dma->work_scheduled = false;
	gc555_video_dma_return_buffers_locked(video_dma);
	video_dma->last_channel = -1;
	video_dma->stopping = false;
	spin_unlock_irqrestore(&video_dma->state_lock, flags);

unlock:
	mutex_unlock(&video_dma->control_lock);
	return ret;
}

void gc555_video_dma_stop(struct gc555_dev *gc555)
{
	struct gc555_video_dma *video_dma;
	unsigned long flags;

	if (!gc555 || !gc555->video_dma)
		return;

	video_dma = gc555->video_dma;
	mutex_lock(&video_dma->control_lock);
	gc555_video_dma_stop_locked(video_dma, true);
	spin_lock_irqsave(&video_dma->state_lock, flags);
	video_dma->failed = false;
	spin_unlock_irqrestore(&video_dma->state_lock, flags);
	mutex_unlock(&video_dma->control_lock);
}

int gc555_video_dma_init(struct gc555_dev *gc555)
{
	struct gc555_video_dma *video_dma;
	int ret;

	if (!gc555 || !gc555->dma)
		return -ENODEV;
	if (gc555->video_dma)
		return 0;

	video_dma = kzalloc(sizeof(*video_dma), GFP_KERNEL);
	if (!video_dma)
		return -ENOMEM;

	video_dma->gc555 = gc555;
	video_dma->last_channel = -1;
	mutex_init(&video_dma->control_lock);
	spin_lock_init(&video_dma->state_lock);
	INIT_LIST_HEAD(&video_dma->all_buffers);
	INIT_LIST_HEAD(&video_dma->ready_buffers);
	INIT_LIST_HEAD(&video_dma->done_buffers);
	INIT_WORK(&video_dma->completion_work,
		  gc555_video_dma_completion_work);
	INIT_DELAYED_WORK(&video_dma->watchdog_work,
			  gc555_video_dma_watchdog_work);
	init_completion(&video_dma->termination);

	video_dma->completion_wq = alloc_ordered_workqueue("gc573_dma",
							   WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!video_dma->completion_wq) {
		kfree(video_dma);
		return -ENOMEM;
	}

	ret = gc555_dma_register_video_irq(gc555, gc555_video_dma_irq,
					  video_dma);
	if (ret) {
		destroy_workqueue(video_dma->completion_wq);
		kfree(video_dma);
		return ret;
	}
	gc555->video_dma = video_dma;

	return 0;
}

void gc555_video_dma_cleanup(struct gc555_dev *gc555)
{
	struct gc555_video_dma_buffer *buffer;
	struct gc555_video_dma_buffer *next;
	struct gc555_video_dma *video_dma;

	if (!gc555 || !gc555->video_dma)
		return;

	video_dma = gc555->video_dma;
	gc555_video_dma_stop(gc555);
	gc555_dma_unregister_video_irq(gc555, video_dma);
	if (video_dma->completion_wq)
		destroy_workqueue(video_dma->completion_wq);
	list_for_each_entry_safe(buffer, next, &video_dma->all_buffers,
				 all_node) {
		buffer->registered = false;
		list_del_init(&buffer->all_node);
		gc555_video_dma_put_buffer(video_dma, buffer);
	}
	gc555->video_dma = NULL;
	kfree(video_dma);
}

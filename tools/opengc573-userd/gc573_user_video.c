/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_video.c — Userspace VFIO video DMA ring for the GC573
 *
 * Mirrors the descriptor/channel handling of open_gc573/gc573-video-dma.c,
 * but keeps one descriptor per frame buffer and runs the completion loop in
 * userspace.
 */

#include "gc573_user_video.h"

#include <time.h>

#define GC573_USER_VIDEO_PAGE_SIZE	4096U
/*
 * The frame IOVA is one contiguous mapping. A 4 KB descriptor list is
 * ~1800 entries at 1440p and the engine cannot finish a frame inside an
 * 8 ms period, so 120 Hz input comes out near 60. 128 KB stays inside a
 * 16-bit dword length and cuts the list to a few dozen entries.
 */
#define GC573_USER_VIDEO_DESC_CHUNK	(128U * 1024U)
#define GC573_USER_VIDEO_MAX_DESCRIPTORS 2048U
#define GC573_USER_VIDEO_STALL_MS	2000U
#define GC573_USER_VIDEO_STOP_WAIT_MS	2000U

static uint64_t gc573_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint64_t gc573_now_ms(void)
{
	return gc573_now_us() / 1000;
}

static void gc573_video_irq_ack(void *bar0, uint32_t status)
{
	uint32_t ack = status & GC573_DMA_IRQ_VIDEO_ACK_MASK;

	if (ack)
		writel(ack, (u8 *)bar0 + GC573_DMA_INTERRUPT_STATUS);
}

static void gc573_video_clear_controls(void *bar0)
{
	uint32_t value;

	value = readl((u8 *)bar0 + GC573_DMA_CONTROL);
	writel(value & ~0x1fU, (u8 *)bar0 + GC573_DMA_CONTROL);
	value = readl((u8 *)bar0 + GC573_DMA_CHROMA_CONTROL);
	writel(value & ~0x1fU, (u8 *)bar0 + GC573_DMA_CHROMA_CONTROL);
	value = readl((u8 *)bar0 + GC573_DMA_THIRD_CONTROL);
	writel(value & ~0x1fU, (u8 *)bar0 + GC573_DMA_THIRD_CONTROL);
}

static int gc573_video_channel_idle(const struct gc573_user_video *video,
				    unsigned int channel)
{
	return video->channel_buf[channel] < 0;
}

static void gc573_video_arm_channel(struct gc573_user_video *video,
				    unsigned int channel, int index)
{
	struct gc573_user_video_buffer *buffer = &video->buffers[index];
	uint32_t table = GC573_DMA_TABLE_BASE + channel * 0x0c;
	uint32_t control;
	size_t remaining = video->frame_bytes;
	uint64_t curr_iova = buffer->iova;
	unsigned int desc_count = 0;

	while (remaining > 0 && desc_count < GC573_USER_VIDEO_MAX_DESCRIPTORS) {
		size_t chunk = remaining > GC573_USER_VIDEO_DESC_CHUNK ?
			       GC573_USER_VIDEO_DESC_CHUNK : remaining;

		buffer->desc[desc_count].address_low = (uint32_t)curr_iova;
		buffer->desc[desc_count].address_high = (uint32_t)(curr_iova >> 32);
		buffer->desc[desc_count].length_dwords = chunk / sizeof(uint32_t);
		buffer->desc[desc_count].control = GC573_DMA_DESCRIPTOR_CONTROL;

		curr_iova += chunk;
		remaining -= chunk;
		desc_count++;
	}

	/* Publish descriptor contents before the DMA engine sees them. */
	__sync_synchronize();

	writel((uint32_t)buffer->desc_iova, (u8 *)video->bar0 + table);
	writel((uint32_t)(buffer->desc_iova >> 32), (u8 *)video->bar0 + table + 4);
	writel(desc_count, (u8 *)video->bar0 + table + 8);

	control = readl((u8 *)video->bar0 + GC573_DMA_CONTROL);
	writel(control | BIT(channel + 1),
	       (u8 *)video->bar0 + GC573_DMA_CONTROL);

	video->channel_buf[channel] = index;
	buffer->channel = (int)channel;
}

static void gc573_video_arm_idle_channels(struct gc573_user_video *video)
{
	for (unsigned int channel = 0;
	     channel < video->num_channels && video->free_count;
	     channel++) {
		int index;

		if (!gc573_video_channel_idle(video, channel))
			continue;

		index = video->free_stack[--video->free_count];
		gc573_video_arm_channel(video, channel, index);
	}
}

static void gc573_video_push_ready(struct gc573_user_video *video, int index)
{
	if (video->ready_count >= video->num_buffers)
		return;

	video->ready_queue[video->ready_tail] = index;
	video->ready_tail = (video->ready_tail + 1) % video->num_buffers;
	video->ready_count++;
}

static int gc573_video_pop_ready(struct gc573_user_video *video)
{
	int index;

	if (!video->ready_count)
		return -1;

	index = video->ready_queue[video->ready_head];
	video->ready_head = (video->ready_head + 1) % video->num_buffers;
	video->ready_count--;
	return index;
}

static void gc573_video_retire(struct gc573_user_video *video,
			       unsigned int channel, int index)
{
	uint32_t control = readl((u8 *)video->bar0 + GC573_DMA_CONTROL);

	writel(control & ~BIT(channel + 1),
	       (u8 *)video->bar0 + GC573_DMA_CONTROL);
	video->channel_buf[channel] = -1;
	video->buffers[index].channel = -1;
	video->last_complete_ms = gc573_now_ms();
	video->frame_ready_us[index] = gc573_now_us();
	__atomic_fetch_add(&video->frames, 1, __ATOMIC_RELAXED);
	gc573_video_push_ready(video, index);
}

static void gc573_video_process_irq(struct gc573_user_video *video)
{
	void *bar0 = video->bar0;

	/*
	 * Serialize the read/acknowledge pair with the audio worker.  The
	 * status register is write-one-to-clear and shared by both engines;
	 * without the lock one thread can clear a completion the other has not
	 * consumed yet.
	 */
	gc573_vfio_irq_lock(video->vfio);

	for (unsigned int pass = 0; pass < 8; pass++) {
		uint32_t status = readl((u8 *)bar0 + GC573_DMA_INTERRUPT_STATUS);
		uint32_t ack = status & GC573_DMA_IRQ_VIDEO_ACK_MASK;

		if (!ack)
			break;

		if (status & GC573_DMA_IRQ_COMPLETE) {
			uint32_t channel_status;
			uint32_t code;

			channel_status = readl((u8 *)bar0 +
					       GC573_DMA_VIDEO_CHANNEL);
			code = channel_status & 0x7;
			if (code >= 1 && code <= video->num_channels) {
				unsigned int channel = code - 1;
				int index = video->channel_buf[channel];

				if (index >= 0)
					gc573_video_retire(video, channel,
							   index);
			}
		}

		/*
		 * Acknowledge exactly the bits sampled above.  BIT(5) belongs to
		 * the audio engine and is excluded from this mask.  Channels are
		 * retired only for a real completion IRQ with a valid channel
		 * code; GC573_DMA_CONTROL is never used as a completion oracle.
		 */
		writel(ack, (u8 *)bar0 + GC573_DMA_INTERRUPT_STATUS);
	}

	gc573_vfio_irq_unlock(video->vfio);
}

static int gc573_video_process_ready(struct gc573_user_video *video)
{
	int index;

	gc573_video_process_irq(video);
	gc573_video_arm_idle_channels(video);

	index = gc573_video_pop_ready(video);
	return index;
}

int gc573_user_video_init(struct gc573_user_video *video,
			  struct gc573_vfio_ctx *vfio, void *bar0,
			  unsigned int width, unsigned int height,
			  unsigned int num_buffers)
{
	size_t frame_bytes;
	size_t map_size;
	int ret = 0;

	if (!video || !vfio || !bar0)
		return -EINVAL;
	if (!width || !height || (width & 1))
		return -EINVAL;
	if (num_buffers < 1 || num_buffers > GC573_USER_VIDEO_MAX_BUFFERS)
		return -EINVAL;

	memset(video, 0, sizeof(*video));
	video->vfio = vfio;
	video->bar0 = bar0;
	video->width = width;
	video->height = height;
	video->num_buffers = num_buffers;
	video->num_channels = num_buffers > 1 ? num_buffers - 1 : 1;
	if (video->num_channels > GC573_USER_VIDEO_MAX_CHANNELS)
		video->num_channels = GC573_USER_VIDEO_MAX_CHANNELS;

	frame_bytes = (size_t)width * height * 2;
	map_size = (frame_bytes + GC573_USER_VIDEO_PAGE_SIZE - 1) &
		   ~(size_t)(GC573_USER_VIDEO_PAGE_SIZE - 1);
	video->frame_bytes = (unsigned int)frame_bytes;

	for (unsigned int i = 0; i < num_buffers; i++) {
		struct gc573_user_video_buffer *buffer = &video->buffers[i];
		uint64_t desc_iova = GC573_USER_VIDEO_DESC_IOVA +
				     (uint64_t)i * GC573_USER_VIDEO_DESC_TABLE_SIZE;
		uint64_t frame_iova = GC573_USER_VIDEO_FRAME_IOVA +
				      (uint64_t)i * map_size;

		buffer->channel = -1;
		buffer->map_size = map_size;
		buffer->iova = frame_iova;
		buffer->desc_iova = desc_iova;

		buffer->vaddr = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
				     -1, 0);
		if (buffer->vaddr == MAP_FAILED) {
			buffer->vaddr = NULL;
			ret = -errno;
			break;
		}

		buffer->desc = mmap(NULL, GC573_USER_VIDEO_DESC_TABLE_SIZE,
				    PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
				    -1, 0);
		if (buffer->desc == MAP_FAILED) {
			buffer->desc = NULL;
			ret = -errno;
			break;
		}
		memset(buffer->desc, 0, GC573_USER_VIDEO_DESC_TABLE_SIZE);

		ret = gc573_vfio_map_dma(vfio, buffer->vaddr, map_size,
					 frame_iova);
		if (ret < 0)
			break;
		buffer->mapped = true;

		ret = gc573_vfio_map_dma(vfio, buffer->desc,
					 GC573_USER_VIDEO_DESC_TABLE_SIZE,
					 desc_iova);
		if (ret < 0)
			break;
		buffer->desc_mapped = true;
	}

	if (ret < 0) {
		gc573_user_video_cleanup(video);
		return ret;
	}

	for (unsigned int i = 0; i < num_buffers; i++)
		video->free_stack[i] = (int)i;
	video->free_count = num_buffers;

	for (unsigned int i = 0; i < num_buffers; i++)
		video->frame_ready_us[i] = 0;

	for (unsigned int i = 0; i < video->num_channels; i++)
		video->channel_buf[i] = -1;

	pr_info("Video DMA: %ux%u YUYV, %u buffers, %u channels, %zu KB/frame",
		width, height, num_buffers, video->num_channels,
		frame_bytes / 1024);

	return 0;
}

int gc573_user_video_start(struct gc573_user_video *video)
{
	uint32_t pending;
	uint32_t control;

	if (!video || !video->bar0)
		return -EINVAL;
	if (video->streaming)
		return 0;

	/* Make sure the VIP is not pushing pixels while we arm the ring. */
	gc573_user_fpga_set_output_enabled(video->bar0, false);

	gc573_video_clear_controls(video->bar0);

	gc573_vfio_irq_lock(video->vfio);
	pending = readl((u8 *)video->bar0 + GC573_DMA_INTERRUPT_STATUS);
	if (pending & GC573_DMA_IRQ_VIDEO_ACK_MASK)
		writel(pending & GC573_DMA_IRQ_VIDEO_ACK_MASK,
		       (u8 *)video->bar0 + GC573_DMA_INTERRUPT_STATUS);

	control = readl((u8 *)video->bar0 + GC573_DMA_INTERRUPT_ENABLE);
	writel(control | GC573_DMA_IRQ_MASK,
	       (u8 *)video->bar0 + GC573_DMA_INTERRUPT_ENABLE);
	gc573_vfio_irq_unlock(video->vfio);

	for (unsigned int i = 0; i < video->num_buffers; i++) {
		video->buffers[i].channel = -1;
		video->free_stack[i] = (int)i;
	}
	video->free_count = video->num_buffers;
	video->ready_head = 0;
	video->ready_tail = 0;
	video->ready_count = 0;
	video->frames = 0;
	video->last_complete_ms = gc573_now_ms();

	for (unsigned int i = 0; i < video->num_buffers; i++)
		video->frame_ready_us[i] = 0;

	for (unsigned int i = 0; i < video->num_channels; i++)
		video->channel_buf[i] = -1;

	gc573_video_arm_idle_channels(video);
	if (video->free_count == video->num_buffers) {
		pr_err("Video DMA: no channel could be armed");
		return -EIO;
	}

	control = readl((u8 *)video->bar0 + GC573_DMA_CONTROL);
	writel(control | BIT(0), (u8 *)video->bar0 + GC573_DMA_CONTROL);

	video->streaming = true;
	return gc573_user_fpga_set_output_enabled(video->bar0, true);
}

int gc573_user_video_poll(struct gc573_user_video *video, int timeout_ms)
{
	struct pollfd pfd;
	int index;
	int ret;

	if (!video || !video->bar0)
		return -EINVAL;

	if (!video->streaming)
		return -1;

	index = gc573_video_process_ready(video);
	if (index >= 0)
		return index;

	if (video->vfio->irq_efd >= 0) {
		pfd.fd = video->vfio->irq_efd;
		pfd.events = POLLIN;
		ret = poll(&pfd, 1, timeout_ms);
		if (ret > 0 && (pfd.revents & POLLIN)) {
			uint64_t count;

			if (read(video->vfio->irq_efd, &count, sizeof(count)) < 0 &&
			    errno != EAGAIN)
				pr_warn("Video DMA: eventfd read failed: %s",
					strerror(errno));
		}
	} else if (timeout_ms > 0) {
		/* No MSI/INTx: fall back to polling the IRQ status register. */
		poll(NULL, 0, timeout_ms);
	}

	index = gc573_video_process_ready(video);
	if (index >= 0)
		return index;

	if (video->streaming &&
	    gc573_now_ms() - video->last_complete_ms > GC573_USER_VIDEO_STALL_MS)
		return -2;

	return -1;
}

void gc573_user_video_release(struct gc573_user_video *video, int index)
{
	if (!video || index < 0 || index >= (int)video->num_buffers)
		return;

	if (video->free_count >= video->num_buffers)
		return;

	video->free_stack[video->free_count++] = index;
	if (video->streaming)
		gc573_video_arm_idle_channels(video);
}

uint64_t gc573_user_video_frame_time_us(const struct gc573_user_video *video,
					int index)
{
	if (!video || index < 0 || index >= (int)video->num_buffers)
		return 0;

	return video->frame_ready_us[index];
}

void gc573_user_video_stop_timeout(struct gc573_user_video *video,
				   unsigned int timeout_ms)
{
	uint32_t status = 0;
	uint32_t control;
	bool terminated = false;
	bool was_streaming;

	if (!video || !video->bar0)
		return;

	if (!timeout_ms)
		timeout_ms = GC573_USER_VIDEO_STOP_WAIT_MS;

	was_streaming = video->streaming;
	video->streaming = false;
	/*
	 * Mirror the kernel stop order: stop the VIP output first.  The DMA
	 * engine only reports TERMINATED after the pixel source is quiet;
	 * waiting with the VIP still enabled always times out, and unmapping
	 * the IOMMU buffers while the engine is still fetching descriptors
	 * makes the FPGA fall back to its red-blinking default.
	 */
	if (was_streaming) {
		gc573_user_fpga_set_output_enabled(video->bar0, false);
		for (unsigned int elapsed = 0; elapsed < timeout_ms; elapsed++) {
			status = readl((u8 *)video->bar0 +
				       GC573_DMA_INTERRUPT_STATUS);
			if (status & GC573_DMA_IRQ_TERMINATED) {
				terminated = true;
				break;
			}
			usleep_range(1000, 2000);
		}
		if (!terminated) {
			status = readl((u8 *)video->bar0 +
				       GC573_DMA_INTERRUPT_STATUS);
			terminated = (status & GC573_DMA_IRQ_TERMINATED) != 0;
		}
		if (!terminated)
			pr_warn("Video DMA: termination timed out after %u ms",
				timeout_ms);
		else
			pr_info("Video DMA: terminated cleanly");
	}
	if (status) {
		gc573_vfio_irq_lock(video->vfio);
		gc573_video_irq_ack(video->bar0, status);
		gc573_vfio_irq_unlock(video->vfio);
	}

	gc573_vfio_irq_lock(video->vfio);
	control = readl((u8 *)video->bar0 + GC573_DMA_INTERRUPT_ENABLE);
	writel(control & ~GC573_DMA_IRQ_MASK,
	       (u8 *)video->bar0 + GC573_DMA_INTERRUPT_ENABLE);
	gc573_vfio_irq_unlock(video->vfio);

	gc573_video_clear_controls(video->bar0);

	for (unsigned int i = 0; i < video->num_channels; i++)
		video->channel_buf[i] = -1;
	for (unsigned int i = 0; i < video->num_buffers; i++) {
		video->buffers[i].channel = -1;
		video->free_stack[i] = (int)i;
		video->frame_ready_us[i] = 0;
	}
	video->free_count = video->num_buffers;
	video->ready_head = 0;
	video->ready_tail = 0;
	video->ready_count = 0;
}

void gc573_user_video_stop(struct gc573_user_video *video)
{
	gc573_user_video_stop_timeout(video, GC573_USER_VIDEO_STOP_WAIT_MS);
}

void gc573_user_video_unmap(struct gc573_user_video *video)
{
	if (!video)
		return;

	for (unsigned int i = 0; i < GC573_USER_VIDEO_MAX_BUFFERS; i++) {
		struct gc573_user_video_buffer *buffer = &video->buffers[i];

		if (buffer->desc) {
			if (buffer->desc_mapped)
				gc573_vfio_unmap_dma(video->vfio,
						     buffer->desc_iova,
						     GC573_USER_VIDEO_DESC_TABLE_SIZE);
			munmap(buffer->desc, GC573_USER_VIDEO_DESC_TABLE_SIZE);
			buffer->desc = NULL;
			buffer->desc_mapped = false;
		}
		if (buffer->vaddr) {
			if (buffer->mapped)
				gc573_vfio_unmap_dma(video->vfio,
						     buffer->iova,
						     buffer->map_size);
			munmap(buffer->vaddr, buffer->map_size);
			buffer->vaddr = NULL;
			buffer->mapped = false;
		}
	}
}

void gc573_user_video_cleanup(struct gc573_user_video *video)
{
	if (!video)
		return;

	gc573_user_video_stop(video);
	gc573_user_video_unmap(video);
}

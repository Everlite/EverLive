// SPDX-License-Identifier: GPL-2.0-only
/*
 * V4L2 capture-device registration, format negotiation, videobuf2 queues, and
 * streaming control for both HDMI inputs.
 */
#include "gc570d.h"
#include "../data/gc570d_no_signal_640x360.inc"

#define GC570D_HDMI2_LINK_POLL_NS       (250ULL * NSEC_PER_MSEC)
#define GC570D_HDMI2_HPD_DELAY_POLLS    4
#define GC570D_HDMI2_HPD_RETRY_NS       (5ULL * NSEC_PER_SEC)
#define GC570D_HDMI2_SCDT_STABLE_POLLS  2
/* Preserve the proven tolerance for transient userspace/descriptor pauses. */
#define GC570D_HDMI2_DMA_TIMEOUT_MS      2000
#define GC570D_HDMI2_DMA_POLL_MS         50
#define GC570D_HDMI1_LINK_POLL_NS        (250ULL * NSEC_PER_MSEC)
#define GC570D_HDMI1_STABLE_POLLS        2
#define GC570D_HDMI1_DMA_TIMEOUT_MS      1000
#define GC570D_HDMI1_DMA_POLL_MS         50

static bool gc570d_video_is_no_signal_error(int ret)
{
	return ret == -ENOLINK || ret == -ENODATA || ret == -ERANGE ||
	       ret == -EINVAL || ret == -EILSEQ || ret == -EOPNOTSUPP;
}

static u8 *gc570d_no_signal_frame_create(u16 width, u16 height)
{
	const u32 source_width = GC570D_NO_SIGNAL_WIDTH;
	const u32 source_height = GC570D_NO_SIGNAL_HEIGHT;
	u32 scaled_width;
	u32 scaled_height;
	u32 x_offset;
	u32 y_offset;
	size_t frame_size;
	u8 *frame;
	u32 x;
	u32 y;

	if (!width || !height || (width & 1) ||
	    gc570d_no_signal_yuyv_len != source_width * source_height * 2)
		return ERR_PTR(-EINVAL);

	frame_size = (size_t)width * height * 2;
	frame = kvzalloc(frame_size, GFP_KERNEL);
	if (!frame)
		return ERR_PTR(-ENOMEM);

	/* Limited-range Rec.709 black for the letterbox/pillarbox area. */
	for (y = 0; y < height; y++) {
		u8 *line = frame + (size_t)y * width * 2;

		for (x = 0; x < width; x += 2) {
			line[x * 2] = 16;
			line[x * 2 + 1] = 128;
			line[x * 2 + 2] = 16;
			line[x * 2 + 3] = 128;
		}
	}

	if ((u32)width * source_height <= (u32)height * source_width) {
		scaled_width = width;
		scaled_height = (u32)width * source_height / source_width;
	} else {
		scaled_height = height;
		scaled_width = (u32)height * source_width / source_height;
	}
	scaled_width &= ~1U;
	if (!scaled_width || !scaled_height) {
		kvfree(frame);
		return ERR_PTR(-EINVAL);
	}
	x_offset = ((width - scaled_width) / 2) & ~1U;
	y_offset = (height - scaled_height) / 2;

	/* Nearest-neighbour scaling preserves crisp edges in this status slate. */
	for (y = 0; y < scaled_height; y++) {
		u32 source_y = y * source_height / scaled_height;
		const u8 *source = gc570d_no_signal_yuyv +
			(size_t)source_y * source_width * 2;
		u8 *destination = frame +
			(size_t)(y + y_offset) * width * 2 + x_offset * 2;

		for (x = 0; x < scaled_width; x += 2) {
			u32 source_x0 = x * source_width / scaled_width;
			u32 source_x1 = (x + 1) * source_width /
				scaled_width;
			u32 chroma_x = ((source_x0 + source_x1) / 2) & ~1U;
			u32 source_pair0 = (source_x0 & ~1U) * 2;
			u32 source_pair1 = (source_x1 & ~1U) * 2;
			u32 chroma_pair = chroma_x * 2;

			destination[x * 2] =
				source[source_pair0 + (source_x0 & 1 ? 2 : 0)];
			destination[x * 2 + 1] = source[chroma_pair + 1];
			destination[x * 2 + 2] =
				source[source_pair1 + (source_x1 & 1 ? 2 : 0)];
			destination[x * 2 + 3] = source[chroma_pair + 3];
		}
	}

	return frame;
}

static int gc570d_no_signal_complete(struct gc570d_buffer *buffer,
				      const u8 *frame, size_t frame_size,
				      u32 sequence)
{
	void *destination = vb2_plane_vaddr(&buffer->vb.vb2_buf, 0);

	if (!destination)
		return -EFAULT;
	memcpy(destination, frame, frame_size);
	buffer->vb.sequence = sequence;
	buffer->vb.field = V4L2_FIELD_NONE;
	buffer->vb.vb2_buf.timestamp = ktime_get_ns();
	vb2_set_plane_payload(&buffer->vb.vb2_buf, 0, frame_size);
	return 0;
}

static void gc570d_no_signal_pace(u32 frame_interval, u64 *deadline_ns)
{
	u64 interval_ns = (u64)frame_interval * 100;
	u64 now = ktime_get_ns();
	u64 delay_us;

	if (!*deadline_ns || *deadline_ns < now)
		*deadline_ns = now;
	*deadline_ns += interval_ns;
	if (*deadline_ns <= now)
		return;

	delay_us = div_u64(*deadline_ns - now, NSEC_PER_USEC);
	if (delay_us)
		usleep_range(delay_us, delay_us + 200);
}

static void gc570d_video_return_buffers(struct gc570d_dev *gc,
					 enum vb2_buffer_state state)
{
	struct gc570d_buffer *buffer;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&gc->video_qlock, flags);
		if (list_empty(&gc->video_buffers)) {
			spin_unlock_irqrestore(&gc->video_qlock, flags);
			break;
		}
		buffer = list_first_entry(&gc->video_buffers,
					  struct gc570d_buffer, list);
		list_del(&buffer->list);
		spin_unlock_irqrestore(&gc->video_qlock, flags);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
}

int gc570d_video_switch_to_no_signal(struct gc570d_dev *gc)
{
	u8 *frame = gc570d_no_signal_frame_create(GC570D_WIDTH,
						   GC570D_HEIGHT);

	if (IS_ERR(frame))
		return PTR_ERR(frame);

	mutex_lock(&gc->capture_lock);
	if (gc->video_stopping || !gc->video_streaming ||
	    gc->video_no_signal) {               /* match video0; defensive */
		mutex_unlock(&gc->capture_lock);
		kvfree(frame);
		return -ESHUTDOWN;
	}

	writel(0, gc->bar0 + GC570D_DMA_DESC_CONTROL);
	writel(GC570D_DMA_FRAME_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	readl(gc->bar0 + GC570D_DMA_DESC_CONTROL);
	if (gc->video_irq_active) {
		gc570d_video_irq_put_locked(gc);
		gc->video_irq_active = false;
	}
	gc570d_reset_video_channel(gc, GC570D_DMA_CHANNEL);
	if (gc->video_sg_cpu)
		dma_free_coherent(&gc->pdev->dev, GC570D_DMA_SG_SIZE,
				  gc->video_sg_cpu, gc->video_sg_dma);
	gc->video_sg_cpu = NULL;
	gc->video_sg_dma = 0;
	gc->video_no_signal_frame = frame;
	gc->video_dma_active = false;
	WRITE_ONCE(gc->video_no_signal, true);
	if (!gc->audio_prepared && !gc->audio0_prepared &&
	    !gc->video0_streaming)
		pci_clear_master(gc->pdev);
	mutex_unlock(&gc->capture_lock);
	snd_jack_report(gc->hdmi2_jack, 0);   /* reported outside the lock */
	wake_up_interruptible(&gc->audio_wait);
	return 0;
}


/*
 * The official IT6802 state machine reacts to source-5-V and SCDT changes
 * while the capture pin remains open.  Keep the VB2 stream alive here and
 * replace its software frame producer with the proven VIP1/DMA path only
 * after the receiver has remained locked across consecutive polls.
 */
static int gc570d_video_try_recover_live(struct gc570d_dev *gc,
					 unsigned int *no_scdt_polls,
					 unsigned int *scdt_stable_polls,
					 u64 *hpd_retry_ns)
{
	dma_addr_t sg_dma;
	u8 *placeholder;
	__le32 *sg_cpu;
	bool source_5v;
	bool scdt;
	u64 now;
	int audio_ret;
	int ret;

	if (!mutex_trylock(&gc->capture_lock))
		return 0;
	if (gc->video_stopping || !gc->video_streaming ||
	    !gc->video_no_signal) {
		mutex_unlock(&gc->capture_lock);
		return 0;
	}

	ret = gc570d_it6802_link_status(gc, &source_5v, &scdt);
	if (ret) {
		dev_warn_ratelimited(&gc->pdev->dev,
				     "HDMI IN 2 link-status poll failed: %d\n",
				     ret);
		goto out_unlock;
	}

	now = ktime_get_ns();
	if (!source_5v) {
		*no_scdt_polls = 0;
		*scdt_stable_polls = 0;
		*hpd_retry_ns = 0;
		ret = 0;
		goto out_unlock;
	}

	if (!scdt) {
		*scdt_stable_polls = 0;
		if (*no_scdt_polls < GC570D_HDMI2_HPD_DELAY_POLLS)
			(*no_scdt_polls)++;
		if (*no_scdt_polls < GC570D_HDMI2_HPD_DELAY_POLLS ||
		    (*hpd_retry_ns && now < *hpd_retry_ns)) {
			ret = 0;
			goto out_unlock;
		}

		ret = gc570d_it6802_pulse_hpd(gc);
		if (ret) {
			dev_warn_ratelimited(&gc->pdev->dev,
				     "HDMI IN 2 automatic HPD pulse failed: %d\n",
				     ret);
			goto out_unlock;
		}
		*no_scdt_polls = 0;
		*hpd_retry_ns = ktime_get_ns() + GC570D_HDMI2_HPD_RETRY_NS;
		dev_info(&gc->pdev->dev,
			 "HDMI IN 2 source 5 V detected without SCDT; automatic HPD pulse completed\n");
		ret = 0;
		goto out_unlock;
	}

	*no_scdt_polls = 0;
	*hpd_retry_ns = 0;
	if (++(*scdt_stable_polls) < GC570D_HDMI2_SCDT_STABLE_POLLS) {
		ret = 0;
		goto out_unlock;
	}

	/*
	 * This transition owns video-output recovery even when the HDMI IN 2
	 * PCM is already prepared.  With a software placeholder active,
	 * video_streaming is true but no receiver/VIP pixel path exists; ALSA
	 * therefore cannot be used as evidence that the video transition ran.
	 * Reapply StableOutput and the physical bus format before starting VIP1.
	 */
	ret = gc570d_it6802_output_enable(gc);
	if (ret)
		goto out_link_changed;
	ret = gc570d_it6802_format_init(gc);
	if (ret)
		goto out_link_changed;
	ret = gc570d_vip_init(gc);
	if (ret)
		goto out_link_changed;
	/* StableOutput mutes the receiver; restore an independently open PCM. */
	if (READ_ONCE(gc->audio_recovering)) {
		audio_ret = gc570d_audio_recover_locked(gc);
		if (audio_ret < 0)
			dev_warn_ratelimited(&gc->pdev->dev,
				"HDMI IN 2 video recovered but audio recovery is pending: %d\n",
				audio_ret);
	} else if (gc->audio_prepared) {
		audio_ret = gc570d_it6802_audio_output_set(gc, true);
		if (audio_ret)
			dev_warn_ratelimited(&gc->pdev->dev,
				"HDMI IN 2 video recovered but audio restore is pending: %d\n",
				audio_ret);
	}
	if (readl(gc->bar0 + GC570D_DMA_DESC_CONTROL) & 0x1f) {
		ret = -EBUSY;
		goto out_warn;
	}

	sg_cpu = dma_alloc_coherent(&gc->pdev->dev, GC570D_DMA_SG_SIZE,
				    &sg_dma, GFP_KERNEL);
	if (!sg_cpu) {
		ret = -ENOMEM;
		goto out_warn;
	}
	memset(sg_cpu, 0, GC570D_DMA_SG_SIZE);

	gc->video_sg_cpu = sg_cpu;
	gc->video_sg_dma = sg_dma;
	pci_set_master(gc->pdev);
	gc570d_video_irq_get_locked(gc);
	gc->video_irq_active = true;
	gc->video_dma_active = true;
	placeholder = gc->video_no_signal_frame;
	gc->video_no_signal_frame = NULL;
	WRITE_ONCE(gc->video_no_signal, false);
	mutex_unlock(&gc->capture_lock);
	snd_jack_report(gc->hdmi2_jack, SND_JACK_LINEIN);
	kvfree(placeholder);

	dev_info(&gc->pdev->dev,
		 "HDMI IN 2 signal stable; switched the open V4L2 stream from the built-in placeholder to VIP1/DMA capture\n");
	return 1;

out_link_changed:
	*scdt_stable_polls = 0;
	if (gc570d_video_is_no_signal_error(ret)) {
		ret = 0;
		goto out_unlock;
	}
out_warn:
	dev_warn_ratelimited(&gc->pdev->dev,
			     "HDMI IN 2 automatic live recovery deferred: %d\n",
			     ret);
	ret = 0;
out_unlock:
	mutex_unlock(&gc->capture_lock);
	return ret;
}

static bool gc570d_video_claim_missed_dma(struct gc570d_dev *gc,
					   u32 *hardware_index)
{
	u32 irq_status;

	/* Let an already-running shared-INTx handler publish its completion. */
	synchronize_irq(gc->irq);
	if (try_wait_for_completion(&gc->dma_completion))
		return true;

	*hardware_index = readl(gc->bar0 + GC570D_DMA_DESC_INDEX) & 7;
	irq_status = readl(gc->bar0 + GC570D_REG_IRQ_STATUS);
	if (*hardware_index != 1 ||
	    !(irq_status & GC570D_DMA_FRAME_IRQ))
		return false;

	/*
	 * The GC570D can leave the DMA W1C bit asserted without delivering a
	 * fresh shared INTx edge.  In that case the descriptor itself is the
	 * authoritative completion record, so retire it exactly as the ISR does.
	 */
	writel(0, gc->bar0 + GC570D_DMA_DESC_CONTROL);
	WRITE_ONCE(gc->dma_irq_status, irq_status);
	writel(GC570D_DMA_FRAME_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
	readl(gc->bar0 + GC570D_REG_IRQ_STATUS);
	atomic64_inc(&gc->irq_dma);
	atomic64_inc(&gc->irq_dma1);
	dev_warn_ratelimited(&gc->pdev->dev,
			     "HDMI IN 2 recovered completed DMA descriptor after missed INTx: index=%u irq=0x%08x\n",
			     *hardware_index, irq_status);
	return true;
}

static int gc570d_video_thread(void *data)
{
	struct gc570d_dev *gc = data;
	struct gc570d_buffer *buffer;
	unsigned long flags;
	unsigned long completed;
	dma_addr_t frame_dma;
	u64 no_signal_deadline = 0;
	u64 next_link_poll_ns = 0;
	u64 hpd_retry_ns = 0;
	unsigned int no_scdt_polls = 0;
	unsigned int scdt_stable_polls = 0;
	unsigned int dma_poll;
	u32 hardware_index;
	int ret;

	while (!kthread_should_stop()) {
		ret = wait_event_interruptible(gc->video_wait,
				kthread_should_stop() ||
				READ_ONCE(gc->video_stopping) ||
				!list_empty_careful(&gc->video_buffers));
		if (ret)
			continue;
		if (READ_ONCE(gc->video_stopping) || kthread_should_stop())
			break;

		if (READ_ONCE(gc->video_no_signal) &&
		    ktime_get_ns() >= next_link_poll_ns) {
			ret = gc570d_video_try_recover_live(gc,
						     &no_scdt_polls,
						     &scdt_stable_polls,
						     &hpd_retry_ns);
			next_link_poll_ns = ktime_get_ns() +
				GC570D_HDMI2_LINK_POLL_NS;
			if (ret > 0)
				no_signal_deadline = 0;
		}

		spin_lock_irqsave(&gc->video_qlock, flags);
		if (list_empty(&gc->video_buffers)) {
			spin_unlock_irqrestore(&gc->video_qlock, flags);
			continue;
		}
		buffer = list_first_entry(&gc->video_buffers,
					  struct gc570d_buffer, list);
		list_del(&buffer->list);
		spin_unlock_irqrestore(&gc->video_qlock, flags);

		if (READ_ONCE(gc->video_no_signal)) {
			gc570d_no_signal_pace(GC570D_NO_SIGNAL_INTERVAL,
					       &no_signal_deadline);
			if (READ_ONCE(gc->video_stopping) ||
			    kthread_should_stop()) {
				vb2_buffer_done(&buffer->vb.vb2_buf,
						VB2_BUF_STATE_ERROR);
				break;
			}
			ret = gc570d_no_signal_complete(buffer,
						gc->video_no_signal_frame,
						GC570D_DMA_FRAME_SIZE,
						gc->video_sequence++);
			if (ret) {
				vb2_buffer_done(&buffer->vb.vb2_buf,
						VB2_BUF_STATE_ERROR);
				vb2_queue_error(&gc->video_queue);
				break;
			}
			vb2_buffer_done(&buffer->vb.vb2_buf,
					VB2_BUF_STATE_DONE);
			continue;
		}

		frame_dma = vb2_dma_contig_plane_dma_addr(&buffer->vb.vb2_buf, 0);
		gc->video_sg_cpu[0] = cpu_to_le32(lower_32_bits(frame_dma));
		gc->video_sg_cpu[1] = cpu_to_le32(upper_32_bits(frame_dma));
		gc->video_sg_cpu[2] =
			cpu_to_le32(GC570D_DMA_FRAME_SIZE / sizeof(u32));
		gc->video_sg_cpu[3] = cpu_to_le32(0x80006000);
		dma_wmb();

		writel(GC570D_DMA_FRAME_IRQ,
		       gc->bar0 + GC570D_REG_IRQ_STATUS);
		writel(lower_32_bits(gc->video_sg_dma),
		       gc->bar0 + GC570D_DMA_DESC_ADDRESS);
		writel(upper_32_bits(gc->video_sg_dma),
		       gc->bar0 + GC570D_DMA_DESC_ADDRESS + 4);
		writel(1, gc->bar0 + GC570D_DMA_DESC_ADDRESS + 8);
		reinit_completion(&gc->dma_completion);
		WRITE_ONCE(gc->dma_irq_status, 0);
		writel(BIT(1), gc->bar0 + GC570D_DMA_DESC_CONTROL);
		wmb();
		writel(BIT(1) | BIT(0),
		       gc->bar0 + GC570D_DMA_DESC_CONTROL);

		completed = 0;
		for (dma_poll = 0;
		     dma_poll < DIV_ROUND_UP(GC570D_HDMI2_DMA_TIMEOUT_MS,
						 GC570D_HDMI2_DMA_POLL_MS);
		     dma_poll++) {
			completed = wait_for_completion_timeout(
				&gc->dma_completion,
				msecs_to_jiffies(GC570D_HDMI2_DMA_POLL_MS));
			if (completed || READ_ONCE(gc->video_stopping) ||
			    kthread_should_stop())
				break;
			if (gc570d_video_claim_missed_dma(gc, &hardware_index)) {
				completed = 1;
				break;
			}
		}
		if (READ_ONCE(gc->video_stopping) || kthread_should_stop()) {
			vb2_buffer_done(&buffer->vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
			break;
		}

		hardware_index = readl(gc->bar0 + GC570D_DMA_DESC_INDEX) & 7;
		if (!completed) {
			dev_warn(&gc->pdev->dev,
				 "HDMI IN 2 DMA timeout snapshot: index=%u irq=0x%08x saved_irq=0x%08x control=0x%08x\n",
				 hardware_index,
				 readl(gc->bar0 + GC570D_REG_IRQ_STATUS),
				 READ_ONCE(gc->dma_irq_status),
				 readl(gc->bar0 + GC570D_DMA_DESC_CONTROL));
			ret = gc570d_video_switch_to_no_signal(gc);
			if (ret == -ESHUTDOWN) {
				vb2_buffer_done(&buffer->vb.vb2_buf,
						VB2_BUF_STATE_ERROR);
				break;
			}
			if (!ret) {
				no_signal_deadline = 0;
				next_link_poll_ns = 0;
				hpd_retry_ns = 0;
				no_scdt_polls = 0;
				scdt_stable_polls = 0;
				gc570d_no_signal_pace(
					GC570D_NO_SIGNAL_INTERVAL,
					&no_signal_deadline);
				ret = gc570d_no_signal_complete(
					buffer, gc->video_no_signal_frame,
					GC570D_DMA_FRAME_SIZE,
					gc->video_sequence++);
				if (!ret) {
					vb2_buffer_done(&buffer->vb.vb2_buf,
							VB2_BUF_STATE_DONE);
					continue;
				}
			}
		}

		if (!completed || hardware_index != 1 ||
		    !(READ_ONCE(gc->dma_irq_status) & GC570D_DMA_FRAME_IRQ)) {
			dev_err(&gc->pdev->dev,
				"V4L2 DMA failed: completion=%lu index=%u irq=0x%08x control=0x%08x\n",
				completed, hardware_index,
				READ_ONCE(gc->dma_irq_status),
				readl(gc->bar0 + GC570D_DMA_DESC_CONTROL));
			writel(0, gc->bar0 + GC570D_DMA_DESC_CONTROL);
			vb2_buffer_done(&buffer->vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
			vb2_queue_error(&gc->video_queue);
			break;
		}

		/* Match the proven diagnostic rearm boundary in process context. */
		writel(GC570D_DMA_FRAME_IRQ,
		       gc->bar0 + GC570D_REG_IRQ_STATUS);
		writel(0, gc->bar0 + GC570D_DMA_DESC_CONTROL);
		readl(gc->bar0 + GC570D_DMA_DESC_CONTROL);

		dma_rmb();
		buffer->vb.sequence = gc->video_sequence++;
		buffer->vb.field = V4L2_FIELD_NONE;
		buffer->vb.vb2_buf.timestamp = ktime_get_ns();
		vb2_set_plane_payload(&buffer->vb.vb2_buf, 0,
				      GC570D_DMA_FRAME_SIZE);
		vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}

	return 0;
}

static int gc570d_video_queue_setup(struct vb2_queue *queue,
				     unsigned int *num_buffers,
				     unsigned int *num_planes,
				     unsigned int sizes[],
				     struct device *alloc_devs[])
{
	if (*num_planes) {
		if (sizes[0] < GC570D_DMA_FRAME_SIZE)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = GC570D_DMA_FRAME_SIZE;
	return 0;
}

static int gc570d_video_buffer_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < GC570D_DMA_FRAME_SIZE)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, GC570D_DMA_FRAME_SIZE);
	return 0;
}

static void gc570d_video_buffer_queue(struct vb2_buffer *vb)
{
	struct gc570d_dev *gc = vb2_get_drv_priv(vb->vb2_queue);
	struct gc570d_buffer *buffer =
		container_of(to_vb2_v4l2_buffer(vb), struct gc570d_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&gc->video_qlock, flags);
	list_add_tail(&buffer->list, &gc->video_buffers);
	spin_unlock_irqrestore(&gc->video_qlock, flags);
	wake_up_interruptible(&gc->video_wait);
}

static int gc570d_video_start_streaming(struct vb2_queue *queue,
					 unsigned int count)
{
	struct gc570d_dev *gc = vb2_get_drv_priv(queue);
	bool no_signal = false;
	int ret;

	mutex_lock(&gc->capture_lock);
	if (READ_ONCE(gc->suspended)) {
		ret = -EBUSY;
		goto out_return;
	}
	if (gc->video_stopping || gc->video_streaming) {
		ret = -EBUSY;
		goto out_return;
	}
	if (!gc->audio_prepared) {
		ret = gc570d_it6802_output_enable(gc);
		if (ret && !gc570d_video_is_no_signal_error(ret))
			goto out_return;
		if (ret) {
			no_signal = true;
		} else {
			ret = gc570d_it6802_format_init(gc);
			if (ret && !gc570d_video_is_no_signal_error(ret))
				goto out_return;
			no_signal = ret != 0;
		}
	}
	if (!no_signal) {
		ret = gc570d_vip_init(gc);
		if (ret && !gc570d_video_is_no_signal_error(ret))
			goto out_return;
		no_signal = ret != 0;
	}

	if (no_signal) {
		gc->video_no_signal_frame =
			gc570d_no_signal_frame_create(GC570D_WIDTH,
						       GC570D_HEIGHT);
		if (IS_ERR(gc->video_no_signal_frame)) {
			ret = PTR_ERR(gc->video_no_signal_frame);
			gc->video_no_signal_frame = NULL;
			goto out_return;
		}
	} else {
		if (readl(gc->bar0 + GC570D_DMA_DESC_CONTROL) & 0x1f) {
			ret = -EBUSY;
			goto out_return;
		}
		gc->video_sg_cpu = dma_alloc_coherent(&gc->pdev->dev,
						      GC570D_DMA_SG_SIZE,
						      &gc->video_sg_dma,
						      GFP_KERNEL);
		if (!gc->video_sg_cpu) {
			ret = -ENOMEM;
			goto out_return;
		}
		memset(gc->video_sg_cpu, 0, GC570D_DMA_SG_SIZE);
	}
	gc->video_sequence = 0;
	gc->video_source_width = GC570D_WIDTH;
	gc->video_source_height = GC570D_HEIGHT;
	gc->video_source_fps = 60;
	gc->video_source_profile = GC570D_VIDEO0_SDR_RGB;
	gc->video_stopping = false;
	gc->video_streaming = true;
	gc->video_no_signal = no_signal;
	gc->video_dma_active = !no_signal;
	gc->video_irq_active = false;
	atomic64_set(&gc->irq_total, 0);
	atomic64_set(&gc->irq_dma, 0);
	atomic64_set(&gc->irq_dma0, 0);
	atomic64_set(&gc->irq_dma1, 0);
	atomic64_set(&gc->irq_audio, 0);
	atomic64_set(&gc->irq_other, 0);
	atomic_set(&gc->irq_other_status, 0);
	if (!no_signal) {
		pci_set_master(gc->pdev);
		gc570d_video_irq_get_locked(gc);
		gc->video_irq_active = true;
	}

	gc->video_thread = kthread_run(gc570d_video_thread, gc,
				       "gc570d-video1");
	if (IS_ERR(gc->video_thread)) {
		ret = PTR_ERR(gc->video_thread);
		gc->video_thread = NULL;
		gc->video_streaming = false;
		gc->video_no_signal = false;
		gc->video_dma_active = false;
		if (gc->video_irq_active) {
			gc570d_video_irq_put_locked(gc);
			gc->video_irq_active = false;
		}
		if (!gc->audio_prepared && !gc->audio0_prepared &&
		    !gc->video0_streaming)
			pci_clear_master(gc->pdev);
		if (gc->video_sg_cpu)
			dma_free_coherent(&gc->pdev->dev,
					  GC570D_DMA_SG_SIZE,
					  gc->video_sg_cpu,
					  gc->video_sg_dma);
		gc->video_sg_cpu = NULL;
		kvfree(gc->video_no_signal_frame);
		gc->video_no_signal_frame = NULL;
		goto out_return;
	}
	get_task_struct(gc->video_thread);

	mutex_unlock(&gc->capture_lock);
	wake_up_interruptible(&gc->video_wait);
	if (no_signal)
		dev_info(&gc->pdev->dev,
			 "HDMI IN 2 has no usable signal; streaming the built-in 1920x1080 placeholder at 60 fps\n");
	return 0;

out_return:
	mutex_unlock(&gc->capture_lock);
	gc570d_video_return_buffers(gc, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void gc570d_video_stop_streaming(struct vb2_queue *queue)
{
	struct gc570d_dev *gc = vb2_get_drv_priv(queue);
	struct task_struct *thread;
	bool dma_active;

	mutex_lock(&gc->capture_lock);
	WRITE_ONCE(gc->video_stopping, true);
	dma_active = gc->video_dma_active;
	if (dma_active) {
		writel(0, gc->bar0 + GC570D_DMA_DESC_CONTROL);
		complete(&gc->dma_completion);
	}
	wake_up_interruptible(&gc->video_wait);
	thread = gc->video_thread;
	gc->video_thread = NULL;
	mutex_unlock(&gc->capture_lock);

	if (thread) {
		kthread_stop(thread);
		put_task_struct(thread);
	}

	mutex_lock(&gc->capture_lock);
	if (dma_active)
		gc570d_reset_video_channel(gc, GC570D_DMA_CHANNEL);
	gc->video_streaming = false;
	gc->video_stopping = false;
	gc->video_no_signal = false;
	gc->video_dma_active = false;
	if (gc->video_irq_active) {
		gc570d_video_irq_put_locked(gc);
		gc->video_irq_active = false;
	}
	if (!gc->audio_prepared && !gc->audio0_prepared &&
	    !gc->video0_streaming)
		pci_clear_master(gc->pdev);
	if (gc->video_sg_cpu)
		dma_free_coherent(&gc->pdev->dev, GC570D_DMA_SG_SIZE,
				  gc->video_sg_cpu, gc->video_sg_dma);
	gc->video_sg_cpu = NULL;
	kvfree(gc->video_no_signal_frame);
	gc->video_no_signal_frame = NULL;
	mutex_unlock(&gc->capture_lock);

	gc570d_video_return_buffers(gc, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops gc570d_video_queue_ops = {
	.queue_setup = gc570d_video_queue_setup,
	.buf_prepare = gc570d_video_buffer_prepare,
	.buf_queue = gc570d_video_buffer_queue,
	.start_streaming = gc570d_video_start_streaming,
	.stop_streaming = gc570d_video_stop_streaming,
};

static void gc570d_video_fill_format(struct v4l2_pix_format *pix)
{
	pix->width = GC570D_WIDTH;
	pix->height = GC570D_HEIGHT;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = GC570D_WIDTH * 2;
	pix->sizeimage = GC570D_DMA_FRAME_SIZE;
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static int gc570d_video_querycap(struct file *file, void *priv,
				  struct v4l2_capability *cap)
{
	struct gc570d_dev *gc = video_drvdata(file);

	strscpy(cap->driver, "gc570d", sizeof(cap->driver));
	strscpy(cap->card, "AVerMedia Live Gamer DUO HDMI 2",
		sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s:hdmi2",
		 pci_name(gc->pdev));
	return 0;
}

static int gc570d_video_enum_format(struct file *file, void *priv,
				     struct v4l2_fmtdesc *format)
{
	if (format->index)
		return -EINVAL;
	format->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int gc570d_video_get_format(struct file *file, void *priv,
				    struct v4l2_format *format)
{
	gc570d_video_fill_format(&format->fmt.pix);
	return 0;
}

static int gc570d_video_try_format(struct file *file, void *priv,
				    struct v4l2_format *format)
{
	gc570d_video_fill_format(&format->fmt.pix);
	return 0;
}

static int gc570d_video_set_format(struct file *file, void *priv,
				    struct v4l2_format *format)
{
	struct gc570d_dev *gc = video_drvdata(file);

	if (vb2_is_busy(&gc->video_queue))
		return -EBUSY;
	return gc570d_video_try_format(file, priv, format);
}

static int gc570d_video_enum_framesizes(struct file *file, void *priv,
					 struct v4l2_frmsizeenum *size)
{
	if (size->index || size->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	size->discrete.width = GC570D_WIDTH;
	size->discrete.height = GC570D_HEIGHT;
	return 0;
}

static int gc570d_video_enum_frameintervals(struct file *file, void *priv,
					     struct v4l2_frmivalenum *ival)
{
	if (ival->index || ival->pixel_format != V4L2_PIX_FMT_YUYV ||
	    ival->width != GC570D_WIDTH || ival->height != GC570D_HEIGHT)
		return -EINVAL;
	ival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	ival->discrete.numerator = 1;
	ival->discrete.denominator = 60;
	return 0;
}

static int gc570d_video_enum_input(struct file *file, void *priv,
				    struct v4l2_input *input)
{
	struct gc570d_dev *gc = video_drvdata(file);

	if (input->index)
		return -EINVAL;
	strscpy(input->name, "HDMI IN 2", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	if (READ_ONCE(gc->video_no_signal))
		input->status = V4L2_IN_ST_NO_SIGNAL;
	return 0;
}

static int gc570d_video_get_input(struct file *file, void *priv,
				   unsigned int *input)
{
	*input = 0;
	return 0;
}

static int gc570d_video_set_input(struct file *file, void *priv,
				   unsigned int input)
{
	return input ? -EINVAL : 0;
}

static int gc570d_video_get_parm(struct file *file, void *priv,
				  struct v4l2_streamparm *parm)
{
	struct v4l2_captureparm *capture = &parm->parm.capture;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(capture, 0, sizeof(*capture));
	capture->capability = V4L2_CAP_TIMEPERFRAME;
	capture->timeperframe.numerator = 1;
	capture->timeperframe.denominator = 60;
	capture->readbuffers = 4;
	return 0;
}

static int gc570d_video_set_parm(struct file *file, void *priv,
				  struct v4l2_streamparm *parm)
{
	return gc570d_video_get_parm(file, priv, parm);
}

static const struct v4l2_ioctl_ops gc570d_video_ioctl_ops = {
	.vidioc_querycap = gc570d_video_querycap,
	.vidioc_enum_fmt_vid_cap = gc570d_video_enum_format,
	.vidioc_g_fmt_vid_cap = gc570d_video_get_format,
	.vidioc_try_fmt_vid_cap = gc570d_video_try_format,
	.vidioc_s_fmt_vid_cap = gc570d_video_set_format,
	.vidioc_enum_framesizes = gc570d_video_enum_framesizes,
	.vidioc_enum_frameintervals = gc570d_video_enum_frameintervals,
	.vidioc_enum_input = gc570d_video_enum_input,
	.vidioc_g_input = gc570d_video_get_input,
	.vidioc_s_input = gc570d_video_set_input,
	.vidioc_g_parm = gc570d_video_get_parm,
	.vidioc_s_parm = gc570d_video_set_parm,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations gc570d_video_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

int gc570d_video_register(struct gc570d_dev *gc)
{
	struct vb2_queue *queue = &gc->video_queue;
	struct video_device *video = &gc->video_dev;
	int ret;

	ret = v4l2_device_register(&gc->pdev->dev, &gc->v4l2_dev);
	if (ret)
		return ret;

	queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	queue->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	queue->drv_priv = gc;
	queue->buf_struct_size = sizeof(struct gc570d_buffer);
	queue->ops = &gc570d_video_queue_ops;
	queue->mem_ops = &vb2_dma_contig_memops;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	queue->lock = &gc->video_lock;
	queue->dev = &gc->pdev->dev;
	queue->min_queued_buffers = 2;
	ret = vb2_queue_init(queue);
	if (ret)
		goto out_v4l2;

	strscpy(video->name, "gc570d-hdmi2", sizeof(video->name));
	video->v4l2_dev = &gc->v4l2_dev;
	video->fops = &gc570d_video_fops;
	video->ioctl_ops = &gc570d_video_ioctl_ops;
	video->queue = queue;
	video->lock = &gc->video_lock;
	video->release = video_device_release_empty;
	video->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			     V4L2_CAP_READWRITE;
	video_set_drvdata(video, gc);

	ret = video_register_device(video, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto out_v4l2;

	dev_info(&gc->pdev->dev, "registered HDMI IN 2 as /dev/video%d\n",
		 video->num);
	return 0;

out_v4l2:
	v4l2_device_unregister(&gc->v4l2_dev);
	return ret;
}

static size_t gc570d_video0_frame_size(const struct gc570d_dev *gc)
{
	return (size_t)gc->video0_width * gc->video0_height * 2;
}

/* Snapshot the detected HDMI IN 1 source for the EverLive OSD / status log. */
static void gc570d_video0_note_source(struct gc570d_dev *gc,
				      const struct gc570d_video0_signal *signal)
{
	WRITE_ONCE(gc->video0_source_width, signal->detected_width);
	WRITE_ONCE(gc->video0_source_height, signal->detected_height);
	WRITE_ONCE(gc->video0_source_fps, signal->frame_rate);
	WRITE_ONCE(gc->video0_source_profile, signal->profile);
}

static bool gc570d_video0_work_pending(struct gc570d_dev *gc)
{
	return READ_ONCE(gc->video0_stopping) ||
	       READ_ONCE(gc->video0_event_overflow) ||
	       READ_ONCE(gc->video0_event_head) !=
		READ_ONCE(gc->video0_event_tail) ||
	       (READ_ONCE(gc->video0_no_signal) &&
		!list_empty_careful(&gc->video0_buffers));
}

static void gc570d_video0_fill_sg(struct gc570d_dev *gc,
				   unsigned int slot)
{
	dma_addr_t frame_dma = gc->video0_dma_addr[slot];
	__le32 *sg = gc->video0_sg_cpu[slot];
	unsigned int line;

	/*
	 * The official driver publishes 1,080 scanline fragments in reverse address order
	 * because its positive-height YUY2 DIB is bottom-up.  V4L2 packed YUYV
	 * is top-first, so retain the exact one-fragment-per-line DMA contract
	 * but publish ascending line addresses at this API boundary.
	 */
	for (line = 0; line < gc->video0_height; line++) {
		__le32 *entry = sg + line * 4;
		dma_addr_t line_dma = frame_dma +
			(size_t)line * gc->video0_width * 2;

		entry[0] = cpu_to_le32(lower_32_bits(line_dma));
		entry[1] = cpu_to_le32(upper_32_bits(line_dma));
		entry[2] = cpu_to_le32((gc->video0_width * 2) /
					 sizeof(u32));
		entry[3] = cpu_to_le32(GC570D_DMA_STREAM_SG_FLAGS);
	}
	dma_wmb();
}

static void gc570d_video0_publish_slot(struct gc570d_dev *gc,
					unsigned int slot, bool rearm)
{
	u32 address = GC570D_DMA0_DESC_ADDRESS + slot * 0x0c;

	writel(lower_32_bits(gc->video0_sg_dma[slot]),
	       gc->bar0 + address);
	writel(upper_32_bits(gc->video0_sg_dma[slot]),
	       gc->bar0 + address + 4);
	writel(gc->video0_height, gc->bar0 + address + 8);
	if (rearm)
		writel(readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL) |
		       BIT(slot + 1),
		       gc->bar0 + GC570D_DMA0_DESC_CONTROL);
}

static int gc570d_video0_pop_event(struct gc570d_dev *gc,
				    unsigned int *slot)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&gc->video0_qlock, flags);
	if (gc->video0_event_overflow) {
		gc->video0_event_overflow = false;
		ret = -EOVERFLOW;
	} else if (gc->video0_event_tail != gc->video0_event_head) {
		*slot = gc->video0_events[gc->video0_event_tail];
		gc->video0_event_tail = (gc->video0_event_tail + 1) %
			ARRAY_SIZE(gc->video0_events);
		ret = 1;
	}
	spin_unlock_irqrestore(&gc->video0_qlock, flags);
	return ret;
}

static bool gc570d_video0_claim_missed_dma(struct gc570d_dev *gc)
{
	unsigned long flags;
	u32 irq_status;
	u32 control;
	u32 index;
	unsigned int recovered = 0;
	unsigned int slot;

	/* Own the shared line briefly so the ISR cannot publish the same slots. */
	disable_irq(gc->irq);
	spin_lock_irqsave(&gc->video0_qlock, flags);
	if (gc->video0_event_overflow ||
	    gc->video0_event_head != gc->video0_event_tail) {
		spin_unlock_irqrestore(&gc->video0_qlock, flags);
		enable_irq(gc->irq);
		return true;
	}
	spin_unlock_irqrestore(&gc->video0_qlock, flags);

	irq_status = readl(gc->bar0 + GC570D_REG_IRQ_STATUS);
	if (!(irq_status & GC570D_DMA0_FRAME_IRQ)) {
		enable_irq(gc->irq);
		return false;
	}
	index = readl(gc->bar0 + GC570D_DMA0_DESC_INDEX) & 7;
	control = readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL);

	spin_lock_irqsave(&gc->video0_qlock, flags);
	if (gc->video0_event_overflow ||
	    gc->video0_event_head != gc->video0_event_tail) {
		spin_unlock_irqrestore(&gc->video0_qlock, flags);
		enable_irq(gc->irq);
		return true;
	}
	for (slot = 0; slot < GC570D_DMA_BURST_FRAMES; slot++) {
		u8 next;

		if (!gc->video0_dma_cpu[slot] || (control & BIT(slot + 1)))
			continue;
		next = (gc->video0_event_head + 1) %
			ARRAY_SIZE(gc->video0_events);
		if (next == gc->video0_event_tail) {
			gc->video0_event_overflow = true;
			break;
		}
		gc->video0_events[gc->video0_event_head] = slot;
		gc->video0_event_head = next;
		recovered++;
	}
	if (!recovered && index >= 1 &&
	    index <= GC570D_DMA_BURST_FRAMES &&
	    gc->video0_dma_cpu[index - 1]) {
		u8 next = (gc->video0_event_head + 1) %
			  ARRAY_SIZE(gc->video0_events);

		if (next == gc->video0_event_tail)
			gc->video0_event_overflow = true;
		else {
			gc->video0_events[gc->video0_event_head] = index - 1;
			gc->video0_event_head = next;
			recovered = 1;
		}
	}
	spin_unlock_irqrestore(&gc->video0_qlock, flags);
	if (!recovered) {
		enable_irq(gc->irq);
		return false;
	}

	WRITE_ONCE(gc->dma0_irq_status, irq_status);
	writel(GC570D_DMA0_FRAME_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
	readl(gc->bar0 + GC570D_REG_IRQ_STATUS);
	enable_irq(gc->irq);
	atomic64_add(recovered, &gc->irq_dma);
	atomic64_add(recovered, &gc->irq_dma0);
	wake_up_interruptible(&gc->video0_wait);
	dev_warn_ratelimited(&gc->pdev->dev,
			     "HDMI IN 1 recovered %u completed DMA0 descriptor(s) after missed INTx: index=%u irq=0x%08x control=0x%08x\n",
			     recovered, index, irq_status, control);
	return true;
}

static int gc570d_video0_try_recover_live(struct gc570d_dev *gc,
					   unsigned int *stable_polls);

static int gc570d_video0_thread(void *data)
{
	struct gc570d_dev *gc = data;
	struct gc570d_buffer *buffer;
	unsigned long flags;
	u64 no_signal_deadline = 0;
	u64 next_link_poll_ns = 0;
	unsigned int stable_polls = 0;
	unsigned int dma_idle_polls = 0;
	unsigned int slot;
	int ret;

	while (!kthread_should_stop()) {
		ret = wait_event_interruptible_timeout(gc->video0_wait,
			kthread_should_stop() || gc570d_video0_work_pending(gc),
			msecs_to_jiffies(GC570D_HDMI1_DMA_POLL_MS));
		if (ret < 0)
			continue;
		if (kthread_should_stop() || READ_ONCE(gc->video0_stopping))
			break;
		if (!ret && !READ_ONCE(gc->video0_no_signal)) {
			if (gc570d_video0_claim_missed_dma(gc)) {
				dma_idle_polls = 0;
				ret = 1;
			} else if (++dma_idle_polls <
				   DIV_ROUND_UP(GC570D_HDMI1_DMA_TIMEOUT_MS,
						GC570D_HDMI1_DMA_POLL_MS)) {
				continue;
			}
		}
		if (!ret && !READ_ONCE(gc->video0_no_signal)) {
			dev_warn(&gc->pdev->dev,
				 "HDMI IN 1 DMA0 timeout snapshot: index=%u irq=0x%08x saved_irq=0x%08x control=0x%08x events=%u/%u\n",
				 readl(gc->bar0 + GC570D_DMA0_DESC_INDEX) & 7,
				 readl(gc->bar0 + GC570D_REG_IRQ_STATUS),
				 READ_ONCE(gc->dma0_irq_status),
				 readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL),
				 READ_ONCE(gc->video0_event_head),
				 READ_ONCE(gc->video0_event_tail));
			ret = gc570d_video0_switch_to_no_signal(gc);
			dma_idle_polls = 0;
			if (!ret)
				continue;
			if (ret == -ESHUTDOWN)
				break;
			vb2_queue_error(&gc->video0_queue);
			break;
		}
		if (ret > 0 && !READ_ONCE(gc->video0_no_signal))
			dma_idle_polls = 0;
		if (READ_ONCE(gc->video0_no_signal) &&
		    ktime_get_ns() >= next_link_poll_ns) {
			ret = gc570d_video0_try_recover_live(gc,
						       &stable_polls);
			next_link_poll_ns = ktime_get_ns() +
				GC570D_HDMI1_LINK_POLL_NS;
			if (ret > 0)
				no_signal_deadline = 0;
		}

		if (READ_ONCE(gc->video0_no_signal)) {
			spin_lock_irqsave(&gc->video0_qlock, flags);
			if (list_empty(&gc->video0_buffers)) {
				spin_unlock_irqrestore(&gc->video0_qlock, flags);
				continue;
			}
			buffer = list_first_entry(&gc->video0_buffers,
						  struct gc570d_buffer, list);
			list_del(&buffer->list);
			spin_unlock_irqrestore(&gc->video0_qlock, flags);

			gc570d_no_signal_pace(gc->video0_frame_interval,
					       &no_signal_deadline);
			if (READ_ONCE(gc->video0_stopping) ||
			    kthread_should_stop()) {
				vb2_buffer_done(&buffer->vb.vb2_buf,
						VB2_BUF_STATE_ERROR);
				break;
			}
			ret = gc570d_no_signal_complete(
				buffer, gc->video0_no_signal_frame,
				gc570d_video0_frame_size(gc),
				gc->video0_sequence++);
			if (ret) {
				vb2_buffer_done(&buffer->vb.vb2_buf,
						VB2_BUF_STATE_ERROR);
				vb2_queue_error(&gc->video0_queue);
				break;
			}
			vb2_buffer_done(&buffer->vb.vb2_buf,
					VB2_BUF_STATE_DONE);
			continue;
		}

		ret = gc570d_video0_pop_event(gc, &slot);
		if (ret < 0) {
			dev_err(&gc->pdev->dev,
				"HDMI IN 1 DMA0 completion queue overflow\n");
			writel(0, gc->bar0 + GC570D_DMA0_DESC_CONTROL);
			vb2_queue_error(&gc->video0_queue);
			break;
		}
		if (ret > 0) {
			void *destination;
			size_t frame_size = gc570d_video0_frame_size(gc);
			u32 vip_event;

			spin_lock_irqsave(&gc->video0_qlock, flags);
			if (list_empty(&gc->video0_buffers))
				buffer = NULL;
			else {
				buffer = list_first_entry(&gc->video0_buffers,
							  struct gc570d_buffer,
							  list);
				list_del(&buffer->list);
			}
			spin_unlock_irqrestore(&gc->video0_qlock, flags);
			if (!buffer) {
				gc->video0_sequence++;
				dev_dbg_ratelimited(&gc->pdev->dev,
					"HDMI IN 1 consumer has no queued buffer; dropped one DMA0 frame\n");
				/* FIX: Return slot ownership to hardware when dropping */
				gc570d_video0_publish_slot(gc, slot, true);
				goto video0_housekeeping;
			}

			dma_rmb();
			destination = vb2_plane_vaddr(&buffer->vb.vb2_buf, 0);
			if (!destination) {
				vb2_buffer_done(&buffer->vb.vb2_buf,
						VB2_BUF_STATE_ERROR);
				writel(0, gc->bar0 + GC570D_DMA0_DESC_CONTROL);
				vb2_queue_error(&gc->video0_queue);
				break;
			}
			memcpy(destination, gc->video0_dma_cpu[slot], frame_size);
			buffer->vb.sequence = gc->video0_sequence++;
			buffer->vb.field = V4L2_FIELD_NONE;
			buffer->vb.vb2_buf.timestamp = ktime_get_ns();
			vb2_set_plane_payload(&buffer->vb.vb2_buf, 0, frame_size);
			vb2_buffer_done(&buffer->vb.vb2_buf,
					VB2_BUF_STATE_DONE);
			
			/* FIX: Only publish slot back to hardware DMA *after* safely extracting payload */
			gc570d_video0_publish_slot(gc, slot, true);

		video0_housekeeping:
			vip_event = readl(gc->bar0 +
					 GC570D_VIP0_EVENT_STATUS);
			if ((vip_event & (BIT(0) | BIT(1))) ==
			    (BIT(0) | BIT(1)))
				writel(BIT(1), gc->bar0 +
				       GC570D_VIP0_EVENT_STATUS);
		}
	}

	return 0;
}

static void gc570d_video0_return_buffers(struct gc570d_dev *gc,
					  enum vb2_buffer_state state)
{
	struct gc570d_buffer *buffer;
	unsigned long flags;

	spin_lock_irqsave(&gc->video0_qlock, flags);
	gc->video0_event_head = 0;
	gc->video0_event_tail = 0;
	gc->video0_event_overflow = false;
	spin_unlock_irqrestore(&gc->video0_qlock, flags);

	for (;;) {
		spin_lock_irqsave(&gc->video0_qlock, flags);
		if (list_empty(&gc->video0_buffers)) {
			spin_unlock_irqrestore(&gc->video0_qlock, flags);
			break;
		}
		buffer = list_first_entry(&gc->video0_buffers,
					  struct gc570d_buffer, list);
		list_del(&buffer->list);
		spin_unlock_irqrestore(&gc->video0_qlock, flags);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
}

static void gc570d_video0_free_sg(struct gc570d_dev *gc)
{
	size_t frame_size = gc->video0_dma_size ?
			    gc->video0_dma_size :
			    gc570d_video0_frame_size(gc);
	unsigned int slot;

	for (slot = 0; slot < GC570D_DMA_BURST_FRAMES; slot++) {
		if (gc->video0_dma_cpu[slot])
			dma_free_coherent(&gc->pdev->dev, frame_size,
					  gc->video0_dma_cpu[slot],
					  gc->video0_dma_addr[slot]);
		gc->video0_dma_cpu[slot] = NULL;
		gc->video0_dma_addr[slot] = 0;
		if (gc->video0_sg_cpu[slot])
			dma_free_coherent(&gc->pdev->dev, GC570D_DMA_SG_SIZE,
					  gc->video0_sg_cpu[slot],
					  gc->video0_sg_dma[slot]);
		gc->video0_sg_cpu[slot] = NULL;
		gc->video0_sg_dma[slot] = 0;
	}
	gc->video0_dma_size = 0;
}

static int gc570d_video0_alloc_sg_locked(struct gc570d_dev *gc)
{
	size_t frame_size = gc570d_video0_frame_size(gc);
	unsigned int slot;

	if (gc->video0_dma_size && gc->video0_dma_size != frame_size)
		gc570d_video0_free_sg(gc);

	for (slot = 0; slot < GC570D_DMA_BURST_FRAMES; slot++) {
		if (!gc->video0_sg_cpu[slot]) {
			gc->video0_sg_cpu[slot] =
				dma_alloc_coherent(&gc->pdev->dev,
						   GC570D_DMA_SG_SIZE,
						   &gc->video0_sg_dma[slot],
						   GFP_KERNEL);
			if (!gc->video0_sg_cpu[slot]) {
				gc570d_video0_free_sg(gc);
				return -ENOMEM;
			}
		}
		memset(gc->video0_sg_cpu[slot], 0, GC570D_DMA_SG_SIZE);

		if (!gc->video0_dma_cpu[slot]) {
			gc->video0_dma_cpu[slot] =
				dma_alloc_coherent(&gc->pdev->dev, frame_size,
						   &gc->video0_dma_addr[slot],
						   GFP_KERNEL);
			if (!gc->video0_dma_cpu[slot]) {
				gc570d_video0_free_sg(gc);
				return -ENOMEM;
			}
		}
		gc570d_video0_fill_sg(gc, slot);
	}
	gc->video0_dma_size = frame_size;
	return 0;
}

int gc570d_video0_switch_to_no_signal(struct gc570d_dev *gc)
{
	u8 *frame = gc570d_no_signal_frame_create(gc->video0_width,
						   gc->video0_height);
	unsigned long flags;

	if (IS_ERR(frame))
		return PTR_ERR(frame);

	mutex_lock(&gc->capture_lock);
	if (gc->video0_stopping || !gc->video0_streaming ||
	    gc->video0_no_signal) {
		mutex_unlock(&gc->capture_lock);
		kvfree(frame);
		return -ESHUTDOWN;
	}

	writel(0, gc->bar0 + GC570D_DMA0_DESC_CONTROL);
	writel(GC570D_DMA0_FRAME_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL);
	WRITE_ONCE(gc->dma0_continuous, false);
	if (gc->video0_irq_active) {
		gc570d_video_irq_put_locked(gc);
		gc->video0_irq_active = false;
	}
	msleep(35);
	gc570d_reset_video_channel(gc, 0);

	spin_lock_irqsave(&gc->video0_qlock, flags);
	gc->video0_event_head = 0;
	gc->video0_event_tail = 0;
	gc->video0_event_overflow = false;
	spin_unlock_irqrestore(&gc->video0_qlock, flags);

	/* Retain coherent SG and bounce buffers across signal drops to avoid
	 * Order-10 coherent reallocation failures on fragmented systems.
	 */
	gc->video0_no_signal_frame = frame;
	gc->video0_dma_active = false;
	gc->video0_receiver_output_valid = false;
	WRITE_ONCE(gc->video0_no_signal, true);
	if (!gc->video_dma_active && !gc->audio_prepared &&
	    !gc->audio0_prepared)
		pci_clear_master(gc->pdev);
	mutex_unlock(&gc->capture_lock);
	snd_jack_report(gc->hdmi1_jack, 0);
	wake_up_interruptible(&gc->video0_wait);
	wake_up_interruptible(&gc->audio0_wait);
	return 0;
}


static int gc570d_video0_try_recover_live(struct gc570d_dev *gc,
					   unsigned int *stable_polls)
{
	struct gc570d_video0_signal signal;
	u8 *placeholder;
	unsigned long flags;
	unsigned int slot;
	bool dma_started = false;
	int ret;

	if (!mutex_trylock(&gc->capture_lock))
		return 0;
	if (gc->video0_stopping || !gc->video0_streaming ||
	    !gc->video0_no_signal ||
	    (auto_hdmi1 && !READ_ONCE(gc->hdmi1_auto_ready))) {
		mutex_unlock(&gc->capture_lock);
		return 0;
	}

	ret = gc570d_it68051_video_output_on(gc);
	if (ret)
		goto out_signal;
	ret = gc570d_vip0_detect_signal(gc, &signal);
	if (ret)
		goto out_signal;
	gc570d_video0_note_source(gc, &signal);
	if (++(*stable_polls) < GC570D_HDMI1_STABLE_POLLS) {
		mutex_unlock(&gc->capture_lock);
		return 0;
	}

	if (readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL) & 0x1f) {
		ret = -EBUSY;
		goto out_deferred;
	}
	ret = gc570d_xilinx_prepare_video0(gc, &signal, gc->video0_width,
					   gc->video0_height);
	if (ret)
		goto out_deferred;
	if (signal.profile == GC570D_VIDEO0_HDR_BT2020) {
		ret = gc570d_xilinx_load_hdr_lut_3000(gc);
		if (ret)
			goto out_deferred;
	}
	ret = gc570d_xilinx_prepare_video0(gc, &signal, gc->video0_width,
					   gc->video0_height);
	if (ret)
		goto out_deferred;

	ret = gc570d_video0_alloc_sg_locked(gc);
	if (ret)
		goto out_free;

	spin_lock_irqsave(&gc->video0_qlock, flags);
	gc->video0_event_head = 0;
	gc->video0_event_tail = 0;
	gc->video0_event_overflow = false;
	spin_unlock_irqrestore(&gc->video0_qlock, flags);

	pci_set_master(gc->pdev);
	writel(GC570D_DMA0_FRAME_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	for (slot = 0; slot < GC570D_DMA_BURST_FRAMES; slot++) {
		gc570d_video0_publish_slot(gc, slot, false);
		writel(readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL) |
		       BIT(slot + 1), gc->bar0 + GC570D_DMA0_DESC_CONTROL);
	}
	WRITE_ONCE(gc->dma0_continuous, true);
	gc570d_video_irq_get_locked(gc);
	gc->video0_irq_active = true;
	wmb();
	writel(readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL) | BIT(0),
	       gc->bar0 + GC570D_DMA0_DESC_CONTROL);
	dma_started = true;
	gc->video0_dma_active = true;
	if ((readl(gc->bar0 + GC570D_VIP0_EVENT_STATUS) &
	     (BIT(0) | BIT(1))) == (BIT(0) | BIT(1)))
		writel(BIT(1), gc->bar0 + GC570D_VIP0_EVENT_STATUS);
	ret = gc570d_vip0_program_yuy2(gc, &signal, gc->video0_width,
				       gc->video0_height,
				       gc->video0_frame_interval);
	if (ret)
		goto out_requeue;
	if (READ_ONCE(gc->audio0_recovering)) {
		ret = gc570d_audio0_recover_locked(gc);
		if (ret < 0)
			dev_warn_ratelimited(&gc->pdev->dev,
				"HDMI IN 1 audio recovery deferred: %d\n", ret);
	}

	placeholder = gc->video0_no_signal_frame;
	gc->video0_no_signal_frame = NULL;
	WRITE_ONCE(gc->video0_no_signal, false);
	mutex_unlock(&gc->capture_lock);
	snd_jack_report(gc->hdmi1_jack, SND_JACK_LINEIN);
	kvfree(placeholder);
	wake_up_interruptible(&gc->video0_wait);

	dev_info(&gc->pdev->dev,
		 "HDMI IN 1 signal stable; switched the open V4L2 stream from the built-in placeholder to %ux%u %s capture at %ux%u with four private DMA0 slots\n",
		 signal.detected_width, signal.detected_height,
		 signal.profile == GC570D_VIDEO0_HDR_BT2020 ?
		 "BT.2020 HDR-to-SDR" :
		 signal.profile == GC570D_VIDEO0_SDR_RGB ?
		 "RGB SDR" : "BT.709 YCbCr420 SDR",
		 gc->video0_width, gc->video0_height);
	return 1;

out_requeue:
	writel(0, gc->bar0 + GC570D_DMA0_DESC_CONTROL);
	writel(GC570D_DMA0_FRAME_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	WRITE_ONCE(gc->dma0_continuous, false);
	if (gc->video0_irq_active) {
		gc570d_video_irq_put_locked(gc);
		gc->video0_irq_active = false;
	}
	if (dma_started)
		gc570d_reset_video_channel(gc, 0);
	gc->video0_dma_active = false;
out_free:
	gc570d_video0_free_sg(gc);
	if (!gc->video_dma_active && !gc->audio_prepared &&
	    !gc->audio0_prepared)
		pci_clear_master(gc->pdev);
out_deferred:
	dev_warn_ratelimited(&gc->pdev->dev,
			     "HDMI IN 1 automatic live recovery deferred: %d\n",
			     ret);
	mutex_unlock(&gc->capture_lock);
	return 0;

out_signal:
	*stable_polls = 0;
	if (!gc570d_video_is_no_signal_error(ret))
		dev_warn_ratelimited(&gc->pdev->dev,
			     "HDMI IN 1 automatic signal check failed: %d\n",
			     ret);
	mutex_unlock(&gc->capture_lock);
	return 0;
}

static int gc570d_video0_queue_setup(struct vb2_queue *queue,
				      unsigned int *num_buffers,
				      unsigned int *num_planes,
				      unsigned int sizes[],
				      struct device *alloc_devs[])
{
	struct gc570d_dev *gc = vb2_get_drv_priv(queue);
	size_t frame_size = gc570d_video0_frame_size(gc);

	if (*num_planes) {
		if (sizes[0] < frame_size)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = frame_size;
	return 0;
}

static int gc570d_video0_buffer_prepare(struct vb2_buffer *vb)
{
	struct gc570d_dev *gc = vb2_get_drv_priv(vb->vb2_queue);
	size_t frame_size = gc570d_video0_frame_size(gc);

	if (vb2_plane_size(vb, 0) < frame_size)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, frame_size);
	return 0;
}

static void gc570d_video0_buffer_queue(struct vb2_buffer *vb)
{
	struct gc570d_dev *gc = vb2_get_drv_priv(vb->vb2_queue);
	struct gc570d_buffer *buffer =
		container_of(to_vb2_v4l2_buffer(vb), struct gc570d_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&gc->video0_qlock, flags);
	list_add_tail(&buffer->list, &gc->video0_buffers);
	spin_unlock_irqrestore(&gc->video0_qlock, flags);
	wake_up_interruptible(&gc->video0_wait);
}

/* capture_lock is held and no DMA0 resources have been published. */
static int gc570d_video0_start_no_signal_locked(struct gc570d_dev *gc)
{
	int ret;

	ret = gc570d_video0_alloc_sg_locked(gc);
	if (ret)
		return ret;

	gc->video0_no_signal_frame =
		gc570d_no_signal_frame_create(gc->video0_width,
					       gc->video0_height);
	if (IS_ERR(gc->video0_no_signal_frame)) {
		ret = PTR_ERR(gc->video0_no_signal_frame);
		gc->video0_no_signal_frame = NULL;
		gc570d_video0_free_sg(gc);
		return ret;
	}

	gc->video0_sequence = 0;
	gc->video0_event_head = 0;
	gc->video0_event_tail = 0;
	gc->video0_event_overflow = false;
	gc->video0_stopping = false;
	gc->video0_streaming = true;
	gc->video0_no_signal = true;
	gc->video0_dma_active = false;
	gc->video0_irq_active = false;
	gc->video0_receiver_output_valid = false;
	WRITE_ONCE(gc->dma0_continuous, false);
	gc->video0_thread = kthread_run(gc570d_video0_thread, gc,
					       "gc570d-video0");
	if (IS_ERR(gc->video0_thread)) {
		ret = PTR_ERR(gc->video0_thread);
		gc->video0_thread = NULL;
		gc->video0_streaming = false;
		gc->video0_no_signal = false;
		kvfree(gc->video0_no_signal_frame);
		gc->video0_no_signal_frame = NULL;
		gc570d_video0_free_sg(gc);
		return ret;
	}
	get_task_struct(gc->video0_thread);

	return 0;
}

static int gc570d_video0_start_streaming(struct vb2_queue *queue,
					  unsigned int count)
{
	struct gc570d_dev *gc = vb2_get_drv_priv(queue);
	unsigned int slot;
	bool irq_enabled = false;
	bool dma_started = false;
	struct gc570d_video0_signal signal;
	int ret;

	mutex_lock(&gc->capture_lock);
	if (READ_ONCE(gc->suspended)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	if (gc->video0_stopping || gc->video0_streaming) {
		ret = -EBUSY;
		goto out_unlock;
	}

	if (readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL) & 0x1f) {
		ret = -EBUSY;
		goto out_unlock;
	}

	if (auto_hdmi1 && !READ_ONCE(gc->hdmi1_auto_ready))
		goto out_no_signal;

	/*
	 * The official driver's periodic IT68051 state machine reapplies the receiver
	 * configuration when the HDMI format changes.  The Linux passthrough pump owns only the
	 * splitter, so refresh the HDMI IN 1 receiver here before every RUN.
	 */
	ret = gc570d_it68051_video_output_on(gc);
	if (ret && gc570d_video_is_no_signal_error(ret))
		goto out_no_signal;
	if (ret)
		goto out_unlock;
	ret = gc570d_vip0_detect_signal(gc, &signal);
	if (ret && gc570d_video_is_no_signal_error(ret))
		goto out_no_signal;
	if (ret)
		goto out_unlock;
	gc570d_video0_note_source(gc, &signal);
	ret = gc570d_xilinx_prepare_video0(gc, &signal, gc->video0_width,
					   gc->video0_height);
	if (ret)
		goto out_unlock;
	if (signal.profile == GC570D_VIDEO0_HDR_BT2020) {
		ret = gc570d_xilinx_load_hdr_lut_3000(gc);
		if (ret)
			goto out_unlock;
	}
	ret = gc570d_xilinx_prepare_video0(gc, &signal, gc->video0_width,
					   gc->video0_height);
	if (ret)
		goto out_unlock;

	ret = gc570d_video0_alloc_sg_locked(gc);
	if (ret)
		goto out_free;

	gc->video0_sequence = 0;
	gc->video0_event_head = 0;
	gc->video0_event_tail = 0;
	gc->video0_event_overflow = false;
	gc->video0_stopping = false;
	gc->video0_streaming = true;
	gc->video0_no_signal = false;
	gc->video0_dma_active = false;
	gc->video0_irq_active = false;
	WRITE_ONCE(gc->dma0_continuous, true);
	gc->video0_thread = kthread_run(gc570d_video0_thread, gc,
					       "gc570d-video0");
	if (IS_ERR(gc->video0_thread)) {
		ret = PTR_ERR(gc->video0_thread);
		gc->video0_thread = NULL;
		gc->video0_streaming = false;
		WRITE_ONCE(gc->dma0_continuous, false);
		goto out_free;
	}
	get_task_struct(gc->video0_thread);

	if (!gc->video_streaming) {
		atomic64_set(&gc->irq_total, 0);
		atomic64_set(&gc->irq_dma, 0);
		atomic64_set(&gc->irq_dma0, 0);
		atomic64_set(&gc->irq_dma1, 0);
		atomic64_set(&gc->irq_other, 0);
		atomic_set(&gc->irq_other_status, 0);
	}
	pci_set_master(gc->pdev);
	writel(GC570D_DMA0_FRAME_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	for (slot = 0; slot < GC570D_DMA_BURST_FRAMES; slot++) {
		gc570d_video0_publish_slot(gc, slot, false);
		writel(readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL) |
		       BIT(slot + 1),
		       gc->bar0 + GC570D_DMA0_DESC_CONTROL);
	}
	gc570d_video_irq_get_locked(gc);
	irq_enabled = true;
	gc->video0_irq_active = true;
	wmb();
	writel(readl(gc->bar0 + GC570D_DMA0_DESC_CONTROL) | BIT(0),
	       gc->bar0 + GC570D_DMA0_DESC_CONTROL);
	dma_started = true;
	gc->video0_dma_active = true;
	if ((readl(gc->bar0 + GC570D_VIP0_EVENT_STATUS) &
	     (BIT(0) | BIT(1))) == (BIT(0) | BIT(1)))
		writel(BIT(1), gc->bar0 + GC570D_VIP0_EVENT_STATUS);
	ret = gc570d_vip0_program_yuy2(gc, &signal, gc->video0_width,
				       gc->video0_height,
				       gc->video0_frame_interval);
	if (ret)
		goto out_stop;

	mutex_unlock(&gc->capture_lock);
	wake_up_interruptible(&gc->video0_wait);
	dev_info(&gc->pdev->dev,
		 "HDMI IN 1 V4L2 DMA0 private ring started: %ux%u %s to %ux%u YUYV at %u fps\n",
		 signal.detected_width, signal.detected_height,
		 signal.profile == GC570D_VIDEO0_HDR_BT2020 ?
		 "BT.2020 HDR" :
		 signal.profile == GC570D_VIDEO0_SDR_RGB ?
		 "RGB SDR" : "BT.709 YCbCr420 SDR",
		 gc->video0_width, gc->video0_height,
		 gc->video0_frame_interval == 200000 ? 50 : 60);
	return 0;

out_no_signal:
	ret = gc570d_video0_start_no_signal_locked(gc);
	if (ret)
		goto out_unlock;
	mutex_unlock(&gc->capture_lock);
	wake_up_interruptible(&gc->video0_wait);
	dev_info(&gc->pdev->dev,
		 "HDMI IN 1 has no usable signal; streaming the built-in %ux%u placeholder at %u fps\n",
		 gc->video0_width, gc->video0_height,
		 gc->video0_frame_interval == 200000 ? 50 : 60);
	return 0;

out_stop:
	gc->video0_stopping = true;
	writel(0, gc->bar0 + GC570D_DMA0_DESC_CONTROL);
	wake_up_interruptible(&gc->video0_wait);
	if (gc->video0_thread) {
		kthread_stop(gc->video0_thread);
		put_task_struct(gc->video0_thread);
		gc->video0_thread = NULL;
	}
	if (irq_enabled)
		gc570d_video_irq_put_locked(gc);
	gc->video0_irq_active = false;
	if (dma_started)
		gc570d_reset_video_channel(gc, 0);
	gc->video0_dma_active = false;
	gc->video0_streaming = false;
	gc->video0_no_signal = false;
	if (!gc->video_streaming && !gc->audio_prepared &&
	    !gc->audio0_prepared)
		pci_clear_master(gc->pdev);
	WRITE_ONCE(gc->dma0_continuous, false);
out_free:
	gc570d_video0_free_sg(gc);
out_unlock:
	mutex_unlock(&gc->capture_lock);
	gc570d_video0_return_buffers(gc, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void gc570d_video0_stop_streaming(struct vb2_queue *queue)
{
	struct gc570d_dev *gc = vb2_get_drv_priv(queue);
	struct task_struct *thread;
	bool dma_active;

	mutex_lock(&gc->capture_lock);
	WRITE_ONCE(gc->video0_stopping, true);
	dma_active = gc->video0_dma_active;
	if (dma_active) {
		writel(0, gc->bar0 + GC570D_DMA0_DESC_CONTROL);
		writel(GC570D_DMA0_FRAME_IRQ,
		       gc->bar0 + GC570D_REG_IRQ_STATUS);
	}
	wake_up_interruptible(&gc->video0_wait);
	thread = gc->video0_thread;
	gc->video0_thread = NULL;
	mutex_unlock(&gc->capture_lock);

	if (thread) {
		kthread_stop(thread);
		put_task_struct(thread);
	}

	mutex_lock(&gc->capture_lock);
	if (dma_active)
		gc570d_reset_video_channel(gc, 0);
	WRITE_ONCE(gc->dma0_continuous, false);
	WRITE_ONCE(gc->video0_streaming, false);
	WRITE_ONCE(gc->video0_stopping, false);
	WRITE_ONCE(gc->video0_no_signal, false);
	WRITE_ONCE(gc->video0_dma_active, false);
	if (gc->video0_irq_active) {
		gc570d_video_irq_put_locked(gc);
		gc->video0_irq_active = false;
	}
	if (!gc->video_streaming && !gc->audio_prepared &&
	    !gc->audio0_prepared)
		pci_clear_master(gc->pdev);
	gc570d_video0_free_sg(gc);
	kvfree(gc->video0_no_signal_frame);
	gc->video0_no_signal_frame = NULL;
	mutex_unlock(&gc->capture_lock);

	gc570d_video0_return_buffers(gc, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops gc570d_video0_queue_ops = {
	.queue_setup = gc570d_video0_queue_setup,
	.buf_prepare = gc570d_video0_buffer_prepare,
	.buf_queue = gc570d_video0_buffer_queue,
	.start_streaming = gc570d_video0_start_streaming,
	.stop_streaming = gc570d_video0_stop_streaming,
};

/* Exact YUY2 geometries and default intervals from GC570D table 0x1400f7620. */
static const struct gc570d_video0_format gc570d_video0_formats[] = {
	{ 1920, 1080, 166666 },
	{ 640, 480, 166666 },
	{ 720, 480, 166666 },
	{ 720, 576, 200000 },
	{ 1024, 768, 166666 },
	{ 1280, 720, 166666 },
};

static const struct gc570d_video0_format *
gc570d_video0_find_format(u32 width, u32 height)
{
	const struct gc570d_video0_format *best = &gc570d_video0_formats[0];
	u32 best_distance = U32_MAX;
	size_t i;

	if (!width || !height)
		return best;
	for (i = 0; i < ARRAY_SIZE(gc570d_video0_formats); i++) {
		const struct gc570d_video0_format *candidate =
			&gc570d_video0_formats[i];
		u32 distance = abs((int)width - candidate->width) +
			abs((int)height - candidate->height);

		if (distance < best_distance) {
			best = candidate;
			best_distance = distance;
		}
	}
	return best;
}

static void gc570d_video0_fill_format(struct v4l2_pix_format *pix,
				       u16 width, u16 height)
{
	pix->width = width;
	pix->height = height;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = width * 2;
	pix->sizeimage = width * height * 2;
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static int gc570d_video0_get_format(struct file *file, void *priv,
				     struct v4l2_format *format)
{
	struct gc570d_dev *gc = video_drvdata(file);

	gc570d_video0_fill_format(&format->fmt.pix, gc->video0_width,
				  gc->video0_height);
	return 0;
}

static int gc570d_video0_try_format(struct file *file, void *priv,
				     struct v4l2_format *format)
{
	const struct gc570d_video0_format *selected;

	selected = gc570d_video0_find_format(format->fmt.pix.width,
					     format->fmt.pix.height);
	gc570d_video0_fill_format(&format->fmt.pix, selected->width,
				  selected->height);
	return 0;
}

static int gc570d_video0_querycap(struct file *file, void *priv,
				   struct v4l2_capability *cap)
{
	struct gc570d_dev *gc = video_drvdata(file);

	strscpy(cap->driver, "gc570d", sizeof(cap->driver));
	strscpy(cap->card, "AVerMedia Live Gamer DUO HDMI 1",
		sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s:hdmi1",
		 pci_name(gc->pdev));
	return 0;
}

static int gc570d_video0_set_format(struct file *file, void *priv,
				     struct v4l2_format *format)
{
	struct gc570d_dev *gc = video_drvdata(file);
	const struct gc570d_video0_format *selected;
	int ret;

	if (vb2_is_busy(&gc->video0_queue))
		return -EBUSY;
	ret = gc570d_video0_try_format(file, priv, format);
	if (ret)
		return ret;
	selected = gc570d_video0_find_format(format->fmt.pix.width,
					     format->fmt.pix.height);
	mutex_lock(&gc->capture_lock);
	gc->video0_width = selected->width;
	gc->video0_height = selected->height;
	gc->video0_frame_interval = selected->frame_interval;
	mutex_unlock(&gc->capture_lock);
	return 0;
}

static int gc570d_video0_enum_framesizes(struct file *file, void *priv,
					  struct v4l2_frmsizeenum *size)
{
	if (size->index >= ARRAY_SIZE(gc570d_video0_formats) ||
	    size->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	size->discrete.width = gc570d_video0_formats[size->index].width;
	size->discrete.height = gc570d_video0_formats[size->index].height;
	return 0;
}

static int gc570d_video0_enum_frameintervals(
	struct file *file, void *priv, struct v4l2_frmivalenum *ival)
{
	const struct gc570d_video0_format *selected;

	if (ival->index || ival->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	selected = gc570d_video0_find_format(ival->width, ival->height);
	if (selected->width != ival->width || selected->height != ival->height)
		return -EINVAL;
	ival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	ival->discrete.numerator = 1;
	ival->discrete.denominator = selected->frame_interval == 200000 ?
		50 : 60;
	return 0;
}

static int gc570d_video0_get_parm(struct file *file, void *priv,
				   struct v4l2_streamparm *parm)
{
	struct gc570d_dev *gc = video_drvdata(file);
	struct v4l2_captureparm *capture = &parm->parm.capture;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	memset(capture, 0, sizeof(*capture));
	capture->capability = V4L2_CAP_TIMEPERFRAME;
	capture->timeperframe.numerator = 1;
	capture->timeperframe.denominator =
		gc->video0_frame_interval == 200000 ? 50 : 60;
	capture->readbuffers = GC570D_DMA_BURST_FRAMES;
	return 0;
}

static int gc570d_video0_set_parm(struct file *file, void *priv,
				   struct v4l2_streamparm *parm)
{
	return gc570d_video0_get_parm(file, priv, parm);
}

static int gc570d_video0_enum_input(struct file *file, void *priv,
				     struct v4l2_input *input)
{
	struct gc570d_dev *gc = video_drvdata(file);

	if (input->index)
		return -EINVAL;
	strscpy(input->name, "HDMI IN 1", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	if (READ_ONCE(gc->video0_no_signal))
		input->status = V4L2_IN_ST_NO_SIGNAL;
	return 0;
}

static const struct v4l2_ioctl_ops gc570d_video0_ioctl_ops = {
	.vidioc_querycap = gc570d_video0_querycap,
	.vidioc_enum_fmt_vid_cap = gc570d_video_enum_format,
	.vidioc_g_fmt_vid_cap = gc570d_video0_get_format,
	.vidioc_try_fmt_vid_cap = gc570d_video0_try_format,
	.vidioc_s_fmt_vid_cap = gc570d_video0_set_format,
	.vidioc_enum_framesizes = gc570d_video0_enum_framesizes,
	.vidioc_enum_frameintervals = gc570d_video0_enum_frameintervals,
	.vidioc_enum_input = gc570d_video0_enum_input,
	.vidioc_g_input = gc570d_video_get_input,
	.vidioc_s_input = gc570d_video_set_input,
	.vidioc_g_parm = gc570d_video0_get_parm,
	.vidioc_s_parm = gc570d_video0_set_parm,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

int gc570d_video0_register(struct gc570d_dev *gc)
{
	struct vb2_queue *queue = &gc->video0_queue;
	struct video_device *video = &gc->video0_dev;
	int ret;

	queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	queue->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	queue->drv_priv = gc;
	queue->buf_struct_size = sizeof(struct gc570d_buffer);
	queue->ops = &gc570d_video0_queue_ops;
	queue->mem_ops = &vb2_dma_contig_memops;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	queue->lock = &gc->video0_lock;
	queue->dev = &gc->pdev->dev;
	queue->min_queued_buffers = 2;
	ret = vb2_queue_init(queue);
	if (ret)
		return ret;

	strscpy(video->name, "gc570d-hdmi1", sizeof(video->name));
	video->v4l2_dev = &gc->v4l2_dev;
	video->fops = &gc570d_video_fops;
	video->ioctl_ops = &gc570d_video0_ioctl_ops;
	video->queue = queue;
	video->lock = &gc->video0_lock;
	video->release = video_device_release_empty;
	video->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			     V4L2_CAP_READWRITE;
	video_set_drvdata(video, gc);

	ret = video_register_device(video, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;

	dev_info(&gc->pdev->dev, "registered HDMI IN 1 as /dev/video%d\n",
		 video->num);
	return 0;
}

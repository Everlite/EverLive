// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALSA PCM capture integration and audio DMA handling for both HDMI inputs.
 */
#include "gc570d.h"
#ifndef GC570D_USERSPACE
#include <sound/control.h>
#endif

static void gc570d_audio_update_start_bit(struct gc570d_dev *gc, u32 mask,
					  bool enable)
{
	unsigned long flags;
	u32 control;

	spin_lock_irqsave(&gc->audio_pcm_lock, flags);
	control = readl(gc->bar0 + GC570D_AUDIO_START);
	if (enable)
		control |= mask;
	else
		control &= ~mask;
	writel(control, gc->bar0 + GC570D_AUDIO_START);
	spin_unlock_irqrestore(&gc->audio_pcm_lock, flags);
}

static bool gc570d_audio_is_no_signal_error(int ret)
{
	return ret == -ENOLINK || ret == -ENODATA || ret == -ERANGE ||
	       ret == -EINVAL || ret == -EILSEQ || ret == -EOPNOTSUPP;
}

static int gc570d_audio_take_period(void *buffer_cpu[2], u32 *cursor,
				    void *destination)
{
	const u32 slots_per_buf = GC570D_AUDIO_STAGING_SIZE /
				  GC570D_AUDIO_PERIOD_BYTES;
	const u32 total_slots = 2 * slots_per_buf;
	u32 descriptor;
	u32 best_slot = 0;
	u32 step;
	size_t best_changed = 0;
	size_t changed;
	size_t offset;
	size_t i;

	dma_rmb();
	for (step = 0; step < total_slots; step++) {
		u32 slot = (*cursor + step) % total_slots;
		const u64 *words;

		descriptor = slot / slots_per_buf;
		offset = (slot % slots_per_buf) * GC570D_AUDIO_PERIOD_BYTES;
		words = (const u64 *)((const u8 *)buffer_cpu[descriptor] +
				      offset);
		changed = 0;
		for (i = 0; i < GC570D_AUDIO_PERIOD_BYTES / sizeof(u64); i++) {
			if (words[i] != 0x5a5a5a5a5a5a5a5aULL)
				changed += sizeof(u64);
		}
		if (changed > best_changed) {
			best_changed = changed;
			best_slot = slot;
			if (best_changed == GC570D_AUDIO_PERIOD_BYTES)
				break;
		}
	}
	if (best_changed < GC570D_AUDIO_PERIOD_BYTES / 2)
		return -ENODATA;

	descriptor = best_slot / slots_per_buf;
	offset = (best_slot % slots_per_buf) * GC570D_AUDIO_PERIOD_BYTES;
	memcpy(destination, buffer_cpu[descriptor] + offset,
	       GC570D_AUDIO_PERIOD_BYTES);
	memset(buffer_cpu[descriptor] + offset, GC570D_AUDIO_SENTINEL,
	       GC570D_AUDIO_PERIOD_BYTES);
	*cursor = (best_slot + 1) % total_slots;
	dma_wmb();

	return 0;
}

static int gc570d_audio0_dma_capture_periods(struct gc570d_dev *gc,
					      unsigned int periods)
{
	void *buffer_cpu[2] = { NULL, NULL };
	dma_addr_t buffer_dma[2] = { 0, 0 };
	ktime_t capture_start;
	u64 term_baseline;
	u32 irq_status = 0;
	u32 cursor = 0;
	u32 index;
	long waited;
	size_t changed = 0;
	size_t i;
	int ret;

	mutex_lock(&gc->capture_lock);
	if (!READ_ONCE(gc->video0_streaming)) {
		ret = -EAGAIN;
		goto out_status;
	}
	
	/* FIX: Verify ALSA has not already prepared the hardware for capture */
	if (gc->audio0_prepared || (readl(gc->bar0 + GC570D_AUDIO_START) &
	    GC570D_AUDIO0_START_BIT)) {
		ret = -EBUSY;
		goto out_status;
	}

	vfree(gc->audio0_capture_data);
	gc->audio0_capture_data = NULL;
	gc->audio0_capture_size = 0;
	gc->audio0_capture_changed = 0;
	gc->audio0_capture_irq = 0;
	gc->audio0_capture_index = 0;
	gc->audio0_capture_control = 0;
	gc->audio0_capture_requested = periods;
	gc->audio0_capture_completed = 0;
	gc->audio0_capture_elapsed_us = 0;
	gc->audio0_capture_error = 0;
	gc->audio0_capture_data = vmalloc(array_size(periods,
						     GC570D_AUDIO_PERIOD_BYTES));
	if (!gc->audio0_capture_data) {
		ret = -ENOMEM;
		goto out_status;
	}

	ret = gc570d_it68051_audio_output_set(gc, true);
	if (ret)
		goto out_status;
	for (i = 0; i < 2; i++) {
		buffer_cpu[i] = dma_alloc_coherent(&gc->pdev->dev,
						   GC570D_AUDIO_BUFFER_SIZE,
						   &buffer_dma[i], GFP_KERNEL);
		if (!buffer_cpu[i]) {
			ret = -ENOMEM;
			goto out_free;
		}
		memset(buffer_cpu[i], GC570D_AUDIO_SENTINEL,
		       GC570D_AUDIO_BUFFER_SIZE);
	}

	gc570d_reset_audio_channel(gc, GC570D_AUDIO0_CHANNEL);
	writel(0, gc->bar0 + GC570D_AUDIO0_FORMAT);
	writel(0, gc->bar0 + GC570D_AUDIO_GLOBAL_FORMAT);
	writel(GC570D_AUDIO_PERIOD_FRAMES, gc->bar0 + GC570D_AUDIO0_RATE);
	writel(lower_32_bits(buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER0);
	writel(upper_32_bits(buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER0 + 4);
	writel(lower_32_bits(buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER1);
	writel(upper_32_bits(buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER1 + 4);
	dma_wmb();

	atomic_set(&gc->audio0_irq_pending, 0);
	writel(GC570D_AUDIO0_IRQ | GC570D_AUDIO0_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	pci_set_master(gc->pdev);
	term_baseline = atomic64_read(&gc->irq_audio0_term);
	capture_start = ktime_get();
	gc570d_audio_update_start_bit(gc, GC570D_AUDIO0_START_BIT, true);
	ret = 0;
	while (gc->audio0_capture_completed < periods) {
		waited = wait_event_interruptible_timeout(
			gc->audio0_wait,
			atomic_read(&gc->audio0_irq_pending) ||
			!READ_ONCE(gc->video0_streaming),
			msecs_to_jiffies(500));
		if (waited < 0) {
			ret = waited;
			break;
		}
		if (!waited) {
			ret = -ETIMEDOUT;
			break;
		}
		if (!READ_ONCE(gc->video0_streaming)) {
			ret = -EPIPE;
			break;
		}
		atomic_set(&gc->audio0_irq_pending, 0);
		irq_status = readl(gc->bar0 + GC570D_REG_IRQ_STATUS);
		index = readl(gc->bar0 + 0x14) & 0x03;
		gc->audio0_capture_irq = irq_status | GC570D_AUDIO0_IRQ;
		gc->audio0_capture_index = index;
		if (index < 1 || index > 2) {
			ret = -EIO;
			break;
		}
		ret = gc570d_audio_take_period(
			buffer_cpu, &cursor,
			gc->audio0_capture_data +
			gc->audio0_capture_completed * GC570D_AUDIO_PERIOD_BYTES);
		if (ret == -ENODATA) {
			ret = 0;
			continue;
		}
		if (ret)
			break;
		gc->audio0_capture_completed++;
	}
	gc->audio0_capture_elapsed_us =
		ktime_us_delta(ktime_get(), capture_start);

	gc570d_audio_update_start_bit(gc, GC570D_AUDIO0_START_BIT, false);
	wake_up_interruptible(&gc->audio0_wait);
	readl_poll_timeout(gc->bar0 + GC570D_REG_IRQ_STATUS, irq_status,
			   (irq_status & GC570D_AUDIO0_TERM_IRQ) ||
			   atomic64_read(&gc->irq_audio0_term) != term_baseline,
			   100, 100000);
	writel(GC570D_AUDIO0_IRQ | GC570D_AUDIO0_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	gc570d_reset_audio_channel(gc, GC570D_AUDIO0_CHANNEL);
	gc->audio0_capture_control = readl(gc->bar0 + GC570D_AUDIO_START);

	gc->audio0_capture_size = gc->audio0_capture_completed *
				  GC570D_AUDIO_PERIOD_BYTES;
	for (i = 0; i < gc->audio0_capture_size; i++)
		changed += gc->audio0_capture_data[i] != GC570D_AUDIO_SENTINEL;
	gc->audio0_capture_changed = changed;
	if (!ret && !changed)
		ret = -ENODATA;

out_free:
	for (i = 0; i < 2; i++) {
		if (buffer_cpu[i])
			dma_free_coherent(&gc->pdev->dev,
					  GC570D_AUDIO_BUFFER_SIZE,
					  buffer_cpu[i], buffer_dma[i]);
	}
out_status:
	gc->audio0_capture_error = ret;
	dev_info(&gc->pdev->dev,
		 "HDMI IN 1 audio DMA0 periods=%u/%u result=%d irq=0x%08x index=%u changed=%zu/%zu elapsed_us=%llu\n",
		 gc->audio0_capture_completed, gc->audio0_capture_requested,
		 ret, gc->audio0_capture_irq, gc->audio0_capture_index,
		 gc->audio0_capture_changed, gc->audio0_capture_size,
		 gc->audio0_capture_elapsed_us);
	mutex_unlock(&gc->capture_lock);
	return ret;
}

static ssize_t gc570d_audio0_dma_record_write(struct file *file,
					       const char __user *buffer,
					       size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	ret = gc570d_audio0_dma_capture_periods(gc,
						GC570D_AUDIO_RECORD_PERIODS);
	return ret ? ret : count;
}

const struct file_operations gc570d_audio0_dma_record_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_audio0_dma_record_write,
	.llseek = noop_llseek,
};

static ssize_t gc570d_audio0_dma_frame_read(struct file *file,
					     char __user *buffer,
					     size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	ssize_t ret;

	mutex_lock(&gc->capture_lock);
	ret = simple_read_from_buffer(buffer, count, position,
				      gc->audio0_capture_data,
				      gc->audio0_capture_size);
	mutex_unlock(&gc->capture_lock);
	return ret;
}

const struct file_operations gc570d_audio0_dma_frame_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = gc570d_audio0_dma_frame_read,
	.llseek = default_llseek,
};

static int gc570d_audio0_dma_status_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;

	mutex_lock(&gc->capture_lock);
	seq_printf(s,
		   "error=%d irq=0x%08x index=%u control=0x%08x periods=%u/%u changed=%zu size=%zu period_bytes=%u elapsed_us=%llu\n",
		   gc->audio0_capture_error, gc->audio0_capture_irq,
		   gc->audio0_capture_index, gc->audio0_capture_control,
		   gc->audio0_capture_completed, gc->audio0_capture_requested,
		   gc->audio0_capture_changed, gc->audio0_capture_size,
		   GC570D_AUDIO_PERIOD_BYTES, gc->audio0_capture_elapsed_us);
	mutex_unlock(&gc->capture_lock);
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_audio0_dma_status);

static int gc570d_audio_dma_capture_periods(struct gc570d_dev *gc,
					     unsigned int periods)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	void *buffer_cpu[2] = { NULL, NULL };
	dma_addr_t buffer_dma[2] = { 0, 0 };
	ktime_t capture_start;
	u32 irq_status = 0;
	u32 cursor = 0;
	u32 index;
	size_t changed = 0;
	size_t i;
	int mute_ret;
	int ret;

	mutex_lock(&gc->capture_lock);
	if (gc->video_streaming || gc->video0_streaming) {
		ret = -EBUSY;
		goto out_status;
	}
	
	/* FIX: Verify ALSA has not already prepared the hardware for capture */
	if (gc->audio_prepared || gc->audio0_prepared ||
	    (readl(gc->bar0 + GC570D_AUDIO_START) & GC570D_AUDIO_START_BIT)) {
		ret = -EBUSY;
		goto out_status;
	}

	vfree(gc->audio_capture_data);
	gc->audio_capture_data = NULL;
	gc->audio_capture_size = 0;
	gc->audio_capture_changed = 0;
	gc->audio_capture_irq = 0;
	gc->audio_capture_index = 0;
	gc->audio_capture_control = 0;
	gc->audio_capture_requested = periods;
	gc->audio_capture_completed = 0;
	gc->audio_capture_elapsed_us = 0;
	gc->audio_capture_mute_error = 0;
	gc->audio_capture_error = 0;
	gc->audio_capture_data = vmalloc(array_size(periods,
						    GC570D_AUDIO_PERIOD_BYTES));
	if (!gc->audio_capture_data) {
		ret = -ENOMEM;
		goto out_status;
	}

	buffer_cpu[0] = dma_alloc_coherent(&gc->pdev->dev,
					  GC570D_AUDIO_BUFFER_SIZE,
					  &buffer_dma[0], GFP_KERNEL);
	if (!buffer_cpu[0]) {
		ret = -ENOMEM;
		goto out_status;
	}
	buffer_cpu[1] = dma_alloc_coherent(&gc->pdev->dev,
					  GC570D_AUDIO_BUFFER_SIZE,
					  &buffer_dma[1], GFP_KERNEL);
	if (!buffer_cpu[1]) {
		ret = -ENOMEM;
		goto out_free;
	}
	memset(buffer_cpu[0], GC570D_AUDIO_SENTINEL,
	       GC570D_AUDIO_BUFFER_SIZE);
	memset(buffer_cpu[1], GC570D_AUDIO_SENTINEL,
	       GC570D_AUDIO_BUFFER_SIZE);

	gc570d_reset_audio_channel(gc, GC570D_AUDIO_CHANNEL);
	writel(0, gc->bar0 + GC570D_AUDIO_FORMAT);
	writel(0, gc->bar0 + GC570D_AUDIO_GLOBAL_FORMAT);
	writel(480, gc->bar0 + GC570D_AUDIO_RATE);
	writel(lower_32_bits(buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO_BUFFER0);
	writel(upper_32_bits(buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO_BUFFER0 + 4);
	writel(lower_32_bits(buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO_BUFFER1);
	writel(upper_32_bits(buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO_BUFFER1 + 4);
	dma_wmb();

	writel(GC570D_AUDIO_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
	pci_set_master(gc->pdev);
	capture_start = ktime_get();
	gc570d_audio_update_start_bit(gc, GC570D_AUDIO_START_BIT, true);
	ret = 0;
	while (gc->audio_capture_completed < periods) {
		ret = readl_poll_timeout(gc->bar0 + GC570D_REG_IRQ_STATUS,
					 irq_status,
					 irq_status & GC570D_AUDIO_IRQ,
					 100, 500000);
		if (ret)
			break;

		index = (readl(gc->bar0 + 0x14) >>
			 (GC570D_AUDIO_CHANNEL * 2)) & 0x03;
		gc->audio_capture_irq = irq_status;
		gc->audio_capture_index = index;
		if (index < 1 || index > 2) {
			ret = -EIO;
			break;
		}

		ret = gc570d_audio_take_period(
			buffer_cpu, &cursor,
			gc->audio_capture_data +
			gc->audio_capture_completed * GC570D_AUDIO_PERIOD_BYTES);
		if (ret)
			break;
		gc->audio_capture_completed++;
		writel(GC570D_AUDIO_IRQ,
		       gc->bar0 + GC570D_REG_IRQ_STATUS);
	}
	gc->audio_capture_elapsed_us =
		ktime_us_delta(ktime_get(), capture_start);

	mute_ret = gc570d_receiver_update8(gc, bus, 0x52, 0x1f, 0x1f);
	gc->audio_capture_mute_error = mute_ret;
	gc570d_audio_update_start_bit(gc, GC570D_AUDIO_START_BIT, false);
	writel(GC570D_AUDIO_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
	gc570d_reset_audio_channel(gc, GC570D_AUDIO_CHANNEL);
	pci_clear_master(gc->pdev);
	gc->audio_capture_control = readl(gc->bar0 + GC570D_AUDIO_START);

	gc->audio_capture_size = gc->audio_capture_completed *
				 GC570D_AUDIO_PERIOD_BYTES;
	for (i = 0; i < gc->audio_capture_size; i++)
		changed += gc->audio_capture_data[i] != GC570D_AUDIO_SENTINEL;
	gc->audio_capture_changed = changed;
	if (!ret && !changed)
		ret = -ENODATA;

out_free:
	if (buffer_cpu[1])
		dma_free_coherent(&gc->pdev->dev, GC570D_AUDIO_BUFFER_SIZE,
				  buffer_cpu[1], buffer_dma[1]);
	if (buffer_cpu[0])
		dma_free_coherent(&gc->pdev->dev, GC570D_AUDIO_BUFFER_SIZE,
				  buffer_cpu[0], buffer_dma[0]);
out_status:
	gc->audio_capture_error = ret;
	dev_info(&gc->pdev->dev,
		 "audio DMA capture periods=%u/%u result=%d irq=0x%08x index=%u changed=%zu/%zu elapsed_us=%llu\n",
		 gc->audio_capture_completed, gc->audio_capture_requested,
		 ret, gc->audio_capture_irq, gc->audio_capture_index,
		 gc->audio_capture_changed, gc->audio_capture_size,
		 gc->audio_capture_elapsed_us);
	mutex_unlock(&gc->capture_lock);
	return ret;
}

static ssize_t gc570d_audio_dma_capture_write(struct file *file,
					       const char __user *buffer,
					       size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	ret = gc570d_audio_dma_capture_periods(gc, 1);
	return ret ? ret : count;
}

const struct file_operations gc570d_audio_dma_capture_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_audio_dma_capture_write,
	.llseek = noop_llseek,
};

static ssize_t gc570d_audio_dma_record_write(struct file *file,
					      const char __user *buffer,
					      size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	ret = gc570d_audio_dma_capture_periods(gc,
					       GC570D_AUDIO_RECORD_PERIODS);
	return ret ? ret : count;
}

const struct file_operations gc570d_audio_dma_record_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_audio_dma_record_write,
	.llseek = noop_llseek,
};

static ssize_t gc570d_audio_dma_frame_read(struct file *file,
					    char __user *buffer,
					    size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	ssize_t ret;

	mutex_lock(&gc->capture_lock);
	ret = simple_read_from_buffer(buffer, count, position,
				      gc->audio_capture_data,
				      gc->audio_capture_size);
	mutex_unlock(&gc->capture_lock);
	return ret;
}

const struct file_operations gc570d_audio_dma_frame_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = gc570d_audio_dma_frame_read,
	.llseek = default_llseek,
};

static ssize_t gc570d_audio_dma_period_read(struct file *file,
					     char __user *buffer,
					     size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	ssize_t ret;

	mutex_lock(&gc->capture_lock);
	if (!gc->audio_capture_data ||
	    gc->audio_capture_size < GC570D_AUDIO_PERIOD_BYTES) {
		ret = -ENODATA;
		goto out_unlock;
	}
	ret = simple_read_from_buffer(buffer, count, position,
				      gc->audio_capture_data,
				      GC570D_AUDIO_PERIOD_BYTES);

out_unlock:
	mutex_unlock(&gc->capture_lock);
	return ret;
}

const struct file_operations gc570d_audio_dma_period_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = gc570d_audio_dma_period_read,
	.llseek = default_llseek,
};

static int gc570d_audio_dma_status_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;

	mutex_lock(&gc->capture_lock);
	seq_printf(s,
		   "error=%d mute_error=%d irq=0x%08x index=%u control=0x%08x periods=%u/%u changed=%zu size=%zu period_bytes=%u elapsed_us=%llu\n",
		   gc->audio_capture_error, gc->audio_capture_mute_error,
		   gc->audio_capture_irq, gc->audio_capture_index,
		   gc->audio_capture_control, gc->audio_capture_completed,
		   gc->audio_capture_requested,
		   gc->audio_capture_changed, gc->audio_capture_size,
		   GC570D_AUDIO_PERIOD_BYTES, gc->audio_capture_elapsed_us);
	mutex_unlock(&gc->capture_lock);
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_audio_dma_status);

static int gc570d_dummy_capture_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	/* Two-channel readback so the stereo switch reports both as ON. */
	ucontrol->value.integer.value[0] = 1;
	ucontrol->value.integer.value[1] = 1;
	return 0;
}

static int gc570d_dummy_capture_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	return 0; /* Ignore writes from the OS to keep it permanently ON */
}

/*
 * Stereo capture switch.  PipeWire's ALSA Card Profile (ACP) subsystem
 * classifies the input path by the channel count of the capture switch
 * and volume elements; a mono switch on a stereo PCM is treated as a
 * mono-only input, which causes ACP to publish only the generic
 * "pro-audio" profile.  Advertising two channels here and providing a
 * matching stereo Capture Volume below lets ACP build the standard
 * input:analog-stereo profile, so PipeWire and PulseAudio clients see
 * a normal stereo capture device.
 */
static const struct snd_kcontrol_new gc570d_capture_switch = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Capture Switch",
	.info = snd_ctl_boolean_stereo_info,
	.get = gc570d_dummy_capture_get,
	.put = gc570d_dummy_capture_put,
};

/*
 * The GC570D has no hardware capture gain: the HDMI receiver delivers
 * a fixed-level PCM stream at unity.  ACP nevertheless requires a
 * "Capture Volume" element before it will publish input:analog-stereo,
 * so expose a stereo integer control that accepts and ignores writes.
 * Clients that present a volume slider for the device will move it
 * without affecting the captured samples.
 */
static int gc570d_dummy_capture_volume_info(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;          /* stereo */
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 100;
	uinfo->value.integer.step = 1;
	return 0;
}

static int gc570d_dummy_capture_volume_get(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = 100;
	ucontrol->value.integer.value[1] = 100;
	return 0;
}

static int gc570d_dummy_capture_volume_put(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	/* Hardware has no capture gain; accept and ignore the request. */
	return 0;
}

static const struct snd_kcontrol_new gc570d_capture_volume = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Capture Volume",
	.info = gc570d_dummy_capture_volume_info,
	.get = gc570d_dummy_capture_volume_get,
	.put = gc570d_dummy_capture_volume_put,
};

static const struct snd_pcm_hardware gc570d_audio_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = GC570D_AUDIO_RATE_HZ,
	.rate_max = GC570D_AUDIO_RATE_HZ,
	.channels_min = GC570D_AUDIO_CHANNELS,
	.channels_max = GC570D_AUDIO_CHANNELS,
	.buffer_bytes_max = GC570D_AUDIO_PCM_BUFFER_MAX,
	.period_bytes_min = GC570D_AUDIO_PERIOD_BYTES,
	.period_bytes_max = GC570D_AUDIO_PERIOD_BYTES,
	.periods_min = 2,
	.periods_max = GC570D_AUDIO_PCM_BUFFER_MAX /
		       GC570D_AUDIO_PERIOD_BYTES,
};

int gc570d_audio_recover_locked(struct gc570d_dev *gc)
{
	int i;
	int ret;

	if (!READ_ONCE(gc->audio_prepared) ||
	    !READ_ONCE(gc->audio_running) ||
	    !READ_ONCE(gc->audio_recovering))
		return 0;

	ret = gc570d_it6802_audio_output_set(gc, true);
	if (ret)
		return ret;

	gc570d_audio_update_start_bit(gc, GC570D_AUDIO_START_BIT, false);
	writel(GC570D_AUDIO_IRQ | GC570D_AUDIO_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	gc570d_reset_audio_channel(gc, GC570D_AUDIO_CHANNEL);

	for (i = 0; i < 2; i++)
		memset(gc->audio_pcm_buffer_cpu[i], GC570D_AUDIO_SENTINEL,
		       GC570D_AUDIO_BUFFER_SIZE);
	writel(0, gc->bar0 + GC570D_AUDIO_FORMAT);
	writel(0, gc->bar0 + GC570D_AUDIO_GLOBAL_FORMAT);
	writel(GC570D_AUDIO_PERIOD_FRAMES, gc->bar0 + GC570D_AUDIO_RATE);
	writel(lower_32_bits(gc->audio_pcm_buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO_BUFFER0);
	writel(upper_32_bits(gc->audio_pcm_buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO_BUFFER0 + 4);
	writel(lower_32_bits(gc->audio_pcm_buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO_BUFFER1);
	writel(upper_32_bits(gc->audio_pcm_buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO_BUFFER1 + 4);
	gc->audio_pcm_cursor = 0;
	dma_wmb();

	atomic_set(&gc->audio_irq_pending, 0);
	writel(GC570D_AUDIO_IRQ | GC570D_AUDIO_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	gc->audio_term_baseline = atomic64_read(&gc->irq_audio_term);
	gc->audio_stop_pending = true;
	if (!READ_ONCE(gc->audio_running))
		return -ESHUTDOWN;
	gc570d_audio_update_start_bit(gc, GC570D_AUDIO_START_BIT, true);
	gc->audio_thread_error = 0;
	WRITE_ONCE(gc->audio_recovering, false);
	wake_up_interruptible(&gc->audio_wait);

	dev_info(&gc->pdev->dev,
		 "HDMI IN 2 audio signal stable; restarted the open PCM capture\n");
	return 1;
}

static int gc570d_audio_pcm_thread(void *data)
{
	struct gc570d_dev *gc = data;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	u8 *period; /* Changed from an array to a pointer */
	u32 index;
	size_t buffer_bytes;
	size_t first;
	size_t position;
	unsigned long next_recovery = 0;
	long waited;
	int ret;

	/* Allocate the buffer on the kernel heap instead of the stack */
	period = kzalloc(GC570D_AUDIO_PERIOD_BYTES, GFP_KERNEL);
	if (!period)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		wait_event_interruptible(gc->audio_wait,
			kthread_should_stop() || READ_ONCE(gc->audio_running));
		if (kthread_should_stop())
			break;

		/* If HDMI cable is disconnected, deliver pure digital silence */
		if (READ_ONCE(gc->video_no_signal) ||
		    READ_ONCE(gc->audio_recovering)) {
			if (!READ_ONCE(gc->audio_recovering)) {
				WRITE_ONCE(gc->audio_recovering, true);
				atomic_set(&gc->audio_irq_pending, 0);
				gc570d_audio_update_start_bit(
					gc, GC570D_AUDIO_START_BIT, false);
				writel(GC570D_AUDIO_IRQ,
				       gc->bar0 + GC570D_REG_IRQ_STATUS);
			}
			memset(period, 0, GC570D_AUDIO_PERIOD_BYTES);
			substream = READ_ONCE(gc->audio_substream);
			if (substream && substream->runtime) {
				runtime = substream->runtime;
				buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
				position = READ_ONCE(gc->audio_pcm_pos);
				first = min_t(size_t, GC570D_AUDIO_PERIOD_BYTES,
					      buffer_bytes - position);
				memcpy(runtime->dma_area + position, period, first);
				if (first < GC570D_AUDIO_PERIOD_BYTES)
					memcpy(runtime->dma_area, period + first,
					       GC570D_AUDIO_PERIOD_BYTES - first);
				position = (position + GC570D_AUDIO_PERIOD_BYTES) % buffer_bytes;
				WRITE_ONCE(gc->audio_pcm_pos, position);
				snd_pcm_period_elapsed(substream);
			}
			if (!READ_ONCE(gc->video_no_signal) &&
			    time_after_eq(jiffies, next_recovery) &&
			    mutex_trylock(&gc->capture_lock)) {
				ret = gc570d_audio_recover_locked(gc);
				mutex_unlock(&gc->capture_lock);
				next_recovery = jiffies + msecs_to_jiffies(250);
				if (ret > 0)
					continue;
			}
			/* Pace silence to match 48kHz stereo timing (~10.6ms per period) */
			usleep_range(10000, 11000);
			continue;
		}

		waited = wait_event_interruptible_timeout(
			gc->audio_wait,
			kthread_should_stop() ||
			!READ_ONCE(gc->audio_running) ||
			READ_ONCE(gc->video_no_signal) ||
			READ_ONCE(gc->audio_recovering) ||
			atomic_read(&gc->audio_irq_pending) ||
			(readl(gc->bar0 + GC570D_REG_IRQ_STATUS) &
			 GC570D_AUDIO_IRQ),
			msecs_to_jiffies(200));
		if (kthread_should_stop())
			break;
		if (!READ_ONCE(gc->audio_running) ||
		    READ_ONCE(gc->video_no_signal) ||
		    READ_ONCE(gc->audio_recovering))
			continue;
		if (waited <= 0) {
			gc->audio_thread_error = waited ? (int)waited : -ETIMEDOUT;
			WRITE_ONCE(gc->audio_recovering, true);
			atomic_set(&gc->audio_irq_pending, 0);
			gc570d_audio_update_start_bit(gc,
						      GC570D_AUDIO_START_BIT,
						      false);
			writel(GC570D_AUDIO_IRQ,
			       gc->bar0 + GC570D_REG_IRQ_STATUS);
			dev_warn(&gc->pdev->dev,
				 "HDMI IN 2 audio DMA paused; keeping the open PCM alive with silence until the signal returns\n");
			continue;
		}
		atomic_set(&gc->audio_irq_pending, 0);
		writel(GC570D_AUDIO_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);

		index = (readl(gc->bar0 + 0x14) >>
			 (GC570D_AUDIO_CHANNEL * 2)) & 0x03;
		if (index < 1 || index > 2) {
			ret = -EIO;
			goto xrun;
		}
		/* Drain all ready periods from the 2x64KB hardware staging buffers per wakeup. */
		while ((ret = gc570d_audio_take_period(gc->audio_pcm_buffer_cpu,
						       &gc->audio_pcm_cursor,
						       period)) == 0) {
			substream = READ_ONCE(gc->audio_substream);
			if (!substream || !substream->runtime) {
				ret = -ENODEV;
				goto xrun;
			}
			runtime = substream->runtime;
			buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
			position = READ_ONCE(gc->audio_pcm_pos);
			first = min_t(size_t, GC570D_AUDIO_PERIOD_BYTES,
				      buffer_bytes - position);
			memcpy(runtime->dma_area + position, period, first);
			if (first < GC570D_AUDIO_PERIOD_BYTES)
				memcpy(runtime->dma_area, period + first,
				       GC570D_AUDIO_PERIOD_BYTES - first);
			position = (position + GC570D_AUDIO_PERIOD_BYTES) % buffer_bytes;
			WRITE_ONCE(gc->audio_pcm_pos, position);
			snd_pcm_period_elapsed(substream);
		}
		/* INTx can repeat before a new staging period becomes visible. */
		if (ret && ret != -ENODATA)
			goto xrun;
		continue;

xrun:
		gc->audio_thread_error = ret;
		WRITE_ONCE(gc->audio_running, false);
		WRITE_ONCE(gc->audio_recovering, false);
		atomic_set(&gc->audio_irq_pending, 0);
		gc570d_audio_update_start_bit(gc, GC570D_AUDIO_START_BIT,
					      false);
		writel(GC570D_AUDIO_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
		substream = READ_ONCE(gc->audio_substream);
		if (substream)
			snd_pcm_stop_xrun(substream);
	}

	/* Free the memory before the thread exits */
	kfree(period);
	return 0;
}

static int gc570d_audio_pcm_open(struct snd_pcm_substream *substream)
{
	int ret;

	substream->runtime->hw = gc570d_audio_pcm_hardware;
	ret = snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	return ret;
}

static int gc570d_audio_pcm_close(struct snd_pcm_substream *substream)
{
	return 0;
}

static void gc570d_audio_finish_stop(struct gc570d_dev *gc)
{
	u32 status = readl(gc->bar0 + GC570D_REG_IRQ_STATUS);
	int ret = 0;

	if (gc->audio_stop_pending) {
		ret = readl_poll_timeout(gc->bar0 + GC570D_REG_IRQ_STATUS,
					 status,
					 (status & GC570D_AUDIO_TERM_IRQ) ||
					 atomic64_read(&gc->irq_audio_term) !=
					 gc->audio_term_baseline,
					 100, 100000);
		if (status & GC570D_AUDIO_TERM_IRQ)
			atomic64_inc(&gc->irq_audio_term);
		gc->audio_stop_pending = false;
	}

	/* These events are W1C and must not survive into the next stream. */
	writel(GC570D_AUDIO_IRQ | GC570D_AUDIO_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	if (ret)
		dev_warn(&gc->pdev->dev,
			 "audio DMA termination was not observed before release\n");
}

static int gc570d_audio_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct gc570d_dev *gc = snd_pcm_substream_chip(substream);
	struct task_struct *thread;
	int i;

	WRITE_ONCE(gc->audio_running, false);
	WRITE_ONCE(gc->audio_recovering, false);
	atomic_set(&gc->audio_irq_pending, 0);
	gc570d_audio_update_start_bit(gc, GC570D_AUDIO_START_BIT, false);
	writel(GC570D_AUDIO_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
	wake_up_interruptible(&gc->audio_wait);
	thread = gc->audio_thread;
	gc->audio_thread = NULL;
	if (thread)
		kthread_stop(thread);

	mutex_lock(&gc->capture_lock);
	gc570d_audio_finish_stop(gc);
	for (i = 0; i < 2; i++) {
		if (gc->audio_pcm_buffer_cpu[i])
			dma_free_coherent(&gc->pdev->dev,
					  GC570D_AUDIO_BUFFER_SIZE,
					  gc->audio_pcm_buffer_cpu[i],
					  gc->audio_pcm_buffer_dma[i]);
		gc->audio_pcm_buffer_cpu[i] = NULL;
	}
	if (gc->audio_irq_active) {
		gc570d_video_irq_put_locked(gc);
		gc->audio_irq_active = false;
	}
	if (gc->audio_substream == substream)
		gc->audio_substream = NULL;
	gc->audio_prepared = false;
	if (!gc->video_streaming && !gc->video0_streaming &&
	    !gc->audio0_prepared)
		pci_clear_master(gc->pdev);
	mutex_unlock(&gc->capture_lock);

	return 0;
}

static int gc570d_audio_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct gc570d_dev *gc = snd_pcm_substream_chip(substream);
	bool no_signal = false;
	int i;
	int ret;

	mutex_lock(&gc->capture_lock);
	if (READ_ONCE(gc->suspended)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	if (gc->audio_prepared) {
		gc570d_audio_update_start_bit(gc, GC570D_AUDIO_START_BIT,
					      false);
		for (i = 0; i < 2; i++) {
			if (gc->audio_pcm_buffer_cpu[i])
				memset(gc->audio_pcm_buffer_cpu[i],
				       GC570D_AUDIO_SENTINEL,
				       GC570D_AUDIO_BUFFER_SIZE);
		}
		gc570d_reset_audio_channel(gc, GC570D_AUDIO_CHANNEL);
		writel(GC570D_AUDIO_IRQ | GC570D_AUDIO_TERM_IRQ,
		       gc->bar0 + GC570D_REG_IRQ_STATUS);
		writel(0, gc->bar0 + GC570D_AUDIO_FORMAT);
		writel(0, gc->bar0 + GC570D_AUDIO_GLOBAL_FORMAT);
		writel(GC570D_AUDIO_PERIOD_FRAMES,
		       gc->bar0 + GC570D_AUDIO_RATE);
		writel(lower_32_bits(gc->audio_pcm_buffer_dma[0]),
		       gc->bar0 + GC570D_AUDIO_BUFFER0);
		writel(upper_32_bits(gc->audio_pcm_buffer_dma[0]),
		       gc->bar0 + GC570D_AUDIO_BUFFER0 + 4);
		writel(lower_32_bits(gc->audio_pcm_buffer_dma[1]),
		       gc->bar0 + GC570D_AUDIO_BUFFER1);
		writel(upper_32_bits(gc->audio_pcm_buffer_dma[1]),
		       gc->bar0 + GC570D_AUDIO_BUFFER1 + 4);
		dma_wmb();
		gc->audio_pcm_pos = 0;
		gc->audio_pcm_cursor = 0;
		ret = 0;
		goto out_unlock;
	}

	/*
	 * ALSA and V4L2 are independent pins.  PipeWire may prepare this PCM
	 * before OBS starts HDMI IN 2 video, so establish the receiver state and
	 * take an independent reference to the shared bridge IRQ.
	 * Allow prepare to succeed in recovering/silence mode if no signal is
	 * present yet.
	 */
	if (!gc->video_streaming) {
		ret = gc570d_it6802_output_enable(gc);
		if (ret && !gc570d_audio_is_no_signal_error(ret))
			goto out_unlock;
		if (ret) {
			no_signal = true;
		} else {
			ret = gc570d_it6802_format_init(gc);
			if (ret && !gc570d_audio_is_no_signal_error(ret))
				goto out_unlock;
			no_signal = ret != 0;
		}
	} else if (READ_ONCE(gc->video_no_signal)) {
		no_signal = true;
	}
	if (!no_signal) {
		ret = gc570d_it6802_audio_output_set(gc, true);
		if (ret && !gc570d_audio_is_no_signal_error(ret))
			goto out_unlock;
		no_signal = ret != 0;
	}
	for (i = 0; i < 2; i++) {
		gc->audio_pcm_buffer_cpu[i] =
			dma_alloc_coherent(&gc->pdev->dev,
					   GC570D_AUDIO_BUFFER_SIZE,
					   &gc->audio_pcm_buffer_dma[i],
					   GFP_KERNEL);
		if (!gc->audio_pcm_buffer_cpu[i]) {
			ret = -ENOMEM;
			goto out_free;
		}
		memset(gc->audio_pcm_buffer_cpu[i], GC570D_AUDIO_SENTINEL,
		       GC570D_AUDIO_BUFFER_SIZE);
	}

	gc570d_reset_audio_channel(gc, GC570D_AUDIO_CHANNEL);
	writel(GC570D_AUDIO_IRQ | GC570D_AUDIO_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	writel(0, gc->bar0 + GC570D_AUDIO_FORMAT);
	writel(0, gc->bar0 + GC570D_AUDIO_GLOBAL_FORMAT);
	writel(GC570D_AUDIO_PERIOD_FRAMES, gc->bar0 + GC570D_AUDIO_RATE);
	writel(lower_32_bits(gc->audio_pcm_buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO_BUFFER0);
	writel(upper_32_bits(gc->audio_pcm_buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO_BUFFER0 + 4);
	writel(lower_32_bits(gc->audio_pcm_buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO_BUFFER1);
	writel(upper_32_bits(gc->audio_pcm_buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO_BUFFER1 + 4);
	dma_wmb();

	gc->audio_pcm_pos = 0;
	gc->audio_pcm_cursor = 0;
	gc->audio_thread_error = 0;
	WRITE_ONCE(gc->audio_recovering, no_signal);
	atomic_set(&gc->audio_irq_pending, 0);
	gc->audio_substream = substream;
	gc->audio_prepared = true;
	pci_set_master(gc->pdev);
	gc570d_video_irq_get_locked(gc);
	gc->audio_irq_active = true;
	gc->audio_thread = kthread_run(gc570d_audio_pcm_thread, gc,
				       "gc570d-audio1");
	if (IS_ERR(gc->audio_thread)) {
		ret = PTR_ERR(gc->audio_thread);
		gc->audio_thread = NULL;
		gc->audio_substream = NULL;
		gc->audio_prepared = false;
		WRITE_ONCE(gc->audio_recovering, false);
		if (gc->audio_irq_active) {
			gc570d_video_irq_put_locked(gc);
			gc->audio_irq_active = false;
		}
		if (!gc->video_streaming && !gc->video0_streaming &&
		    !gc->audio0_prepared)
			pci_clear_master(gc->pdev);
		goto out_free;
	}
	mutex_unlock(&gc->capture_lock);
	return 0;

out_free:
	for (i = 0; i < 2; i++) {
		if (gc->audio_pcm_buffer_cpu[i])
			dma_free_coherent(&gc->pdev->dev,
					  GC570D_AUDIO_BUFFER_SIZE,
					  gc->audio_pcm_buffer_cpu[i],
					  gc->audio_pcm_buffer_dma[i]);
		gc->audio_pcm_buffer_cpu[i] = NULL;
	}
out_unlock:
	mutex_unlock(&gc->capture_lock);
	return ret;
}

static int gc570d_audio_pcm_trigger(struct snd_pcm_substream *substream,
				    int command)
{
	struct gc570d_dev *gc = snd_pcm_substream_chip(substream);

	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!READ_ONCE(gc->audio_prepared))
			return -EIO;
		atomic_set(&gc->audio_irq_pending, 0);
		writel(GC570D_AUDIO_IRQ | GC570D_AUDIO_TERM_IRQ,
		       gc->bar0 + GC570D_REG_IRQ_STATUS);
		gc->audio_term_baseline = atomic64_read(&gc->irq_audio_term);
		gc->audio_stop_pending = true;
		WRITE_ONCE(gc->audio_running, true);
		if (!READ_ONCE(gc->audio_recovering) &&
		    !READ_ONCE(gc->video_no_signal))
			gc570d_audio_update_start_bit(gc,
						      GC570D_AUDIO_START_BIT,
						      true);
		wake_up_interruptible(&gc->audio_wait);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(gc->audio_running, false);
		atomic_set(&gc->audio_irq_pending, 0);
		gc570d_audio_update_start_bit(gc, GC570D_AUDIO_START_BIT,
					      false);
		writel(GC570D_AUDIO_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
		wake_up_interruptible(&gc->audio_wait);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t
gc570d_audio_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct gc570d_dev *gc = snd_pcm_substream_chip(substream);

	return bytes_to_frames(substream->runtime,
			       READ_ONCE(gc->audio_pcm_pos));
}

static const struct snd_pcm_ops gc570d_audio_pcm_ops = {
	.open = gc570d_audio_pcm_open,
	.close = gc570d_audio_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_free = gc570d_audio_pcm_hw_free,
	.prepare = gc570d_audio_pcm_prepare,
	.trigger = gc570d_audio_pcm_trigger,
	.pointer = gc570d_audio_pcm_pointer,
};

/*
 * HDMI IN 1 follows the Windows receiver state machine after a link change:
 * VideoStable sends audio back through RequestAudio, WaitForReady and AudioOn.
 * capture_lock is held by the caller and the coherent staging buffers remain
 * allocated for the lifetime of the prepared PCM.
 */
int gc570d_audio0_recover_locked(struct gc570d_dev *gc)
{
	int i;
	int ret;

	if (!READ_ONCE(gc->audio0_prepared) ||
	    !READ_ONCE(gc->audio0_running) ||
	    !READ_ONCE(gc->audio0_recovering))
		return 0;

	ret = gc570d_it68051_audio_output_set(gc, true);
	if (ret)
		return ret;

	gc570d_audio_update_start_bit(gc, GC570D_AUDIO0_START_BIT, false);
	writel(GC570D_AUDIO0_IRQ | GC570D_AUDIO0_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	gc570d_reset_audio_channel(gc, GC570D_AUDIO0_CHANNEL);

	for (i = 0; i < 2; i++)
		memset(gc->audio0_pcm_buffer_cpu[i], GC570D_AUDIO_SENTINEL,
		       GC570D_AUDIO_BUFFER_SIZE);
	writel(0, gc->bar0 + GC570D_AUDIO0_FORMAT);
	writel(0, gc->bar0 + GC570D_AUDIO_GLOBAL_FORMAT);
	writel(GC570D_AUDIO_PERIOD_FRAMES, gc->bar0 + GC570D_AUDIO0_RATE);
	writel(lower_32_bits(gc->audio0_pcm_buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER0);
	writel(upper_32_bits(gc->audio0_pcm_buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER0 + 4);
	writel(lower_32_bits(gc->audio0_pcm_buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER1);
	writel(upper_32_bits(gc->audio0_pcm_buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER1 + 4);
	gc->audio0_pcm_cursor = 0;
	dma_wmb();

	atomic_set(&gc->audio0_irq_pending, 0);
	writel(GC570D_AUDIO0_IRQ | GC570D_AUDIO0_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	gc->audio0_term_baseline = atomic64_read(&gc->irq_audio0_term);
	gc->audio0_stop_pending = true;
	if (!READ_ONCE(gc->audio0_running))
		return -ESHUTDOWN;
	gc570d_audio_update_start_bit(gc, GC570D_AUDIO0_START_BIT, true);
	gc->audio0_thread_error = 0;
	WRITE_ONCE(gc->audio0_recovering, false);
	wake_up_interruptible(&gc->audio0_wait);

	dev_info(&gc->pdev->dev,
		 "HDMI IN 1 audio signal stable; restarted the open PCM capture\n");
	return 1;
}

static int gc570d_audio0_pcm_push_period(struct gc570d_dev *gc,
					 const u8 *period)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	size_t buffer_bytes;
	size_t first;
	size_t position;

	substream = READ_ONCE(gc->audio0_substream);
	if (!substream || !substream->runtime)
		return -ENODEV;
	runtime = substream->runtime;
	buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
	position = READ_ONCE(gc->audio0_pcm_pos);
	first = min_t(size_t, GC570D_AUDIO_PERIOD_BYTES,
		      buffer_bytes - position);
	memcpy(runtime->dma_area + position, period, first);
	if (first < GC570D_AUDIO_PERIOD_BYTES)
		memcpy(runtime->dma_area, period + first,
		       GC570D_AUDIO_PERIOD_BYTES - first);
	position = (position + GC570D_AUDIO_PERIOD_BYTES) % buffer_bytes;
	WRITE_ONCE(gc->audio0_pcm_pos, position);
	snd_pcm_period_elapsed(substream);
	return 0;
}

static int gc570d_audio0_pcm_thread(void *data)
{
	struct gc570d_dev *gc = data;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	u8 *period;
	u32 index;
	size_t buffer_bytes;
	size_t first;
	size_t position;
	unsigned long next_recovery = 0;
	long waited;
	int ret;

	period = kzalloc(GC570D_AUDIO_PERIOD_BYTES, GFP_KERNEL);
	if (!period)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		wait_event_interruptible(gc->audio0_wait,
			kthread_should_stop() || READ_ONCE(gc->audio0_running));
		if (kthread_should_stop())
			break;

		/*
		 * Keep the ALSA/PipeWire ring fed with silence while recovering,
		 * and actively probe for audio recovery when HDMI 1 video SCDT is
		 * already locked (upstream commit 4673bb9).
		 */
		if (READ_ONCE(gc->video0_no_signal) ||
		    READ_ONCE(gc->audio0_recovering)) {
			if (!READ_ONCE(gc->audio0_recovering)) {
				WRITE_ONCE(gc->audio0_recovering, true);
				atomic_set(&gc->audio0_irq_pending, 0);
				gc570d_audio_update_start_bit(
					gc, GC570D_AUDIO0_START_BIT, false);
				writel(GC570D_AUDIO0_IRQ,
				       gc->bar0 + GC570D_REG_IRQ_STATUS);
			}
			memset(period, 0, GC570D_AUDIO_PERIOD_BYTES);
			substream = READ_ONCE(gc->audio0_substream);
			if (substream && substream->runtime) {
				runtime = substream->runtime;
				buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
				position = READ_ONCE(gc->audio0_pcm_pos);
				first = min_t(size_t, GC570D_AUDIO_PERIOD_BYTES,
					      buffer_bytes - position);
				memcpy(runtime->dma_area + position, period, first);
				if (first < GC570D_AUDIO_PERIOD_BYTES)
					memcpy(runtime->dma_area, period + first,
					       GC570D_AUDIO_PERIOD_BYTES - first);
				position = (position + GC570D_AUDIO_PERIOD_BYTES) % buffer_bytes;
				WRITE_ONCE(gc->audio0_pcm_pos, position);
				snd_pcm_period_elapsed(substream);
			}
			if (!READ_ONCE(gc->video0_no_signal) &&
			    READ_ONCE(gc->video0_receiver_output_valid) &&
			    time_after_eq(jiffies, next_recovery) &&
			    mutex_trylock(&gc->capture_lock)) {
				ret = gc570d_audio0_recover_locked(gc);
				mutex_unlock(&gc->capture_lock);
				next_recovery = jiffies + msecs_to_jiffies(250);
				if (ret > 0)
					continue;
			}
			usleep_range(10000, 11000);
			continue;
		}

		waited = wait_event_interruptible_timeout(
			gc->audio0_wait,
			kthread_should_stop() ||
			!READ_ONCE(gc->audio0_running) ||
			READ_ONCE(gc->video0_no_signal) ||
			READ_ONCE(gc->audio0_recovering) ||
			atomic_read(&gc->audio0_irq_pending) ||
			(readl(gc->bar0 + GC570D_REG_IRQ_STATUS) &
			 GC570D_AUDIO0_IRQ),
			msecs_to_jiffies(100));
		if (kthread_should_stop())
			break;
		if (!READ_ONCE(gc->audio0_running) ||
		    READ_ONCE(gc->video0_no_signal) ||
		    READ_ONCE(gc->audio0_recovering))
			continue;
		if (waited <= 0) {
			gc->audio0_thread_error = waited ? (int)waited : -ETIMEDOUT;
			WRITE_ONCE(gc->audio0_recovering, true);
			atomic_set(&gc->audio0_irq_pending, 0);
			gc570d_audio_update_start_bit(gc,
						      GC570D_AUDIO0_START_BIT,
						      false);
			writel(GC570D_AUDIO0_IRQ,
			       gc->bar0 + GC570D_REG_IRQ_STATUS);
			dev_warn(&gc->pdev->dev,
				 "HDMI IN 1 audio DMA paused; keeping the open PCM alive with silence until the signal returns\n");
			continue;
		}
		atomic_set(&gc->audio0_irq_pending, 0);
		writel(GC570D_AUDIO0_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);

		index = readl(gc->bar0 + 0x14) & 0x03;
		if (index < 1 || index > 2) {
			ret = -EIO;
			goto xrun;
		}
		/* Drain all ready periods from the 2x64KB hardware staging buffers per wakeup. */
		while ((ret = gc570d_audio_take_period(gc->audio0_pcm_buffer_cpu,
						       &gc->audio0_pcm_cursor,
						       period)) == 0) {
			ret = gc570d_audio0_pcm_push_period(gc, period);
			if (ret)
				goto xrun;
		}
		if (ret && ret != -ENODATA)
			goto xrun;
		continue;

xrun:
		gc->audio0_thread_error = ret;
		WRITE_ONCE(gc->audio0_running, false);
		WRITE_ONCE(gc->audio0_recovering, false);
		atomic_set(&gc->audio0_irq_pending, 0);
		gc570d_audio_update_start_bit(gc, GC570D_AUDIO0_START_BIT,
					      false);
		writel(GC570D_AUDIO0_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
		substream = READ_ONCE(gc->audio0_substream);
		if (substream)
			snd_pcm_stop_xrun(substream);
	}

	kfree(period);
	return 0;
}

static int gc570d_audio0_pcm_open(struct snd_pcm_substream *substream)
{
	int ret;

	substream->runtime->hw = gc570d_audio_pcm_hardware;
	ret = snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	return ret;
}

static int gc570d_audio0_pcm_close(struct snd_pcm_substream *substream)
{
	return 0;
}

static void gc570d_audio0_finish_stop(struct gc570d_dev *gc)
{
	u32 status = readl(gc->bar0 + GC570D_REG_IRQ_STATUS);
	int ret = 0;

	if (gc->audio0_stop_pending) {
		ret = readl_poll_timeout(gc->bar0 + GC570D_REG_IRQ_STATUS,
					 status,
					 (status & GC570D_AUDIO0_TERM_IRQ) ||
					 atomic64_read(&gc->irq_audio0_term) !=
					 gc->audio0_term_baseline,
					 100, 100000);
		if (status & GC570D_AUDIO0_TERM_IRQ)
			atomic64_inc(&gc->irq_audio0_term);
		gc->audio0_stop_pending = false;
	}

	writel(GC570D_AUDIO0_IRQ | GC570D_AUDIO0_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	if (ret)
		dev_warn(&gc->pdev->dev,
			 "HDMI IN 1 audio DMA termination was not observed before release\n");
}

static int gc570d_audio0_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct gc570d_dev *gc = snd_pcm_substream_chip(substream);
	struct task_struct *thread;
	int i;

	WRITE_ONCE(gc->audio0_running, false);
	WRITE_ONCE(gc->audio0_recovering, false);
	atomic_set(&gc->audio0_irq_pending, 0);
	gc570d_audio_update_start_bit(gc, GC570D_AUDIO0_START_BIT, false);
	writel(GC570D_AUDIO0_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
	wake_up_interruptible(&gc->audio0_wait);
	thread = gc->audio0_thread;
	gc->audio0_thread = NULL;
	if (thread)
		kthread_stop(thread);

	mutex_lock(&gc->capture_lock);
	gc570d_audio0_finish_stop(gc);
	for (i = 0; i < 2; i++) {
		if (gc->audio0_pcm_buffer_cpu[i])
			dma_free_coherent(&gc->pdev->dev,
					  GC570D_AUDIO_BUFFER_SIZE,
					  gc->audio0_pcm_buffer_cpu[i],
					  gc->audio0_pcm_buffer_dma[i]);
		gc->audio0_pcm_buffer_cpu[i] = NULL;
	}
	if (gc->audio0_irq_active) {
		gc570d_video_irq_put_locked(gc);
		gc->audio0_irq_active = false;
	}
	if (gc->audio0_substream == substream)
		gc->audio0_substream = NULL;
	gc->audio0_prepared = false;
	if (!gc->video0_streaming && !gc->video_streaming &&
	    !gc->audio_prepared)
		pci_clear_master(gc->pdev);
	mutex_unlock(&gc->capture_lock);

	return 0;
}

static int gc570d_audio0_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct gc570d_dev *gc = snd_pcm_substream_chip(substream);
	bool no_signal = false;
	int i;
	int ret;

	mutex_lock(&gc->capture_lock);
	if (READ_ONCE(gc->suspended)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	if (gc->audio0_prepared) {
		gc570d_audio_update_start_bit(gc, GC570D_AUDIO0_START_BIT,
					      false);
		for (i = 0; i < 2; i++) {
			if (gc->audio0_pcm_buffer_cpu[i])
				memset(gc->audio0_pcm_buffer_cpu[i],
				       GC570D_AUDIO_SENTINEL,
				       GC570D_AUDIO_BUFFER_SIZE);
		}
		gc570d_reset_audio_channel(gc, GC570D_AUDIO0_CHANNEL);
		writel(GC570D_AUDIO0_IRQ | GC570D_AUDIO0_TERM_IRQ,
		       gc->bar0 + GC570D_REG_IRQ_STATUS);
		writel(0, gc->bar0 + GC570D_AUDIO0_FORMAT);
		writel(0, gc->bar0 + GC570D_AUDIO_GLOBAL_FORMAT);
		writel(GC570D_AUDIO_PERIOD_FRAMES,
		       gc->bar0 + GC570D_AUDIO0_RATE);
		writel(lower_32_bits(gc->audio0_pcm_buffer_dma[0]),
		       gc->bar0 + GC570D_AUDIO0_BUFFER0);
		writel(upper_32_bits(gc->audio0_pcm_buffer_dma[0]),
		       gc->bar0 + GC570D_AUDIO0_BUFFER0 + 4);
		writel(lower_32_bits(gc->audio0_pcm_buffer_dma[1]),
		       gc->bar0 + GC570D_AUDIO0_BUFFER1);
		writel(upper_32_bits(gc->audio0_pcm_buffer_dma[1]),
		       gc->bar0 + GC570D_AUDIO0_BUFFER1 + 4);
		dma_wmb();
		gc->audio0_pcm_pos = 0;
		gc->audio0_pcm_cursor = 0;
		ret = 0;
		goto out_unlock;
	}
	if (!gc->video0_streaming) {
		ret = gc570d_it68051_video_output_on(gc);
		if (ret && !gc570d_audio_is_no_signal_error(ret))
			goto out_unlock;
		no_signal = ret != 0;
	} else if (READ_ONCE(gc->video0_no_signal)) {
		no_signal = true;
	}
	if (!no_signal) {
		ret = gc570d_it68051_audio_output_set(gc, true);
		if (ret && !gc570d_audio_is_no_signal_error(ret))
			goto out_unlock;
		no_signal = ret != 0;
	}
	for (i = 0; i < 2; i++) {
		gc->audio0_pcm_buffer_cpu[i] =
			dma_alloc_coherent(&gc->pdev->dev,
					   GC570D_AUDIO_BUFFER_SIZE,
					   &gc->audio0_pcm_buffer_dma[i],
					   GFP_KERNEL);
		if (!gc->audio0_pcm_buffer_cpu[i]) {
			ret = -ENOMEM;
			goto out_free;
		}
		memset(gc->audio0_pcm_buffer_cpu[i], GC570D_AUDIO_SENTINEL,
		       GC570D_AUDIO_BUFFER_SIZE);
	}

	gc570d_reset_audio_channel(gc, GC570D_AUDIO0_CHANNEL);
	writel(GC570D_AUDIO0_IRQ | GC570D_AUDIO0_TERM_IRQ,
	       gc->bar0 + GC570D_REG_IRQ_STATUS);
	writel(0, gc->bar0 + GC570D_AUDIO0_FORMAT);
	writel(0, gc->bar0 + GC570D_AUDIO_GLOBAL_FORMAT);
	writel(GC570D_AUDIO_PERIOD_FRAMES, gc->bar0 + GC570D_AUDIO0_RATE);
	writel(lower_32_bits(gc->audio0_pcm_buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER0);
	writel(upper_32_bits(gc->audio0_pcm_buffer_dma[0]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER0 + 4);
	writel(lower_32_bits(gc->audio0_pcm_buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER1);
	writel(upper_32_bits(gc->audio0_pcm_buffer_dma[1]),
	       gc->bar0 + GC570D_AUDIO0_BUFFER1 + 4);
	dma_wmb();

	gc->audio0_pcm_pos = 0;
	gc->audio0_pcm_cursor = 0;
	gc->audio0_thread_error = 0;
	WRITE_ONCE(gc->audio0_recovering, no_signal);
	atomic_set(&gc->audio0_irq_pending, 0);
	gc->audio0_substream = substream;
	gc->audio0_prepared = true;
	pci_set_master(gc->pdev);
	gc570d_video_irq_get_locked(gc);
	gc->audio0_irq_active = true;
	gc->audio0_thread = kthread_run(gc570d_audio0_pcm_thread, gc,
					"gc570d-audio0");
	if (IS_ERR(gc->audio0_thread)) {
		ret = PTR_ERR(gc->audio0_thread);
		gc->audio0_thread = NULL;
		gc->audio0_substream = NULL;
		gc->audio0_prepared = false;
		WRITE_ONCE(gc->audio0_recovering, false);
		if (gc->audio0_irq_active) {
			gc570d_video_irq_put_locked(gc);
			gc->audio0_irq_active = false;
		}
		if (!gc->video0_streaming && !gc->video_streaming &&
		    !gc->audio_prepared)
			pci_clear_master(gc->pdev);
		goto out_free;
	}
	mutex_unlock(&gc->capture_lock);
	return 0;

out_free:
	for (i = 0; i < 2; i++) {
		if (gc->audio0_pcm_buffer_cpu[i])
			dma_free_coherent(&gc->pdev->dev,
					  GC570D_AUDIO_BUFFER_SIZE,
					  gc->audio0_pcm_buffer_cpu[i],
					  gc->audio0_pcm_buffer_dma[i]);
		gc->audio0_pcm_buffer_cpu[i] = NULL;
	}
out_unlock:
	mutex_unlock(&gc->capture_lock);
	return ret;
}

static int gc570d_audio0_pcm_trigger(struct snd_pcm_substream *substream,
				     int command)
{
	struct gc570d_dev *gc = snd_pcm_substream_chip(substream);

	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!READ_ONCE(gc->audio0_prepared))
			return -EIO;
		atomic_set(&gc->audio0_irq_pending, 0);
		writel(GC570D_AUDIO0_IRQ | GC570D_AUDIO0_TERM_IRQ,
		       gc->bar0 + GC570D_REG_IRQ_STATUS);
		gc->audio0_term_baseline = atomic64_read(&gc->irq_audio0_term);
		gc->audio0_stop_pending = true;
		WRITE_ONCE(gc->audio0_running, true);
		if (!READ_ONCE(gc->audio0_recovering) &&
		    !READ_ONCE(gc->video0_no_signal))
			gc570d_audio_update_start_bit(gc,
						      GC570D_AUDIO0_START_BIT,
						      true);
		wake_up_interruptible(&gc->audio0_wait);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(gc->audio0_running, false);
		atomic_set(&gc->audio0_irq_pending, 0);
		gc570d_audio_update_start_bit(gc, GC570D_AUDIO0_START_BIT,
					      false);
		writel(GC570D_AUDIO0_IRQ, gc->bar0 + GC570D_REG_IRQ_STATUS);
		wake_up_interruptible(&gc->audio0_wait);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t
gc570d_audio0_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct gc570d_dev *gc = snd_pcm_substream_chip(substream);

	return bytes_to_frames(substream->runtime,
			       READ_ONCE(gc->audio0_pcm_pos));
}

static const struct snd_pcm_ops gc570d_audio0_pcm_ops = {
	.open = gc570d_audio0_pcm_open,
	.close = gc570d_audio0_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_free = gc570d_audio0_pcm_hw_free,
	.prepare = gc570d_audio0_pcm_prepare,
	.trigger = gc570d_audio0_pcm_trigger,
	.pointer = gc570d_audio0_pcm_pointer,
};

int gc570d_audio_register(struct gc570d_dev *gc)
{
	struct snd_card *card1;
	struct snd_card *card2;
	struct snd_pcm *pcm;
	struct snd_pcm *pcm0;
	int ret;

	/* Defaults until the IT68051 audio state machine reports a format. */
	gc->audio_channels = 2;
	gc->audio_rate = 48000;
	gc->audio0_channels = 2;
	gc->audio0_rate = 48000;

	/* --- HDMI 2 as its own card --- */
	ret = snd_devm_card_new(&gc->pdev->dev, -1, "GC570D-HDMI2",
				THIS_MODULE, 0, &card2);
	if (ret)
		return ret;
	strscpy(card2->driver, "gc570d", sizeof(card2->driver));
	strscpy(card2->shortname, "Live Gamer DUO - HDMI 2",
		sizeof(card2->shortname));
	snprintf(card2->longname, sizeof(card2->longname),
		 "AVerMedia Live Gamer DUO HDMI 2 at %s", pci_name(gc->pdev));

	ret = snd_pcm_new(card2, "HDMI 2 Capture Only", 0, 0, 1, &pcm);
	if (ret)
		return ret;
	pcm->private_data = gc;
	strscpy(pcm->name, "HDMI 2 (Capture Only)", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &gc570d_audio_pcm_ops);
	ret = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
					     0, 0);
	if (ret)
		return ret;

	/* Use LINEIN and report plugged immediately */
	ret = snd_jack_new(card2, "HDMI 2 In", SND_JACK_LINEIN,
			   &gc->hdmi2_jack, true, false);
	if (ret)
		dev_warn(&gc->pdev->dev, "Failed to create HDMI 2 jack\n");
	else
		snd_jack_report(gc->hdmi2_jack, SND_JACK_LINEIN);

	ret = snd_ctl_add(card2, snd_ctl_new1(&gc570d_capture_switch, gc));
	if (ret < 0)
		dev_warn(&gc->pdev->dev,
			 "Failed to create HDMI 2 capture switch: %d\n", ret);

	ret = snd_ctl_add(card2, snd_ctl_new1(&gc570d_capture_volume, gc));
	if (ret < 0)
		dev_warn(&gc->pdev->dev,
			 "Failed to create HDMI 2 capture volume: %d\n", ret);

	ret = snd_card_register(card2);
	if (ret)
		return ret;

	gc->audio_card_hdmi2 = card2;
	gc->audio_pcm = pcm;

	/* --- HDMI 1 as its own card --- */
	ret = snd_devm_card_new(&gc->pdev->dev, -1, "GC570D-HDMI1",
				THIS_MODULE, 0, &card1);
	if (ret)
		return ret;
	strscpy(card1->driver, "gc570d", sizeof(card1->driver));
	strscpy(card1->shortname, "Live Gamer DUO - HDMI 1",
		sizeof(card1->shortname));
	snprintf(card1->longname, sizeof(card1->longname),
		 "AVerMedia Live Gamer DUO HDMI 1 at %s", pci_name(gc->pdev));

	ret = snd_pcm_new(card1, "HDMI 1 Passthrough", 0, 0, 1, &pcm0);
	if (ret)
		return ret;
	pcm0->private_data = gc;
	strscpy(pcm0->name, "HDMI 1 (Passthrough)", sizeof(pcm0->name));
	snd_pcm_set_ops(pcm0, SNDRV_PCM_STREAM_CAPTURE, &gc570d_audio0_pcm_ops);
	ret = snd_pcm_set_managed_buffer_all(pcm0, SNDRV_DMA_TYPE_VMALLOC, NULL,
					     0, 0);
	if (ret)
		return ret;

	/* Use LINEIN and report plugged immediately */
	ret = snd_jack_new(card1, "HDMI 1 In", SND_JACK_LINEIN,
			   &gc->hdmi1_jack, true, false);
	if (ret)
		dev_warn(&gc->pdev->dev, "Failed to create HDMI 1 jack\n");
	else
		snd_jack_report(gc->hdmi1_jack, SND_JACK_LINEIN);

	ret = snd_ctl_add(card1, snd_ctl_new1(&gc570d_capture_switch, gc));
	if (ret < 0)
		dev_warn(&gc->pdev->dev,
			 "Failed to create HDMI 1 capture switch: %d\n", ret);

	ret = snd_ctl_add(card1, snd_ctl_new1(&gc570d_capture_volume, gc));
	if (ret < 0)
		dev_warn(&gc->pdev->dev,
			 "Failed to create HDMI 1 capture volume: %d\n", ret);

	ret = snd_card_register(card1);
	if (ret)
		return ret;

	gc->audio_card_hdmi1 = card1;
	gc->audio0_pcm = pcm0;

	dev_info(&gc->pdev->dev,
		 "registered HDMI IN 2 (card %d) and HDMI IN 1 (card %d) as distinct stereo ALSA cards\n",
		 card2->number, card1->number);
	return 0;
}


static int gc570d_audio_pcm_status_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;

	seq_printf(s,
		   "prepared=%u running=%u thread_error=%d position=%zu control=0x%08x recovering=%u\n",
		   READ_ONCE(gc->audio_prepared),
		   READ_ONCE(gc->audio_running), gc->audio_thread_error,
		   READ_ONCE(gc->audio_pcm_pos),
		   readl(gc->bar0 + GC570D_AUDIO_START),
		   READ_ONCE(gc->audio_recovering));
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_audio_pcm_status);

static int gc570d_audio0_pcm_status_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;

	seq_printf(s,
		   "prepared=%u running=%u thread_error=%d position=%zu control=0x%08x recovering=%u\n",
		   READ_ONCE(gc->audio0_prepared),
		   READ_ONCE(gc->audio0_running), gc->audio0_thread_error,
		   READ_ONCE(gc->audio0_pcm_pos),
		   readl(gc->bar0 + GC570D_AUDIO_START),
		   READ_ONCE(gc->audio0_recovering));
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_audio0_pcm_status);

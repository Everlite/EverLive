// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCI driver entry point, module parameters, device lifetime, debugfs setup,
 * and coordination of the GC570D hardware subsystems.
 */
#include "gc570d.h"
#ifndef GC570D_USERSPACE
#include <linux/pm.h>
#endif

bool auto_hdmi1 = true;
module_param(auto_hdmi1, bool, 0444);
MODULE_PARM_DESC(auto_hdmi1,
		 "initialize HDMI IN 1 and passthrough automatically (default: true)");

bool copy_edid;
module_param(copy_edid, bool, 0644);
MODULE_PARM_DESC(copy_edid,
		 "copy downstream HDMI OUT monitor EDID instead of internal 4K60 EDID (default: false)");

static int gc570d_bridge_init(struct gc570d_dev *gc)
{
	writel(0, gc->bar0 + GC570D_REG_DMA_START);
	writel(0x7fff, gc->bar0 + GC570D_REG_IRQ_STATUS);
	gc570d_reset_channel(gc, 0);
	gc570d_reset_channel(gc, 1);

	mutex_lock(&gc->i2c_lock);
	writel(0x0138, gc->bar0 + 0x0180);
	writel(0x0138, gc->bar0 + 0x01c0);
	writel(0x0138, gc->bar0 + 0x0120);
	writel(0x007d, gc->bar0 + 0x0150);
	writel(0x00f9, gc->bar0 + 0x0100);
	writel(0x0000, gc->bar0 + 0x0104);
	writel(0x0080, gc->bar0 + 0x0108);
	msleep(100);

	writel(0x7fff, gc->bar0 + GC570D_REG_IRQ_STATUS);
	writel(0x0fff, gc->bar0 + GC570D_REG_IRQ_MASK);
	readl(gc->bar0 + GC570D_REG_IRQ_CONTROL);
	writel(5, gc->bar0 + GC570D_REG_IRQ_MODE);
	mutex_unlock(&gc->i2c_lock);

	dev_info(&gc->pdev->dev, "bridge initialization sequence completed\n");
	return 0;
}

static ssize_t gc570d_bridge_init_write(struct file *file,
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

	/* FIX: Prevent debugfs from ripping hardware out from under active streams */
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_bridge_init(gc);
	mutex_unlock(&gc->capture_lock);
	
	return ret ? ret : count;
}

static const struct file_operations gc570d_bridge_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_bridge_init_write,
	.llseek = noop_llseek,
};

void gc570d_set_reset_bit(struct gc570d_dev *gc, unsigned int bit,
			  bool asserted)
{
	u32 value = readl(gc->bar0 + GC570D_REG_XILINX_RESET);

	if (asserted)
		value |= BIT(bit);
	else
		value &= ~BIT(bit);
	writel(value, gc->bar0 + GC570D_REG_XILINX_RESET);
	readl(gc->bar0 + GC570D_REG_XILINX_RESET);
}

static void gc570d_pulse_receiver_reset(struct gc570d_dev *gc)
{
	gc570d_set_reset_bit(gc, GC570D_RST_RECEIVER_A, true);
	msleep(5);
	gc570d_set_reset_bit(gc, GC570D_RST_RECEIVER_A, false);
	msleep(10);
	gc570d_set_reset_bit(gc, GC570D_RST_RECEIVER_A, true);
	msleep(10);
	gc570d_set_reset_bit(gc, GC570D_RST_RECEIVER_B, true);
	msleep(5);
	gc570d_set_reset_bit(gc, GC570D_RST_RECEIVER_B, false);
	msleep(10);
	gc570d_set_reset_bit(gc, GC570D_RST_RECEIVER_B, true);
	msleep(10);
}

static void gc570d_pulse_splitter_reset(struct gc570d_dev *gc)
{
	gc570d_set_reset_bit(gc, 4, true);
	msleep(5);
	gc570d_set_reset_bit(gc, 4, false);
	msleep(10);
	gc570d_set_reset_bit(gc, 4, true);
	msleep(5);
}

static void gc570d_pulse_preprocess_reset(struct gc570d_dev *gc)
{
	gc570d_set_reset_bit(gc, 0, true);
	msleep(2);
	gc570d_set_reset_bit(gc, 0, false);
	msleep(2);
	gc570d_set_reset_bit(gc, 0, true);
	msleep(2);
}

static int gc570d_frontend_init(struct gc570d_dev *gc)
{
	mutex_lock(&gc->i2c_lock);
	msleep(200);
	gc570d_pulse_splitter_reset(gc);
	gc570d_pulse_preprocess_reset(gc);
	mutex_unlock(&gc->i2c_lock);

	dev_info(&gc->pdev->dev, "frontend initialization sequence completed\n");
	return 0;
}

static ssize_t gc570d_frontend_init_write(struct file *file,
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

	/* FIX: Prevent debugfs from ripping hardware out from under active streams */
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_frontend_init(gc);
	mutex_unlock(&gc->capture_lock);
	
	return ret ? ret : count;
}

static const struct file_operations gc570d_frontend_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_frontend_init_write,
	.llseek = noop_llseek,
};

static int gc570d_receiver_reset(struct gc570d_dev *gc)
{
	mutex_lock(&gc->i2c_lock);
	gc570d_pulse_receiver_reset(gc);
	mutex_unlock(&gc->i2c_lock);

	dev_info(&gc->pdev->dev, "receiver reset sequence completed\n");
	return 0;
}

static ssize_t gc570d_receiver_reset_write(struct file *file,
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

	/* FIX: Prevent debugfs from ripping hardware out from under active streams */
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_receiver_reset(gc);
	mutex_unlock(&gc->capture_lock);
	
	return ret ? ret : count;
}

static const struct file_operations gc570d_receiver_reset_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_receiver_reset_write,
	.llseek = noop_llseek,
};

static int gc570d_hdmi2_base_init(struct gc570d_dev *gc)
{
	int ret;

	/* FIX: Acquire global capture lock so async S3 resume doesn't crash active userspace streams */
	mutex_lock(&gc->capture_lock);

	ret = gc570d_bridge_init(gc);
	if (ret)
		goto out_unlock;
	ret = gc570d_frontend_init(gc);
	if (ret)
		goto out_unlock;
	ret = gc570d_receiver_reset(gc);
	if (ret)
		goto out_unlock;

	writel(0x7fff, gc->bar0 + GC570D_REG_IRQ_STATUS);
	ret = gc570d_it6802_init(gc);
	if (ret)
		goto out_unlock;
	ret = gc570d_it6802_program_edid(gc);
	if (ret)
		goto out_unlock;
	ret = gc570d_it6802_pulse_hpd(gc);

out_unlock:
	mutex_unlock(&gc->capture_lock);
	if (!ret) {
		msleep(2000);
		dev_info(&gc->pdev->dev, "HDMI IN 2 base initialization completed\n");
	}
	return ret;
}

enum gc570d_hdmi1_auto_phase {
	GC570D_HDMI1_AUTO_RECEIVER,
	GC570D_HDMI1_AUTO_SPLITTER,
	GC570D_HDMI1_AUTO_SOURCE,
	GC570D_HDMI1_AUTO_HDCP_OFF,
	GC570D_HDMI1_AUTO_MAIN_TIMER,
	GC570D_HDMI1_AUTO_CONNECT,
	GC570D_HDMI1_AUTO_RECEIVER_EVENT,
	GC570D_HDMI1_AUTO_POST_TIMER,
	GC570D_HDMI1_AUTO_STABLE_IRQ,
	GC570D_HDMI1_AUTO_WORKER_PROBE,
	GC570D_HDMI1_AUTO_WORKERS,
	GC570D_HDMI1_AUTO_PUMP,
	GC570D_HDMI1_AUTO_READY,
};

static const char *gc570d_hdmi1_auto_phase_name(u8 phase)
{
	switch (phase) {
	case GC570D_HDMI1_AUTO_RECEIVER: return "receiver";
	case GC570D_HDMI1_AUTO_SPLITTER: return "splitter";
	case GC570D_HDMI1_AUTO_SOURCE: return "source";
	case GC570D_HDMI1_AUTO_HDCP_OFF: return "hdcp-off";
	case GC570D_HDMI1_AUTO_MAIN_TIMER: return "main-timer";
	case GC570D_HDMI1_AUTO_CONNECT: return "connect";
	case GC570D_HDMI1_AUTO_RECEIVER_EVENT: return "receiver-event";
	case GC570D_HDMI1_AUTO_POST_TIMER: return "post-timer";
	case GC570D_HDMI1_AUTO_STABLE_IRQ: return "stable-irq";
	case GC570D_HDMI1_AUTO_WORKER_PROBE: return "worker-probe";
	case GC570D_HDMI1_AUTO_WORKERS: return "workers";
	case GC570D_HDMI1_AUTO_PUMP: return "pump";
	case GC570D_HDMI1_AUTO_READY: return "ready";
	default: return "invalid";
	}
}

static void gc570d_hdmi1_auto_work(struct work_struct *work)
{
	struct gc570d_dev *gc = container_of(to_delayed_work(work),
						struct gc570d_dev,
						hdmi1_auto_work);
	unsigned long retry_delay = msecs_to_jiffies(1000);
	unsigned long next_delay = 0;
	bool reschedule = false;
	u8 phase;
	int ret = 0;

	if (READ_ONCE(gc->removing) || READ_ONCE(gc->suspended) ||
	    !auto_hdmi1)
		return;

	mutex_lock(&gc->capture_lock);
	phase = gc->hdmi1_auto_phase;

	switch (phase) {
	case GC570D_HDMI1_AUTO_RECEIVER:
		ret = gc570d_it68051_apply_base_table(gc);
		if (!ret) ret = gc570d_it68051_calibrate(gc);
		if (!ret) ret = gc570d_it68051_timing_init(gc);
		if (!ret) ret = gc570d_it68051_program_edid(gc);
		if (!ret) ret = gc570d_it68051_pulse_hpd(gc);
		if (!ret) gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_SPLITTER;
		break;
	case GC570D_HDMI1_AUTO_SPLITTER:
		ret = gc570d_splitter_core_preamble(gc);
		if (!ret) ret = gc570d_splitter_clock_init(gc);
		if (!ret) ret = gc570d_splitter_route_init(gc);
		if (!ret) ret = gc570d_splitter_output_preamble(gc);
		if (!ret) ret = gc570d_splitter_post_reset_init(gc);
		if (!ret) ret = gc570d_splitter_output_mode_init(gc);
		if (!ret) ret = gc570d_splitter_output_followup(gc);
		if (!ret) ret = gc570d_splitter_aux_enable_pulse(gc);
		if (!ret) ret = gc570d_splitter_aux_ports_init(gc);
		if (!ret) ret = gc570d_splitter_irq_init(gc);
		if (!ret) gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_SOURCE;
		break;
	case GC570D_HDMI1_AUTO_SOURCE:
		ret = gc570d_splitter_source_power_event(gc, true);
		if (!ret || ret == -ENODATA || ret == -ENOLINK) {
			ret = 0;
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_HDCP_OFF;
		}
		break;
	case GC570D_HDMI1_AUTO_HDCP_OFF:
		ret = gc570d_splitter_windows_receiver_hdcp_off(gc);
		if (!ret) {
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_MAIN_TIMER;
			next_delay = msecs_to_jiffies(500);
		}
		break;
	case GC570D_HDMI1_AUTO_MAIN_TIMER:
		ret = gc570d_splitter_main_timer_irq(gc);
		if (!ret || ret == -ENODATA) {
			ret = 0;
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_CONNECT;
		}
		break;
	case GC570D_HDMI1_AUTO_CONNECT:
		ret = gc570d_splitter_windows_channel2_connect(gc, !copy_edid);
		if (!ret || ret == -ENOLINK || ret == -ENODATA) {
			ret = 0;
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_RECEIVER_EVENT;
			next_delay = msecs_to_jiffies(1500);
		}
		break;
	case GC570D_HDMI1_AUTO_RECEIVER_EVENT:
		ret = gc570d_splitter_source_power_event(gc, true);
		if (!ret) {
			gc->splitter_eq_retry = 0;
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_POST_TIMER;
			next_delay = msecs_to_jiffies(1500);
		} else if ((ret == -ENODATA || ret == -ENOLINK) &&
			   (gc->splitter_video_stable_pending ||
			    ++gc->splitter_eq_retry >= 4)) {
			gc->splitter_eq_retry = 0;
			ret = 0;
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_POST_TIMER;
			next_delay = msecs_to_jiffies(200);
		}
		break;
	case GC570D_HDMI1_AUTO_POST_TIMER:
		ret = gc570d_splitter_main_timer_irq(gc);
		if (!ret || ret == -ENODATA) {
			ret = 0;
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_STABLE_IRQ;
		}
		break;
	case GC570D_HDMI1_AUTO_STABLE_IRQ:
		ret = gc570d_splitter_channel_video_stable_irq(gc);
		if (ret == -ENOLINK || ret == -ENODATA ||
		    (ret == -EAGAIN && ++gc->splitter_eq_retry >= 3)) {
			gc->splitter_eq_retry = 0;
			dev_info(&gc->pdev->dev, "No passthrough sink stable yet; starting state pump for standalone HDMI IN 1 capture\n");
			gc->hdmi1_auto_phase = gc->splitter_video_stable_pending ?
				GC570D_HDMI1_AUTO_WORKERS : GC570D_HDMI1_AUTO_PUMP;
			ret = 0;
		} else if (!ret || ret == -EALREADY) {
			gc->splitter_eq_retry = 0;
			ret = 0;
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_WORKER_PROBE;
		}
		break;
	case GC570D_HDMI1_AUTO_WORKER_PROBE:
		ret = gc570d_splitter_stable_worker_probe(gc);
		if (ret == -ENOLINK || ret == -ENODATA) {
			gc->hdmi1_auto_phase = gc->splitter_video_stable_pending ?
				GC570D_HDMI1_AUTO_WORKERS : GC570D_HDMI1_AUTO_PUMP;
			ret = 0;
		} else if (!ret) {
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_WORKERS;
		}
		break;
	case GC570D_HDMI1_AUTO_WORKERS:
		ret = gc570d_splitter_stable_workers_windows_order(gc);
		if (!ret) gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_PUMP;
		break;
	case GC570D_HDMI1_AUTO_PUMP:
		ret = gc570d_splitter_windows_state_pump_start(gc);
		if (!ret || ret == -EALREADY) {
			ret = 0;
			gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_READY;
			WRITE_ONCE(gc->hdmi1_auto_ready, true);
			wake_up_interruptible(&gc->video0_wait);
			wake_up_interruptible(&gc->audio0_wait);
			dev_info(&gc->pdev->dev,
				 "HDMI IN 1 and passthrough automatic initialization completed\n");
		}
		break;
	case GC570D_HDMI1_AUTO_READY:
		gc->hdmi1_auto_ready = true;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (!ret && gc->hdmi1_auto_phase != GC570D_HDMI1_AUTO_READY)
		reschedule = true;
	else if (ret)
		reschedule = true;

	mutex_unlock(&gc->capture_lock);

	if (ret && ret != -ENODATA && ret != -ENOLINK && ret != -EAGAIN &&
	    ret != -EBUSY)
		dev_warn_ratelimited(&gc->pdev->dev,
			"HDMI IN 1 automatic phase %s will retry: %d\n",
			gc570d_hdmi1_auto_phase_name(phase), ret);
	if (reschedule && !READ_ONCE(gc->removing) &&
	    !READ_ONCE(gc->suspended))
		schedule_delayed_work(&gc->hdmi1_auto_work,
				      ret ? retry_delay : next_delay);
}

static ssize_t gc570d_xilinx_reset_write(struct file *file,
					 const char __user *buffer,
					 size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	u32 reset_control;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	/* FIX: Prevent debugfs from ripping hardware out from under active streams */
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc)) {
		mutex_unlock(&gc->capture_lock);
		return -EBUSY;
	}
	mutex_lock(&gc->i2c_lock);

	writel(GC570D_IRQ_XILINX_INTC, gc->bar0 + GC570D_REG_IRQ_STATUS);
	reset_control = readl(gc->bar0 + GC570D_REG_XILINX_RESET);
	writel(reset_control | BIT(0), gc->bar0 + GC570D_REG_XILINX_RESET);
	msleep(15);
	writel(reset_control & ~BIT(0), gc->bar0 + GC570D_REG_XILINX_RESET);
	msleep(15);
	writel(reset_control | BIT(0), gc->bar0 + GC570D_REG_XILINX_RESET);
	msleep(15);

	mutex_unlock(&gc->i2c_lock);
	mutex_unlock(&gc->capture_lock);
	dev_info(&gc->pdev->dev, "Xilinx reset sequence completed\n");

	return count;
}

static const struct file_operations gc570d_xilinx_reset_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_xilinx_reset_write,
	.llseek = noop_llseek,
};

static void gc570d_resume_work(struct work_struct *work);

static int gc570d_probe(struct pci_dev *pdev,
			const struct pci_device_id *id)
{
	struct gc570d_dev *gc;
	int ret;

	gc = devm_kzalloc(&pdev->dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable PCI device\n");

	pci_disable_link_state(pdev, PCIE_LINK_STATE_L0S | PCIE_LINK_STATE_L1 | PCIE_LINK_STATE_CLKPM);

	gc->bar0_len = pci_resource_len(pdev, GC570D_BAR);
	if (!(pci_resource_flags(pdev, GC570D_BAR) & IORESOURCE_MEM))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "BAR0 is not an MMIO resource\n");

	if (gc->bar0_len < GC570D_EXPECTED_BAR_SIZE)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "BAR0 is too small: %pa bytes\n",
				     &gc->bar0_len);

	gc->bar0 = pcim_iomap_region(pdev, GC570D_BAR, "gc570d");
	if (IS_ERR(gc->bar0))
		return dev_err_probe(&pdev->dev, PTR_ERR(gc->bar0),
				     "failed to map BAR0\n");

	gc->pdev = pdev;
	mutex_init(&gc->i2c_lock);
	mutex_init(&gc->capture_lock);
	mutex_init(&gc->video_lock);
	mutex_init(&gc->video0_lock);
	mutex_init(&gc->led_lock);
	INIT_DELAYED_WORK(&gc->led_work, gc570d_led_breath_work);
	INIT_DELAYED_WORK(&gc->hdmi1_auto_work, gc570d_hdmi1_auto_work);
	INIT_WORK(&gc->resume_work, gc570d_resume_work);
	spin_lock_init(&gc->video_qlock);
	spin_lock_init(&gc->video0_qlock);
	spin_lock_init(&gc->audio_pcm_lock);
	INIT_LIST_HEAD(&gc->video_buffers);
	INIT_LIST_HEAD(&gc->video0_buffers);
	init_waitqueue_head(&gc->video_wait);
	init_waitqueue_head(&gc->video0_wait);
	init_waitqueue_head(&gc->audio_wait);
	init_waitqueue_head(&gc->audio0_wait);
	init_completion(&gc->dma_completion);
	init_completion(&gc->dma0_completion);
	atomic64_set(&gc->irq_total, 0);
	atomic64_set(&gc->irq_dma, 0);
	atomic64_set(&gc->irq_dma0, 0);
	atomic64_set(&gc->irq_dma1, 0);
	atomic64_set(&gc->irq_audio, 0);
	atomic64_set(&gc->irq_audio_term, 0);
	atomic64_set(&gc->irq_audio0, 0);
	atomic64_set(&gc->irq_audio0_term, 0);
	atomic64_set(&gc->irq_other, 0);
	atomic_set(&gc->irq_other_status, 0);
	atomic_set(&gc->control_irq_seen, 0);
	atomic_set(&gc->audio_irq_pending, 0);
	atomic_set(&gc->audio0_irq_pending, 0);
	gc->video0_width = gc570d_cfg.width;
	gc->video0_height = gc570d_cfg.height;
	gc->video0_frame_interval = 166666;
	pci_set_drvdata(pdev, gc);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to allocate PCI interrupt\n");
		goto err_free_gc;
	}
	gc->irq = pci_irq_vector(pdev, 0);
	ret = request_irq(gc->irq, gc570d_irq_handler,
			  pci_dev_msi_enabled(pdev) ? 0 : IRQF_SHARED,
			  "gc570d", gc);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to request PCI interrupt\n");
		goto err_vectors;
	}
	disable_irq(gc->irq);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to configure DMA mask\n");
		goto err_irq;
	}
	ret = gc570d_hdmi2_base_init(gc);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to initialize HDMI IN 2\n");
		goto err_irq;
	}
	ret = gc570d_video_register(gc);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to register V4L2 device\n");
		goto err_irq;
	}
	ret = gc570d_video0_register(gc);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to register HDMI IN 1 V4L2 device\n");
		goto err_video;
	}
	if (gc570d_cfg.enable_audio) {
		ret = gc570d_audio_register(gc);
		if (ret) {
			dev_err_probe(&pdev->dev, ret,
				      "failed to register ALSA device\n");
			goto err_video0;
		}
	}
	if (gc570d_cfg.enable_led) {
		ret = gc570d_led_register(gc);
		if (ret)
			dev_warn(&pdev->dev,
				 "LED class registration/default breathing failed: %d\n",
				 ret);
	}

#ifdef GC570D_DEVELOPER_DEBUGFS
	gc->debugfs_dir = debugfs_create_dir("gc570d", NULL);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("registers", 0400, gc->debugfs_dir, gc,
				    &gc570d_registers_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("irq_stats", 0400, gc->debugfs_dir, gc,
				    &gc570d_irq_stats_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("xilinx_registers", 0400, gc->debugfs_dir,
				    gc, &gc570d_xilinx_registers_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("preprocess_enable", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_preprocess_enable_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("led_basic", 0200, gc->debugfs_dir, gc,
				    &gc570d_led_basic_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("receiver_registers", 0400, gc->debugfs_dir,
				    gc, &gc570d_receiver_registers_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_status", 0400, gc->debugfs_dir,
				    gc, &gc570d_splitter_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_probe", 0400, gc->debugfs_dir,
				    gc, &gc570d_splitter_probe_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_core_preamble", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_core_preamble_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_clock_init", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_clock_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_route_init", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_route_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_output_preamble", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_output_preamble_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_post_reset_init", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_post_reset_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_output_mode_init", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_output_mode_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_output_followup", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_output_followup_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_aux_enable_pulse", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_aux_enable_pulse_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_aux_ports_init", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_aux_ports_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_irq_init", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_irq_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_windows_receiver_hdcp_off", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_windows_receiver_hdcp_off_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_source_power_event", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_source_power_event_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_receiver_event", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_source_power_event_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_hpd_high", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_hpd_high_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_edid_read", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_edid_read_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_windows_edid_copy", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_windows_edid_copy_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_windows_bridge_edid", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_windows_bridge_edid_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_windows_channel2_copy_connect", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_windows_channel2_copy_connect_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_windows_channel2_bridge_connect", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_windows_channel2_bridge_connect_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_channel_irq_on", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_channel_irq_on_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_channel1_irq_on", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_channel1_irq_on_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_main_timer_irq", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_main_timer_irq_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_channel_video_stable_irq", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_channel_video_stable_irq_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_stable_worker_probe", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_stable_worker_probe_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_stable_worker_link_setup", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_stable_worker_link_setup_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_stable_worker_port1_link_setup", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_stable_worker_port1_link_setup_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_stable_worker_output_setup", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_stable_worker_output_setup_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_stable_worker_port1_output_setup", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_stable_worker_port1_output_setup_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_stable_workers_windows_order", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_stable_workers_windows_order_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_windows_state_pump", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_windows_state_pump_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_pump_status", 0444,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_pump_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_eq_start", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_eq_start_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("splitter_aux_status", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_splitter_aux_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it68051_status", 0400, gc->debugfs_dir,
				    gc, &gc570d_it68051_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it68051_audio_status", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_it68051_audio_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it68051_init", 0200, gc->debugfs_dir,
				    gc, &gc570d_it68051_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it68051_calibrate", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_it68051_calibrate_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it68051_timing_init", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_it68051_timing_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it68051_edid", 0200, gc->debugfs_dir,
				    gc, &gc570d_it68051_edid_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it68051_edid_status", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_it68051_edid_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it68051_hpd", 0200, gc->debugfs_dir,
				    gc, &gc570d_it68051_hpd_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it68051_video_output_on", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_it68051_video_output_on_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("xilinx_reset", 0200, gc->debugfs_dir, gc,
				    &gc570d_xilinx_reset_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("bridge_init", 0200, gc->debugfs_dir, gc,
				    &gc570d_bridge_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("receiver_reset", 0200, gc->debugfs_dir, gc,
				    &gc570d_receiver_reset_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("frontend_init", 0200, gc->debugfs_dir, gc,
				    &gc570d_frontend_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it6802_init", 0200, gc->debugfs_dir, gc,
				    &gc570d_it6802_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it6802_edid", 0200, gc->debugfs_dir, gc,
				    &gc570d_it6802_edid_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it6802_edid_status", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_it6802_edid_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it6802_hpd", 0200, gc->debugfs_dir, gc,
				    &gc570d_it6802_hpd_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it6802_video_format", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_it6802_video_format_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it6802_audio_status", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_it6802_audio_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it6802_audio_output_enable", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_it6802_audio_output_enable_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it6802_output_enable", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_it6802_output_enable_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("it6802_format_init", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_it6802_format_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("vip_init", 0200, gc->debugfs_dir, gc,
				    &gc570d_vip_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("vip_registers", 0400, gc->debugfs_dir, gc,
				    &gc570d_vip_registers_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("vip0_init", 0200, gc->debugfs_dir, gc,
				    &gc570d_vip0_init_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("vip0_profile", 0400, gc->debugfs_dir, gc,
				    &gc570d_vip0_profile_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("vip0_registers", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_vip0_registers_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("dma_capture", 0200, gc->debugfs_dir, gc,
				    &gc570d_dma_capture_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("dma0_capture", 0200, gc->debugfs_dir, gc,
				    &gc570d_dma0_capture_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("dma0_burst", 0200, gc->debugfs_dir, gc,
				    &gc570d_dma0_burst_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("dma_burst", 0200, gc->debugfs_dir, gc,
				    &gc570d_dma_burst_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("dma_stress", 0200, gc->debugfs_dir, gc,
				    &gc570d_dma_stress_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("dma_status", 0400, gc->debugfs_dir, gc,
				    &gc570d_dma_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("dma_frame", 0400, gc->debugfs_dir, gc,
				    &gc570d_dma_frame_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio_dma_capture", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_audio_dma_capture_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio_dma_record", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_audio_dma_record_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio_dma_status", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_audio_dma_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio_dma_frame", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_audio_dma_frame_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio_dma_period", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_audio_dma_period_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio0_dma_record", 0200,
				    gc->debugfs_dir, gc,
				    &gc570d_audio0_dma_record_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio0_dma_status", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_audio0_dma_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio0_dma_frame", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_audio0_dma_frame_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio_pcm_status", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_audio_pcm_status_fops);
	if (!IS_ERR(gc->debugfs_dir))
		debugfs_create_file("audio0_pcm_status", 0400,
				    gc->debugfs_dir, gc,
				    &gc570d_audio0_pcm_status_fops);
#endif

	dev_info(&pdev->dev,
		 "probe complete: BAR0 %pr mapped at %pR, HDMI IN 1 automatic initialization=%s\n",
		 &pdev->resource[GC570D_BAR], &pdev->resource[GC570D_BAR],
		 auto_hdmi1 ? "on" : "off");
	if (auto_hdmi1)
		schedule_delayed_work(&gc->hdmi1_auto_work, 0);

	return 0;

err_video0:
	gc570d_audio_unregister(gc);
	video_unregister_device(&gc->video0_dev);
err_video:
	video_unregister_device(&gc->video_dev);
	v4l2_device_unregister(&gc->v4l2_dev);
err_irq:
	free_irq(gc->irq, gc);
err_vectors:
	pci_free_irq_vectors(pdev);
err_free_gc:
	vfree(gc->capture_data);
	vfree(gc->audio_capture_data);
	vfree(gc->audio0_capture_data);
	pci_set_drvdata(pdev, NULL);
#ifdef GC570D_USERSPACE
	kfree(gc);
#endif
	return ret;
}

static void gc570d_remove(struct pci_dev *pdev)
{
	struct gc570d_dev *gc = pci_get_drvdata(pdev);

	if (!gc)
		return;

	WRITE_ONCE(gc->removing, true);
	cancel_work_sync(&gc->resume_work);
	cancel_delayed_work_sync(&gc->hdmi1_auto_work);
	cancel_delayed_work_sync(&gc->led_work);
	gc570d_splitter_windows_state_pump_stop(gc);
	if (!IS_ERR_OR_NULL(gc->debugfs_dir))
		debugfs_remove_recursive(gc->debugfs_dir);
	gc570d_led_unregister(gc);
	gc570d_audio_unregister(gc);
	video_unregister_device(&gc->video0_dev);
	video_unregister_device(&gc->video_dev);
	v4l2_device_unregister(&gc->v4l2_dev);
	free_irq(gc->irq, gc);
	pci_free_irq_vectors(pdev);
	vfree(gc->capture_data);
	vfree(gc->audio_capture_data);
	vfree(gc->audio0_capture_data);
	dev_info(&pdev->dev, "diagnostic probe removed\n");
	pci_set_drvdata(pdev, NULL);
#ifdef GC570D_USERSPACE
	/* devm_kzalloc() is a plain calloc() in the userspace shim. */
	kfree(gc);
#endif
}

void gc570d_reset_resume_state(struct gc570d_dev *gc)
{
	gc->hdmi1_auto_phase = GC570D_HDMI1_AUTO_RECEIVER;
	gc->hdmi1_auto_ready = false;
	gc->splitter_rclk = 0;
	gc->splitter_main_timer_serviced = false;
	gc->splitter_video_stable_pending = false;
	gc->splitter_channel_active_mask = 0;
	gc->splitter_video_stable_mask = 0;
	gc->splitter_worker_completed_mask = 0;
	gc->splitter_worker_state4_mask = 0;
	gc->splitter_eq_state = 0;
	gc->splitter_eq_retry = 0;
	memset(gc->splitter_pclk, 0, sizeof(gc->splitter_pclk));
	memset(gc->splitter_vclk, 0, sizeof(gc->splitter_vclk));
	gc->splitter_receiver_hdcp_off = false;
	gc->splitter_edid_published = false;
	gc->video0_receiver_output_valid = false;
	gc->video0_receiver_width = 0;
	gc->video0_receiver_height = 0;
	atomic_set(&gc->control_irq_seen, 0);
	atomic_set(&gc->audio_irq_pending, 0);
	atomic_set(&gc->audio0_irq_pending, 0);
	reinit_completion(&gc->dma_completion);
}

static void gc570d_resume_work(struct work_struct *work)
{
	struct gc570d_dev *gc = container_of(work, struct gc570d_dev,
					     resume_work);
	int led_ret;
	int ret;

	if (READ_ONCE(gc->removing))
		return;

	mutex_lock(&gc->capture_lock);
	gc570d_reset_resume_state(gc);
	ret = gc570d_hdmi2_base_init(gc);
	if (ret) {
		mutex_unlock(&gc->capture_lock);
		dev_err(&gc->pdev->dev,
			"failed to reinitialize capture hardware after system resume: %d\n",
			ret);
		return;
	}
	WRITE_ONCE(gc->suspended, false);
	mutex_unlock(&gc->capture_lock);

	led_ret = gc570d_led_resume(gc);
	if (led_ret)
		dev_warn(&gc->pdev->dev,
			 "RGB restore after system resume failed: %d\n",
			 led_ret);

	if (auto_hdmi1)
		schedule_delayed_work(&gc->hdmi1_auto_work, 0);

	dev_info(&gc->pdev->dev,
		 "capture hardware reinitialized after system resume; HDMI inputs are renegotiating\n");
}

static int __maybe_unused gc570d_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct gc570d_dev *gc = pci_get_drvdata(pdev);

	mutex_lock(&gc->capture_lock);
	if (gc->video_streaming || gc->video0_streaming ||
	    gc->audio_prepared || gc->audio0_prepared ||
	    gc->video_irq_users) {
		dev_warn(dev,
			 "system suspend refused while a video or audio capture stream is active\n");
		mutex_unlock(&gc->capture_lock);
		return -EBUSY;
	}
	WRITE_ONCE(gc->suspended, true);
	mutex_unlock(&gc->capture_lock);

	cancel_work_sync(&gc->resume_work);
	cancel_delayed_work_sync(&gc->hdmi1_auto_work);
	gc570d_splitter_windows_state_pump_stop(gc);
	cancel_delayed_work_sync(&gc->led_work);

	mutex_lock(&gc->capture_lock);
	writel(0, gc->bar0 + GC570D_REG_DMA_START);
	writel(0, gc->bar0 + GC570D_DMA0_DESC_CONTROL);
	writel(0, gc->bar0 + GC570D_DMA_DESC_CONTROL);
	writel(0x7fff, gc->bar0 + GC570D_REG_IRQ_STATUS);
	pci_clear_master(pdev);
	mutex_unlock(&gc->capture_lock);

	mutex_lock(&gc->led_lock);
	gc->led_initialized = false;
	gc->led_active = false;
	mutex_unlock(&gc->led_lock);

	dev_info(dev, "capture hardware quiesced for system suspend\n");
	return 0;
}

static int __maybe_unused gc570d_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct gc570d_dev *gc = pci_get_drvdata(pdev);
	int led_ret;
	int ret;

	pci_disable_link_state(pdev, PCIE_LINK_STATE_L0S |
				     PCIE_LINK_STATE_L1 |
				     PCIE_LINK_STATE_CLKPM);

	mutex_lock(&gc->capture_lock);
	gc570d_reset_resume_state(gc);
	ret = gc570d_hdmi2_base_init(gc);
	if (ret) {
		mutex_unlock(&gc->capture_lock);
		return dev_err_probe(dev, ret,
			"failed to reinitialize capture hardware after system resume\n");
	}
	WRITE_ONCE(gc->suspended, false);
	mutex_unlock(&gc->capture_lock);

	snd_jack_report(gc->hdmi1_jack, 0);
	snd_jack_report(gc->hdmi2_jack, 0);

	if (gc->audio_card_hdmi2)
		snd_power_change_state(gc->audio_card_hdmi2,
				       SNDRV_CTL_POWER_D0);
	if (gc->audio_card_hdmi1)
		snd_power_change_state(gc->audio_card_hdmi1,
				       SNDRV_CTL_POWER_D0);

	led_ret = gc570d_led_resume(gc);
	if (led_ret)
		dev_warn(dev, "RGB restore after system resume failed: %d\n",
			 led_ret);

	if (auto_hdmi1)
		schedule_delayed_work(&gc->hdmi1_auto_work, 0);

	dev_info(dev,
		 "capture hardware reinitialized after system resume; HDMI inputs are renegotiating\n");
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(gc570d_pm_ops, gc570d_suspend, gc570d_resume);

static const struct pci_device_id gc570d_pci_ids[] = {
	{
		PCI_DEVICE_SUB(GC570D_VENDOR_ID, GC570D_DEVICE_ID,
			       GC570D_SUBSYSTEM_VENDOR_ID, GC570D_SUBSYSTEM_ID),
	},
	{ }
};
MODULE_DEVICE_TABLE(pci, gc570d_pci_ids);

static struct pci_driver gc570d_pci_driver = {
	.name = "gc570d",
	.id_table = gc570d_pci_ids,
	.probe = gc570d_probe,
	.remove = gc570d_remove,
	.driver = {
		.pm = pm_sleep_ptr(&gc570d_pm_ops),
	},
};
module_pci_driver(gc570d_pci_driver);

MODULE_AUTHOR("neoyagami and OpenGC570D contributors");
MODULE_DESCRIPTION("Experimental AVerMedia Live Gamer DUO GC570D driver");
MODULE_SOFTDEP("pre: led-class-multicolor");
MODULE_LICENSE("GPL v2");

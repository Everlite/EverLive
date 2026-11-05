// SPDX-License-Identifier: GPL-2.0-only
/*
 * Video DMA rings, scatter-gather descriptors, VIP/scaler programming, and
 * diagnostic frame capture for both GC570D channels.
 */
#include "gc570d.h"

static u32
gc570d_xilinx_video0_control(const struct gc570d_video0_signal *signal,
			     u16 width, u16 height);

void gc570d_reset_channel(struct gc570d_dev *gc, unsigned int channel)
{
	u32 status;
	u32 video_reset = BIT(channel);
	u32 audio_reset = BIT(channel + 8);
	int ret;

	/*
	 * The official driver toggles Xilinx register 0xfc before every video-0
	 * bridge reset, driving it 0 -> 1 with 2-ms
	 * delays.  This prefix is specific to channel 0.
	 */
	if (channel == 0) {
		ret = gc570d_xilinx_write32(gc, 0x0000fc, 0);
		if (!ret) {
			msleep(2);
			ret = gc570d_xilinx_write32(gc, 0x0000fc, 1);
			msleep(2);
		}
		if (ret)
			dev_warn(&gc->pdev->dev,
				 "Xilinx video-0 reset prefix failed: %d\n", ret);
	}

	writel(video_reset, gc->bar0 + GC570D_REG_RESET_STATUS);
	readl_poll_timeout(gc->bar0 + GC570D_REG_RESET_STATUS, status,
			   status & BIT(channel + 3), 1000, 11000);
	msleep(30);

	writel(audio_reset, gc->bar0 + GC570D_REG_RESET_STATUS);
	readl_poll_timeout(gc->bar0 + GC570D_REG_RESET_STATUS, status,
			   status & audio_reset, 1000, 11000);
	msleep(30);
}

void gc570d_reset_video_channel(struct gc570d_dev *gc,
					unsigned int channel)
{
	u32 status;
	u32 video_reset = BIT(channel);
	int ret;

	if (channel == 0) {
		ret = gc570d_xilinx_write32(gc, 0x0000fc, 0);
		if (!ret) {
			msleep(2);
			ret = gc570d_xilinx_write32(gc, 0x0000fc, 1);
			msleep(2);
		}
		if (ret)
			dev_warn(&gc->pdev->dev,
				 "Xilinx video-0 reset prefix failed: %d\n", ret);
	}

	writel(video_reset, gc->bar0 + GC570D_REG_RESET_STATUS);
	readl_poll_timeout(gc->bar0 + GC570D_REG_RESET_STATUS, status,
			   status & BIT(channel + 3), 1000, 11000);
	msleep(30);
}

void gc570d_reset_audio_channel(struct gc570d_dev *gc,
					unsigned int channel)
{
	u32 status;
	u32 audio_reset = BIT(channel + 8);

	writel(audio_reset, gc->bar0 + GC570D_REG_RESET_STATUS);
	readl_poll_timeout(gc->bar0 + GC570D_REG_RESET_STATUS, status,
			   status & audio_reset, 1000, 11000);
	msleep(30);
}

static int gc570d_interlaced_frame_height(u16 hactive, u16 vactive,
					  u16 *frame_height)
{
	if (hactive < 640 || hactive > 1920 || (hactive & 1))
		return -ERANGE;

	/*
	 * Receivers differ in whether the vertical-active register reports a
	 * field or the complete frame.  Normalise both conventions so 480i,
	 * 576i, 1080i, and 1440x1080i all reach the same VIP path.
	 */
	switch (vactive) {
	case 240:
		*frame_height = 480;
		break;
	case 288:
		*frame_height = 576;
		break;
	case 540:
		*frame_height = 1080;
		break;
	case 480:
	case 576:
	case 1080:
		*frame_height = vactive;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/*
 * gc570d_splitter_measure_rx_timing() derives the rate from integer clock and
 * total divisions, so a nominal 60 Hz source can measure as 57..59 Hz.  Map
 * the measurement onto the supported HDMI cadence families with a +/-3 Hz
 * tolerance before it drives mode selection or reaches userspace.  100/120 Hz
 * inputs are recognised so the high-rate Xilinx mode is retained; the captured
 * stream itself is capped at 60 fps.  Zero means "unknown" and lets callers
 * fall back to the configured interval.
 */
static u16 gc570d_normalize_frame_rate(u32 measured)
{
	static const u16 families[] = { 24, 25, 30, 50, 60, 100, 120 };
	u16 best = 0;
	u32 best_delta = U32_MAX;
	size_t i;

	if (!measured)
		return 0;
	for (i = 0; i < ARRAY_SIZE(families); i++) {
		u32 delta = measured > families[i] ? measured - families[i] :
			     families[i] - measured;

		if (delta < best_delta) {
			best_delta = delta;
			best = families[i];
		}
	}
	return best_delta <= 3 ? best : 0;
}

int gc570d_dma_detect_vip1_format(struct gc570d_dev *gc,
				  u16 *width_out, u16 *height_out,
				  bool *interlaced_out)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	static const u8 registers[] = { 0x0a, 0x99, 0x9e, 0x9f, 0xa4, 0xa5 };
	u8 value[ARRAY_SIZE(registers)];
	u32 last_irq;
	u32 last_status;
	u16 hactive;
	u16 vactive;
	u16 frame_height;
	bool interlaced;
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = gc570d_receiver_read8(gc, bus, registers[i], &value[i],
					   &last_irq, &last_status);
		if (ret)
			return ret;
	}

	hactive = ((value[3] & 0x3f) << 8) | value[2];
	vactive = ((value[4] & 0xf0) << 4) | value[5];
	if (!(value[0] & BIT(7)))
		return -ENOLINK;
	/*
	 * Accept both the interlace flag and the field-height conventions
	 * used by different receivers (240/288 fields, 540-line 1080i).
	 */
	interlaced = !!(value[1] & BIT(1)) ||
		     vactive == 240 || vactive == 288 ||
		     (hactive == 1920 && vactive == 540);
	if (interlaced) {
		ret = gc570d_interlaced_frame_height(hactive, vactive,
						     &frame_height);
		if (ret)
			return ret;
	} else {
		if (hactive < 640 || hactive > 1920 ||
		    vactive < 480 || vactive > 1080 ||
		    (hactive & 1) || (vactive & 1))
			return -ERANGE;
		frame_height = vactive;
	}

	if (width_out)
		*width_out = hactive;
	if (height_out)
		*height_out = frame_height;
	if (interlaced_out)
		*interlaced_out = interlaced;
	return 0;
}

static int gc570d_dma_validate_format(struct gc570d_dev *gc)
{
	return gc570d_dma_detect_vip1_format(gc, NULL, NULL, NULL);
}

static void gc570d_vip_program_yuy2(struct gc570d_dev *gc,
				    u16 input_width, u16 input_height,
				    u16 output_width, u16 output_height,
				    bool interlaced)
{
	u16 out_w = output_width ? output_width : GC570D_WIDTH;
	u16 out_h = output_height ? output_height : GC570D_HEIGHT;
	u16 in_h = interlaced ? (input_height >> 1) : input_height;
	u32 scaler_in_h = in_h;
	u32 scaler_out_h = out_h;
	u32 scaler_flags = (out_w * 2 < input_width ? BIT(0) : 0) |
			   (scaler_in_h > scaler_out_h * 2 ? BIT(4) : 0);
	bool scale = input_width != out_w || scaler_in_h != scaler_out_h ||
		     input_width != GC570D_WIDTH || in_h != GC570D_HEIGHT;
	u32 value;

	/* CVDecVIP::Start, channel 1, packed YUY2 with optional VIP1 scaling. */
	value = readl(gc->bar0 + GC570D_VIP_GLOBAL_FORMAT);
	writel(value & ~GENMASK(1, 0),
	       gc->bar0 + GC570D_VIP_GLOBAL_FORMAT);
	value = readl(gc->bar0 + GC570D_VIP_GLOBAL_ENABLE);
	writel(value | BIT(2), gc->bar0 + GC570D_VIP_GLOBAL_ENABLE);

	value = readl(gc->bar0 + GC570D_VIP_CHANNEL_CTRL);
	value &= ~(GENMASK(15, 8) | BIT(7));
	if (scale)
		value |= BIT(7);
	writel(value, gc->bar0 + GC570D_VIP_CHANNEL_CTRL);
	writel(0, gc->bar0 + GC570D_VIP_CHANNEL_CLOCK);

	writel(input_width, gc->bar0 + GC570D_VIP_CROP_WIDTH);
	writel(input_width, gc->bar0 + GC570D_VIP_OUTPUT_WIDTH);
	writel(in_h, gc->bar0 + GC570D_VIP_CROP_HEIGHT);
	writel(in_h, gc->bar0 + GC570D_VIP_OUTPUT_HEIGHT);

	writel(0, gc->bar0 + GC570D_VIP_SCALER_CTRL);
	writel(div_u64((u64)input_width << 20, out_w),
	       gc->bar0 + GC570D_VIP_SCALE_X);
	writel(div_u64((u64)scaler_in_h << 20, scaler_out_h),
	       gc->bar0 + GC570D_VIP_SCALE_Y);
	writel((scaler_in_h << 16) | input_width,
	       gc->bar0 + GC570D_VIP_INPUT_SIZE);
	writel(((u32)(input_width - 1)) << 16,
	       gc->bar0 + GC570D_VIP_INPUT_X_LAST);
	writel((scaler_in_h - 1) << 16,
	       gc->bar0 + GC570D_VIP_INPUT_Y_LAST);
	writel((scaler_out_h << 16) | out_w,
	       gc->bar0 + GC570D_VIP_OUTPUT_SIZE);
	writel(0x00004040, gc->bar0 + GC570D_VIP_SCALER_PHASE);
	writel(scaler_flags, gc->bar0 + GC570D_VIP_SCALER_FLAGS);
	if (!READ_ONCE(gc->video0_dma_active))
		writel(0, gc->bar0 + GC570D_VIP_BYPASS);
	writel(3, gc->bar0 + GC570D_VIP_SCALER_CTRL);
	writel(0, gc->bar0 + GC570D_VIP_AUX_1);
	writel(0, gc->bar0 + GC570D_VIP_AUX_0);
	writel(0, gc->bar0 + GC570D_VIP_AUX_2);
	writel(0, gc->bar0 + GC570D_VIP_AUX_3);

	value = readl(gc->bar0 + GC570D_VIP_CHANNEL_CTRL);
	writel(value | BIT(0), gc->bar0 + GC570D_VIP_CHANNEL_CTRL);
	/*
	 * The colour-default registers are shared with VIP0.  When HDMI IN 1 is
	 * already streaming, preserve the values it programmed instead of
	 * overwriting them from the HDMI IN 2 path.
	 */
	if (!READ_ONCE(gc->video0_dma_active)) {
		writel(0, gc->bar0 + GC570D_VIP_COLOR_0);
		/* GC570D stream-start defaults selected by the official driver. */
		writel(0x000005a0, gc->bar0 + GC570D_VIP_COLOR_1);
		writel(0x00000009, gc->bar0 + GC570D_VIP_COLOR_2);
		writel(0x000c000c, gc->bar0 + GC570D_VIP_COLOR_3);
	}
	value = readl(gc->bar0 + GC570D_VIP_GLOBAL_START);
	writel(value | BIT(0), gc->bar0 + GC570D_VIP_GLOBAL_START);
	readl(gc->bar0 + GC570D_VIP_GLOBAL_START);
}

int gc570d_vip_init(struct gc570d_dev *gc)
{
	u16 input_width = GC570D_WIDTH;
	u16 input_height = GC570D_HEIGHT;
	u16 output_width = gc->video_width ? gc->video_width : GC570D_WIDTH;
	u16 output_height = gc->video_height ? gc->video_height : GC570D_HEIGHT;
	bool interlaced = false;
	int ret;

	ret = gc570d_dma_detect_vip1_format(gc, &input_width, &input_height,
					    &interlaced);
	if (ret)
		return dev_err_probe(&gc->pdev->dev, ret,
				     "VIP init requires 480p..1080p or 1080i on channel 1\n");

	gc->video_input_width = input_width;
	gc->video_input_height = input_height;
	gc->video_input_interlaced = interlaced;
	gc570d_vip_program_yuy2(gc, input_width, input_height,
				output_width, output_height, interlaced);
	dev_info(&gc->pdev->dev,
		 "VIP channel 1 initialized: %ux%u%s input to %ux%u packed YUY2\n",
		 input_width, input_height, interlaced ? "i" : "p",
		 output_width, output_height);
	return 0;
}

static ssize_t gc570d_vip_init_write(struct file *file,
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

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_vip_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_vip_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_vip_init_write,
	.llseek = noop_llseek,
};

static int gc570d_vip_registers_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;
	static const u32 offsets[] = {
		GC570D_VIP_GLOBAL_FORMAT, GC570D_VIP_GLOBAL_ENABLE,
		GC570D_VIP_BYPASS, GC570D_VIP_GLOBAL_START,
		GC570D_VIP_COLOR_0, GC570D_VIP_COLOR_1,
		GC570D_VIP_COLOR_2, GC570D_VIP_COLOR_3,
		GC570D_VIP_CHANNEL_CTRL, GC570D_VIP_CHANNEL_CLOCK,
		GC570D_VIP_SCALER_CTRL, GC570D_VIP_CROP_WIDTH,
		GC570D_VIP_OUTPUT_WIDTH, GC570D_VIP_CROP_HEIGHT,
		GC570D_VIP_OUTPUT_HEIGHT, GC570D_VIP_SCALE_X,
		GC570D_VIP_SCALE_Y, GC570D_VIP_INPUT_SIZE,
		GC570D_VIP_INPUT_X_LAST, GC570D_VIP_INPUT_Y_LAST,
		GC570D_VIP_OUTPUT_SIZE, GC570D_VIP_SCALER_PHASE,
		GC570D_VIP_SCALER_FLAGS,
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		seq_printf(s, "0x%04x 0x%08x\n", offsets[i],
			   readl(gc->bar0 + offsets[i]));
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_vip_registers);

static u8 gc570d_video0_geometry_class(u16 width, u16 height)
{
	if (width < 1921) {
		if (width <= 720 && height <= 576)
			return 0;
		return 1;
	}
	return height < 1080 ? 1 : 2;
}

int gc570d_vip0_detect_signal(
	struct gc570d_dev *gc, struct gc570d_video0_signal *signal)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	struct gc570d_splitter_rx_timing timing = { 0 };
	static const u8 registers[] = { 0x19, 0x98, 0x9d, 0x9e,
					  0xa4, 0xa5, 0x6b };
	u8 value[ARRAY_SIZE(registers)];
	u8 avi15;
	u8 avi16;
	u8 avi17;
	u8 input_color;
	u8 output_color;
	u8 colorimetry;
	u8 extended_colorimetry;
	u32 last_irq;
	u32 last_status;
	u16 width;
	u16 height;
	size_t i;
	int restore_ret;
	int ret;

	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = gc570d_receiver_read8(gc, bus, registers[i], &value[i],
					   &last_irq, &last_status);
		if (ret)
			goto out_restore_page;
	}

	width = value[2] | ((value[3] & 0x3f) << 8);
	height = value[4] | ((value[5] & 0x3f) << 8);
	output_color = (value[6] >> 2) & 0x03;

	ret = gc570d_it68051_select_page(gc, 2);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x15, &avi15,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x16, &avi16,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x17, &avi17,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	input_color = (avi15 >> 5) & 0x03;
	colorimetry = avi16 >> 6;
	extended_colorimetry = (avi17 >> 4) & 0x07;

	/*
	 * IT68051 reports the horizontal sample count for YCbCr 4:2:0.  The
	 * official driver expands that count back to the logical pixel width
	 * before publishing and classifying the decoded format.  Without this,
	 * 3840x2160 4:2:0 appears as 1920x2160, incorrectly selects working class
	 * 1/Xilinx mode 0 and bypasses the card's 4K-to-1080p preprocessing path.
	 */
	if (input_color == 3)
		width *= 2;

out_restore_page:
	restore_ret = gc570d_it68051_select_page(gc, 0);
	if (!ret)
		ret = restore_ret;
	if (ret)
		return ret;

	if (!(value[0] & BIT(7)))
		return -ENOLINK;
	signal->interlaced = !!(value[1] & BIT(1)) ||
			     height == 240 || height == 288 ||
			     (width == 1920 && height == 540);
	if (signal->interlaced) {
		u16 frame_height;

		ret = gc570d_interlaced_frame_height(width, height,
						     &frame_height);
		if (ret)
			return ret;
		height = frame_height;
	} else if (width < 640 || height < 480 || (width & 1) || (height & 1)) {
		return -ERANGE;
	}
	gc->video0_input_interlaced = signal->interlaced;
	gc->video0_input_frame_rate = 0;
	if (colorimetry == 3 &&
	    (extended_colorimetry == 5 || extended_colorimetry == 6)) {
		if (output_color != 0)
			return -EILSEQ;
		signal->profile = GC570D_VIDEO0_HDR_BT2020;
	} else if (input_color == 0 && colorimetry != 3) {
		/*
		 * The official driver leaves CVDecVIP+0x68 at zero when no HDR DRM
		 * packet is active.  Native RGB SDR therefore stays RGB444 on the
		 * IT68051 output bus and uses the SDR RGB transform.
		 */
		if (output_color != 0)
			return -EILSEQ;
		signal->profile = GC570D_VIDEO0_SDR_RGB;
	} else if (input_color != 0 && colorimetry != 3 &&
		   output_color == 0) {
		/*
		 * Full-range SDR YCbCr 4:2:2/4:4:4 is converted to RGB444 by the
		 * receiver and uses the same SDR RGB transform as native RGB.
		 */
		signal->profile = GC570D_VIDEO0_SDR_RGB;
	} else if (input_color != 0 && colorimetry != 3 &&
		   output_color == 2) {
		/*
		 * Limited or unspecified-range SDR YCbCr (including 4:2:0) stays
		 * YCbCr 4:4:4 and takes the no-CSC Xilinx path.  This avoids
		 * applying a full-range YCbCr-to-RGB matrix to limited-range
		 * video, for which no verified matrix is available.
		 */
		signal->profile = GC570D_VIDEO0_SDR_YUV420_BT709;
	} else {
		return -ENODATA;
	}

	signal->detected_width = width;
	signal->detected_height = height;
	signal->working_mode = gc570d_video0_geometry_class(width, height);
	/*
	 * The official driver preserves the detected source window up to 1920 pixels
	 * wide.  Above that boundary the GC570D channel-0 path clamps the VIP
	 * side to 1920x1080 because Xilinx supplies the downscaled image.
	 */
	if (width > 1920) {
		signal->vip_width = 1920;
		signal->vip_height = 1080;
	} else {
		signal->vip_width = width;
		signal->vip_height = height;
	}

	/*
	 * The official driver selects Xilinx mode 3 for working mode 2 regardless of
	 * rate.  Classes 0/1 select mode 3 only above 60 Hz, so obtain the same
	 * read-only receiver timing used by the passthrough worker rather than
	 * assuming that every 1080-line source is 60 Hz.
	 */
	ret = gc570d_splitter_measure_rx_timing(gc, 0, &timing);
	if (ret && signal->working_mode != 2)
		return ret;
	signal->frame_rate = ret ? 0 :
		gc570d_normalize_frame_rate(timing.frame_rate);
	signal->xilinx_mode = signal->working_mode == 2 ||
				      signal->frame_rate > 60 ? 3 : 0;
	gc->video0_input_frame_rate = signal->frame_rate;

	/* Detection also runs from diagnostic paths, so keep the log rate limited. */
	dev_info_ratelimited(&gc->pdev->dev,
			     "VIP0 signal validated: %ux%u@%u class=%u xilinx_mode=%u vip=%ux%u profile=%s input_color=%u C=%u EC=%u output=%s\n",
		 width, height,
		 signal->frame_rate, signal->working_mode, signal->xilinx_mode,
		 signal->vip_width, signal->vip_height,
		 signal->profile == GC570D_VIDEO0_HDR_BT2020 ? "HDR-BT.2020" :
		 signal->profile == GC570D_VIDEO0_SDR_RGB ? "SDR-RGB" :
		 "SDR-YCbCr420-BT.709", input_color, colorimetry,
		 extended_colorimetry, output_color == 0 ? "RGB" : "YCbCr444");

	return 0;
}

static int gc570d_vip0_profile_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;
	struct gc570d_video0_signal signal;
	u32 control;
	int ret;

	mutex_lock(&gc->capture_lock);
	ret = gc570d_vip0_detect_signal(gc, &signal);
	mutex_unlock(&gc->capture_lock);
	
	if (ret) {
		seq_printf(s, "profile=unsupported error=%d\n", ret);
		return 0;
	}

	control = gc570d_xilinx_video0_control(&signal, gc->video0_width,
					       gc->video0_height);
	if (signal.profile == GC570D_VIDEO0_HDR_BT2020)
		control |= BIT(16);
	seq_printf(s,
		   "profile=%s source=%ux%u@%u class=%u vip=%ux%u xilinx_mode=%u output=%ux%u xilinx_control=0x%08x hdr_lut=%s\n",
		   signal.profile == GC570D_VIDEO0_HDR_BT2020 ?
		   "HDR-BT.2020" :
		   signal.profile == GC570D_VIDEO0_SDR_RGB ?
		   "SDR-RGB" : "SDR-YCbCr420-BT.709",
		   signal.detected_width, signal.detected_height,
		   signal.frame_rate, signal.working_mode,
		   signal.vip_width, signal.vip_height, signal.xilinx_mode,
		   gc->video0_width, gc->video0_height, control,
		   signal.profile == GC570D_VIDEO0_HDR_BT2020 ? "required" :
		   "skipped");
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_vip0_profile);

static u32 gc570d_xilinx_video0_control(
	const struct gc570d_video0_signal *signal, u16 width, u16 height)
{
	u16 xilinx_width = signal->working_mode < 2 ? signal->vip_width : width;
	u16 xilinx_height = signal->working_mode < 2 ? signal->vip_height : height;
	u32 control;

	control = ((gc570d_video0_geometry_class(xilinx_width,
						 xilinx_height) + 2) << 8) |
		(signal->profile == GC570D_VIDEO0_HDR_BT2020 ? 0x05 :
		 signal->profile == GC570D_VIDEO0_SDR_RGB ? 0x10 : 0x33);
	if (signal->profile == GC570D_VIDEO0_HDR_BT2020)
		control |= BIT(16);
	return control;
}

int gc570d_xilinx_prepare_video0(
	struct gc570d_dev *gc, const struct gc570d_video0_signal *signal,
	u16 output_width, u16 output_height)
{
	u8 raw[4];
	u32 last_irq;
	u32 last_status;
	u32 control;
	u32 mode;
	u32 verified;
	int ret;

	/*
	 * The official driver selects mode 3 for working class 2 or an input rate
	 * above 60 Hz, and mode 0 otherwise.  Preserve every unrelated bit in
	 * logical register 0x000000.
	 */
	ret = gc570d_xilinx_read32(gc, 0x000000, &mode, raw,
				    &last_irq, &last_status);
	if (ret)
		return ret;
	mode = (mode & ~GENMASK(1, 0)) | signal->xilinx_mode;
	ret = gc570d_xilinx_write32(gc, 0x000000, mode);
	if (ret)
		return ret;
	ret = gc570d_xilinx_read32(gc, 0x000000, &verified, raw,
				    &last_irq, &last_status);
	if (ret)
		return ret;
	if ((verified & GENMASK(1, 0)) != signal->xilinx_mode)
		return -EIO;

	ret = gc570d_xilinx_read32(gc, 0x000004, &control, raw,
				    &last_irq, &last_status);
	if (ret)
		return ret;

	/*
	 * The official driver stores the negotiated YUY2 FourCC before rebuilding
	 * the format at stream start.  With Capture Video already
	 * negotiated at 1920x1080, the official driver selects geometry nibble 3.
	 * For BT.2020 HDR, the IT68051's RGB-full output is transformed to
	 * sampling/colorspace 5/0, so the official driver packs 3/0/5 as 0x305.
	 *
	 * The official driver sets the HDR policy cache to zero for SDR.  Its
	 * zero-policy branch disables preprocessing, does not publish the tone-map
	 * LUT, and finally packs
	 * RGB-full/SDR/1080 as 3/1/0 = 0x310.  HDR BT.2020 keeps the proven
	 * enabled 3/0/5 = 0x305 path.
	 */
	control = gc570d_xilinx_video0_control(signal, output_width,
					      output_height);

	/*
	 * Do not write logical register 0x08 here.  The official driver's pre-stream
	 * callbacks update only the mode, packed format, LUT, and enable state.  It
	 * writes the clock later, after programming the VIP scaler block.
	 */
	ret = gc570d_xilinx_write32(gc, 0x000004, control);
	if (ret)
		return ret;
	ret = gc570d_xilinx_read32(gc, 0x000004, &verified, raw,
				    &last_irq, &last_status);
	if (ret)
		return ret;
	if ((verified & (BIT(16) | GENMASK(11, 0))) != control)
		return -EIO;

	return 0;
}

static int gc570d_xilinx_program_video0_transform(
	struct gc570d_dev *gc, const struct gc570d_video0_signal *signal,
	u16 output_width, u16 output_height)
{
	u8 raw[4];
	u32 last_irq;
	u32 last_status;
	u32 control;
	u32 verified;
	int ret;

	/*
	 * The official driver rebuilds the video transform at stream start.  That path
	 * rebuilds the three format/geometry nibbles in Xilinx register 0x04,
	 * preserves only its independent enable bit 16, and clears every other
	 * bit.  Mode and clock live in separate registers.
	 */
	ret = gc570d_xilinx_read32(gc, 0x000004, &control, raw,
				    &last_irq, &last_status);
	if (ret)
		return ret;
	control = (control & BIT(16)) |
		(gc570d_xilinx_video0_control(signal, output_width,
					       output_height) & GENMASK(11, 0));
	ret = gc570d_xilinx_write32(gc, 0x000004, control);
	if (ret)
		return ret;
	ret = gc570d_xilinx_read32(gc, 0x000004, &verified, raw,
				    &last_irq, &last_status);
	if (ret)
		return ret;
	if ((verified & GENMASK(11, 0)) !=
	    (gc570d_xilinx_video0_control(signal, output_width,
					 output_height) & GENMASK(11, 0)))
		return -EIO;

	return 0;
}

int gc570d_vip0_program_yuy2(
	struct gc570d_dev *gc, const struct gc570d_video0_signal *signal,
	u16 output_width, u16 output_height, u32 frame_interval)
{
	u16 input_width = signal->vip_width;
	u16 input_height = signal->vip_height;
	u32 input_field_height = input_height >> 1;
	u32 output_field_height = output_height >> 1;
	u32 scaler_in_height = input_field_height;
	u32 scaler_out_height = output_field_height;
	u32 scaler_flags = output_width * 2 < input_width ? BIT(0) : 0;
	bool scale = input_width != output_width || input_height != output_height;
	/*
	 * VIP0 consumes a progressive upscale as complete frames.  Applying the
	 * software-interlace half-height encoding to this case makes the hardware
	 * deliver duplicated fields at half cadence.  Native, downscale, and true
	 * interlaced paths retain the recovered field encoding below.
	 */
	bool progressive_upscale = !signal->interlaced &&
		input_height < output_height;
	u32 clock_interval;
	u32 value;
	int ret;

	if (!input_width || !input_field_height ||
	    !output_width || !output_field_height)
		return -EINVAL;

	if (progressive_upscale) {
		scaler_in_height = input_height;
		scaler_out_height = output_height;
	}

	/* The official driver preserves every existing 0x1040 bit and sets bit 2. */
	value = readl(gc->bar0 + GC570D_VIP_GLOBAL_ENABLE);
	writel(value | BIT(2), gc->bar0 + GC570D_VIP_GLOBAL_ENABLE);

	/*
	 * The official driver selects YUY2 and rebuilds the video transform.  At this
	 * point it changes only Xilinx register 0x04; its mode and
	 * preprocessing enable were established by the format/HDR callbacks.
	 */
	value = readl(gc->bar0 + GC570D_VIP0_CHANNEL_CTRL);
	value &= ~(GENMASK(15, 8) | BIT(7) | BIT(0));
	writel(value, gc->bar0 + GC570D_VIP0_CHANNEL_CTRL);
	ret = gc570d_xilinx_program_video0_transform(gc, signal,
						     output_width,
						     output_height);
	if (ret)
		return ret;

	/* The official driver selects the input alignment in BAR+0x0004. */
	value = readl(gc->bar0 + GC570D_VIP_GLOBAL_FORMAT);
	writel(value & ~GENMASK(1, 0),
	       gc->bar0 + GC570D_VIP_GLOBAL_FORMAT);

	/*
	 * The official driver programs the requested capture window.  GC570D_x64.inf
	 * sets "Software Interlace" to one, and the driver also passes the nonzero
	 * YUY2 FourCC to CVDecVIP+0xd0.  Consequently it encodes the logical image
	 * as two fields here; this does
	 * not change the progressive packed-YUY2 DMA buffer geometry.
	 */
	/* The first and third operands are the zero-based window origins. */
	writel(0, gc->bar0 + GC570D_VIP0_WINDOW_WIDTH);
	writel(input_width, gc->bar0 + GC570D_VIP0_OUTPUT_WIDTH);
	writel(0, gc->bar0 + GC570D_VIP0_WINDOW_HEIGHT);
	writel(progressive_upscale ? input_height :
	       ((input_height & ~1) << 15) | input_field_height,
	       gc->bar0 + GC570D_VIP0_OUTPUT_HEIGHT);

	/*
	 * The official driver copies the negotiated Capture Video pin format into
	 * CVDecVIP +0x90/+0x94.  the official driver separately clamps HDMI IN1's
	 * requested window to 1920x1080 at bridge +0x6b8/+0x6bc only for
	 * inputs wider than 1920; smaller detected windows remain native. The INF's
	 * Software Interlace=1 and the nonzero YUY2 FourCC make
	 * The official driver halve both vertical operands passed to the official driver.
	 * The scaler maps that effective source window to the separately
	 * negotiated Capture Video geometry.
	 */
	writel(0, gc->bar0 + GC570D_VIP0_SCALER_CTRL);
	writel(div_u64((u64)input_width << 20, output_width),
	       gc->bar0 + GC570D_VIP0_SCALE_X);
	writel(div_u64((u64)scaler_in_height << 20, scaler_out_height),
	       gc->bar0 + GC570D_VIP0_SCALE_Y);
	writel((scaler_in_height << 16) | input_width,
	       gc->bar0 + GC570D_VIP0_INPUT_SIZE);
	writel((input_width - 1) << 16,
	       gc->bar0 + GC570D_VIP0_INPUT_X_LAST);
	writel((scaler_in_height - 1) << 16,
	       gc->bar0 + GC570D_VIP0_INPUT_Y_LAST);
	writel((scaler_out_height << 16) | output_width,
	       gc->bar0 + GC570D_VIP0_OUTPUT_SIZE);
	writel(0x00004040, gc->bar0 + GC570D_VIP0_SCALER_PHASE);
	if (scaler_in_height > scaler_out_height * 2)
		scaler_flags |= BIT(4);
	writel(scaler_flags, gc->bar0 + GC570D_VIP0_SCALER_FLAGS);

	/*
	 * The official driver programs the clock only after finishing the scaler
	 * block.  Prefer the measured source rate so a 50 Hz source is not clocked
	 * as 60 Hz merely because the application negotiated a 1080p format.
	 * The 25/50 Hz family uses the 50 Hz PLL; 24/30/60/120 Hz sources use the
	 * 60 Hz PLL, which is also what the 60 Hz format table entry requests.
	 */
	if (signal->frame_rate == 25 || signal->frame_rate == 50)
		clock_interval = 200000;
	else if (signal->frame_rate)
		clock_interval = 166666;
	else
		clock_interval = frame_interval;
	ret = gc570d_xilinx_write32(gc, 0x000008,
		clock_interval == 200000 ? GC570D_XILINX_50HZ_CLOCK :
		GC570D_XILINX_60HZ_CLOCK);
	if (ret)
		return ret;

	/*
	 * Equal logical sides take the official driver's bypass branch and clear
	 * channel bit 7. Scaling sets it. The equal-size working-mode-2 branch
	 * writes one to BAR+0x1084; scaling and the lower classes write zero.
	 * BAR+0x1084 is shared with VIP1, so preserve whatever HDMI IN 2 set
	 * while its DMA is active.
	 */
	value = readl(gc->bar0 + GC570D_VIP0_CHANNEL_CTRL);
	writel(scale ? value | BIT(7) : value & ~BIT(7),
	       gc->bar0 + GC570D_VIP0_CHANNEL_CTRL);
	if (!READ_ONCE(gc->video_dma_active))
		writel(!scale && signal->working_mode == 2 ? 1 : 0,
		       gc->bar0 + GC570D_VIP_BYPASS);
	writel(3, gc->bar0 + GC570D_VIP0_SCALER_CTRL);
	writel(0, gc->bar0 + GC570D_VIP_AUX_1);
	writel(0, gc->bar0 + GC570D_VIP_AUX_0);
	writel(0, gc->bar0 + GC570D_VIP_AUX_2);
	writel(0, gc->bar0 + GC570D_VIP_AUX_3);

	value = readl(gc->bar0 + GC570D_VIP0_CHANNEL_CTRL);
	writel(value | BIT(0), gc->bar0 + GC570D_VIP0_CHANNEL_CTRL);
	/*
	 * Capture Video enters through bridge vtable +0x100, which does not invoke
	 * the auxiliary color-default path.  The allocator
	 * zeroes the complete CVDecVIP object and its constructor leaves the
	 * cached +0x70..+0x7c values untouched, so the official driver writes zero to
	 * all four registers here.  Values 0/0x5a0/9/12 belong exclusively to
	 * the auxiliary +0x178 path, not this DMA0 stream.
	 */
	if (!READ_ONCE(gc->video_dma_active)) {
		writel(0, gc->bar0 + GC570D_VIP_COLOR_0);
		writel(0, gc->bar0 + GC570D_VIP_COLOR_1);
		writel(0, gc->bar0 + GC570D_VIP_COLOR_2);
		writel(0, gc->bar0 + GC570D_VIP_COLOR_3);
	}
	/* The official driver preserves the current global state for the start edge. */
	value = readl(gc->bar0 + GC570D_VIP_GLOBAL_START);
	writel(value | BIT(0), gc->bar0 + GC570D_VIP_GLOBAL_START);
	readl(gc->bar0 + GC570D_VIP_GLOBAL_START);

	return 0;
}

static ssize_t gc570d_vip0_init_write(struct file *file,
				       const char __user *buffer,
				       size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	struct gc570d_video0_signal signal;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc)) {
		ret = -EBUSY;
	} else {
		ret = gc570d_vip0_detect_signal(gc, &signal);
		if (!ret)
			ret = gc570d_vip0_program_yuy2(gc, &signal,
				gc->video0_width, gc->video0_height,
				gc->video0_frame_interval);
	}
	mutex_unlock(&gc->capture_lock);
	if (ret) {
		dev_err(&gc->pdev->dev,
			"VIP0 Capture Video initialization failed: %d\n", ret);
		return ret;
	}

	dev_info(&gc->pdev->dev,
		 "VIP channel 0 initialized for %ux%u %s input to %ux%u packed YUY2\n",
		 signal.detected_width, signal.detected_height,
		 signal.profile == GC570D_VIDEO0_HDR_BT2020 ?
		 "BT.2020 HDR" :
		 signal.profile == GC570D_VIDEO0_SDR_RGB ?
		 "RGB SDR" : "BT.709 YCbCr420 SDR",
		 gc->video0_width, gc->video0_height);
	return count;
}

const struct file_operations gc570d_vip0_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_vip0_init_write,
	.llseek = noop_llseek,
};

static int gc570d_vip0_registers_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;
	static const u32 offsets[] = {
		GC570D_VIP_GLOBAL_FORMAT, GC570D_VIP_GLOBAL_ENABLE,
		GC570D_VIP_BYPASS, GC570D_VIP_GLOBAL_START,
		GC570D_VIP_COLOR_0, GC570D_VIP_COLOR_1,
		GC570D_VIP_COLOR_2, GC570D_VIP_COLOR_3,
		GC570D_VIP0_CHANNEL_CTRL, GC570D_VIP0_EVENT_STATUS,
		GC570D_VIP0_ACTIVE_STATUS, GC570D_VIP0_PERIOD_STATUS,
		GC570D_VIP0_WINDOW_WIDTH, GC570D_VIP0_OUTPUT_WIDTH,
		GC570D_VIP0_WINDOW_HEIGHT, GC570D_VIP0_OUTPUT_HEIGHT,
		GC570D_VIP0_SCALER_CTRL, GC570D_VIP0_SCALE_X,
		GC570D_VIP0_SCALE_Y, GC570D_VIP0_INPUT_SIZE,
		GC570D_VIP0_INPUT_X_LAST, GC570D_VIP0_INPUT_Y_LAST,
		GC570D_VIP0_OUTPUT_SIZE, GC570D_VIP0_SCALER_PHASE,
		GC570D_VIP0_SCALER_FLAGS,
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		seq_printf(s, "0x%04x 0x%08x\n", offsets[i],
			   readl(gc->bar0 + offsets[i]));
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_vip0_registers);


static int gc570d_dma_capture_frames_channel(struct gc570d_dev *gc,
				     unsigned int channel,
				     unsigned int frame_count,
				     unsigned int buffer_count)
{
	__le32 *sg_cpu = NULL;
	void *image_cpu = NULL;
	dma_addr_t sg_dma = 0;
	dma_addr_t image_dma = 0;
	__le32 *slot_sg_cpu[GC570D_DMA_BURST_FRAMES] = { NULL };
	void *slot_image_cpu[GC570D_DMA_BURST_FRAMES] = { NULL };
	dma_addr_t slot_sg_dma[GC570D_DMA_BURST_FRAMES] = { 0 };
	dma_addr_t slot_image_dma[GC570D_DMA_BURST_FRAMES] = { 0 };
	size_t capture_bytes;
	size_t descriptor_bytes;
	size_t inspected_bytes;
	size_t sg_alloc_size;
	size_t frame_size;
	u16 capture_width;
	u16 capture_height;
	u32 control = 0;
	u32 irq_status = 0;
	u32 index = 0;
	u32 hardware_index;
	u32 completed = 0;
	u32 desc_index;
	u32 desc_control;
	u32 desc_address;
	u32 frame_irq;
	u32 ring_slots;
	u32 vip_event_before = 0;
	u32 vip_active_before = 0;
	u32 vip_period_before = 0;
	u32 vip_event_after = 0;
	u32 vip_active_after = 0;
	u32 vip_period_after = 0;
	u32 dma0_control_before = 0;
	u32 dma0_control_publish[GC570D_DMA_BURST_FRAMES] = { 0 };
	u32 dma0_index_before_preprocess = 0;
	u32 dma0_index_before_publish = 0;
	u32 dma0_index_publish[GC570D_DMA_BURST_FRAMES] = { 0 };
	u32 dma0_index_after_start = 0;
	u32 dma0_index_after_vip = 0;
	u32 dma0_vip_event_before_ack = 0;
	u32 dma0_vip_event_after_ack = 0;
	u32 dma0_vip_poll_ack_before = 0;
	u32 dma0_vip_poll_ack_after = 0;
	u32 dma0_vip_poll_ack_count = 0;
	u32 dma0_poll_index_first = 0;
	u32 dma0_poll_index_last = 0;
	u32 dma0_poll_index_changes = 0;
	u32 dma0_poll_index_low_nonzero = 0;
	u32 dma0_poll_control_first = 0;
	u32 dma0_poll_control_last = 0;
	u32 dma0_poll_control_changes = 0;
	u32 dma0_poll_irq_or = 0;
	u32 dma0_completion_index[GC570D_DMA_BURST_FRAMES] = { 0 };
	bool dma0_poll_sampled = false;
	bool dma0_continuous = false;
	bool mastering = false;
	bool irq_enabled = false;
	struct gc570d_video0_signal signal = {
		.profile = GC570D_VIDEO0_HDR_BT2020,
	};
	ktime_t capture_start;
	s64 elapsed_us = 0;
	size_t changed = 0;
	size_t i;
	int ret;

	if (channel > 1 || !frame_count || !buffer_count ||
	    buffer_count > GC570D_DMA_BURST_FRAMES)
		return -EINVAL;
	if (channel == 0 && frame_count > GC570D_DMA_BURST_FRAMES)
		return -EINVAL;
	dma0_continuous = channel == 0 && frame_count > 1;
	desc_index = GC570D_DMA0_DESC_INDEX + channel * 0x800;
	desc_control = GC570D_DMA0_DESC_CONTROL + channel * 0x800;
	desc_address = GC570D_DMA0_DESC_ADDRESS + channel * 0x800;
	frame_irq = BIT(channel * 2 + 1);
	ring_slots = channel == 0 ? GC570D_DMA_BURST_FRAMES : 1;
	capture_width = channel == 0 ? gc->video0_width : gc->video_width;
	capture_height = channel == 0 ? gc->video0_height : gc->video_height;
	frame_size = (size_t)capture_width * capture_height * 2;
	capture_bytes = max_t(unsigned int, buffer_count, ring_slots) *
		frame_size;
	/*
	 * The official driver fills CVDecVIP +0x90/+0x94 from the negotiated
	 * Capture Video pin and forwards those fields to the DMA setup.  Size
	 * the diagnostic descriptors from the negotiated geometry instead of
	 * assuming 1920x1080.
	 */
	descriptor_bytes = frame_size;
	inspected_bytes = channel == 0 ? ring_slots * descriptor_bytes :
		capture_bytes;
	sg_alloc_size = ring_slots * GC570D_DMA_SG_SIZE;

	mutex_lock(&gc->capture_lock);
	if (gc->video_streaming || gc->video0_streaming) {
		ret = -EBUSY;
		goto out_status;
	}
	if (gc->audio_prepared || gc->audio0_prepared) {
		ret = -EBUSY;
		goto out_status;
	}
	vfree(gc->capture_data);
	gc->capture_data = NULL;
	gc->capture_size = 0;
	gc->capture_irq = 0;
	gc->capture_index = 0;
	gc->capture_control = 0;
	gc->capture_changed = 0;
	gc->capture_elapsed_us = 0;
	gc->capture_error = 0;

	if (channel == 0) {
		ret = gc570d_vip0_detect_signal(gc, &signal);
	} else {
		ret = gc570d_dma_validate_format(gc);
	}
	if (ret)
		goto out_status;

	if (channel == 0)
		dma0_index_before_preprocess = readl(gc->bar0 + desc_index);

	control = readl(gc->bar0 + desc_control);
	if (channel == 0)
		dma0_control_before = control;
	if (control & 0x1f) {
		ret = -EBUSY;
		goto out_status;
	}

	ret = dma_set_mask_and_coherent(&gc->pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto out_status;

	if (channel == 0) {
		/*
		 * The official driver enables the GC570D Xilinx preprocessing path from the
		 * decoded-format callback, before stream start publishes descriptors and
		 * programs the VIP channel.
		 * Prepare it before the four coherent ring allocations so DMA0 does
		 * not see the preprocessing edge and the VIP start edge in the same
		 * immediate sequence. gc570d_vip0_program_yuy2() reapplies
		 * and verifies the same idempotent format at stream start.
		 */
		ret = gc570d_xilinx_prepare_video0(gc, &signal, capture_width,
						   capture_height);
		if (ret)
			goto out_status;
		if (signal.profile == GC570D_VIDEO0_HDR_BT2020) {
			ret = gc570d_xilinx_load_hdr_lut_3000(gc);
			if (ret)
				goto out_status;
		}
		/* The official driver finishes by applying enable and packed format. */
		ret = gc570d_xilinx_prepare_video0(gc, &signal, capture_width,
						   capture_height);
		if (ret)
			goto out_status;
		dev_info(&gc->pdev->dev,
			 "DMA0 Xilinx %s profile prepared before descriptor allocation\n",
			 signal.profile == GC570D_VIDEO0_HDR_BT2020 ?
			 "HDR preprocessing/LUT" : "SDR bypass");

		for (i = 0; i < ring_slots; i++) {
			slot_image_cpu[i] = dma_alloc_coherent(&gc->pdev->dev,
					GC570D_DMA_IMAGE_SIZE, &slot_image_dma[i],
					GFP_KERNEL);
			if (!slot_image_cpu[i]) {
				ret = -ENOMEM;
				goto out_free;
			}
			slot_sg_cpu[i] = dma_alloc_coherent(&gc->pdev->dev,
					GC570D_DMA_SG_SIZE, &slot_sg_dma[i],
					GFP_KERNEL);
			if (!slot_sg_cpu[i]) {
				ret = -ENOMEM;
				goto out_free;
			}
			memset(slot_image_cpu[i], GC570D_DMA_SENTINEL,
			       GC570D_DMA_IMAGE_SIZE);
			memset(slot_sg_cpu[i], 0, GC570D_DMA_SG_SIZE);
		}
		image_cpu = slot_image_cpu[0];
		image_dma = slot_image_dma[0];
		sg_cpu = slot_sg_cpu[0];
		sg_dma = slot_sg_dma[0];
	} else {
		/* FIX: Utilize dynamically calculated `capture_bytes` instead of fixed GC570D_DMA_IMAGE_SIZE */
		image_cpu = dma_alloc_coherent(&gc->pdev->dev,
					capture_bytes, &image_dma,
					GFP_KERNEL);
		if (!image_cpu) {
			ret = -ENOMEM;
			goto out_status;
		}
		sg_cpu = dma_alloc_coherent(&gc->pdev->dev, sg_alloc_size,
					    &sg_dma, GFP_KERNEL);
		if (!sg_cpu) {
			ret = -ENOMEM;
			goto out_free;
		}
		memset(image_cpu, GC570D_DMA_SENTINEL, capture_bytes);
		memset(sg_cpu, 0, sg_alloc_size);
	}

	pci_set_master(gc->pdev);
	mastering = true;
	atomic64_set(&gc->irq_total, 0);
	atomic64_set(&gc->irq_dma, 0);
	atomic64_set(&gc->irq_dma0, 0);
	atomic64_set(&gc->irq_dma1, 0);
	atomic64_set(&gc->irq_other, 0);
	atomic_set(&gc->irq_other_status, 0);
	WRITE_ONCE(gc->dma0_continuous, dma0_continuous);
	gc570d_video_irq_get_locked(gc);
	irq_enabled = true;

	if (channel == 0) {
		writel(frame_irq, gc->bar0 + GC570D_REG_IRQ_STATUS);
		reinit_completion(&gc->dma0_completion);
		WRITE_ONCE(gc->dma0_irq_status, 0);
		dma0_index_before_publish = readl(gc->bar0 + desc_index);
	}
	capture_start = ktime_get();
	for (i = 0; i < frame_count; i++) {
		u32 ring_control = 0;
		u32 slot;

		if (channel != 0 || i == 0) {
		for (slot = 0; slot < ring_slots; slot++) {
			__le32 *sg_slot = channel == 0 ? slot_sg_cpu[slot] :
				(__le32 *)((u8 *)sg_cpu +
					   slot * GC570D_DMA_SG_SIZE);
			dma_addr_t sg_slot_dma = channel == 0 ?
				slot_sg_dma[slot] :
				sg_dma + slot * GC570D_DMA_SG_SIZE;
			dma_addr_t frame_dma = channel == 0 ?
				slot_image_dma[slot] :
				image_dma + (i % buffer_count) * frame_size;
			u32 slot_address = desc_address + slot * 0x0c;
			u32 sg_entries = 1;

			if (channel == 0) {
				u32 line;

				/*
				 * GC570D forces device+0x26c = 1 in the official driver.
				 * For the 1920x1080 YUY2 KS format, the modern adapter
				 * path makes local_12c == 1 in the official driver.  That
				 * branch divides even one contiguous 4,147,200-byte
				 * KSMAPPING at every 3,840-byte scanline boundary.  Its
				 * final copy loop reverses the scanline groups for
				 * Software Flip=1, so a one-fragment-per-line mapping is
				 * published bottom-to-top as 1,080 entries with
				 * 0x80008000 flags.
				 */
				for (line = 0; line < capture_height; line++) {
					__le32 *entry = sg_slot + line * 4;
					dma_addr_t line_dma = frame_dma +
						(size_t)(capture_height - 1 - line) *
						capture_width * 2;

					entry[0] = cpu_to_le32(lower_32_bits(line_dma));
					entry[1] = cpu_to_le32(upper_32_bits(line_dma));
					entry[2] = cpu_to_le32((capture_width * 2) /
								 sizeof(u32));
					entry[3] = cpu_to_le32(GC570D_DMA_STREAM_SG_FLAGS);
				}
				sg_entries = capture_height;
			} else {
				sg_slot[0] = cpu_to_le32(lower_32_bits(frame_dma));
				sg_slot[1] = cpu_to_le32(upper_32_bits(frame_dma));
				sg_slot[2] = cpu_to_le32(descriptor_bytes /
							       sizeof(u32));
				sg_slot[3] = cpu_to_le32(0x80006000);
			}
			dma_wmb();
			writel(lower_32_bits(sg_slot_dma),
			       gc->bar0 + slot_address);
			writel(upper_32_bits(sg_slot_dma),
			       gc->bar0 + slot_address + 4);
			writel(sg_entries, gc->bar0 + slot_address + 8);
			ring_control |= BIT(slot + 1);

			if (channel == 0) {
				writel(readl(gc->bar0 + desc_control) |
				       BIT(slot + 1), gc->bar0 + desc_control);
				dma0_control_publish[slot] =
					readl(gc->bar0 + desc_control);
				dma0_index_publish[slot] =
					readl(gc->bar0 + desc_index);
			} else {
				writel(ring_control, gc->bar0 + desc_control);
			}
		}

		if (channel != 0) {
			writel(frame_irq,
			       gc->bar0 + GC570D_REG_IRQ_STATUS);
			reinit_completion(&gc->dma_completion);
			WRITE_ONCE(gc->dma_irq_status, 0);
		}
		wmb();
		if (channel == 0) {
			writel(readl(gc->bar0 + desc_control) | BIT(0),
			       gc->bar0 + desc_control);
		} else {
			writel(ring_control | BIT(0), gc->bar0 + desc_control);
		}
		if (channel == 0)
			dma0_index_after_start = readl(gc->bar0 + desc_index);

		if (channel == 0) {
			dma0_vip_event_before_ack = readl(gc->bar0 +
							 GC570D_VIP0_EVENT_STATUS);
			if ((dma0_vip_event_before_ack & (BIT(0) | BIT(1))) ==
			    (BIT(0) | BIT(1)))
				writel(BIT(1), gc->bar0 +
				       GC570D_VIP0_EVENT_STATUS);
			dma0_vip_event_after_ack = readl(gc->bar0 +
							GC570D_VIP0_EVENT_STATUS);
			ret = gc570d_vip0_program_yuy2(gc, &signal,
						       capture_width,
						       capture_height,
						       gc->video0_frame_interval);
			if (ret)
				break;
			dma0_index_after_vip = readl(gc->bar0 + desc_index);
		}
		dev_info(&gc->pdev->dev,
			 "DMA%u armed image=%pad sg=%pad start=0x%08x irq_mask=0x%08x irq=0x%08x reset=0x%08x index=0x%08x control=0x%08x desc0=%08x:%08x/%u sg0=%08x:%08x/%08x/%08x\n",
			 channel, &image_dma, &sg_dma,
			 readl(gc->bar0 + GC570D_REG_DMA_START),
			 readl(gc->bar0 + GC570D_REG_IRQ_MASK),
			 readl(gc->bar0 + GC570D_REG_IRQ_STATUS),
			 readl(gc->bar0 + GC570D_REG_RESET_STATUS),
			 readl(gc->bar0 + desc_index),
			 readl(gc->bar0 + desc_control),
			 readl(gc->bar0 + desc_address + 4),
			 readl(gc->bar0 + desc_address),
			 readl(gc->bar0 + desc_address + 8),
			 le32_to_cpu(sg_cpu[1]), le32_to_cpu(sg_cpu[0]),
			 le32_to_cpu(sg_cpu[2]), le32_to_cpu(sg_cpu[3]));
		if (channel == 0) {
			dev_info(&gc->pdev->dev,
				 "DMA0 control RMW before=%08x publish=%08x/%08x/%08x/%08x start=%08x\n",
				 dma0_control_before, dma0_control_publish[0],
				 dma0_control_publish[1], dma0_control_publish[2],
				 dma0_control_publish[3],
				 readl(gc->bar0 + desc_control));
			dev_info(&gc->pdev->dev,
				 "DMA0 index stages pre_preprocess=%08x pre_publish=%08x publish=%08x/%08x/%08x/%08x post_start=%08x post_vip=%08x\n",
				 dma0_index_before_preprocess,
				 dma0_index_before_publish, dma0_index_publish[0],
				 dma0_index_publish[1], dma0_index_publish[2],
				 dma0_index_publish[3], dma0_index_after_start,
				 dma0_index_after_vip);
			dev_info(&gc->pdev->dev,
				 "DMA0 official-driver pre-VIP event ACK=%08x->%08x\n",
				 dma0_vip_event_before_ack,
				 dma0_vip_event_after_ack);

			vip_event_before = readl(gc->bar0 +
						 GC570D_VIP0_EVENT_STATUS);
			vip_active_before = readl(gc->bar0 +
						  GC570D_VIP0_ACTIVE_STATUS);
			vip_period_before = readl(gc->bar0 +
						  GC570D_VIP0_PERIOD_STATUS);
			for (slot = 0; slot < ring_slots; slot++) {
				__le32 *slot_sg = slot_sg_cpu[slot];
				u32 slot_address = desc_address + slot * 0x0c;

				dev_info(&gc->pdev->dev,
					 "DMA0 slot%u desc=%08x:%08x/%u sg_first=%08x:%08x/%08x/%08x sg_last=%08x:%08x/%08x/%08x image=%pad sg_dma=%pad\n",
					 slot,
					 readl(gc->bar0 + slot_address + 4),
					 readl(gc->bar0 + slot_address),
					 readl(gc->bar0 + slot_address + 8),
					 le32_to_cpu(slot_sg[1]),
					 le32_to_cpu(slot_sg[0]),
					 le32_to_cpu(slot_sg[2]),
					 le32_to_cpu(slot_sg[3]),
					 le32_to_cpu(slot_sg[(capture_height - 1) * 4 + 1]),
					 le32_to_cpu(slot_sg[(capture_height - 1) * 4]),
					 le32_to_cpu(slot_sg[(capture_height - 1) * 4 + 2]),
					 le32_to_cpu(slot_sg[(capture_height - 1) * 4 + 3]),
					 &slot_image_dma[slot],
					 &slot_sg_dma[slot]);
			}
		}
		if (channel == 0)
			dev_info(&gc->pdev->dev,
				 "VIP0 armed ctrl=%08x enable=%08x aux=%08x/%08x/%08x/%08x bypass=%08x start=%08x window=%08x/%08x/%08x/%08x scaler=%08x scale=%08x/%08x size=%08x/%08x\n",
				 readl(gc->bar0 + GC570D_VIP0_CHANNEL_CTRL),
				 readl(gc->bar0 + GC570D_VIP_GLOBAL_ENABLE),
				 readl(gc->bar0 + GC570D_VIP_AUX_0),
				 readl(gc->bar0 + GC570D_VIP_AUX_1),
				 readl(gc->bar0 + GC570D_VIP_AUX_2),
				 readl(gc->bar0 + GC570D_VIP_AUX_3),
				 readl(gc->bar0 + GC570D_VIP_BYPASS),
				 readl(gc->bar0 + GC570D_VIP_GLOBAL_START),
				 readl(gc->bar0 + GC570D_VIP0_WINDOW_WIDTH),
				 readl(gc->bar0 + GC570D_VIP0_OUTPUT_WIDTH),
				 readl(gc->bar0 + GC570D_VIP0_WINDOW_HEIGHT),
				 readl(gc->bar0 + GC570D_VIP0_OUTPUT_HEIGHT),
				 readl(gc->bar0 + GC570D_VIP0_SCALER_CTRL),
				 readl(gc->bar0 + GC570D_VIP0_SCALE_X),
				 readl(gc->bar0 + GC570D_VIP0_SCALE_Y),
				 readl(gc->bar0 + GC570D_VIP0_INPUT_SIZE),
				 readl(gc->bar0 + GC570D_VIP0_OUTPUT_SIZE));
		}

		if (channel == 0) {
			unsigned int poll;

			ret = -ETIMEDOUT;
			for (poll = 0; poll < 40; poll++) {
				u32 poll_index;
				u32 poll_control;
				u32 poll_irq;

				if (wait_for_completion_timeout(&gc->dma0_completion,
							msecs_to_jiffies(50))) {
					ret = 0;
					break;
				}
				poll_index = readl(gc->bar0 + desc_index);
				poll_control = readl(gc->bar0 + desc_control);
				poll_irq = readl(gc->bar0 + GC570D_REG_IRQ_STATUS);
				if (!dma0_poll_sampled) {
					dma0_poll_index_first = poll_index;
					dma0_poll_control_first = poll_control;
					dma0_poll_sampled = true;
				} else {
					if (poll_index != dma0_poll_index_last) {
						dma0_poll_index_changes++;
						dev_info(&gc->pdev->dev,
							 "DMA0 50-ms poll%u index transition=%08x->%08x\n",
							 poll, dma0_poll_index_last,
							 poll_index);
					}
					if (poll_control != dma0_poll_control_last) {
						dma0_poll_control_changes++;
						dev_info(&gc->pdev->dev,
							 "DMA0 50-ms poll%u control transition=%08x->%08x\n",
							 poll, dma0_poll_control_last,
							 poll_control);
					}
				}
				dma0_poll_index_last = poll_index;
				dma0_poll_control_last = poll_control;
				if (poll_index & 7)
					dma0_poll_index_low_nonzero++;
				dma0_poll_irq_or |= poll_irq;
				dma0_vip_poll_ack_before = readl(gc->bar0 +
							GC570D_VIP0_EVENT_STATUS);
				if ((dma0_vip_poll_ack_before &
				     (BIT(0) | BIT(1))) == (BIT(0) | BIT(1))) {
					writel(BIT(1), gc->bar0 +
					       GC570D_VIP0_EVENT_STATUS);
					dma0_vip_poll_ack_count++;
				}
				dma0_vip_poll_ack_after = readl(gc->bar0 +
							GC570D_VIP0_EVENT_STATUS);
			}
		} else if (!wait_for_completion_timeout(&gc->dma_completion,
							msecs_to_jiffies(2000))) {
			ret = -ETIMEDOUT;
		} else {
			ret = 0;
		}
		if (channel == 0) {
			vip_event_after = readl(gc->bar0 +
						GC570D_VIP0_EVENT_STATUS);
			vip_active_after = readl(gc->bar0 +
						 GC570D_VIP0_ACTIVE_STATUS);
			vip_period_after = readl(gc->bar0 +
						 GC570D_VIP0_PERIOD_STATUS);
		}
		irq_status = channel == 0 ?
			READ_ONCE(gc->dma0_irq_status) :
			READ_ONCE(gc->dma_irq_status);
		control = readl(gc->bar0 + desc_control);
		hardware_index = readl(gc->bar0 + desc_index) & 7;
		if (ret || !hardware_index || hardware_index > ring_slots) {
			if (!ret)
				ret = -EIO;
			break;
		}
		if (channel == 0)
			dma0_completion_index[i] = hardware_index;
		completed++;
		writel(frame_irq,
		       gc->bar0 + GC570D_REG_IRQ_STATUS);
		if (dma0_continuous && i + 1 < frame_count) {
			reinit_completion(&gc->dma0_completion);
			WRITE_ONCE(gc->dma0_irq_status, 0);
			dma_wmb();
			writel(readl(gc->bar0 + desc_control) |
			       BIT(hardware_index), gc->bar0 + desc_control);
		} else {
			writel(0, gc->bar0 + desc_control);
		}
	}
	elapsed_us = ktime_us_delta(ktime_get(), capture_start);
	index = readl(gc->bar0 + desc_index) & 7;

	dma_rmb();
	if (channel == 0) {
		for (i = 0; i < ring_slots; i++) {
			size_t byte;
			u8 *frame = slot_image_cpu[i];

			for (byte = 0; byte < descriptor_bytes; byte++)
				changed += frame[byte] != GC570D_DMA_SENTINEL;
		}
	} else {
		for (i = 0; i < capture_bytes; i++)
			changed += ((u8 *)image_cpu)[i] != GC570D_DMA_SENTINEL;
	}
	gc570d_video_irq_put_locked(gc);
	irq_enabled = false;
	writel(frame_irq, gc->bar0 + GC570D_REG_IRQ_STATUS);
	writel(0, gc->bar0 + desc_control);
	if (channel == GC570D_DMA_CHANNEL)
		gc570d_reset_channel(gc, channel);
	else
		gc570d_reset_video_channel(gc, channel);
	pci_clear_master(gc->pdev);
	mastering = false;

	if (ret)
		goto out_free;
	if (completed != frame_count) {
		ret = -EIO;
		goto out_free;
	}

	dma_rmb();
	gc->capture_data = vmalloc(capture_bytes);
	if (!gc->capture_data) {
		ret = -ENOMEM;
		goto out_free;
	}
	if (channel == 0) {
		for (i = 0; i < ring_slots; i++)
			memcpy((u8 *)gc->capture_data +
			       i * frame_size,
			       slot_image_cpu[i], frame_size);
	} else {
		memcpy(gc->capture_data, image_cpu, capture_bytes);
	}
	gc->capture_size = capture_bytes;

out_free:
	if (irq_enabled)
		gc570d_video_irq_put_locked(gc);
	if (mastering) {
		writel(0, gc->bar0 + desc_control);
		if (channel == GC570D_DMA_CHANNEL)
			gc570d_reset_channel(gc, channel);
		else
			gc570d_reset_video_channel(gc, channel);
		pci_clear_master(gc->pdev);
	}
	if (channel == 0) {
		for (i = 0; i < ring_slots; i++) {
			if (slot_sg_cpu[i])
				dma_free_coherent(&gc->pdev->dev,
					GC570D_DMA_SG_SIZE, slot_sg_cpu[i],
					slot_sg_dma[i]);
			if (slot_image_cpu[i])
				dma_free_coherent(&gc->pdev->dev,
					GC570D_DMA_IMAGE_SIZE,
					slot_image_cpu[i], slot_image_dma[i]);
		}
	} else {
		// Fixed: free buffers using dynamically calculated sizes
		if (sg_cpu)
			dma_free_coherent(&gc->pdev->dev, sg_alloc_size,
					  sg_cpu, sg_dma);
		if (image_cpu)
			dma_free_coherent(&gc->pdev->dev,
					  capture_bytes,
					  image_cpu, image_dma);
	}

out_status:
	gc->capture_irq = irq_status;
	gc->capture_index = index;
	gc->capture_control = control;
	gc->capture_changed = changed;
	gc->capture_elapsed_us = elapsed_us;
	gc->capture_error = ret;
	dev_info(&gc->pdev->dev,
		 "DMA%u capture frames=%u buffers=%u completed=%u result=%d elapsed_us=%lld irq=0x%08x control=0x%08x changed=%zu/%zu\n",
		 channel, frame_count, buffer_count, completed, ret, elapsed_us,
		 irq_status, control, changed, inspected_bytes);
	if (channel == 0)
		dev_info(&gc->pdev->dev,
			 "VIP0 official-driver status event=%08x->%08x active=%08x->%08x period=%08x->%08x\n",
			 vip_event_before, vip_event_after,
			 vip_active_before, vip_active_after,
			 vip_period_before, vip_period_after);
	if (channel == 0)
		dev_info(&gc->pdev->dev,
			 "VIP0 official-driver 50-ms poll ACK count=%u last=%08x->%08x\n",
			 dma0_vip_poll_ack_count, dma0_vip_poll_ack_before,
			 dma0_vip_poll_ack_after);
	if (channel == 0)
		dev_info(&gc->pdev->dev,
			 "DMA0 official-driver-wait polling index=%08x->%08x changes=%u low_nonzero=%u control=%08x->%08x changes=%u irq_or=%08x\n",
			 dma0_poll_index_first, dma0_poll_index_last,
			 dma0_poll_index_changes, dma0_poll_index_low_nonzero,
			 dma0_poll_control_first, dma0_poll_control_last,
			 dma0_poll_control_changes, dma0_poll_irq_or);
	if (channel == 0)
		dev_info(&gc->pdev->dev,
			 "DMA0 completion indices=%u/%u/%u/%u\n",
			 dma0_completion_index[0], dma0_completion_index[1],
			 dma0_completion_index[2], dma0_completion_index[3]);
	WRITE_ONCE(gc->dma0_continuous, false);
	mutex_unlock(&gc->capture_lock);

	return ret;
}

static int gc570d_dma_capture_frames(struct gc570d_dev *gc,
				     unsigned int frame_count,
				     unsigned int buffer_count)
{
	return gc570d_dma_capture_frames_channel(gc, GC570D_DMA_CHANNEL,
						 frame_count, buffer_count);
}

static ssize_t gc570d_dma0_capture_write(struct file *file,
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

	ret = gc570d_dma_capture_frames_channel(gc, 0, 1, 1);
	return ret ? ret : count;
}

const struct file_operations gc570d_dma0_capture_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_dma0_capture_write,
	.llseek = noop_llseek,
};

static ssize_t gc570d_dma0_burst_write(struct file *file,
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

	ret = gc570d_dma_capture_frames_channel(gc, 0,
						 GC570D_DMA_BURST_FRAMES,
						 GC570D_DMA_BURST_FRAMES);
	return ret ? ret : count;
}

const struct file_operations gc570d_dma0_burst_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_dma0_burst_write,
	.llseek = noop_llseek,
};

static ssize_t gc570d_dma_capture_write(struct file *file,
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

	ret = gc570d_dma_capture_frames(gc, 1, 1);
	return ret ? ret : count;
}

const struct file_operations gc570d_dma_capture_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_dma_capture_write,
	.llseek = noop_llseek,
};

static ssize_t gc570d_dma_burst_write(struct file *file,
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

	ret = gc570d_dma_capture_frames(gc, GC570D_DMA_BURST_FRAMES,
					GC570D_DMA_BURST_FRAMES);
	return ret ? ret : count;
}

const struct file_operations gc570d_dma_burst_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_dma_burst_write,
	.llseek = noop_llseek,
};

static ssize_t gc570d_dma_stress_write(struct file *file,
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

	ret = gc570d_dma_capture_frames(gc, GC570D_DMA_STRESS_FRAMES,
					GC570D_DMA_BURST_FRAMES);
	return ret ? ret : count;
}

const struct file_operations gc570d_dma_stress_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_dma_stress_write,
	.llseek = noop_llseek,
};

static ssize_t gc570d_dma_frame_read(struct file *file, char __user *buffer,
				      size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	ssize_t ret;

	mutex_lock(&gc->capture_lock);
	ret = simple_read_from_buffer(buffer, count, position, gc->capture_data,
				      gc->capture_size);
	mutex_unlock(&gc->capture_lock);
	return ret;
}

const struct file_operations gc570d_dma_frame_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = gc570d_dma_frame_read,
	.llseek = default_llseek,
};

static int gc570d_dma_status_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;

	mutex_lock(&gc->capture_lock);
	seq_printf(s,
		   "error=%d irq=0x%08x index=%u control=0x%08x changed=%zu size=%zu elapsed_us=%llu\n",
		   gc->capture_error, gc->capture_irq, gc->capture_index,
		   gc->capture_control, gc->capture_changed, gc->capture_size,
		   gc->capture_elapsed_us);
	mutex_unlock(&gc->capture_lock);
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_dma_status);

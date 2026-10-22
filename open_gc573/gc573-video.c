// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/version.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-sg.h>
#include <media/videobuf2-v4l2.h>

#include "gc573.h"

#define GC555_VIDEO_MIN_QUEUED_BUFFERS	2U
#define GC555_VIDEO_POLL_MS		100U
#define GC555_CID_HDR_MODE		(V4L2_CID_USER_BASE + 0x1000)
#define GC555_VIDEO_DEFAULT_WIDTH	1920U
#define GC555_VIDEO_DEFAULT_HEIGHT	1080U
#define GC555_VIDEO_DEFAULT_RATE	60U

struct gc555_video_format_info {
	u32 fourcc;
	enum gc555_video_format format;
	const char *description;
	bool yuv;
};

struct gc555_video_mode {
	u16 width;
	u16 height;
	u8 rates[16];
	u8 rate_count;
};

struct gc555_video_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head node;
	bool driver_owned;
};

struct gc555_video {
	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct video_device vdev;
	struct vb2_queue queue;
	/* Serializes format negotiation and vb2 queue operations. */
	struct mutex lock;
	/* Protects signal/stream state and the driver-owned buffer list. */
	spinlock_t queue_lock;
	struct list_head owned_buffers;
	struct delayed_work monitor_work;
	struct gc555_dev *gc555;
	const struct gc555_video_format_info *format_info;
	struct v4l2_pix_format pix;
	struct v4l2_dv_timings configured_timings;
	struct gc555_video_signal cached_signal;
	struct gc555_video_signal stream_signal;
	atomic64_t last_published_ns;
	u32 frame_rate_hz;
	u32 stream_sizeimage;
	u32 sequence;
	bool signal_valid;
	bool configured_timings_valid;
	bool configured_timings_explicit;
	bool starting;
	bool streaming;
	bool detaching;
};

static const struct gc555_video_format_info gc555_video_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.format = GC555_VIDEO_FORMAT_YUYV,
		.description = "YUYV 4:2:2",
		.yuv = true,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12,
		.format = GC555_VIDEO_FORMAT_NV12,
		.description = "NV12 4:2:0",
		.yuv = true,
	}, {
		.fourcc = V4L2_PIX_FMT_P010,
		.format = GC555_VIDEO_FORMAT_P010,
		.description = "P010 10-bit 4:2:0",
		.yuv = true,
	}, {
		.fourcc = V4L2_PIX_FMT_BGR24,
		.format = GC555_VIDEO_FORMAT_BGR24,
		.description = "24-bit BGR",
	}, {
		.fourcc = V4L2_PIX_FMT_RGB32,
		.format = GC555_VIDEO_FORMAT_RGB32,
		.description = "32-bit BGRX",
	},
};

static const struct gc555_video_mode gc555_video_modes[] = {
	{ 640, 400, { 70 }, 1 },
	{ 640, 480, { 60, 75 }, 2 },
	{ 720, 400, { 70 }, 1 },
	{ 720, 480, { 24, 25, 30, 50, 60 }, 5 },
	{ 720, 576, { 25, 50, 60 }, 3 },
	{ 800, 600, { 60 }, 1 },
	{ 1024, 768, { 60, 75 }, 2 },
	{ 1280, 800, { 60 }, 1 },
	{ 1280, 960, { 60 }, 1 },
	{ 1280, 720, { 50, 60, 100, 120 }, 4 },
	{ 1280, 1024, { 60, 75 }, 2 },
	{ 1360, 765, { 60 }, 1 },
	{ 1440, 240, { 50, 60 }, 2 },
	{ 1440, 480, { 24, 25, 30, 50, 60 }, 5 },
	{ 1440, 576, { 25, 50, 60 }, 3 },
	{ 1440, 900, { 60 }, 1 },
	{ 1680, 1050, { 60 }, 1 },
	{ 1920, 240, { 50, 60 }, 2 },
	{ 1920, 1080, { 24, 25, 30, 50, 60, 70, 96, 100, 120, 144, 180, 240 }, 12 },
	{ 1920, 1200, { 60 }, 1 },
	{ 2560, 1080, { 50, 60, 100, 144 }, 4 },
	{ 2560, 1440, { 50, 60, 70, 100, 120, 144 }, 6 },
	{ 2880, 240, { 50, 60 }, 2 },
	{ 2880, 2160, { 24, 25, 30, 50, 60 }, 5 },
	{ 3440, 1440, { 50, 60, 100 }, 3 },
	{ 3840, 1080, { 60, 120 }, 2 },
	{ 3840, 2160, { 24, 25, 30, 50, 60 }, 5 },
};

static const struct gc555_video_format_info *
gc555_video_find_format(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gc555_video_formats); i++)
		if (gc555_video_formats[i].fourcc == fourcc)
			return &gc555_video_formats[i];

	return NULL;
}

static const struct gc555_video_mode *
gc555_video_find_mode(u32 width, u32 height)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gc555_video_modes); i++)
		if (gc555_video_modes[i].width == width &&
		    gc555_video_modes[i].height == height)
			return &gc555_video_modes[i];

	return NULL;
}

static bool
gc555_video_mode_supports_rate(const struct gc555_video_mode *mode, u32 rate)
{
	unsigned int i;

	if (!mode)
		return false;
	for (i = 0; i < mode->rate_count; i++) {
		int diff = (int)mode->rates[i] - (int)rate;
		int tolerance = mode->rates[i] >= 100 ? 5 : 2;

		if (diff >= -tolerance && diff <= tolerance)
			return true;
	}

	return false;
}

static bool
gc555_video_mode_supports_p010(const struct gc555_video_mode *mode)
{
	if (!mode)
		return false;

	switch (mode->width) {
	case 640:
		return mode->height == 400 || mode->height == 480;
	case 720:
		return mode->height == 400 || mode->height == 480 ||
		       mode->height == 576;
	case 800:
		return mode->height == 600;
	case 1024:
		return mode->height == 768;
	case 1280:
		return mode->height == 720 || mode->height == 800 ||
		       mode->height == 960 || mode->height == 1024;
	case 1360:
		return mode->height == 765;
	case 1440:
		return mode->height == 240 || mode->height == 480 ||
		       mode->height == 576 || mode->height == 900;
	case 1680:
		return mode->height == 1050;
	case 1920:
		return mode->height == 240 || mode->height == 1080 ||
		       mode->height == 1200;
	case 2560:
		return mode->height == 1080 || mode->height == 1440;
	case 2880:
		return mode->height == 240 || mode->height == 2160;
	case 3440:
		return mode->height == 1440;
	case 3840:
		return mode->height == 2160 || mode->height == 1080;
	default:
		return false;
	}
}

static bool
gc555_video_p010_supports_rate(const struct gc555_video_mode *mode, u32 rate)
{
	if (!gc555_video_mode_supports_p010(mode))
		return false;

	return gc555_video_mode_supports_rate(mode, rate);
}

static bool
gc555_video_supports_format(const struct gc555_video_mode *mode,
			    const struct gc555_video_format_info *format)
{
	if (!mode || !format)
		return false;
	if (format->format == GC555_VIDEO_FORMAT_YUYV ||
	    format->format == GC555_VIDEO_FORMAT_NV12)
		return true;
	if (format->format == GC555_VIDEO_FORMAT_P010)
		return gc555_video_mode_supports_p010(mode);

	/* RGB host layouts (BGR24, RGB32) are supported for progressive modes */
	return mode->height != 240;
}

static bool
gc555_video_supports_rate(const struct gc555_video_mode *mode,
			  const struct gc555_video_format_info *format,
			  u32 rate)
{
	if (!gc555_video_supports_format(mode, format))
		return false;
	if (format->format == GC555_VIDEO_FORMAT_P010)
		return gc555_video_p010_supports_rate(mode, rate);

	return gc555_video_mode_supports_rate(mode, rate);
}

static const struct gc555_video_mode *
gc555_video_mode_by_index(const struct gc555_video_format_info *format,
			  u32 index)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gc555_video_modes); i++) {
		if (!gc555_video_supports_format(&gc555_video_modes[i], format))
			continue;
		if (!index--)
			return &gc555_video_modes[i];
	}

	return NULL;
}

static bool
gc555_video_supports_signal(const struct gc555_video_format_info *format,
			    const struct gc555_video_signal *signal)
{
	const struct gc555_video_mode *mode;

	if (!format || !signal)
		return false;
	mode = gc555_video_find_mode(signal->width, signal->height);
	if (!gc555_video_supports_rate(mode, format, signal->frame_rate_hz))
		return false;

	return format->format == GC555_VIDEO_FORMAT_YUYV ||
	       !signal->interlaced;
}

static int
gc555_video_layout(const struct gc555_video_format_info *format,
		   u32 width, u32 height, u32 *bytesperline,
		   u32 *sizeimage, size_t *luma_size, size_t *chroma_size)
{
	size_t pixels;
	size_t line;
	size_t size;
	size_t luma;
	size_t chroma = 0;

	if (!format || !width || !height ||
	    check_mul_overflow((size_t)width, (size_t)height, &pixels))
		return -EINVAL;

	switch (format->format) {
	case GC555_VIDEO_FORMAT_YUYV:
		if (check_mul_overflow((size_t)width, 2UL, &line) ||
		    check_mul_overflow(pixels, 2UL, &size))
			return -EOVERFLOW;
		luma = size;
		break;
	case GC555_VIDEO_FORMAT_NV12:
		line = width;
		luma = pixels;
		chroma = pixels / 2;
		if (check_add_overflow(luma, chroma, &size))
			return -EOVERFLOW;
		break;
	case GC555_VIDEO_FORMAT_P010:
		if (check_mul_overflow((size_t)width, 2UL, &line) ||
		    check_mul_overflow(pixels, 2UL, &luma) ||
		    check_mul_overflow(pixels, 3UL, &size))
			return -EOVERFLOW;
		chroma = pixels;
		break;
	case GC555_VIDEO_FORMAT_BGR24:
		if (check_mul_overflow((size_t)width, 3UL, &line) ||
		    check_mul_overflow(pixels, 3UL, &size))
			return -EOVERFLOW;
		luma = size;
		break;
	case GC555_VIDEO_FORMAT_RGB32:
		if (check_mul_overflow((size_t)width, 4UL, &line) ||
		    check_mul_overflow(pixels, 4UL, &size))
			return -EOVERFLOW;
		luma = size;
		break;
	default:
		return -EINVAL;
	}

	if (line > U32_MAX || size > U32_MAX)
		return -EOVERFLOW;
	if (bytesperline)
		*bytesperline = line;
	if (sizeimage)
		*sizeimage = size;
	if (luma_size)
		*luma_size = luma;
	if (chroma_size)
		*chroma_size = chroma;

	return 0;
}

static void
gc555_video_set_colorimetry(const struct gc555_video_format_info *format,
			    const struct gc555_video_signal *signal,
			    struct v4l2_pix_format *pix)
{
	enum gc555_video_colorimetry colorimetry;

	bool is_hdr = signal && (signal->hdr_mode == GC555_VIDEO_HDR_PQ ||
				 signal->hdr_mode == GC555_VIDEO_HDR_PQ_BT2020 ||
				 signal->hdr_mode == GC555_VIDEO_HDR_HLG);

	if (!format->yuv) {
		pix->colorspace = V4L2_COLORSPACE_SRGB;
		pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
		return;
	}

	if (is_hdr) {
		pix->colorspace = V4L2_COLORSPACE_BT2020;
		pix->xfer_func = (signal->hdr_mode == GC555_VIDEO_HDR_HLG) ?
			V4L2_XFER_FUNC_DEFAULT : V4L2_XFER_FUNC_SMPTE2084;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_BT2020;
		pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
		return;
	}

	colorimetry = signal ? signal->colorimetry :
		(pix->width <= 720 && pix->height <= 576 ?
		 GC555_VIDEO_COLORIMETRY_BT601 :
		 GC555_VIDEO_COLORIMETRY_BT709);
	switch (colorimetry) {
	case GC555_VIDEO_COLORIMETRY_BT601:
		pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
		break;
	case GC555_VIDEO_COLORIMETRY_BT2020:
		pix->colorspace = V4L2_COLORSPACE_BT2020;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_BT2020;
		break;
	case GC555_VIDEO_COLORIMETRY_UNKNOWN:
	case GC555_VIDEO_COLORIMETRY_BT709:
	default:
		pix->colorspace = V4L2_COLORSPACE_REC709;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
		break;
	}
	pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	pix->quantization = (colorimetry == GC555_VIDEO_COLORIMETRY_BT2020) ?
		V4L2_QUANTIZATION_LIM_RANGE : V4L2_QUANTIZATION_FULL_RANGE;
}

static int
gc555_video_fill_pix(const struct gc555_video_format_info *format,
		     u32 width, u32 height,
		     const struct gc555_video_signal *signal,
		     struct v4l2_pix_format *pix)
{
	u32 bytesperline;
	u32 sizeimage;
	int ret;

	ret = gc555_video_layout(format, width, height, &bytesperline,
				 &sizeimage, NULL, NULL);
	if (ret)
		return ret;

	memset(pix, 0, sizeof(*pix));
	pix->width = width;
	pix->height = height;
	pix->pixelformat = format->fourcc;
	pix->field = signal && signal->interlaced ?
		V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	pix->bytesperline = bytesperline;
	pix->sizeimage = sizeimage;
	gc555_video_set_colorimetry(format, signal, pix);

	return 0;
}

static int gc555_video_get_signal(struct gc555_video *video,
				  struct gc555_video_signal *signal)
{
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);

	if (!gc555)
		return -ENODEV;

	return gc555_link_get_video_signal(gc555, signal);
}

static int gc555_video_get_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc555_video *video =
		container_of(ctrl->handler, struct gc555_video, ctrl_handler);
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	unsigned long flags;
	bool present;
	int ret;

	if (!gc555 || gc555_dma_device_lost(gc555))
		return -ENODEV;

	switch (ctrl->id) {
	case V4L2_CID_DV_RX_POWER_PRESENT:
		ret = gc555_link_get_input_power(gc555, &present);
		if (!ret)
			ctrl->val = present;
		return ret;
	case GC555_CID_HDR_MODE:
		spin_lock_irqsave(&video->queue_lock, flags);
		ctrl->val = video->signal_valid ?
			video->cached_signal.hdr_mode : GC555_VIDEO_HDR_SDR;
		spin_unlock_irqrestore(&video->queue_lock, flags);
		return 0;
	default:
		return -EINVAL;
	}
}

static int gc555_video_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc555_video *video =
		container_of(ctrl->handler, struct gc555_video, ctrl_handler);
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	enum gc555_fpga_color_control control;

	if (!gc555 || gc555_dma_device_lost(gc555))
		return -ENODEV;

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		control = GC555_FPGA_COLOR_BRIGHTNESS;
		break;
	case V4L2_CID_CONTRAST:
		control = GC555_FPGA_COLOR_CONTRAST;
		break;
	case V4L2_CID_HUE:
		control = GC555_FPGA_COLOR_HUE;
		break;
	case V4L2_CID_SATURATION:
		control = GC555_FPGA_COLOR_SATURATION;
		break;
	default:
		return -EINVAL;
	}

	return gc555_fpga_set_color_control(gc555, control, ctrl->val);
}

static const struct v4l2_ctrl_ops gc555_video_ctrl_ops = {
	.s_ctrl = gc555_video_set_ctrl,
	.g_volatile_ctrl = gc555_video_get_volatile_ctrl,
};

static const struct v4l2_ctrl_config gc555_ctrl_hdr_mode = {
	.ops = &gc555_video_ctrl_ops,
	.id = GC555_CID_HDR_MODE,
	.name = "hdr_mode",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = GC555_VIDEO_HDR_SDR,
	.max = GC555_VIDEO_HDR_HLG,
	.step = 1,
	.def = GC555_VIDEO_HDR_SDR,
	.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
};

static bool gc555_video_signal_equal(const struct gc555_video_signal *a,
				     const struct gc555_video_signal *b)
{
	return a->width == b->width && a->height == b->height &&
	       a->frame_rate_hz == b->frame_rate_hz &&
	       a->input_class == b->input_class &&
	       a->encoding == b->encoding && a->sampling == b->sampling &&
	       a->colorimetry == b->colorimetry &&
	       a->hdr_mode == b->hdr_mode &&
	       a->interlaced == b->interlaced &&
	       a->dual_pixel == b->dual_pixel && a->ddr == b->ddr;
}


static bool
gc555_video_stream_compatible(const struct gc555_video_signal *sample,
			      const struct gc555_video_signal *stream,
			      enum gc555_video_format format)
{
	/*
	 * Frame transport dimensions and bus layout must match.
	 * HDR and colorimetry changes keep the same pixel layout.
	 * The monitor updates the color matrix only. A full VIP
	 * reconfigure while DMA is live stops the engine.
	 */
	return sample->width == stream->width &&
	       sample->height == stream->height &&
	       sample->input_class == stream->input_class &&
	       sample->encoding == stream->encoding &&
	       sample->sampling == stream->sampling &&
	       sample->interlaced == stream->interlaced &&
	       sample->dual_pixel == stream->dual_pixel &&
	       sample->ddr == stream->ddr;
}

static void
gc555_video_update_format_locked(struct gc555_video *video,
				 const struct gc555_video_signal *signal)
{
	const struct gc555_video_format_info *format = video->format_info;
	struct v4l2_pix_format pix;

	if (!signal || video->configured_timings_explicit ||
	    vb2_is_busy(&video->queue) ||
	    !gc555_video_find_mode(signal->width, signal->height))
		return;
	if (!gc555_video_supports_signal(format, signal))
		format = &gc555_video_formats[0];
	if (!gc555_video_supports_signal(format, signal) ||
	    gc555_video_fill_pix(format, signal->width,
				 signal->height, signal, &pix))
		return;

	video->format_info = format;
	video->pix = pix;
	video->frame_rate_hz = signal->frame_rate_hz;
}

static void gc555_video_queue_source_change(struct gc555_video *video)
{
	const struct v4l2_event event = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	if (video_is_registered(&video->vdev))
		v4l2_event_queue(&video->vdev, &event);
}

static void gc555_video_monitor_work(struct work_struct *work)
{
	struct gc555_video *video =
		container_of(to_delayed_work(work), struct gc555_video,
			     monitor_work);
	struct gc555_video_signal signal = {};
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	bool signal_valid;
	bool changed;
	bool device_lost;
	unsigned long flags;
	int ret;

	if (READ_ONCE(video->detaching) || !gc555)
		return;

	device_lost = gc555_dma_device_lost(gc555);
	ret = device_lost ? -ENODEV : gc555_link_get_video_signal(gc555,
								  &signal);
	signal_valid = !ret;

	mutex_lock(&video->lock);
	spin_lock_irqsave(&video->queue_lock, flags);
	changed = signal_valid != video->signal_valid;
	if (signal_valid &&
	    !gc555_video_signal_equal(&signal, &video->cached_signal))
		changed = true;
	video->signal_valid = signal_valid;
	if (signal_valid)
		video->cached_signal = signal;
	spin_unlock_irqrestore(&video->queue_lock, flags);
	if (signal_valid)
		gc555_video_update_format_locked(video, &signal);
	if (signal_valid && vb2_is_streaming(&video->queue)) {
		if (signal.frame_rate_hz != video->stream_signal.frame_rate_hz &&
		    signal.width == video->stream_signal.width &&
		    signal.height == video->stream_signal.height) {
			dev_info(gc555->dev, "Input frame rate changed: %u Hz -> %u Hz\n",
				 video->stream_signal.frame_rate_hz, signal.frame_rate_hz);
			spin_lock_irqsave(&video->queue_lock, flags);
			video->stream_signal.frame_rate_hz = signal.frame_rate_hz;
			video->frame_rate_hz = signal.frame_rate_hz;
			spin_unlock_irqrestore(&video->queue_lock, flags);
			gc555_fpga_set_frame_rate(gc555, signal.frame_rate_hz, signal.frame_rate_x100);
		}
		if ((signal.hdr_mode != video->stream_signal.hdr_mode ||
		     signal.colorimetry != video->stream_signal.colorimetry) &&
		    signal.width == video->stream_signal.width &&
		    signal.height == video->stream_signal.height) {
			dev_info(gc555->dev, "Input HDR/Colorimetry changed: HDR=%d -> %d, Colorimetry=%d -> %d\n",
				 video->stream_signal.hdr_mode, signal.hdr_mode,
				 video->stream_signal.colorimetry, signal.colorimetry);
			spin_lock_irqsave(&video->queue_lock, flags);
			video->stream_signal.hdr_mode = signal.hdr_mode;
			video->stream_signal.colorimetry = signal.colorimetry;
			spin_unlock_irqrestore(&video->queue_lock, flags);
			ret = gc555_fpga_update_color_path(gc555, &signal,
							  video->format_info->format,
							  video->pix.width,
							  video->pix.height,
							  signal.frame_rate_hz);
			if (ret)
				dev_warn(gc555->dev,
					 "HDR color path update failed: %d\n",
					 ret);
		}
		gc555_video_set_colorimetry(video->format_info, &signal,
					    &video->pix);
	}
	if (device_lost && vb2_is_streaming(&video->queue))
		vb2_queue_error(&video->queue);
	if (changed)
		gc555_video_queue_source_change(video);
	mutex_unlock(&video->lock);

	if (!READ_ONCE(video->detaching)) {
		unsigned int poll_ms = vb2_is_streaming(&video->queue) ? 1000U : GC555_VIDEO_POLL_MS;
		schedule_delayed_work(&video->monitor_work,
				      msecs_to_jiffies(poll_ms));
	}
}

static struct gc555_video_buffer *
gc555_video_to_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb),
			    struct gc555_video_buffer, vb);
}

static void gc555_video_return_buffers(struct gc555_video *video,
				       enum vb2_buffer_state state)
{
	struct gc555_video_buffer *buffer;
	struct gc555_video_buffer *next;
	unsigned long flags;
	LIST_HEAD(buffers);

	spin_lock_irqsave(&video->queue_lock, flags);
	list_splice_init(&video->owned_buffers, &buffers);
	list_for_each_entry(buffer, &buffers, node)
		buffer->driver_owned = false;
	spin_unlock_irqrestore(&video->queue_lock, flags);

	list_for_each_entry_safe(buffer, next, &buffers, node) {
		list_del_init(&buffer->node);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
}

static void gc555_video_dma_error(void *context)
{
	struct gc555_video *video = context;
	unsigned long flags;

	vb2_queue_error(&video->queue);
	spin_lock_irqsave(&video->queue_lock, flags);
	video->starting = false;
	video->streaming = false;
	spin_unlock_irqrestore(&video->queue_lock, flags);
	atomic64_set(&video->last_published_ns, 0);
	gc555_video_return_buffers(video, VB2_BUF_STATE_ERROR);
}

static int gc555_video_queue_setup(struct vb2_queue *queue,
				   unsigned int *num_buffers,
				   unsigned int *num_planes,
				   unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct gc555_video *video = vb2_get_drv_priv(queue);

	if (*num_planes) {
		if (*num_planes != 1 || sizes[0] < video->pix.sizeimage)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = video->pix.sizeimage;
	return 0;
}

static int gc555_video_buffer_init(struct vb2_buffer *vb)
{
	struct gc555_video_buffer *buffer = gc555_video_to_buffer(vb);

	INIT_LIST_HEAD(&buffer->node);
	buffer->driver_owned = false;
	return 0;
}

static int gc555_video_buffer_prepare(struct vb2_buffer *vb)
{
	struct gc555_video *video = vb2_get_drv_priv(vb->vb2_queue);
	struct gc555_video_buffer *buffer = gc555_video_to_buffer(vb);
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	struct sg_table *sgt;
	size_t luma_size;
	size_t chroma_size;
	int ret;

	if (!gc555)
		return -ENODEV;
	if (vb2_plane_size(vb, 0) < video->pix.sizeimage)
		return -EINVAL;

	ret = gc555_video_layout(video->format_info, video->pix.width,
				 video->pix.height, NULL, NULL,
				 &luma_size, &chroma_size);
	if (ret)
		return ret;
	sgt = vb2_dma_sg_plane_desc(vb, 0);
	if (!sgt)
		return -EINVAL;

	ret = gc555_video_dma_prepare(gc555, buffer, sgt,
				      video->format_info->format,
				      luma_size, chroma_size);
	if (ret)
		return ret;

	vb2_set_plane_payload(vb, 0, video->pix.sizeimage);
	return 0;
}

static void gc555_video_buffer_cleanup(struct vb2_buffer *vb)
{
	struct gc555_video *video = vb2_get_drv_priv(vb->vb2_queue);
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	struct gc555_video_buffer *buffer = gc555_video_to_buffer(vb);
	int ret;

	if (!gc555)
		return;
	ret = gc555_video_dma_cleanup_buffer(gc555, buffer);
	if (ret && ret != -ENOENT)
		dev_warn(gc555->dev,
			 "video buffer cleanup failed: %d\n", ret);
}

static int gc555_video_unpack_p010(struct gc555_video *video,
				   struct gc555_video_buffer *buffer,
				   u32 width, u32 height)
{
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	struct vb2_buffer *vb = &buffer->vb.vb2_buf;
	struct sg_table *sgt;
	u8 *data;
	u32 row;

	if (!gc555)
		return -ENODEV;
	if (!width || !height || width % 8)
		return -EINVAL;
	sgt = vb2_dma_sg_plane_desc(vb, 0);
	if (!sgt)
		return -EINVAL;

	dma_sync_sgtable_for_cpu(gc555->dev, sgt, DMA_FROM_DEVICE);
	data = vb2_plane_vaddr(vb, 0);
	if (!data)
		return -EFAULT;

	/* Expand in reverse because input and output share storage. */
	for (row = height; row-- > 0;) {
		u8 *source = data + (size_t)row * width * 3 / 2;
		u8 *destination = data + (size_t)row * width * 2;
		u32 pair;

		for (pair = width / 2; pair-- > 0;) {
			u32 source_offset = pair * 3;
			u32 destination_offset = pair * 4;
			u8 packed0 = source[source_offset];
			u8 packed1 = source[source_offset + 1];
			u8 packed2 = source[source_offset + 2];
			u16 pixel0 = ((u16)packed0 |
				      ((u16)(packed1 & 0x0f) << 8)) << 4;
			u16 pixel1 = ((u16)(packed1 >> 4) |
				      ((u16)packed2 << 4)) << 4;

			destination[destination_offset] = pixel0;
			destination[destination_offset + 1] = pixel0 >> 8;
			destination[destination_offset + 2] = pixel1;
			destination[destination_offset + 3] = pixel1 >> 8;
		}
	}

	return 0;
}

static enum gc555_video_dma_completion
gc555_video_buffer_complete(void *cookie, u64 completion_ns, void *context)
{
	struct gc555_video_buffer *buffer = cookie;
	struct gc555_video *video = context;
	const struct gc555_video_format_info *format;
	struct gc555_video_signal stream_signal;
	struct gc555_video_signal current_signal = {};
	enum vb2_buffer_state state;
	unsigned long flags;
	u64 last_published_ns;
	u32 sequence;
	u32 sizeimage;
	bool signal_valid;
	bool starting;
	bool streaming;
	int ret;

	spin_lock_irqsave(&video->queue_lock, flags);
	starting = video->starting;
	streaming = video->streaming;
	stream_signal = video->stream_signal;
	current_signal = video->cached_signal;
	signal_valid = video->signal_valid;
	format = video->format_info;
	sizeimage = video->stream_sizeimage;
	spin_unlock_irqrestore(&video->queue_lock, flags);
	if (starting)
		return GC555_VIDEO_DMA_RECYCLE;
	if (!streaming)
		return GC555_VIDEO_DMA_RELEASE;

	if (!signal_valid ||
	    !gc555_video_stream_compatible(&current_signal, &stream_signal,
					   format->format)) {
		atomic64_set(&video->last_published_ns, 0);
		return GC555_VIDEO_DMA_RECYCLE;
	}

	if (current_signal.frame_rate_hz &&
	    current_signal.frame_rate_hz != stream_signal.frame_rate_hz) {
		spin_lock_irqsave(&video->queue_lock, flags);
		video->stream_signal.frame_rate_hz = current_signal.frame_rate_hz;
		video->frame_rate_hz = current_signal.frame_rate_hz;
		stream_signal.frame_rate_hz = current_signal.frame_rate_hz;
		spin_unlock_irqrestore(&video->queue_lock, flags);
		if (video->gc555)
			gc555_fpga_set_frame_rate(video->gc555, current_signal.frame_rate_hz,
						  current_signal.frame_rate_x100);
	}

	if (current_signal.hdr_mode != stream_signal.hdr_mode ||
	    current_signal.colorimetry != stream_signal.colorimetry)
		mod_delayed_work(system_wq, &video->monitor_work, 0);

	last_published_ns = atomic64_read(&video->last_published_ns);
	if (last_published_ns && completion_ns <= last_published_ns)
		return GC555_VIDEO_DMA_RECYCLE;

	if (format->format == GC555_VIDEO_FORMAT_P010)
		ret = gc555_video_unpack_p010(video, buffer,
					      stream_signal.width,
					      stream_signal.height);
	else
		ret = 0;

	spin_lock_irqsave(&video->queue_lock, flags);
	if (!buffer->driver_owned) {
		spin_unlock_irqrestore(&video->queue_lock, flags);
		return GC555_VIDEO_DMA_RELEASE;
	}
	list_del_init(&buffer->node);
	buffer->driver_owned = false;
	streaming = video->streaming;
	sequence = video->sequence++;
	state = ret || !streaming ? VB2_BUF_STATE_ERROR :
		VB2_BUF_STATE_DONE;
	spin_unlock_irqrestore(&video->queue_lock, flags);

	buffer->vb.field = stream_signal.interlaced ?
		V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	buffer->vb.sequence = sequence;
	buffer->vb.vb2_buf.timestamp = completion_ns;
	if (state == VB2_BUF_STATE_DONE)
		atomic64_set(&video->last_published_ns, completion_ns);
	vb2_buffer_done(&buffer->vb.vb2_buf, state);

	return GC555_VIDEO_DMA_RELEASE;
}

static void gc555_video_buffer_queue(struct vb2_buffer *vb)
{
	struct gc555_video *video = vb2_get_drv_priv(vb->vb2_queue);
	struct gc555_video_buffer *buffer = gc555_video_to_buffer(vb);
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	unsigned long flags;
	bool return_error = false;
	int ret;

	if (!gc555) {
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}

	spin_lock_irqsave(&video->queue_lock, flags);
	buffer->driver_owned = true;
	list_add_tail(&buffer->node, &video->owned_buffers);
	spin_unlock_irqrestore(&video->queue_lock, flags);

	ret = gc555_video_dma_queue(gc555, buffer,
				    gc555_video_buffer_complete, video);
	if (!ret)
		return;

	spin_lock_irqsave(&video->queue_lock, flags);
	if (buffer->driver_owned) {
		list_del_init(&buffer->node);
		buffer->driver_owned = false;
		return_error = true;
	}
	spin_unlock_irqrestore(&video->queue_lock, flags);
	if (return_error)
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
}

static void gc555_video_start_failed(struct gc555_video *video)
{
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	unsigned long flags;

	spin_lock_irqsave(&video->queue_lock, flags);
	video->starting = false;
	video->streaming = false;
	spin_unlock_irqrestore(&video->queue_lock, flags);
	atomic64_set(&video->last_published_ns, 0);
	if (gc555)
		gc555_video_dma_stop(gc555);
	gc555_video_return_buffers(video, VB2_BUF_STATE_QUEUED);
}

static int gc555_video_start_streaming(struct vb2_queue *queue,
				       unsigned int count)
{
	struct gc555_video *video = vb2_get_drv_priv(queue);
	struct gc555_video_signal signal = {};
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	unsigned long flags;
	int ret;

	if (!gc555) {
		gc555_video_start_failed(video);
		return -ENODEV;
	}
	if (count < GC555_VIDEO_MIN_QUEUED_BUFFERS) {
		gc555_video_start_failed(video);
		return -ENOBUFS;
	}

	ret = gc555_link_get_video_signal(gc555, &signal);
	if (ret)
		goto fail;
	if (!gc555_video_supports_signal(video->format_info, &signal)) {
		dev_info(&gc555->pdev->dev,
			 "stream: mode table rejects %ux%u@%u il=%d req_fmt=%s(%d) in_enc=%d\n",
			 signal.width, signal.height, signal.frame_rate_hz,
			 signal.interlaced, video->format_info->description,
			 video->format_info->format, signal.encoding);
		ret = -EOPNOTSUPP;
		goto fail;
	}
	if (video->pix.width != signal.width ||
	    video->pix.height != signal.height ||
	    video->pix.field != (signal.interlaced ?
				V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE)) {
		ret = -EPIPE;
		goto fail;
	}

	ret = gc555_fpga_validate_video(&signal, video->format_info->format,
					video->pix.width, video->pix.height,
					signal.frame_rate_hz);
	if (ret)
		goto fail;
	ret = gc555_video_dma_reset(gc555);
	if (ret)
		goto fail;
	ret = gc555_fpga_configure(gc555, &signal,
				   video->format_info->format,
				  video->pix.width, video->pix.height,
				  signal.frame_rate_hz);
	if (ret)
		goto fail;

	spin_lock_irqsave(&video->queue_lock, flags);
	video->stream_signal = signal;
	video->stream_sizeimage = video->pix.sizeimage;
	video->sequence = 0;
	video->starting = true;
	video->streaming = false;
	spin_unlock_irqrestore(&video->queue_lock, flags);
	atomic64_set(&video->last_published_ns, 0);

	ret = gc555_video_dma_start(gc555);
	if (ret)
		goto fail;
	spin_lock_irqsave(&video->queue_lock, flags);
	video->starting = false;
	video->streaming = true;
	spin_unlock_irqrestore(&video->queue_lock, flags);
	video->frame_rate_hz = signal.frame_rate_hz;
	return 0;

fail:
	gc555_video_start_failed(video);
	return ret;
}

static void gc555_video_stop_streaming(struct vb2_queue *queue)
{
	struct gc555_video *video = vb2_get_drv_priv(queue);
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	unsigned long flags;

	spin_lock_irqsave(&video->queue_lock, flags);
	video->starting = false;
	video->streaming = false;
	spin_unlock_irqrestore(&video->queue_lock, flags);
	atomic64_set(&video->last_published_ns, 0);
	if (gc555)
		gc555_video_dma_stop(gc555);
	gc555_video_return_buffers(video, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops gc555_video_queue_ops = {
	.queue_setup = gc555_video_queue_setup,
	.buf_init = gc555_video_buffer_init,
	.buf_prepare = gc555_video_buffer_prepare,
	.buf_cleanup = gc555_video_buffer_cleanup,
	.buf_queue = gc555_video_buffer_queue,
	.start_streaming = gc555_video_start_streaming,
	.stop_streaming = gc555_video_stop_streaming,
};

static int gc555_video_querycap(struct file *file, void *priv,
				struct v4l2_capability *capability)
{
	struct gc555_video *video = video_drvdata(file);
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);

	strscpy(capability->driver, "open_gc573", sizeof(capability->driver));
	strscpy(capability->card,
		gc555 && gc555->pdev->subsystem_device == 0x5730 ?
		"AVerMedia Live Gamer 4K GC573" :
		"AVerMedia Live Gamer BOLT GC555",
		sizeof(capability->card));
	if (gc555)
		snprintf(capability->bus_info, sizeof(capability->bus_info),
			 "PCI:%s", pci_name(gc555->pdev));
	return 0;
}

static int gc555_video_enum_format(struct file *file, void *priv,
				   struct v4l2_fmtdesc *description)
{
	const struct gc555_video_format_info *format;

	if (description->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    description->index >= ARRAY_SIZE(gc555_video_formats))
		return -EINVAL;

	format = &gc555_video_formats[description->index];
	description->pixelformat = format->fourcc;
	strscpy(description->description, format->description,
		sizeof(description->description));
	return 0;
}

static int gc555_video_get_format(struct file *file, void *priv,
				  struct v4l2_format *format)
{
	struct gc555_video *video = video_drvdata(file);
	struct gc555_video_signal signal = {};
	int ret;

	if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	ret = gc555_video_get_signal(video, &signal);
	if (!ret)
		gc555_video_update_format_locked(video, &signal);
	format->fmt.pix = video->pix;
	if (!ret)
		gc555_video_set_colorimetry(video->format_info, &signal,
					    &format->fmt.pix);
	return 0;
}

static int gc555_video_try_format(struct file *file, void *priv,
				  struct v4l2_format *format)
{
	struct gc555_video *video = video_drvdata(file);
	const struct gc555_video_format_info *format_info;
	struct gc555_video_signal signal = {};
	const struct gc555_video_mode *mode;
	u32 width;
	u32 height;
	int ret;

	if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	format_info = gc555_video_find_format(format->fmt.pix.pixelformat);
	if (!format_info)
		format_info = &gc555_video_formats[0];

	ret = gc555_video_get_signal(video, &signal);
	if (!ret && gc555_video_find_mode(signal.width, signal.height)) {
		if (!gc555_video_supports_signal(format_info, &signal))
			format_info = &gc555_video_formats[0];
		width = signal.width;
		height = signal.height;
	} else {
		mode = gc555_video_find_mode(format->fmt.pix.width,
					     format->fmt.pix.height);
		if (gc555_video_supports_format(mode, format_info)) {
			width = mode->width;
			height = mode->height;
		} else {
			width = GC555_VIDEO_DEFAULT_WIDTH;
			height = GC555_VIDEO_DEFAULT_HEIGHT;
		}
	}

	return gc555_video_fill_pix(format_info, width, height,
				    ret ? NULL : &signal, &format->fmt.pix);
}

static int gc555_video_set_format(struct file *file, void *priv,
				  struct v4l2_format *format)
{
	struct gc555_video *video = video_drvdata(file);
	const struct gc555_video_format_info *format_info;
	struct gc555_video_signal signal = {};
	int ret;

	if (vb2_is_busy(&video->queue))
		return -EBUSY;
	ret = gc555_video_try_format(file, priv, format);
	if (ret)
		return ret;
	format_info = gc555_video_find_format(format->fmt.pix.pixelformat);
	if (!format_info)
		return -EINVAL;

	video->format_info = format_info;
	video->pix = format->fmt.pix;
	if (!gc555_video_get_signal(video, &signal) &&
	    signal.width == video->pix.width &&
	    signal.height == video->pix.height)
		video->frame_rate_hz = signal.frame_rate_hz;
	return 0;
}

static int gc555_video_enum_framesizes(struct file *file, void *priv,
				       struct v4l2_frmsizeenum *size)
{
	const struct gc555_video_format_info *format;
	const struct gc555_video_mode *mode;

	format = gc555_video_find_format(size->pixel_format);
	if (!format)
		return -EINVAL;

	mode = gc555_video_mode_by_index(format, size->index);
	if (!mode)
		return -EINVAL;
	size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	size->discrete.width = mode->width;
	size->discrete.height = mode->height;
	return 0;
}

static int
gc555_video_enum_frameintervals(struct file *file, void *priv,
				struct v4l2_frmivalenum *interval)
{
	const struct gc555_video_format_info *format;
	const struct gc555_video_mode *mode;
	unsigned int i;
	u32 index;
	u32 rate;

	format = gc555_video_find_format(interval->pixel_format);
	if (!format)
		return -EINVAL;
	mode = gc555_video_find_mode(interval->width, interval->height);
	if (!gc555_video_supports_format(mode, format))
		return -EINVAL;

	index = interval->index;
	for (i = 0; i < mode->rate_count; i++) {
		rate = mode->rates[i];
		if (!gc555_video_supports_rate(mode, format, rate))
			continue;
		if (!index--)
			goto found;
	}
	return -EINVAL;

found:
	interval->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	interval->discrete.numerator = 1;
	interval->discrete.denominator = rate;
	return 0;
}

static int gc555_video_enum_input(struct file *file, void *priv,
				  struct v4l2_input *input)
{
	struct gc555_video *video = video_drvdata(file);
	struct gc555_video_signal signal = {};
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	int ret;

	if (input->index)
		return -EINVAL;
	strscpy(input->name, "HDMI", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	input->status = 0;
	if (!gc555 || gc555_dma_device_lost(gc555)) {
		input->status = V4L2_IN_ST_NO_POWER;
		return 0;
	}

	ret = gc555_link_get_video_signal(gc555, &signal);
	if (ret == -ENOLINK)
		input->status = V4L2_IN_ST_NO_SIGNAL;
	else if (ret)
		input->status = V4L2_IN_ST_NO_SYNC;
	return 0;
}

static int gc555_video_get_input(struct file *file, void *priv,
				 unsigned int *input)
{
	*input = 0;
	return 0;
}

static int gc555_video_set_input(struct file *file, void *priv,
				 unsigned int input)
{
	return input ? -EINVAL : 0;
}

static int
gc555_video_signal_to_dv_timings(const struct gc555_video_signal *signal,
				 struct v4l2_dv_timings *timings)
{
	struct v4l2_dv_timings cea = {};
	struct v4l2_bt_timings *bt;
	u64 total_pixels;
	u32 measured_rate;

	memset(timings, 0, sizeof(*timings));
	timings->type = V4L2_DV_BT_656_1120;
	bt = &timings->bt;
	bt->width = signal->width;
	bt->height = signal->height;
	bt->interlaced = signal->interlaced ?
		V4L2_DV_INTERLACED : V4L2_DV_PROGRESSIVE;
	bt->pixelclock = (u64)signal->pixel_clock_khz * 1000U;
	bt->hfrontporch = signal->hfrontporch;
	bt->hsync = signal->hsync;
	bt->hbackporch = signal->hbackporch;
	bt->vfrontporch = signal->vfrontporch;
	bt->vsync = signal->vsync;
	bt->vbackporch = signal->vbackporch;
	if (!signal->interlaced && signal->frame_rate_hz) {
		total_pixels = (u64)(bt->width + bt->hfrontporch + bt->hsync +
				     bt->hbackporch) *
			       (bt->height + bt->vfrontporch + bt->vsync +
				     bt->vbackporch);
		measured_rate = DIV_ROUND_CLOSEST_ULL(bt->pixelclock,
						      total_pixels);
		if (abs_diff(measured_rate, signal->frame_rate_hz) > 1)
			bt->pixelclock = total_pixels * signal->frame_rate_hz;
	}
	if (signal->interlaced) {
		bt->il_vfrontporch = signal->vfrontporch;
		bt->il_vsync = signal->vsync;
		bt->il_vbackporch = signal->vbackporch;
	}
	if (signal->cea861_vic) {
		bt->standards = V4L2_DV_BT_STD_CEA861;
		bt->flags = V4L2_DV_FL_HAS_CEA861_VIC;
		bt->cea861_vic = signal->cea861_vic;
		if (v4l2_find_dv_timings_cea861_vic(&cea,
						    signal->cea861_vic)) {
			bt->polarities = cea.bt.polarities;
			bt->picture_aspect = cea.bt.picture_aspect;
			if (signal->interlaced) {
				bt->il_vfrontporch = cea.bt.il_vfrontporch;
				bt->il_vsync = cea.bt.il_vsync;
				bt->il_vbackporch = cea.bt.il_vbackporch;
			}
		}
	}

	return 0;
}

static int
gc555_video_query_dv_timings(struct file *file, void *priv,
			     struct v4l2_dv_timings *timings)
{
	struct gc555_video *video = video_drvdata(file);
	struct gc555_video_signal signal = {};
	int ret;

	ret = gc555_video_get_signal(video, &signal);
	if (ret)
		return ret;

	return gc555_video_signal_to_dv_timings(&signal, timings);
}

static int
gc555_video_fill_dv_timings_cap(struct v4l2_dv_timings_cap *cap)
{
	memset(cap, 0, sizeof(*cap));
	cap->type = V4L2_DV_BT_656_1120;
	cap->bt.min_width = 320;
	cap->bt.max_width = 3840;
	cap->bt.min_height = 240;
	cap->bt.max_height = 2160;
	cap->bt.min_pixelclock = 13500000ULL;
	cap->bt.max_pixelclock = 600000000ULL;
	cap->bt.standards = V4L2_DV_BT_STD_CEA861 |
			    V4L2_DV_BT_STD_DMT |
			    V4L2_DV_BT_STD_CVT |
			    V4L2_DV_BT_STD_GTF;
	cap->bt.capabilities = V4L2_DV_BT_CAP_INTERLACED |
			       V4L2_DV_BT_CAP_PROGRESSIVE |
			       V4L2_DV_BT_CAP_REDUCED_BLANKING |
			       V4L2_DV_BT_CAP_CUSTOM;

	return 0;
}

static int
gc555_video_dv_timings_cap(struct file *file, void *priv,
			   struct v4l2_dv_timings_cap *cap)
{
	if (cap->pad)
		return -EINVAL;

	return gc555_video_fill_dv_timings_cap(cap);
}

static bool
gc555_video_check_dv_timings(const struct v4l2_dv_timings *timings,
			     void *handle)
{
	const struct gc555_video_mode *mode;
	struct v4l2_fract timeperframe;
	u32 rate;

	mode = gc555_video_find_mode(timings->bt.width, timings->bt.height);
	if (!mode)
		return false;
	timeperframe = v4l2_calc_timeperframe(timings);
	if (!timeperframe.numerator)
		return false;
	rate = DIV_ROUND_CLOSEST(timeperframe.denominator,
				 timeperframe.numerator);

	return gc555_video_mode_supports_rate(mode, rate) ||
	       (rate && gc555_video_mode_supports_rate(mode, rate - 1)) ||
	       gc555_video_mode_supports_rate(mode, rate + 1);
}

static int
gc555_video_enum_dv_timings(struct file *file, void *priv,
			    struct v4l2_enum_dv_timings *timings)
{
	struct v4l2_dv_timings_cap cap;

	if (timings->pad)
		return -EINVAL;
	gc555_video_fill_dv_timings_cap(&cap);

	return v4l2_enum_dv_timings_cap(timings, &cap,
					gc555_video_check_dv_timings, NULL);
}

static int
gc555_video_get_dv_timings(struct file *file, void *priv,
			   struct v4l2_dv_timings *timings)
{
	struct gc555_video *video = video_drvdata(file);
	int ret;

	if (!video->configured_timings_valid) {
		ret = gc555_video_query_dv_timings(file, priv, timings);
		if (ret)
			return ret;
		video->configured_timings = *timings;
		video->configured_timings_valid = true;
	} else {
		*timings = video->configured_timings;
	}

	return 0;
}

static int
gc555_video_set_dv_timings(struct file *file, void *priv,
			   struct v4l2_dv_timings *timings)
{
	struct gc555_video *video = video_drvdata(file);
	struct gc555_video_signal signal = {};
	struct gc555_video_signal configured_signal = {};
	struct v4l2_dv_timings_cap cap;
	struct v4l2_fract timeperframe;
	struct v4l2_pix_format pix;
	const struct gc555_video_signal *format_signal = &configured_signal;
	bool unchanged;
	int ret;

	gc555_video_fill_dv_timings_cap(&cap);
	if (!v4l2_valid_dv_timings(timings, &cap,
				   gc555_video_check_dv_timings, NULL))
		return -ERANGE;
	unchanged = video->configured_timings_valid &&
		v4l2_match_dv_timings(timings, &video->configured_timings,
				      0, false);
	if (vb2_is_busy(&video->queue))
		return unchanged ? 0 : -EBUSY;

	if (!gc555_video_get_signal(video, &signal) &&
	    signal.width == timings->bt.width &&
	    signal.height == timings->bt.height &&
	    signal.interlaced == timings->bt.interlaced)
		format_signal = &signal;
	configured_signal.interlaced = timings->bt.interlaced;
	ret = gc555_video_fill_pix(video->format_info, timings->bt.width,
				   timings->bt.height, format_signal, &pix);
	if (ret)
		return ret;

	timeperframe = v4l2_calc_timeperframe(timings);
	if (!timeperframe.numerator)
		return -ERANGE;

	video->configured_timings = *timings;
	video->configured_timings_valid = true;
	video->configured_timings_explicit = true;
	video->pix = pix;
	video->frame_rate_hz =
		DIV_ROUND_CLOSEST(timeperframe.denominator,
				  timeperframe.numerator);

	return 0;
}

static const char *
gc555_video_sampling_name(enum gc555_video_sampling sampling)
{
	switch (sampling) {
	case GC555_VIDEO_SAMPLING_RGB:
		return "RGB";
	case GC555_VIDEO_SAMPLING_YUV422:
		return "YCbCr 4:2:2";
	case GC555_VIDEO_SAMPLING_YUV444:
		return "YCbCr 4:4:4";
	case GC555_VIDEO_SAMPLING_YUV420:
		return "YCbCr 4:2:0";
	}

	return "unknown";
}

static const char *
gc555_video_encoding_name(enum gc555_video_encoding encoding)
{
	switch (encoding) {
	case GC555_VIDEO_ENCODING_YUV:
		return "YUV";
	case GC555_VIDEO_ENCODING_RGB_FULL:
		return "RGB full";
	case GC555_VIDEO_ENCODING_RGB_LIMITED:
		return "RGB limited";
	}

	return "unknown";
}

static const char *
gc555_video_colorimetry_name(enum gc555_video_colorimetry colorimetry)
{
	switch (colorimetry) {
	case GC555_VIDEO_COLORIMETRY_UNKNOWN:
		return "unknown";
	case GC555_VIDEO_COLORIMETRY_BT601:
		return "BT.601";
	case GC555_VIDEO_COLORIMETRY_BT709:
		return "BT.709";
	case GC555_VIDEO_COLORIMETRY_BT2020:
		return "BT.2020";
	}

	return "unknown";
}

static const char *gc555_video_hdr_name(enum gc555_video_hdr_mode hdr)
{
	switch (hdr) {
	case GC555_VIDEO_HDR_SDR:
		return "SDR";
	case GC555_VIDEO_HDR_PQ:
		return "HDR10 PQ";
	case GC555_VIDEO_HDR_PQ_BT2020:
		return "HDR10 PQ BT.2020";
	case GC555_VIDEO_HDR_HLG:
		return "HLG";
	}

	return "unknown";
}

static int gc555_video_log_status(struct file *file, void *priv)
{
	struct gc555_video *video = video_drvdata(file);
	struct gc555_hdmi_audio_format audio_format = {};
	struct gc555_video_signal signal = {};
	struct gc555_dev *gc555 = READ_ONCE(video->gc555);
	u32 audio_rate_hz;
	int ret;

	if (!gc555)
		return -ENODEV;
	ret = gc555_video_get_signal(video, &signal);
	if (ret) {
		v4l2_info(&video->v4l2_dev, "HDMI signal unavailable: %d\n",
			  ret);
		return 0;
	}

	v4l2_info(&video->v4l2_dev,
		  "HDMI: %ux%u%s%u, pixel clock %u kHz, VIC %u\n",
		  signal.width, signal.height,
		  signal.interlaced ? "i" : "p", signal.frame_rate_hz,
		  signal.pixel_clock_khz, signal.cea861_vic);
	v4l2_info(&video->v4l2_dev,
		  "HDMI format: %s, %s, %s, %s\n",
		  gc555_video_sampling_name(signal.sampling),
		  gc555_video_encoding_name(signal.encoding),
		  gc555_video_colorimetry_name(signal.colorimetry),
		  gc555_video_hdr_name(signal.hdr_mode));
	v4l2_info(&video->v4l2_dev,
		  "HDMI transport: dual-pixel %s, DDR %s\n",
		  signal.dual_pixel ? "on" : "off",
		  signal.ddr ? "on" : "off");
	if (!gc555_it6805_get_audio_format(gc555->it6805, &audio_format))
		v4l2_info(&video->v4l2_dev,
			  "HDMI audio receiver: %u Hz, %u channels\n",
			  audio_format.rate_hz, audio_format.channels);
	else
		v4l2_info(&video->v4l2_dev,
			  "HDMI audio receiver: unavailable\n");
	if (!gc555_bridge_get_audio_rate(gc555, &audio_rate_hz))
		v4l2_info(&video->v4l2_dev, "HDMI audio FPGA: %u Hz\n",
			  audio_rate_hz);
	else
		v4l2_info(&video->v4l2_dev, "HDMI audio FPGA: unavailable\n");

	return 0;
}

static int gc555_video_get_streamparm(struct file *file, void *priv,
				      struct v4l2_streamparm *parm)
{
	struct gc555_video *video = video_drvdata(file);
	struct gc555_video_signal signal = {};
	u32 rate = video->frame_rate_hz;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (!gc555_video_get_signal(video, &signal))
		rate = signal.frame_rate_hz;
	memset(&parm->parm.capture, 0, sizeof(parm->parm.capture));
	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.timeperframe.numerator = 1;
	parm->parm.capture.timeperframe.denominator =
		rate ? rate : GC555_VIDEO_DEFAULT_RATE;
	parm->parm.capture.readbuffers =
		GC555_VIDEO_MIN_QUEUED_BUFFERS + 1;
	return 0;
}

static int gc555_video_set_streamparm(struct file *file, void *priv,
				      struct v4l2_streamparm *parm)
{
	return gc555_video_get_streamparm(file, priv, parm);
}

static int
gc555_video_subscribe_event(struct v4l2_fh *fh,
			    const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_event_subscribe(fh, sub, 4, NULL);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ioctl_ops gc555_video_ioctl_ops = {
	.vidioc_querycap = gc555_video_querycap,
	.vidioc_enum_fmt_vid_cap = gc555_video_enum_format,
	.vidioc_g_fmt_vid_cap = gc555_video_get_format,
	.vidioc_try_fmt_vid_cap = gc555_video_try_format,
	.vidioc_s_fmt_vid_cap = gc555_video_set_format,
	.vidioc_enum_framesizes = gc555_video_enum_framesizes,
	.vidioc_enum_frameintervals = gc555_video_enum_frameintervals,
	.vidioc_enum_input = gc555_video_enum_input,
	.vidioc_g_input = gc555_video_get_input,
	.vidioc_s_input = gc555_video_set_input,
	.vidioc_g_dv_timings = gc555_video_get_dv_timings,
	.vidioc_s_dv_timings = gc555_video_set_dv_timings,
	.vidioc_query_dv_timings = gc555_video_query_dv_timings,
	.vidioc_enum_dv_timings = gc555_video_enum_dv_timings,
	.vidioc_dv_timings_cap = gc555_video_dv_timings_cap,
	.vidioc_log_status = gc555_video_log_status,
	.vidioc_g_parm = gc555_video_get_streamparm,
	.vidioc_s_parm = gc555_video_set_streamparm,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_expbuf = vb2_ioctl_expbuf,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	.vidioc_remove_bufs = vb2_ioctl_remove_bufs,
#endif
	.vidioc_subscribe_event = gc555_video_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations gc555_video_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
};

static void gc555_video_final_release(struct v4l2_device *v4l2_dev)
{
	struct gc555_video *video =
		container_of(v4l2_dev, struct gc555_video, v4l2_dev);

	v4l2_ctrl_handler_free(&video->ctrl_handler);
	v4l2_device_unregister(v4l2_dev);
	kfree(video);
}

int gc555_video_init(struct gc555_dev *gc555)
{
	struct gc555_video_signal signal = {};
	struct v4l2_ctrl *power_present;
	struct gc555_video *video;
	int ret;

	if (!gc555 || !gc555->video_dma)
		return -ENODEV;
	if (gc555->video)
		return 0;

	video = kzalloc(sizeof(*video), GFP_KERNEL);
	if (!video)
		return -ENOMEM;
	video->gc555 = gc555;
	video->format_info = &gc555_video_formats[0];
	video->frame_rate_hz = GC555_VIDEO_DEFAULT_RATE;
	mutex_init(&video->lock);
	spin_lock_init(&video->queue_lock);
	INIT_LIST_HEAD(&video->owned_buffers);
	INIT_DELAYED_WORK(&video->monitor_work, gc555_video_monitor_work);
	atomic64_set(&video->last_published_ns, 0);

	ret = v4l2_device_register(gc555->dev, &video->v4l2_dev);
	if (ret)
		goto free_video;
	video->v4l2_dev.release = gc555_video_final_release;

	v4l2_ctrl_handler_init(&video->ctrl_handler, 6);
	v4l2_ctrl_new_std(&video->ctrl_handler, &gc555_video_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, 0, 0x3ff, 1, 0x200);
	v4l2_ctrl_new_std(&video->ctrl_handler, &gc555_video_ctrl_ops,
			  V4L2_CID_CONTRAST, 0, 0x1ff, 1, 0x100);
	v4l2_ctrl_new_std(&video->ctrl_handler, &gc555_video_ctrl_ops,
			  V4L2_CID_HUE, 0, 360, 1, 0);
	v4l2_ctrl_new_std(&video->ctrl_handler, &gc555_video_ctrl_ops,
			  V4L2_CID_SATURATION, 0, 0x1ff, 1, 0x80);
	power_present =
		v4l2_ctrl_new_std(&video->ctrl_handler, &gc555_video_ctrl_ops,
				  V4L2_CID_DV_RX_POWER_PRESENT, 0, 1, 0, 0);
	if (power_present)
		power_present->flags |= V4L2_CTRL_FLAG_READ_ONLY |
					V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_custom(&video->ctrl_handler, &gc555_ctrl_hdr_mode, NULL);
	ret = video->ctrl_handler.error;
	if (ret)
		goto put_v4l2_device;

	video->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	video->queue.io_modes = VB2_READ | VB2_MMAP | VB2_DMABUF;
	video->queue.dev = gc555->dev;
	video->queue.dma_dir = DMA_FROM_DEVICE;
	video->queue.lock = &video->lock;
	video->queue.ops = &gc555_video_queue_ops;
	video->queue.mem_ops = &vb2_dma_sg_memops;
	video->queue.drv_priv = video;
	video->queue.buf_struct_size = sizeof(struct gc555_video_buffer);
	video->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
		V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	video->queue.gfp_flags = GFP_DMA32;
	video->queue.min_queued_buffers = GC555_VIDEO_MIN_QUEUED_BUFFERS;
	ret = vb2_queue_init(&video->queue);
	if (ret)
		goto put_v4l2_device;

	if (!gc555_link_get_video_signal(gc555, &signal) &&
	    gc555_video_find_mode(signal.width, signal.height)) {
		video->signal_valid = true;
		video->cached_signal = signal;
		video->frame_rate_hz = signal.frame_rate_hz;
		gc555_video_signal_to_dv_timings(&signal,
						 &video->configured_timings);
		video->configured_timings_valid = true;
		ret = gc555_video_fill_pix(video->format_info, signal.width,
					   signal.height, &signal,
					   &video->pix);
	} else {
		ret = gc555_video_fill_pix(video->format_info,
					   GC555_VIDEO_DEFAULT_WIDTH,
					   GC555_VIDEO_DEFAULT_HEIGHT,
					   NULL, &video->pix);
	}
	if (ret)
		goto release_queue;

	if (gc555->pdev->subsystem_device == 0x5730)
		strscpy(video->vdev.name, "Live Gamer 4K (open_gc573)", sizeof(video->vdev.name));
	else
		strscpy(video->vdev.name, "Live Gamer BOLT (open_gc555)", sizeof(video->vdev.name));
	video->vdev.v4l2_dev = &video->v4l2_dev;
	video->vdev.ctrl_handler = &video->ctrl_handler;
	video->vdev.fops = &gc555_video_fops;
	video->vdev.ioctl_ops = &gc555_video_ioctl_ops;
	video->vdev.release = video_device_release_empty;
	video->vdev.lock = &video->lock;
	video->vdev.queue = &video->queue;
	video->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE |
		V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	video->vdev.vfl_dir = VFL_DIR_RX;
	video_set_drvdata(&video->vdev, video);

	ret = gc555_video_dma_set_error_handler(gc555, gc555_video_dma_error, video);
	if (ret)
		goto release_queue;
	ret = video_register_device(&video->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto clear_error_handler;
	gc555->video = video;
	schedule_delayed_work(&video->monitor_work,
			      msecs_to_jiffies(GC555_VIDEO_POLL_MS));
	dev_info(gc555->dev, "registered %s\n",
		 video_device_node_name(&video->vdev));
	return 0;

clear_error_handler:
	gc555_video_dma_set_error_handler(gc555, NULL, NULL);
release_queue:
	vb2_queue_release(&video->queue);
put_v4l2_device:
	v4l2_device_put(&video->v4l2_dev);
	return ret;

free_video:
	kfree(video);
	return ret;
}

void gc555_video_cleanup(struct gc555_dev *gc555)
{
	struct gc555_video *video;

	if (!gc555 || !gc555->video)
		return;

	video = gc555->video;
	gc555->video = NULL;
	WRITE_ONCE(video->detaching, true);
	cancel_delayed_work_sync(&video->monitor_work);
	v4l2_device_disconnect(&video->v4l2_dev);
	vb2_video_unregister_device(&video->vdev);
	gc555_video_dma_set_error_handler(gc555, NULL, NULL);
	/* Open handles may outlive removal and must lose hardware access. */
	WRITE_ONCE(video->gc555, NULL);
	v4l2_device_put(&video->v4l2_dev);
}

void gc555_video_suspend(struct gc555_dev *gc555)
{
	struct gc555_video *video;
	unsigned long flags;
	bool streaming;

	if (!gc555 || !gc555->video)
		return;

	video = gc555->video;
	cancel_delayed_work_sync(&video->monitor_work);

	mutex_lock(&video->lock);
	streaming = vb2_is_streaming(&video->queue);
	if (streaming)
		vb2_queue_error(&video->queue);
	spin_lock_irqsave(&video->queue_lock, flags);
	video->starting = false;
	video->streaming = false;
	video->signal_valid = false;
	spin_unlock_irqrestore(&video->queue_lock, flags);
	atomic64_set(&video->last_published_ns, 0);
	if (streaming) {
		gc555_video_dma_stop(gc555);
		gc555_video_return_buffers(video, VB2_BUF_STATE_ERROR);
	}
	mutex_unlock(&video->lock);
}

void gc555_video_resume(struct gc555_dev *gc555)
{
	if (!gc555 || !gc555->video)
		return;

	schedule_delayed_work(&gc555->video->monitor_work,
			      msecs_to_jiffies(GC555_VIDEO_POLL_MS));
}

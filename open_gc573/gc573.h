/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * open_gc573 - Native Linux driver for AVerMedia Live Gamer 4K GC573 & BOLT GC555
 *
 * Derived from the gc555 driver authored by ostrich (https://github.com/ostrich/gc555).
 * Adapted, extended for GC573 PCIe, and maintained by Everlite and contributors (2026).
 *
 * Licensed under the GNU General Public License version 2 (GPL-2.0-only).
 */
#ifndef GC555_H
#define GC555_H

#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define GC555_PCI_VENDOR_ID		0x1461
#define GC555_PCI_DEVICE_ID		0x0054
#define GC555_PCI_SUBVENDOR_ID		0x1461
#define GC555_PCI_SUBDEVICE_ID		0x5550
#define GC573_PCI_SUBDEVICE_ID		0x5730

struct device;
struct i2c_adapter;
struct pci_dev;
struct sg_table;
struct gc555_audio;
struct gc555_dma;
struct gc555_fpga;
struct gc555_i2c;
struct gc555_it6664;
struct gc555_it6805;
struct gc555_led;
struct gc555_video;
struct gc555_video_dma;

#define GC555_AUDIO_RATE_32000_HZ	32000U
#define GC555_AUDIO_RATE_44100_HZ	44100U
#define GC555_AUDIO_RATE_48000_HZ	48000U
#define GC555_AUDIO_CHANNELS_STEREO	2U
#define GC555_AUDIO_CHANNELS_7_1		8U

enum gc555_hdmi_audio_transport {
	GC555_HDMI_AUDIO_SAMPLES,
	GC555_HDMI_AUDIO_ONE_BIT,
	GC555_HDMI_AUDIO_MULTISTREAM,
	GC555_HDMI_AUDIO_3D,
};

struct gc555_hdmi_audio_format {
	u32 generation;
	u32 rate_hz;
	u8 channels;
	u8 channel_allocation;
	enum gc555_hdmi_audio_transport transport;
	bool valid;
};

typedef void (*gc555_audio_data_t)(void *context, const void *data,
				   size_t bytes);

typedef void (*gc555_dma_video_irq_t)(void *context, u32 status,
				      u32 channel_status);
typedef void (*gc555_video_dma_error_t)(void *context);

enum gc555_video_dma_completion {
	GC555_VIDEO_DMA_RELEASE,
	GC555_VIDEO_DMA_RECYCLE,
};

enum gc555_i2c_bus_id {
	GC555_I2C_BUS_PRIMARY,
	GC555_I2C_BUS_SECONDARY,
	GC555_I2C_BUS_TERTIARY,
	GC555_I2C_BUS_COUNT,
};

struct gc555_bridge {
	void __iomem *regs;
	resource_size_t regs_size;
	/* Serializes GPIO register read-modify-write operations. */
	struct mutex gpio_lock;
	bool ready;
	bool host_irq_routing_enabled;
};

struct gc555_dev {
	struct device *dev;
	struct pci_dev *pdev;
	struct gc555_bridge bridge;
	struct gc555_dma *dma;
	struct gc555_video_dma *video_dma;
	struct gc555_video *video;
	struct gc555_audio *audio;
	struct gc555_fpga *fpga;
	struct gc555_i2c *i2c;
	struct gc555_it6664 *it6664;
	struct gc555_it6805 *it6805;
	struct gc555_led *led;
};

struct gc555_edid_caps {
	u32 max_tmds_clock_khz;
	bool hdmi;
	bool scdc;
	bool low_rate_scrambling;
	bool supports_1080p;
	bool supports_4k30;
	bool supports_4k60;
	bool supports_ycbcr420_4k60;
	bool requires_ycbcr420_4k60;
	bool deep_color_30;
	bool deep_color_36;
	bool deep_color_ycbcr444;
	bool deep_color_ycbcr420_30;
	bool deep_color_ycbcr420_36;
};

struct gc555_bridge_frame_info {
	u32 status;
	u32 width;
	u32 height;
	bool interlaced;
};

enum gc555_video_input_class {
	GC555_VIDEO_INPUT_SD,
	GC555_VIDEO_INPUT_HD,
	GC555_VIDEO_INPUT_UHD,
};

enum gc555_video_encoding {
	GC555_VIDEO_ENCODING_YUV,
	GC555_VIDEO_ENCODING_RGB_FULL,
	GC555_VIDEO_ENCODING_RGB_LIMITED,
};

enum gc555_video_sampling {
	GC555_VIDEO_SAMPLING_RGB,
	GC555_VIDEO_SAMPLING_YUV422,
	GC555_VIDEO_SAMPLING_YUV444,
	GC555_VIDEO_SAMPLING_YUV420,
};

enum gc555_video_colorimetry {
	GC555_VIDEO_COLORIMETRY_UNKNOWN,
	GC555_VIDEO_COLORIMETRY_BT601,
	GC555_VIDEO_COLORIMETRY_BT709,
	GC555_VIDEO_COLORIMETRY_BT2020,
};

enum gc555_video_hdr_mode {
	GC555_VIDEO_HDR_SDR,
	GC555_VIDEO_HDR_PQ,
	GC555_VIDEO_HDR_PQ_BT2020,
	GC555_VIDEO_HDR_HLG,
};

enum gc555_hdcp_level {
	GC555_HDCP_NONE,
	GC555_HDCP_1X,
	GC555_HDCP_2X,
};

enum gc555_video_format {
	GC555_VIDEO_FORMAT_YUYV,
	GC555_VIDEO_FORMAT_NV12,
	GC555_VIDEO_FORMAT_P010,
	GC555_VIDEO_FORMAT_BGR24,
	GC555_VIDEO_FORMAT_RGB32,
};

enum gc555_fpga_color_control {
	GC555_FPGA_COLOR_BRIGHTNESS,
	GC555_FPGA_COLOR_CONTRAST,
	GC555_FPGA_COLOR_HUE,
	GC555_FPGA_COLOR_SATURATION,
};

struct gc555_video_signal {
	u32 width;
	u32 height;
	u32 pixel_clock_khz;
	u32 frame_rate_hz;
	u32 frame_rate_x100;
	u16 hfrontporch;
	u16 hsync;
	u16 hbackporch;
	u16 vfrontporch;
	u16 vsync;
	u16 vbackporch;
	u8 cea861_vic;
	enum gc555_video_input_class input_class;
	enum gc555_video_encoding encoding;
	enum gc555_video_sampling sampling;
	enum gc555_video_colorimetry colorimetry;
	enum gc555_video_hdr_mode hdr_mode;
	bool interlaced;
	bool dual_pixel;
	bool ddr;
};

int gc555_bridge_init(struct gc555_dev *gc555, void __iomem *regs,
		      resource_size_t regs_size);
void gc555_bridge_cleanup(struct gc555_dev *gc555);
bool gc555_bridge_is_accessible(struct gc555_dev *gc555);
void gc555_bridge_mark_disconnected(struct gc555_dev *gc555);
void gc555_bridge_suspend(struct gc555_dev *gc555);
int gc555_bridge_resume(struct gc555_dev *gc555);
int gc555_bridge_resume_complete(struct gc555_dev *gc555);
int gc555_bridge_restore_host_irq_routing(struct gc555_dev *gc555);
int gc555_bridge_set_host_irq_routing(struct gc555_dev *gc555, bool enable);
bool gc555_bridge_host_irq_routing_enabled(struct gc555_dev *gc555);
bool gc555_bridge_is_ready(struct gc555_dev *gc555);
int gc555_bridge_read(struct gc555_dev *gc555, u32 offset, u32 *value);
int gc555_bridge_read8(struct gc555_dev *gc555, u32 offset, u8 *value);
int gc555_bridge_write(struct gc555_dev *gc555, u32 offset, u32 value);
int gc555_bridge_get_frame_info(struct gc555_dev *gc555,
				struct gc555_bridge_frame_info *frame_info);
int gc555_bridge_get_audio_rate(struct gc555_dev *gc555, u32 *rate_hz);
int gc555_bridge_set_gpio(struct gc555_dev *gc555, unsigned int pin,
			  bool high, unsigned int delay_ms);
int gc555_bridge_get_gpio(struct gc555_dev *gc555, unsigned int pin,
			  bool *high);

int gc555_dma_init(struct gc555_dev *gc555);
void gc555_dma_cleanup(struct gc555_dev *gc555);
int gc555_dma_start_audio(struct gc555_dev *gc555, unsigned int rate_hz,
			  unsigned int channels, unsigned int sample_bits,
			  gc555_audio_data_t data,
			  void *data_context);
void gc555_dma_stop_audio(struct gc555_dev *gc555, void *data_context);
int gc555_dma_start_line_audio(struct gc555_dev *gc555,
			       gc555_audio_data_t data, void *data_context);
void gc555_dma_stop_line_audio(struct gc555_dev *gc555, void *data_context);
int gc555_dma_read_register(struct gc555_dev *gc555, u32 offset, u32 *value);
int gc555_dma_write_register(struct gc555_dev *gc555, u32 offset, u32 value);
int gc555_dma_update_register_bits(struct gc555_dev *gc555, u32 offset,
				   u32 mask, u32 value);
int gc555_dma_reset_video(struct gc555_dev *gc555);
bool gc555_dma_device_lost(struct gc555_dev *gc555);
void gc555_dma_mark_device_lost(struct gc555_dev *gc555);
int gc555_dma_register_video_irq(struct gc555_dev *gc555,
				 gc555_dma_video_irq_t handler,
				 void *context);
void gc555_dma_unregister_video_irq(struct gc555_dev *gc555, void *context);
void gc555_dma_synchronize_irq(struct gc555_dev *gc555);

int gc555_video_dma_init(struct gc555_dev *gc555);
void gc555_video_dma_cleanup(struct gc555_dev *gc555);
int gc555_video_dma_set_error_handler(struct gc555_dev *gc555,
				      gc555_video_dma_error_t handler,
				      void *context);
int gc555_video_dma_prepare(struct gc555_dev *gc555, void *buffer,
			    struct sg_table *sgt,
			    enum gc555_video_format format,
			    size_t luma_size, size_t chroma_size);
int gc555_video_dma_queue(struct gc555_dev *gc555, void *buffer,
			  enum gc555_video_dma_completion
			  (*complete)(void *buffer, u64 completion_ns,
				      void *context),
			  void *complete_context);
int gc555_video_dma_cleanup_buffer(struct gc555_dev *gc555, void *buffer);
int gc555_video_dma_reset(struct gc555_dev *gc555);
int gc555_video_dma_start(struct gc555_dev *gc555);
void gc555_video_dma_stop(struct gc555_dev *gc555);

int gc555_video_init(struct gc555_dev *gc555);
void gc555_video_cleanup(struct gc555_dev *gc555);
void gc555_video_suspend(struct gc555_dev *gc555);
void gc555_video_resume(struct gc555_dev *gc555);

int gc555_audio_init(struct gc555_dev *gc555);
void gc555_audio_cleanup(struct gc555_dev *gc555);
void gc555_audio_suspend(struct gc555_dev *gc555);
void gc555_audio_resume(struct gc555_dev *gc555);
void gc555_audio_hdmi_format_changed(struct gc555_dev *gc555);

int gc555_fpga_init(struct gc555_dev *gc555);
void gc555_fpga_cleanup(struct gc555_dev *gc555);
int gc555_fpga_validate_video(const struct gc555_video_signal *input,
			      enum gc555_video_format output_format,
			       u32 output_width, u32 output_height,
			       u32 output_frame_rate_hz);
int gc555_fpga_configure(struct gc555_dev *gc555,
			 const struct gc555_video_signal *input,
			 enum gc555_video_format output_format,
			 u32 output_width, u32 output_height,
			 u32 output_frame_rate_hz);
int gc555_fpga_set_output_enabled(struct gc555_dev *gc555, bool enabled);
int gc555_fpga_set_color_control(struct gc555_dev *gc555,
				 enum gc555_fpga_color_control control,
				 u32 value);
int gc555_fpga_update_color_path(struct gc555_dev *gc555,
				 const struct gc555_video_signal *input,
				 enum gc555_video_format output_format,
				 u32 output_width, u32 output_height,
				 u32 output_frame_rate_hz);
int gc555_fpga_reset_vip(struct gc555_dev *gc555);
int gc555_fpga_set_frame_rate(struct gc555_dev *gc555, u32 frame_rate_hz,
			      u32 frame_rate_x100);

int gc555_link_init(struct gc555_dev *gc555);
int gc555_link_get_video_signal(struct gc555_dev *gc555,
				struct gc555_video_signal *signal);
int gc555_link_get_input_power(struct gc555_dev *gc555, bool *present);
int gc555_link_get_input_edid(struct gc555_dev *gc555, u8 *edid, size_t size);
int gc555_link_get_source_hdcp(struct gc555_dev *gc555,
			      enum gc555_hdcp_level *level);
int gc555_link_get_input_hpd(struct gc555_dev *gc555, bool *high);
int gc555_link_set_input_hpd(struct gc555_dev *gc555, bool high);
int gc555_link_set_splitter_scdt(struct gc555_dev *gc555, bool high);
int gc555_link_set_tx_hpd_gate(struct gc555_dev *gc555, unsigned int port,
			       bool high);
int gc555_link_tx_is_hdmi(struct gc555_dev *gc555, unsigned int port,
			  bool *is_hdmi);

int gc555_edid_get(const u8 **edid, size_t *size);
int gc555_edid_parse_caps(const u8 *edid, size_t size,
			  struct gc555_edid_caps *caps);
int gc555_edid_merge(const u8 *sink, size_t sink_size,
		     u8 *merged, size_t merged_size);

int gc555_i2c_init(struct gc555_dev *gc555);
void gc555_i2c_cleanup(struct gc555_dev *gc555);
u32 gc555_i2c_irq(struct gc555_dev *gc555, u32 irq_status);
struct i2c_adapter *
gc555_i2c_get_adapter(struct gc555_dev *gc555,
		      enum gc555_i2c_bus_id bus);

#ifdef GC555_HAS_LED_CLASS
int gc555_led_init(struct gc555_dev *gc555);
void gc555_led_cleanup(struct gc555_dev *gc555);
void gc555_led_suspend(struct gc555_dev *gc555);
int gc555_led_resume(struct gc555_dev *gc555);
#else
static inline int gc555_led_init(struct gc555_dev *gc555)
{
	return 0;
}

static inline void gc555_led_cleanup(struct gc555_dev *gc555)
{
}

static inline void gc555_led_suspend(struct gc555_dev *gc555)
{
}

static inline int gc555_led_resume(struct gc555_dev *gc555)
{
	return 0;
}
#endif

int gc555_it6664_init(struct gc555_dev *gc555);
void gc555_it6664_cleanup(struct gc555_dev *gc555);
void gc555_it6664_suspend(struct gc555_dev *gc555);
int gc555_it6664_resume(struct gc555_dev *gc555);

int gc555_it6805_init(struct gc555_dev *gc555);
void gc555_it6805_cleanup(struct gc555_dev *gc555);
void gc555_it6805_suspend(struct gc555_dev *gc555);
int gc555_it6805_resume(struct gc555_dev *gc555);
int gc555_it6805_get_video_signal(struct gc555_it6805 *it6805,
				  struct gc555_video_signal *signal);
int gc555_it6805_get_input_power(struct gc555_it6805 *it6805, bool *present);
int gc555_it6805_get_audio_format(struct gc555_it6805 *it6805,
				  struct gc555_hdmi_audio_format *format);

#endif

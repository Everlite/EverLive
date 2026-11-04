// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared GC570D driver declarations: register definitions, device state,
 * subsystem interfaces, constants, and debugfs operations.
 */
#ifndef GC570D_H
#define GC570D_H

#ifdef GC570D_USERSPACE
#include "gc570d-user-compat.h"
#else
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/kthread.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>
#include <sound/control.h>
#endif
#include "gc570d-config.h"

/* DEFINE_SHOW_ATTRIBUTE keeps its fops static; probe assembles ours centrally. */
#ifdef GC570D_USERSPACE
#define GC570D_DEFINE_SHOW_ATTRIBUTE(__name)                         \
static int __name ## _open(struct inode *inode, struct file *file)   \
{                                                                    \
	return single_open(file, __name ## _show, inode->i_private);    \
}                                                                    \
const struct file_operations __name ## _fops = {                    \
	.owner   = THIS_MODULE,                                         \
	.open    = __name ## _open,                                    \
	.read    = seq_read,                                            \
	.llseek  = seq_lseek,                                           \
	.release = single_release,                                      \
	.show_fn = __name ## _show,                                     \
}
#else
#define GC570D_DEFINE_SHOW_ATTRIBUTE(__name)                         \
static int __name ## _open(struct inode *inode, struct file *file)   \
{                                                                    \
	return single_open(file, __name ## _show, inode->i_private);    \
}                                                                    \
const struct file_operations __name ## _fops = {                    \
	.owner   = THIS_MODULE,                                         \
	.open    = __name ## _open,                                    \
	.read    = seq_read,                                            \
	.llseek  = seq_lseek,                                           \
	.release = single_release,                                      \
}
#endif

#define GC570D_VENDOR_ID          0x1461
#define GC570D_DEVICE_ID          0x0054
#define GC570D_SUBSYSTEM_VENDOR_ID 0x1461
#define GC570D_SUBSYSTEM_ID       0x5700

extern bool auto_hdmi1;
extern bool copy_edid;
#define GC570D_BAR                0
#define GC570D_EXPECTED_BAR_SIZE  SZ_512K

#define GC570D_REG_IRQ_STATUS     0x0010
#define GC570D_REG_DMA_START      0x0008
#define GC570D_REG_RESET_STATUS   0x000c
#define GC570D_REG_IRQ_MASK       0x001c
#define GC570D_REG_IRQ_CONTROL    0x0034
#define GC570D_REG_IRQ_MODE       0x0038
#define GC570D_REG_XILINX_RESET   0x0040
#define GC570D_IRQ_XILINX_INTC    BIT(10)
#define GC570D_IRQ_I2C0           BIT(11)
#define GC570D_IRQ_I2C1           BIT(12)
#define GC570D_IRQ_I2C2           BIT(13)
#define GC570D_IRQ_I2C3           BIT(14)
#define GC570D_REG_I2C2_BASE      0x0120
#define GC570D_REG_I2C0_BASE      0x0180
#define GC570D_REG_I2C1_BASE      0x01c0
#define GC570D_REG_I2C3_CLOCK     0x0150
#define GC570D_REG_I2C3_SLAVE     0x0154
#define GC570D_REG_I2C3_ADDR_LEN  0x0158
#define GC570D_REG_I2C3_WIDTH     0x015c
#define GC570D_REG_I2C3_ADDRESS   0x0160
#define GC570D_REG_I2C3_COUNT     0x0164
#define GC570D_REG_I2C3_WRITE_FIFO 0x0168
#define GC570D_REG_I2C3_READ_FIFO 0x016c
#define GC570D_REG_I2C3_COMMAND   0x0170
#define GC570D_REG_I2C3_STATUS    0x0174
#define GC570D_RST_RECEIVER_A     5
#define GC570D_RST_RECEIVER_B     6

#define GC570D_I2C_XILINX_ADDR    0x4e
#define GC570D_I2C_RECEIVER_ADDR8 0x90
#define GC570D_I2C_EDID_ADDR8     0xa8
#define GC570D_I2C_LED_ADDR8      0xe8
#define GC570D_I2C_READ_COMMAND   0x08
#define GC570D_I2C_WRITE_COMMAND  0x04
#define GC570D_I2C_CLEAR_COMMAND  0x10
#define GC570D_I2C_WRITE_OK       BIT(0)
#define GC570D_I2C_READ_OK        BIT(2)
#define GC570D_I2C3_CLOCK_VALUE   0x007d
#define GC570D_I2C_RX_CLOCK_VALUE 0x0138

#define GC570D_LED_NAME            "gc570d:rgb:status"
#define GC570D_LED_MAX_BRIGHTNESS  0xff
#define GC570D_LED_WINDOWS_BLUE    0xb2
#define GC570D_LED_BREATH_STEPS    50
#define GC570D_LED_BREATH_DELAY_MS 20

#define GC570D_DMA_CHANNEL        1
#define GC570D_DMA_FRAME_SIZE     (1920 * 1080 * 2)
#define GC570D_DMA_BURST_FRAMES   4
#define GC570D_DMA_STRESS_FRAMES  120
#define GC570D_WIDTH              1920
#define GC570D_HEIGHT             1080
#define GC570D_DMA_IMAGE_SIZE     0x021c0000
#define GC570D_DMA_SG_SIZE        0x00022000
#define GC570D_DMA_DESC_INDEX     0x0b00
#define GC570D_DMA_DESC_CONTROL   0x0b04
#define GC570D_DMA_DESC_ADDRESS   0x0b08
#define GC570D_DMA_FRAME_IRQ      BIT(3)
#define GC570D_DMA0_DESC_INDEX    0x0300
#define GC570D_DMA0_DESC_CONTROL  0x0304
#define GC570D_DMA0_DESC_ADDRESS  0x0308
#define GC570D_DMA0_FRAME_IRQ     BIT(1)
#define GC570D_DMA_STREAM_SG_FLAGS 0x80008000
#define GC570D_VIDEO_TERM_IRQS    (BIT(0) | BIT(2))
#define GC570D_DMA_SENTINEL       0xa5
#define GC570D_NO_SIGNAL_WIDTH    640
#define GC570D_NO_SIGNAL_HEIGHT   360
#define GC570D_NO_SIGNAL_INTERVAL 166666

#define GC570D_AUDIO_CHANNEL      1
#define GC570D_AUDIO_FORMAT       0x0280
#define GC570D_AUDIO_GLOBAL_FORMAT 0x0270
#define GC570D_AUDIO_BUFFER0      0x0288
#define GC570D_AUDIO_BUFFER1      0x0290
#define GC570D_AUDIO_RATE         0x029c
#define GC570D_AUDIO_START        GC570D_REG_DMA_START
#define GC570D_AUDIO_START_BIT    BIT(3)
#define GC570D_AUDIO0_TERM_IRQ    BIT(4)
#define GC570D_AUDIO0_IRQ         BIT(5)
#define GC570D_AUDIO_TERM_IRQ     BIT(8)
#define GC570D_AUDIO_TERM_IRQS    (GC570D_AUDIO0_TERM_IRQ | \
				    GC570D_AUDIO_TERM_IRQ)
#define GC570D_AUDIO_IRQ          BIT(9)
#define GC570D_AUDIO_BUFFER_SIZE  0x0000c000
#define GC570D_AUDIO_STAGING_SIZE 0x0000b400
#define GC570D_AUDIO_PERIOD_BYTES 0x00000780
#define GC570D_AUDIO_RECORD_PERIODS 1000
#define GC570D_AUDIO_SENTINEL     0x5a
#define GC570D_AUDIO_RATE_HZ      48000
#define GC570D_AUDIO_CHANNELS     2
#define GC570D_AUDIO_PERIOD_FRAMES 480
#define GC570D_AUDIO_PCM_BUFFER_MAX (GC570D_AUDIO_PERIOD_BYTES * 64)

#define GC570D_AUDIO0_CHANNEL       0
#define GC570D_AUDIO0_FORMAT        0x0200
#define GC570D_AUDIO0_BUFFER0       0x0208
#define GC570D_AUDIO0_BUFFER1       0x0210
#define GC570D_AUDIO0_RATE          0x021c
#define GC570D_AUDIO0_START_BIT     BIT(1)

#define GC570D_VIP_GLOBAL_FORMAT  0x0004
#define GC570D_VIP_GLOBAL_ENABLE  0x1040
#define GC570D_VIP_AUX_0          0x105c
#define GC570D_VIP_AUX_1          0x1060
#define GC570D_VIP_AUX_2          0x1064
#define GC570D_VIP_AUX_3          0x1068
#define GC570D_VIP_BYPASS         0x1084
#define GC570D_VIP_GLOBAL_START   0x108c
#define GC570D_VIP_COLOR_0        0x1090
#define GC570D_VIP_COLOR_1        0x1094
#define GC570D_VIP_COLOR_2        0x1098
#define GC570D_VIP_COLOR_3        0x109c
#define GC570D_VIP_CHANNEL_CTRL   0x1400
#define GC570D_VIP_CHANNEL_CLOCK  0x14a4
#define GC570D_VIP_SCALER_CTRL    0x1600
#define GC570D_VIP_CROP_WIDTH     0x1620
#define GC570D_VIP_OUTPUT_WIDTH   0x1624
#define GC570D_VIP_CROP_HEIGHT    0x1628
#define GC570D_VIP_OUTPUT_HEIGHT  0x162c
#define GC570D_VIP_SCALE_X        0x1700
#define GC570D_VIP_SCALE_Y        0x1704
#define GC570D_VIP_INPUT_SIZE     0x1708
#define GC570D_VIP_INPUT_X_LAST   0x170c
#define GC570D_VIP_INPUT_Y_LAST   0x1710
#define GC570D_VIP_OUTPUT_SIZE    0x1714
#define GC570D_VIP_SCALER_PHASE   0x1718
#define GC570D_VIP_SCALER_FLAGS   0x171c

#define GC570D_VIP0_CHANNEL_CTRL   0x1000
#define GC570D_VIP0_EVENT_STATUS   0x1004
#define GC570D_VIP0_ACTIVE_STATUS  0x100c
#define GC570D_VIP0_PERIOD_STATUS  0x1010
#define GC570D_VIP0_WINDOW_WIDTH   0x1020
#define GC570D_VIP0_OUTPUT_WIDTH   0x1024
#define GC570D_VIP0_WINDOW_HEIGHT  0x1028
#define GC570D_VIP0_OUTPUT_HEIGHT  0x102c
#define GC570D_VIP0_SCALER_CTRL    0x1200
#define GC570D_VIP0_SCALE_X        0x1300
#define GC570D_VIP0_SCALE_Y        0x1304
#define GC570D_VIP0_INPUT_SIZE     0x1308
#define GC570D_VIP0_INPUT_X_LAST   0x130c
#define GC570D_VIP0_INPUT_Y_LAST   0x1310
#define GC570D_VIP0_OUTPUT_SIZE    0x1314
#define GC570D_VIP0_SCALER_PHASE   0x1318
#define GC570D_VIP0_SCALER_FLAGS   0x131c

#define GC570D_XILINX_HDR_RGB_FULL_TO_1080_MODE 0x00000305
#define GC570D_XILINX_SDR_RGB_FULL_TO_1080_MODE 0x00000310
#define GC570D_XILINX_60HZ_CLOCK   2475000
#define GC570D_XILINX_50HZ_CLOCK   2970000

#define GC570D_I2C_CLOCK          0x00
#define GC570D_I2C_SLAVE          0x04
#define GC570D_I2C_ADDR_LEN       0x08
#define GC570D_I2C_WIDTH          0x0c
#define GC570D_I2C_ADDRESS        0x10
#define GC570D_I2C_COUNT          0x14
#define GC570D_I2C_WRITE_FIFO     0x18
#define GC570D_I2C_READ_FIFO      0x1c
#define GC570D_I2C_COMMAND        0x20
#define GC570D_I2C_STATUS         0x24

struct gc570d_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

enum gc570d_video0_profile {
	GC570D_VIDEO0_HDR_BT2020,
	GC570D_VIDEO0_SDR_RGB,
	GC570D_VIDEO0_SDR_YUV420_BT709,
};

struct gc570d_video0_signal {
	enum gc570d_video0_profile profile;
	u16 detected_width;
	u16 detected_height;
	u16 vip_width;
	u16 vip_height;
	u16 frame_rate;
	u8 working_mode;
	u8 xilinx_mode;
};

struct gc570d_video0_format {
	u16 width;
	u16 height;
	u32 frame_interval;
};

struct gc570d_splitter_rx_timing {
	u32 counter_sum;
	u32 pixel_clock;
	u32 tmds_clock;
	u16 htotal;
	u16 hactive;
	u16 vtotal;
	u16 vactive;
	u16 hfront;
	u16 hsync;
	u16 vfront;
	u16 vsync;
	u16 frame_rate;
	u8 depth_code;
	u8 flags;
};

struct gc570d_i2c_bus {
	u32 base;
	u32 irq;
	u8 expected_device;
	const char *receiver;
};

struct gc570d_dev {
	struct pci_dev *pdev;
	void __iomem *bar0;
	resource_size_t bar0_len;
	struct dentry *debugfs_dir;
	struct mutex i2c_lock;
	/*
	 * Lock Hierarchy (outer to inner):
	 *   1. capture_lock   (protects splitter state machine & hardware streaming switches)
	 *   2. video_lock / video0_lock (protects V4L2 device state per channel)
	 *   3. i2c_lock       (serializes hardware I2C bus transactions)
	 *   4. led_lock       (protects LED breathing/RGB register writes)
	 * Never acquire capture_lock while holding video_lock, video0_lock, or i2c_lock.
	 */
	struct mutex capture_lock;
	struct completion dma_completion;
	struct completion dma0_completion;
	u32 dma_irq_status;
	u32 dma0_irq_status;
	atomic64_t irq_total;
	atomic64_t irq_dma;
	atomic64_t irq_dma0;
	atomic64_t irq_dma1;
	atomic64_t irq_audio;
	atomic64_t irq_audio_term;
	atomic64_t irq_audio0;
	atomic64_t irq_audio0_term;
	atomic64_t irq_other;
	atomic_t irq_other_status;
	atomic_t control_irq_seen;
	atomic_t audio_irq_pending;
	atomic_t audio0_irq_pending;
	int irq;
	struct v4l2_device v4l2_dev;
	struct video_device video_dev;
	struct vb2_queue video_queue;
	struct mutex video_lock;
	spinlock_t video_qlock;
	struct list_head video_buffers;
	wait_queue_head_t video_wait;
	struct task_struct *video_thread;
	__le32 *video_sg_cpu;
	dma_addr_t video_sg_dma;
	u32 video_sequence;
	u16 video_source_width;
	u16 video_source_height;
	u16 video_source_fps;
	u8 video_source_profile;
	bool video_streaming;
	bool video_stopping;
	bool video_no_signal;
	bool video_dma_active;
	bool video_irq_active;
	u8 *video_no_signal_frame;
	struct video_device video0_dev;
	struct vb2_queue video0_queue;
	struct mutex video0_lock;
	spinlock_t video0_qlock;
	struct list_head video0_buffers;
	wait_queue_head_t video0_wait;
	struct task_struct *video0_thread;
	__le32 *video0_sg_cpu[GC570D_DMA_BURST_FRAMES];
	dma_addr_t video0_sg_dma[GC570D_DMA_BURST_FRAMES];
	u8 *video0_dma_cpu[GC570D_DMA_BURST_FRAMES];
	dma_addr_t video0_dma_addr[GC570D_DMA_BURST_FRAMES];
	size_t video0_dma_size;
	u8 video0_events[16];
	u8 video0_event_head;
	u8 video0_event_tail;
	bool video0_event_overflow;
	u32 video0_sequence;
	u16 video0_width;
	u16 video0_height;
	u16 video0_source_width;
	u16 video0_source_height;
	u16 video0_source_fps;
	u8 video0_source_profile;
	u32 video0_frame_interval;
	bool video0_receiver_output_valid;
	u16 video0_receiver_width;
	u16 video0_receiver_height;
	u8 video0_receiver_input_color;
	u8 video0_receiver_colorimetry;
	u8 video0_receiver_extended_colorimetry;
	bool video0_receiver_dual_pixel;
	bool video0_streaming;
	bool video0_stopping;
	bool video0_no_signal;
	bool video0_dma_active;
	bool video0_irq_active;
	u8 *video0_no_signal_frame;
	bool dma0_continuous;
	u8 *capture_data;
	size_t capture_size;
	u32 capture_irq;
	u32 capture_index;
	u32 capture_control;
	size_t capture_changed;
	u64 capture_elapsed_us;
	int capture_error;
	u8 *audio_capture_data;
	size_t audio_capture_size;
	size_t audio_capture_changed;
	u32 audio_capture_irq;
	u32 audio_capture_index;
	u32 audio_capture_control;
	u32 audio_capture_requested;
	u32 audio_capture_completed;
	u64 audio_capture_elapsed_us;
	int audio_capture_mute_error;
	int audio_capture_error;
	u8 *audio0_capture_data;
	size_t audio0_capture_size;
	size_t audio0_capture_changed;
	u32 audio0_capture_irq;
	u32 audio0_capture_index;
	u32 audio0_capture_control;
	u32 audio0_capture_requested;
	u32 audio0_capture_completed;
	u64 audio0_capture_elapsed_us;
	int audio0_capture_error;
	wait_queue_head_t audio0_wait;
	struct snd_card *audio_card_hdmi1;
	struct snd_card *audio_card_hdmi2;
	struct snd_jack *hdmi1_jack;
	struct snd_jack *hdmi2_jack;
	struct snd_pcm *audio_pcm;
	struct snd_pcm *audio0_pcm;
	struct snd_pcm_substream *audio0_substream;
	struct task_struct *audio0_thread;
	void *audio0_pcm_buffer_cpu[2];
	dma_addr_t audio0_pcm_buffer_dma[2];
	size_t audio0_pcm_pos;
	u32 audio0_pcm_cursor;
	u8 audio0_channels;
	u16 audio0_rate;
	bool audio0_prepared;
	bool audio0_running;
	bool audio0_recovering;
	bool audio0_irq_active;
	bool audio0_stop_pending;
	u64 audio0_term_baseline;
	int audio0_thread_error;
	struct snd_pcm_substream *audio_substream;
	struct task_struct *audio_thread;
	wait_queue_head_t audio_wait;
	spinlock_t audio_pcm_lock;
	void *audio_pcm_buffer_cpu[2];
	dma_addr_t audio_pcm_buffer_dma[2];
	size_t audio_pcm_pos;
	u32 audio_pcm_cursor;
	u8 audio_channels;
	u16 audio_rate;
	bool audio_prepared;
	bool audio_running;
	bool audio_recovering;
	bool audio_irq_active;
	bool audio_stop_pending;
	u64 audio_term_baseline;
	int audio_thread_error;
	unsigned int video_irq_users;
	u32 splitter_rclk;
	bool splitter_main_timer_serviced;
	bool splitter_video_stable_pending;
	u8 splitter_channel_active_mask;
	u8 splitter_video_stable_mask;
	u8 splitter_worker_completed_mask;
	u8 splitter_worker_state4_mask;
	u8 splitter_eq_state;
	u8 splitter_eq_retry;
	u32 splitter_pclk[4];
	u32 splitter_vclk[4];
	struct task_struct *splitter_pump_thread;
	bool splitter_receiver_hdcp_off;
	u8 splitter_pump_last_main05;
	u32 splitter_pump_cycles;
	u32 splitter_pump_events;
	int splitter_pump_last_error;
	struct delayed_work hdmi1_auto_work;
	u8 hdmi1_auto_phase;
	bool hdmi1_auto_ready;
	bool splitter_edid_published;
	bool removing;
	bool suspended;
	struct led_classdev_mc led_mcdev;
	struct mc_subled led_subleds[3];
	struct mutex led_lock;
	struct delayed_work led_work;
	struct work_struct resume_work;
	u8 led_step;
	bool led_descending;
	bool led_breathing;
	bool led_colorful;
	u8 led_palette_index;
	bool led_initialized;
	bool led_registered;
	bool led_effect_created;
	bool led_removing;
	bool led_active;
};

int gc570d_audio_register(struct gc570d_dev *gc);
int gc570d_audio_recover_locked(struct gc570d_dev *gc);
int gc570d_audio0_recover_locked(struct gc570d_dev *gc);
int gc570d_i2c_read8(struct gc570d_dev *gc,
			    const struct gc570d_i2c_bus *bus, u8 slave_addr8,
			    u8 address, u8 *value, u32 *last_irq,
			    u32 *last_status);
int gc570d_i2c_read_buf(struct gc570d_dev *gc,
			       const struct gc570d_i2c_bus *bus, u8 slave_addr8,
			       u8 address, u8 *values, u8 count,
			       u32 *last_irq, u32 *last_status);
int gc570d_i2c_write8(struct gc570d_dev *gc,
			     const struct gc570d_i2c_bus *bus, u8 slave_addr8,
			     u8 address, u8 value);
int gc570d_i2c_write_buf(struct gc570d_dev *gc,
				const struct gc570d_i2c_bus *bus,
				u8 slave_addr8, u8 address,
				const u8 *values, u8 count);
void gc570d_control_irq_finish(struct gc570d_dev *gc, u32 irq);
void gc570d_control_irq_prepare(struct gc570d_dev *gc, u32 irq);
int gc570d_control_irq_wait(struct gc570d_dev *gc, u32 irq,
				    u32 *observed_irq);
irqreturn_t gc570d_irq_handler(int irq, void *data);
int gc570d_it6802_audio_output_set(struct gc570d_dev *gc, bool enable);
int gc570d_it6802_format_init(struct gc570d_dev *gc);
int gc570d_it6802_init(struct gc570d_dev *gc);
int gc570d_it6802_link_status(struct gc570d_dev *gc, bool *source_5v,
				       bool *scdt);
int gc570d_it6802_output_enable(struct gc570d_dev *gc);
int gc570d_it6802_program_edid(struct gc570d_dev *gc);
int gc570d_it6802_pulse_hpd(struct gc570d_dev *gc);
int gc570d_it68051_apply_base_table(struct gc570d_dev *gc);
int gc570d_it68051_audio_output_set(struct gc570d_dev *gc,
						    bool enable);
int gc570d_it68051_calibrate(struct gc570d_dev *gc);
int gc570d_it68051_program_edid(struct gc570d_dev *gc);
int gc570d_it68051_pulse_hpd(struct gc570d_dev *gc);
int gc570d_it68051_select_page(struct gc570d_dev *gc, u8 page);
int gc570d_it68051_timing_init(struct gc570d_dev *gc);
int gc570d_it68051_video_output_on(struct gc570d_dev *gc);
void gc570d_led_breath_work(struct work_struct *work);
int gc570d_led_register(struct gc570d_dev *gc);
void gc570d_led_unregister(struct gc570d_dev *gc);
void gc570d_led_suspend(struct gc570d_dev *gc);
int gc570d_led_resume(struct gc570d_dev *gc);
void gc570d_reset_resume_state(struct gc570d_dev *gc);
int gc570d_receiver_read8(struct gc570d_dev *gc,
				 const struct gc570d_i2c_bus *bus,
				 u8 address, u8 *value, u32 *last_irq,
				 u32 *last_status);
int gc570d_receiver_update8(struct gc570d_dev *gc,
				   const struct gc570d_i2c_bus *bus,
				   u8 address, u8 mask, u8 value);
int gc570d_receiver_write8(struct gc570d_dev *gc,
				  const struct gc570d_i2c_bus *bus,
				  u8 address, u8 value);
/*
 * True if any V4L2 or ALSA stream currently owns the HDMI receiver,
 * splitter, or the shared DMA/INTx resources.  Used by debugfs write
 * handlers that would reprogram the receiver or splitter out from under
 * an open stream.  Conservative on purpose: any active stream blocks
 * the write, because reprogramming the splitter or IT68051 can disturb
 * the shared I2C0 bus and clock tree even when only the other input's
 * capture path is running.
 */
bool gc570d_subsystem_busy(struct gc570d_dev *gc);
void gc570d_reset_audio_channel(struct gc570d_dev *gc,
					unsigned int channel);
void gc570d_reset_channel(struct gc570d_dev *gc, unsigned int channel);
void gc570d_reset_video_channel(struct gc570d_dev *gc,
					unsigned int channel);
void gc570d_set_reset_bit(struct gc570d_dev *gc, unsigned int bit,
			  bool asserted);
int gc570d_splitter_aux_enable_pulse(struct gc570d_dev *gc);
int gc570d_splitter_aux_ports_init(struct gc570d_dev *gc);
int gc570d_splitter_channel_video_stable_irq(struct gc570d_dev *gc);
int gc570d_splitter_clock_init(struct gc570d_dev *gc);
int gc570d_splitter_core_preamble(struct gc570d_dev *gc);
int gc570d_splitter_irq_init(struct gc570d_dev *gc);
int gc570d_splitter_main_timer_irq(struct gc570d_dev *gc);
int gc570d_splitter_measure_rx_timing(
	struct gc570d_dev *gc, u8 input_color,
	struct gc570d_splitter_rx_timing *timing);
int gc570d_splitter_output_followup(struct gc570d_dev *gc);
int gc570d_splitter_output_mode_init(struct gc570d_dev *gc);
int gc570d_splitter_output_preamble(struct gc570d_dev *gc);
int gc570d_splitter_post_reset_init(struct gc570d_dev *gc);
int gc570d_splitter_route_init(struct gc570d_dev *gc);
int gc570d_splitter_source_power_event(struct gc570d_dev *gc,
					      bool quiet_idle);
int gc570d_splitter_stable_worker_probe(struct gc570d_dev *gc);
int
gc570d_splitter_stable_workers_windows_order(struct gc570d_dev *gc);
int
gc570d_splitter_windows_channel2_connect(struct gc570d_dev *gc,
					  bool bridge_default);
int gc570d_splitter_windows_receiver_hdcp_off(struct gc570d_dev *gc);
int gc570d_splitter_windows_state_pump_start(struct gc570d_dev *gc);
void gc570d_splitter_windows_state_pump_stop(struct gc570d_dev *gc);
int gc570d_video0_register(struct gc570d_dev *gc);
int gc570d_video0_switch_to_no_signal(struct gc570d_dev *gc);
void gc570d_video_irq_get_locked(struct gc570d_dev *gc);
void gc570d_video_irq_put_locked(struct gc570d_dev *gc);
int gc570d_video_register(struct gc570d_dev *gc);
int gc570d_video_switch_to_no_signal(struct gc570d_dev *gc);
int gc570d_vip0_detect_signal(
	struct gc570d_dev *gc, struct gc570d_video0_signal *signal);
int gc570d_vip0_program_yuy2(
	struct gc570d_dev *gc, const struct gc570d_video0_signal *signal,
	u16 output_width, u16 output_height, u32 frame_interval);
int gc570d_vip_init(struct gc570d_dev *gc);
int gc570d_xilinx_load_hdr_lut_3000(struct gc570d_dev *gc);
int gc570d_xilinx_prepare_video0(
	struct gc570d_dev *gc, const struct gc570d_video0_signal *signal,
	u16 output_width, u16 output_height);
int gc570d_xilinx_read32(struct gc570d_dev *gc, u32 address,
				u32 *value, u8 raw[4], u32 *last_irq,
				u32 *last_status);
int gc570d_xilinx_write32(struct gc570d_dev *gc, u32 address,
				  u32 value);

extern const struct file_operations gc570d_audio0_dma_frame_fops;
extern const struct file_operations gc570d_audio0_dma_record_fops;
extern const struct file_operations gc570d_audio0_dma_status_fops;
extern const struct file_operations gc570d_audio0_pcm_status_fops;
extern const struct file_operations gc570d_audio_dma_capture_fops;
extern const struct file_operations gc570d_audio_dma_frame_fops;
extern const struct file_operations gc570d_audio_dma_period_fops;
extern const struct file_operations gc570d_audio_dma_record_fops;
extern const struct file_operations gc570d_audio_dma_status_fops;
extern const struct file_operations gc570d_audio_pcm_status_fops;
extern const struct file_operations gc570d_dma0_burst_fops;
extern const struct file_operations gc570d_dma0_capture_fops;
extern const struct file_operations gc570d_dma_burst_fops;
extern const struct file_operations gc570d_dma_capture_fops;
extern const struct file_operations gc570d_dma_frame_fops;
extern const struct file_operations gc570d_dma_status_fops;
extern const struct file_operations gc570d_dma_stress_fops;
extern const struct file_operations gc570d_irq_stats_fops;
extern const struct file_operations gc570d_it6802_audio_output_enable_fops;
extern const struct file_operations gc570d_it6802_audio_status_fops;
extern const struct file_operations gc570d_it6802_edid_fops;
extern const struct file_operations gc570d_it6802_edid_status_fops;
extern const struct file_operations gc570d_it6802_format_init_fops;
extern const struct file_operations gc570d_it6802_hpd_fops;
extern const struct file_operations gc570d_it6802_init_fops;
extern const struct file_operations gc570d_it6802_output_enable_fops;
extern const struct file_operations gc570d_it6802_video_format_fops;
extern const struct file_operations gc570d_it68051_audio_status_fops;
extern const struct file_operations gc570d_it68051_calibrate_fops;
extern const struct file_operations gc570d_it68051_edid_fops;
extern const struct file_operations gc570d_it68051_edid_status_fops;
extern const struct file_operations gc570d_it68051_hpd_fops;
extern const struct file_operations gc570d_it68051_init_fops;
extern const struct file_operations gc570d_it68051_status_fops;
extern const struct file_operations gc570d_it68051_timing_init_fops;
extern const struct file_operations gc570d_it68051_video_output_on_fops;
extern const struct file_operations gc570d_led_basic_fops;
extern const struct file_operations gc570d_preprocess_enable_fops;
extern const struct file_operations gc570d_receiver_registers_fops;
extern const struct file_operations gc570d_registers_fops;
extern const struct file_operations gc570d_splitter_aux_enable_pulse_fops;
extern const struct file_operations gc570d_splitter_aux_ports_init_fops;
extern const struct file_operations gc570d_splitter_aux_status_fops;
extern const struct file_operations gc570d_splitter_channel1_irq_on_fops;
extern const struct file_operations gc570d_splitter_channel_irq_on_fops;
extern const struct file_operations gc570d_splitter_channel_video_stable_irq_fops;
extern const struct file_operations gc570d_splitter_clock_init_fops;
extern const struct file_operations gc570d_splitter_core_preamble_fops;
extern const struct file_operations gc570d_splitter_edid_read_fops;
extern const struct file_operations gc570d_splitter_eq_start_fops;
extern const struct file_operations gc570d_splitter_hpd_high_fops;
extern const struct file_operations gc570d_splitter_irq_init_fops;
extern const struct file_operations gc570d_splitter_main_timer_irq_fops;
extern const struct file_operations gc570d_splitter_output_followup_fops;
extern const struct file_operations gc570d_splitter_output_mode_init_fops;
extern const struct file_operations gc570d_splitter_output_preamble_fops;
extern const struct file_operations gc570d_splitter_post_reset_init_fops;
extern const struct file_operations gc570d_splitter_probe_fops;
extern const struct file_operations gc570d_splitter_pump_status_fops;
extern const struct file_operations gc570d_splitter_route_init_fops;
extern const struct file_operations gc570d_splitter_source_power_event_fops;
extern const struct file_operations gc570d_splitter_stable_worker_link_setup_fops;
extern const struct file_operations gc570d_splitter_stable_worker_output_setup_fops;
extern const struct file_operations gc570d_splitter_stable_worker_port1_link_setup_fops;
extern const struct file_operations gc570d_splitter_stable_worker_port1_output_setup_fops;
extern const struct file_operations gc570d_splitter_stable_worker_probe_fops;
extern const struct file_operations gc570d_splitter_stable_workers_windows_order_fops;
extern const struct file_operations gc570d_splitter_status_fops;
extern const struct file_operations gc570d_splitter_windows_bridge_edid_fops;
extern const struct file_operations gc570d_splitter_windows_channel2_bridge_connect_fops;
extern const struct file_operations gc570d_splitter_windows_channel2_copy_connect_fops;
extern const struct file_operations gc570d_splitter_windows_edid_copy_fops;
extern const struct file_operations gc570d_splitter_windows_receiver_hdcp_off_fops;
extern const struct file_operations gc570d_splitter_windows_state_pump_fops;
extern const struct file_operations gc570d_vip0_init_fops;
extern const struct file_operations gc570d_vip0_profile_fops;
extern const struct file_operations gc570d_vip0_registers_fops;
extern const struct file_operations gc570d_vip_init_fops;
extern const struct file_operations gc570d_vip_registers_fops;
extern const struct file_operations gc570d_xilinx_registers_fops;

extern const struct gc570d_i2c_bus gc570d_receiver_buses[2];
extern const struct gc570d_i2c_bus gc570d_splitter_bus;
extern const struct gc570d_i2c_bus gc570d_led_bus;
extern const struct gc570d_i2c_bus gc570d_probe_buses[3];
extern const u8 gc570d_it68051_edid[256];
void gc570d_splitter_build_bridge_edid(u8 *edid);
void gc570d_splitter_merge_cta_data_blocks(u8 *dst_cta, const u8 *src_cta);


#endif /* GC570D_H */
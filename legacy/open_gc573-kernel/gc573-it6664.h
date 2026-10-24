/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC555_IT6664_H
#define GC555_IT6664_H

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "gc573.h"

#define IT6664_ID_LENGTH		4
#define IT6664_TX_PORT_COUNT	4
#define IT6664_TX_IRQ_COUNT	5
#define IT6664_EDID_BLOCK_SIZE	128
#define IT6664_EDID_MAX_BLOCKS	4
#define IT6664_EDID_SIZE		(2 * IT6664_EDID_BLOCK_SIZE)
#define IT6664_DRM_INFOFRAME_CAPTURE_SIZE	19
#define IT6664_RX_EQ_LANE_COUNT	3
#define IT6664_RX_EQ20_SNAPSHOT_SIZE	9
#define IT6664_RX_EQ20_CANDIDATE_COUNT	14
#define IT6664_RX_EQ20_TUNING_SIZE	3

struct i2c_client;
struct gc555_dev;

enum it6664_tx_video_state {
	IT6664_TX_VIDEO_WAIT_IRQ,
	IT6664_TX_VIDEO_STABLE,
	IT6664_TX_VIDEO_STABLE_OFF,
	IT6664_TX_VIDEO_RESET,
	IT6664_TX_VIDEO_OK,
};

enum it6664_tx_hdcp_state {
	IT6664_TX_HDCP_WAIT_IRQ,
	IT6664_TX_HDCP_RESET,
	IT6664_TX_HDCP_GOING,
	IT6664_TX_HDCP_DONE,
	IT6664_TX_HDCP_REAUTH,
	IT6664_TX_HDCP_CHECK,
	IT6664_TX_HDCP_RETRY,
};

enum it6664_tx_source {
	IT6664_TX_SOURCE_DIRECT,
	IT6664_TX_SOURCE_CSC,
	IT6664_TX_SOURCE_CONVERTER,
	IT6664_TX_SOURCE_SCALER,
	IT6664_TX_SOURCE_NONE,
};

enum it6664_rx_eq_state {
	IT6664_RX_EQ_IDLE,
	IT6664_RX_EQ_RESET,
	IT6664_RX_EQ_START,
	IT6664_RX_EQ_READ_RESULT,
	IT6664_RX_EQ_VALIDATE,
	IT6664_RX_EQ_MONITOR,
};

enum it6664_rx_eq20_path {
	IT6664_RX_EQ20_NONE,
	IT6664_RX_EQ20_INVALID,
	IT6664_RX_EQ20_RESTORE_SNAPSHOT,
	IT6664_RX_EQ20_SCORE_AMP,
};

enum it6664_rx_timer_state {
	IT6664_RX_TIMER_IDLE = 0,
	IT6664_RX_TIMER_ARMED = 3,
	IT6664_RX_TIMER_TX_OFF = 5,
};

enum it6664_rx_colorspace {
	IT6664_RX_COLORSPACE_RGB,
	IT6664_RX_COLORSPACE_YCBCR422,
	IT6664_RX_COLORSPACE_YCBCR444,
	IT6664_RX_COLORSPACE_YCBCR420,
};

enum it6664_upstream_edid {
	IT6664_UPSTREAM_EDID_NONE,
	IT6664_UPSTREAM_EDID_FIXED,
	IT6664_UPSTREAM_EDID_MERGED,
};

enum it6664_map_id {
	IT6664_MAP_SWITCH,
	IT6664_MAP_RX_PORT0,
	IT6664_MAP_RX_EDID_RAM,
	IT6664_MAP_TX_COMMON,
	IT6664_MAP_TX_PORT0,
	IT6664_MAP_TX_PORT1,
	IT6664_MAP_TX_PORT2,
	IT6664_MAP_TX_PORT3,
	IT6664_MAP_COUNT,
};

struct it6664_map {
	struct i2c_client *client;
	struct regmap *regmap;
};

struct it6664_tx_irq {
	u8 status;
	u8 irq[IT6664_TX_IRQ_COUNT];
};

struct it6664_tx_irq_pending {
	struct it6664_tx_irq snapshot;
	u64 consumed;
	u8 acknowledged;
	u8 ddc_hang_stage;
	bool ddc_hang_restore;
	bool valid;
};

struct it6664_tx_port_state {
	enum it6664_tx_video_state video_state;
	enum it6664_tx_hdcp_state hdcp_state;
	enum it6664_tx_source source;
	enum gc555_hdcp_level source_hdcp_level;
	struct gc555_edid_caps sink_caps;
	u32 pclk_khz;
	u16 hdcp_status_count;
	u8 scdc_version;
	u8 scdc_config;
	u8 scdc_attempts;
	u8 edid_sessions;
	u8 hdcp_fire_version;
	u8 hdcp_wait_count;
	u8 hdcp_fire_count;
	u8 deferred_irq[IT6664_TX_IRQ_COUNT];
	bool powered;
	bool hpd;
	bool rx_sense;
	bool edid_attempted;
	bool edid_parsed;
	bool dvi_mode;
	bool afe_configured;
	bool high_bandwidth;
	bool scrambling_required;
	bool video_stable;
	bool tmds_stable;
	bool scdc_configured;
	bool hdcp_going;
	bool hdcp_done;
	bool hdcp2_done;
	bool hdcp2_rsa_busy;
	bool force_hdcp1;
};

struct it6664_rx_eq20_state {
	enum it6664_rx_eq20_path path;
	u16 invalid_mask[IT6664_RX_EQ_LANE_COUNT];
	u8 seed[IT6664_RX_EQ_LANE_COUNT];
	u8 snapshot[IT6664_RX_EQ20_SNAPSHOT_SIZE];
	u8 readback[IT6664_RX_EQ20_SNAPSHOT_SIZE];
	u8 tuning[IT6664_RX_EQ20_CANDIDATE_COUNT]
		 [IT6664_RX_EQ_LANE_COUNT][IT6664_RX_EQ20_TUNING_SIZE];
	u8 skew_status;
};

struct it6664_rx_video_timing {
	u32 pixel_clock_khz;
	u32 adjusted_pixel_clock_khz;
	u32 frame_rate_hz;
	u16 htotal;
	u16 vtotal;
	u16 hactive;
	u16 vactive;
	bool is_4k30;
	bool high_frame_rate;
	bool valid;
};

struct it6664_rx_state {
	enum it6664_rx_eq_state eq_state;
	enum it6664_rx_timer_state timer_state;
	enum it6664_rx_colorspace colorspace;
	struct it6664_rx_eq20_state eq20;
	struct it6664_rx_video_timing video_timing;
	u8 eq_fixed[IT6664_RX_EQ_LANE_COUNT];
	u8 bus_mode;
	u8 converter_output_mode_request;
	u8 converter_output_mode;
	u8 csc_output_mode;
	u8 csc_output_quantization;
	u8 color_depth;
	u8 eq14_retry_count;
	u8 eq_postcheck_delay;
	u8 eq_validate_not_ready;
	u8 eq_validate_recoveries;
	u8 eq_monitor_not_ready;
	u8 eq_monitor_recoveries;
	u8 eq_manual_lane2_index;
	u8 eq_manual_lane2_runs;
	u8 eq_terminal_reg14;
	u8 source_hdcp_none_count;
	u8 source_hdcp_1x_count;
	u8 source_hdcp_2x_count;
	u8 source_hdcp_content_type;
	enum gc555_hdcp_level source_hdcp_level;
	enum gc555_hdcp_level source_hdcp_raw_level;
	enum gc555_hdcp_level source_hdcp_effective_level;
	int source_hdcp_last_error;
	bool eq_lane_failed[IT6664_RX_EQ_LANE_COUNT];
	bool signal_started;
	bool irq12_handled;
	bool hdcp_enabled;
	bool source_hdcp_valid;
	bool source_hdcp_content_type_valid;
	bool eq14_done;
	bool eq14_running;
	bool eq20_done;
	bool eq20_running;
	bool mode_rearm_pending;
	bool eq_terminal_sampled;
	bool eq_terminal_valid;
	bool eq_recovery_needed;
	bool scdt;
	bool clock_configured;
};

struct it6664_sink_edid {
	u8 data[IT6664_EDID_MAX_BLOCKS * IT6664_EDID_BLOCK_SIZE];
	unsigned int length;
	u8 cta_block;
	bool valid;
};

struct it6664_switch_irq {
	u8 irq6;
	u8 irq7;
	u8 irq21;
	u8 irq22;
	u8 irq23;
};

struct it6664_switch_irq_pending {
	struct it6664_switch_irq snapshot;
	u16 consumed;
	u8 acknowledged;
	u8 timer0_stage;
	u8 timer1_stage;
	bool valid;
};

struct it6664_runtime {
	struct workqueue_struct *wq;
	struct delayed_work work;
	struct it6664_rx_state rx;
	struct it6664_tx_port_state tx[IT6664_TX_PORT_COUNT];
	struct it6664_tx_irq_pending tx_irq[IT6664_TX_PORT_COUNT];
	struct it6664_switch_irq_pending switch_irq;
	struct it6664_sink_edid sink_edid;
	u8 merged_edid[IT6664_EDID_SIZE];
	enum it6664_upstream_edid upstream_edid;
	u8 edid_publish_sessions;
	bool merge_attempted;
	bool merged_edid_pending;
	u8 tx_hpd_mask;
	atomic_t enabled;
};

struct gc555_it6664 {
	struct gc555_dev *gc555;
	struct it6664_map maps[IT6664_MAP_COUNT];
	struct it6664_runtime runtime;
	/* Protects the exact EDID image last published to the HDMI source. */
	struct mutex input_edid_lock;
	u8 input_edid[IT6664_EDID_SIZE];
	bool input_edid_valid;
	u8 identity[IT6664_ID_LENGTH];
	u32 siprom_raw;
	u32 rclk_khz;
};

static inline int it6664_write_bits(struct regmap *map, unsigned int reg,
				    unsigned int mask, unsigned int value)
{
	return regmap_write_bits(map, reg, mask, value);
}

int gc555_it6664_rx_set_hpd(struct gc555_it6664 *it6664, bool high);
int gc555_it6664_rx_refresh_video_timing(struct gc555_it6664 *it6664);
int gc555_it6664_tx_init(struct gc555_it6664 *it6664);
int gc555_it6664_tx_poll(struct gc555_it6664 *it6664);
int gc555_it6664_tx_read_sink_edid(struct gc555_it6664 *it6664);
int gc555_it6664_tx_read_mode(struct gc555_it6664 *it6664,
			      unsigned int port, u8 *status, u8 *mode);
int
gc555_it6664_tx_reset_signal(struct gc555_it6664 *it6664, bool *active);
int gc555_it6664_tx_power_connected_ports(struct gc555_it6664 *it6664);
int gc555_it6664_tx_power_down_all(struct gc555_it6664 *it6664);
int gc555_it6664_get_source_hdcp(struct gc555_it6664 *it6664,
				 enum gc555_hdcp_level *level);
int gc555_it6664_get_input_edid(struct gc555_it6664 *it6664, u8 *edid,
				size_t size);

#endif

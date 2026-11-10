/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_card.h — Core Hardware Abstraction Layer for EverLive Capture Cards
 *
 * Provides a polymorphic, modular interface for discovering, initializing,
 * and managing PCIe and USB capture cards (GC573, GC570D, and future hardware).
 */

#ifndef EVERLIVE_CARD_H
#define EVERLIVE_CARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Hardware Capability Bitflags
 * ------------------------------------------------------------------------- */
#define EVERLIVE_CAP_VIDEO_CAPTURE       (1U << 0)
#define EVERLIVE_CAP_AUDIO_CAPTURE       (1U << 1)
#define EVERLIVE_CAP_PASSTHROUGH         (1U << 2)
#define EVERLIVE_CAP_1080P60             (1U << 3)
#define EVERLIVE_CAP_4K60                (1U << 4)
#define EVERLIVE_CAP_1440P120            (1U << 5)
#define EVERLIVE_CAP_1080P240            (1U << 6)
#define EVERLIVE_CAP_HDR10               (1U << 7)
#define EVERLIVE_CAP_DUAL_INPUT          (1U << 8)
#define EVERLIVE_CAP_RGB_LED             (1U << 9)
#define EVERLIVE_CAP_BITSTREAM_AUDIO     (1U << 10)
#define EVERLIVE_CAP_FRAMEGEN            (1U << 11)
#define EVERLIVE_CAP_HARDWARE_SCALER     (1U << 12)
#define EVERLIVE_CAP_HARDWARE_SPLITTER   (1U << 13)
#define EVERLIVE_CAP_EDID_PASSTHROUGH    (1U << 14)
#define EVERLIVE_CAP_4K120               (1U << 15)

/* -------------------------------------------------------------------------
 * Lighting & Visual Feedback Modes
 * ------------------------------------------------------------------------- */
enum everlive_led_mode {
	EVERLIVE_LED_MODE_OFF = 0,
	EVERLIVE_LED_MODE_STATIC,
	EVERLIVE_LED_MODE_BREATHING,
	EVERLIVE_LED_MODE_RAINBOW,
	EVERLIVE_LED_MODE_WAVE,
};

/* -------------------------------------------------------------------------
 * Pixel & Audio Formats
 * ------------------------------------------------------------------------- */
enum everlive_pixel_format {
	EVERLIVE_PIX_FMT_UNKNOWN = 0,
	EVERLIVE_PIX_FMT_YUYV,
	EVERLIVE_PIX_FMT_NV12,
	EVERLIVE_PIX_FMT_P010,
	EVERLIVE_PIX_FMT_RGB24,
	EVERLIVE_PIX_FMT_BGR24,
	EVERLIVE_PIX_FMT_RGBA32,
};

struct everlive_video_timing {
	uint32_t width;
	uint32_t height;
	double refresh_rate;
	bool interlaced;
	bool hdr10;
	enum everlive_pixel_format format;
};

struct everlive_audio_timing {
	uint32_t sample_rate;       /* e.g. 48000 */
	uint8_t channels;           /* e.g. 2, 6, 8 */
	uint8_t bit_depth;          /* e.g. 16, 24 */
	bool is_compressed;         /* true for AC-3 / DTS IEC 61937 */
	char codec_name[16];        /* e.g. "LPCM", "AC-3", "DTS" */
};

/* -------------------------------------------------------------------------
 * Live Telemetry & Diagnostics
 * ------------------------------------------------------------------------- */
struct everlive_telemetry {
	bool signal_locked;
	int active_input_port;
	struct everlive_video_timing video;
	struct everlive_audio_timing audio;
	double pci_bandwidth_mbps;
	uint64_t frames_captured;
	uint64_t frames_dropped;
	float temperature_c;        /* Negative value indicates unsupported */
};

/* Forward declarations */
struct everlive_device;
struct everlive_card_ops;

/* -------------------------------------------------------------------------
 * Declarative Device Descriptor (Hardware Registry Entry)
 * ------------------------------------------------------------------------- */
struct everlive_device_desc {
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t subsystem_id;      /* 0 = wildcard match */
	const char *model_slug;     /* Short identifier, e.g. "gc573", "gc570d" */
	const char *name;           /* Friendly name, e.g. "AVerMedia Live Gamer 4K" */
	uint32_t capabilities;
	int num_input_ports;
	const struct everlive_card_ops *ops;
	bool fallback_match;        /* Last-resort match for this vendor/device */
};

/* -------------------------------------------------------------------------
 * Polymorphic Hardware Operations Interface
 * ------------------------------------------------------------------------- */
struct everlive_card_ops {
	/* Probe and identify the hardware at the given PCI/USB location */
	int (*probe)(struct everlive_device *dev);

	/* Initialize hardware registers, clock trees, and I2C buses */
	int (*init_hardware)(struct everlive_device *dev);

	/* Enter low-power standby or hand off to background daemon */
	int (*standby)(struct everlive_device *dev);

	/* Release hardware resources and close descriptors */
	void (*release)(struct everlive_device *dev);

	/* Select active HDMI input port (0-indexed) */
	int (*set_input_port)(struct everlive_device *dev, int port);

	/* Start DMA video and audio streaming pipelines */
	int (*start_capture)(struct everlive_device *dev);

	/* Stop streaming while keeping hardware awake */
	int (*stop_capture)(struct everlive_device *dev);

	/* Configure or bridge HDMI EDID */
	int (*set_edid_profile)(struct everlive_device *dev, int port,
				const char *profile_name,
				const uint8_t *custom_bin, size_t bin_len);

	/* Enable or disable zero-latency HDMI passthrough */
	int (*enable_passthrough)(struct everlive_device *dev, bool enable);

	/* Update RGB LED illumination */
	int (*set_led)(struct everlive_device *dev, enum everlive_led_mode mode,
		       uint8_t r, uint8_t g, uint8_t b);

	/* Retrieve real-time telemetry (signal lock, timings, stats) */
	int (*get_telemetry)(struct everlive_device *dev,
			     struct everlive_telemetry *out_telemetry);
};

/* -------------------------------------------------------------------------
 * Active Device Instance Handle
 * ------------------------------------------------------------------------- */
struct everlive_device {
	const struct everlive_device_desc *desc;
	char pci_bdf[32];           /* e.g. "0000:03:00.0" */
	int iommu_group;
	bool is_vfio_bound;
	void *priv;                 /* Backend-specific context */
};

/* -------------------------------------------------------------------------
 * Registry & Discovery Functions
 * ------------------------------------------------------------------------- */

/* Returns a pointer to the static registry table and sets out_count */
const struct everlive_device_desc *everlive_get_card_registry(size_t *out_count);

/*
 * Bind a backend implementation to every registry entry with @model_slug.
 * Backends own their hardware context; the core copies the vtable by value,
 * so @ops only has to remain valid for the duration of this call.
 *
 * Registration is a startup operation: it must finish before the first
 * everlive_card_ops_for() call, which is also the point where the library may
 * start being used from multiple threads.  No lock is taken, so callers must
 * register before spawning threads.  Once a lookup has happened the registry
 * is frozen and later calls return -EBUSY.
 *
 * Returns 0, -EINVAL for bad arguments, -ENOENT for an unknown model,
 * -EBUSY if the registry is already frozen, or -ENOSPC when the binding table
 * is full.
 */
int everlive_register_card_ops(const char *model_slug,
			       const struct everlive_card_ops *ops);

/*
 * Resolve the active operations for a descriptor.  This returns the most
 * recently registered backend vtable, the descriptor's built-in vtable, or
 * the core default (which reports -ENOTSUP for hardware actions) in that
 * order, and never NULL.  The first call freezes the registry; register all
 * backends before calling this function.
 */
const struct everlive_card_ops *
everlive_card_ops_for(const struct everlive_device_desc *desc);

/* Matches vendor, device, and subsystem IDs to a registered card descriptor */
const struct everlive_device_desc *everlive_match_card(uint16_t vendor,
						       uint16_t device,
						       uint16_t subsystem);

/* Scans sysfs (/sys/bus/pci/devices) and detects installed capture cards */
int everlive_detect_installed_cards(struct everlive_device *out_devices,
				    size_t max_devices,
				    const char *sysfs_pci_root);

/* Checks if a given PCI BDF is currently bound to vfio-pci */
bool everlive_is_pci_vfio_bound(const char *pci_bdf, const char *sysfs_pci_root);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_CARD_H */

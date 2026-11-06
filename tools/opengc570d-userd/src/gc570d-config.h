// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime configuration for the gc570d userspace driver.
 * Allows adjusting parameters without recompiling.
 */
#ifndef GC570D_CONFIG_H
#define GC570D_CONFIG_H

#include <stdbool.h>

struct gc570d_config {
	int width;             /* default video width (1920) */
	int height;            /* default video height (1080) */
	int hdmi1_poll_ms;     /* HDMI1 DMA poll interval (ms) */
	int hdmi2_poll_ms;     /* HDMI2 DMA poll interval (ms) */
	bool enable_led;       /* Enable LED handling */
	bool enable_audio;     /* Enable ALSA/PipeWire audio capture */
	bool edid_bridge_mode; /* Default to internal EDID bridge (true) or clone (false) */
};

extern struct gc570d_config gc570d_cfg;

/* Load configuration from path (or search standard locations if path is NULL).
 * Returns 0 on success or if no config file is present (keeping defaults),
 * or a negative error code on syntax/file failure.
 */
int gc570d_load_config(const char *path);

#endif /* GC570D_CONFIG_H */

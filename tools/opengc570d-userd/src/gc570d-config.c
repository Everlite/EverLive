// SPDX-License-Identifier: GPL-2.0-only
#include "gc570d-config.h"
#include "gc570d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <ctype.h>

struct gc570d_config gc570d_cfg = {
	.width = GC570D_WIDTH,
	.height = GC570D_HEIGHT,
	.hdmi1_poll_ms = 50,
	.hdmi2_poll_ms = 50,
	.enable_led = true,
	.enable_audio = true,
	.edid_bridge_mode = true,
};

static bool parse_bool(const char *val, bool default_val)
{
	if (!val || !*val)
		return default_val;
	if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 ||
	    strcasecmp(val, "yes") == 0 || strcasecmp(val, "on") == 0)
		return true;
	if (strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0 ||
	    strcasecmp(val, "no") == 0 || strcasecmp(val, "off") == 0)
		return false;
	return default_val;
}

static char *trim_str(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;
	char *end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

int gc570d_load_config(const char *path)
{
	char resolved_path[512];
	FILE *f = NULL;

	if (path && *path) {
		f = fopen(path, "r");
	} else {
		const char *env_path = getenv("GC570D_CONF");
		if (env_path && *env_path)
			f = fopen(env_path, "r");
		if (!f) {
			const char *home = getenv("HOME");
			if (home && *home) {
				snprintf(resolved_path, sizeof(resolved_path),
					 "%s/.config/everlive/gc570d.conf", home);
				f = fopen(resolved_path, "r");
			}
		}
		if (!f)
			f = fopen("/etc/gc570d.conf", "r");
	}

	if (!f)
		return 0; /* No config file found; retain default settings */

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *p = trim_str(line);
		if (!*p || *p == '#' || *p == ';')
			continue;

		char *eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = trim_str(p);
		char *val = trim_str(eq + 1);

		if (strcasecmp(key, "width") == 0) {
			int w = atoi(val);
			if (w >= 640 && w <= 3840)
				gc570d_cfg.width = w;
		} else if (strcasecmp(key, "height") == 0) {
			int h = atoi(val);
			if (h >= 480 && h <= 2160)
				gc570d_cfg.height = h;
		} else if (strcasecmp(key, "hdmi1_poll_ms") == 0) {
			int ms = atoi(val);
			if (ms >= 5 && ms <= 1000)
				gc570d_cfg.hdmi1_poll_ms = ms;
		} else if (strcasecmp(key, "hdmi2_poll_ms") == 0) {
			int ms = atoi(val);
			if (ms >= 5 && ms <= 1000)
				gc570d_cfg.hdmi2_poll_ms = ms;
		} else if (strcasecmp(key, "enable_led") == 0) {
			gc570d_cfg.enable_led = parse_bool(val, gc570d_cfg.enable_led);
		} else if (strcasecmp(key, "enable_audio") == 0) {
			gc570d_cfg.enable_audio = parse_bool(val, gc570d_cfg.enable_audio);
		} else if (strcasecmp(key, "edid_bridge") == 0 ||
			   strcasecmp(key, "edid_bridge_mode") == 0) {
			gc570d_cfg.edid_bridge_mode = parse_bool(val, gc570d_cfg.edid_bridge_mode);
		}
	}

	fclose(f);
	return 0;
}

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

static bool parse_int(const char *val, int min_val, int max_val, int *out, const char *key)
{
	if (!val || !*val)
		return false;

	char *end = NULL;
	errno = 0;
	long v = strtol(val, &end, 10);
	if (end == val || *end != '\0' || errno == ERANGE) {
		fprintf(stderr,
			"[gc570d-config] WARNING: invalid numeric value '%s' for '%s'\n",
			val, key);
		return false;
	}
	if (v < min_val || v > max_val) {
		fprintf(stderr,
			"[gc570d-config] WARNING: value %ld for '%s' out of range [%d, %d]\n",
			v, key, min_val, max_val);
		return false;
	}
	*out = (int)v;
	return true;
}

static bool parse_bool(const char *val, bool default_val, const char *key)
{
	if (!val || !*val)
		return default_val;
	if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 ||
	    strcasecmp(val, "yes") == 0 || strcasecmp(val, "on") == 0)
		return true;
	if (strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0 ||
	    strcasecmp(val, "no") == 0 || strcasecmp(val, "off") == 0)
		return false;
	fprintf(stderr,
		"[gc570d-config] WARNING: unrecognized boolean value '%s' for '%s' (keeping default %s)\n",
		val, key, default_val ? "true" : "false");
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
			parse_int(val, 640, 3840, &gc570d_cfg.width, key);
		} else if (strcasecmp(key, "height") == 0) {
			parse_int(val, 480, 2160, &gc570d_cfg.height, key);
		} else if (strcasecmp(key, "hdmi1_poll_ms") == 0) {
			parse_int(val, 5, 1000, &gc570d_cfg.hdmi1_poll_ms, key);
		} else if (strcasecmp(key, "hdmi2_poll_ms") == 0) {
			parse_int(val, 5, 1000, &gc570d_cfg.hdmi2_poll_ms, key);
		} else if (strcasecmp(key, "enable_led") == 0) {
			gc570d_cfg.enable_led = parse_bool(val, gc570d_cfg.enable_led, key);
		} else if (strcasecmp(key, "enable_audio") == 0) {
			gc570d_cfg.enable_audio = parse_bool(val, gc570d_cfg.enable_audio, key);
		} else if (strcasecmp(key, "edid_bridge") == 0 ||
			   strcasecmp(key, "edid_bridge_mode") == 0) {
			gc570d_cfg.edid_bridge_mode = parse_bool(val, gc570d_cfg.edid_bridge_mode, key);
		}
	}

	fclose(f);
	return 0;
}

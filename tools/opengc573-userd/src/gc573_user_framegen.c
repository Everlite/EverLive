/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_framegen.c — lsfg-vk glue + PS5 phase-locked 2:1 deduplicator
 *
 * Frame generation is done by the native lsfg-vk implicit Vulkan layer
 * (Lossless Scaling Frame Generation, LSFGVK_MULTIPLIER=2).  This file owns
 * everything the layer cannot do on this hardware:
 *
 *   1. Finding the installation: the implicit-layer manifest and lsfg-vk.dll,
 *      including every extra Steam library in libraryfolders.vdf.
 *   2. Exporting the LSFGVK_* environment *before* SDL creates the Vulkan
 *      instance, with locale-independent number formatting.
 *   3. The PS5 30-in-60 parity deduplicator: a 30 FPS game inside a 60 Hz HDMI
 *      container sends A, A, B, B.  Presenting every frame would give the
 *      layer 60 duplicate-heavy inputs (2x -> 120, still 30 FPS motion), so the
 *      deduplicator emits exactly one frame per game frame and lsfg-vk's 2x
 *      lands on a clean 60.0 FPS.
 *   4. A 3-slot capture ring; the render thread presents once per accepted
 *      frame and never re-presents on idle refreshes (a duplicate present
 *      would make the layer generate motion from a frozen pair).
 */

#include "gc573_user_framegen.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FG_INPUT_SLOTS	8U

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t fg_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000U + (uint64_t)(ts.tv_nsec / 1000L);
}

/* Case-insensitive substring search (strcasestr is GNU-only). */
static const char *fg_strcasestr(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	if (!n)
		return hay;

	for (const char *p = hay; *p; p++) {
		size_t i = 0;

		while (i < n && p[i] &&
		       tolower((unsigned char)p[i]) ==
		       tolower((unsigned char)needle[i]))
			i++;
		if (i == n)
			return p;
	}
	return NULL;
}

static bool fg_readable(const char *path)
{
	return path && *path && access(path, R_OK) == 0;
}

/* Bounded copy that cannot trip -Wformat-truncation at -O2. */
static void fg_copy(char *dst, size_t len, const char *src)
{
	size_t n;

	if (!dst || len == 0)
		return;
	if (!src)
		src = "";
	n = strlen(src);
	if (n >= len)
		n = len - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* Append one fragment to an already NUL-terminated buffer, truncating safely. */
static void fg_append(char *dst, size_t len, const char *src)
{
	size_t used;
	size_t n;

	if (!dst || len == 0 || !src)
		return;
	used = strlen(dst);
	n = strlen(src);
	if (used + n >= len)
		n = len - used - 1;
	memcpy(dst + used, src, n);
	dst[used + n] = '\0';
}

/* dst = a + b {+ c}; any NULL fragment is skipped. */
static void fg_path(char *dst, size_t len, const char *a, const char *b,
		    const char *c)
{
	if (!dst || len == 0)
		return;
	dst[0] = '\0';
	fg_append(dst, len, a);
	fg_append(dst, len, b);
	fg_append(dst, len, c);
}

static void fg_home_dir(char *out, size_t len)
{
	const char *home = getenv("HOME");
	struct passwd *pw;

	if (home && *home) {
		fg_copy(out, len, home);
		return;
	}
	pw = getpwuid(getuid());
	if (pw && pw->pw_dir) {
		fg_copy(out, len, pw->pw_dir);
		return;
	}
	fg_copy(out, len, "/tmp");
}

/* mkdir -p for the config directory. */
static void fg_mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	size_t len;
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (len == 0)
		return;
	if (tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return;
		*p = '/';
	}
	(void)mkdir(tmp, 0755);
}

void gc573_fg_format_scale(float flow_scale, char *buf, size_t len)
{
	int pct;

	if (flow_scale < 0.5f)
		flow_scale = 0.5f;
	if (flow_scale > 1.0f)
		flow_scale = 1.0f;
	pct = (int)(flow_scale * 100.0f + 0.5f);
	/* Integer split: a locale comma can never leak into the env or TOML. */
	snprintf(buf, len, "%d.%02d", pct / 100, pct % 100);
}

const char *gc573_fg_mode_name(int mode)
{
	switch (mode) {
	case GC573_FG_60_TO_120:
		return "60to120";
	case GC573_FG_30_TO_60:
		return "30to60";
	default:
		return "off";
	}
}

int gc573_fg_clamp_multiplier(int multiplier)
{
	if (multiplier <= 1)
		return 2;
	if (multiplier > 4)
		return 4;
	return multiplier;
}

bool gc573_fg_parse_spec(const char *name, int *mode_out, int *mult_out)
{
	int mode = -1;
	int mult = 2;

	if (!name || !*name)
		return false;
	if (!strcasecmp(name, "off") || !strcasecmp(name, "none") ||
	    !strcasecmp(name, "native") || !strcmp(name, "0")) {
		mode = GC573_FG_OFF;
	} else if (!strcasecmp(name, "60to120") || !strcasecmp(name, "2x") ||
		   !strcasecmp(name, "60") || !strcasecmp(name, "120")) {
		mode = GC573_FG_60_TO_120;
		mult = 2;
	} else if (!strcasecmp(name, "3x") || !strcasecmp(name, "60to180") ||
		   !strcasecmp(name, "180")) {
		mode = GC573_FG_60_TO_120;
		mult = 3;
	} else if (!strcasecmp(name, "4x") || !strcasecmp(name, "60to240") ||
		   !strcasecmp(name, "240")) {
		mode = GC573_FG_60_TO_120;
		mult = 4;
	} else if (!strcasecmp(name, "30to60") || !strcasecmp(name, "30") ||
		   !strcasecmp(name, "ps5")) {
		mode = GC573_FG_30_TO_60;
		mult = 2;
	} else if (!strcasecmp(name, "30to90") || !strcasecmp(name, "90")) {
		mode = GC573_FG_30_TO_60;
		mult = 3;
	} else if (!strcasecmp(name, "30to120")) {
		mode = GC573_FG_30_TO_60;
		mult = 4;
	} else {
		return false;
	}

	if (mode_out)
		*mode_out = mode;
	if (mult_out)
		*mult_out = mult;
	return true;
}

int gc573_fg_mode_parse(const char *name)
{
	int mode = -1;

	if (!gc573_fg_parse_spec(name, &mode, NULL))
		return -1;
	return mode;
}

/* ------------------------------------------------------------------ */
/* lsfg-vk installation probe                                          */
/* ------------------------------------------------------------------ */

/* Load up to 64 KiB of a file into a caller buffer (NUL terminated). */
static bool fg_slurp(const char *path, char *buf, size_t len)
{
	FILE *f;
	size_t got;

	if (!path || !buf || len < 2)
		return false;
	f = fopen(path, "r");
	if (!f)
		return false;
	got = fread(buf, 1, len - 1, f);
	fclose(f);
	buf[got] = '\0';
	return true;
}

/*
 * A Vulkan implicit-layer directory matches when a *.json manifest is named
 * after lsfg or mentions it inside (the CachyOS package ships
 * VkLayer_LSFGVK_frame_generation.json).
 */
static bool fg_dir_has_lsfg(const char *dir, char *out, size_t out_len)
{
	DIR *d;
	struct dirent *ent;

	d = opendir(dir);
	if (!d)
		return false;

	while ((ent = readdir(d)) != NULL) {
		char path[PATH_MAX];
		bool hit = false;

		if (ent->d_name[0] == '.')
			continue;
		if (!fg_strcasestr(ent->d_name, ".json"))
			continue;

		if (fg_strcasestr(ent->d_name, "lsfg")) {
			hit = true;
		} else {
			char buf[65536];

			fg_path(path, sizeof(path), dir, "/", ent->d_name);
			if (fg_slurp(path, buf, sizeof(buf)) &&
			    (fg_strcasestr(buf, "lsfg") ||
			     fg_strcasestr(buf, "lossless")))
				hit = true;
		}

		if (hit) {
			snprintf(out, out_len, "%s", dir);
			closedir(d);
			return true;
		}
	}

	closedir(d);
	return false;
}

/*
 * Resolve a candidate shader DLL path for lsfg-vk 2.x.  If a stale config or
 * env var still points at the Windows DirectX Lossless.dll, prefer a sibling
 * lsfg-vk.dll (shipped by Steam's 'lsfg-vk' beta branch) and reject bare
 * Lossless.dll which lacks the precompiled Vulkan SPIR-V shaders ('mipmaps').
 */
static bool fg_resolve_lsfg_dll(const char *cand, char *out, size_t out_len)
{
	const char *slash;

	if (!cand || !*cand)
		return false;

	slash = strrchr(cand, '/');
	if (slash && strcasecmp(slash + 1, "Lossless.dll") == 0) {
		char sibling[PATH_MAX];
		size_t dir_len = (size_t)(slash - cand);

		if (dir_len + sizeof("/lsfg-vk.dll") <= sizeof(sibling)) {
			memcpy(sibling, cand, dir_len);
			memcpy(sibling + dir_len, "/lsfg-vk.dll",
			       sizeof("/lsfg-vk.dll"));
			if (fg_readable(sibling)) {
				fg_copy(out, out_len, sibling);
				return true;
			}
		}
		return false;
	}

	if (fg_readable(cand)) {
		fg_copy(out, out_len, cand);
		return true;
	}
	return false;
}

/* dll = "/path/to/lsfg-vk.dll" out of lsfg-vk's own conf.toml. */
static bool fg_conf_toml_dll(const char *path, char *out, size_t out_len)
{
	char buf[65536];
	char *line;
	char *save = NULL;

	if (!fg_slurp(path, buf, sizeof(buf)))
		return false;

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *key = line;
		char *quote;
		char *end;

		while (*key == ' ' || *key == '\t')
			key++;
		if (strncasecmp(key, "dll", 3) != 0)
			continue;
		quote = strchr(key, '"');
		if (!quote)
			continue;
		quote++;
		end = strchr(quote, '"');
		if (!end)
			continue;
		*end = '\0';
		if (fg_resolve_lsfg_dll(quote, out, out_len))
			return true;
	}
	return false;
}

/*
 * Walk a libraryfolders.vdf for "path" entries and test each library for the
 * Lossless Scaling 'lsfg-vk' beta install (lsfg-vk.dll).  Backslashes are
 * unescaped to '/'.
 */
static bool fg_libraryfolders_dll(const char *vdf, char *out, size_t out_len)
{
	char buf[65536];
	const char *p = buf;

	if (!fg_slurp(vdf, buf, sizeof(buf)))
		return false;

	while ((p = fg_strcasestr(p, "\"path\"")) != NULL) {
		char lib[PATH_MAX];
		char cand[PATH_MAX];
		size_t j = 0;

		p += 6;
		while (*p == ' ' || *p == '\t' || *p == '\r')
			p++;
		if (*p != '"') {
			p++;
			continue;
		}
		p++;

		while (*p && *p != '"' && j + 1 < sizeof(lib)) {
			if (p[0] == '\\' && p[1] == '\\') {
				lib[j++] = '/';
				p += 2;
				continue;
			}
			lib[j++] = *p++;
		}
		lib[j] = '\0';

		fg_path(cand, sizeof(cand), lib,
			"/steamapps/common/Lossless Scaling/lsfg-vk.dll", NULL);
		if (fg_readable(cand)) {
			fg_copy(out, out_len, cand);
			return true;
		}
	}
	return false;
}

static bool fg_steam_roots_dll(char *out, size_t out_len)
{
	static const char *const rel[] = {
		"/.local/share/Steam",
		"/.steam/steam",
	};
	char home[PATH_MAX];

	fg_home_dir(home, sizeof(home));

	for (size_t i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
		char cand[PATH_MAX];
		char vdf[PATH_MAX];

		fg_path(cand, sizeof(cand), home, rel[i],
			"/steamapps/common/Lossless Scaling/lsfg-vk.dll");
		if (fg_readable(cand)) {
			fg_copy(out, out_len, cand);
			return true;
		}

		fg_path(vdf, sizeof(vdf), home, rel[i],
			"/steamapps/libraryfolders.vdf");
		if (fg_libraryfolders_dll(vdf, out, out_len))
			return true;
	}

	return false;
}

void gc573_fg_probe_system(struct gc573_fg_probe *out)
{
	char home[PATH_MAX];
	char xdg[PATH_MAX];
	char dir[PATH_MAX];
	const char *env;
	const char *xdg_data;
	const char *xdg_cfg;

	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	fg_home_dir(home, sizeof(home));

	/* --- 1. implicit-layer manifest ---------------------------------- */
	xdg_data = getenv("XDG_DATA_HOME");
	if (!xdg_data || !*xdg_data) {
		fg_path(xdg, sizeof(xdg), home, "/.local/share", NULL);
		xdg_data = xdg;
	}

	if (fg_dir_has_lsfg("/usr/share/vulkan/implicit_layer.d",
			    out->layer_dir, sizeof(out->layer_dir)) ||
	    fg_dir_has_lsfg("/etc/vulkan/implicit_layer.d",
			    out->layer_dir, sizeof(out->layer_dir))) {
		out->layer_found = true;
	} else {
		fg_path(dir, sizeof(dir), xdg_data,
			"/vulkan/implicit_layer.d", NULL);
		if (fg_dir_has_lsfg(dir, out->layer_dir, sizeof(out->layer_dir)))
			out->layer_found = true;
	}

	/* --- 2. lsfg-vk.dll ---------------------------------------------- */
	env = getenv("LSFGVK_DLL_PATH");
	if (fg_resolve_lsfg_dll(env, out->dll_path, sizeof(out->dll_path)))
		out->dll_found = true;

	if (!out->dll_found) {
		char conf[PATH_MAX];

		xdg_cfg = getenv("XDG_CONFIG_HOME");
		if (xdg_cfg && *xdg_cfg)
			fg_path(conf, sizeof(conf), xdg_cfg, "/lsfg-vk/conf.toml",
				NULL);
		else
			fg_path(conf, sizeof(conf), home,
				"/.config/lsfg-vk/conf.toml", NULL);
		if (fg_conf_toml_dll(conf, out->dll_path, sizeof(out->dll_path)))
			out->dll_found = true;
		else {
			fg_path(conf, sizeof(conf), home,
				"/.config/lsfg-vk/conf.toml", NULL);
			if (fg_conf_toml_dll(conf, out->dll_path,
					     sizeof(out->dll_path)))
				out->dll_found = true;
		}
	}

	if (!out->dll_found &&
	    fg_steam_roots_dll(out->dll_path, sizeof(out->dll_path)))
		out->dll_found = true;
}

bool gc573_fg_apply_env(const struct gc573_fg_config *cfg,
			struct gc573_fg_probe *out)
{
	struct gc573_fg_probe local;
	char scale[16];
	char mult_str[8];
	int mult;

	if (!cfg)
		return false;
	if (!out)
		out = &local;

	/* Re-probe: the user may have just installed lsfg-vk or the game. */
	gc573_fg_probe_system(out);

	/* Never let a stale "disable" poison the instance we are about to make. */
	unsetenv("DISABLE_LSFGVK");
	/* FP16 acceleration stays enabled. */
	unsetenv("LSFGVK_NO_FP16");

	mult = gc573_fg_clamp_multiplier(cfg->multiplier);
	snprintf(mult_str, sizeof(mult_str), "%d", mult);
	setenv("LSFGVK_ENV", "1", 1);
	setenv("LSFGVK_MULTIPLIER", mult_str, 1);

	gc573_fg_format_scale(cfg->flow_scale, scale, sizeof(scale));
	setenv("LSFGVK_FLOW_SCALE", scale, 1);

	/*
	 * Presence-based check: "0" would still count as enabled, so the
	 * variable must be absent when performance mode is off.
	 */
	if (cfg->perf_mode)
		setenv("LSFGVK_PERFORMANCE_MODE", "1", 1);
	else
		unsetenv("LSFGVK_PERFORMANCE_MODE");

	/*
	 * Low-latency swapchain. With frame generation off no lsfg-vk instance
	 * exists at all, so leave the variable unset.
	 * At 120 Hz input on 240 Hz panels (or 3x/4x), LSFG-VK needs extra
	 * swapchain images to pace companion frames without acquire stalls.
	 */
	if (cfg->mode == GC573_FG_OFF || !cfg->low_latency || mult >= 3 || cfg->multiplier >= 2)
		unsetenv("LSFGVK_PRESERVE_SWAPCHAIN_IMAGE_COUNT");
	else
		setenv("LSFGVK_PRESERVE_SWAPCHAIN_IMAGE_COUNT", "1", 1);

	if (out->dll_found)
		setenv("LSFGVK_DLL_PATH", out->dll_path, 1);

	return out->layer_found && out->dll_found;
}

void gc573_fg_write_toml(const struct gc573_fg_config *cfg,
			 const struct gc573_fg_probe *probe)
{
	char home[PATH_MAX];
	char dir[PATH_MAX];
	char path[PATH_MAX];
	char scale[16];
	FILE *f;

	if (!cfg)
		return;

	fg_home_dir(home, sizeof(home));
	fg_path(dir, sizeof(dir), home, "/.config/everlive", NULL);
	fg_mkdir_p(dir);
	fg_path(path, sizeof(path), dir, "/lsfg.toml", NULL);

	f = fopen(path, "w");
	if (!f)
		return;

	gc573_fg_format_scale(cfg->flow_scale, scale, sizeof(scale));

	fprintf(f, "# Managed by EverLive - reference profile.\n");
	fprintf(f, "# The engine drives lsfg-vk through LSFGVK_* environment\n");
	fprintf(f, "# variables; this file documents and mirrors that setup.\n");
	fprintf(f, "version = 2\n\n");
	fprintf(f, "[global]\nallow_fp16 = true\n");
	if (probe && probe->dll_found)
		fprintf(f, "dll = \"%s\"\n", probe->dll_path);
	fprintf(f, "\n[[profile]]\n");
	fprintf(f, "name = \"everlive\"\n");
	fprintf(f, "active_in = [\"gc573_user_streamer\"]\n");
	fprintf(f, "multiplier = %d\n", gc573_fg_clamp_multiplier(cfg->multiplier));
	fprintf(f, "flow_scale = %s\n", scale);
	fprintf(f, "performance_mode = %s\n",
		cfg->perf_mode ? "true" : "false");
	fprintf(f, "preserve_swapchain_image_count = %s\n",
		cfg->low_latency ? "true" : "false");
	fclose(f);
}

static void fg_env_append_token(const char *env_name, const char *token)
{
	const char *cur = getenv(env_name);
	size_t tlen;

	if (!env_name || !token || !*token)
		return;
	tlen = strlen(token);
	if (!cur || !*cur) {
		setenv(env_name, token, 1);
		return;
	}

	const char *p = cur;

	while (*p) {
		const char *sep = strchr(p, ':');
		size_t flen = sep ? (size_t)(sep - p) : strlen(p);

		if (flen == tlen && strncmp(p, token, tlen) == 0)
			return;
		if (!sep)
			break;
		p = sep + 1;
	}

	size_t clen = strlen(cur);
	char *joined = malloc(clen + tlen + 2);

	if (!joined)
		return;
	memcpy(joined, cur, clen);
	joined[clen] = ':';
	memcpy(joined + clen + 1, token, tlen + 1);
	setenv(env_name, joined, 1);
	free(joined);
}

static bool fg_resolve_pacer_lib(char *out, size_t out_len)
{
	char exe_path[PATH_MAX];
	char exe_dir[PATH_MAX] = "";
	ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);

	if (n > 0) {
		exe_path[n] = '\0';
		char *slash = strrchr(exe_path, '/');

		if (slash) {
			*slash = '\0';
			fg_copy(exe_dir, sizeof(exe_dir), exe_path);
		}
	}

	char cand1[PATH_MAX] = "";
	char cand2[PATH_MAX] = "";

	if (exe_dir[0]) {
		fg_path(cand1, sizeof(cand1), exe_dir, "/libeverlive-vk-pacer.so", NULL);
		fg_path(cand2, sizeof(cand2), exe_dir, "/../lib/libeverlive-vk-pacer.so", NULL);
	}

	const char *candidates[] = {
		cand1[0] ? cand1 : NULL,
		cand2[0] ? cand2 : NULL,
		"/usr/local/lib/libeverlive-vk-pacer.so",
		"/usr/lib/libeverlive-vk-pacer.so",
	};

	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		char resolved[PATH_MAX];

		if (!candidates[i] || !fg_readable(candidates[i]))
			continue;
		if (realpath(candidates[i], resolved)) {
			fg_copy(out, out_len, resolved);
			return true;
		}
	}
	return false;
}

static void fg_write_json_escaped(FILE *f, const char *s)
{
	for (const char *p = s; *p; p++) {
		if (*p == '\\' || *p == '"')
			fputc('\\', f);
		fputc(*p, f);
	}
}

double gc573_fg_target_output_hz(const struct gc573_fg_config *cfg,
				 double source_hz)
{
	double base_hz;

	if (!cfg || cfg->mode == GC573_FG_OFF || cfg->native_backend)
		return 0.0;

	base_hz = (source_hz >= 20.0 && source_hz <= 250.0) ? source_hz : 60.0;
	if (cfg->mode == GC573_FG_30_TO_60 && base_hz > 45.0)
		base_hz *= 0.5;
	return base_hz * (double)gc573_fg_clamp_multiplier(cfg->multiplier);
}

void gc573_fg_update_pacer_hz(double target_hz)
{
	char buf[32];

	if (target_hz > 10.0 && target_hz < 235.0) {
		int centihz = (int)(target_hz * 100.0 + 0.5);

		snprintf(buf, sizeof(buf), "%d.%02d",
			 centihz / 100, centihz % 100);
	} else {
		snprintf(buf, sizeof(buf), "0");
	}
	setenv("EVERLIVE_VK_PACER_HZ", buf, 1);

	void (*set_hz)(double) =
		(void (*)(double))dlsym(RTLD_DEFAULT, "everlive_vk_pacer_set_hz");
	if (set_hz)
		set_hz(target_hz);
}

bool gc573_fg_prepare_pacer(const struct gc573_fg_config *cfg,
			    double source_hz)
{
	char lib_path[PATH_MAX];
	char layer_dir[PATH_MAX];
	char json_path[PATH_MAX];
	char xdg_fallback[PATH_MAX];
	const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
	double target_hz = gc573_fg_target_output_hz(cfg, source_hz);
	FILE *f;

	gc573_fg_update_pacer_hz(target_hz);
	if (target_hz <= 0.0)
		return false;

	if (!fg_resolve_pacer_lib(lib_path, sizeof(lib_path)))
		return false;

	if (!xdg_cfg || !*xdg_cfg) {
		char home[PATH_MAX];

		fg_home_dir(home, sizeof(home));
		fg_path(xdg_fallback, sizeof(xdg_fallback), home, "/.config", NULL);
		xdg_cfg = xdg_fallback;
	}

	fg_path(layer_dir, sizeof(layer_dir), xdg_cfg, "/everlive/vk_layers", NULL);
	fg_mkdir_p(layer_dir);
	fg_path(json_path, sizeof(json_path), layer_dir,
		"/VkLayer_EVERLIVE_pacer.json", NULL);

	f = fopen(json_path, "w");
	if (!f)
		return false;

	fprintf(f, "{\n");
	fprintf(f, "  \"file_format_version\": \"1.1.0\",\n");
	fprintf(f, "  \"layer\": {\n");
	fprintf(f, "    \"name\": \"VK_LAYER_EVERLIVE_pacer\",\n");
	fprintf(f, "    \"description\": \"EverLive Vulkan framegen present pacer\",\n");
	fprintf(f, "    \"implementation_version\": \"1\",\n");
	fprintf(f, "    \"library_path\": \"");
	fg_write_json_escaped(f, lib_path);
	fprintf(f, "\",\n");
	fprintf(f, "    \"type\": \"GLOBAL\",\n");
	fprintf(f, "    \"api_version\": \"1.4.0\"\n");
	fprintf(f, "  }\n");
	fprintf(f, "}\n");
	fclose(f);

	fg_env_append_token("VK_ADD_LAYER_PATH", layer_dir);
	fg_env_append_token("VK_INSTANCE_LAYERS", "VK_LAYER_EVERLIVE_pacer");
	return true;
}

/* ------------------------------------------------------------------ */
/* Phase-locked 2:1 HDMI parity deduplicator                           */
/* ------------------------------------------------------------------ */

/*
 * Sample one grid cell as the average of a 4x2 luma block: four adjacent YUYV
 * macropixels (8 luma samples) across two rows, clamped inside the frame.
 * Averaging cancels the temporal/spatial dithering the console capture chain
 * adds to otherwise identical HDMI repeats.
 */
static void fg_cadence_sample(const struct gc573_fg_cadence *cad,
			      const uint8_t *yuyv, uint8_t *out)
{
	const uint32_t stride = cad->width * 2U;
	const uint32_t top = cad->height * GC573_FG_GRID_MARGIN_PCT / 100U;
	const uint32_t active = cad->height - top * 2U;

	for (uint32_t gy = 0; gy < GC573_FG_GRID_H; gy++) {
		uint32_t y = top + (active * (gy * 2U + 1U)) /
				   (uint32_t)(GC573_FG_GRID_H * 2U);
		uint32_t y1;
		const uint8_t *row0;
		const uint8_t *row1;

		if (y >= cad->height)
			y = cad->height - 1U;
		y1 = (y + 1U < cad->height) ? y + 1U : y;
		row0 = yuyv + (size_t)y * stride;
		row1 = yuyv + (size_t)y1 * stride;

		for (uint32_t gx = 0; gx < GC573_FG_GRID_W; gx++) {
			uint32_t x = (cad->width * (gx * 2U + 1U)) /
				     (uint32_t)(GC573_FG_GRID_W * 2U);
			uint32_t sum = 0;

			if (x >= cad->width)
				x = cad->width - 1U;
			x &= ~1U;

			for (uint32_t k = 0; k < 4U; k++) {
				uint32_t px = x + k * 2U;

				if (px + 1U >= cad->width)
					px = cad->width - 2U;
				sum += row0[px * 2U] + row0[px * 2U + 2U];
				sum += row1[px * 2U] + row1[px * 2U + 2U];
			}

			/* 4 macropixels x 2 luma x 2 rows = 16 samples. */
			out[gy * GC573_FG_GRID_W + gx] =
				(uint8_t)((sum + 8U) / 16U);
		}
	}
}

static float fg_cadence_measure(const struct gc573_fg_cadence *cad)
{
	const uint32_t cells = (uint32_t)GC573_FG_GRID_W * GC573_FG_GRID_H;
	uint64_t total = 0;

	for (uint32_t i = 0; i < cells; i++) {
		int diff = (int)cad->prev_raw[i] - (int)cad->scratch[i];

		total += (uint64_t)(diff < 0 ? -diff : diff);
	}

	return (float)total / (float)cells;
}

void gc573_fg_cadence_init(struct gc573_fg_cadence *cad,
			   uint32_t width, uint32_t height)
{
	if (!cad)
		return;

	memset(cad, 0, sizeof(*cad));
	cad->width = width;
	cad->height = height;
	cad->active_parity = 0;
	if (width < 2U || height < 2U)
		return;

	cad->prev_raw = calloc((size_t)GC573_FG_GRID_W * GC573_FG_GRID_H, 1);
	cad->scratch = calloc((size_t)GC573_FG_GRID_W * GC573_FG_GRID_H, 1);
	if (!cad->prev_raw || !cad->scratch)
		gc573_fg_cadence_free(cad);
}

void gc573_fg_cadence_reset(struct gc573_fg_cadence *cad)
{
	if (!cad)
		return;
	cad->have_prev = false;
	cad->raw_frames = 0;
	cad->unique_frames = 0;
	cad->duplicates = 0;
	cad->parity_flips = 0;
	cad->even_mad_ema = 0.0f;
	cad->odd_mad_ema = 0.0f;
	cad->active_parity = 0;
	cad->locked = false;
	cad->auto_locked = false;
	cad->consecutive_60p = 0;
	cad->last_mad = 0.0f;
	cad->prev_raw_mad = 0.0f;
	cad->last_unique_us = 0;
	cad->dt_unique_us = 0;
	cad->smoothed_dt_us = 0.0;
}

void gc573_fg_cadence_free(struct gc573_fg_cadence *cad)
{
	if (!cad)
		return;
	free(cad->prev_raw);
	free(cad->scratch);
	cad->prev_raw = NULL;
	cad->scratch = NULL;
}

/* Choose the parity that carries the large (new-frame) transitions. */
static void fg_cadence_relock(struct gc573_fg_cadence *cad)
{
	int want = cad->active_parity;

	if (cad->even_mad_ema > cad->odd_mad_ema * GC573_FG_LOCK_RATIO +
				       GC573_FG_LOCK_BIAS)
		want = 0;
	else if (cad->odd_mad_ema > cad->even_mad_ema * GC573_FG_LOCK_RATIO +
					 GC573_FG_LOCK_BIAS)
		want = 1;
	else
		return;

	if (!cad->locked || want != cad->active_parity)
		cad->parity_flips++;

	cad->active_parity = want;
	cad->locked = true;
}

int gc573_fg_cadence_classify(struct gc573_fg_cadence *cad,
			      const uint8_t *yuyv, uint64_t now_us)
{
	uint32_t parity;
	float mad;

	if (!cad || !yuyv || !cad->scratch || !cad->prev_raw)
		return GC573_FG_DUPLICATE;

	fg_cadence_sample(cad, yuyv, cad->scratch);

	if (!cad->have_prev) {
		/* Seed: the very first raw frame is always a new frame. */
		memcpy(cad->prev_raw, cad->scratch,
		       (size_t)GC573_FG_GRID_W * GC573_FG_GRID_H);
		cad->have_prev = true;
		cad->raw_frames = 1;
		cad->active_parity = 0;
		cad->locked = false;
		cad->last_unique_us = now_us;
		cad->unique_frames++;
		return GC573_FG_UNIQUE;
	}

	parity = cad->raw_frames & 1U;
	mad = fg_cadence_measure(cad);
	cad->last_mad = mad;

	/*
	 * In true 60p, adjacent transitions have comparable motion:
	 * max_m < min_m * 2.5f + 0.6f.
	 * In 30-in-60 (A, A, B, B), one transition is high motion and the other is
	 * a duplicate frame with noise/water (ratio max/min >> 3.0).
	 */
	{
		float min_m = fminf(mad, cad->prev_raw_mad);
		float max_m = fmaxf(mad, cad->prev_raw_mad);

		if (min_m > 1.1f && max_m < min_m * 2.5f + 0.6f)
			cad->consecutive_60p++;
		else if (max_m > min_m * 3.0f + 0.8f || max_m < 0.6f)
			cad->consecutive_60p = 0;
	}

	/*
	 * Instantaneous phase slip (A, A, A, B, B): a frame that arrives on the
	 * expected-duplicate parity but is clearly more different than the
	 * previous transition means the console dropped/reordered a frame.
	 * Flip immediately so B is never delayed or lost.
	 */
	if ((int)parity != cad->active_parity &&
	    mad > cad->prev_raw_mad * GC573_FG_SLIP_MAD_FACTOR &&
	    mad > GC573_FG_SLIP_MAD_MIN) {
		float tmp = cad->even_mad_ema;

		cad->active_parity = (int)parity;
		cad->parity_flips++;

		/* The two EMAs are off by one phase: swap and nudge the new one. */
		cad->even_mad_ema = cad->odd_mad_ema;
		cad->odd_mad_ema = tmp;
		if (parity == 0 && cad->even_mad_ema < mad * 0.5f)
			cad->even_mad_ema = mad * 0.5f;
		if (parity == 1 && cad->odd_mad_ema < mad * 0.5f)
			cad->odd_mad_ema = mad * 0.5f;
	}

	/* Per-parity motion EMA. */
	if (parity == 0)
		cad->even_mad_ema = cad->even_mad_ema * (1.0f - GC573_FG_MAD_EMA_ALPHA) +
				    mad * GC573_FG_MAD_EMA_ALPHA;
	else
		cad->odd_mad_ema = cad->odd_mad_ema * (1.0f - GC573_FG_MAD_EMA_ALPHA) +
				   mad * GC573_FG_MAD_EMA_ALPHA;

	fg_cadence_relock(cad);

	{
		float hi = fmaxf(cad->even_mad_ema, cad->odd_mad_ema);
		float lo = fminf(cad->even_mad_ema, cad->odd_mad_ema);

		if (!cad->auto_locked) {
			if (cad->raw_frames >= 8U && cad->consecutive_60p == 0U &&
			    hi > lo * 3.0f + 0.55f)
				cad->auto_locked = true;
		} else if (cad->consecutive_60p >= 10U && hi < lo * 2.0f + 0.8f) {
			cad->auto_locked = false;
			cad->last_unique_us = 0;
			cad->smoothed_dt_us = 0.0;
		}
	}

	/*
	 * Emit strictly on the locked parity - also when the scene is frozen
	 * (both EMAs ~0), which keeps the 2:1 heartbeat at a steady 30.00 Hz.
	 */
	if ((int)parity != cad->active_parity) {
		cad->duplicates++;
		cad->raw_frames++;
		cad->prev_raw_mad = mad;
		memcpy(cad->prev_raw, cad->scratch,
		       (size_t)GC573_FG_GRID_W * GC573_FG_GRID_H);
		return GC573_FG_DUPLICATE;
	}

	if (cad->last_unique_us && now_us > cad->last_unique_us) {
		uint64_t dt = now_us - cad->last_unique_us;
		double clamped = (double)dt;

		if (clamped < 14000.0)
			clamped = 14000.0;
		if (clamped > 50000.0)
			clamped = 50000.0;
		if (dt < 200000ULL) {
			cad->dt_unique_us = dt;
			if (cad->smoothed_dt_us <= 0.0)
				cad->smoothed_dt_us = clamped;
			else
				cad->smoothed_dt_us =
					cad->smoothed_dt_us * 0.85 + clamped * 0.15;
		}
	}

	cad->last_unique_us = now_us;
	cad->unique_frames++;
	cad->raw_frames++;
	cad->prev_raw_mad = mad;
	memcpy(cad->prev_raw, cad->scratch,
	       (size_t)GC573_FG_GRID_W * GC573_FG_GRID_H);
	return GC573_FG_UNIQUE;
}

double gc573_fg_cadence_unique_fps(const struct gc573_fg_cadence *cad)
{
	if (!cad || cad->smoothed_dt_us <= 0.0)
		return 0.0;
	return 1000000.0 / cad->smoothed_dt_us;
}

/* ------------------------------------------------------------------ */
/* Capture ring                                                        */
/* ------------------------------------------------------------------ */

struct gc573_framegen {
	pthread_mutex_t lock;
	bool lock_ready;
	/* Signalled by the DMA thread when an accepted frame is queued. */
	pthread_cond_t ready_cond;
	bool cond_ready;

	struct gc573_fg_config cfg;
	uint32_t width;
	uint32_t height;
	size_t slot_bytes;

	uint8_t *slots[FG_INPUT_SLOTS];
	uint64_t slot_ts[FG_INPUT_SLOTS];
	bool slot_used[FG_INPUT_SLOTS];
	unsigned int queue[FG_INPUT_SLOTS];
	unsigned int head;
	unsigned int tail;
	unsigned int count;
	int taken_slot;
	/* Wait() predicate: a queued frame exists and none is being presented. */
	bool has_unread;
	bool in_flight;

	struct gc573_fg_cadence cadence;
	unsigned int source_hz_x100;
	bool source_fast;
	uint64_t prev_raw_ts;

	struct gc573_fg_probe probe;
	bool vulkan_active;
	gc573_fg_frame_filter filter;
	void *filter_opaque;

	uint32_t input_dropped;

	/* Rates. */
	uint64_t present_win_start;
	uint32_t present_win_count;
	double present_fps;
	uint64_t raw_win_start;
	uint32_t raw_win_count;
	double raw_fps;
};

static bool fg_source_is_fast(struct gc573_framegen *fg, uint64_t now)
{
	bool fast;

	if (fg->source_hz_x100) {
		fast = (fg->source_hz_x100 > GC573_FG_DEDUP_MIN_HZ_X100 &&
			fg->source_hz_x100 <= GC573_FG_DEDUP_MAX_HZ_X100);
	} else if (fg->prev_raw_ts && now > fg->prev_raw_ts) {
		uint64_t dt = now - fg->prev_raw_ts;
		/* Between 11 ms (90 Hz) and 24 ms (41 Hz) */
		fast = (dt >= 11000ULL && dt < 24000ULL);
	} else {
		fast = true;	/* unknown: assume a 60 Hz container */
	}

	fg->prev_raw_ts = now;
	return fast;
}

static void fg_slot_free_all_locked(struct gc573_framegen *fg)
{
	for (unsigned int i = 0; i < FG_INPUT_SLOTS; i++) {
		free(fg->slots[i]);
		fg->slots[i] = NULL;
		fg->slot_used[i] = false;
		fg->slot_ts[i] = 0;
	}
	fg->head = 0;
	fg->tail = 0;
	fg->count = 0;
	fg->slot_bytes = 0;
	fg->taken_slot = -1;
	fg->has_unread = false;
	fg->in_flight = false;
}

struct gc573_framegen *gc573_framegen_create(const struct gc573_fg_config *cfg)
{
	struct gc573_framegen *fg;

	fg = calloc(1, sizeof(*fg));
	if (!fg)
		return NULL;

	fg->cfg.mode = GC573_FG_OFF;
	fg->cfg.multiplier = 2;
	fg->cfg.flow_scale = 1.0f;
	fg->cfg.perf_mode = false;
	fg->cfg.low_latency = true;
	if (cfg) {
		int mode = cfg->mode;

		if (mode < GC573_FG_OFF || mode > GC573_FG_30_TO_60)
			mode = GC573_FG_OFF;
		fg->cfg = *cfg;
		fg->cfg.mode = mode;
		fg->cfg.multiplier = gc573_fg_clamp_multiplier(cfg->multiplier);
		if (fg->cfg.flow_scale < 0.5f)
			fg->cfg.flow_scale = 0.5f;
		if (fg->cfg.flow_scale > 1.0f)
			fg->cfg.flow_scale = 1.0f;
	}

	if (pthread_mutex_init(&fg->lock, NULL) != 0) {
		free(fg);
		return NULL;
	}
	fg->lock_ready = true;

	{
		pthread_condattr_t attr;
		int err = pthread_condattr_init(&attr);

		if (err == 0) {
			/* SDL_Delay-free wakeups need a monotonic timed wait. */
			(void)pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
			err = pthread_cond_init(&fg->ready_cond, &attr);
			pthread_condattr_destroy(&attr);
		}
		if (err != 0) {
			pthread_mutex_destroy(&fg->lock);
			fg->lock_ready = false;
			free(fg);
			return NULL;
		}
		fg->cond_ready = true;
	}

	fg->taken_slot = -1;
	return fg;
}

void gc573_framegen_destroy(struct gc573_framegen *fg)
{
	if (!fg)
		return;

	if (fg->lock_ready)
		pthread_mutex_lock(&fg->lock);
	fg_slot_free_all_locked(fg);
	gc573_fg_cadence_free(&fg->cadence);
	if (fg->lock_ready)
		pthread_mutex_unlock(&fg->lock);

	if (fg->lock_ready) {
		pthread_mutex_destroy(&fg->lock);
		fg->lock_ready = false;
	}
	if (fg->cond_ready) {
		pthread_cond_destroy(&fg->ready_cond);
		fg->cond_ready = false;
	}
	free(fg);
}

void gc573_framegen_set_config(struct gc573_framegen *fg,
			       const struct gc573_fg_config *cfg)
{
	struct gc573_fg_config next;
	int previous_mode;

	if (!fg || !cfg)
		return;

	next = *cfg;
	if (next.mode < GC573_FG_OFF || next.mode > GC573_FG_30_TO_60)
		next.mode = GC573_FG_OFF;
	next.multiplier = gc573_fg_clamp_multiplier(next.multiplier);
	if (next.flow_scale < 0.5f)
		next.flow_scale = 0.5f;
	if (next.flow_scale > 1.0f)
		next.flow_scale = 1.0f;

	previous_mode = fg->cfg.mode;
	fg->cfg = next;

	if (next.mode != previous_mode) {
		pthread_mutex_lock(&fg->lock);
		for (unsigned int i = 0; i < FG_INPUT_SLOTS; i++)
			fg->slot_used[i] = false;
		fg->head = 0;
		fg->tail = 0;
		fg->count = 0;
		fg->taken_slot = -1;
		fg->has_unread = false;
		fg->in_flight = false;
		gc573_fg_cadence_reset(&fg->cadence);
		fg->source_fast = true;
		fg->prev_raw_ts = 0;
		pthread_mutex_unlock(&fg->lock);
	}
}

void gc573_framegen_get_config(const struct gc573_framegen *fg,
			       struct gc573_fg_config *cfg)
{
	if (!fg || !cfg)
		return;
	*cfg = fg->cfg;
}

void gc573_framegen_set_frame_filter(struct gc573_framegen *fg,
				     gc573_fg_frame_filter fn, void *opaque)
{
	if (!fg)
		return;
	fg->filter = fn;
	fg->filter_opaque = opaque;
}

void gc573_framegen_set_source_hz(struct gc573_framegen *fg,
				  unsigned int hz_x100)
{
	if (!fg)
		return;
	pthread_mutex_lock(&fg->lock);
	fg->source_hz_x100 = hz_x100;
	pthread_mutex_unlock(&fg->lock);
}

void gc573_framegen_set_probe(struct gc573_framegen *fg,
			      const struct gc573_fg_probe *probe)
{
	if (!fg || !probe)
		return;
	pthread_mutex_lock(&fg->lock);
	fg->probe = *probe;
	pthread_mutex_unlock(&fg->lock);
}

void gc573_framegen_set_vulkan_active(struct gc573_framegen *fg, bool active)
{
	if (!fg)
		return;
	pthread_mutex_lock(&fg->lock);
	fg->vulkan_active = active;
	pthread_mutex_unlock(&fg->lock);
}

int gc573_framegen_resize(struct gc573_framegen *fg,
			  uint32_t width, uint32_t height)
{
	size_t bytes;

	if (!fg)
		return -EINVAL;

	width &= ~1U;
	if (width < 16U || height < 16U)
		return -EINVAL;

	bytes = (size_t)width * height * 2U;

	pthread_mutex_lock(&fg->lock);
	if (width == fg->width && height == fg->height && fg->slot_bytes) {
		pthread_mutex_unlock(&fg->lock);
		return 0;
	}

	fg_slot_free_all_locked(fg);
	fg->width = width;
	fg->height = height;
	fg->slot_bytes = bytes;

	for (unsigned int i = 0; i < FG_INPUT_SLOTS; i++) {
		fg->slots[i] = malloc(bytes);
		if (!fg->slots[i]) {
			fg_slot_free_all_locked(fg);
			fg->width = 0;
			fg->height = 0;
			pthread_mutex_unlock(&fg->lock);
			return -ENOMEM;
		}
		fg->slot_used[i] = false;
	}
	fg->head = 0;
	fg->tail = 0;
	fg->count = 0;
	fg->input_dropped = 0;

	gc573_fg_cadence_free(&fg->cadence);
	gc573_fg_cadence_init(&fg->cadence, width, height);
	fg->source_fast = true;
	fg->prev_raw_ts = 0;

	pthread_mutex_unlock(&fg->lock);
	return 0;
}

void gc573_framegen_input(struct gc573_framegen *fg, const uint8_t *yuyv,
			  uint64_t frame_us)
{
	uint64_t now;
	unsigned int slot;
	bool accept = true;

	if (!fg || !yuyv || !fg->lock_ready)
		return;

	now = frame_us ? frame_us : fg_now_us();

	pthread_mutex_lock(&fg->lock);

	if (!fg->slot_bytes || !fg->slots[0]) {
		pthread_mutex_unlock(&fg->lock);
		return;
	}

	fg->raw_win_count++;
	if (!fg->raw_win_start)
		fg->raw_win_start = now;
	if (now > fg->raw_win_start + 1000000ULL) {
		fg->raw_fps = (double)fg->raw_win_count * 1000000.0 /
			      (double)(now - fg->raw_win_start);
		fg->raw_win_start = now;
		fg->raw_win_count = 0;
	}

	fg->source_fast = fg_source_is_fast(fg, now);

	if (fg->source_fast &&
	    (fg->cfg.mode == GC573_FG_30_TO_60 ||
	     fg->cfg.mode == GC573_FG_60_TO_120)) {
		int cls = gc573_fg_cadence_classify(&fg->cadence, yuyv, now);

		if (fg->cfg.mode == GC573_FG_30_TO_60 ||
		    fg->cadence.auto_locked)
			accept = (cls == GC573_FG_UNIQUE);
	}

	if (!accept) {
		pthread_mutex_unlock(&fg->lock);
		return;
	}

	for (slot = 0; slot < FG_INPUT_SLOTS; slot++) {
		if (!fg->slot_used[slot] && fg->slots[slot])
			break;
	}
	if (slot == FG_INPUT_SLOTS) {
		/* Ring full: drop the oldest queued frame. */
		unsigned int old = fg->queue[fg->tail];

		fg->slot_used[old] = false;
		fg->tail = (fg->tail + 1U) % FG_INPUT_SLOTS;
		fg->count--;
		fg->input_dropped++;
		slot = old;
	}

	memcpy(fg->slots[slot], yuyv, fg->slot_bytes);
	if (fg->filter)
		fg->filter(fg->slots[slot], fg->width, fg->height,
			   fg->filter_opaque);

	fg->slot_ts[slot] = now;
	fg->slot_used[slot] = true;
	fg->queue[fg->head] = slot;
	fg->head = (fg->head + 1U) % FG_INPUT_SLOTS;
	fg->count++;
	fg->has_unread = true;

	/* Wake the render thread immediately (PCIe MSI completion is ~now). */
	if (fg->cond_ready)
		pthread_cond_signal(&fg->ready_cond);

	pthread_mutex_unlock(&fg->lock);
}

bool gc573_framegen_take(struct gc573_framegen *fg, const uint8_t **pixels)
{
	uint64_t now;
	unsigned int slot;

	if (!fg || !pixels || !fg->lock_ready)
		return false;

	pthread_mutex_lock(&fg->lock);

	if (fg->count == 0) {
		pthread_mutex_unlock(&fg->lock);
		return false;
	}

	/*
	 * When the presentation loop runs at full speed, count stays 0 or 1.
	 * If presentation slows down, coalesce immediately to the newest frame
	 * so stale frames do not accumulate latency.
	 */
	while (fg->count > 1U) {
		unsigned int old = fg->queue[fg->tail];

		fg->tail = (fg->tail + 1U) % FG_INPUT_SLOTS;
		fg->count--;
		fg->slot_used[old] = false;
		fg->input_dropped++;
	}

	slot = fg->queue[fg->tail];
	fg->tail = (fg->tail + 1U) % FG_INPUT_SLOTS;
	fg->count--;
	fg->taken_slot = (int)slot;
	fg->in_flight = true;
	fg->has_unread = false;
	*pixels = fg->slots[slot];

	/* This take will be exactly one SDL_RenderPresent. */
	now = fg_now_us();
	fg->present_win_count++;
	if (!fg->present_win_start)
		fg->present_win_start = now;
	if (now > fg->present_win_start + 1000000ULL) {
		fg->present_fps = (double)fg->present_win_count * 1000000.0 /
				  (double)(now - fg->present_win_start);
		fg->present_win_start = now;
		fg->present_win_count = 0;
	}

	pthread_mutex_unlock(&fg->lock);
	return true;
}

void gc573_framegen_release(struct gc573_framegen *fg)
{
	if (!fg || !fg->lock_ready)
		return;

	pthread_mutex_lock(&fg->lock);
	if (fg->taken_slot >= 0 && fg->taken_slot < (int)FG_INPUT_SLOTS)
		fg->slot_used[fg->taken_slot] = false;
	fg->taken_slot = -1;
	fg->in_flight = false;
	if (fg->count > 0) {
		fg->has_unread = true;
		if (fg->cond_ready)
			pthread_cond_signal(&fg->ready_cond);
	}
	pthread_mutex_unlock(&fg->lock);
}

bool gc573_framegen_has_unread(struct gc573_framegen *fg)
{
	bool unread;

	if (!fg || !fg->lock_ready)
		return false;

	pthread_mutex_lock(&fg->lock);
	unread = fg->count > 0U;
	pthread_mutex_unlock(&fg->lock);
	return unread;
}

bool gc573_framegen_wait(struct gc573_framegen *fg, unsigned int timeout_us)
{
	struct timespec deadline;
	uint64_t deadline_us;
	bool ready;

	if (!fg || !fg->lock_ready)
		return false;

	pthread_mutex_lock(&fg->lock);
	if (fg->has_unread && !fg->in_flight) {
		pthread_mutex_unlock(&fg->lock);
		return true;
	}

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline_us = (uint64_t)deadline.tv_sec * 1000000ULL +
		      (uint64_t)(deadline.tv_nsec / 1000L) + timeout_us;
	deadline.tv_sec = (time_t)(deadline_us / 1000000ULL);
	deadline.tv_nsec = (long)((deadline_us % 1000000ULL) * 1000ULL);

	while (!fg->has_unread && !fg->in_flight) {
		int err = fg->cond_ready ?
			pthread_cond_timedwait(&fg->ready_cond, &fg->lock,
					       &deadline) : ETIMEDOUT;

		if (err == ETIMEDOUT)
			break;
		/* rc == 0 (or EINTR): re-check the predicate. */
	}

	ready = fg->has_unread && !fg->in_flight;
	pthread_mutex_unlock(&fg->lock);
	return ready;
}

void gc573_framegen_get_stats(struct gc573_framegen *fg,
			      struct gc573_fg_stats *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!fg)
		return;

	pthread_mutex_lock(&fg->lock);

	out->mode = fg->cfg.mode;
	out->multiplier = gc573_fg_clamp_multiplier(fg->cfg.multiplier);
	out->layer_found = fg->probe.layer_found;
	out->dll_found = fg->probe.dll_found;
	snprintf(out->dll_path, sizeof(out->dll_path), "%s", fg->probe.dll_path);
	out->vulkan_active = fg->vulkan_active;
	out->deduplicator_active = fg->source_fast &&
				   (fg->cfg.mode == GC573_FG_30_TO_60 ||
				    fg->cadence.auto_locked);
	out->flow_scale = fg->cfg.flow_scale;
	out->perf_mode = fg->cfg.perf_mode;
	out->low_latency = fg->cfg.low_latency;
	out->present_fps = fg->present_fps;
	out->raw_fps = fg->raw_fps;
	out->unique_fps = gc573_fg_cadence_unique_fps(&fg->cadence);
	out->unique_frames = fg->cadence.unique_frames;
	out->duplicates = fg->cadence.duplicates;
	out->parity_flips = fg->cadence.parity_flips;
	out->input_dropped = fg->input_dropped;
	out->queued = fg->count;

	/*
	 * lsfg-vk only doubles presents it actually intercepts.  Claim 2x only when
	 * the layer, the DLL and the live Vulkan renderer are all present;
	 * otherwise report the raw present rate so the UI can warn instead of lie.
	 */
	if (fg->probe.layer_found && fg->probe.dll_found && fg->vulkan_active)
		out->output_fps = fg->present_fps * (double)out->multiplier;
	else
		out->output_fps = fg->present_fps;

	pthread_mutex_unlock(&fg->lock);
}

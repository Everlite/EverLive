// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc570d-jsonrpc.c — Unified JSON-RPC 2.0 control socket for the GC570D
 *
 * Transport: newline-delimited JSON-RPC 2.0 over AF_UNIX, exactly like the
 * GC573 streamer.  The daemon loop polls the listener and every connected
 * client, then calls gc570d_jsonrpc_service() to drain request lines.  No
 * extra thread is used, so hardware access stays serialized with the
 * watchdog and re-initialization paths.
 */

#include "gc570d-jsonrpc.h"

#include "gc570d.h"
#include "gc570d_everlive.h"
#include "everlive_json.h"
#include "everlive_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RPC_REPLY_MAX	8192
#define RPC_RESULT_MAX	6144
#define RPC_METHOD_MAX	48

static int g_listen_fd = -1;
static int g_lock_fd = -1;
static bool g_bound;
static char g_path[EVERLIVE_IPC_PATH_MAX];
static char g_lock_path[EVERLIVE_IPC_PATH_MAX + 8];
static char g_version[24] = "unknown";
static struct gc570d_dev *g_dev;
static void (*g_stats_fn)(struct gc570d_jsonrpc_stats *out);

/* Per-input FPS estimation from the monotonic frame counters. */
static uint64_t g_last_seq[2];
static uint64_t g_last_fps_ns[2];
static double g_fps[2];

/* Last LED colour applied through this interface. */
static bool g_led_valid;
static unsigned int g_led_r;
static unsigned int g_led_g;
static unsigned int g_led_b;

static uint64_t rpc_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void rpc_write_all(int fd, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = send(fd, data, len, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		data += n;
		len -= (size_t)n;
	}
}

static void rpc_reply(int fd, const char *id, const char *result_json)
{
	char esc_id[EVERLIVE_JSON_ID_MAX * 2];
	char buf[RPC_REPLY_MAX];
	int n;

	everlive_json_format_id(esc_id, sizeof(esc_id), id);
	n = snprintf(buf, sizeof(buf),
		     "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n",
		     esc_id, result_json);
	if (n > 0 && (size_t)n < sizeof(buf))
		rpc_write_all(fd, buf, (size_t)n);
}

static void rpc_error(int fd, const char *id, int code, const char *message)
{
	char esc_id[EVERLIVE_JSON_ID_MAX * 2];
	char esc_msg[256];
	char buf[512];
	int n;

	everlive_json_format_id(esc_id, sizeof(esc_id), id);
	everlive_json_escape_into(esc_msg, sizeof(esc_msg),
				  message ? message : "error");
	n = snprintf(buf, sizeof(buf),
		     "{\"jsonrpc\":\"2.0\",\"id\":%s,"
		     "\"error\":{\"code\":%d,\"message\":\"%s\"}}\n",
		     esc_id, code, esc_msg);
	if (n > 0 && (size_t)n < sizeof(buf))
		rpc_write_all(fd, buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* status.get                                                          */
/* ------------------------------------------------------------------ */

static double rpc_port_fps(int port, uint64_t seq, uint64_t now)
{
	if (g_last_fps_ns[port] && now > g_last_fps_ns[port] &&
	    seq >= g_last_seq[port]) {
		g_fps[port] = (double)(seq - g_last_seq[port]) * 1000000000.0 /
			      (double)(now - g_last_fps_ns[port]);
	}
	g_last_seq[port] = seq;
	g_last_fps_ns[port] = now;
	return g_fps[port];
}

static void rpc_status(int fd, const char *id)
{
	struct gc570d_jsonrpc_stats st;
	struct gc570d_dev *gc = g_dev;
	char result[RPC_RESULT_MAX];
	char path_esc[96];
	uint64_t now = rpc_now_ns();
	int port = gc570d_everlive_port();
	unsigned int src_w = 0;
	unsigned int src_h = 0;
	unsigned int out_w = 0;
	unsigned int out_h = 0;
	unsigned int rate = 0;
	unsigned int audio_rate = 48000;
	unsigned int audio_ch = 2;
	unsigned int led_r = 0;
	unsigned int led_g = 0;
	unsigned int led_b = 0;
	bool streaming = false;
	bool no_signal = true;
	bool interlaced = false;
	double fps;
	int n;

	if (port < 0 || port > 1)
		port = 0;

	memset(&st, 0, sizeof(st));
	if (g_stats_fn)
		g_stats_fn(&st);

	if (gc) {
		if (port == 0) {
			streaming = READ_ONCE(gc->video0_streaming);
			no_signal = READ_ONCE(gc->video0_no_signal);
			interlaced = READ_ONCE(gc->video0_input_interlaced);
			src_w = READ_ONCE(gc->video0_source_width);
			src_h = READ_ONCE(gc->video0_source_height);
			rate = READ_ONCE(gc->video0_source_fps);
			out_w = READ_ONCE(gc->video0_width);
			out_h = READ_ONCE(gc->video0_height);
			audio_rate = READ_ONCE(gc->audio0_rate);
			audio_ch = READ_ONCE(gc->audio0_channels);
			led_r = gc->led_subleds[0].brightness;
			led_g = gc->led_subleds[1].brightness;
			led_b = gc->led_subleds[2].brightness;
		} else {
			streaming = READ_ONCE(gc->video_streaming);
			no_signal = READ_ONCE(gc->video_no_signal);
			interlaced = READ_ONCE(gc->video_input_interlaced);
			src_w = READ_ONCE(gc->video_source_width);
			src_h = READ_ONCE(gc->video_source_height);
			rate = READ_ONCE(gc->video_source_fps);
			out_w = READ_ONCE(gc->video_width);
			out_h = READ_ONCE(gc->video_height);
			audio_rate = READ_ONCE(gc->audio_rate);
			audio_ch = READ_ONCE(gc->audio_channels);
			led_r = gc->led_subleds[0].brightness;
			led_g = gc->led_subleds[1].brightness;
			led_b = gc->led_subleds[2].brightness;
		}
	}

	if (g_led_valid) {
		led_r = g_led_r;
		led_g = g_led_g;
		led_b = g_led_b;
	}

	if (!src_w)
		src_w = out_w ? out_w : GC570D_WIDTH;
	if (!src_h)
		src_h = out_h ? out_h : GC570D_HEIGHT;
	if (!out_w)
		out_w = src_w;
	if (!out_h)
		out_h = src_h;
	if (!rate)
		rate = 60;
	if (!audio_rate)
		audio_rate = 48000;
	if (!audio_ch)
		audio_ch = 2;

	fps = rpc_port_fps(port, st.video_seq[port], now);
	everlive_json_escape_into(path_esc, sizeof(path_esc),
				  gc570d_everlive_v4l2(port));

	n = snprintf(result, sizeof(result),
		     "{\"proto\":1,\"version\":\"%s\",\"pid\":%ld,"
		     "\"headless\":%s,\"streaming\":%s,"
		     "\"signal\":{\"locked\":%s,\"width\":%u,\"height\":%u,"
		     "\"interlaced\":%s,\"rate_hz\":%.2f,"
		     "\"pixel_clock_khz\":0},"
		     "\"video\":{\"fps\":%.2f,\"frames\":%llu,"
		     "\"ring_buffers\":4,\"ring_channels\":4,"
		     "\"output_width\":%u,\"output_height\":%u},"
		     "\"audio\":{\"state\":\"lpcm\",\"label\":\"L-PCM 48kHz\","
		     "\"rate_hz\":%u,\"channels\":%u,\"out_channels\":%u,"
		     "\"layout\":\"stereo\",\"bits\":16,"
		     "\"gain\":%.3f,\"muted\":%s},"
		     "\"v4l2\":{\"path\":\"%s\",\"configured\":%s,"
		     "\"width\":%u,\"height\":%u,\"frames\":%llu,"
		     "\"dropped\":%llu},"
		     "\"osd\":{\"enabled\":%s,\"position\":\"%s\"},"
		     "\"aspect\":\"%s\",\"scale\":\"%s\","
		     "\"vsync\":false,\"fullscreen\":false,"
		     "\"framegen\":{\"mode\":\"off\",\"present_fps\":0.00,"
		     "\"output_fps\":0.00,\"unique_fps\":0.00,"
		     "\"raw_fps\":0.00,\"flow_scale\":1.00,"
		     "\"perf_mode\":false,\"low_latency\":false,"
		     "\"layer_found\":false,\"dll_found\":false,"
		     "\"dll_path\":\"\",\"vulkan_active\":false,"
		     "\"deduplicator_active\":false},"
		     "\"led\":{\"r\":%u,\"g\":%u,\"b\":%u},"
		     "\"clients\":1,\"age_ms\":0}",
		     g_version, (long)getpid(),
		     gc570d_everlive_headless() ? "true" : "false",
		     streaming ? "true" : "false",
		     (streaming && !no_signal) ? "true" : "false",
		     src_w, src_h, interlaced ? "true" : "false",
		     (double)rate,
		     fps, (unsigned long long)st.video_seq[port],
		     out_w, out_h,
		     audio_rate, audio_ch, audio_ch,
		     (double)gc570d_everlive_volume() / 100.0,
		     gc570d_everlive_muted() ? "true" : "false",
		     path_esc, st.v4l2_configured[port] ? "true" : "false",
		     out_w, out_h,
		     (unsigned long long)st.v4l2_frames[port],
		     (unsigned long long)st.v4l2_dropped[port],
		     gc570d_everlive_osd() ? "true" : "false",
		     gc570d_everlive_osd_pos(),
		     gc570d_everlive_aspect(), gc570d_everlive_scale(),
		     led_r, led_g, led_b);

	if (n > 0 && (size_t)n < sizeof(result))
		rpc_reply(fd, id, result);
	else
		rpc_error(fd, id, -32603, "status serialization failed");
}

/* ------------------------------------------------------------------ */
/* Request handlers                                                    */
/* ------------------------------------------------------------------ */

static void rpc_ok(int fd, const char *id)
{
	rpc_reply(fd, id, "{\"ok\":true}");
}

static int rpc_require_dev(int fd, const char *id)
{
	if (g_dev)
		return 0;
	rpc_error(fd, id, -32603, "capture hardware unavailable");
	return -1;
}

static void rpc_hello(int fd, const char *id)
{
	char result[640];
	char path_esc[128];
	int n;

	everlive_json_escape_into(path_esc, sizeof(path_esc), g_path);
	n = snprintf(result, sizeof(result),
		     "{\"proto\":1,\"app\":\"everlive\",\"version\":\"%s\","
		     "\"pid\":%ld,\"socket\":\"%s\","
		     "\"methods\":[\"hello\",\"ping\",\"status.get\","
		     "\"led.set\",\"led.mode.set\",\"audio.volume.set\","
		     "\"audio.mute.set\",\"audio.channels.set\","
		     "\"osd.set\",\"osd.position.set\",\"aspect.set\","
		     "\"scale.set\",\"input.set\",\"pip.set\","
		     "\"hotplug\",\"edid.set\"]}",
		     g_version, (long)getpid(), path_esc);
	if (n > 0 && (size_t)n < sizeof(result))
		rpc_reply(fd, id, result);
	else
		rpc_error(fd, id, -32603, "hello serialization failed");
}

static void rpc_led_set(int fd, const char *id, const char *line)
{
	long r, g, b;
	int ret;

	if (rpc_require_dev(fd, id) < 0)
		return;
	if (!everlive_json_get_long(line, "r", &r) ||
	    !everlive_json_get_long(line, "g", &g) ||
	    !everlive_json_get_long(line, "b", &b)) {
		rpc_error(fd, id, -32602, "invalid params: r, g, b required");
		return;
	}

	r = clamp_t(long, r, 0L, 255L);
	g = clamp_t(long, g, 0L, 255L);
	b = clamp_t(long, b, 0L, 255L);

	ret = gc570d_led_set_rgb(g_dev, (u8)r, (u8)g, (u8)b);
	if (ret) {
		rpc_error(fd, id, -32603, "failed to apply LED colour");
		return;
	}

	g_led_r = (unsigned int)r;
	g_led_g = (unsigned int)g;
	g_led_b = (unsigned int)b;
	g_led_valid = true;
	rpc_ok(fd, id);
}

static void rpc_led_mode(int fd, const char *id, const char *line)
{
	char mode[24];
	const char *effect;
	int ret;

	if (rpc_require_dev(fd, id) < 0)
		return;
	if (!everlive_json_get_string(line, "mode", mode, sizeof(mode))) {
		rpc_error(fd, id, -32602, "invalid params: mode required");
		return;
	}

	if (!strcasecmp(mode, "off") || !strcasecmp(mode, "dark"))
		effect = "off";
	else if (!strcasecmp(mode, "static") || !strcasecmp(mode, "solid"))
		effect = "static";
	else if (!strcasecmp(mode, "breathing") || !strcasecmp(mode, "breathe") ||
		 !strcasecmp(mode, "wave"))
		effect = "breathing";
	else if (!strcasecmp(mode, "colorful") || !strcasecmp(mode, "rainbow") ||
		 !strcasecmp(mode, "cycle"))
		effect = "colorful";
	else {
		rpc_error(fd, id, -32602,
			  "unknown mode: off|static|breathing|colorful|wave|rainbow");
		return;
	}

	ret = gc570d_led_set_effect(g_dev, effect);
	if (ret) {
		rpc_error(fd, id, -32603, "failed to apply LED effect");
		return;
	}
	g_led_valid = false;
	rpc_ok(fd, id);
}

static void rpc_audio_volume(int fd, const char *id, const char *line)
{
	double gain;
	int pct;

	if (!everlive_json_get_double(line, "gain", &gain)) {
		rpc_error(fd, id, -32602, "invalid params: gain required");
		return;
	}
	if (gain < 0.0)
		gain = 0.0;
	if (gain > 1.5)
		gain = 1.5;

	pct = (int)(gain * 100.0 + 0.5);
	gc570d_everlive_set_volume(pct);
	rpc_ok(fd, id);
}

static void rpc_audio_mute(int fd, const char *id, const char *line)
{
	bool muted;

	if (!everlive_json_get_bool(line, "muted", &muted)) {
		rpc_error(fd, id, -32602, "invalid params: muted required");
		return;
	}
	gc570d_everlive_set_mute(muted);
	rpc_ok(fd, id);
}

static void rpc_audio_channels(int fd, const char *id, const char *line)
{
	char mode[16];

	if (!everlive_json_get_string(line, "mode", mode, sizeof(mode))) {
		rpc_error(fd, id, -32602, "invalid params: mode required");
		return;
	}
	if (strcasecmp(mode, "auto") && strcasecmp(mode, "stereo") &&
	    strcasecmp(mode, "2") && strcasecmp(mode, "2.0")) {
		rpc_error(fd, id, -32602,
			  "GC570D captures LPCM stereo only (auto|stereo)");
		return;
	}
	rpc_ok(fd, id);
}

static void rpc_osd(int fd, const char *id, const char *line)
{
	bool enabled;

	if (!everlive_json_get_bool(line, "enabled", &enabled)) {
		rpc_error(fd, id, -32602, "invalid params: enabled required");
		return;
	}
	gc570d_everlive_set_osd(enabled);
	rpc_ok(fd, id);
}

static void rpc_osd_pos(int fd, const char *id, const char *line)
{
	char pos[16];

	if (!everlive_json_get_string(line, "pos", pos, sizeof(pos))) {
		rpc_error(fd, id, -32602, "invalid params: pos required");
		return;
	}
	if (strcasecmp(pos, "tr") && strcasecmp(pos, "br") &&
	    strcasecmp(pos, "bl") && strcasecmp(pos, "tl") &&
	    strcasecmp(pos, "top-right") && strcasecmp(pos, "bottom-right") &&
	    strcasecmp(pos, "bottom-left") && strcasecmp(pos, "top-left")) {
		rpc_error(fd, id, -32602, "unknown pos: tr|br|bl|tl");
		return;
	}
	gc570d_everlive_set_osd_pos(pos);
	rpc_ok(fd, id);
}

static void rpc_aspect(int fd, const char *id, const char *line)
{
	char mode[24];

	if (!everlive_json_get_string(line, "mode", mode, sizeof(mode))) {
		rpc_error(fd, id, -32602, "invalid params: mode required");
		return;
	}
	if (strcasecmp(mode, "auto") && strcasecmp(mode, "16:9") &&
	    strcasecmp(mode, "16/9") && strcasecmp(mode, "4:3") &&
	    strcasecmp(mode, "4/3") && strcasecmp(mode, "stretch") &&
	    strcasecmp(mode, "fill")) {
		rpc_error(fd, id, -32602,
			  "unknown mode: auto|16:9|4:3|stretch");
		return;
	}
	gc570d_everlive_set_aspect(mode);
	rpc_ok(fd, id);
}

static void rpc_scale(int fd, const char *id, const char *line)
{
	char mode[16];

	if (!everlive_json_get_string(line, "mode", mode, sizeof(mode))) {
		rpc_error(fd, id, -32602, "invalid params: mode required");
		return;
	}
	if (strcasecmp(mode, "fit") && strcasecmp(mode, "aspect") &&
	    strcasecmp(mode, "integer") && strcasecmp(mode, "nearest")) {
		rpc_error(fd, id, -32602, "unknown mode: fit|integer");
		return;
	}
	gc570d_everlive_set_scale(mode);
	rpc_ok(fd, id);
}

static void rpc_input(int fd, const char *id, const char *line)
{
	long port;

	if (!everlive_json_get_long(line, "port", &port)) {
		rpc_error(fd, id, -32602, "invalid params: port required");
		return;
	}
	/*
	 * Canonical values are 1-based (matching gc570d-ctl): 1 = HDMI 1,
	 * 2 = HDMI 2.  0 is also accepted as HDMI 1 for 0-based clients.
	 */
	if (port < 0 || port > 2) {
		rpc_error(fd, id, -32602, "port must be 1 or 2");
		return;
	}
	gc570d_everlive_set_port(port == 2 ? 1 : 0);
	rpc_ok(fd, id);
}

static void rpc_pip(int fd, const char *id, const char *line)
{
	bool enabled;

	if (!everlive_json_get_bool(line, "enabled", &enabled)) {
		rpc_error(fd, id, -32602, "invalid params: enabled required");
		return;
	}
	gc570d_everlive_set_pip(enabled);
	rpc_ok(fd, id);
}

static void rpc_hotplug(int fd, const char *id, const char *line)
{
	long port = 0;
	const char *node;
	int ret;

	if (!everlive_json_get_long(line, "port", &port))
		port = 1;
	if (port < 0 || port > 2) {
		rpc_error(fd, id, -32602, "port must be 1 or 2");
		return;
	}

	node = (port == 2) ? "it6802_hpd" : "it68051_hpd";
	ret = gc570d_user_diag_action(node, "1");
	if (ret < 0) {
		rpc_error(fd, id, -32603, "HPD pulse failed");
		return;
	}
	rpc_ok(fd, id);
}

static void rpc_edid(int fd, const char *id, const char *line)
{
	char profile[32];
	const char *node;
	int ret;

	if (!everlive_json_get_string(line, "profile", profile,
				      sizeof(profile))) {
		rpc_error(fd, id, -32602, "invalid params: profile required");
		return;
	}

	if (!strcasecmp(profile, "default") || !strcasecmp(profile, "bridge") ||
	    !strcasecmp(profile, "internal"))
		node = "splitter_windows_bridge_edid";
	else if (!strcasecmp(profile, "copy") ||
		 !strcasecmp(profile, "passthrough") ||
		 !strcasecmp(profile, "downstream"))
		node = "splitter_windows_edid_copy";
	else {
		rpc_error(fd, id, -32602,
			  "unknown profile: default|bridge|copy|passthrough");
		return;
	}

	ret = gc570d_user_diag_action(node, "1");
	if (ret < 0) {
		rpc_error(fd, id, -32603, "EDID switch failed");
		return;
	}
	rpc_ok(fd, id);
}

static bool rpc_is_unsupported(const char *method)
{
	static const char *const unsupported[] = {
		"mode.set", "v4l2.set", "hdr.set", "hdr.passthrough",
		"hdr.exposure", "framegen.set", "toast.show",
	};

	for (size_t i = 0; i < ARRAY_SIZE(unsupported); i++) {
		if (strcmp(method, unsupported[i]) == 0)
			return true;
	}
	return false;
}

static void rpc_dispatch(int fd, const char *id, const char *method,
			 const char *line)
{
	if (strcmp(method, "hello") == 0)
		rpc_hello(fd, id);
	else if (strcmp(method, "ping") == 0)
		rpc_reply(fd, id, "\"pong\"");
	else if (strcmp(method, "status.get") == 0)
		rpc_status(fd, id);
	else if (strcmp(method, "led.set") == 0)
		rpc_led_set(fd, id, line);
	else if (strcmp(method, "led.mode.set") == 0)
		rpc_led_mode(fd, id, line);
	else if (strcmp(method, "audio.volume.set") == 0)
		rpc_audio_volume(fd, id, line);
	else if (strcmp(method, "audio.mute.set") == 0)
		rpc_audio_mute(fd, id, line);
	else if (strcmp(method, "audio.channels.set") == 0)
		rpc_audio_channels(fd, id, line);
	else if (strcmp(method, "osd.set") == 0)
		rpc_osd(fd, id, line);
	else if (strcmp(method, "osd.position.set") == 0)
		rpc_osd_pos(fd, id, line);
	else if (strcmp(method, "aspect.set") == 0)
		rpc_aspect(fd, id, line);
	else if (strcmp(method, "scale.set") == 0)
		rpc_scale(fd, id, line);
	else if (strcmp(method, "input.set") == 0)
		rpc_input(fd, id, line);
	else if (strcmp(method, "pip.set") == 0)
		rpc_pip(fd, id, line);
	else if (strcmp(method, "hotplug") == 0)
		rpc_hotplug(fd, id, line);
	else if (strcmp(method, "edid.set") == 0)
		rpc_edid(fd, id, line);
	else if (rpc_is_unsupported(method))
		rpc_error(fd, id, -32601,
			  "method not supported on the GC570D engine");
	else
		rpc_error(fd, id, -32601, "method not found");
}

static void rpc_handle_request(int fd, const char *line)
{
	char id[EVERLIVE_JSON_ID_MAX];
	char method[RPC_METHOD_MAX];
	bool has_id;
	const char *rid;

	has_id = everlive_json_find_value(line, "id", id, sizeof(id)) &&
		 strcmp(id, "null") != 0;
	rid = has_id ? id : NULL;

	if (!everlive_json_get_string(line, "method", method, sizeof(method))) {
		if (has_id)
			rpc_error(fd, rid, -32700,
				  "parse error: missing method");
		return;
	}

	rpc_dispatch(fd, rid, method, line);
}

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

#define RPC_CLIENT_IN_MAX 8192

struct rpc_client {
	int fd;
	size_t inlen;
	bool active;
	char in[RPC_CLIENT_IN_MAX];
};

static struct rpc_client g_clients[GC570D_JSONRPC_MAX_CLIENTS];

static struct rpc_client *rpc_client_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_clients); i++) {
		if (!g_clients[i].active)
			return &g_clients[i];
	}
	return NULL;
}

static void rpc_client_close(struct rpc_client *c)
{
	if (!c->active)
		return;
	close(c->fd);
	c->fd = -1;
	c->active = false;
	c->inlen = 0;
}

static void rpc_client_close_all(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_clients); i++)
		rpc_client_close(&g_clients[i]);
}

static bool rpc_peer_ok(int fd)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
		return false;
	return cred.uid == getuid();
}

static void rpc_drain_lines(struct rpc_client *c)
{
	size_t consumed = 0;

	while (consumed < c->inlen) {
		char *line = c->in + consumed;
		size_t avail = c->inlen - consumed;
		char *nl = memchr(line, '\n', avail);
		size_t line_len;
		size_t total;

		if (!nl)
			break;
		line_len = (size_t)(nl - line);
		total = line_len + 1;
		*nl = '\0';
		if (line_len && line[line_len - 1] == '\r')
			line[--line_len] = '\0';
		if (line_len)
			rpc_handle_request(c->fd, line);
		consumed += total;
		if (!c->active)
			return;
	}

	if (consumed) {
		memmove(c->in, c->in + consumed, c->inlen - consumed);
		c->inlen -= consumed;
	}
	c->in[c->inlen] = '\0';
}

static void rpc_client_read(struct rpc_client *c)
{
	for (;;) {
		size_t space = sizeof(c->in) - c->inlen - 1;
		ssize_t n;

		if (space == 0) {
			rpc_error(c->fd, NULL, -32000, "frame too large");
			rpc_client_close(c);
			return;
		}

		n = recv(c->fd, c->in + c->inlen, space, MSG_DONTWAIT);
		if (n == 0) {
			rpc_client_close(c);
			return;
		}
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			rpc_client_close(c);
			return;
		}

		c->inlen += (size_t)n;
		c->in[c->inlen] = '\0';
		rpc_drain_lines(c);
		if (!c->active)
			return;
	}
}

int gc570d_jsonrpc_fill_pollfds(struct pollfd *fds, int max)
{
	int n = 0;

	if (!fds || max <= 0)
		return 0;

	if (g_listen_fd >= 0 && n < max) {
		fds[n].fd = g_listen_fd;
		fds[n].events = POLLIN;
		n++;
	}

	for (size_t i = 0; i < ARRAY_SIZE(g_clients) && n < max; i++) {
		if (!g_clients[i].active)
			continue;
		fds[n].fd = g_clients[i].fd;
		fds[n].events = POLLIN;
		n++;
	}

	return n;
}

void gc570d_jsonrpc_service(void)
{
	struct pollfd pfd[1 + GC570D_JSONRPC_MAX_CLIENTS];
	int map[1 + GC570D_JSONRPC_MAX_CLIENTS];
	int n = 0;

	if (g_listen_fd >= 0) {
		pfd[n].fd = g_listen_fd;
		pfd[n].events = POLLIN;
		map[n] = -1;
		n++;
	}

	for (size_t i = 0; i < ARRAY_SIZE(g_clients); i++) {
		if (!g_clients[i].active)
			continue;
		pfd[n].fd = g_clients[i].fd;
		pfd[n].events = POLLIN;
		map[n] = (int)i;
		n++;
	}

	if (n == 0)
		return;
	if (poll(pfd, (nfds_t)n, 0) <= 0)
		return;

	for (int i = 0; i < n; i++) {
		if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;

		if (map[i] < 0) {
			for (;;) {
				struct rpc_client *c;
				int cfd = accept4(g_listen_fd, NULL, NULL,
						  SOCK_NONBLOCK | SOCK_CLOEXEC);

				if (cfd < 0) {
					if (errno == EINTR)
						continue;
					break;
				}
				c = rpc_client_alloc();
				if (!c || !rpc_peer_ok(cfd)) {
					close(cfd);
					continue;
				}
				c->fd = cfd;
				c->inlen = 0;
				c->in[0] = '\0';
				c->active = true;
			}
		} else {
			rpc_client_read(&g_clients[map[i]]);
		}
	}
}

int gc570d_jsonrpc_init(const char *version)
{
	struct sockaddr_un addr;
	int ret;

	if (g_listen_fd >= 0)
		return 0;

	snprintf(g_version, sizeof(g_version), "%s",
		 version && *version ? version : "unknown");

	ret = everlive_ipc_prepare_socket_path(g_path, sizeof(g_path), NULL);
	if (ret < 0)
		return ret;

	snprintf(g_lock_path, sizeof(g_lock_path), "%.*s.lock",
		 EVERLIVE_IPC_PATH_MAX - 1, g_path);

	g_lock_fd = open(g_lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (g_lock_fd < 0) {
		ret = -errno;
		goto fail;
	}
	if (flock(g_lock_fd, LOCK_EX | LOCK_NB) < 0) {
		ret = (errno == EWOULDBLOCK) ? -EADDRINUSE : -errno;
		goto fail;
	}

	/* A leftover socket without a live lock owner is stale. */
	if (access(g_path, F_OK) == 0)
		(void)unlink(g_path);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, g_path, strlen(g_path) + 1);

	g_listen_fd = socket(AF_UNIX,
			     SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (g_listen_fd < 0) {
		ret = -errno;
		goto fail;
	}
	if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ret = -errno;
		goto fail;
	}
	g_bound = true;
	(void)chmod(g_path, 0600);
	if (listen(g_listen_fd, GC570D_JSONRPC_MAX_CLIENTS) < 0) {
		ret = -errno;
		goto fail;
	}

	return 0;

fail:
	gc570d_jsonrpc_shutdown();
	return ret;
}

void gc570d_jsonrpc_shutdown(void)
{
	rpc_client_close_all();

	if (g_listen_fd >= 0) {
		close(g_listen_fd);
		g_listen_fd = -1;
	}
	if (g_path[0] && g_bound)
		(void)unlink(g_path);
	g_path[0] = '\0';
	g_bound = false;
	if (g_lock_fd >= 0) {
		(void)flock(g_lock_fd, LOCK_UN);
		close(g_lock_fd);
		g_lock_fd = -1;
	}
	g_lock_path[0] = '\0';
}

int gc570d_jsonrpc_listen_fd(void)
{
	return g_listen_fd;
}

const char *gc570d_jsonrpc_socket_path(void)
{
	return g_path;
}

void gc570d_jsonrpc_set_stats_fn(void (*fn)(struct gc570d_jsonrpc_stats *out))
{
	g_stats_fn = fn;
}

void gc570d_jsonrpc_set_dev(struct gc570d_dev *gc)
{
	g_dev = gc;
}

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_ipc.c — Unix control socket for the EverLive userspace streamer
 *
 * Design notes:
 *  - The control thread owns the socket, the client table and all JSON
 *    handling.  It never touches VFIO, SDL or the DMA ring.
 *  - The main loop publishes a stats snapshot through a seqlock; readers retry
 *    until they observe an even, stable sequence counter.
 *  - Requests are flat JSON objects, so a small scanner is enough.  A full
 *    JSON parser would add a dependency for no benefit; unknown fields are
 *    ignored and the raw "id" token is echoed back verbatim.
 *  - Replies are small (< 4 KiB) and the peer is always local, so a single
 *    send() loop is sufficient.  A client that cannot take its reply is dropped
 *    on its next read instead of being queued indefinitely.
 */

#include "gc573-user-compat.h"
#include "gc573_user_ipc.h"

#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>

#define IPC_DEFAULT_NAME	"everlive.sock"
#define IPC_PATH_LIMIT		100U	/* stay clear of sun_path[108] */
#define IPC_REPLY_MAX		3072U
#define IPC_METHOD_MAX		48U
#define IPC_ID_MAX		48U

struct ipc_client {
	int fd;
	bool active;
	bool hello_done;
	size_t inlen;
	char inbuf[GC573_IPC_MAX_FRAME];
};

struct ipc_server {
	int listen_fd;
	int epoll_fd;
	int stop_fd;
	int lock_fd;
	pthread_t thread;
	int running;			/* accessed with __atomic builtins */
	bool thread_started;
	char path[108];
	char lock_path[120];
	char version[24];
	struct ipc_client clients[GC573_IPC_MAX_CLIENTS];
};

static struct ipc_server g_ipc = {
	.listen_fd = -1,
	.epoll_fd = -1,
	.stop_fd = -1,
	.lock_fd = -1,
};

/* Published by the main loop, read by the control thread. */
static struct {
	unsigned int seq;
	struct gc573_stats data;
} g_snapshot;

/*
 * Mutations travel from the control thread (producer) to the render loop
 * (consumer) through this lock-free SPSC ring.  Neither side ever blocks.
 */
static struct {
	struct gc573_ipc_cmd ring[GC573_IPC_CMD_RING];
	unsigned int head;
	unsigned int tail;
} g_cmdq;

static bool ipc_cmd_push(const struct gc573_ipc_cmd *cmd)
{
	unsigned int head = __atomic_load_n(&g_cmdq.head, __ATOMIC_RELAXED);
	unsigned int next = (head + 1U) % GC573_IPC_CMD_RING;

	if (next == __atomic_load_n(&g_cmdq.tail, __ATOMIC_ACQUIRE))
		return false;	/* ring full: the render loop is behind */

	g_cmdq.ring[head] = *cmd;
	__atomic_store_n(&g_cmdq.head, next, __ATOMIC_RELEASE);
	return true;
}

bool gc573_ipc_poll_cmd(struct gc573_ipc_cmd *out)
{
	unsigned int tail = __atomic_load_n(&g_cmdq.tail, __ATOMIC_RELAXED);

	if (tail == __atomic_load_n(&g_cmdq.head, __ATOMIC_ACQUIRE))
		return false;

	*out = g_cmdq.ring[tail];
	__atomic_store_n(&g_cmdq.tail,
			 (tail + 1U) % GC573_IPC_CMD_RING, __ATOMIC_RELEASE);
	return true;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t ipc_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)(ts.tv_nsec / 1000000L);
}

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

/*
 * Copy a JSON value token from @p into @out.  Strings keep their quotes, so
 * the result can be embedded in a reply unchanged; numbers, booleans and null
 * are copied up to the next structural character.
 */
static bool json_copy_token(const char *p, char *out, size_t outsz)
{
	const char *start = p;
	const char *end;

	if (*p == '"') {
		bool esc = false;

		p++;
		while (*p) {
			if (esc) {
				esc = false;
				p++;
				continue;
			}
			if (*p == '\\') {
				esc = true;
				p++;
				continue;
			}
			if (*p == '"') {
				p++;
				break;
			}
			p++;
		}
		end = p;
	} else {
		while (*p && *p != ',' && *p != '}' && *p != ']' &&
		       *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
			p++;
		end = p;
	}

	size_t len = (size_t)(end - start);

	if (len == 0 || len >= outsz)
		return false;
	memcpy(out, start, len);
	out[len] = '\0';
	return true;
}

/*
 * Find "key": <value> in a flat JSON object and copy the raw value token.
 * Key names are matched only in key position, so a string value containing the
 * key name is never mistaken for the key itself.
 */
static bool json_find_value(const char *s, const char *key, char *out,
			    size_t outsz)
{
	const size_t klen = strlen(key);
	const char *p = s;

	while (*p) {
		const char *start;
		const char *end;
		bool esc = false;

		if (*p != '"') {
			if (*p == '\\' && p[1]) {
				p += 2;
				continue;
			}
			p++;
			continue;
		}

		start = p + 1;
		end = start;
		while (*end) {
			if (esc) {
				esc = false;
				end++;
				continue;
			}
			if (*end == '\\') {
				esc = true;
				end++;
				continue;
			}
			if (*end == '"')
				break;
			end++;
		}
		if (!*end)
			return false;	/* unterminated string */

		if ((size_t)(end - start) == klen &&
		    strncmp(start, key, klen) == 0) {
			const char *q = skip_ws(end + 1);

			if (*q == ':')
				return json_copy_token(skip_ws(q + 1), out, outsz);
		}
		p = end + 1;
	}
	return false;
}

/* Same, but requires a plain JSON string and strips the quotes. */
static bool json_get_string(const char *s, const char *key, char *out,
			    size_t outsz)
{
	char token[192];
	size_t len;

	if (!json_find_value(s, key, token, sizeof(token)))
		return false;
	if (token[0] != '"')
		return false;
	len = strlen(token);
	if (len < 2 || token[len - 1] != '"')
		return false;
	if (len - 1 > outsz)
		return false;
	memcpy(out, token + 1, len - 2);
	out[len - 2] = '\0';
	return true;
}

static bool json_get_long(const char *s, const char *key, long *out)
{
	char token[32];
	char *end;
	long value;

	if (!json_find_value(s, key, token, sizeof(token)))
		return false;
	if (token[0] == '"' || token[0] == '\0')
		return false;

	errno = 0;
	value = strtol(token, &end, 10);
	if (errno || end == token || *end != '\0')
		return false;
	*out = value;
	return true;
}

static bool json_get_double(const char *s, const char *key, double *out)
{
	char token[48];
	char *end;
	double value;

	if (!json_find_value(s, key, token, sizeof(token)))
		return false;
	if (token[0] == '"' || token[0] == '\0')
		return false;

	errno = 0;
	value = strtod(token, &end);
	if (errno || end == token || *end != '\0')
		return false;
	*out = value;
	return true;
}

static bool json_get_bool(const char *s, const char *key, bool *out)
{
	char token[16];

	if (!json_find_value(s, key, token, sizeof(token)))
		return false;
	if (strcmp(token, "true") == 0) {
		*out = true;
		return true;
	}
	if (strcmp(token, "false") == 0) {
		*out = false;
		return true;
	}
	return false;
}

static void json_escape_into(char *dst, size_t dstsz, const char *src)
{
	size_t o = 0;

	if (dstsz == 0)
		return;
	while (src && *src && o + 2 < dstsz) {
		unsigned char ch = (unsigned char)*src++;

		if (ch == '"' || ch == '\\') {
			dst[o++] = '\\';
			dst[o++] = (char)ch;
		} else if (ch < 0x20) {
			if (o + 6 >= dstsz)
				break;
			o += (size_t)snprintf(dst + o, dstsz - o, "\\u%04x", ch);
		} else {
			dst[o++] = (char)ch;
		}
	}
	dst[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Wire I/O                                                            */
/* ------------------------------------------------------------------ */

static void ipc_send_raw(struct ipc_client *c, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = send(c->fd, data, len, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return;	/* peer is gone or its window is full */
		}
		data += n;
		len -= (size_t)n;
	}
}

static void ipc_reply(struct ipc_client *c, const char *id,
		      const char *result_json)
{
	char buf[IPC_REPLY_MAX];
	int n = snprintf(buf, sizeof(buf),
			 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n",
			 id ? id : "null", result_json);

	if (n > 0 && (size_t)n < sizeof(buf))
		ipc_send_raw(c, buf, (size_t)n);
}

static void ipc_error(struct ipc_client *c, const char *id, int code,
		      const char *message)
{
	char buf[256];
	int n = snprintf(buf, sizeof(buf),
			 "{\"jsonrpc\":\"2.0\",\"id\":%s,"
			 "\"error\":{\"code\":%d,\"message\":\"%s\"}}\n",
			 id ? id : "null", code, message);

	if (n > 0 && (size_t)n < sizeof(buf))
		ipc_send_raw(c, buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Client table                                                        */
/* ------------------------------------------------------------------ */

static struct ipc_client *ipc_find_client(struct ipc_server *s, int fd)
{
	for (unsigned int i = 0; i < GC573_IPC_MAX_CLIENTS; i++) {
		if (s->clients[i].active && s->clients[i].fd == fd)
			return &s->clients[i];
	}
	return NULL;
}

static struct ipc_client *ipc_free_slot(struct ipc_server *s)
{
	for (unsigned int i = 0; i < GC573_IPC_MAX_CLIENTS; i++) {
		if (!s->clients[i].active)
			return &s->clients[i];
	}
	return NULL;
}

static void ipc_close_client(struct ipc_server *s, struct ipc_client *c)
{
	if (!c->active)
		return;
	if (s->epoll_fd >= 0)
		(void)epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	c->fd = -1;
	__atomic_store_n(&c->active, false, __ATOMIC_RELEASE);
	c->hello_done = false;
	c->inlen = 0;
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

static void ipc_snapshot_read(struct gc573_stats *out)
{
	for (;;) {
		unsigned int s1, s2;

		s1 = __atomic_load_n(&g_snapshot.seq, __ATOMIC_ACQUIRE);
		if (s1 & 1U)
			continue;	/* writer active, retry */
		*out = g_snapshot.data;
		s2 = __atomic_load_n(&g_snapshot.seq, __ATOMIC_ACQUIRE);
		if (s1 == s2)
			return;
	}
}

void gc573_ipc_publish_stats(const struct gc573_stats *stats)
{
	unsigned int seq = __atomic_load_n(&g_snapshot.seq, __ATOMIC_RELAXED);

	__atomic_store_n(&g_snapshot.seq, seq + 1U, __ATOMIC_RELEASE);
	g_snapshot.data = *stats;
	__atomic_store_n(&g_snapshot.seq, seq + 2U, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* Request handling                                                    */
/* ------------------------------------------------------------------ */

static void ipc_send_status(struct ipc_server *s, struct ipc_client *c,
			    const char *id)
{
	struct gc573_stats st = { 0 };
	char result[IPC_REPLY_MAX];
	char path_esc[96];
	char label_esc[64];
	char pos_esc[24];
	char aspect_esc[24];
	uint64_t age_ms;
	int n;

	ipc_snapshot_read(&st);
	age_ms = st.timestamp_ms ? ipc_now_ms() - st.timestamp_ms : 0;

	json_escape_into(path_esc, sizeof(path_esc), st.v4l2_path);
	json_escape_into(label_esc, sizeof(label_esc), st.audio_label);
	json_escape_into(pos_esc, sizeof(pos_esc), st.osd_position);
	json_escape_into(aspect_esc, sizeof(aspect_esc), st.aspect);

	n = snprintf(result, sizeof(result),
		     "{\"proto\":%d,\"version\":\"%s\",\"pid\":%ld,"
		     "\"headless\":%s,\"streaming\":%s,"
		     "\"signal\":{\"locked\":%s,\"width\":%u,\"height\":%u,"
		     "\"interlaced\":%s,\"rate_hz\":%.2f,"
		     "\"pixel_clock_khz\":%u},"
		     "\"video\":{\"fps\":%.2f,\"frames\":%llu,"
		     "\"ring_buffers\":%u,\"ring_channels\":%u,"
		     "\"output_width\":%u,\"output_height\":%u},"
		     "\"audio\":{\"state\":\"%s\",\"label\":\"%s\","
		     "\"rate_hz\":%u,\"channels\":%u,\"bits\":%u,"
		     "\"gain\":%.3f,\"muted\":%s},"
		     "\"v4l2\":{\"path\":\"%s\",\"configured\":%s,"
		     "\"width\":%u,\"height\":%u,\"frames\":%llu,"
		     "\"dropped\":%llu},"
		     "\"osd\":{\"enabled\":%s,\"position\":\"%s\"},"
		     "\"aspect\":\"%s\",\"vsync\":%s,\"fullscreen\":%s,"
		     "\"led\":{\"r\":%u,\"g\":%u,\"b\":%u},"
		     "\"clients\":%u,\"age_ms\":%llu}",
		     GC573_IPC_PROTO_VERSION, s->version, (long)getpid(),
		     st.headless ? "true" : "false",
		     st.streaming ? "true" : "false",
		     st.signal_locked ? "true" : "false",
		     st.width, st.height,
		     st.interlaced ? "true" : "false",
		     (double)st.rate_x100 / 100.0, st.pixel_clock_khz,
		     st.fps, (unsigned long long)st.frames,
		     st.ring_buffers, st.ring_channels,
		     st.output_width, st.output_height,
		     st.audio_state, label_esc,
		     st.audio_rate, st.audio_channels, st.audio_bits,
		     (double)st.audio_gain,
		     st.audio_muted ? "true" : "false",
		     path_esc, st.v4l2_configured ? "true" : "false",
		     st.v4l2_width, st.v4l2_height,
		     (unsigned long long)st.v4l2_frames,
		     (unsigned long long)st.v4l2_dropped,
		     st.osd_enabled ? "true" : "false", pos_esc,
		     aspect_esc, st.vsync ? "true" : "false",
		     st.fullscreen ? "true" : "false",
		     st.led_r, st.led_g, st.led_b,
		     (unsigned int)gc573_ipc_has_clients(),
		     (unsigned long long)age_ms);

	if (n > 0 && (size_t)n < sizeof(result))
		ipc_reply(c, id, result);
	else
		ipc_error(c, id, -32603, "status serialization failed");
}

static void ipc_send_hello(struct ipc_server *s, struct ipc_client *c,
			   const char *id)
{
	char result[512];
	char path_esc[128];
	int n;

	json_escape_into(path_esc, sizeof(path_esc), s->path);
	n = snprintf(result, sizeof(result),
		     "{\"proto\":%d,\"app\":\"everlive\",\"version\":\"%s\","
		     "\"pid\":%ld,\"socket\":\"%s\","
		     "\"methods\":[\"hello\",\"ping\",\"status.get\","
		     "\"led.set\",\"led.mode.set\",\"audio.volume.set\","
		     "\"audio.mute.set\",\"osd.set\",\"osd.position.set\","
		     "\"aspect.set\"]}",
		     GC573_IPC_PROTO_VERSION, s->version, (long)getpid(),
		     path_esc);
	if (n > 0 && (size_t)n < sizeof(result))
		ipc_reply(c, id, result);
	else
		ipc_error(c, id, -32603, "hello serialization failed");
}

/* ------------------------------------------------------------------ */
/* Mutating methods (queued for the render loop)                       */
/* ------------------------------------------------------------------ */

static int parse_led_mode(const char *name)
{
	if (!strcasecmp(name, "static") || !strcasecmp(name, "solid"))
		return GC573_IPC_LED_STATIC;
	if (!strcasecmp(name, "wave"))
		return GC573_IPC_LED_WAVE;
	if (!strcasecmp(name, "rainbow") || !strcasecmp(name, "cycle"))
		return GC573_IPC_LED_RAINBOW;
	if (!strcasecmp(name, "off") || !strcasecmp(name, "dark"))
		return GC573_IPC_LED_OFF;
	return -1;
}

static int parse_osd_position(const char *name)
{
	if (!strcasecmp(name, "tr") || !strcasecmp(name, "top-right"))
		return GC573_IPC_OSD_TR;
	if (!strcasecmp(name, "br") || !strcasecmp(name, "bottom-right"))
		return GC573_IPC_OSD_BR;
	if (!strcasecmp(name, "bl") || !strcasecmp(name, "bottom-left"))
		return GC573_IPC_OSD_BL;
	if (!strcasecmp(name, "tl") || !strcasecmp(name, "top-left"))
		return GC573_IPC_OSD_TL;
	return -1;
}

static int parse_aspect_mode(const char *name)
{
	if (!strcasecmp(name, "auto"))
		return GC573_IPC_ASPECT_AUTO;
	if (!strcasecmp(name, "16:9") || !strcasecmp(name, "16/9"))
		return GC573_IPC_ASPECT_16_9;
	if (!strcasecmp(name, "4:3") || !strcasecmp(name, "4/3"))
		return GC573_IPC_ASPECT_4_3;
	if (!strcasecmp(name, "stretch") || !strcasecmp(name, "fill"))
		return GC573_IPC_ASPECT_STRETCH;
	return -1;
}

static void ipc_push_and_reply(struct ipc_client *c, const char *id,
			       const struct gc573_ipc_cmd *cmd)
{
	if (!ipc_cmd_push(cmd)) {
		ipc_error(c, id, -32001, "command queue full, retry");
		return;
	}
	ipc_reply(c, id, "{\"ok\":true}");
}

static void ipc_handle_led_set(struct ipc_client *c, const char *id,
			       const char *line)
{
	struct gc573_ipc_cmd cmd = { .kind = GC573_IPC_CMD_LED_SET };
	long r, g, b;

	if (!json_get_long(line, "r", &r) || !json_get_long(line, "g", &g) ||
	    !json_get_long(line, "b", &b)) {
		ipc_error(c, id, -32602, "invalid params: r, g, b required");
		return;
	}

	cmd.a = (int32_t)clamp(r, 0L, 255L);
	cmd.b = (int32_t)clamp(g, 0L, 255L);
	cmd.c = (int32_t)clamp(b, 0L, 255L);
	ipc_push_and_reply(c, id, &cmd);
}

static void ipc_handle_led_mode(struct ipc_client *c, const char *id,
				const char *line)
{
	struct gc573_ipc_cmd cmd = { .kind = GC573_IPC_CMD_LED_MODE };
	char mode[24];
	int code;

	if (!json_get_string(line, "mode", mode, sizeof(mode))) {
		ipc_error(c, id, -32602, "invalid params: mode required");
		return;
	}

	code = parse_led_mode(mode);
	if (code < 0) {
		ipc_error(c, id, -32602,
			  "unknown mode: static|wave|rainbow|off");
		return;
	}

	cmd.a = code;
	ipc_push_and_reply(c, id, &cmd);
}

static void ipc_handle_audio_gain(struct ipc_client *c, const char *id,
				  const char *line)
{
	struct gc573_ipc_cmd cmd = { .kind = GC573_IPC_CMD_AUDIO_GAIN };
	double gain;

	if (!json_get_double(line, "gain", &gain)) {
		ipc_error(c, id, -32602, "invalid params: gain required");
		return;
	}

	if (gain < 0.0)
		gain = 0.0;
	if (gain > 1.5)
		gain = 1.5;

	cmd.f = (float)gain;
	ipc_push_and_reply(c, id, &cmd);
}

static void ipc_handle_audio_mute(struct ipc_client *c, const char *id,
				  const char *line)
{
	struct gc573_ipc_cmd cmd = { .kind = GC573_IPC_CMD_AUDIO_MUTE };
	bool muted;

	if (!json_get_bool(line, "muted", &muted)) {
		ipc_error(c, id, -32602, "invalid params: muted required");
		return;
	}

	cmd.a = muted ? 1 : 0;
	ipc_push_and_reply(c, id, &cmd);
}

static void ipc_handle_osd_enable(struct ipc_client *c, const char *id,
				  const char *line)
{
	struct gc573_ipc_cmd cmd = { .kind = GC573_IPC_CMD_OSD_ENABLE };
	bool enabled;

	if (!json_get_bool(line, "enabled", &enabled)) {
		ipc_error(c, id, -32602, "invalid params: enabled required");
		return;
	}

	cmd.a = enabled ? 1 : 0;
	ipc_push_and_reply(c, id, &cmd);
}

static void ipc_handle_osd_position(struct ipc_client *c, const char *id,
				    const char *line)
{
	struct gc573_ipc_cmd cmd = { .kind = GC573_IPC_CMD_OSD_POSITION };
	char pos[24];
	int code;

	if (!json_get_string(line, "pos", pos, sizeof(pos))) {
		ipc_error(c, id, -32602, "invalid params: pos required");
		return;
	}

	code = parse_osd_position(pos);
	if (code < 0) {
		ipc_error(c, id, -32602, "unknown pos: tr|br|bl|tl");
		return;
	}

	cmd.a = code;
	ipc_push_and_reply(c, id, &cmd);
}

static void ipc_handle_aspect(struct ipc_client *c, const char *id,
			      const char *line)
{
	struct gc573_ipc_cmd cmd = { .kind = GC573_IPC_CMD_ASPECT };
	char mode[24];
	int code;

	if (!json_get_string(line, "mode", mode, sizeof(mode))) {
		ipc_error(c, id, -32602, "invalid params: mode required");
		return;
	}

	code = parse_aspect_mode(mode);
	if (code < 0) {
		ipc_error(c, id, -32602,
			  "unknown mode: auto|16:9|4:3|stretch");
		return;
	}

	cmd.a = code;
	ipc_push_and_reply(c, id, &cmd);
}

static void ipc_handle_line(struct ipc_server *s, struct ipc_client *c,
			    const char *line)
{
	char id[IPC_ID_MAX];
	char method[IPC_METHOD_MAX];
	bool has_id = json_find_value(line, "id", id, sizeof(id)) &&
		      strcmp(id, "null") != 0;

	if (!json_get_string(line, "method", method, sizeof(method))) {
		if (has_id)
			ipc_error(c, id, -32700, "parse error: missing method");
		return;
	}

	if (strcmp(method, "hello") == 0) {
		c->hello_done = true;
		ipc_send_hello(s, c, has_id ? id : NULL);
	} else if (strcmp(method, "ping") == 0) {
		ipc_reply(c, has_id ? id : NULL, "\"pong\"");
	} else if (strcmp(method, "status.get") == 0) {
		ipc_send_status(s, c, has_id ? id : NULL);
	} else if (strcmp(method, "led.set") == 0) {
		ipc_handle_led_set(c, has_id ? id : NULL, line);
	} else if (strcmp(method, "led.mode.set") == 0) {
		ipc_handle_led_mode(c, has_id ? id : NULL, line);
	} else if (strcmp(method, "audio.volume.set") == 0) {
		ipc_handle_audio_gain(c, has_id ? id : NULL, line);
	} else if (strcmp(method, "audio.mute.set") == 0) {
		ipc_handle_audio_mute(c, has_id ? id : NULL, line);
	} else if (strcmp(method, "osd.set") == 0) {
		ipc_handle_osd_enable(c, has_id ? id : NULL, line);
	} else if (strcmp(method, "osd.position.set") == 0) {
		ipc_handle_osd_position(c, has_id ? id : NULL, line);
	} else if (strcmp(method, "aspect.set") == 0) {
		ipc_handle_aspect(c, has_id ? id : NULL, line);
	} else {
		ipc_error(c, has_id ? id : NULL, -32601, "method not found");
	}
}

/* Process every complete line and keep the partial tail. */
static void ipc_drain_lines(struct ipc_server *s, struct ipc_client *c)
{
	size_t consumed = 0;

	while (consumed < c->inlen) {
		char *line = c->inbuf + consumed;
		size_t avail = c->inlen - consumed;
		char *nl = memchr(line, '\n', avail);
		size_t len;

		if (!nl)
			break;
		len = (size_t)(nl - line);
		*nl = '\0';
		if (len > 0 && line[len - 1] == '\r')
			line[len - 1] = '\0';
		if (line[0] != '\0')
			ipc_handle_line(s, c, line);
		consumed += len + 1U;
		if (!c->active)
			return;
	}

	if (consumed > 0) {
		memmove(c->inbuf, c->inbuf + consumed, c->inlen - consumed);
		c->inlen -= consumed;
	}
	c->inbuf[c->inlen] = '\0';
}

static void ipc_client_read(struct ipc_server *s, struct ipc_client *c)
{
	for (;;) {
		size_t space = sizeof(c->inbuf) - c->inlen - 1U;
		ssize_t n;

		if (space == 0) {
			ipc_error(c, NULL, -32000, "frame too large");
			ipc_close_client(s, c);
			return;
		}

		n = recv(c->fd, c->inbuf + c->inlen, space, MSG_DONTWAIT);
		if (n == 0) {
			ipc_close_client(s, c);
			return;
		}
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			ipc_close_client(s, c);
			return;
		}

		c->inlen += (size_t)n;
		c->inbuf[c->inlen] = '\0';
		ipc_drain_lines(s, c);
		if (!c->active)
			return;
	}
}

static void ipc_accept_all(struct ipc_server *s)
{
	for (;;) {
		struct ucred cred;
		socklen_t clen = sizeof(cred);
		struct ipc_client *c;
		struct epoll_event ev;
		int fd = accept4(s->listen_fd, NULL, NULL,
				 SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			return;	/* EAGAIN or a real accept error */
		}

		/*
		 * The socket mode is already 0600, but an explicit credential
		 * check keeps the guarantee even if an administrator widens the
		 * permissions or the socket is rebound in a shared directory.
		 */
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0 ||
		    cred.uid != getuid()) {
			close(fd);
			continue;
		}

		c = ipc_free_slot(s);
		if (!c) {
			close(fd);
			continue;
		}

		c->fd = fd;
		c->hello_done = false;
		c->inlen = 0;
		c->inbuf[0] = '\0';
		__atomic_store_n(&c->active, true, __ATOMIC_RELEASE);

		ev.events = EPOLLIN;
		ev.data.fd = fd;
		if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
			__atomic_store_n(&c->active, false, __ATOMIC_RELEASE);
			c->fd = -1;
			close(fd);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Control thread                                                      */
/* ------------------------------------------------------------------ */

static void *ipc_thread(void *arg)
{
	struct ipc_server *s = arg;
	struct epoll_event events[GC573_IPC_MAX_CLIENTS + 2];

	while (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
		int n = epoll_wait(s->epoll_fd, events,
				   (int)ARRAY_SIZE(events), 500);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		for (int i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == s->stop_fd)
				return NULL;
			if (fd == s->listen_fd) {
				ipc_accept_all(s);
				continue;
			}

			struct ipc_client *c = ipc_find_client(s, fd);

			if (c)
				ipc_client_read(s, c);
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

int gc573_ipc_start(const char *version, const char *socket_override)
{
	struct ipc_server *s = &g_ipc;
	struct sockaddr_un addr;
	struct epoll_event ev;
	const char *dir;
	const char *stage = "resolve path";
	char candidate[128];
	int ret;
	int n;

	if (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE))
		return 0;

	memset(s, 0, sizeof(*s));
	s->listen_fd = -1;
	s->epoll_fd = -1;
	s->stop_fd = -1;
	s->lock_fd = -1;
	__atomic_store_n(&g_cmdq.head, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_cmdq.tail, 0, __ATOMIC_RELAXED);
	snprintf(s->version, sizeof(s->version), "%s",
		 version && *version ? version : "unknown");

	/* 1. Resolve the socket path. */
	if (socket_override && *socket_override) {
		if (strlen(socket_override) >= IPC_PATH_LIMIT) {
			ret = -ENAMETOOLONG;
			goto fail;
		}
		snprintf(s->path, sizeof(s->path), "%s", socket_override);
	} else {
		dir = getenv("XDG_RUNTIME_DIR");
		if (!dir || !*dir || access(dir, W_OK) != 0) {
			snprintf(candidate, sizeof(candidate), "/run/user/%u",
				 (unsigned int)getuid());
			if (access(candidate, W_OK) == 0) {
				dir = candidate;
			} else {
				snprintf(candidate, sizeof(candidate),
					 "/tmp/everlive-%u", (unsigned int)getuid());
				if (mkdir(candidate, 0700) < 0 && errno != EEXIST) {
					ret = -errno;
					goto fail;
				}
				dir = candidate;
			}
		}
		n = snprintf(s->path, sizeof(s->path), "%s/%s", dir,
			     IPC_DEFAULT_NAME);
		if (n < 0 || (size_t)n >= IPC_PATH_LIMIT) {
			ret = -ENAMETOOLONG;
			goto fail;
		}
	}

	/* 2. Take the lock file — this is the authoritative ownership test. */
	stage = "lock file";
	snprintf(s->lock_path, sizeof(s->lock_path), "%.107s.lock", s->path);
	s->lock_fd = open(s->lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (s->lock_fd < 0) {
		ret = -errno;
		goto fail;
	}
	if (flock(s->lock_fd, LOCK_EX | LOCK_NB) < 0) {
		ret = (errno == EWOULDBLOCK) ? -EADDRINUSE : -errno;
		goto fail;
	}

	/* 3. A leftover socket without a live lock owner is stale. */
	if (access(s->path, F_OK) == 0)
		(void)unlink(s->path);

	stage = "bind socket";
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, s->path, strlen(s->path) + 1);

	s->listen_fd = socket(AF_UNIX,
			      SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s->listen_fd < 0) {
		ret = -errno;
		goto fail;
	}
	if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ret = -errno;
		goto fail;
	}
	(void)chmod(s->path, 0600);
	if (listen(s->listen_fd, GC573_IPC_MAX_CLIENTS) < 0) {
		ret = -errno;
		goto fail;
	}

	s->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (s->epoll_fd < 0) {
		ret = -errno;
		goto fail;
	}

	stage = "epoll listen fd";
	ev.events = EPOLLIN;
	ev.data.fd = s->listen_fd;
	if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->listen_fd, &ev) < 0) {
		ret = -errno;
		goto fail;
	}

	s->stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (s->stop_fd < 0) {
		ret = -errno;
		goto fail;
	}
	stage = "epoll stop fd";
	ev.events = EPOLLIN;
	ev.data.fd = s->stop_fd;
	if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->stop_fd, &ev) < 0) {
		ret = -errno;
		goto fail;
	}

	stage = "control thread";
	__atomic_store_n(&s->running, 1, __ATOMIC_RELEASE);
	if (pthread_create(&s->thread, NULL, ipc_thread, s) != 0) {
		__atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
		ret = -EAGAIN;
		goto fail;
	}
	s->thread_started = true;

	pr_info("IPC control socket: %s (JSON-RPC %d, read-only)", s->path,
		GC573_IPC_PROTO_VERSION);
	return 0;

fail:
	pr_warn("IPC setup failed while %s: %s", stage, strerror(-ret));
	gc573_ipc_stop();
	return ret;
}

void gc573_ipc_stop(void)
{
	struct ipc_server *s = &g_ipc;

	if (!__atomic_load_n(&s->running, __ATOMIC_ACQUIRE) && !s->path[0])
		return;

	__atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);

	if (s->stop_fd >= 0) {
		uint64_t one = 1;
		ssize_t written = write(s->stop_fd, &one, sizeof(one));

		(void)written;
	}

	if (s->thread_started) {
		pthread_join(s->thread, NULL);
		s->thread_started = false;
	}

	for (unsigned int i = 0; i < GC573_IPC_MAX_CLIENTS; i++)
		ipc_close_client(s, &s->clients[i]);

	if (s->listen_fd >= 0) {
		close(s->listen_fd);
		s->listen_fd = -1;
	}
	if (s->epoll_fd >= 0) {
		close(s->epoll_fd);
		s->epoll_fd = -1;
	}
	if (s->stop_fd >= 0) {
		close(s->stop_fd);
		s->stop_fd = -1;
	}
	if (s->path[0]) {
		(void)unlink(s->path);
		s->path[0] = '\0';
	}
	if (s->lock_fd >= 0) {
		(void)flock(s->lock_fd, LOCK_UN);
		close(s->lock_fd);
		s->lock_fd = -1;
	}
	s->lock_path[0] = '\0';
}

bool gc573_ipc_has_clients(void)
{
	for (unsigned int i = 0; i < GC573_IPC_MAX_CLIENTS; i++) {
		if (__atomic_load_n(&g_ipc.clients[i].active, __ATOMIC_RELAXED))
			return true;
	}
	return false;
}

const char *gc573_ipc_socket_path(void)
{
	return g_ipc.path;
}

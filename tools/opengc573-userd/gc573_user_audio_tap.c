/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_audio_tap.c — PipeWire capture tap for the EverLive streamer
 *
 * Architecture
 * ------------
 *  audio worker ──(SPSC ring)──> PipeWire RT thread ──> "EverLive Capture Audio"
 *
 * The audio worker owns the write side, the PipeWire data-loop thread the read
 * side.  No mutex is taken on the audio path; the control plane (node lookup,
 * stream connect) runs under the PipeWire thread-loop lock and only ever from
 * the audio worker at a format change.
 *
 * The sink itself is expected to come from a PipeWire config drop-in installed
 * by install.sh ("EverLive Capture Audio").  If the node is missing, the
 * adapter module is loaded at runtime so a source build still works.
 */

#include "gc573-user-compat.h"
#include "gc573_user_audio_tap.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <sys/wait.h>

#define TAP_RING_BYTES		65536U	/* power of two, ~340 ms at 48 kHz */
#define TAP_CHANNELS		2U
#define TAP_SAMPLE_BYTES	2U

struct gc573_audio_tap {
	int running;
	char node_name[64];
	int rate;
	int pending_seq;

	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_stream *stream;

	struct spa_hook core_listener;
	struct spa_hook registry_listener;
	struct spa_hook stream_listener;

	unsigned long target_seen;
	unsigned long written_bytes;
	unsigned long dropped_bytes;
};

static struct gc573_audio_tap g_tap;

/* Written by the audio worker, read by the PipeWire RT thread. */
static uint8_t g_ring[TAP_RING_BYTES];
static uint64_t g_ring_head;
static uint64_t g_ring_tail;

/* S32 -> S16 conversion buffer, audio worker only. */
static uint8_t g_convert[65536];

static bool g_pw_initialized;

/* ------------------------------------------------------------------ */
/* Lock-free ring                                                      */
/* ------------------------------------------------------------------ */

static void tap_ring_write(const void *data, size_t bytes)
{
	uint64_t head = __atomic_load_n(&g_ring_head, __ATOMIC_RELAXED);
	uint64_t tail = __atomic_load_n(&g_ring_tail, __ATOMIC_ACQUIRE);
	size_t offset = (size_t)(head & (TAP_RING_BYTES - 1U));
	size_t first;

	if (bytes > (size_t)(TAP_RING_BYTES - (head - tail))) {
		/* Never stall the audio path for a slow reader. */
		__atomic_add_fetch(&g_tap.dropped_bytes, bytes, __ATOMIC_RELAXED);
		return;
	}

	first = TAP_RING_BYTES - offset;
	if (first > bytes)
		first = bytes;

	memcpy(g_ring + offset, data, first);
	memcpy(g_ring, (const uint8_t *)data + first, bytes - first);

	__atomic_store_n(&g_ring_head, head + bytes, __ATOMIC_RELEASE);
	__atomic_add_fetch(&g_tap.written_bytes, bytes, __ATOMIC_RELAXED);
}

static size_t tap_ring_read(void *dst, size_t bytes)
{
	uint64_t tail = __atomic_load_n(&g_ring_tail, __ATOMIC_RELAXED);
	uint64_t head = __atomic_load_n(&g_ring_head, __ATOMIC_ACQUIRE);
	size_t avail = (size_t)(head - tail);
	size_t offset = (size_t)(tail & (TAP_RING_BYTES - 1U));
	size_t first;

	if (bytes > avail)
		bytes = avail;
	if (!bytes)
		return 0;

	first = TAP_RING_BYTES - offset;
	if (first > bytes)
		first = bytes;

	memcpy(dst, g_ring + offset, first);
	memcpy((uint8_t *)dst + first, g_ring, bytes - first);

	__atomic_store_n(&g_ring_tail, tail + bytes, __ATOMIC_RELEASE);
	return bytes;
}

/* ------------------------------------------------------------------ */
/* PipeWire callbacks                                                  */
/* ------------------------------------------------------------------ */

static void tap_process(void *userdata)
{
	struct gc573_audio_tap *tap = userdata;
	struct pw_buffer *pwbuf;
	struct spa_buffer *buf;
	struct spa_data *data;
	size_t n;

	if (!tap->stream)
		return;

	pwbuf = pw_stream_dequeue_buffer(tap->stream);
	if (!pwbuf)
		return;

	buf = pwbuf->buffer;
	if (!buf || buf->n_datas == 0) {
		pw_stream_queue_buffer(tap->stream, pwbuf);
		return;
	}

	data = &buf->datas[0];
	if (!data->data) {
		pw_stream_queue_buffer(tap->stream, pwbuf);
		return;
	}

	n = tap_ring_read(data->data, data->maxsize);
	if (data->chunk) {
		data->chunk->offset = 0;
		data->chunk->size = (uint32_t)n;
		data->chunk->stride = TAP_CHANNELS * TAP_SAMPLE_BYTES;
	}

	pw_stream_queue_buffer(tap->stream, pwbuf);
}

static void tap_stream_state_changed(void *userdata,
				     enum pw_stream_state old,
				     enum pw_stream_state state,
				     const char *error)
{
	struct gc573_audio_tap *tap = userdata;

	(void)old;
	if (state == PW_STREAM_STATE_ERROR)
		pr_warn("Audio tap: stream error: %s",
			error ? error : "unknown");
	else if (state == PW_STREAM_STATE_STREAMING)
		pr_info("Audio tap: connected to '%s' at %d Hz S16 stereo",
			tap->node_name, tap->rate);
}

static const struct pw_stream_events tap_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = tap_stream_state_changed,
	.process = tap_process,
};

static void tap_core_done(void *userdata, uint32_t id, int seq)
{
	struct gc573_audio_tap *tap = userdata;

	if (id == PW_ID_CORE && seq == tap->pending_seq)
		pw_thread_loop_signal(tap->loop, false);
}

static const struct pw_core_events tap_core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = tap_core_done,
};

static void tap_registry_global(void *userdata, uint32_t id,
				uint32_t permissions, const char *type,
				uint32_t version, const struct spa_dict *props)
{
	struct gc573_audio_tap *tap = userdata;
	const char *name;

	(void)id;
	(void)permissions;
	(void)type;
	(void)version;

	if (!props)
		return;

	name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	if (name && strcmp(name, tap->node_name) == 0)
		__atomic_store_n(&tap->target_seen, 1, __ATOMIC_RELAXED);
}

static const struct pw_registry_events tap_registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = tap_registry_global,
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void tap_pw_init_once(void)
{
	if (g_pw_initialized)
		return;

	{
		int argc = 0;
		char *argv_data[] = { NULL };
		char **argv = argv_data;

		pw_init(&argc, &argv);
	}
	g_pw_initialized = true;
}

/* Caller must hold the thread-loop lock. */
static void tap_teardown_locked(struct gc573_audio_tap *tap)
{
	if (tap->stream) {
		pw_stream_disconnect(tap->stream);
		pw_stream_destroy(tap->stream);
		tap->stream = NULL;
	}
	if (tap->registry) {
		pw_proxy_destroy((struct pw_proxy *)tap->registry);
		tap->registry = NULL;
	}
	if (tap->core) {
		pw_core_disconnect(tap->core);
		tap->core = NULL;
	}
	if (tap->context) {
		pw_context_destroy(tap->context);
		tap->context = NULL;
	}
}

/*
 * Create the sink in-process when the config drop-in is not installed.
 *
 * PipeWire's module loader is an internal (impl) API, so the sink is created
 * through the public tooling instead: `pw-cli load-module` asks the running
 * daemon to add the adapter node.  fork/exec keeps the arguments out of a
 * shell, so the node name can never be interpreted as shell syntax.
 */
static void tap_ensure_sink(struct gc573_audio_tap *tap)
{
	char args[512];
	pid_t pid;

	if (__atomic_load_n(&tap->target_seen, __ATOMIC_RELAXED))
		return;

	snprintf(args, sizeof(args),
		 "{ factory.name = support.null-audio-sink "
		 "node.name = %s "
		 "node.description = \"EverLive Capture Audio\" "
		 "media.class = Audio/Sink "
		 "audio.position = [ FL FR ] "
		 "monitor.channel-volumes = true "
		 "node.virtual = false }",
		 tap->node_name);

	pid = fork();
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDWR);

		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		execlp("pw-cli", "pw-cli", "load-module",
		       "libpipewire-module-adapter", args, (char *)NULL);
		_exit(127);
	}

	if (pid > 0) {
		int status = 0;

		if (waitpid(pid, &status, 0) > 0 &&
		    WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			pr_info("Audio tap: created sink '%s' via pw-cli",
				tap->node_name);
			return;
		}
	}

	pr_warn("Audio tap: sink '%s' not found; install the PipeWire "
		"drop-in (sudo ./install.sh) for a persistent node",
		tap->node_name);
}

int gc573_audio_tap_start(const char *node_name)
{
	struct gc573_audio_tap *tap = &g_tap;
	int ret = 0;

	if (tap->loop)
		return 0;

	memset(tap, 0, sizeof(*tap));
	snprintf(tap->node_name, sizeof(tap->node_name), "%s",
		 node_name && *node_name ? node_name : GC573_AUDIO_TAP_DEFAULT);

	__atomic_store_n(&g_ring_head, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_ring_tail, 0, __ATOMIC_RELAXED);

	tap_pw_init_once();

	tap->loop = pw_thread_loop_new("everlive-audio-tap", NULL);
	if (!tap->loop)
		return -ENOMEM;

	if (pw_thread_loop_start(tap->loop) < 0) {
		pw_thread_loop_destroy(tap->loop);
		tap->loop = NULL;
		return -EIO;
	}

	pw_thread_loop_lock(tap->loop);

	tap->context = pw_context_new(pw_thread_loop_get_loop(tap->loop),
				      NULL, 0);
	if (!tap->context) {
		ret = -ENOMEM;
		goto fail;
	}

	tap->core = pw_context_connect(tap->context, NULL, 0);
	if (!tap->core) {
		ret = -EIO;
		goto fail;
	}

	pw_core_add_listener(tap->core, &tap->core_listener,
			     &tap_core_events, tap);

	tap->registry = pw_core_get_registry(tap->core, PW_VERSION_REGISTRY, 0);
	if (tap->registry)
		pw_registry_add_listener(tap->registry, &tap->registry_listener,
					 &tap_registry_events, tap);

	/*
	 * Round-trip so the registry is fully populated before we decide
	 * whether the drop-in sink exists.
	 */
	tap->pending_seq = pw_core_sync(tap->core, PW_ID_CORE, 0);
	if (pw_thread_loop_timed_wait(tap->loop, 2) != 0)
		pr_warn("Audio tap: PipeWire did not answer the registry sync");

	tap_ensure_sink(tap);

	__atomic_store_n(&tap->running, 1, __ATOMIC_RELEASE);
	pw_thread_loop_unlock(tap->loop);

	pr_info("Audio tap: '%s' ready (%s)", tap->node_name,
		__atomic_load_n(&tap->target_seen, __ATOMIC_RELAXED) ?
		"sink from PipeWire config" : "runtime sink");
	return 0;

fail:
	tap_teardown_locked(tap);
	pw_thread_loop_unlock(tap->loop);
	pw_thread_loop_stop(tap->loop);
	pw_thread_loop_destroy(tap->loop);
	tap->loop = NULL;
	return ret;
}

void gc573_audio_tap_set_format(unsigned int rate_hz)
{
	struct gc573_audio_tap *tap = &g_tap;
	struct spa_audio_info_raw info;
	uint8_t buffer[1024];
	struct spa_pod_builder builder;
	const struct spa_pod *params[1];
	struct pw_properties *props;
	int res;

	if (!tap->loop || !tap->core)
		return;
	if (!rate_hz)
		rate_hz = 48000;

	if (tap->stream && tap->rate == (int)rate_hz)
		return;

	pw_thread_loop_lock(tap->loop);

	if (tap->stream) {
		pw_stream_disconnect(tap->stream);
		pw_stream_destroy(tap->stream);
		tap->stream = NULL;
	}

	tap->rate = (int)rate_hz;

	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
				  PW_KEY_MEDIA_CATEGORY, "Capture",
				  PW_KEY_MEDIA_ROLE, "Game",
				  PW_KEY_APP_NAME, "EverLive",
				  PW_KEY_APP_ICON_NAME, "everlive",
				  PW_KEY_NODE_NAME, "everlive-capture-tap",
				  PW_KEY_NODE_DESCRIPTION,
				  "EverLive Capture Tap",
				  PW_KEY_NODE_VIRTUAL, "false",
				  PW_KEY_TARGET_OBJECT, tap->node_name,
				  NULL);
	if (!props) {
		pw_thread_loop_unlock(tap->loop);
		return;
	}

	tap->stream = pw_stream_new(tap->core, "EverLive Capture Tap", props);
	if (!tap->stream) {
		pw_thread_loop_unlock(tap->loop);
		return;
	}

	pw_stream_add_listener(tap->stream, &tap->stream_listener,
			       &tap_stream_events, tap);

	spa_zero(info);
	info.format = SPA_AUDIO_FORMAT_S16_LE;
	info.rate = rate_hz;
	info.channels = TAP_CHANNELS;

	spa_pod_builder_init(&builder, buffer, sizeof(buffer));
	params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
					       &info);

	res = pw_stream_connect(tap->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_RT_PROCESS,
				params, 1);

	pw_thread_loop_unlock(tap->loop);

	if (res < 0)
		pr_warn("Audio tap: connect failed (%s)", spa_strerror(res));
	else
		pr_info("Audio tap: publishing %u Hz S16 stereo to '%s'",
			rate_hz, tap->node_name);
}

void gc573_audio_tap_write(const void *data, size_t bytes,
			   unsigned int sample_bits)
{
	const uint8_t *src = data;

	if (!__atomic_load_n(&g_tap.running, __ATOMIC_RELAXED) ||
	    !data || !bytes)
		return;

	if (sample_bits > 16) {
		/*
		 * The streamer hands us left-aligned S32LSB (S24_3LE was widened
		 * for SDL), so the top 16 bits are exactly the S16 sample.
		 */
		size_t samples = bytes / 4U;
		size_t i;

		if (samples * 2U > sizeof(g_convert))
			samples = sizeof(g_convert) / 2U;

		for (i = 0; i < samples; i++) {
			int32_t v;
			uint16_t out;

			memcpy(&v, src + i * 4U, sizeof(v));
			out = (uint16_t)((uint32_t)v >> 16);
			memcpy(g_convert + i * 2U, &out, sizeof(out));
		}

		tap_ring_write(g_convert, samples * 2U);
	} else {
		/* Whole stereo frames only: never split a sample pair. */
		tap_ring_write(src, bytes & ~3U);
	}
}

void gc573_audio_tap_stop(void)
{
	struct gc573_audio_tap *tap = &g_tap;

	if (!tap->loop)
		return;

	__atomic_store_n(&tap->running, 0, __ATOMIC_RELEASE);

	pw_thread_loop_lock(tap->loop);
	tap_teardown_locked(tap);
	pw_thread_loop_unlock(tap->loop);

	pw_thread_loop_stop(tap->loop);
	pw_thread_loop_destroy(tap->loop);
	tap->loop = NULL;
	tap->node_name[0] = '\0';
	tap->rate = 0;
}

bool gc573_audio_tap_active(void)
{
	return __atomic_load_n(&g_tap.running, __ATOMIC_RELAXED) != 0;
}

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_video_tap.c — Native PipeWire Video/Source for the GC573
 *
 * The capture thread calls gc573_video_tap_write() with each finished YUY2
 * frame.  A private PipeWire thread-loop owns a driving Video/Source stream
 * whose timer is paced by the HDMI frame rate.  Every timer tick copies the
 * latest frame into a graph buffer, so a slow consumer can only drop frames,
 * never stall the DMA ring.
 */

#include "gc573-user-compat.h"
#include "gc573_user_video_tap.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t video_tap_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#define VIDEO_TAP_DEFAULT_WIDTH		1920U
#define VIDEO_TAP_DEFAULT_HEIGHT	1080U
#define VIDEO_TAP_DEFAULT_FPS_X100	6000U

struct gc573_video_tap {
	int running;
	char node_name[64];

	unsigned int width;
	unsigned int height;
	unsigned int fps_x100;

	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_source *timer;
	struct spa_video_info_raw negotiated;
	bool format_valid;
	uint32_t seq;

	/* Written from the capture thread, read from the PipeWire thread. */
	pthread_mutex_t lock;
	bool lock_ready;
	uint8_t *frame;
	size_t frame_capacity;
	size_t frame_bytes;
	unsigned int frame_width;
	unsigned int frame_height;
	uint64_t written;
	uint64_t dropped;
};

static struct gc573_video_tap g_video_tap;

static void video_tap_update_timer(struct gc573_video_tap *tap, bool active)
{
	uint64_t interval_ns;
	struct timespec timeout = { .tv_sec = 0, .tv_nsec = 1 };
	struct timespec interval;

	if (!tap->loop || !tap->timer)
		return;

	if (!active) {
		pw_loop_update_timer(pw_thread_loop_get_loop(tap->loop),
				     tap->timer, NULL, NULL, false);
		return;
	}

	interval_ns = tap->fps_x100 ?
		(100000000000ULL / (uint64_t)tap->fps_x100) : 16666667ULL;
	if (interval_ns < 4000000ULL)
		interval_ns = 16666667ULL;

	interval.tv_sec = (time_t)(interval_ns / 1000000000ULL);
	interval.tv_nsec = (long)(interval_ns % 1000000000ULL);
	pw_loop_update_timer(pw_thread_loop_get_loop(tap->loop), tap->timer,
			     &timeout, &interval, false);
}

static void video_tap_on_timer(void *data, uint64_t expirations)
{
	struct gc573_video_tap *tap = data;

	(void)expirations;
	if (!tap || !tap->stream)
		return;

	if (pw_stream_get_state(tap->stream, NULL) ==
		    PW_STREAM_STATE_STREAMING &&
	    pw_stream_is_driving(tap->stream))
		pw_stream_trigger_process(tap->stream);
}

static void video_tap_on_process(void *data)
{
	struct gc573_video_tap *tap = data;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *dst;
	uint32_t w;
	uint32_t h;
	size_t stride;
	size_t size;

	if (!tap || !tap->stream || !tap->format_valid)
		return;

	b = pw_stream_dequeue_buffer(tap->stream);
	if (!b)
		return;

	buf = b->buffer;
	dst = buf->datas[0].data;
	if (!dst) {
		pw_stream_queue_buffer(tap->stream, b);
		return;
	}

	w = tap->negotiated.size.width;
	h = tap->negotiated.size.height;
	if (!w || !h) {
		w = tap->width;
		h = tap->height;
	}
	stride = (size_t)w * 2U;
	size = stride * h;

	if (size > buf->datas[0].maxsize)
		size = buf->datas[0].maxsize;

	pthread_mutex_lock(&tap->lock);
	if (tap->frame && tap->frame_width == w && tap->frame_height == h &&
	    tap->frame_bytes <= size) {
		memcpy(dst, tap->frame, tap->frame_bytes);
		size = tap->frame_bytes;
		tap->written++;
	} else {
		/* No matching frame yet: publish black so consumers stay live. */
		memset(dst, 0, size);
		tap->dropped++;
	}
	pthread_mutex_unlock(&tap->lock);

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = (int32_t)stride;
	buf->datas[0].chunk->size = (uint32_t)size;
	buf->datas[0].chunk->flags = 0;

	struct spa_meta_header *mh =
		spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*mh));
	if (mh) {
		uint64_t now_ns = pw_stream_get_nsec(tap->stream);

		mh->pts = (int64_t)(now_ns ? now_ns : video_tap_now_ns());
		mh->flags = 0;
		mh->seq = tap->seq++;
		mh->dts_offset = 0;
	}

	struct spa_meta *md = spa_buffer_find_meta(buf, SPA_META_VideoDamage);
	if (md) {
		struct spa_meta_region *r = spa_meta_first(md);

		if (spa_meta_check(r, md)) {
			r->region.position = SPA_POINT(0, 0);
			r->region.size = SPA_RECTANGLE(w, h);
			r++;
		}
		if (spa_meta_check(r, md))
			r->region = SPA_REGION(0, 0, 0, 0);
	}

	pw_stream_queue_buffer(tap->stream, b);
}

static void video_tap_on_param_changed(void *data, uint32_t id,
				       const struct spa_pod *param)
{
	struct gc573_video_tap *tap = data;
	uint32_t w;
	uint32_t h;
	size_t stride;
	size_t frame_bytes;
	uint8_t params_buffer[1024];
	struct spa_pod_builder builder =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[3];

	if (!tap || !param || id != SPA_PARAM_Format)
		return;

	if (spa_format_video_raw_parse(param, &tap->negotiated) < 0)
		return;

	tap->format_valid = true;
	w = tap->negotiated.size.width ? tap->negotiated.size.width :
		VIDEO_TAP_DEFAULT_WIDTH;
	h = tap->negotiated.size.height ? tap->negotiated.size.height :
		VIDEO_TAP_DEFAULT_HEIGHT;
	stride = (size_t)w * 2U;
	frame_bytes = stride * h;

	params[0] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int((int32_t)frame_bytes),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int32_t)stride),
		SPA_PARAM_BUFFERS_align, SPA_POD_Int(16));

	params[1] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	params[2] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
		SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
			sizeof(struct spa_meta_region) * 16,
			sizeof(struct spa_meta_region) * 1,
			sizeof(struct spa_meta_region) * 16));

	pw_stream_update_params(tap->stream, params, 3);
	if (pw_stream_get_state(tap->stream, NULL) ==
	    PW_STREAM_STATE_STREAMING)
		video_tap_update_timer(tap, true);

	pr_info("Video tap: '%s' negotiated %ux%u YUY2",
		tap->node_name, w, h);
}

static void video_tap_on_state_changed(void *data, enum pw_stream_state old,
				       enum pw_stream_state state,
				       const char *error)
{
	struct gc573_video_tap *tap = data;

	(void)old;
	if (state == PW_STREAM_STATE_STREAMING)
		pr_info("Video tap: '%s' streaming", tap->node_name);
	else if (state == PW_STREAM_STATE_ERROR)
		pr_warn("Video tap: '%s' error%s%s", tap->node_name,
			error ? ": " : "", error ? error : "");

	video_tap_update_timer(tap, state == PW_STREAM_STATE_STREAMING);
}

static const struct pw_stream_events g_video_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = video_tap_on_state_changed,
	.param_changed = video_tap_on_param_changed,
	.process = video_tap_on_process,
};

static const struct spa_pod *
video_tap_build_format(struct spa_pod_builder *builder, unsigned int width,
		       unsigned int height, unsigned int fps_x100)
{
	struct spa_pod_frame frames[2];
	unsigned int num = fps_x100 ? fps_x100 : VIDEO_TAP_DEFAULT_FPS_X100;
	unsigned int denom = 100;

	if (num % 100U == 0) {
		num /= 100U;
		denom = 1;
	}

	spa_pod_builder_push_object(builder, &frames[0],
				    SPA_TYPE_OBJECT_Format,
				    SPA_PARAM_EnumFormat);
	spa_pod_builder_add(builder,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_YUY2),
		SPA_FORMAT_VIDEO_size,
		SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
		0);
	spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_framerate, 0);
	spa_pod_builder_push_choice(builder, &frames[1], SPA_CHOICE_Enum, 0);
	spa_pod_builder_fraction(builder, num, denom);
	spa_pod_builder_fraction(builder, num, denom);
	spa_pod_builder_fraction(builder, 50, 1);
	spa_pod_builder_fraction(builder, 30, 1);
	spa_pod_builder_pop(builder, &frames[1]);
	return spa_pod_builder_pop(builder, &frames[0]);
}

/* Caller holds the PipeWire thread-loop lock. */
static int video_tap_stream_locked(struct gc573_video_tap *tap)
{
	struct pw_properties *props;
	uint8_t buffer[1024];
	struct spa_pod_builder builder =
		SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	char description[96];

	if (tap->stream) {
		pw_stream_disconnect(tap->stream);
		pw_stream_destroy(tap->stream);
		tap->stream = NULL;
	}
	tap->format_valid = false;

	snprintf(description, sizeof(description), "EverLive GC573 Video (%s)",
		 tap->node_name);
	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_MEDIA_CATEGORY, "Source",
		PW_KEY_MEDIA_ROLE, "Camera",
		PW_KEY_MEDIA_CLASS, "Video/Source",
		PW_KEY_NODE_NAME, tap->node_name,
		PW_KEY_NODE_DESCRIPTION, description,
		PW_KEY_NODE_NICK, "GC573 Video",
		PW_KEY_DEVICE_API, "vfio-pci",
		"node.autoconnect", "false",
		NULL);
	if (!props)
		return -ENOMEM;

	tap->stream = pw_stream_new(tap->core, description, props);
	if (!tap->stream)
		return -ENOMEM;

	pw_stream_add_listener(tap->stream, &tap->stream_listener,
			       &g_video_stream_events, tap);
	params[0] = video_tap_build_format(&builder, tap->width, tap->height,
					   tap->fps_x100);
	return pw_stream_connect(tap->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				 PW_STREAM_FLAG_MAP_BUFFERS |
				 PW_STREAM_FLAG_DRIVER,
				 params, 1);
}

int gc573_video_tap_start(const char *node_name)
{
	struct gc573_video_tap *tap = &g_video_tap;
	int ret;

	if (__atomic_load_n(&tap->running, __ATOMIC_ACQUIRE))
		return 0;

	/* A previous stop keeps the node name; a fresh start resets it. */
	if (!tap->loop) {
		memset(tap, 0, sizeof(*tap));
		snprintf(tap->node_name, sizeof(tap->node_name), "%s",
			 node_name && *node_name ? node_name :
			 GC573_VIDEO_TAP_DEFAULT);
		tap->width = VIDEO_TAP_DEFAULT_WIDTH;
		tap->height = VIDEO_TAP_DEFAULT_HEIGHT;
		tap->fps_x100 = VIDEO_TAP_DEFAULT_FPS_X100;
	}

	if (!tap->lock_ready) {
		if (pthread_mutex_init(&tap->lock, NULL) != 0)
			return -ENOMEM;
		tap->lock_ready = true;
	}

	pw_init(NULL, NULL);
	tap->loop = pw_thread_loop_new("everlive-video-tap", NULL);
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
		pr_warn("Video tap: cannot connect to PipeWire (is a session manager running?)");
		ret = -ECONNREFUSED;
		goto fail;
	}

	tap->timer = pw_loop_add_timer(pw_thread_loop_get_loop(tap->loop),
				       video_tap_on_timer, tap);
	if (!tap->timer) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = video_tap_stream_locked(tap);
	if (ret < 0) {
		pr_warn("Video tap: stream creation failed: %s",
			spa_strerror(ret));
		goto fail;
	}

	__atomic_store_n(&tap->running, 1, __ATOMIC_RELEASE);
	pw_thread_loop_unlock(tap->loop);

	pr_info("Video tap: published '%s' (Video/Source, YUY2)", tap->node_name);
	return 0;

fail:
	if (tap->stream) {
		pw_stream_disconnect(tap->stream);
		pw_stream_destroy(tap->stream);
		tap->stream = NULL;
	}
	if (tap->timer) {
		pw_loop_destroy_source(pw_thread_loop_get_loop(tap->loop),
				       tap->timer);
		tap->timer = NULL;
	}
	if (tap->core) {
		pw_context_destroy(tap->context);
		tap->core = NULL;
		tap->context = NULL;
	}
	pw_thread_loop_unlock(tap->loop);
	pw_thread_loop_stop(tap->loop);
	pw_thread_loop_destroy(tap->loop);
	tap->loop = NULL;
	return ret;
}

void gc573_video_tap_set_format(unsigned int width, unsigned int height,
				unsigned int fps_x100)
{
	struct gc573_video_tap *tap = &g_video_tap;

	if (!__atomic_load_n(&tap->running, __ATOMIC_ACQUIRE))
		return;
	if (!width || !height)
		return;
	if (!fps_x100)
		fps_x100 = VIDEO_TAP_DEFAULT_FPS_X100;

	if (tap->width == width && tap->height == height &&
	    tap->fps_x100 == fps_x100)
		return;

	pw_thread_loop_lock(tap->loop);
	tap->width = width;
	tap->height = height;
	tap->fps_x100 = fps_x100;
	if (video_tap_stream_locked(tap) < 0)
		pr_warn("Video tap: reconnect for %ux%u failed", width, height);
	pw_thread_loop_unlock(tap->loop);
}

void gc573_video_tap_write(const void *data, size_t bytes,
			   unsigned int width, unsigned int height)
{
	struct gc573_video_tap *tap = &g_video_tap;
	size_t stride = (size_t)width * 2U;
	size_t needed = stride * height;

	if (!__atomic_load_n(&tap->running, __ATOMIC_RELAXED) ||
	    !tap->lock_ready || !data || !width || !height)
		return;
	if (bytes < needed)
		return;

	pthread_mutex_lock(&tap->lock);
	if (tap->frame_capacity < needed) {
		uint8_t *grown = realloc(tap->frame, needed);

		if (!grown) {
			pthread_mutex_unlock(&tap->lock);
			return;
		}
		tap->frame = grown;
		tap->frame_capacity = needed;
	}
	memcpy(tap->frame, data, needed);
	tap->frame_bytes = needed;
	tap->frame_width = width;
	tap->frame_height = height;
	pthread_mutex_unlock(&tap->lock);
}

void gc573_video_tap_stop(void)
{
	struct gc573_video_tap *tap = &g_video_tap;

	if (!tap->loop)
		return;

	__atomic_store_n(&tap->running, 0, __ATOMIC_RELEASE);

	pw_thread_loop_lock(tap->loop);
	if (tap->timer) {
		pw_loop_update_timer(pw_thread_loop_get_loop(tap->loop),
				     tap->timer, NULL, NULL, false);
	}
	if (tap->stream) {
		pw_stream_disconnect(tap->stream);
		pw_stream_destroy(tap->stream);
		tap->stream = NULL;
	}
	if (tap->timer) {
		pw_loop_destroy_source(pw_thread_loop_get_loop(tap->loop),
				       tap->timer);
		tap->timer = NULL;
	}
	if (tap->core) {
		pw_context_destroy(tap->context);
		tap->core = NULL;
		tap->context = NULL;
	}
	pw_thread_loop_unlock(tap->loop);

	pw_thread_loop_stop(tap->loop);
	pw_thread_loop_destroy(tap->loop);
	tap->loop = NULL;

	pthread_mutex_lock(&tap->lock);
	free(tap->frame);
	tap->frame = NULL;
	tap->frame_capacity = 0;
	tap->frame_bytes = 0;
	pthread_mutex_unlock(&tap->lock);
}

bool gc573_video_tap_active(void)
{
	return __atomic_load_n(&g_video_tap.running, __ATOMIC_RELAXED) != 0;
}

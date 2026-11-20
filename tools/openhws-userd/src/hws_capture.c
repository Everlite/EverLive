// SPDX-License-Identifier: GPL-2.0-only
/*
 * hws_capture.c — capture thread for the HWS UHD engine (see hws_capture.h).
 */

#include "hws_capture.h"
#include "hws_user_hw.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_STALE_PACKETS	2	/* the card replays old packets after an enable */
#define INPUT_POLL_US		100000	/* follow the input every 100 ms */
#define STALL_US		1000000	/* live but no frame for 1 s: restart video */

struct hws_capture {
	struct hws_user_dev dev;
	struct hws_capture_cfg cfg;
	pthread_t thread;
	bool thread_started;
	volatile bool stop;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	/* Under lock */
	int latest_slot;
	uint64_t latest_seq;
	uint64_t latest_ts;
	unsigned int latest_w, latest_h;
	int held[HWS_CAPTURE_MAX_CONSUMERS];
	struct hws_capture_status st;

	/* Capture-thread private */
	bool live;
	bool top_pending;
	int top_slot;
	unsigned int audio_skip;
	uint64_t last_frame_us;
	uint64_t rate_t0, rate_frames;
};

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

/* Under lock: a slot that is neither newest, held, nor being written. */
static unsigned int pick_slot_locked(struct hws_capture *c, unsigned int busy)
{
	unsigned int n = c->dev.nslots;

	for (unsigned int k = 1; k <= n; k++) {
		unsigned int s = (busy + k) % n;
		bool used = s == busy || (int)s == c->latest_slot;

		for (int i = 0; i < HWS_CAPTURE_MAX_CONSUMERS && !used; i++)
			used = c->held[i] == (int)s;
		if (!used)
			return s;
	}
	return (busy + 1) % n;	/* cannot happen with >= 5 slots and 2 consumers */
}

static void video_start(struct hws_capture *c, unsigned int w, unsigned int h)
{
	hws_user_stop(&c->dev);
	pthread_mutex_lock(&c->lock);
	c->latest_slot = -1;	/* old frames have the old geometry */
	pthread_mutex_unlock(&c->lock);
	if (hws_user_set_geometry(&c->dev, w, h) < 0 ||
	    hws_user_start(&c->dev, c->cfg.audio) < 0) {
		c->live = false;
		return;
	}
	c->live = true;
	c->top_pending = false;
	c->audio_skip = AUDIO_STALE_PACKETS;
	c->last_frame_us = now_us();
	c->rate_t0 = 0;
	c->rate_frames = 0;
	pthread_mutex_lock(&c->lock);
	c->st.restarts++;
	pthread_mutex_unlock(&c->lock);
	printf("[HWS-USER] capturing %ux%u\n", w, h);
	fflush(stdout);
}

static void video_halt(struct hws_capture *c)
{
	hws_user_stop(&c->dev);
	c->live = false;
	pthread_mutex_lock(&c->lock);
	c->latest_slot = -1;
	c->st.signal = false;
	c->st.fps = 0;
	pthread_cond_broadcast(&c->cond);
	pthread_mutex_unlock(&c->lock);
}

static void follow_input(struct hws_capture *c)
{
	struct hws_user_input in;
	bool usable = hws_user_read_input(&c->dev, &in);

	pthread_mutex_lock(&c->lock);
	c->st.present = in.present;
	c->st.interlaced = in.interlaced;
	c->st.hdcp = in.hdcp;
	c->st.input_fps = in.fps;
	if (in.present) {
		c->st.width = in.width;
		c->st.height = in.height;
	}
	pthread_mutex_unlock(&c->lock);

	if (!usable) {
		if (c->live) {
			printf("[HWS-USER] signal lost\n");
			fflush(stdout);
			video_halt(c);
		}
		return;
	}
	if (!c->live || in.width != c->dev.width || in.height != c->dev.height) {
		video_start(c, in.width, in.height);
		return;
	}
	if (now_us() - c->last_frame_us > STALL_US) {
		printf("[HWS-USER] no frame for 1 s; restarting video\n");
		fflush(stdout);
		video_start(c, in.width, in.height);
	}
}

static void on_top_half(struct hws_capture *c)
{
	unsigned int s = c->dev.quarter_slot[0];
	unsigned int next;

	if (c->top_pending) {
		pthread_mutex_lock(&c->lock);
		c->st.dropped++;
		pthread_mutex_unlock(&c->lock);
	}
	c->top_pending = true;
	c->top_slot = hws_user_pair_written(&c->dev, 0, s) ? (int)s : -1;
	pthread_mutex_lock(&c->lock);
	next = pick_slot_locked(c, s);
	pthread_mutex_unlock(&c->lock);
	hws_user_point_pair(&c->dev, 0, next);
}

static void on_bottom_half(struct hws_capture *c)
{
	unsigned int s = c->dev.quarter_slot[2];
	bool ok = c->top_pending && c->top_slot == (int)s &&
		  hws_user_pair_written(&c->dev, 1, s);
	uint64_t t = now_us();

	c->top_pending = false;
	/* Follow the top pair into the slot it now points at. */
	hws_user_point_pair(&c->dev, 1, c->dev.quarter_slot[0]);

	pthread_mutex_lock(&c->lock);
	if (!ok) {
		c->st.dropped++;
	} else {
		c->latest_slot = (int)s;
		c->latest_seq++;
		c->latest_ts = t;
		c->latest_w = c->dev.width;
		c->latest_h = c->dev.height;
		c->st.frames++;
		c->st.signal = true;
		c->last_frame_us = t;
		if (!c->rate_t0) {
			c->rate_t0 = t;
			c->rate_frames = 0;
		} else if (++c->rate_frames && t - c->rate_t0 >= 1000000) {
			c->st.fps = c->rate_frames * 1e6 / (double)(t - c->rate_t0);
			c->rate_t0 = t;
			c->rate_frames = 0;
		}
		pthread_cond_broadcast(&c->cond);
	}
	pthread_mutex_unlock(&c->lock);
}

static void *capture_thread(void *arg)
{
	struct hws_capture *c = arg;
	uint64_t last_poll = 0;

	while (!c->stop) {
		struct pollfd pfd = { .fd = c->dev.vfio.irq_efd, .events = POLLIN };
		struct hws_user_events ev;
		uint64_t cnt;
		int pr = poll(&pfd, 1, 20);

		if (pr > 0)
			(void)!read(c->dev.vfio.irq_efd, &cnt, sizeof(cnt));
		/* Service on timeouts too, in case an MSI went missing. */
		hws_user_service_irq(&c->dev, &ev);
		if (ev.gone) {
			fprintf(stderr, "[HWS-USER][ERROR] card stopped answering (register 0 all ones)\n");
			pthread_mutex_lock(&c->lock);
			c->st.failed = true;
			c->st.signal = false;
			c->latest_slot = -1;
			pthread_cond_broadcast(&c->cond);
			pthread_mutex_unlock(&c->lock);
			break;
		}
		if (c->live) {
			for (unsigned int i = 0; i < ev.nhalves; i++) {
				if (ev.halves[i] == 1)
					on_top_half(c);
				else
					on_bottom_half(c);
			}
		}
		for (unsigned int i = 0; i < ev.naudio; i++) {
			if (c->audio_skip) {
				c->audio_skip--;
				continue;
			}
			pthread_mutex_lock(&c->lock);
			c->st.audio_packets++;
			pthread_mutex_unlock(&c->lock);
			if (c->cfg.audio_cb)
				c->cfg.audio_cb(hws_user_audio_packet(&c->dev, ev.audio[i]),
						HWS_AUDIO_PACKET_BYTES, c->cfg.user);
		}
		if (now_us() - last_poll >= INPUT_POLL_US) {
			last_poll = now_us();
			follow_input(c);
		}
	}
	return NULL;
}

int hws_capture_start(struct hws_capture **out, const struct hws_capture_cfg *cfg)
{
	struct hws_capture *c = calloc(1, sizeof(*c));
	int ret;

	if (!c)
		return -ENOMEM;
	c->cfg = *cfg;
	pthread_mutex_init(&c->lock, NULL);
	pthread_cond_init(&c->cond, NULL);
	c->latest_slot = -1;
	for (int i = 0; i < HWS_CAPTURE_MAX_CONSUMERS; i++)
		c->held[i] = -1;

	ret = hws_user_open(&c->dev, cfg->bdf,
			    cfg->nslots ? cfg->nslots : HWS_CAPTURE_DEFAULT_SLOTS);
	if (ret < 0) {
		free(c);
		return ret;
	}
	if (c->dev.nslots < HWS_CAPTURE_DEFAULT_SLOTS)
		fprintf(stderr, "[HWS-USER][WARN] %u slots: held frames may be overwritten (5 recommended)\n",
			c->dev.nslots);
	snprintf(c->st.bdf, sizeof(c->st.bdf), "%.31s", c->dev.vfio.pci_addr);
	c->st.fpga_version = c->dev.fpga_version;
	c->st.nslots = c->dev.nslots;
	hws_user_link_string(&c->dev, c->st.link, sizeof(c->st.link));

	if (pthread_create(&c->thread, NULL, capture_thread, c) != 0) {
		hws_user_close(&c->dev);
		free(c);
		return -EAGAIN;
	}
	c->thread_started = true;
	*out = c;
	return 0;
}

void hws_capture_stop(struct hws_capture *c)
{
	if (!c)
		return;
	c->stop = true;
	if (c->thread_started)
		pthread_join(c->thread, NULL);
	pthread_mutex_lock(&c->lock);
	pthread_cond_broadcast(&c->cond);
	pthread_mutex_unlock(&c->lock);
	hws_user_close(&c->dev);
	pthread_cond_destroy(&c->cond);
	pthread_mutex_destroy(&c->lock);
	free(c);
}

int hws_capture_acquire(struct hws_capture *c, int consumer, uint64_t after_seq,
			int timeout_ms, struct hws_frame_ref *out)
{
	struct timespec dl;
	int ret = 0;

	if (consumer < 0 || consumer >= HWS_CAPTURE_MAX_CONSUMERS)
		return -EINVAL;
	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += timeout_ms / 1000;
	dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (dl.tv_nsec >= 1000000000L) {
		dl.tv_sec++;
		dl.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&c->lock);
	c->held[consumer] = -1;
	while (!c->stop && (c->latest_slot < 0 || c->latest_seq <= after_seq)) {
		if (pthread_cond_timedwait(&c->cond, &c->lock, &dl) == ETIMEDOUT) {
			ret = -ETIMEDOUT;
			break;
		}
	}
	if (!ret && (c->stop || c->latest_slot < 0))
		ret = -ETIMEDOUT;
	if (!ret) {
		c->held[consumer] = c->latest_slot;
		out->slot = c->latest_slot;
		out->seq = c->latest_seq;
		out->width = c->latest_w;
		out->height = c->latest_h;
		out->bytes = (size_t)c->latest_w * c->latest_h * 2;
		out->ts_us = c->latest_ts;
		out->data = hws_user_slot(&c->dev, (unsigned int)c->latest_slot);
	}
	pthread_mutex_unlock(&c->lock);
	return ret;
}

void hws_capture_release(struct hws_capture *c, int consumer)
{
	if (consumer < 0 || consumer >= HWS_CAPTURE_MAX_CONSUMERS)
		return;
	pthread_mutex_lock(&c->lock);
	c->held[consumer] = -1;
	pthread_mutex_unlock(&c->lock);
}

void hws_capture_get_status(struct hws_capture *c, struct hws_capture_status *st)
{
	pthread_mutex_lock(&c->lock);
	*st = c->st;
	if (!c->live)
		st->signal = false;
	pthread_mutex_unlock(&c->lock);
}

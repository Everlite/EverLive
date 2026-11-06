/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_framegen.c — Phase-Locked 2:1 HDMI parity deduplicator tests
 *
 * The PS5 does not send a 30 Hz signal for a 30 FPS game: it sends 59.94/60 Hz
 * with every game frame repeated (A, A, B, B), so EverLive's 30to60 mode must
 * emit exactly one frame per game frame or lsfg-vk's 2x would still look like
 * 30 FPS.  These tests exercise the parity lock, the noise floor, the
 * instantaneous phase-slip recovery and the frozen-scene heartbeat.
 *
 * No display, no Vulkan, no hardware: pure CPU cadence classifier.
 */

#include "gc573_user_framegen.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_W		320
#define TEST_H		180
#define TEST_FRAME_US	16667U
#define TEST_START_US	1000000U

static int failures;

static void check(int condition, const char *what, const char *detail)
{
	if (condition) {
		printf("  [ok]   %s\n", what);
		return;
	}
	printf("  [FAIL] %s%s%s\n", what, detail && *detail ? " — " : "",
	       detail ? detail : "");
	failures++;
}

static int luma_at(int x, int y, int bar_x, int bias)
{
	int base = 32 + (y * 80) / TEST_H + bias;

	if (x >= bar_x && x < bar_x + 28)
		return 225;
	if (y > TEST_H - 30 && x > 20 && x < 120)
		return 190;	/* static HUD block */
	if (base < 0)
		base = 0;
	if (base > 255)
		base = 255;
	return base;
}

static void fill_yuyv(uint8_t *buf, int bar_x, int bias)
{
	for (int y = 0; y < TEST_H; y++) {
		uint8_t *row = buf + (size_t)y * TEST_W * 2U;

		for (int x = 0; x < TEST_W; x += 2) {
			row[x * 2 + 0] = (uint8_t)luma_at(x, y, bar_x, bias);
			row[x * 2 + 1] = 128;
			row[x * 2 + 2] = (uint8_t)luma_at(x + 1, y, bar_x, bias);
			row[x * 2 + 3] = 128;
		}
	}
}

/* Deterministic dither/grain, emulating the console capture chain. */
static uint32_t g_lcg = 0x12345678u;

static int noise_sample(int amp)
{
	g_lcg = g_lcg * 1664525u + 1013904223u;
	return (int)((g_lcg >> 16) % (uint32_t)(2 * amp + 1)) - amp;
}

static uint8_t clamp_u8(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return (uint8_t)v;
}

struct wait_probe {
	struct gc573_framegen *fg;
	uint8_t *frame;
	int sent;
};

static void *wait_producer(void *arg)
{
	struct wait_probe *p = arg;
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 5000000L };

	nanosleep(&delay, NULL);
	fill_yuyv(p->frame, 100, 0);
	gc573_framegen_input(p->fg, p->frame, 0);
	p->sent = 1;
	return NULL;
}

static void fill_yuyv_noisy(uint8_t *buf, int bar_x, int amp)
{
	for (int y = 0; y < TEST_H; y++) {
		uint8_t *row = buf + (size_t)y * TEST_W * 2U;

		for (int x = 0; x < TEST_W; x += 2) {
			row[x * 2 + 0] = clamp_u8(luma_at(x, y, bar_x, 0) +
						  noise_sample(amp));
			row[x * 2 + 1] = 128;
			row[x * 2 + 2] = clamp_u8(luma_at(x + 1, y, bar_x, 0) +
						  noise_sample(amp));
			row[x * 2 + 3] = 128;
		}
	}
}

int main(void)
{
	struct gc573_fg_cadence cad;
	uint8_t *frame;
	uint64_t t = TEST_START_US;

	frame = malloc((size_t)TEST_W * TEST_H * 2U);
	if (!frame) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	printf("EverLive Experimental — phase-locked 2:1 deduplicator self-test\n\n");

	/* ---------------------------------------------------------------- */
	printf("1) Clean 30-in-60 HDMI container (A, A, B, B, ...)\n");
	{
		int accepted = 0;
		int rejected = 0;
		double fps;

		gc573_fg_cadence_init(&cad, TEST_W, TEST_H);
		for (int i = 0; i < 30; i++) {
			for (int rep = 0; rep < 2; rep++) {
				int cls;

				fill_yuyv(frame, 20 + i * 9, 0);
				cls = gc573_fg_cadence_classify(&cad, frame, t);
				if (cls == GC573_FG_UNIQUE)
					accepted++;
				else
					rejected++;
				t += TEST_FRAME_US;
			}
		}
		fps = gc573_fg_cadence_unique_fps(&cad);
		printf("  raw %u, accepted %d, rejected %d, unique FPS %.2f\n",
		       cad.raw_frames, accepted, rejected, fps);
		{
			char d[160];

			snprintf(d, sizeof(d),
				 "raw=%u unique=%u dup=%u flips=%u ema even=%.2f odd=%.2f",
				 cad.raw_frames, cad.unique_frames, cad.duplicates,
				 cad.parity_flips, (double)cad.even_mad_ema,
				 (double)cad.odd_mad_ema);
			check(cad.raw_frames == 60, "60 raw HDMI frames counted", d);
			check(accepted == 30 && rejected == 30,
			      "exact 30/60 accepted (steady 2:1 cadence)", d);
			check(fps > 28.5 && fps < 31.5,
			      "unique cadence reported as ~30.0 FPS", d);
		}
		gc573_fg_cadence_free(&cad);
	}

	/* ---------------------------------------------------------------- */
	printf("\n2) Noisy/dithered 30-in-60 (grain on every HDMI repeat)\n");
	{
		int accepted = 0;
		int rejected = 0;
		double fps;

		gc573_fg_cadence_init(&cad, TEST_W, TEST_H);
		g_lcg = 0x12345678u;
		for (int i = 0; i < 30; i++) {
			for (int rep = 0; rep < 2; rep++) {
				int cls;

				fill_yuyv_noisy(frame, 20 + (i * 9) % 260, 2);
				cls = gc573_fg_cadence_classify(&cad, frame, t);
				if (cls == GC573_FG_UNIQUE)
					accepted++;
				else
					rejected++;
				t += TEST_FRAME_US;
			}
		}
		fps = gc573_fg_cadence_unique_fps(&cad);
		printf("  raw %u, accepted %d, rejected %d, unique FPS %.2f, "
		       "even_ema %.2f, odd_ema %.2f\n",
		       cad.raw_frames, accepted, rejected, fps,
		       (double)cad.even_mad_ema, (double)cad.odd_mad_ema);
		check(cad.raw_frames == 60, "60 noisy raw frames counted", NULL);
		check(accepted == 30 && rejected == 30,
		      "dither/grain does not break the 2:1 lock", NULL);
		check(cad.parity_flips <= 2, "parity stays locked under noise",
		      NULL);
		gc573_fg_cadence_free(&cad);
	}

	/* ---------------------------------------------------------------- */
	printf("\n3) Phase slip A, A, A, B, B, C, C\n");
	{
		static const int bars[] = { 20, 20, 20, 150, 150, 240, 240 };
		int accepted[7] = { 0 };
		int count = 0;

		gc573_fg_cadence_init(&cad, TEST_W, TEST_H);
		for (int i = 0; i < 7; i++) {
			int cls;

			fill_yuyv(frame, bars[i], 0);
			cls = gc573_fg_cadence_classify(&cad, frame, t);
			accepted[i] = (cls == GC573_FG_UNIQUE);
			if (accepted[i])
				count++;
			t += TEST_FRAME_US;
		}

		printf("  accepted: %d%d%d%d%d%d%d (count %d), parity=%d, flips=%u\n",
		       accepted[0], accepted[1], accepted[2], accepted[3],
		       accepted[4], accepted[5], accepted[6], count,
		       cad.active_parity, cad.parity_flips);

		check(accepted[3], "first B frame is not lost on the slip", NULL);
		check(!accepted[4], "second B frame is recognised as the repeat",
		      NULL);
		check(accepted[5], "first C frame is presented", NULL);
		check(!accepted[6], "second C frame is recognised as the repeat",
		      NULL);
		check(cad.active_parity == 1,
		      "parity re-locked onto the new phase", NULL);
		check(cad.parity_flips >= 1, "parity flip was recorded", NULL);
		check(count >= 3 && count <= 4,
		      "at most one redundant frame during the slip", NULL);

		/* Continue with a normal A, A, B, B stream: 2:1 must resume. */
		{
			int steady = 0;
			int steady_raw = 0;

			for (int i = 0; i < 20; i++) {
				for (int rep = 0; rep < 2; rep++) {
					fill_yuyv(frame, 20 + (i * 7) % 260, 0);
					if (gc573_fg_cadence_classify(
						    &cad, frame, t) ==
					    GC573_FG_UNIQUE)
						steady++;
					steady_raw++;
					t += TEST_FRAME_US;
				}
			}
			printf("  after slip: %d unique / %d raw\n", steady,
			       steady_raw);
			check(steady == 20, "2:1 cadence resumes after the slip",
			      NULL);
		}
		gc573_fg_cadence_free(&cad);
	}

	/* ---------------------------------------------------------------- */
	printf("\n4) Frozen scene (no motion at all)\n");
	{
		int accepted = 0;
		int rejected = 0;
		double fps;

		gc573_fg_cadence_init(&cad, TEST_W, TEST_H);
		fill_yuyv(frame, 40, 0);
		for (int i = 0; i < 60; i++) {
			if (gc573_fg_cadence_classify(&cad, frame, t) ==
			    GC573_FG_UNIQUE)
				accepted++;
			else
				rejected++;
			t += TEST_FRAME_US;
		}
		fps = gc573_fg_cadence_unique_fps(&cad);
		printf("  accepted %d, rejected %d, unique FPS %.2f\n",
		       accepted, rejected, fps);
		check(accepted == 30 && rejected == 30,
		      "frozen 30-in-60 keeps a steady 2:1 heartbeat", NULL);
		check(fps > 28.5 && fps < 31.5,
		      "frozen heartbeat stays at ~30.0 FPS", NULL);
		check(cad.parity_flips <= 1,
		      "frozen scene never flips the parity", NULL);
		gc573_fg_cadence_free(&cad);
	}

	/* ---------------------------------------------------------------- */
	printf("\n5) Ring accept/present contract\n");
	{
		struct gc573_fg_config cfg = {
			.mode = GC573_FG_60_TO_120,
			.flow_scale = 1.0f,
			.perf_mode = false,
		};
		struct gc573_framegen *fg = gc573_framegen_create(&cfg);

		if (!fg) {
			check(0, "framegen ring allocation", NULL);
		} else {
			struct gc573_fg_stats st;
			const uint8_t *out = NULL;
			int takes = 0;

			gc573_framegen_resize(fg, TEST_W, TEST_H);
			gc573_framegen_set_source_hz(fg, 5994);
			for (int i = 0; i < 3; i++) {
				fill_yuyv(frame, 20 + i * 40, 0);
				gc573_framegen_input(fg, frame, t);
				t += TEST_FRAME_US;
			}
			gc573_framegen_get_stats(fg, &st);
			check(st.queued == 3,
			      "60to120 queues every captured frame", NULL);

			while (gc573_framegen_take(fg, &out) && takes < 10) {
				takes++;
				gc573_framegen_release(fg);
			}
			check(takes == 1,
			      "take() coalesces to exactly one present", NULL);

			/* Fast HDMI source in 30to60: A, A, B -> two accepted. */
			cfg.mode = GC573_FG_30_TO_60;
			gc573_framegen_set_config(fg, &cfg);
			for (int i = 0; i < 3; i++) {
				fill_yuyv(frame, 20 + i * 40, 0);
				/* Frame 1 repeats frame 0. */
				if (i == 1)
					fill_yuyv(frame, 20, 0);
				gc573_framegen_input(fg, frame, t);
				t += TEST_FRAME_US;
			}
			gc573_framegen_get_stats(fg, &st);
			check(st.queued == 2,
			      "30to60 drops the repeated HDMI frame", NULL);
			gc573_framegen_destroy(fg);
		}
	}

	/* ---------------------------------------------------------------- */
	printf("\n6) Low-latency swapchain (env + profile)\n");
	{
		struct gc573_fg_probe probe;
		struct gc573_fg_config cfg = {
			.mode = GC573_FG_60_TO_120,
			.flow_scale = 1.0f,
			.perf_mode = false,
			.low_latency = true,
		};
		char tmpl[] = "/tmp/everlive-fgtest.XXXXXX";
		char home_saved[512] = "";
		const char *home_env = getenv("HOME");
		char *home;

		if (home_env)
			snprintf(home_saved, sizeof(home_saved), "%s", home_env);

		(void)gc573_fg_apply_env(&cfg, &probe);
		{
			const char *v = getenv("LSFGVK_PRESERVE_SWAPCHAIN_IMAGE_COUNT");

			check(v && strcmp(v, "1") == 0,
			      "low_latency exports PRESERVE_SWAPCHAIN_IMAGE_COUNT=1",
			      NULL);
		}

		cfg.low_latency = false;
		(void)gc573_fg_apply_env(&cfg, &probe);
		check(getenv("LSFGVK_PRESERVE_SWAPCHAIN_IMAGE_COUNT") == NULL,
		      "low_latency=false unsets the variable", NULL);

		cfg.mode = GC573_FG_OFF;
		cfg.low_latency = true;
		(void)gc573_fg_apply_env(&cfg, &probe);
		check(getenv("LSFGVK_PRESERVE_SWAPCHAIN_IMAGE_COUNT") == NULL,
		      "GC573_FG_OFF leaves the variable unset", NULL);

		home = mkdtemp(tmpl);
		if (!home) {
			check(0, "mkdtemp for profile test", NULL);
		} else {
			char path[768];
			char buf[4096];
			FILE *f;
			size_t n;

			setenv("HOME", home, 1);
			snprintf(path, sizeof(path),
				 "%s/.config/everlive-experimental/lsfg.toml", home);

			cfg.mode = GC573_FG_60_TO_120;
			cfg.low_latency = true;
			gc573_fg_write_toml(&cfg, &probe);
			f = fopen(path, "r");
			n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
			if (f)
				fclose(f);
			buf[n] = '\0';
			check(n > 0 && strstr(buf,
				"preserve_swapchain_image_count = true") != NULL,
			      "lsfg.toml emits preserve_swapchain_image_count = true",
			      NULL);

			cfg.low_latency = false;
			gc573_fg_write_toml(&cfg, &probe);
			f = fopen(path, "r");
			n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
			if (f)
				fclose(f);
			buf[n] = '\0';
			check(n > 0 && strstr(buf,
				"preserve_swapchain_image_count = false") != NULL,
			      "lsfg.toml mirrors low_latency=false", NULL);

			if (home_saved[0])
				setenv("HOME", home_saved, 1);
			else
				unsetenv("HOME");
		}
	}

	/* ---------------------------------------------------------------- */
	printf("\n7) Event-driven wait() wakeup\n");
	{
		struct gc573_fg_config cfg = {
			.mode = GC573_FG_60_TO_120,
			.flow_scale = 1.0f,
			.perf_mode = false,
			.low_latency = true,
		};
		struct gc573_framegen *fg = gc573_framegen_create(&cfg);

		if (!fg) {
			check(0, "framegen ring for wait test", NULL);
		} else {
			struct timespec a, b;
			long elapsed_ms;
			bool ready;

			gc573_framegen_resize(fg, TEST_W, TEST_H);
			gc573_framegen_set_source_hz(fg, 5994);

			clock_gettime(CLOCK_MONOTONIC, &a);
			ready = gc573_framegen_wait(fg, 20000);
			clock_gettime(CLOCK_MONOTONIC, &b);
			elapsed_ms = (long)((b.tv_sec - a.tv_sec) * 1000L +
					    (b.tv_nsec - a.tv_nsec) / 1000000L);
			check(!ready, "wait() returns false when idle", NULL);
			check(elapsed_ms >= 15 && elapsed_ms < 500,
			      "idle wait honours the ~20 ms timeout", NULL);

			{
				struct wait_probe wp = {
					.fg = fg,
					.frame = frame,
					.sent = 0,
				};
				pthread_t producer;
				const uint8_t *out = NULL;

				if (pthread_create(&producer, NULL, wait_producer,
						   &wp) != 0) {
					check(0, "producer thread", NULL);
				} else {
					clock_gettime(CLOCK_MONOTONIC, &a);
					ready = gc573_framegen_wait(fg, 500000);
					clock_gettime(CLOCK_MONOTONIC, &b);
					pthread_join(producer, NULL);
					elapsed_ms = (long)((b.tv_sec - a.tv_sec) *
							    1000L +
							    (b.tv_nsec - a.tv_nsec) /
							    1000000L);
					check(ready && wp.sent,
					      "wait() wakes on a producer frame", NULL);
					check(elapsed_ms < 200,
					      "wakeup is prompt (< 200 ms)", NULL);
					check(gc573_framegen_take(fg, &out) && out,
					      "frame is takeable after wait()", NULL);
					gc573_framegen_release(fg);
				}
			}
			gc573_framegen_destroy(fg);
		}
	}

	printf("\n%s\n", failures ? "FAILED" : "All deduplicator checks passed.");
	free(frame);
	return failures ? 1 : 0;
}

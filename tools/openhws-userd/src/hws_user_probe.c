// SPDX-License-Identifier: GPL-2.0-only
/*
 * hws_user_probe.c — bring-up and validation tool for the HWS UHD family
 * (ACASIS VS2583 / AVMatrix VC12-4K) under vfio-pci.
 *
 * Opens the card through VFIO, reports identity, link and input, then runs
 * zero-copy video DMA (rotating frame slots) and HDMI audio DMA for a while
 * and reports frame and audio rates, dropped/torn frames and IRQ health.
 * Optionally writes the last captured frame (.yuv and .ppm) and the audio
 * (.wav).
 *
 *   hws_user_probe [BDF] [--seconds N] [--slots N] [--frame PREFIX]
 *                  [--wav FILE] [--no-audio] [--regs] [--wait N]
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hws_user_hw.h"

#define AUDIO_STALE_PACKETS	2	/* old packets the card replays after an enable */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [BDF] [options]\n"
		"  --seconds N     capture for N seconds (default 10)\n"
		"  --slots N       frame slots in the DMA ring, %d..%d (default 3)\n"
		"  --frame PREFIX  write the last good frame to PREFIX.yuv and PREFIX.ppm\n"
		"  --wav FILE      write captured HDMI audio to FILE\n"
		"  --no-audio      do not enable audio DMA\n"
		"  --wait N        wait up to N seconds for a signal (default 10)\n"
		"  --regs          print identity, link and input registers, then exit\n",
		argv0, HWS_USER_MIN_SLOTS, HWS_USER_MAX_SLOTS);
}

static void print_input(const struct hws_user_input *in)
{
	if (!in->present) {
		printf("  input       : no signal\n");
		return;
	}
	printf("  input       : %ux%u%s @ %u fps%s\n", in->width,
	       in->interlaced ? in->height * 2 : in->height,
	       in->interlaced ? "i" : "p", in->fps,
	       in->hdcp ? "  [HDCP active: card withholds video]" : "");
}

static void dump_regs(struct hws_user_dev *dev)
{
	printf("  status (r0) : 0x%08x\n", hws_user_rd(dev, HWS_REG_STATUS));
	printf("  int    (r1) : 0x%08x\n", hws_user_rd(dev, HWS_REG_INT_STATUS));
	printf("  venable(r2) : 0x%08x\n", hws_user_rd(dev, HWS_REG_VIDEO_ENABLE));
	printf("  aenable(r3) : 0x%08x\n", hws_user_rd(dev, HWS_REG_AUDIO_ENABLE));
	printf("  r4          : 0x%08x\n", hws_user_rd(dev, HWS_CAP(4)));
	printf("  signal (r5) : 0x%08x\n", hws_user_rd(dev, HWS_REG_SIGNAL));
	printf("  hdcp   (r8) : 0x%08x\n", hws_user_rd(dev, HWS_REG_HDCP));
	printf("  dmamax (r9) : 0x%08x\n", hws_user_rd(dev, HWS_REG_DMA_MAX));
	printf("  devinfo(r88): 0x%08x\n", hws_user_rd(dev, HWS_REG_DEVINFO));
	printf("  insize (r90): 0x%08x\n", hws_user_rd(dev, HWS_REG_INPUT_SIZE));
	printf("  picture(r91): 0x%08x\n", hws_user_rd(dev, HWS_REG_PICTURE));
	printf("  infps (r110): 0x%08x\n", hws_user_rd(dev, HWS_REG_INPUT_FPS));
}

static inline uint8_t clamp8(float v)
{
	return v < 0.f ? 0 : v > 255.f ? 255 : (uint8_t)lrintf(v);
}

/* YUYV -> binary PPM, BT.709 full range (the card's default output). */
static int write_ppm(const char *path, const uint8_t *yuyv, unsigned int w, unsigned int h)
{
	FILE *f = fopen(path, "wb");
	uint8_t *row;

	if (!f)
		return -errno;
	row = malloc((size_t)w * 3);
	if (!row) {
		fclose(f);
		return -ENOMEM;
	}
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (unsigned int y = 0; y < h; y++) {
		const uint8_t *s = yuyv + (size_t)y * w * 2;

		for (unsigned int x = 0; x < w; x += 2, s += 4) {
			float u = s[1] - 128.f, v = s[3] - 128.f;
			float y0 = s[0], y1 = s[2];
			float dr = 1.5748f * v, dg = -0.1873f * u - 0.4681f * v, db = 1.8556f * u;
			uint8_t *d = row + (size_t)x * 3;

			d[0] = clamp8(y0 + dr); d[1] = clamp8(y0 + dg); d[2] = clamp8(y0 + db);
			d[3] = clamp8(y1 + dr); d[4] = clamp8(y1 + dg); d[5] = clamp8(y1 + db);
		}
		fwrite(row, 3, w, f);
	}
	free(row);
	return fclose(f) == 0 ? 0 : -errno;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = v; p[1] = v >> 8;
}

static void write_wav_header(FILE *f, uint32_t data_bytes)
{
	uint8_t h[44];

	memcpy(h, "RIFF", 4);
	put_le32(h + 4, 36 + data_bytes);
	memcpy(h + 8, "WAVEfmt ", 8);
	put_le32(h + 16, 16);
	put_le16(h + 20, 1);
	put_le16(h + 22, HWS_AUDIO_CHANNELS);
	put_le32(h + 24, HWS_AUDIO_RATE);
	put_le32(h + 28, HWS_AUDIO_RATE * HWS_AUDIO_CHANNELS * 2);
	put_le16(h + 32, HWS_AUDIO_CHANNELS * 2);
	put_le16(h + 34, 16);
	memcpy(h + 36, "data", 4);
	put_le32(h + 40, data_bytes);
	rewind(f);
	fwrite(h, 1, sizeof(h), f);
}

struct probe_stats {
	uint64_t irqs, spurious, halves, frames, dropped_top, dropped_bottom, torn;
	uint64_t audio_packets, audio_skipped, size_changes;
	uint64_t first_frame_us, last_frame_us, rate_frames;	/* since the last (re)start */
	uint64_t max_service_us;
	uint32_t unknown_bits;
	int audio_peak;
};

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "seconds", required_argument, NULL, 's' },
		{ "slots", required_argument, NULL, 'n' },
		{ "frame", required_argument, NULL, 'f' },
		{ "wav", required_argument, NULL, 'w' },
		{ "no-audio", no_argument, NULL, 'A' },
		{ "wait", required_argument, NULL, 'W' },
		{ "regs", no_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct hws_user_dev dev;
	struct hws_user_input in;
	struct probe_stats st = { 0 };
	const char *bdf = NULL, *frame_prefix = NULL, *wav_path = NULL;
	unsigned int seconds = 10, slots = 3, wait_s = 10;
	bool audio = true, regs_only = false;
	uint8_t *snap = NULL;
	unsigned int snap_w = 0, snap_h = 0;
	FILE *wav = NULL;
	uint32_t wav_bytes = 0;
	char link[32];
	int c, ret;

	while ((c = getopt_long(argc, argv, "s:n:f:w:AW:rh", opts, NULL)) != -1) {
		switch (c) {
		case 's': seconds = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'n': slots = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'f': frame_prefix = optarg; break;
		case 'w': wav_path = optarg; break;
		case 'A': audio = false; break;
		case 'W': wait_s = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'r': regs_only = true; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	if (optind < argc)
		bdf = argv[optind];

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	ret = hws_user_open(&dev, bdf, slots);
	if (ret < 0) {
		fprintf(stderr, "[HWS] open failed: %s\n", strerror(-ret));
		return 1;
	}
	hws_user_link_string(&dev, link, sizeof(link));
	printf("HWS UHD card %s (device 0x%04x)\n", dev.vfio.pci_addr, dev.device_id);
	printf("  FPGA        : version %u, sub-version %u (devinfo 0x%08x)\n",
	       dev.fpga_version, dev.fpga_subversion, dev.devinfo);
	printf("  PCIe link   : %s\n", link);
	printf("  DMA arena   : %u slots x %zu KiB + audio, IOVA 0x%" PRIx64 "\n",
	       dev.nslots, dev.slot_stride / 1024, (uint64_t)HWS_USER_IOVA_BASE);
	hws_user_read_input(&dev, &in);
	print_input(&in);
	if (regs_only) {
		dump_regs(&dev);
		hws_user_close(&dev);
		return 0;
	}

	/* Wait for a usable progressive input. */
	for (uint64_t t0 = now_us(); !g_stop; usleep(100 * 1000)) {
		if (hws_user_read_input(&dev, &in))
			break;
		if (now_us() - t0 > (uint64_t)wait_s * 1000000ULL) {
			fprintf(stderr, "[HWS] no usable signal after %u s", wait_s);
			if (in.present && in.interlaced)
				fprintf(stderr, " (interlaced input is not supported by the probe)");
			fprintf(stderr, "\n");
			hws_user_close(&dev);
			return 1;
		}
	}
	if (g_stop) {
		hws_user_close(&dev);
		return 1;
	}
	print_input(&in);

	if (frame_prefix) {
		snap = malloc((size_t)HWS_MAX_WIDTH * HWS_MAX_HEIGHT * 2);
		if (!snap) {
			hws_user_close(&dev);
			return 1;
		}
	}
	if (wav_path && audio) {
		wav = fopen(wav_path, "w+b");
		if (!wav)
			fprintf(stderr, "[HWS] cannot write %s: %s\n", wav_path, strerror(errno));
		else
			write_wav_header(wav, 0);
	}

	ret = hws_user_set_geometry(&dev, in.width, in.height);
	if (!ret)
		ret = hws_user_start(&dev, audio);
	if (ret < 0) {
		fprintf(stderr, "[HWS] start failed: %s\n", strerror(-ret));
		hws_user_close(&dev);
		return 1;
	}
	printf("Capturing %ux%u for %u s (%u slots, audio %s)...\n",
	       dev.width, dev.height, seconds, dev.nslots, audio ? "on" : "off");

	bool top_pending = false;
	int top_slot = -1;
	unsigned int audio_skip = AUDIO_STALE_PACKETS;
	uint64_t start = now_us(), last_poll = start, last_frame_seen = start;

	while (!g_stop && now_us() - start < (uint64_t)seconds * 1000000ULL) {
		struct pollfd pfd = { .fd = dev.vfio.irq_efd, .events = POLLIN };
		struct hws_user_events ev;
		uint64_t cnt, t_irq;
		int pr = poll(&pfd, 1, 50);

		if (pr < 0 && errno != EINTR)
			break;
		if (pr > 0 && read(dev.vfio.irq_efd, &cnt, sizeof(cnt)) == sizeof(cnt))
			st.irqs++;
		t_irq = now_us();

		/* Service on every wake, also on a timeout, in case an MSI went missing. */
		hws_user_service_irq(&dev, &ev);
		if (ev.gone) {
			fprintf(stderr, "[HWS] card stopped answering (register 0 all ones)\n");
			break;
		}
		if (ev.spurious && pr > 0)
			st.spurious++;
		st.unknown_bits |= ev.unknown_bits;

		for (unsigned int i = 0; i < ev.nhalves; i++) {
			st.halves++;
			if (ev.halves[i] == 1) {
				/* Top half: quarters 0,1 of their slot are done. */
				unsigned int s = dev.quarter_slot[0];

				if (top_pending)
					st.dropped_bottom++;	/* previous frame lost its bottom half */
				top_pending = true;
				top_slot = hws_user_pair_written(&dev, 0, s) ? (int)s : -1;
				hws_user_point_pair(&dev, 0, (s + 1) % dev.nslots);
			} else {
				/* Bottom half: quarters 2,3 done; the frame in that slot is complete. */
				unsigned int s = dev.quarter_slot[2];
				bool ok = hws_user_pair_written(&dev, 1, s);

				if (!top_pending) {
					st.dropped_top++;
				} else if (top_slot != (int)s || !ok) {
					st.torn++;
				} else {
					uint64_t t = now_us();

					if (!st.rate_frames++)
						st.first_frame_us = t;
					st.last_frame_us = t;
					st.frames++;
					last_frame_seen = t;
					if (snap && (st.frames % 30 == 1 || !snap_w)) {
						memcpy(snap, hws_user_slot(&dev, s), dev.frame_bytes);
						snap_w = dev.width;
						snap_h = dev.height;
					}
				}
				top_pending = false;
				/* Follow the top pair into its new slot. */
				hws_user_point_pair(&dev, 1, dev.quarter_slot[0]);
			}
		}

		for (unsigned int i = 0; i < ev.naudio; i++) {
			const int16_t *pk = hws_user_audio_packet(&dev, ev.audio[i]);

			if (audio_skip) {
				audio_skip--;
				st.audio_skipped++;
				continue;
			}
			st.audio_packets++;
			for (unsigned int k = 0; k < HWS_AUDIO_PACKET_BYTES / 2; k++) {
				int a = abs(pk[k]);

				if (a > st.audio_peak)
					st.audio_peak = a;
			}
			if (wav) {
				fwrite(pk, 1, HWS_AUDIO_PACKET_BYTES, wav);
				wav_bytes += HWS_AUDIO_PACKET_BYTES;
			}
		}

		if (now_us() - t_irq > st.max_service_us)
			st.max_service_us = now_us() - t_irq;

		/* Every 250 ms: follow input size changes. */
		if (now_us() - last_poll > 250000) {
			struct hws_user_input cur;

			last_poll = now_us();
			if (hws_user_read_input(&dev, &cur) &&
			    (cur.width != dev.width || cur.height != dev.height)) {
				printf("  input changed to %ux%u, restarting video\n", cur.width, cur.height);
				st.size_changes++;
				hws_user_stop(&dev);
				if (hws_user_set_geometry(&dev, cur.width, cur.height) == 0)
					hws_user_start(&dev, audio);
				top_pending = false;
				audio_skip = AUDIO_STALE_PACKETS;
				st.rate_frames = 0;
			}
			if (now_us() - last_frame_seen > 2000000) {
				fprintf(stderr, "  no frame for 2 s (irqs %" PRIu64 ", halves %" PRIu64 ")\n",
					st.irqs, st.halves);
				last_frame_seen = now_us();
			}
		}
	}

	hws_user_stop(&dev);

	double span = st.rate_frames > 1 ? (st.last_frame_us - st.first_frame_us) / 1e6 : 0;
	double fps = span > 0 ? (st.rate_frames - 1) / span : 0;
	double secs = (now_us() - start) / 1e6;

	printf("\nResults over %.1f s\n", secs);
	printf("  interrupts  : %" PRIu64 " (%" PRIu64 " spurious), unknown bits 0x%x\n",
	       st.irqs, st.spurious, st.unknown_bits);
	printf("  video       : %" PRIu64 " frames, %.2f fps, %" PRIu64 " half events\n",
	       st.frames, fps, st.halves);
	printf("  lost        : %" PRIu64 " no-top, %" PRIu64 " no-bottom, %" PRIu64 " torn/unwritten\n",
	       st.dropped_top, st.dropped_bottom, st.torn);
	printf("  max service : %" PRIu64 " us per wake\n", st.max_service_us);
	if (audio)
		printf("  audio       : %" PRIu64 " packets (%.0f Hz effective), peak %d, %" PRIu64 " stale skipped\n",
		       st.audio_packets, secs > 0 ? st.audio_packets * 1024.0 / secs : 0,
		       st.audio_peak, st.audio_skipped);
	if (st.size_changes)
		printf("  size changes: %" PRIu64 "\n", st.size_changes);

	if (wav) {
		write_wav_header(wav, wav_bytes);
		fclose(wav);
		printf("  wrote %s (%u bytes of PCM)\n", wav_path, wav_bytes);
	}
	if (snap && snap_w) {
		char path[1024];
		FILE *f;

		snprintf(path, sizeof(path), "%s.yuv", frame_prefix);
		f = fopen(path, "wb");
		if (f) {
			fwrite(snap, 1, (size_t)snap_w * snap_h * 2, f);
			fclose(f);
			printf("  wrote %s (YUYV %ux%u)\n", path, snap_w, snap_h);
		}
		snprintf(path, sizeof(path), "%s.ppm", frame_prefix);
		if (write_ppm(path, snap, snap_w, snap_h) == 0)
			printf("  wrote %s\n", path);
	}
	free(snap);
	hws_user_close(&dev);
	return st.frames ? 0 : 1;
}

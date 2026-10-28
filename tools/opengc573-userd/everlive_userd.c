/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_userd.c — EverLive Userspace Hardware & LED Background Daemon
 *
 * Keeps the AVerMedia GC573 hardware initialized, holds the VFIO context open
 * to prevent PCI reset / standby blinking, and maintains the LED lighting.
 */

#include "gc573_vfio.h"
#include "gc573_user_hw.h"
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U +
	       (uint64_t)(ts.tv_nsec / 1000000L);
}

/* ------------------------------------------------------------------ */
/* Smooth colour cycle ("Soft Dream" / spectrum)                      */
/* ------------------------------------------------------------------ */

enum led_mode {
	LED_MODE_STATIC,
	LED_MODE_RAINBOW,
	LED_MODE_WAVE,
};

/* Full hue revolution time. Wave looks best at ~3.0s; whole-card breathing at ~8.0s */
#define WAVE_DEFAULT_SECONDS	3.0
#define RAINBOW_DEFAULT_SECONDS	8.0
#define RAINBOW_MIN_SECONDS	1.0
#define RAINBOW_MAX_SECONDS	120.0

/*
 * Never fully saturated and never at full brightness, so a step every 50 ms
 * reads as a soft glow instead of a blinking alarm light.
 */
#define RAINBOW_SATURATION	150U	/* 0..255, ~59 % */
#define RAINBOW_VALUE_MIN	150U	/* 0..255, ~59 % */
#define RAINBOW_VALUE_MAX	205U	/* 0..255, ~80 % */

/*
 * HSV -> RGB, integer only.
 *
 * @hue is a 0..1535 circle (6 sectors * 256), @sat and @val are 0..255.
 * Every intermediate fits in 32 bits: three multiplies, three divides and a
 * switch.  Cheap enough for a 20 Hz daemon (or a 1 kHz one).
 */
static void hsv_to_rgb(uint32_t hue, uint8_t sat, uint8_t val,
		       uint8_t *r, uint8_t *g, uint8_t *b)
{
	uint32_t region = (hue >> 8) % 6U;
	uint32_t rem = hue & 0xffU;
	uint8_t p = (uint8_t)((val * (255U - sat)) / 255U);
	uint8_t q = (uint8_t)((val * (255U - (sat * rem) / 255U)) / 255U);
	uint8_t t = (uint8_t)((val *
			      (255U - (sat * (255U - rem)) / 255U)) / 255U);

	switch (region) {
	case 0:
		*r = val;
		*g = t;
		*b = p;
		break;
	case 1:
		*r = q;
		*g = val;
		*b = p;
		break;
	case 2:
		*r = p;
		*g = val;
		*b = t;
		break;
	case 3:
		*r = p;
		*g = q;
		*b = val;
		break;
	case 4:
		*r = t;
		*g = p;
		*b = val;
		break;
	default:
		*r = val;
		*g = p;
		*b = q;
		break;
	}
}

/*
 * Advance the cycle by @elapsed_ms and return the next RGB triplet.
 *
 * The phase comes from a monotonic clock, so scheduling jitter in the 50 ms
 * loop never accumulates: one revolution always takes @cycle_ms.
 *
 * Brightness follows a slow triangle wave (one gentle breath per half
 * revolution) between RAINBOW_VALUE_MIN and RAINBOW_VALUE_MAX.  No libm, no
 * floating point, no allocation.
 */
static void rainbow_tick(uint64_t elapsed_ms, uint32_t cycle_ms,
			 uint8_t *r, uint8_t *g, uint8_t *b)
{
	uint32_t pos = cycle_ms ? (uint32_t)(elapsed_ms % cycle_ms) : 0U;
	uint32_t hue = (uint32_t)(((uint64_t)pos * 1536U) / cycle_ms);
	uint32_t half = cycle_ms / 2U;
	uint32_t level, breath, val;

	if (!half)
		half = 1U;

	/* 0..254 ramp over half a revolution, folded into a 0..255 triangle. */
	level = (uint32_t)(((uint64_t)(pos % half) * 255U) / half);
	breath = (level <= 127U) ? level * 2U : (255U - level) * 2U;
	val = RAINBOW_VALUE_MIN +
	      ((RAINBOW_VALUE_MAX - RAINBOW_VALUE_MIN) * breath) / 255U;

	hsv_to_rgb(hue, RAINBOW_SATURATION, (uint8_t)val, r, g, b);
}

/*
 * Calculate 5 phase-shifted RGB colors across the card zones for the multi-zone Wave effect.
 */
static void wave_tick(uint64_t elapsed_ms, uint32_t cycle_ms, uint32_t colors[5])
{
	for (int i = 0; i < 5; i++) {
		/* Each zone is offset by 1/5th (307 / 1536) of the color wheel */
		uint64_t zone_elapsed = elapsed_ms + ((uint64_t)i * cycle_ms) / 5U;
		uint8_t r, g, b;
		rainbow_tick(zone_elapsed, cycle_ms, &r, &g, &b);
		colors[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
	}
}

/*
 * Parse a --cycle argument.  A plain number is always consumed (out-of-range
 * values warn and keep the current setting), anything else - e.g. a PCI
 * address that followed the flag - is left for the normal argument parser.
 */
static bool parse_cycle_seconds(const char *text, double *out)
{
	char *end = NULL;
	double value;

	if (!text || !*text)
		return false;
	value = strtod(text, &end);
	if (end == text || *end != '\0')
		return false;

	if (!(value >= RAINBOW_MIN_SECONDS && value <= RAINBOW_MAX_SECONDS)) {
		fprintf(stderr, "[everlive-userd] --cycle %.1f s outside %.1f-%.0f s; "
			"keeping %.1f s\n", value, RAINBOW_MIN_SECONDS,
			RAINBOW_MAX_SECONDS, *out);
		return true;
	}

	*out = value;
	return true;
}

static void usage(const char *prog)
{
	printf("Usage: %s [PCI_ADDRESS] [OPTIONS]\n"
	       "  --color R G B        hold a static RGB colour (0-255 each)\n"
	       "  --off                switch the LED off\n"
	       "  --rainbow            smooth full-spectrum cycle (%.0f s)\n"
	       "  --wave               multi-zone rainbow wave (%.1f s)\n"
	       "  --cycle [SECONDS]    cycle duration, %.1f-%.0f s per revolution\n"
	       "  -d, --daemon         detach into the background\n"
	       "  --inherit C G D OFF SIZE\n"
	       "                       adopt the streamer's VFIO context\n"
	       "  -h, --help           show this help\n",
	       prog, RAINBOW_DEFAULT_SECONDS, WAVE_DEFAULT_SECONDS,
	       RAINBOW_MIN_SECONDS, RAINBOW_MAX_SECONDS);
}

int main(int argc, char *argv[])
{
	const char *pci_addr = NULL;
	bool daemon_mode = false;
	bool inherit = false;
	int inherit_container = -1;
	int inherit_group = -1;
	int inherit_device = -1;
	uint64_t inherit_bar_offset = 0;
	size_t inherit_bar_size = 0;
	uint8_t red = 0x00;
	uint8_t green = 0xcc;
	uint8_t blue = 0xff;
	enum led_mode mode = LED_MODE_STATIC;
	double cycle_seconds = 0.0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
			daemon_mode = true;
		} else if (strcmp(argv[i], "--color") == 0 && i + 3 < argc) {
			red = (uint8_t)strtoul(argv[++i], NULL, 0);
			green = (uint8_t)strtoul(argv[++i], NULL, 0);
			blue = (uint8_t)strtoul(argv[++i], NULL, 0);
			mode = LED_MODE_STATIC;
		} else if (strcmp(argv[i], "--off") == 0) {
			red = 0; green = 0; blue = 0;
			mode = LED_MODE_STATIC;
		} else if (strcmp(argv[i], "--rainbow") == 0) {
			mode = LED_MODE_RAINBOW;
		} else if (strcmp(argv[i], "--wave") == 0) {
			mode = LED_MODE_WAVE;
		} else if (strcmp(argv[i], "--cycle") == 0) {
			if (mode == LED_MODE_STATIC)
				mode = LED_MODE_RAINBOW;
			if (i + 1 < argc &&
			    parse_cycle_seconds(argv[i + 1], &cycle_seconds))
				i++;
		} else if (strncmp(argv[i], "--cycle=", 8) == 0) {
			if (mode == LED_MODE_STATIC)
				mode = LED_MODE_RAINBOW;
			if (!parse_cycle_seconds(argv[i] + 8, &cycle_seconds))
				fprintf(stderr, "[everlive-userd] invalid --cycle value; "
					"using %.1f s\n", cycle_seconds);
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if (strcmp(argv[i], "--inherit") == 0 && i + 5 < argc) {
			inherit = true;
			inherit_container = atoi(argv[++i]);
			inherit_group = atoi(argv[++i]);
			inherit_device = atoi(argv[++i]);
			inherit_bar_offset = strtoull(argv[++i], NULL, 0);
			inherit_bar_size = (size_t)strtoull(argv[++i], NULL, 0);
		} else if (argv[i][0] != '-') {
			pci_addr = argv[i];
		}
	}

	if (cycle_seconds <= 0.0) {
		cycle_seconds = (mode == LED_MODE_WAVE) ? WAVE_DEFAULT_SECONDS
							: RAINBOW_DEFAULT_SECONDS;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	setlinebuf(stdout);
	setlinebuf(stderr);

	if (daemon_mode) {
		if (daemon(0, 0) < 0) {
			perror("daemon");
			return 1;
		}
	}

	printf("[everlive-userd] Initializing GC573 Userspace Hardware Controller...\n");

	struct gc573_vfio_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.container_fd = -1;
	ctx.group_fd = -1;
	ctx.device_fd = -1;
	ctx.irq_efd = -1;

	if (inherit) {
		ctx.container_fd = inherit_container;
		ctx.group_fd = inherit_group;
		ctx.device_fd = inherit_device;
		ctx.bar0_offset = inherit_bar_offset;
		ctx.bar0_size = inherit_bar_size;
		ctx.bar0 = mmap(NULL, ctx.bar0_size, PROT_READ | PROT_WRITE,
				MAP_SHARED, ctx.device_fd,
				(off_t)ctx.bar0_offset);
		if (ctx.bar0 == MAP_FAILED) {
			perror("[everlive-userd] mmap BAR0");
			ctx.bar0 = NULL;
			gc573_vfio_cleanup(&ctx);
			return 1;
		}
	} else {
		int ret = gc573_vfio_init(&ctx, pci_addr);

		if (ret < 0) {
			fprintf(stderr, "[everlive-userd] Failed to initialize VFIO: %d\n",
				ret);
			return 1;
		}
	}

	if (mode == LED_MODE_WAVE)
		printf("[everlive-userd] LED keepalive: multi-zone wave (%.1f s per revolution), enable 0x%08x.\n",
		       cycle_seconds, readl((uint8_t *)ctx.bar0 + 0x0804));
	else if (mode == LED_MODE_RAINBOW)
		printf("[everlive-userd] LED keepalive: rainbow cycle (%.1f s per revolution, "
		       "soft saturation), enable 0x%08x.\n",
		       cycle_seconds, readl((uint8_t *)ctx.bar0 + 0x0804));
	else
		printf("[everlive-userd] LED keepalive color=(%u,%u,%u), enable 0x%08x.\n",
		       red, green, blue, readl((uint8_t *)ctx.bar0 + 0x0804));
	printf("[everlive-userd] Holding VFIO context active.\n");

	/*
	 * The GC573 LED block falls back to its red-blinking default shortly
	 * after the video DMA stops.  Re-arm the profile at 20 Hz; this also
	 * keeps the inherited VFIO context alive.  In rainbow mode the phase is
	 * derived from the monotonic clock, so the cycle length stays exact even
	 * if a refresh is delayed.
	 */
	{
		uint64_t start_ms = now_ms();
		uint32_t cycle_ms = (uint32_t)(cycle_seconds * 1000.0 + 0.5);

		if (!cycle_ms)
			cycle_ms = (uint32_t)(RAINBOW_DEFAULT_SECONDS * 1000.0);

		while (g_running) {
			if (mode == LED_MODE_WAVE) {
				uint32_t zone_colors[5];
				wave_tick(now_ms() - start_ms, cycle_ms, zone_colors);
				gc573_user_set_led_zones(ctx.bar0, zone_colors);
			} else {
				if (mode == LED_MODE_RAINBOW)
					rainbow_tick(now_ms() - start_ms, cycle_ms,
						     &red, &green, &blue);

				gc573_user_led_refresh(ctx.bar0, red, green, blue);
			}
			usleep(50000);
		}
	}

	printf("\n[everlive-userd] Shutting down...\n");
	gc573_vfio_cleanup(&ctx);
	return 0;
}

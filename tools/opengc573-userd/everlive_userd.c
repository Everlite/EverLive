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
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
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

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
			daemon_mode = true;
		} else if (strcmp(argv[i], "--color") == 0 && i + 3 < argc) {
			red = (uint8_t)strtoul(argv[++i], NULL, 0);
			green = (uint8_t)strtoul(argv[++i], NULL, 0);
			blue = (uint8_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--off") == 0) {
			red = 0; green = 0; blue = 0;
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

	printf("[everlive-userd] LED keepalive color=(%u,%u,%u), enable 0x%08x.\n",
	       red, green, blue, readl((uint8_t *)ctx.bar0 + 0x0804));
	printf("[everlive-userd] Holding VFIO context active.\n");

	/*
	 * The GC573 LED block falls back to its red-blinking default shortly
	 * after the video DMA stops.  Re-arm the same constant-colour profile
	 * at 20 Hz; this also keeps the inherited VFIO context alive.
	 */
	while (g_running) {
		gc573_user_led_refresh(ctx.bar0, red, green, blue);
		usleep(50000);
	}

	printf("\n[everlive-userd] Shutting down...\n");
	gc573_vfio_cleanup(&ctx);
	return 0;
}

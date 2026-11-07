/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_ctl.c — Unified Control & Diagnostic CLI for EverLive Capture Cards
 */

#include "everlive_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

static void print_capabilities(uint32_t caps)
{
	printf("    Capabilities: ");
	bool first = true;
#define CHECK_CAP(cap, name) \
	if (caps & (cap)) { \
		printf("%s%s", first ? "" : ", ", (name)); \
		first = false; \
	}
	CHECK_CAP(EVERLIVE_CAP_4K60, "4K60");
	CHECK_CAP(EVERLIVE_CAP_4K120, "4K120");
	CHECK_CAP(EVERLIVE_CAP_1440P120, "1440p120");
	CHECK_CAP(EVERLIVE_CAP_1080P240, "1080p240");
	CHECK_CAP(EVERLIVE_CAP_1080P60, "1080p60");
	CHECK_CAP(EVERLIVE_CAP_HDR10, "HDR10");
	CHECK_CAP(EVERLIVE_CAP_DUAL_INPUT, "Dual-Input");
	CHECK_CAP(EVERLIVE_CAP_RGB_LED, "RGB-LED");
	CHECK_CAP(EVERLIVE_CAP_BITSTREAM_AUDIO, "Bitstream-Audio (AC3/DTS)");
	CHECK_CAP(EVERLIVE_CAP_FRAMEGEN, "Lossless Scaling (lsfg-vk)");
	CHECK_CAP(EVERLIVE_CAP_HARDWARE_SPLITTER, "Hardware-Splitter");
#undef CHECK_CAP
	if (first)
		printf("None");
	printf("\n");
}

static int cmd_detect(void)
{
	struct everlive_device cards[8];
	int count = everlive_detect_installed_cards(cards, 8, NULL);

	printf("=== EverLive Capture Card Discovery ===\n");
	if (count <= 0) {
		printf("No supported EverLive capture cards detected on PCI bus.\n");
		return 1;
	}

	printf("Found %d supported capture card%s:\n\n", count, count > 1 ? "s" : "");
	for (int i = 0; i < count; i++) {
		const struct everlive_device *dev = &cards[i];
		printf("[%d] %s\n", i + 1, dev->desc->name);
		printf("    Model Slug:   %s\n", dev->desc->model_slug);
		printf("    PCI BDF:      %s\n", dev->pci_bdf);
		printf("    IOMMU Group:  %d\n", dev->iommu_group);
		printf("    Driver Bound: %s\n", dev->is_vfio_bound ? "vfio-pci (Ready)" : "Kernel driver (Needs bind)");
		printf("    Input Ports:  %d\n", dev->desc->num_input_ports);
		print_capabilities(dev->desc->capabilities);
		printf("\n");
	}
	return 0;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s <command> [arguments]\n\n"
	       "Hardware & Diagnostics:\n"
	       "  detect                  List all installed capture cards and capabilities\n"
	       "  list-models             Display all supported card models in EverLive registry\n"
	       "  status                  Display capture daemon status and active hardware\n\n"
	       "Runtime control (JSON-RPC to the running engine):\n"
	       "  runtime status          Engine status (signal, audio, V4L2, HDR...)\n"
	       "  runtime led <RRGGBB|off>\n"
	       "  runtime led-mode <static|wave|rainbow|off>\n"
	       "  runtime edid <profile|/path.edid>\n"
	       "  runtime hdr <auto|off|passthrough>\n"
	       "  runtime hdr-ev <-2.0..2.0>\n"
	       "  runtime hdr-passthrough <0|1>\n"
	       "  runtime hotplug         Pulse HDMI HPD and replay the EDID\n"
	       "  runtime toast <text>\n"
	       "  runtime volume <0..150>\n"
	       "  runtime mute <0|1>\n"
	       "  runtime osd <0|1>\n\n"
	       "Service & Bringup:\n"
	       "  init                    Initialize capture card in background (passthrough/audio)\n"
	       "  service status          Show systemd user service status\n"
	       "  service start           Start the background capture service\n"
	       "  service stop            Stop the background capture service\n"
	       "  service enable          Enable background capture service at user login\n"
	       "  service disable         Disable background capture autostart\n"
	       "  help                    Show this help message\n\n", prog);
}

static int cmd_status(void)
{
	printf("=== EverLive Background Engine Status ===\n");

	/* Check running processes */
	bool gc570d_proc = (system("pgrep -x opengc570d-userd >/dev/null 2>&1") == 0);
	bool gc573_proc = (system("pgrep -x gc573_user_streamer >/dev/null 2>&1") == 0 ||
			   system("pgrep -x everlive_userd >/dev/null 2>&1") == 0);

	if (gc570d_proc) {
		printf("Active Process:   opengc570d-userd (running)\n");
	} else if (gc573_proc) {
		printf("Active Process:   gc573_user_streamer / everlive_userd (running)\n");
	} else {
		printf("Active Process:   None (Engine is idle/standby)\n");
	}

	/* Check systemd user service */
	printf("Service State:    ");
	fflush(stdout);
	int ret = system("systemctl --user is-active everlive.service 2>/dev/null");
	if (ret != 0) {
		system("systemctl --user is-active opengc570d-userd.service 2>/dev/null || echo inactive");
	}

	/* Also report detected card */
	printf("\n");
	cmd_detect();
	return 0;
}

static int cmd_init(void)
{
	printf("[*] Initializing EverLive capture hardware in background...\n");
	/* Check if already running */
	if (system("pgrep -x opengc570d-userd >/dev/null 2>&1") == 0 ||
	    system("pgrep -x gc573_user_streamer >/dev/null 2>&1") == 0) {
		printf("[+] Capture engine is already running and hardware is initialized.\n");
		return 0;
	}

	/* Try starting via systemd if service file exists */
	if (system("systemctl --user is-enabled everlive.service >/dev/null 2>&1") == 0 ||
	    system("systemctl --user is-enabled opengc570d-userd.service >/dev/null 2>&1") == 0) {
		printf("[*] Starting via systemd user service...\n");
		system("systemctl --user start everlive.service 2>/dev/null || systemctl --user start opengc570d-userd.service");
		return 0;
	}

	/* Direct script invocation */
	char script_path[PATH_MAX];
	snprintf(script_path, sizeof(script_path), "%s/everlive-service.sh", "/usr/local/bin");
	if (access(script_path, X_OK) != 0) {
		/* Search relative to executable or current directory */
		snprintf(script_path, sizeof(script_path), "./tools/libeverlive-core/everlive-service.sh");
	}

	char cmd[PATH_MAX + 32];
	snprintf(cmd, sizeof(cmd), "nohup %s >/dev/null 2>&1 &", script_path);
	printf("[*] Spawning %s...\n", script_path);
	int res = system(cmd);
	if (res == 0) {
		printf("[+] EverLive background service launched successfully.\n");
	}
	return res;
}

static int cmd_service(int argc, char **argv)
{
	if (argc < 3) {
		return system("systemctl --user status everlive.service opengc570d-userd.service 2>/dev/null || true");
	}

	const char *action = argv[2];
	char cmd[256];

	if (strcmp(action, "status") == 0) {
		snprintf(cmd, sizeof(cmd), "systemctl --user status everlive.service opengc570d-userd.service");
	} else if (strcmp(action, "start") == 0) {
		snprintf(cmd, sizeof(cmd), "systemctl --user start everlive.service 2>/dev/null || systemctl --user start opengc570d-userd.service");
	} else if (strcmp(action, "stop") == 0) {
		snprintf(cmd, sizeof(cmd), "systemctl --user stop everlive.service opengc570d-userd.service 2>/dev/null");
	} else if (strcmp(action, "enable") == 0) {
		snprintf(cmd, sizeof(cmd), "systemctl --user enable --now everlive.service 2>/dev/null || systemctl --user enable --now opengc570d-userd.service");
	} else if (strcmp(action, "disable") == 0) {
		snprintf(cmd, sizeof(cmd), "systemctl --user disable --now everlive.service opengc570d-userd.service 2>/dev/null");
	} else {
		fprintf(stderr, "Unknown service action: %s\n", action);
		return 1;
	}

	return system(cmd);
}

static int cmd_list_models(void)
{
	size_t count = 0;
	const struct everlive_device_desc *reg = everlive_get_card_registry(&count);

	printf("=== EverLive Supported Card Registry ===\n");
	for (size_t i = 0; i < count; i++) {
		printf("- %s (%s)\n", reg[i].name, reg[i].model_slug);
		printf("  PCI ID: 0x%04x:0x%04x (Subsystem: 0x%04x)\n",
		       reg[i].vendor_id, reg[i].device_id, reg[i].subsystem_id);
		print_capabilities(reg[i].capabilities);
	}
	return 0;
}

static int ctl_resolve_socket(char *buf, size_t size)
{
	const char *override = getenv("EVERLIVE_CONTROL_SOCKET");
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	uid_t uid = getuid();
	int n;

	if (override && *override) {
		n = snprintf(buf, size, "%s", override);
		return (n > 0 && (size_t)n < size) ? 0 : -1;
	}

	if (xdg && *xdg) {
		n = snprintf(buf, size, "%s/everlive-experimental.sock", xdg);
		if (n > 0 && (size_t)n < size && access(buf, F_OK) == 0)
			return 0;
	}

	n = snprintf(buf, size, "/run/user/%u/everlive-experimental.sock",
		     (unsigned int)uid);
	if (n > 0 && (size_t)n < size && access(buf, F_OK) == 0)
		return 0;

	n = snprintf(buf, size, "/tmp/everlive-%u/everlive-experimental.sock",
		     (unsigned int)uid);
	return (n > 0 && (size_t)n < size) ? 0 : -1;
}

static void json_escape(const char *in, char *out, size_t out_size)
{
	size_t o = 0;

	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		if (o + 8 >= out_size)
			break;
		switch (*p) {
		case '"':
			out[o++] = '\\';
			out[o++] = '"';
			break;
		case '\\':
			out[o++] = '\\';
			out[o++] = '\\';
			break;
		case '\n':
			out[o++] = '\\';
			out[o++] = 'n';
			break;
		case '\r':
			out[o++] = '\\';
			out[o++] = 'r';
			break;
		case '\t':
			out[o++] = '\\';
			out[o++] = 't';
			break;
		default:
			if (*p < 0x20)
				o += (size_t)snprintf(out + o, out_size - o,
						      "\\u%04x", *p);
			else
				out[o++] = (char)*p;
			break;
		}
	}
	out[o] = '\0';
}

static int ctl_call(const char *method, const char *params)
{
	char path[108];
	struct sockaddr_un addr;
	char request[2048];
	char response[8192];
	size_t off = 0;
	int fd;

	if (ctl_resolve_socket(path, sizeof(path)) < 0) {
		fprintf(stderr, "EverLive control socket path is too long\n");
		return 1;
	}

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "socket: %s\n", strerror(errno));
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr,
			"Cannot connect to the EverLive engine at %s: %s\n"
			"Is the capture engine running?\n",
			path, strerror(errno));
		close(fd);
		return 1;
	}

	snprintf(request, sizeof(request),
		 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}\n",
		 method, params && *params ? params : "{}");

	while (off < strlen(request)) {
		ssize_t n = write(fd, request + off, strlen(request) - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "write: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		off += (size_t)n;
	}

	off = 0;
	for (;;) {
		ssize_t n;

		if (off + 1 >= sizeof(response)) {
			fprintf(stderr, "Response too large\n");
			close(fd);
			return 1;
		}
		n = read(fd, response + off, sizeof(response) - off - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "read: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		if (n == 0)
			break;
		off += (size_t)n;
		if (memchr(response, '\n', off))
			break;
	}
	close(fd);

	response[off] = '\0';
	while (off > 0 && (response[off - 1] == '\n' || response[off - 1] == '\r'))
		response[--off] = '\0';

	printf("%s\n", response);
	return strstr(response, "\"error\"") ? 1 : 0;
}

static int parse_switch(const char *value)
{
	if (!value)
		return -1;
	if (!strcasecmp(value, "on") || !strcasecmp(value, "true") ||
	    !strcasecmp(value, "1") || !strcasecmp(value, "yes") ||
	    !strcasecmp(value, "enable") || !strcasecmp(value, "enabled"))
		return 1;
	if (!strcasecmp(value, "off") || !strcasecmp(value, "false") ||
	    !strcasecmp(value, "0") || !strcasecmp(value, "no") ||
	    !strcasecmp(value, "disable") || !strcasecmp(value, "disabled"))
		return 0;
	return -1;
}

static int cmd_runtime(int argc, char **argv)
{
	char params[1536];
	char escaped[512];

	if (argc < 3) {
		fprintf(stderr, "Usage: everlive-ctl runtime <topic> [value]\n"
			"  status                     Show the engine JSON status\n"
			"  led <RRGGBB|off>           Set the static LED colour\n"
			"  led-mode <static|wave|rainbow|off>\n"
			"  edid <default|1440p120|1080p240|bitstream|/path.edid>\n"
			"  hdr <auto|off|passthrough> Toggle HDR processing\n"
			"  hdr-ev <-2.0..2.0>         Set tonemap exposure in EV\n"
			"  hdr-passthrough <0|1>      Toggle BT.2020 HDR passthrough\n"
			"  hotplug                    Pulse HDMI HPD / replay EDID\n"
			"  toast <text>               Show an on-screen notification\n"
			"  volume <0..150>            Set desktop audio volume %%\n"
			"  mute <0|1>                 Mute/unmute desktop audio\n"
			"  osd <0|1>                  Toggle the on-screen overlay\n");
		return 1;
	}

	const char *topic = argv[2];

	if (strcmp(topic, "status") == 0)
		return ctl_call("status.get", "{}");

	if (strcmp(topic, "led") == 0) {
		if (argc < 4) {
			fprintf(stderr, "runtime led needs RRGGBB or off\n");
			return 1;
		}
		if (strcasecmp(argv[3], "off") == 0)
			return ctl_call("led.mode.set", "{\"mode\":\"off\"}");
		unsigned long rgb = strtoul(argv[3], NULL, 16);
		snprintf(params, sizeof(params),
			 "{\"r\":%lu,\"g\":%lu,\"b\":%lu}",
			 (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
		return ctl_call("led.set", params);
	}

	if (strcmp(topic, "led-mode") == 0) {
		if (argc < 4) {
			fprintf(stderr, "runtime led-mode needs static|wave|rainbow|off\n");
			return 1;
		}
		json_escape(argv[3], escaped, sizeof(escaped));
		snprintf(params, sizeof(params), "{\"mode\":\"%s\"}", escaped);
		return ctl_call("led.mode.set", params);
	}

	if (strcmp(topic, "edid") == 0) {
		if (argc < 4) {
			fprintf(stderr, "runtime edid needs a profile name or path\n");
			return 1;
		}
		json_escape(argv[3], escaped, sizeof(escaped));
		snprintf(params, sizeof(params), "{\"profile\":\"%s\"}", escaped);
		return ctl_call("edid.set", params);
	}

	if (strcmp(topic, "hdr") == 0) {
		const char *mode = argc >= 4 ? argv[3] : "auto";

		json_escape(mode, escaped, sizeof(escaped));
		snprintf(params, sizeof(params), "{\"mode\":\"%s\"}", escaped);
		return ctl_call("hdr.set", params);
	}

	if (strcmp(topic, "hdr-ev") == 0) {
		if (argc < 4) {
			fprintf(stderr, "runtime hdr-ev needs an EV value\n");
			return 1;
		}
		snprintf(params, sizeof(params), "{\"exposure\":%s}", argv[3]);
		return ctl_call("hdr.exposure", params);
	}

	if (strcmp(topic, "hdr-passthrough") == 0) {
		int on = parse_switch(argc >= 4 ? argv[3] : NULL);

		if (on < 0) {
			fprintf(stderr, "runtime hdr-passthrough needs 0 or 1\n");
			return 1;
		}
		snprintf(params, sizeof(params),
			 "{\"enabled\":%s}", on ? "true" : "false");
		return ctl_call("hdr.passthrough", params);
	}

	if (strcmp(topic, "hotplug") == 0)
		return ctl_call("hotplug", "{\"pulse\":true}");

	if (strcmp(topic, "toast") == 0) {
		if (argc < 4) {
			fprintf(stderr, "runtime toast needs text\n");
			return 1;
		}
		json_escape(argv[3], escaped, sizeof(escaped));
		snprintf(params, sizeof(params), "{\"message\":\"%s\"}", escaped);
		return ctl_call("toast.show", params);
	}

	if (strcmp(topic, "volume") == 0) {
		if (argc < 4) {
			fprintf(stderr, "runtime volume needs 0..150\n");
			return 1;
		}
		snprintf(params, sizeof(params), "{\"gain\":%g}",
			 strtod(argv[3], NULL) / 100.0);
		return ctl_call("audio.volume.set", params);
	}

	if (strcmp(topic, "mute") == 0) {
		int on = parse_switch(argc >= 4 ? argv[3] : NULL);

		if (on < 0) {
			fprintf(stderr, "runtime mute needs 0 or 1\n");
			return 1;
		}
		snprintf(params, sizeof(params),
			 "{\"muted\":%s}", on ? "true" : "false");
		return ctl_call("audio.mute.set", params);
	}

	if (strcmp(topic, "osd") == 0) {
		int on = parse_switch(argc >= 4 ? argv[3] : NULL);

		if (on < 0) {
			fprintf(stderr, "runtime osd needs 0 or 1\n");
			return 1;
		}
		snprintf(params, sizeof(params),
			 "{\"enabled\":%s}", on ? "true" : "false");
		return ctl_call("osd.set", params);
	}

	fprintf(stderr, "Unknown runtime topic: %s\n", topic);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		return cmd_detect();
	}

	const char *cmd = argv[1];
	if (strcmp(cmd, "detect") == 0 || strcmp(cmd, "-d") == 0) {
		return cmd_detect();
	} else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "-s") == 0) {
		return cmd_status();
	} else if (strcmp(cmd, "init") == 0 || strcmp(cmd, "-i") == 0) {
		return cmd_init();
	} else if (strcmp(cmd, "service") == 0) {
		return cmd_service(argc, argv);
	} else if (strcmp(cmd, "list-models") == 0) {
		return cmd_list_models();
	} else if (strcmp(cmd, "runtime") == 0 || strcmp(cmd, "ctl") == 0) {
		return cmd_runtime(argc, argv);
	} else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		print_usage(argv[0]);
		return 0;
	} else {
		fprintf(stderr, "Unknown command: %s\n\n", cmd);
		print_usage(argv[0]);
		return 1;
	}
}

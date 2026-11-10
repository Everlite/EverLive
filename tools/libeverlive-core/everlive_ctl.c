/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_ctl.c — Unified Control & Diagnostic CLI for EverLive Capture Cards
 */

#include "everlive_card.h"
#include "everlive_ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int ctl_connect(char *path, size_t path_size);

static void print_capabilities(uint32_t caps)
{
	printf("    Capabilities: ");
	bool first = true;
#define CHECK_CAP(cap, name) \
	if (caps & (cap)) { \
		printf("%s%s", first ? "" : ", ", (name)); \
		first = false; \
	}
	CHECK_CAP(EVERLIVE_CAP_VIDEO_CAPTURE, "Video-Capture");
	CHECK_CAP(EVERLIVE_CAP_AUDIO_CAPTURE, "Audio-Capture");
	CHECK_CAP(EVERLIVE_CAP_PASSTHROUGH, "Passthrough");
	CHECK_CAP(EVERLIVE_CAP_EDID_PASSTHROUGH, "EDID-Passthrough");
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

/*
 * Execute argv directly (no shell) and return its exit code, or -1 if it could
 * not be spawned or reaped.  When @quiet is true, stderr is discarded.
 */
static int run_command(const char *const argv[], bool quiet)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		if (quiet) {
			int devnull = open("/dev/null", O_WRONLY);

			if (devnull >= 0) {
				dup2(devnull, STDERR_FILENO);
				if (devnull > STDERR_FILENO)
					close(devnull);
			}
		}
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}

	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static bool systemctl_unit_active(const char *unit)
{
	const char *const argv[] = {
		"systemctl", "--user", "is-active", "--quiet", unit, NULL
	};

	return run_command(argv, true) == 0;
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
	char path[108];
	int fd;

	printf("=== EverLive Background Engine Status ===\n");

	/* The engine is running when its JSON-RPC control socket answers. */
	fd = ctl_connect(path, sizeof(path));
	if (fd >= 0) {
		close(fd);
		printf("Active Process:   EverLive engine (running at %s)\n",
		       path);
	} else {
		printf("Active Process:   None (Engine is idle/standby)\n");
	}

	/* Check systemd user service */
	printf("Service State:    ");
	if (systemctl_unit_active("everlive.service") ||
	    systemctl_unit_active("opengc570d-userd.service"))
		printf("active\n");
	else
		printf("inactive\n");

	/* Also report detected card */
	printf("\n");
	cmd_detect();
	return 0;
}

/*
 * Locate the background service script.  Prefer one sitting next to the
 * running everlive-ctl binary (true for both an installed tree and a source
 * build), then the installed locations, then the legacy CWD-relative path.
 */
static bool resolve_service_script(char *out, size_t out_size)
{
	static const char *const candidates[] = {
		"/usr/local/bin/everlive-service",
		"/usr/local/bin/everlive-service.sh",
		"./tools/libeverlive-core/everlive-service.sh",
	};
	char exe_path[PATH_MAX];
	ssize_t len;

	len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
	if (len > 0) {
		char *slash;

		exe_path[len] = '\0';
		slash = strrchr(exe_path, '/');
		if (slash) {
			int n;

			*slash = '\0';
			n = snprintf(out, out_size, "%s/everlive-service",
				     exe_path);
			if (n > 0 && (size_t)n < out_size &&
			    access(out, X_OK) == 0)
				return true;
			n = snprintf(out, out_size, "%s/everlive-service.sh",
				     exe_path);
			if (n > 0 && (size_t)n < out_size &&
			    access(out, X_OK) == 0)
				return true;
		}
	}

	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		int n = snprintf(out, out_size, "%s", candidates[i]);

		if (n > 0 && (size_t)n < out_size && access(out, X_OK) == 0)
			return true;
	}

	return false;
}

/*
 * Launch @path detached from the terminal.  A pipe with O_CLOEXEC tells the
 * parent whether exec() actually succeeded: EOF means success, four bytes
 * mean the child failed with that errno.  The parent does not wait, so the
 * service is not tied to the lifetime of this CLI.
 */
static int spawn_detached(const char *path)
{
	int pipefd[2];
	pid_t pid;
	int err = 0;
	ssize_t n;

	if (pipe2(pipefd, O_CLOEXEC) < 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		int saved = errno;

		close(pipefd[0]);
		close(pipefd[1]);
		errno = saved;
		return -1;
	}

	if (pid == 0) {
		int devnull;

		close(pipefd[0]);
		if (setsid() < 0)
			goto fail;

		devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}

		execl(path, path, (char *)NULL);
fail:
		err = errno;
		(void)!write(pipefd[1], &err, sizeof(err));
		_exit(127);
	}

	close(pipefd[1]);
	do {
		n = read(pipefd[0], &err, sizeof(err));
	} while (n < 0 && errno == EINTR);

	if (n < 0) {
		int saved = errno;

		close(pipefd[0]);
		errno = saved;
		return -1;
	}
	close(pipefd[0]);

	/* Any data means the child failed before exec(); EOF means success. */
	if (n > 0) {
		errno = err;
		return -1;
	}

	return 0;
}

static int cmd_init(void)
{
	const char *const everlive_enabled[] = {
		"systemctl", "--user", "is-enabled", "--quiet",
		"everlive.service", NULL
	};
	const char *const gc570d_enabled[] = {
		"systemctl", "--user", "is-enabled", "--quiet",
		"opengc570d-userd.service", NULL
	};
	const char *const everlive_start[] = {
		"systemctl", "--user", "start", "everlive.service", NULL
	};
	const char *const gc570d_start[] = {
		"systemctl", "--user", "start",
		"opengc570d-userd.service", NULL
	};
	char script_path[PATH_MAX];
	char path[108];
	int fd;

	printf("[*] Initializing EverLive capture hardware in background...\n");

	/* The engine is already up if its control socket answers. */
	fd = ctl_connect(path, sizeof(path));
	if (fd >= 0) {
		close(fd);
		printf("[+] Capture engine is already running and hardware is initialized.\n");
		return 0;
	}

	/* Try starting via systemd if a user unit is installed. */
	if (run_command(everlive_enabled, true) == 0 ||
	    run_command(gc570d_enabled, true) == 0) {
		printf("[*] Starting via systemd user service...\n");
		if (run_command(everlive_start, true) == 0 ||
		    run_command(gc570d_start, false) == 0) {
			printf("[+] EverLive background service started.\n");
			return 0;
		}
		fprintf(stderr,
			"Failed to start the EverLive systemd user service.\n");
		return 1;
	}

	/* Direct script invocation */

	if (!resolve_service_script(script_path, sizeof(script_path))) {
		fprintf(stderr,
			"Could not locate everlive-service; install it or run from the source tree.\n");
		return 1;
	}

	printf("[*] Spawning %s...\n", script_path);
	if (spawn_detached(script_path) < 0) {
		fprintf(stderr, "Failed to launch %s: %s\n",
			script_path, strerror(errno));
		return 1;
	}

	printf("[+] EverLive background service launched successfully.\n");
	return 0;
}

static int cmd_service(int argc, char **argv)
{
	const char *action;

	if (argc < 3)
		action = "status";
	else
		action = argv[2];

	if (strcmp(action, "status") == 0) {
		const char *const status_argv[] = {
			"systemctl", "--user", "status", "everlive.service",
			"opengc570d-userd.service", NULL
		};

		return run_command(status_argv, true);
	}

	if (strcmp(action, "start") == 0) {
		const char *const everlive[] = {
			"systemctl", "--user", "start", "everlive.service", NULL
		};
		const char *const gc570d[] = {
			"systemctl", "--user", "start",
			"opengc570d-userd.service", NULL
		};

		if (run_command(everlive, true) == 0)
			return 0;
		return run_command(gc570d, false);
	}

	if (strcmp(action, "stop") == 0) {
		const char *const stop_argv[] = {
			"systemctl", "--user", "stop", "everlive.service",
			"opengc570d-userd.service", NULL
		};

		return run_command(stop_argv, true);
	}

	if (strcmp(action, "enable") == 0) {
		const char *const everlive[] = {
			"systemctl", "--user", "enable", "--now",
			"everlive.service", NULL
		};
		const char *const gc570d[] = {
			"systemctl", "--user", "enable", "--now",
			"opengc570d-userd.service", NULL
		};

		if (run_command(everlive, true) == 0)
			return 0;
		return run_command(gc570d, false);
	}

	if (strcmp(action, "disable") == 0) {
		const char *const disable_argv[] = {
			"systemctl", "--user", "disable", "--now",
			"everlive.service", "opengc570d-userd.service", NULL
		};

		return run_command(disable_argv, true);
	}

	fprintf(stderr, "Unknown service action: %s\n", action);
	return 1;
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

/*
 * Resolve and connect to the engine control socket.  Returns the fd on
 * success, or -1 with errno set.
 */
static int ctl_connect(char *path, size_t path_size)
{
	struct sockaddr_un addr;
	int fd;
	int saved;

	if (ctl_resolve_socket(path, path_size) < 0) {
		errno = ENAMETOOLONG;
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}

	return fd;
}

static int ctl_call(const char *method, const char *params)
{
	char path[108] = "";
	char request[2048];
	char response[8192];
	size_t off = 0;
	size_t request_len;
	int fd;

	fd = ctl_connect(path, sizeof(path));
	if (fd < 0) {
		if (errno == ENAMETOOLONG) {
			fprintf(stderr,
				"EverLive control socket path is too long\n");
		} else {
			fprintf(stderr,
				"Cannot connect to the EverLive engine at %s: %s\n"
				"Is the capture engine running?\n",
				path, strerror(errno));
		}
		return 1;
	}

	if (everlive_ctl_build_request(method, params, request,
				       sizeof(request)) < 0) {
		fprintf(stderr, "Request too long\n");
		close(fd);
		return 1;
	}

	request_len = strlen(request);

	while (off < request_len) {
		ssize_t n = write(fd, request + off, request_len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "write: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		if (n == 0) {
			fprintf(stderr, "write: no progress\n");
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
	return everlive_ctl_response_has_error(response) ? 1 : 0;
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
		unsigned long rgb;

		if (argc < 4) {
			fprintf(stderr, "runtime led needs RRGGBB or off\n");
			return 1;
		}
		if (strcasecmp(argv[3], "off") == 0)
			return ctl_call("led.mode.set", "{\"mode\":\"off\"}");
		if (everlive_ctl_parse_hex_rgb(argv[3], &rgb) < 0 ||
		    everlive_ctl_format_led(rgb, params, sizeof(params)) < 0) {
			fprintf(stderr, "runtime led needs RRGGBB or off\n");
			return 1;
		}
		return ctl_call("led.set", params);
	}

	if (strcmp(topic, "led-mode") == 0) {
		if (argc < 4) {
			fprintf(stderr, "runtime led-mode needs static|wave|rainbow|off\n");
			return 1;
		}
		everlive_ctl_json_escape(argv[3], escaped, sizeof(escaped));
		snprintf(params, sizeof(params), "{\"mode\":\"%s\"}", escaped);
		return ctl_call("led.mode.set", params);
	}

	if (strcmp(topic, "edid") == 0) {
		if (argc < 4) {
			fprintf(stderr, "runtime edid needs a profile name or path\n");
			return 1;
		}
		everlive_ctl_json_escape(argv[3], escaped, sizeof(escaped));
		snprintf(params, sizeof(params), "{\"profile\":\"%s\"}", escaped);
		return ctl_call("edid.set", params);
	}

	if (strcmp(topic, "hdr") == 0) {
		const char *mode = argc >= 4 ? argv[3] : "auto";

		everlive_ctl_json_escape(mode, escaped, sizeof(escaped));
		snprintf(params, sizeof(params), "{\"mode\":\"%s\"}", escaped);
		return ctl_call("hdr.set", params);
	}

	if (strcmp(topic, "hdr-ev") == 0) {
		double ev;

		if (argc < 4) {
			fprintf(stderr, "runtime hdr-ev needs an EV value\n");
			return 1;
		}
		if (everlive_ctl_parse_double(argv[3], &ev) < 0 ||
		    everlive_ctl_format_exposure(ev, params, sizeof(params)) < 0) {
			fprintf(stderr, "runtime hdr-ev needs a numeric EV value\n");
			return 1;
		}
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
		everlive_ctl_json_escape(argv[3], escaped, sizeof(escaped));
		snprintf(params, sizeof(params), "{\"message\":\"%s\"}", escaped);
		return ctl_call("toast.show", params);
	}

	if (strcmp(topic, "volume") == 0) {
		double percent;

		if (argc < 4) {
			fprintf(stderr, "runtime volume needs 0..150\n");
			return 1;
		}
		if (everlive_ctl_parse_double(argv[3], &percent) < 0 ||
		    everlive_ctl_format_volume(percent, params,
					       sizeof(params)) < 0) {
			fprintf(stderr,
				"runtime volume needs a number in 0..150\n");
			return 1;
		}
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

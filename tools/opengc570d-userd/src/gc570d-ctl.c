// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc570d-ctl — Userspace diagnostic & LED control CLI for opengc570d-userd
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

static void get_sock_path(char *buf, size_t size)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	if (xdg && xdg[0])
		snprintf(buf, size, "%s/gc570d/control.sock", xdg);
	else
		snprintf(buf, size, "/tmp/gc570d-%u/control.sock", (unsigned int)geteuid());
}

int main(int argc, char **argv)
{
	char sock_path[100];
	char cmd[256] = { 0 };
	struct sockaddr_un addr;
	int fd;

	if (argc < 2) {
		strscpy_fallback:
		snprintf(cmd, sizeof(cmd), "status\n");
	} else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
		printf("Usage:\n"
		       "  gc570d-ctl status\n"
		       "  gc570d-ctl reinit                  (re-initialize hardware after sleep/mode change)\n"
		       "  gc570d-ctl list\n"
		       "  gc570d-ctl input <1|2>             (select HDMI IN 1 or HDMI IN 2)\n"
		       "  gc570d-ctl osd <0|1>               (toggle the on-screen overlay)\n"
		       "  gc570d-ctl osd_pos <tr|br|bl|tl>   (set OSD corner position)\n"
		       "  gc570d-ctl pip <0|1>               (toggle picture-in-picture)\n"
		       "  gc570d-ctl aspect <auto|16:9|4:3|stretch>\n"
		       "  gc570d-ctl scale <fit|integer>\n"
		       "  gc570d-ctl volume <0..150>         (set SDL2 desktop audio volume %%)\n"
		       "  gc570d-ctl mute <0|1>              (mute/unmute SDL2 desktop audio)\n"
		       "  gc570d-ctl show <node>             (e.g., splitter_status, it68051_status, vip0_profile)\n"
		       "  gc570d-ctl action <node> [1|0]     (e.g., it68051_hpd, xilinx_reset)\n"
		       "  gc570d-ctl led <colorful|breathing|static|off>\n");
		return 0;
	} else {
		size_t off = 0;
		for (int i = 1; i < argc && off + 2 < sizeof(cmd); i++) {
			off += snprintf(cmd + off, sizeof(cmd) - off, "%s%s",
					i > 1 ? " " : "", argv[i]);
		}
		snprintf(cmd + off, sizeof(cmd) - off, "\n");
		if (cmd[0] == '\n')
			goto strscpy_fallback;
	}

	get_sock_path(sock_path, sizeof(sock_path));
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "Cannot connect to opengc570d-userd at %s: %s\n",
			sock_path, strerror(errno));
		close(fd);
		return 1;
	}

	if (write(fd, cmd, strlen(cmd)) < 0) {
		perror("write");
		close(fd);
		return 1;
	}

	char buf[4096];
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (write(STDOUT_FILENO, buf, n) < 0)
			break;
	}
	close(fd);
	return 0;
}

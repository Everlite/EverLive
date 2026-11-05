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

static int get_sock_path(char *buf, size_t size)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	int n;

	if (xdg && xdg[0])
		n = snprintf(buf, size, "%s/gc570d/control.sock", xdg);
	else
		n = snprintf(buf, size, "/tmp/gc570d-%u/control.sock",
			     (unsigned int)geteuid());

	return (n >= 0 && (size_t)n < size) ? 0 : -1;
}

/* Append one argv element plus its separator, refusing to overflow. */
static int append_arg(char *cmd, size_t size, size_t *off, int i,
		      const char *arg)
{
	size_t avail = size - *off;
	int n = snprintf(cmd + *off, avail, "%s%s", i > 1 ? " " : "", arg);

	if (n < 0)
		return -1;
	if ((size_t)n >= avail)
		return -1;
	*off += (size_t)n;
	return 0;
}

int main(int argc, char **argv)
{
	char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	char cmd[256] = { 0 };
	struct sockaddr_un addr;
	size_t off = 0;
	int fd;

	if (argc < 2) {
		memcpy(cmd, "status\n", sizeof("status\n"));
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
		for (int i = 1; i < argc; i++) {
			if (append_arg(cmd, sizeof(cmd), &off, i, argv[i]) < 0) {
				fprintf(stderr, "Command too long\n");
				return 1;
			}
		}
		if (off + 2 > sizeof(cmd)) {
			fprintf(stderr, "Command too long\n");
			return 1;
		}
		cmd[off++] = '\n';
		cmd[off] = '\0';
	}

	if (get_sock_path(sock_path, sizeof(sock_path)) < 0) {
		fprintf(stderr, "Control socket path is too long\n");
		return 1;
	}
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

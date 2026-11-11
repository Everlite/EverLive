// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc573_user_log.c — Line-buffered, timestamped stdout/stderr.
 *
 * The streamer's output usually goes to a file (streamer.log), where stdio
 * would otherwise buffer it in 4 KB blocks: the Control Hub saw status lines
 * late and the log carried no timing at all.  stdout and stderr are replaced
 * with fopencookie() streams that write straight to fd 1 / fd 2, adding the
 * elapsed time at the start of every line.  Output stays synchronous, so a
 * crash loses no more than the line being written.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "gc573_user_log.h"

struct ts_stream {
	int fd;
	bool line_start;
};

static struct timespec g_t0;
static char g_start_time[32] = "";

static int write_all(int fd, struct iovec *iov, int cnt)
{
	while (cnt > 0) {
		ssize_t n = writev(fd, iov, cnt);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		while (cnt > 0 && (size_t)n >= iov->iov_len) {
			n -= (ssize_t)iov->iov_len;
			iov++;
			cnt--;
		}
		if (cnt > 0) {
			iov->iov_base = (char *)iov->iov_base + n;
			iov->iov_len -= (size_t)n;
		}
	}
	return 0;
}

static ssize_t ts_write(void *cookie, const char *buf, size_t len)
{
	struct ts_stream *s = cookie;
	size_t done = 0;

	while (done < len) {
		const char *nl = memchr(buf + done, '\n', len - done);
		size_t chunk = nl ? (size_t)(nl - (buf + done)) + 1 : len - done;
		struct iovec iov[2];
		char prefix[24];
		int cnt = 0;

		if (s->line_start) {
			struct timespec now;
			int64_t ms;

			clock_gettime(CLOCK_MONOTONIC, &now);
			ms = (int64_t)(now.tv_sec - g_t0.tv_sec) * 1000 +
			     (now.tv_nsec - g_t0.tv_nsec) / 1000000;
			iov[cnt].iov_base = prefix;
			iov[cnt].iov_len = (size_t)snprintf(prefix, sizeof(prefix),
				"[%4lld.%03lld] ", (long long)(ms / 1000),
				(long long)(ms % 1000));
			cnt++;
		}
		iov[cnt].iov_base = (void *)(buf + done);
		iov[cnt].iov_len = chunk;
		cnt++;

		if (write_all(s->fd, iov, cnt) < 0)
			return done ? (ssize_t)done : -1;
		done += chunk;
		s->line_start = nl != NULL;
	}
	return (ssize_t)len;
}

static FILE *ts_open(struct ts_stream *s)
{
	cookie_io_functions_t io = { .write = ts_write };
	FILE *f = fopencookie(s, "w", io);

	if (f)
		setvbuf(f, NULL, _IOLBF, 0);
	return f;
}

void gc573_user_log_init(void)
{
	static struct ts_stream out = { .fd = STDOUT_FILENO, .line_start = true };
	static struct ts_stream err = { .fd = STDERR_FILENO, .line_start = true };
	const char *env = getenv("EVERLIVE_LOG_TIMESTAMPS");
	time_t wall = time(NULL);
	struct tm tm;
	FILE *f;

	clock_gettime(CLOCK_MONOTONIC, &g_t0);
	if (localtime_r(&wall, &tm))
		strftime(g_start_time, sizeof(g_start_time),
			 "%Y-%m-%d %H:%M:%S", &tm);

	fflush(stdout);
	fflush(stderr);
	if (env && strcmp(env, "0") == 0) {
		setvbuf(stdout, NULL, _IOLBF, 0);
		return;
	}

	f = ts_open(&out);
	if (f)
		stdout = f;
	else
		setvbuf(stdout, NULL, _IOLBF, 0);
	f = ts_open(&err);
	if (f)
		stderr = f;
}

const char *gc573_user_log_start_time(void)
{
	return g_start_time;
}

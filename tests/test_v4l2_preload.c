// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_v4l2_preload.c — preload-side tests for the shared V4L2 interposer.
 *
 * This executable deliberately does not link everlive_v4l2_state.c or
 * everlive_v4l2_preload.c.  The test runner executes it with the real DSOs in
 * LD_PRELOAD, once per mode.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_GC573_NODE "/dev/video63"
#define TEST_GC570D_NODE "/dev/video60"
#define TEST_GC573_BASE 0x50000000U
#define TEST_GC570D_BASE 0x51000000U
#define TEST_SHARED_COOKIE 0x52000000U

typedef int (*v4l2_open_fn)(const char *, int, ...);
typedef int (*v4l2_close_fn)(int);
typedef int (*v4l2_dup_fn)(int);
typedef int (*v4l2_ioctl_fn)(int, unsigned long, ...);
typedef void *(*v4l2_mmap_fn)(void *, size_t, int, int, int, int64_t);
typedef int (*v4l2_munmap_fn)(void *, size_t);

static v4l2_open_fn g_v4l2_open;
static v4l2_close_fn g_v4l2_close;
static v4l2_dup_fn g_v4l2_dup;
static v4l2_ioctl_fn g_v4l2_ioctl;
static v4l2_mmap_fn g_v4l2_mmap;
static v4l2_munmap_fn g_v4l2_munmap;

static _Atomic bool g_thread_failed;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1; \
	} \
} while (0)

#define THREAD_CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		atomic_store(&g_thread_failed, true); \
		return NULL; \
	} \
} while (0)

static int resolve_v4l2(void)
{
	g_v4l2_open = (v4l2_open_fn)dlsym(RTLD_DEFAULT, "v4l2_open");
	g_v4l2_close = (v4l2_close_fn)dlsym(RTLD_DEFAULT, "v4l2_close");
	g_v4l2_dup = (v4l2_dup_fn)dlsym(RTLD_DEFAULT, "v4l2_dup");
	g_v4l2_ioctl = (v4l2_ioctl_fn)dlsym(RTLD_DEFAULT, "v4l2_ioctl");
	g_v4l2_mmap = (v4l2_mmap_fn)dlsym(RTLD_DEFAULT, "v4l2_mmap");
	g_v4l2_munmap =
		(v4l2_munmap_fn)dlsym(RTLD_DEFAULT, "v4l2_munmap");
	CHECK(g_v4l2_open && g_v4l2_close && g_v4l2_dup && g_v4l2_ioctl &&
	      g_v4l2_mmap && g_v4l2_munmap);
	return 0;
}

static int count_open_fds(void)
{
	DIR *dir = opendir("/proc/self/fd");
	struct dirent *ent;
	int count = 0;

	CHECK(dir != NULL);
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") != 0 &&
		    strcmp(ent->d_name, "..") != 0)
			count++;
	}
	CHECK(closedir(dir) == 0);
	return count;
}

static int set_format(int fd, uint32_t width, uint32_t height)
{
	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	CHECK(g_v4l2_ioctl(fd, VIDIOC_S_FMT, &fmt) == 0);
	return 0;
}

static int request_buffers(int fd, uint32_t count)
{
	struct v4l2_requestbuffers rb;

	memset(&rb, 0, sizeof(rb));
	rb.count = count;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;
	CHECK(g_v4l2_ioctl(fd, VIDIOC_REQBUFS, &rb) == 0);
	CHECK(rb.count == count);
	return 0;
}

/* -------------------------------------------------------------------------
 * Concurrent lazy initialization
 * ------------------------------------------------------------------------- */

struct init_worker {
	pthread_barrier_t *barrier;
};

static void *init_thread(void *arg)
{
	struct init_worker *worker = arg;

	pthread_barrier_wait(worker->barrier);
	for (int i = 0; i < 16; i++) {
		int fd = open("/dev/null", O_RDONLY);

		THREAD_CHECK(fd >= 0);
		THREAD_CHECK(close(fd) == 0);

		fd = g_v4l2_open("/dev/null", O_RDONLY);
		THREAD_CHECK(fd >= 0);
		THREAD_CHECK(g_v4l2_close(fd) == 0);
	}
	return NULL;
}

static int run_init(void)
{
	enum { THREADS = 8 };
	pthread_t threads[THREADS];
	struct init_worker worker;
	pthread_barrier_t barrier;

	atomic_store(&g_thread_failed, false);
	CHECK(pthread_barrier_init(&barrier, NULL, THREADS) == 0);
	worker.barrier = &barrier;
	for (int i = 0; i < THREADS; i++)
		CHECK(pthread_create(&threads[i], NULL, init_thread,
				     &worker) == 0);
	for (int i = 0; i < THREADS; i++)
		CHECK(pthread_join(threads[i], NULL) == 0);
	CHECK(!atomic_load(&g_thread_failed));
	CHECK(pthread_barrier_destroy(&barrier) == 0);
	return 0;
}

/* -------------------------------------------------------------------------
 * Thread-local synthetic dirent storage (GC570D has video60 and video61)
 * ------------------------------------------------------------------------- */

struct dirent_worker {
	pthread_barrier_t *after_open;
	pthread_barrier_t *after_first;
	pthread_barrier_t *after_second;
	bool second_port;
	struct dirent *saved;
};

static struct dirent *find_name(DIR *dir, const char *name)
{
	for (int i = 0; i < 100000; i++) {
		struct dirent *ent = readdir(dir);

		if (!ent)
			return NULL;
		if (strcmp(ent->d_name, name) == 0)
			return ent;
	}
	return NULL;
}

static void *dirent_thread(void *arg)
{
	struct dirent_worker *worker = arg;
	DIR *dir = opendir("/dev");
	struct dirent *ent;

	THREAD_CHECK(dir != NULL);
	pthread_barrier_wait(worker->after_open);
	ent = find_name(dir, "video60");
	THREAD_CHECK(ent != NULL);
	if (!worker->second_port)
		worker->saved = ent;
	pthread_barrier_wait(worker->after_first);
	if (worker->second_port) {
		ent = find_name(dir, "video61");
		THREAD_CHECK(ent != NULL);
		worker->saved = ent;
	}
	pthread_barrier_wait(worker->after_second);
	THREAD_CHECK(closedir(dir) == 0);
	return NULL;
}

static int run_dirent(void)
{
	pthread_t threads[2];
	struct dirent_worker workers[2];
	pthread_barrier_t after_open;
	pthread_barrier_t after_first;
	pthread_barrier_t after_second;

	atomic_store(&g_thread_failed, false);
	CHECK(pthread_barrier_init(&after_open, NULL, 2) == 0);
	CHECK(pthread_barrier_init(&after_first, NULL, 2) == 0);
	CHECK(pthread_barrier_init(&after_second, NULL, 2) == 0);
	for (int i = 0; i < 2; i++) {
		workers[i].after_open = &after_open;
		workers[i].after_first = &after_first;
		workers[i].after_second = &after_second;
		workers[i].second_port = i != 0;
		workers[i].saved = NULL;
		CHECK(pthread_create(&threads[i], NULL, dirent_thread,
				     &workers[i]) == 0);
	}
	for (int i = 0; i < 2; i++)
		CHECK(pthread_join(threads[i], NULL) == 0);
	CHECK(!atomic_load(&g_thread_failed));
	CHECK(workers[0].saved != NULL);
	CHECK(workers[1].saved != NULL);
	CHECK(workers[0].saved != workers[1].saved);
	CHECK(strcmp(workers[0].saved->d_name, "video60") == 0);
	CHECK(strcmp(workers[1].saved->d_name, "video61") == 0);
	CHECK(pthread_barrier_destroy(&after_open) == 0);
	CHECK(pthread_barrier_destroy(&after_first) == 0);
	CHECK(pthread_barrier_destroy(&after_second) == 0);
	return 0;
}

/* -------------------------------------------------------------------------
 * GC573 + GC570D co-load, including an intentionally shared cookie
 * ------------------------------------------------------------------------- */

static int query_cookie(int fd, uint32_t index, uint32_t *cookie,
			uint32_t *length)
{
	struct v4l2_buffer buf;

	memset(&buf, 0, sizeof(buf));
	buf.index = index;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	CHECK(g_v4l2_ioctl(fd, VIDIOC_QUERYBUF, &buf) == 0);
	*cookie = buf.m.offset;
	*length = buf.length;
	return 0;
}

static int check_driver(int fd, const char *driver)
{
	struct v4l2_capability cap;

	memset(&cap, 0, sizeof(cap));
	CHECK(g_v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0);
	CHECK(strcmp((char *)cap.driver, driver) == 0);
	return 0;
}

static int run_coload(void)
{
	const uint8_t pattern_a = 0xa5;
	const uint8_t pattern_b = 0x5a;
	uint32_t cookie_a;
	uint32_t cookie_b;
	uint32_t len_a;
	uint32_t len_b;
	uint8_t *map_a;
	uint8_t *map_b;
	int fd_a;
	int fd_b;

	fd_a = g_v4l2_open(TEST_GC573_NODE, O_RDWR);
	fd_b = g_v4l2_open(TEST_GC570D_NODE, O_RDWR);
	CHECK(fd_a >= 0 && fd_b >= 0 && fd_a != fd_b);
	CHECK(check_driver(fd_a, "gc573-user") == 0);
	CHECK(check_driver(fd_b, "gc570d-user") == 0);
	CHECK(set_format(fd_a, 640, 360) == 0);
	CHECK(set_format(fd_b, 640, 360) == 0);
	CHECK(request_buffers(fd_a, 3) == 0);
	CHECK(request_buffers(fd_b, 3) == 0);

	CHECK(query_cookie(fd_a, 0, &cookie_a, &len_a) == 0);
	CHECK(cookie_a == TEST_GC573_BASE);
	CHECK(query_cookie(fd_b, 0, &cookie_b, &len_b) == 0);
	CHECK(cookie_b == TEST_GC570D_BASE);

	/* GC573 1 and GC570D 2 deliberately share the same numeric cookie. */
	CHECK(query_cookie(fd_a, 1, &cookie_a, &len_a) == 0);
	CHECK(query_cookie(fd_b, 2, &cookie_b, &len_b) == 0);
	CHECK(cookie_a == TEST_SHARED_COOKIE);
	CHECK(cookie_b == TEST_SHARED_COOKIE);

	errno = 0;
	CHECK(g_v4l2_mmap(NULL, len_a, PROT_READ | PROT_WRITE, MAP_SHARED,
			  fd_a, (int64_t)TEST_GC573_BASE + 1) == MAP_FAILED);
	CHECK(errno == EINVAL);

	map_a = g_v4l2_mmap(NULL, len_a, PROT_READ | PROT_WRITE, MAP_SHARED,
			    fd_a, cookie_a);
	CHECK(map_a != MAP_FAILED);
	map_b = g_v4l2_mmap(NULL, len_b, PROT_READ | PROT_WRITE, MAP_SHARED,
			    fd_b, cookie_b);
	CHECK(map_b != MAP_FAILED);
	CHECK(map_a != map_b);

	memset(map_a, pattern_a, 4096);
	memset(map_b, pattern_b, 4096);
	for (size_t i = 0; i < 4096; i++) {
		CHECK(map_a[i] == pattern_a);
		CHECK(map_b[i] == pattern_b);
	}

	CHECK(g_v4l2_close(fd_a) == 0);
	for (size_t i = 0; i < 4096; i++)
		CHECK(map_b[i] == pattern_b);
	CHECK(query_cookie(fd_b, 0, &cookie_b, &len_b) == 0);
	CHECK(cookie_b == TEST_GC570D_BASE);
	CHECK(g_v4l2_munmap(map_b, len_b) == 0);
	CHECK(g_v4l2_close(fd_b) == 0);
	return 0;
}

/* -------------------------------------------------------------------------
 * v4l2_dup table exhaustion must not leak the pre-created descriptor
 * ------------------------------------------------------------------------- */

static int run_dup(void)
{
	enum { HANDLE_LIMIT = 64 };
	int fds[HANDLE_LIMIT];
	int baseline;
	int fd;
	int dupfd;

	baseline = count_open_fds();
	fd = g_v4l2_open(TEST_GC573_NODE, O_RDWR);
	CHECK(fd >= 0);
	dupfd = g_v4l2_dup(fd);
	CHECK(dupfd >= 0);
	CHECK(g_v4l2_close(dupfd) == 0);
	CHECK(g_v4l2_close(fd) == 0);
	CHECK(count_open_fds() == baseline);

	for (int i = 0; i < HANDLE_LIMIT; i++) {
		fds[i] = g_v4l2_open(TEST_GC573_NODE, O_RDWR);
		CHECK(fds[i] >= 0);
	}
	CHECK(count_open_fds() == baseline + HANDLE_LIMIT);

	errno = 0;
	CHECK(g_v4l2_dup(fds[0]) == -1);
	CHECK(errno == EMFILE);
	CHECK(count_open_fds() == baseline + HANDLE_LIMIT);

	for (int i = 0; i < HANDLE_LIMIT; i++)
		CHECK(g_v4l2_close(fds[i]) == 0);
	CHECK(count_open_fds() == baseline);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";

	if (resolve_v4l2() != 0)
		return EXIT_FAILURE;
	if (strcmp(mode, "init") == 0)
		return run_init() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	if (strcmp(mode, "dirent") == 0)
		return run_dirent() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	if (strcmp(mode, "coload") == 0)
		return run_coload() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	if (strcmp(mode, "dup") == 0)
		return run_dup() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	fprintf(stderr,
		"usage: %s init|dirent|coload|dup\n", argv[0]);
	return EXIT_FAILURE;
}

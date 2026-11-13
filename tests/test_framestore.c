// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_framestore.c — Unit and concurrency tests for everlive_framestore
 */

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "everlive_framestore.h"

#define TEST_TIMEOUT_NS	1000000000ULL

static unsigned int g_name_seq;

static void make_name(char *buf, size_t size, const char *suffix)
{
	snprintf(buf, size, "/everlive_fs_test_%u_%u_%s",
		 (unsigned int)getpid(), g_name_seq++, suffix);
}

static void unlink_name(const char *name)
{
	shm_unlink(name);
}

static uint64_t monotonic_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void fill_words(void *buffer, size_t bytes, uint32_t value)
{
	uint32_t *words = buffer;
	size_t count = bytes / sizeof(*words);

	for (size_t i = 0; i < count; i++)
		words[i] = value;
}

static bool all_words_equal(const void *buffer, size_t bytes, uint32_t value)
{
	const uint32_t *words = buffer;
	size_t count = bytes / sizeof(*words);

	for (size_t i = 0; i < count; i++) {
		if (words[i] != value)
			return false;
	}
	return true;
}

static void corrupt_u32(const char *name, off_t offset, uint32_t value)
{
	int fd = shm_open(name, O_RDWR, 0);

	assert(fd >= 0);
	assert(pwrite(fd, &value, sizeof(value), offset) ==
	       (ssize_t)sizeof(value));
	close(fd);
}

static void test_create_roundtrip(void)
{
	const uint32_t width = 64;
	const uint32_t height = 32;
	const size_t bytes = (size_t)width * height * 2U;
	char name[128];
	struct everlive_framestore_config cfg;
	struct everlive_framestore *fs;
	struct everlive_framestore_frame frame;
	uint8_t *src;
	uint8_t *dst;

	printf("[*] Frame store create/round-trip...\n");
	make_name(name, sizeof(name), "roundtrip");
	cfg.name = name;
	cfg.pci_bdf = "0000:01:00.0";
	cfg.port_count = 1;
	cfg.slot_count = 3;
	cfg.frame_stride = (unsigned int)bytes;

	fs = everlive_framestore_create(&cfg);
	assert(fs);
	assert(everlive_framestore_port_count(fs) == 1);
	assert(everlive_framestore_slot_count(fs) == 3);
	assert(everlive_framestore_frame_stride(fs) >= bytes);
	assert(strcmp(everlive_framestore_bdf(fs), cfg.pci_bdf) == 0);
	assert(!everlive_framestore_alive(fs, TEST_TIMEOUT_NS));

	everlive_framestore_set_geometry(fs, 0, width, height);
	assert(!everlive_framestore_read_begin(fs, 0, &frame));

	src = malloc(bytes);
	dst = malloc(bytes);
	assert(src && dst);

	for (size_t i = 0; i < bytes; i++)
		src[i] = (uint8_t)(i * 7U + 3U);

	assert(everlive_framestore_publish(fs, 0, src, bytes, width, height,
					   false, true));
	assert(everlive_framestore_alive(fs, TEST_TIMEOUT_NS));

	memset(dst, 0, bytes);
	memset(&frame, 0, sizeof(frame));
	assert(everlive_framestore_read_snapshot(fs, 0, dst, bytes, &frame));
	assert(frame.width == width);
	assert(frame.height == height);
	assert(frame.bytes == bytes);
	assert(frame.data == dst);
	assert(frame.seq == 1);
	assert(frame.no_signal == 0);
	assert(frame.streaming == 1);
	assert(memcmp(src, dst, bytes) == 0);

	assert(everlive_framestore_read_begin(fs, 0, &frame));
	assert(frame.width == width);
	assert(frame.height == height);
	assert(everlive_framestore_read_validate(fs, 0, &frame));

	free(src);
	free(dst);
	everlive_framestore_destroy(fs);
	assert(everlive_framestore_attach(name) == NULL);
	unlink_name(name);
}

static void test_multi_port(void)
{
	const uint32_t width = 32;
	const uint32_t height = 16;
	const size_t bytes = (size_t)width * height * 2U;
	char name[128];
	struct everlive_framestore_config cfg;
	struct everlive_framestore *fs;
	struct everlive_framestore_frame frame;
	uint8_t *src;
	uint8_t *dst;

	printf("[*] Frame store multi-port isolation...\n");
	make_name(name, sizeof(name), "ports");
	cfg.name = name;
	cfg.pci_bdf = NULL;
	cfg.port_count = 2;
	cfg.slot_count = 3;
	cfg.frame_stride = (unsigned int)bytes;

	fs = everlive_framestore_create(&cfg);
	assert(fs);

	src = malloc(bytes);
	dst = malloc(bytes);
	assert(src && dst);

	fill_words(src, bytes, 0x11111111U);
	assert(everlive_framestore_publish(fs, 0, src, bytes, width, height,
					   false, true));
	fill_words(src, bytes, 0x22222222U);
	assert(everlive_framestore_publish(fs, 1, src, bytes, width, height,
					   false, true));

	memset(dst, 0, bytes);
	assert(everlive_framestore_read_snapshot(fs, 0, dst, bytes, &frame));
	assert(all_words_equal(dst, bytes, 0x11111111U));

	memset(dst, 0, bytes);
	assert(everlive_framestore_read_snapshot(fs, 1, dst, bytes, &frame));
	assert(all_words_equal(dst, bytes, 0x22222222U));

	free(src);
	free(dst);
	everlive_framestore_destroy(fs);
	unlink_name(name);
}

static void test_slot_rotation(void)
{
	const uint32_t width = 32;
	const uint32_t height = 16;
	const size_t bytes = (size_t)width * height * 2U;
	char name[128];
	struct everlive_framestore_config cfg;
	struct everlive_framestore *fs;
	struct everlive_framestore_frame frame;
	uint8_t *src;
	uint8_t *dst;

	printf("[*] Frame store slot rotation...\n");
	make_name(name, sizeof(name), "rotation");
	cfg.name = name;
	cfg.pci_bdf = NULL;
	cfg.port_count = 1;
	cfg.slot_count = 3;
	cfg.frame_stride = (unsigned int)bytes;

	fs = everlive_framestore_create(&cfg);
	assert(fs);
	src = malloc(bytes);
	dst = malloc(bytes);
	assert(src && dst);

	for (uint32_t i = 0; i < 10; i++) {
		fill_words(src, bytes, 0x10000000U + i);
		assert(everlive_framestore_publish(fs, 0, src, bytes, width,
						   height, false, true));
		memset(dst, 0, bytes);
		assert(everlive_framestore_read_snapshot(fs, 0, dst, bytes,
							 &frame));
		assert(frame.seq == i + 1U);
		assert(all_words_equal(dst, bytes, 0x10000000U + i));
	}

	free(src);
	free(dst);
	everlive_framestore_destroy(fs);
	unlink_name(name);
}

static void test_bad_attach(void)
{
	const uint32_t width = 16;
	const uint32_t height = 8;
	const size_t bytes = (size_t)width * height * 2U;
	char name[128];
	struct everlive_framestore_config cfg;
	struct everlive_framestore *fs;
	int fd;

	printf("[*] Frame store rejects invalid mappings...\n");

	make_name(name, sizeof(name), "badmagic");
	cfg.name = name;
	cfg.pci_bdf = NULL;
	cfg.port_count = 1;
	cfg.slot_count = 3;
	cfg.frame_stride = (unsigned int)bytes;
	fs = everlive_framestore_create(&cfg);
	assert(fs);
	corrupt_u32(name, 0, 0xdeadbeefU);
	assert(everlive_framestore_attach(name) == NULL);
	everlive_framestore_destroy(fs);
	unlink_name(name);

	make_name(name, sizeof(name), "badversion");
	cfg.name = name;
	fs = everlive_framestore_create(&cfg);
	assert(fs);
	corrupt_u32(name, 4, EVERLIVE_FRAMESTORE_VERSION + 100U);
	assert(everlive_framestore_attach(name) == NULL);
	everlive_framestore_destroy(fs);
	unlink_name(name);

	make_name(name, sizeof(name), "truncated");
	fd = shm_open(name, O_CREAT | O_RDWR, 0600);
	assert(fd >= 0);
	assert(ftruncate(fd, 8) == 0);
	close(fd);
	assert(everlive_framestore_attach(name) == NULL);
	unlink_name(name);

	/* Invalid create arguments. */
	make_name(name, sizeof(name), "badcreate");
	cfg.name = name;
	cfg.port_count = 0;
	cfg.slot_count = 3;
	cfg.frame_stride = (unsigned int)bytes;
	assert(everlive_framestore_create(&cfg) == NULL);
	cfg.port_count = 1;
	cfg.slot_count = 1;
	assert(everlive_framestore_create(&cfg) == NULL);
	cfg.slot_count = 3;
	cfg.frame_stride = 0;
	assert(everlive_framestore_create(&cfg) == NULL);
	unlink_name(name);
}

static void test_heartbeat_and_flags(void)
{
	const uint32_t width = 16;
	const uint32_t height = 8;
	const size_t bytes = (size_t)width * height * 2U;
	char name[128];
	struct everlive_framestore_config cfg;
	struct everlive_framestore *fs;
	struct everlive_framestore_frame frame;
	uint8_t *src;
	uint8_t *dst;

	printf("[*] Frame store heartbeat, flags and reset...\n");
	make_name(name, sizeof(name), "flags");
	cfg.name = name;
	cfg.pci_bdf = NULL;
	cfg.port_count = 1;
	cfg.slot_count = 3;
	cfg.frame_stride = (unsigned int)bytes;

	fs = everlive_framestore_create(&cfg);
	assert(fs);
	assert(!everlive_framestore_alive(fs, TEST_TIMEOUT_NS));

	src = malloc(bytes);
	dst = malloc(bytes);
	assert(src && dst);
	fill_words(src, bytes, 0xabcdef01U);

	assert(everlive_framestore_publish(fs, 0, src, bytes, width, height,
					   true, false));
	assert(everlive_framestore_alive(fs, TEST_TIMEOUT_NS));
	assert(everlive_framestore_read_snapshot(fs, 0, dst, bytes, &frame));
	assert(frame.no_signal == 1);
	assert(frame.streaming == 0);

	usleep(10000);
	assert(!everlive_framestore_alive(fs, 1000000ULL));

	assert(everlive_framestore_publish(fs, 0, src, bytes, width, height,
					   false, true));
	assert(everlive_framestore_read_snapshot(fs, 0, dst, bytes, &frame));
	assert(frame.no_signal == 0);
	assert(frame.streaming == 1);
	{
		uint64_t seq = frame.seq;

		everlive_framestore_mark_no_signal(fs, 0);
		assert(everlive_framestore_read_snapshot(fs, 0, dst, bytes,
							 &frame));
		assert(frame.no_signal == 1);
		assert(frame.streaming == 0);
		assert(frame.seq == seq);
	}

	everlive_framestore_reset(fs, 0);
	assert(!everlive_framestore_read_begin(fs, 0, &frame));
	assert(!everlive_framestore_read_snapshot(fs, 0, dst, bytes, &frame));

	assert(everlive_framestore_publish(fs, 0, src, bytes, width, height,
					   false, true));
	assert(everlive_framestore_read_snapshot(fs, 0, dst, bytes, &frame));
	assert(frame.seq == 1);

	free(src);
	free(dst);
	everlive_framestore_destroy(fs);
	unlink_name(name);
}

struct stress_ctx {
	struct everlive_framestore *fs;
	uint32_t width;
	uint32_t height;
	size_t bytes;
	atomic_bool stop;
	atomic_int failures;
	atomic_uint_fast64_t writes;
	atomic_uint_fast64_t reads;
};

static void *stress_writer(void *arg)
{
	struct stress_ctx *ctx = arg;
	uint8_t *frame = malloc(ctx->bytes);
	uint32_t value = 1;

	assert(frame);
	while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
		fill_words(frame, ctx->bytes, value);
		if (!everlive_framestore_publish(ctx->fs, 0, frame, ctx->bytes,
						 ctx->width, ctx->height,
						 false, true))
			atomic_fetch_add_explicit(&ctx->failures, 1,
						  memory_order_relaxed);
		value++;
		atomic_fetch_add_explicit(&ctx->writes, 1,
					  memory_order_relaxed);
		usleep(100);
	}
	free(frame);
	return NULL;
}

static void *stress_reader(void *arg)
{
	struct stress_ctx *ctx = arg;
	uint8_t *frame = malloc(ctx->bytes);

	assert(frame);
	while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
		struct everlive_framestore_frame out;
		uint32_t first;
		bool equal = true;

		if (!everlive_framestore_read_snapshot(ctx->fs, 0, frame,
						       ctx->bytes, &out))
			continue;

		if (out.bytes != ctx->bytes || out.width != ctx->width ||
		    out.height != ctx->height) {
			atomic_fetch_add_explicit(&ctx->failures, 1,
						  memory_order_relaxed);
			break;
		}

		memcpy(&first, frame, sizeof(first));
		{
			const uint32_t *words = (const uint32_t *)frame;
			size_t count = ctx->bytes / sizeof(*words);

			for (size_t i = 0; i < count; i++) {
				if (words[i] != first) {
					equal = false;
					break;
				}
			}
		}
		if (!equal) {
			atomic_fetch_add_explicit(&ctx->failures, 1,
						  memory_order_relaxed);
			break;
		}
		atomic_fetch_add_explicit(&ctx->reads, 1,
					  memory_order_relaxed);
	}
	free(frame);
	return NULL;
}

static void test_concurrency(void)
{
	const uint32_t width = 320;
	const uint32_t height = 180;
	const size_t bytes = (size_t)width * height * 2U;
	char name[128];
	struct everlive_framestore_config cfg;
	struct everlive_framestore *fs;
	struct stress_ctx ctx;
	pthread_t writer;
	pthread_t readers[2];
	uint64_t start;
	uint64_t now;

	printf("[*] Frame store concurrent writer/readers...\n");
	make_name(name, sizeof(name), "stress");
	cfg.name = name;
	cfg.pci_bdf = NULL;
	cfg.port_count = 1;
	cfg.slot_count = 3;
	cfg.frame_stride = (unsigned int)bytes;
	fs = everlive_framestore_create(&cfg);
	assert(fs);

	memset(&ctx, 0, sizeof(ctx));
	ctx.fs = fs;
	ctx.width = width;
	ctx.height = height;
	ctx.bytes = bytes;
	atomic_init(&ctx.stop, false);
	atomic_init(&ctx.failures, 0);
	atomic_init(&ctx.writes, 0);
	atomic_init(&ctx.reads, 0);

	assert(pthread_create(&writer, NULL, stress_writer, &ctx) == 0);
	assert(pthread_create(&readers[0], NULL, stress_reader, &ctx) == 0);
	assert(pthread_create(&readers[1], NULL, stress_reader, &ctx) == 0);

	start = monotonic_now_ns();
	do {
		usleep(1000);
		now = monotonic_now_ns();
	} while (now - start < 1000000000ULL);

	atomic_store_explicit(&ctx.stop, true, memory_order_relaxed);
	pthread_join(writer, NULL);
	pthread_join(readers[0], NULL);
	pthread_join(readers[1], NULL);

	assert(atomic_load_explicit(&ctx.failures, memory_order_relaxed) == 0);
	assert(atomic_load_explicit(&ctx.writes, memory_order_relaxed) > 0);
	assert(atomic_load_explicit(&ctx.reads, memory_order_relaxed) > 0);

	everlive_framestore_destroy(fs);
	unlink_name(name);
}

int main(void)
{
	printf("=== Running Frame Store Tests ===\n");
	test_create_roundtrip();
	test_multi_port();
	test_slot_rotation();
	test_bad_attach();
	test_heartbeat_and_flags();
	test_concurrency();
	printf("=== All Frame Store Tests Passed! ===\n");
	return 0;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * everlive_framestore.c — Lock-free shared-memory frame store for EverLive
 *
 * Layout (all fields little-endian/native, same ABI on publisher/consumer):
 *
 *   header
 *   port 0 slot 0 payload
 *   port 0 slot 1 payload
 *   ...
 *   port 1 slot 0 payload
 *   ...
 *
 * The payload offset is 64-byte aligned.  The header layout is private to
 * this translation unit; callers use the opaque handle and frame view.
 */

#include "everlive_framestore.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FRAMESTORE_NAME_MAX	64U
#define FRAMESTORE_ALIGN	64U

struct everlive_framestore_port {
	uint64_t seq;
	uint64_t timestamp_ns;
	uint32_t width;
	uint32_t height;
	uint32_t no_signal;
	uint32_t streaming;
	uint32_t frame_bytes;
	uint32_t write_idx;
	uint32_t ready_idx;
	uint32_t read_idx;
	uint32_t reserved;
};

struct everlive_framestore_header {
	uint32_t magic;
	uint32_t version;
	uint32_t port_count;
	uint32_t slot_count;
	uint32_t frame_stride;
	uint32_t reserved0;
	char pci_bdf[32];
	uint32_t publisher_pid;
	uint32_t reserved1;
	uint64_t heartbeat_ns;
	struct everlive_framestore_port ports[EVERLIVE_FRAMESTORE_MAX_PORTS];
};

struct everlive_framestore {
	void *map;
	size_t map_size;
	size_t payload_offset;
	int fd;
	bool owner;
	char name[FRAMESTORE_NAME_MAX];
};

static size_t align_up(size_t value, size_t alignment)
{
	return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint64_t monotonic_now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_us(unsigned int usec)
{
	struct timespec ts = {
		.tv_sec = (time_t)(usec / 1000000U),
		.tv_nsec = (long)(usec % 1000000U) * 1000L,
	};

	nanosleep(&ts, NULL);
}

static bool calc_layout(size_t *payload_offset, size_t *region_size,
			uint32_t port_count, uint32_t slot_count,
			uint32_t frame_stride)
{
	size_t offset;
	size_t frames;
	size_t payload;

	if (!payload_offset || !region_size || port_count == 0 ||
	    port_count > EVERLIVE_FRAMESTORE_MAX_PORTS || slot_count < 2 ||
	    slot_count > EVERLIVE_FRAMESTORE_MAX_SLOTS || frame_stride == 0)
		return false;

	if ((size_t)port_count > SIZE_MAX / (size_t)slot_count)
		return false;
	frames = (size_t)port_count * (size_t)slot_count;
	if (frames > SIZE_MAX / (size_t)frame_stride)
		return false;
	payload = frames * (size_t)frame_stride;

	offset = align_up(sizeof(struct everlive_framestore_header),
			  FRAMESTORE_ALIGN);
	if (offset > SIZE_MAX - payload)
		return false;

	*payload_offset = offset;
	*region_size = offset + payload;
	return true;
}

static bool validate_header(const struct everlive_framestore_header *hdr,
			    size_t object_size, size_t *payload_offset,
			    size_t *region_size)
{
	size_t offset;
	size_t region;

	if (!hdr || !payload_offset || !region_size)
		return false;
	if (hdr->magic != EVERLIVE_FRAMESTORE_MAGIC ||
	    hdr->version != EVERLIVE_FRAMESTORE_VERSION)
		return false;
	if (hdr->port_count == 0 ||
	    hdr->port_count > EVERLIVE_FRAMESTORE_MAX_PORTS ||
	    hdr->slot_count < 2 ||
	    hdr->slot_count > EVERLIVE_FRAMESTORE_MAX_SLOTS)
		return false;
	if (hdr->frame_stride == 0 ||
	    (hdr->frame_stride % FRAMESTORE_ALIGN) != 0)
		return false;

	if (!calc_layout(&offset, &region, hdr->port_count, hdr->slot_count,
			 hdr->frame_stride))
		return false;
	if (object_size < region)
		return false;

	*payload_offset = offset;
	*region_size = region;
	return true;
}

static uint8_t *port_base(struct everlive_framestore *fs,
			  struct everlive_framestore_header *hdr,
			  unsigned int port, uint32_t slot)
{
	return (uint8_t *)fs->map + fs->payload_offset +
	       ((size_t)port * hdr->slot_count + slot) * hdr->frame_stride;
}

static bool frame_is_consistent(uint32_t width, uint32_t height,
				uint32_t frame_bytes, uint32_t frame_stride)
{
	uint64_t expected;

	if (width == 0 || height == 0 || frame_bytes == 0)
		return false;
	if (frame_bytes > frame_stride)
		return false;

	/* YUY2 has two bytes per pixel; reject geometry that cannot fit. */
	if ((uint64_t)width * (uint64_t)height > (uint64_t)UINT32_MAX / 2ULL)
		return false;
	expected = (uint64_t)width * (uint64_t)height * 2ULL;
	return expected == frame_bytes;
}

struct everlive_framestore *
everlive_framestore_create(const struct everlive_framestore_config *cfg)
{
	struct everlive_framestore *fs;
	struct everlive_framestore_header *hdr;
	struct stat st;
	size_t payload_offset;
	size_t region_size;
	uint32_t frame_stride;
	int fd = -1;
	void *map = MAP_FAILED;

	if (!cfg || !cfg->name || cfg->name[0] != '/' ||
	    strlen(cfg->name) >= FRAMESTORE_NAME_MAX)
		return NULL;

	if (cfg->frame_stride == 0 ||
	    cfg->frame_stride > UINT32_MAX - (FRAMESTORE_ALIGN - 1U))
		return NULL;
	frame_stride = (uint32_t)align_up(cfg->frame_stride, FRAMESTORE_ALIGN);

	if (!calc_layout(&payload_offset, &region_size, cfg->port_count,
			 cfg->slot_count, frame_stride))
		return NULL;

	fd = shm_open(cfg->name, O_CREAT | O_RDWR, 0600);
	if (fd < 0)
		return NULL;

	if (fchmod(fd, 0600) != 0)
		goto fail;

	if (ftruncate(fd, (off_t)region_size) != 0)
		goto fail;

	map = mmap(NULL, region_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fd, 0);
	if (map == MAP_FAILED)
		goto fail;

	if (fstat(fd, &st) != 0 || st.st_size < (off_t)region_size) {
		munmap(map, region_size);
		map = MAP_FAILED;
		goto fail;
	}

	fs = calloc(1, sizeof(*fs));
	if (!fs) {
		munmap(map, region_size);
		map = MAP_FAILED;
		goto fail;
	}

	memset(map, 0, region_size);

	hdr = map;
	hdr->magic = EVERLIVE_FRAMESTORE_MAGIC;
	hdr->version = EVERLIVE_FRAMESTORE_VERSION;
	hdr->port_count = (uint32_t)cfg->port_count;
	hdr->slot_count = (uint32_t)cfg->slot_count;
	hdr->frame_stride = frame_stride;
	hdr->publisher_pid = (uint32_t)getpid();
	/* A heartbeat of zero means "no frame published yet". */
	hdr->heartbeat_ns = 0;
	if (cfg->pci_bdf)
		snprintf(hdr->pci_bdf, sizeof(hdr->pci_bdf), "%s",
			 cfg->pci_bdf);

	for (uint32_t p = 0; p < hdr->port_count; p++) {
		hdr->ports[p].no_signal = 1;
		hdr->ports[p].write_idx = 0;
		hdr->ports[p].ready_idx = 0;
		hdr->ports[p].read_idx = 0;
	}

	fs->map = map;
	fs->map_size = region_size;
	fs->payload_offset = payload_offset;
	fs->fd = fd;
	fs->owner = true;
	snprintf(fs->name, sizeof(fs->name), "%s", cfg->name);
	return fs;

fail:
	if (map != MAP_FAILED)
		munmap(map, region_size);
	if (fd >= 0)
		close(fd);
	return NULL;
}

struct everlive_framestore *everlive_framestore_attach(const char *name)
{
	struct everlive_framestore *fs;
	struct everlive_framestore_header *hdr;
	struct stat st;
	size_t payload_offset;
	size_t region_size;
	size_t header_map_size;
	void *header_map;
	void *map;
	int fd;

	if (!name || name[0] != '/' || strlen(name) >= FRAMESTORE_NAME_MAX)
		return NULL;

	fd = shm_open(name, O_RDONLY, 0);
	if (fd < 0)
		return NULL;

	if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(*hdr)) {
		close(fd);
		return NULL;
	}

	header_map_size = sizeof(*hdr);
	header_map = mmap(NULL, header_map_size, PROT_READ, MAP_SHARED, fd, 0);
	if (header_map == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	hdr = header_map;
	if (!validate_header(hdr, (size_t)st.st_size, &payload_offset,
			     &region_size)) {
		munmap(header_map, header_map_size);
		close(fd);
		return NULL;
	}

	munmap(header_map, header_map_size);

	map = mmap(NULL, region_size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	fs = calloc(1, sizeof(*fs));
	if (!fs) {
		munmap(map, region_size);
		close(fd);
		return NULL;
	}

	fs->map = map;
	fs->map_size = region_size;
	fs->payload_offset = payload_offset;
	fs->fd = -1;
	fs->owner = false;
	snprintf(fs->name, sizeof(fs->name), "%s", name);

	close(fd);
	return fs;
}

void everlive_framestore_destroy(struct everlive_framestore *fs)
{
	struct stat created;
	struct stat current;
	bool ours = false;
	int probe;

	if (!fs)
		return;

	if (fs->owner && fs->fd >= 0) {
		probe = shm_open(fs->name, O_RDONLY, 0);
		if (probe >= 0) {
			if (fstat(fs->fd, &created) == 0 &&
			    fstat(probe, &current) == 0 &&
			    created.st_dev == current.st_dev &&
			    created.st_ino == current.st_ino)
				ours = true;
			close(probe);
		}
	}

	if (fs->map && fs->map_size)
		munmap(fs->map, fs->map_size);
	if (fs->fd >= 0)
		close(fs->fd);
	if (fs->owner && ours)
		shm_unlink(fs->name);

	free(fs);
}

const char *everlive_framestore_bdf(const struct everlive_framestore *fs)
{
	const struct everlive_framestore_header *hdr;

	if (!fs || !fs->map)
		return NULL;
	hdr = fs->map;
	return hdr->pci_bdf;
}

unsigned int everlive_framestore_port_count(const struct everlive_framestore *fs)
{
	const struct everlive_framestore_header *hdr;

	if (!fs || !fs->map)
		return 0;
	hdr = fs->map;
	return hdr->port_count;
}

unsigned int everlive_framestore_slot_count(const struct everlive_framestore *fs)
{
	const struct everlive_framestore_header *hdr;

	if (!fs || !fs->map)
		return 0;
	hdr = fs->map;
	return hdr->slot_count;
}

unsigned int everlive_framestore_frame_stride(const struct everlive_framestore *fs)
{
	const struct everlive_framestore_header *hdr;

	if (!fs || !fs->map)
		return 0;
	hdr = fs->map;
	return hdr->frame_stride;
}

bool everlive_framestore_publish(struct everlive_framestore *fs,
				 unsigned int port,
				 const void *frame, size_t bytes,
				 uint32_t width, uint32_t height,
				 bool no_signal, bool streaming)
{
	struct everlive_framestore_header *hdr;
	struct everlive_framestore_port *sp;
	uint32_t slot;
	uint64_t seq;
	uint64_t now;
	uint8_t *dst;

	if (!fs || !fs->map || !frame || bytes == 0)
		return false;

	hdr = fs->map;
	if (port >= hdr->port_count || bytes > hdr->frame_stride)
		return false;
	if (!frame_is_consistent(width, height, (uint32_t)bytes,
				 hdr->frame_stride))
		return false;

	sp = &hdr->ports[port];
	slot = __atomic_load_n(&sp->write_idx, __ATOMIC_RELAXED);
	if (slot >= hdr->slot_count)
		slot = 0;
	seq = __atomic_load_n(&sp->seq, __ATOMIC_RELAXED);

	dst = port_base(fs, hdr, port, slot);
	memcpy(dst, frame, bytes);

	now = monotonic_now_ns();
	__atomic_store_n(&sp->width, width, __ATOMIC_RELAXED);
	__atomic_store_n(&sp->height, height, __ATOMIC_RELAXED);
	__atomic_store_n(&sp->frame_bytes, (uint32_t)bytes, __ATOMIC_RELAXED);
	__atomic_store_n(&sp->no_signal, no_signal ? 1U : 0U,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&sp->streaming, streaming ? 1U : 0U,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&sp->timestamp_ns, now, __ATOMIC_RELAXED);
	__atomic_store_n(&hdr->heartbeat_ns, now, __ATOMIC_RELEASE);

	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&sp->ready_idx, slot, __ATOMIC_RELEASE);
	__atomic_store_n(&sp->seq, seq + 1ULL, __ATOMIC_RELEASE);
	__atomic_store_n(&sp->write_idx,
			 (slot + 1U) % hdr->slot_count, __ATOMIC_RELAXED);
	return true;
}

bool everlive_framestore_read_begin(struct everlive_framestore *fs,
				    unsigned int port,
				    struct everlive_framestore_frame *out)
{
	struct everlive_framestore_header *hdr;
	struct everlive_framestore_port *sp;
	uint64_t seq;
	uint32_t slot;
	uint32_t width;
	uint32_t height;
	uint32_t frame_bytes;
	uint32_t no_signal;
	uint32_t streaming;
	uint64_t timestamp_ns;

	if (!fs || !fs->map || !out)
		return false;

	hdr = fs->map;
	if (port >= hdr->port_count)
		return false;

	memset(out, 0, sizeof(*out));
	sp = &hdr->ports[port];

	seq = __atomic_load_n(&sp->seq, __ATOMIC_ACQUIRE);
	if (seq == 0)
		return false;

	slot = __atomic_load_n(&sp->ready_idx, __ATOMIC_ACQUIRE);
	if (slot >= hdr->slot_count)
		return false;

	width = __atomic_load_n(&sp->width, __ATOMIC_ACQUIRE);
	height = __atomic_load_n(&sp->height, __ATOMIC_ACQUIRE);
	frame_bytes = __atomic_load_n(&sp->frame_bytes, __ATOMIC_ACQUIRE);
	no_signal = __atomic_load_n(&sp->no_signal, __ATOMIC_ACQUIRE);
	streaming = __atomic_load_n(&sp->streaming, __ATOMIC_ACQUIRE);
	timestamp_ns = __atomic_load_n(&sp->timestamp_ns, __ATOMIC_ACQUIRE);

	if (!frame_is_consistent(width, height, frame_bytes,
				 hdr->frame_stride))
		return false;

	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	out->data = port_base(fs, hdr, port, slot);
	out->bytes = frame_bytes;
	out->width = width;
	out->height = height;
	out->no_signal = no_signal;
	out->streaming = streaming;
	out->seq = seq;
	out->timestamp_ns = timestamp_ns;
	out->slot = slot;
	return true;
}

bool everlive_framestore_read_validate(struct everlive_framestore *fs,
				       unsigned int port,
				       const struct everlive_framestore_frame *frame)
{
	struct everlive_framestore_header *hdr;

	if (!fs || !fs->map || !frame || port >= EVERLIVE_FRAMESTORE_MAX_PORTS)
		return false;

	hdr = fs->map;
	if (port >= hdr->port_count || frame->seq == 0)
		return false;

	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&hdr->ports[port].seq,
			       __ATOMIC_ACQUIRE) == frame->seq;
}

bool everlive_framestore_read_snapshot(struct everlive_framestore *fs,
				       unsigned int port,
				       void *dst, size_t dst_size,
				       struct everlive_framestore_frame *out)
{
	struct everlive_framestore_frame frame;
	unsigned int tries;

	if (!fs || !fs->map || !dst || dst_size == 0 || !out)
		return false;

	memset(out, 0, sizeof(*out));

	for (tries = 0; tries < 8U; tries++) {
		if (!everlive_framestore_read_begin(fs, port, &frame))
			break;

		if (frame.bytes > dst_size) {
			memset(dst, 0, dst_size);
			return false;
		}

		memcpy(dst, frame.data, frame.bytes);
		if (everlive_framestore_read_validate(fs, port, &frame)) {
			*out = frame;
			out->data = dst;
			return true;
		}
		sleep_us(100U);
	}

	memset(dst, 0, dst_size);
	memset(out, 0, sizeof(*out));
	return false;
}

bool everlive_framestore_alive(const struct everlive_framestore *fs,
			       uint64_t max_age_ns)
{
	const struct everlive_framestore_header *hdr;
	uint64_t heartbeat;
	uint64_t now;
	uint64_t age;

	if (!fs || !fs->map)
		return false;

	hdr = fs->map;
	heartbeat = __atomic_load_n(&hdr->heartbeat_ns, __ATOMIC_ACQUIRE);
	if (heartbeat == 0)
		return false;

	now = monotonic_now_ns();
	if (now == 0)
		return false;

	age = now >= heartbeat ? now - heartbeat : 0;
	return age <= max_age_ns;
}

void everlive_framestore_reset(struct everlive_framestore *fs,
			       unsigned int port)
{
	struct everlive_framestore_header *hdr;
	struct everlive_framestore_port *sp;

	if (!fs || !fs->map)
		return;

	hdr = fs->map;
	if (port >= hdr->port_count)
		return;

	sp = &hdr->ports[port];
	__atomic_store_n(&sp->seq, 0ULL, __ATOMIC_RELEASE);
	__atomic_store_n(&sp->timestamp_ns, 0ULL, __ATOMIC_RELAXED);
	__atomic_store_n(&sp->width, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&sp->height, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&sp->frame_bytes, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&sp->no_signal, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&sp->streaming, 0U, __ATOMIC_RELEASE);
	__atomic_store_n(&sp->write_idx, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&sp->ready_idx, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&sp->read_idx, 0U, __ATOMIC_RELAXED);
}

void everlive_framestore_mark_no_signal(struct everlive_framestore *fs,
					unsigned int port)
{
	struct everlive_framestore_header *hdr;

	if (!fs || !fs->map)
		return;

	hdr = fs->map;
	if (port >= hdr->port_count)
		return;

	__atomic_store_n(&hdr->ports[port].no_signal, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&hdr->ports[port].streaming, 0U, __ATOMIC_RELEASE);
}

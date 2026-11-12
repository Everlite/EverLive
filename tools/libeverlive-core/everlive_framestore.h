/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_framestore.h — Lock-free shared-memory frame store for EverLive
 *
 * Publishers (the GC570D and GC573 capture engines) write complete YUY2
 * frames into a POSIX shared-memory object.  Consumers (the V4L2 preload
 * shim, the GC570D presentation frontend, diagnostic tools) read a stable
 * snapshot without taking a lock.
 *
 * The store is triple-buffered: the producer always fills a private back
 * slot, then publishes it with release semantics.  A reader copies or
 * converts from the published slot and re-checks the sequence number before
 * accepting the result, so a slow consumer never stalls the capture thread
 * and never observes a torn frame.
 *
 * The shared-memory ABI is versioned.  The header layout is private to
 * everlive_framestore.c; callers use the opaque handle and the frame view.
 */

#ifndef EVERLIVE_FRAMESTORE_H
#define EVERLIVE_FRAMESTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVERLIVE_FRAMESTORE_MAGIC	0x45564c53U /* "EVLS" */
#define EVERLIVE_FRAMESTORE_VERSION	3U
#define EVERLIVE_FRAMESTORE_MAX_PORTS	2U
#define EVERLIVE_FRAMESTORE_MAX_SLOTS	8U

struct everlive_framestore;

struct everlive_framestore_config {
	const char *name;	/* POSIX SHM name, e.g. "/gc573_v4l2_shm" */
	const char *pci_bdf;	/* optional PCI BDF string */
	unsigned int port_count;
	unsigned int slot_count;
	unsigned int frame_stride; /* maximum bytes per frame/slot */
};

/*
 * A read view returned by everlive_framestore_read_begin().
 *
 * @data points into the shared store and is only stable until the producer
 * wraps far enough to reuse that slot.  Callers must copy/convert and then
 * call everlive_framestore_read_validate() before using the result.
 */
struct everlive_framestore_frame {
	const uint8_t *data;
	size_t bytes;
	uint32_t width;
	uint32_t height;
	uint32_t no_signal;
	uint32_t streaming;
	uint64_t seq;
	uint64_t timestamp_ns;
	uint32_t slot;
};

/*
 * Create (or truncate and reinitialise) a store.
 *
 * The object is created 0600.  Returns NULL on invalid arguments, allocation
 * failure, SHM creation failure, or mapping failure.
 */
struct everlive_framestore *
everlive_framestore_create(const struct everlive_framestore_config *cfg);

/*
 * Attach to an existing store read-only.
 *
 * Validates magic, version, geometry fields and the object size.  Returns
 * NULL when the object does not exist or does not match the current ABI.
 * Never unlinks the object.
 */
struct everlive_framestore *everlive_framestore_attach(const char *name);

/*
 * Unmap a store.  When @fs was created by this process, unlink the object
 * only if it is still the same inode that this handle created.
 */
void everlive_framestore_destroy(struct everlive_framestore *fs);

const char *everlive_framestore_bdf(const struct everlive_framestore *fs);
unsigned int everlive_framestore_port_count(const struct everlive_framestore *fs);
unsigned int everlive_framestore_slot_count(const struct everlive_framestore *fs);
unsigned int everlive_framestore_frame_stride(const struct everlive_framestore *fs);

/*
 * Publish one complete frame to @port.
 *
 * @bytes must be non-zero and no larger than the configured frame stride.
 * The producer never blocks on readers.
 */
bool everlive_framestore_publish(struct everlive_framestore *fs,
				 unsigned int port,
				 const void *frame, size_t bytes,
				 uint32_t width, uint32_t height,
				 bool no_signal, bool streaming);

/*
 * Begin a zero-copy read of the newest published frame.
 *
 * Returns false when no frame has been published yet, the port is invalid,
 * or the published metadata is inconsistent.  On success, the caller must
 * copy/convert from @out->data and then call
 * everlive_framestore_read_validate().
 */
bool everlive_framestore_read_begin(struct everlive_framestore *fs,
				    unsigned int port,
				    struct everlive_framestore_frame *out);

/*
 * Validate that the frame view is still the one the producer published when
 * read_begin() returned.  Returns false if the sequence changed.
 */
bool everlive_framestore_read_validate(struct everlive_framestore *fs,
				       unsigned int port,
				       const struct everlive_framestore_frame *frame);

/*
 * Copy the newest stable frame into @dst.
 *
 * On success @out->data is set to @dst, so the returned view owns a stable
 * copy rather than pointing into the shared store.  Retries a bounded number
 * of times when the producer is publishing.  On failure @out is zeroed and
 * @dst is zero-filled up to @dst_size.
 */
bool everlive_framestore_read_snapshot(struct everlive_framestore *fs,
				       unsigned int port,
				       void *dst, size_t dst_size,
				       struct everlive_framestore_frame *out);

/*
 * True when the publisher heartbeat is recent enough.
 *
 * A heartbeat of 0 means the store has never been published.  max_age_ns is
 * measured against CLOCK_MONOTONIC.
 */
bool everlive_framestore_alive(const struct everlive_framestore *fs,
			       uint64_t max_age_ns);

/*
 * Reset a port to its initial state: seq = 0, no_signal = 1, streaming = 0,
 * zero geometry and slot indices.  The heartbeat is left untouched.
 */
void everlive_framestore_reset(struct everlive_framestore *fs,
			       unsigned int port);

/*
 * Set the advertised geometry for a port before the first frame is
 * published.  This does not create a readable frame: readers still require a
 * non-zero sequence number.  Useful for diagnostics and for preserving the
 * previous "initial width/height" behaviour.
 */
void everlive_framestore_set_geometry(struct everlive_framestore *fs,
				      unsigned int port,
				      uint32_t width, uint32_t height);

/*
 * Mark a port as temporarily unavailable: no_signal = 1, streaming = 0.
 * Does not clear seq or the heartbeat.
 */
void everlive_framestore_mark_no_signal(struct everlive_framestore *fs,
					unsigned int port);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_FRAMESTORE_H */

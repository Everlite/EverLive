// SPDX-License-Identifier: GPL-2.0-only
/*
 * hws_user_hw.c — userspace VFIO hardware layer for the HWS UHD family.
 *
 * Register sequences follow the card's documented behaviour; see
 * hws_user_hw.h for the DMA model.
 */

#include "hws_user_hw.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>


#define ROUND_UP(x, a)	(((x) + (a) - 1) / (a) * (a))

int hws_user_quarter_layout(unsigned int w, unsigned int h,
			    uint32_t len[4], uint32_t off[4])
{
	uint64_t frame, q, rest;

	if (!w || !h || (w & 1) || w > HWS_MAX_WIDTH || h > HWS_MAX_HEIGHT)
		return -1;
	frame = (uint64_t)w * h * 2;
	/* Quarters 0-2: a quarter of the frame in whole 2 KiB blocks. */
	q = frame / (4 * HWS_DMA_BLOCK) * HWS_DMA_BLOCK;
	if (!q)
		return -1;
	rest = frame - 3 * q;
	len[0] = len[1] = len[2] = (uint32_t)q;
	/* Quarter 3: the rest, rounded up to the next block as the card expects. */
	len[3] = (uint32_t)((rest / HWS_DMA_BLOCK + 1) * HWS_DMA_BLOCK);
	off[0] = 0;
	off[1] = len[0];
	off[2] = len[0] + len[1];
	off[3] = len[0] + len[1] + len[2];
	return 0;
}

size_t hws_user_slot_stride(void)
{
	size_t max = (size_t)HWS_MAX_WIDTH * HWS_MAX_HEIGHT * 2 + 2 * HWS_DMA_BLOCK;

	return ROUND_UP(max + HWS_USER_SLOT_SLACK, 64 * 1024);
}

static int read_sysfs_hex(const char *bdf, const char *attr, unsigned int *out)
{
	char path[256];
	FILE *f;
	int ok;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", bdf, attr);
	f = fopen(path, "r");
	if (!f)
		return -errno;
	ok = fscanf(f, "%x", out) == 1;
	fclose(f);
	return ok ? 0 : -EINVAL;
}

static inline uint32_t card_addr(uint64_t iova, unsigned int window)
{
	return (uint32_t)((window + 1) * HWS_WINDOW_SIZE + (iova & HWS_WINDOW_MASK));
}

static inline uint64_t slot_iova(const struct hws_user_dev *dev, unsigned int slot)
{
	return HWS_USER_IOVA_BASE + (uint64_t)slot * dev->slot_stride;
}

static void core_run(struct hws_user_dev *dev);
static void core_shutdown(struct hws_user_dev *dev);

static void core_probe_reset(struct hws_user_dev *dev)
{
	hws_user_wr(dev, HWS_REG_STATUS, HWS_CTL_RESET);
	usleep(100);
	hws_user_wr(dev, HWS_REG_STATUS, HWS_CTL_STOP);
	usleep(500);
}

int hws_user_open(struct hws_user_dev *dev, const char *bdf, unsigned int nslots)
{
	char addr[64];
	unsigned int id = 0;
	int ret;

	memset(dev, 0, sizeof(*dev));
	dev->video_toggle = -1;

	if (bdf && bdf[0]) {
		if (!hws_vfio_is_supported(bdf)) {
			fprintf(stderr, "[HWS] %s is not an HWS UHD card; refusing to touch it\n", bdf);
			return -ENODEV;
		}
		snprintf(addr, sizeof(addr), "%s", bdf);
	} else if ((ret = hws_vfio_resolve(NULL, addr, sizeof(addr), NULL)) < 0) {
		return ret;
	}
	if (read_sysfs_hex(addr, "device", &id) == 0)
		dev->device_id = (uint16_t)id;

	ret = hws_vfio_init(&dev->vfio, addr);
	if (ret < 0)
		return ret;
	dev->vfio_open = true;
	dev->bar0 = dev->vfio.bar0;
	if (dev->vfio.bar0_size < HWS_CAP(128)) {
		fprintf(stderr, "[HWS] BAR0 is %zu bytes, too small for the capture block\n",
			dev->vfio.bar0_size);
		ret = -ENODEV;
		goto fail;
	}

	dev->devinfo = hws_user_rd(dev, HWS_REG_DEVINFO);
	if (dev->devinfo == HWS_STATUS_GONE) {
		fprintf(stderr, "[HWS] card reads all ones; is it powered and linked?\n");
		ret = -EIO;
		goto fail;
	}
	dev->fpga_version = (dev->devinfo >> 8) & 0xFF;
	dev->fpga_subversion = (dev->devinfo >> 16) & 0xFF;
	dev->out_format = (dev->devinfo >> 28) & 0x0F;
	if (dev->out_format != 0) {
		fprintf(stderr, "[HWS] output format %u is not YUYV; unsupported\n", dev->out_format);
		ret = -ENOTSUP;
		goto fail;
	}

	if (nslots < HWS_USER_MIN_SLOTS)
		nslots = HWS_USER_MIN_SLOTS;
	if (nslots > HWS_USER_MAX_SLOTS)
		nslots = HWS_USER_MAX_SLOTS;
	dev->nslots = nslots;
	dev->slot_stride = hws_user_slot_stride();
	dev->arena_size = dev->slot_stride * nslots + 64 * 1024;	/* + audio ring page */
	if (dev->arena_size > HWS_WINDOW_SIZE) {
		ret = -E2BIG;
		goto fail;
	}
	dev->arena = mmap(NULL, dev->arena_size, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (dev->arena == MAP_FAILED) {
		dev->arena = NULL;
		ret = -errno;
		goto fail;
	}
	memset(dev->arena, 0, dev->arena_size);
	ret = hws_vfio_map_dma(&dev->vfio, dev->arena, dev->arena_size, HWS_USER_IOVA_BASE);
	if (ret < 0) {
		fprintf(stderr, "[HWS] pinning %zu MiB failed; raise RLIMIT_MEMLOCK or setcap cap_ipc_lock=+ep\n",
			dev->arena_size >> 20);
		goto fail;
	}
	dev->arena_mapped = true;
	dev->audio = dev->arena + dev->slot_stride * nslots;
	dev->audio_iova = HWS_USER_IOVA_BASE + dev->slot_stride * nslots;

	ret = hws_vfio_setup_msi(&dev->vfio);
	if (ret < 0)
		goto fail;

	/*
	 * Reset, then run the core with a placeholder geometry: the input
	 * registers only report the live signal while it runs.  No capture
	 * engine is enabled, so nothing is written to memory yet.
	 */
	hws_user_wr(dev, HWS_BR_INT_ENABLE, 0);
	core_probe_reset(dev);
	ret = hws_user_set_geometry(dev, 1920, 1080);
	if (ret < 0)
		goto fail;
	core_run(dev);
	usleep(50 * 1000);	/* let the input registers settle */
	return 0;

fail:
	hws_user_close(dev);
	return ret;
}

void hws_user_close(struct hws_user_dev *dev)
{
	if (dev->vfio_open && dev->bar0)
		core_shutdown(dev);
	if (dev->vfio_open) {
		/* Unmaps the arena from the IOMMU and closes every descriptor. */
		hws_vfio_cleanup(&dev->vfio);
		dev->vfio_open = false;
	}
	dev->arena_mapped = false;
	if (dev->arena) {
		munmap(dev->arena, dev->arena_size);
		dev->arena = NULL;
	}
	dev->bar0 = NULL;
}

void hws_user_link_string(const struct hws_user_dev *dev, char *buf, size_t len)
{
	static const char *const widths[] = { "x1", "x2", "x4", "x8" };
	uint32_t v = hws_user_rd(dev, HWS_BR_LINK_STATUS);

	snprintf(buf, len, "%s GT/s %s", (v & 1) ? "5" : "2.5", widths[(v >> 1) & 3]);
}

bool hws_user_read_input(struct hws_user_dev *dev, struct hws_user_input *in)
{
	uint32_t sig = hws_user_rd(dev, HWS_REG_SIGNAL);
	uint32_t size, fps, hdcp;

	memset(in, 0, sizeof(*in));
	if (sig == HWS_STATUS_GONE)
		return false;
	in->present = sig & 1;
	in->interlaced = (sig >> 8) & 1;
	if (!in->present)
		return false;
	size = hws_user_rd(dev, HWS_REG_INPUT_SIZE);
	in->width = size & 0xFFFF;
	in->height = size >> 16;
	hdcp = hws_user_rd(dev, HWS_REG_HDCP);
	in->hdcp = hdcp != HWS_STATUS_GONE && (hdcp & 1);
	if (dev->fpga_version >= 122) {
		fps = hws_user_rd(dev, HWS_REG_INPUT_FPS);
		in->fps = (fps >= 1 && fps <= 300) ? fps : 0;
	}
	return !in->interlaced && in->width && in->height &&
	       in->width <= HWS_MAX_WIDTH && in->height <= HWS_MAX_HEIGHT &&
	       !(in->width & 1);
}

int hws_user_set_geometry(struct hws_user_dev *dev, unsigned int w, unsigned int h)
{
	uint32_t len[4], off[4], max = 0;

	if (hws_user_quarter_layout(w, h, len, off) < 0)
		return -EINVAL;
	if (off[3] + len[3] + HWS_DMA_BLOCK > dev->slot_stride)
		return -E2BIG;
	memcpy(dev->qlen, len, sizeof(len));
	memcpy(dev->qoff, off, sizeof(off));
	dev->width = w;
	dev->height = h;
	dev->frame_bytes = (size_t)w * h * 2;
	for (int q = 0; q < 4; q++) {
		hws_user_wr(dev, HWS_REG_QUARTER_LEN(q), len[q] / 16);
		if (len[q] > max)
			max = len[q];
	}
	(void)hws_user_rd(dev, HWS_REG_QUARTER_LEN(3));
	/* Cap a stale-length run at the largest quarter. */
	hws_user_wr(dev, HWS_REG_DMA_MAX, max / 16);
	return 0;
}

/* Where the card's write of quarter q ends: the frame's last word for q = 3. */
static inline uint32_t *sentinel_ptr(const struct hws_user_dev *dev, unsigned int q,
				     unsigned int slot)
{
	size_t end = (q == 3) ? dev->frame_bytes : (size_t)dev->qoff[q] + dev->qlen[q];

	return (uint32_t *)(hws_user_slot(dev, slot) + end - 4);
}

void hws_user_point_pair(struct hws_user_dev *dev, unsigned int pair, unsigned int slot)
{
	unsigned int q0 = pair * 2;

	for (unsigned int q = q0; q < q0 + 2; q++) {
		*(volatile uint32_t *)sentinel_ptr(dev, q, slot) = HWS_USER_SENTINEL;
		hws_user_wr(dev, HWS_REG_QUARTER_ADDR(q),
			    card_addr(slot_iova(dev, slot) + dev->qoff[q], q));
		dev->quarter_slot[q] = slot;
	}
	/* Flush the posted writes before the next quarter can start. */
	(void)hws_user_rd(dev, HWS_REG_QUARTER_ADDR(q0 + 1));
}

bool hws_user_pair_written(const struct hws_user_dev *dev, unsigned int pair, unsigned int slot)
{
	unsigned int q0 = pair * 2;

	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return *(volatile uint32_t *)sentinel_ptr(dev, q0, slot) != HWS_USER_SENTINEL &&
	       *(volatile uint32_t *)sentinel_ptr(dev, q0 + 1, slot) != HWS_USER_SENTINEL;
}

static void set_enable_bit(struct hws_user_dev *dev, uint32_t reg, bool on)
{
	uint32_t v = hws_user_rd(dev, reg);

	v = on ? (v | 1u) : (v & ~1u);
	hws_user_wr(dev, reg, v);
	/* Once the cleared bit reads back, the card has finished its burst. */
	(void)hws_user_rd(dev, reg);
}

/*
 * Program the address translation and the audio ring, enable interrupts and
 * run the capture core.  The input registers (size, rate) are only current
 * while the core runs, so this happens at open, not at capture start.
 */
static void core_run(struct hws_user_dev *dev)
{
	hws_user_wr(dev, HWS_REG_STATUS, HWS_CTL_RESET);

	/* Every window translates onto the one host range the arena lives in. */
	for (int i = 0; i < HWS_XLAT_ENTRIES; i++) {
		hws_user_wr(dev, HWS_BR_XLAT_HI(i), (uint32_t)(HWS_USER_IOVA_BASE >> 32));
		hws_user_wr(dev, HWS_BR_XLAT_LO(i),
			    (uint32_t)(HWS_USER_IOVA_BASE & ~HWS_WINDOW_MASK));
		(void)hws_user_rd(dev, HWS_BR_XLAT_LO(i));
	}
	hws_user_point_pair(dev, 0, 0);
	hws_user_point_pair(dev, 1, 0);
	hws_user_set_geometry(dev, dev->width, dev->height);
	hws_user_wr(dev, HWS_REG_AUDIO_ADDR, card_addr(dev->audio_iova, 0));
	(void)hws_user_rd(dev, HWS_REG_AUDIO_ADDR);

	hws_user_wr(dev, HWS_BR_INT_ENABLE, HWS_BR_INT_ENABLE_ALL);
	hws_user_wr(dev, HWS_REG_STATUS, HWS_CTL_START_A);
	usleep(500);
	hws_user_wr(dev, HWS_REG_STATUS, HWS_CTL_START_B);
	hws_user_wr(dev, HWS_REG_STATUS, HWS_CTL_RUN);
	dev->running = true;
}

int hws_user_start(struct hws_user_dev *dev, bool audio)
{
	uint32_t st;

	if (!dev->width)
		return -EINVAL;
	st = hws_user_rd(dev, HWS_REG_STATUS);
	if (st == HWS_STATUS_GONE)
		return -ENODEV;
	/* The core can lose its run state (resume, link retrain): bring it back. */
	if (!dev->running || !(st & HWS_STATUS_RUNNING)) {
		core_run(dev);
	} else {
		hws_user_point_pair(dev, 0, 0);
		hws_user_point_pair(dev, 1, 0);
		hws_user_set_geometry(dev, dev->width, dev->height);
	}

	dev->video_toggle = 0;	/* the first event after an enable is a top half */
	set_enable_bit(dev, HWS_REG_VIDEO_ENABLE, true);
	dev->video_on = true;
	if (audio && !dev->audio_on) {
		set_enable_bit(dev, HWS_REG_AUDIO_ENABLE, true);
		dev->audio_on = true;
	}
	return 0;
}

void hws_user_stop(struct hws_user_dev *dev)
{
	if (!dev->bar0)
		return;
	if (hws_user_rd(dev, HWS_REG_STATUS) == HWS_STATUS_GONE) {
		dev->running = dev->video_on = dev->audio_on = false;
		return;
	}
	if (dev->video_on) {
		set_enable_bit(dev, HWS_REG_VIDEO_ENABLE, false);
		usleep(200);	/* margin; the burst in flight ends before the read-back */
		dev->video_on = false;
	}
	if (dev->audio_on) {
		set_enable_bit(dev, HWS_REG_AUDIO_ENABLE, false);
		dev->audio_on = false;
	}
}

static void core_shutdown(struct hws_user_dev *dev)
{
	hws_user_stop(dev);
	if (!dev->bar0 || hws_user_rd(dev, HWS_REG_STATUS) == HWS_STATUS_GONE)
		return;
	hws_user_wr(dev, HWS_REG_STATUS, HWS_CTL_STOP);
	usleep(1000);
	hws_user_wr(dev, HWS_REG_VIDEO_ENABLE, 0);
	hws_user_wr(dev, HWS_REG_AUDIO_ENABLE, 0);
	hws_user_wr(dev, HWS_BR_INT_ENABLE, 0);
	dev->running = false;
}

void hws_user_service_irq(struct hws_user_dev *dev, struct hws_user_events *ev)
{
	const uint32_t known = HWS_INT_VIDEO0 | HWS_INT_AUDIO0;
	uint32_t st, is;

	memset(ev, 0, sizeof(*ev));
	st = hws_user_rd(dev, HWS_REG_STATUS);
	if (st == HWS_STATUS_GONE) {
		ev->gone = true;
		return;
	}
	if (!(st & HWS_STATUS_IRQ)) {
		ev->spurious = true;
		return;
	}
	is = hws_user_rd(dev, HWS_REG_INT_STATUS);
	if (!is) {
		ev->spurious = true;
		return;
	}
	for (int pass = 0; pass < 16 && is; pass++) {
		if (is & HWS_INT_VIDEO0) {
			int t = (int)(hws_user_rd(dev, HWS_REG_VIDEO_TOGGLE) & 1);

			if (t != dev->video_toggle && ev->nhalves < HWS_USER_MAX_EVENTS) {
				dev->video_toggle = t;
				ev->halves[ev->nhalves++] = (int8_t)t;
			}
		}
		if ((is & HWS_INT_AUDIO0) && ev->naudio < HWS_USER_MAX_EVENTS) {
			int t = (int)(hws_user_rd(dev, HWS_REG_AUDIO_TOGGLE) & 1);

			/* The toggle names the packet being filled; the other one is done. */
			ev->audio[ev->naudio++] = (int8_t)(t ? 0 : 1);
		}
		ev->unknown_bits |= is & ~known;
		hws_user_wr(dev, HWS_REG_INT_STATUS, is & HWS_INT_ACK_MASK);
		is = hws_user_rd(dev, HWS_REG_INT_STATUS);
	}
}

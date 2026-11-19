/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hws_user_hw.h — userspace VFIO hardware layer for the HWS UHD family
 * (ACASIS VS2583 / AVMatrix VC12-4K, PCI 8888:8581 and siblings).
 *
 * DMA model
 * ---------
 * The card writes a frame as four "quarters", quarter q through card-side
 * window q, which the bridge translates with entry q.  Each window is
 * 512 MiB, so every buffer has to sit inside one 512 MiB-aligned host bus
 * range.  Under VFIO the IOVA is ours to choose: all four translation entries
 * point at the same window (HWS_USER_IOVA_BASE) and the quarters of a frame
 * are laid out back to back in one frame slot.  A completed frame is
 * therefore a contiguous YUYV image, with no copy.
 *
 * The quarter address registers are latched per frame: a value written after
 * a quarter's half-done interrupt applies to that quarter in the next frame.
 * The engine rotates through @nslots frame slots by repointing quarter pair
 * 0/1 after the top-half interrupt and pair 2/3 after the bottom-half one.
 *
 * The HDMI audio ring (two 4 KiB packets) follows the frame slots in the same
 * window.
 */

#ifndef HWS_USER_HW_H
#define HWS_USER_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hws_vfio.h"
#include "hws_user_regs.h"

#define HWS_USER_IOVA_BASE	0x20000000ULL	/* 512 MiB-aligned window */
#define HWS_USER_MIN_SLOTS	2
#define HWS_USER_MAX_SLOTS	8
/* Past the frame: quarter 3 rounds up a block and a run can overshoot one. */
#define HWS_USER_SLOT_SLACK	(4 * HWS_DMA_BLOCK)

/* Written where a quarter's data ends; the card overwrites it when it writes the quarter. */
#define HWS_USER_SENTINEL	0x55AAAA55u

struct hws_user_input {
	bool present;
	bool interlaced;
	bool hdcp;
	unsigned int width;
	unsigned int height;	/* per field when interlaced */
	unsigned int fps;	/* whole fps from the card, 0 if unknown */
};

/* What one interrupt service pass found. */
#define HWS_USER_MAX_EVENTS	8
struct hws_user_events {
	/* Video half events in order: 1 = top half (quarters 0,1) done, 0 = bottom half (2,3). */
	int8_t halves[HWS_USER_MAX_EVENTS];
	unsigned int nhalves;
	/* Index (0/1) of each audio packet the card finished. */
	int8_t audio[HWS_USER_MAX_EVENTS];
	unsigned int naudio;
	uint32_t unknown_bits;
	bool gone;	/* register 0 read all ones */
	bool spurious;	/* eventfd fired but the card raised nothing */
};

struct hws_user_dev {
	struct hws_vfio_ctx vfio;
	volatile uint8_t *bar0;
	bool vfio_open;

	/* Identity */
	uint16_t device_id;
	uint32_t devinfo;
	unsigned int fpga_version;
	unsigned int fpga_subversion;
	unsigned int out_format;	/* 0 = YUYV */

	/* DMA arena: nslots frame slots, then the audio ring */
	uint8_t *arena;
	size_t arena_size;
	bool arena_mapped;
	unsigned int nslots;
	size_t slot_stride;
	uint8_t *audio;			/* 2 * HWS_AUDIO_PACKET_BYTES */
	uint64_t audio_iova;

	/* Current geometry */
	unsigned int width;
	unsigned int height;
	size_t frame_bytes;
	uint32_t qlen[4];
	uint32_t qoff[4];
	unsigned int quarter_slot[4];	/* slot each quarter register points at */

	/* Engine state */
	bool running;
	bool video_on;
	bool audio_on;
	int video_toggle;		/* last seen register 32 bit 0 */
};

/* Pure geometry: quarter lengths/offsets of a w x h YUYV frame.  Returns -1 if invalid. */
int hws_user_quarter_layout(unsigned int w, unsigned int h,
			    uint32_t len[4], uint32_t off[4]);

/* Size of one frame slot able to hold any supported input. */
size_t hws_user_slot_stride(void);

/*
 * Open the card through VFIO, map BAR0 and the DMA arena, set up MSI and
 * run the capture core (no capture engine enabled yet), so the input
 * registers are live.  @bdf may be NULL to pick the first "hwsuhd" card
 * from the core registry.  @nslots is clamped to [MIN, MAX].
 */
int hws_user_open(struct hws_user_dev *dev, const char *bdf, unsigned int nslots);

/* Stop the engine, disable interrupts and release every VFIO resource. */
void hws_user_close(struct hws_user_dev *dev);

static inline uint32_t hws_user_rd(const struct hws_user_dev *dev, uint32_t off)
{
	return *(const volatile uint32_t *)(dev->bar0 + off);
}

static inline void hws_user_wr(struct hws_user_dev *dev, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(dev->bar0 + off) = val;
}

/* PCIe link as negotiated by the card's bridge, e.g. "5 GT/s x4". */
void hws_user_link_string(const struct hws_user_dev *dev, char *buf, size_t len);

/* Read the input state.  Returns true when a usable progressive signal is present. */
bool hws_user_read_input(struct hws_user_dev *dev, struct hws_user_input *in);

/* Program quarter lengths and the DMA run cap for a w x h input. */
int hws_user_set_geometry(struct hws_user_dev *dev, unsigned int w, unsigned int h);

/* Frame slot base address. */
static inline uint8_t *hws_user_slot(const struct hws_user_dev *dev, unsigned int slot)
{
	return dev->arena + (size_t)slot * dev->slot_stride;
}

/* Point quarter pair @pair (0 = quarters 0,1; 1 = quarters 2,3) at @slot, arming its sentinels. */
void hws_user_point_pair(struct hws_user_dev *dev, unsigned int pair, unsigned int slot);

/* True when the card overwrote both sentinels of @pair in @slot. */
bool hws_user_pair_written(const struct hws_user_dev *dev, unsigned int pair, unsigned int slot);

/* Point both quarter pairs at slot 0 and enable video (and audio when @audio). */
int hws_user_start(struct hws_user_dev *dev, bool audio);

/* Disable the video and audio engines; the core keeps running. Safe when stopped. */
void hws_user_stop(struct hws_user_dev *dev);

/* Read status, collect events and acknowledge them.  Call after the eventfd fires. */
void hws_user_service_irq(struct hws_user_dev *dev, struct hws_user_events *ev);

/* Finished audio packet @idx (0/1). */
static inline const int16_t *hws_user_audio_packet(const struct hws_user_dev *dev, int idx)
{
	return (const int16_t *)(dev->audio + (size_t)(idx & 1) * HWS_AUDIO_PACKET_BYTES);
}

#endif /* HWS_USER_HW_H */

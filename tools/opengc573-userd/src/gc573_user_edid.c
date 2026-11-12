/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_edid.c — Userspace EDID programming for IT6805 HDMI Receiver
 */

#include "gc573_user_edid.h"
#include "gc573_user_i2c.h"
#include "gc573_user_hw.h"
#include "gc573_user_it6805.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define IT6805_I2C_ADDR			0x48
#define IT6805_EDID_I2C_ADDR		0x54

#define IT6805_BANK_0			0x00
#define IT6805_BANK_CAOF_PORT1_CTRL	0x04

/* Standard GC573 256-byte EDID (1440p120, 4K60, 1080p240, HDR10, 8CH LPCM) */
static const u8 g_gc573_default_edid[GC573_EDID_SIZE] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x06, 0xd8, 0x36, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x1c, 0x01, 0x03, 0x80, 0xa0, 0x5a, 0x78,
	0xea, 0x08, 0xa5, 0xa2, 0x57, 0x4f, 0xa2, 0x28, 0x0f, 0x50, 0x54, 0x25,
	0x0b, 0x00, 0xd1, 0xc0, 0x81, 0x40, 0x81, 0x80, 0x81, 0x00, 0x8b, 0xc0,
	0x95, 0x00, 0xb3, 0x00, 0x3b, 0x80, 0x08, 0xe8, 0x00, 0x30, 0xf2, 0x70,
	0x5a, 0x80, 0xb0, 0x58, 0x8a, 0x00, 0x6d, 0x55, 0x21, 0x00, 0x00, 0x1e,
	0x0c, 0xdf, 0x80, 0xa0, 0x70, 0x38, 0x40, 0x40, 0x30, 0x40, 0x35, 0x00,
	0x20, 0x2f, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x41,
	0x56, 0x54, 0x20, 0x47, 0x43, 0x35, 0x37, 0x33, 0x0a, 0x20, 0x20, 0x20,
	0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0xf0, 0x1e, 0xde, 0x3c, 0x00, 0x0a,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x11, 0x02, 0x03, 0x44, 0xf1,
	0x59, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x10, 0x1f, 0x5a, 0x3f, 0x05, 0x14,
	0x04, 0x13, 0x12, 0x11, 0x03, 0x02, 0x01, 0x22, 0x21, 0x20, 0x16, 0x15,
	0x07, 0x06, 0x23, 0x0f, 0x07, 0x07, 0x83, 0x4f, 0x00, 0x00, 0x67, 0x03,
	0x0c, 0x00, 0x10, 0x00, 0x38, 0x3c, 0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78,
	0x80, 0x03, 0xe2, 0x00, 0xcf, 0xe3, 0x05, 0xc0, 0x00, 0xe2, 0x0f, 0x03,
	0xe3, 0x06, 0x05, 0x01, 0x6f, 0xc2, 0x00, 0xa0, 0xa0, 0xa0, 0x55, 0x50,
	0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1e, 0x9e, 0xe8,
	0x00, 0x78, 0xa0, 0xa0, 0x67, 0x50, 0x08, 0x20, 0x98, 0x04, 0x55, 0x50,
	0x21, 0x00, 0x00, 0x1e, 0x56, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50,
	0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x18, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x06,
};

/*
 * 1440p120 profile: the default table without 4K.  The 4K60 detailed timing
 * and all 4K VICs (93..97) were removed and the 1440p120 DTD promoted to the
 * preferred timing slot, so a console that only offers its UI in 4K falls back
 * to 1440p120 instead of negotiating 4K.
 */
static const u8 g_gc573_edid_1440p120[GC573_EDID_SIZE] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x06, 0xd8, 0x36, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x1c, 0x01, 0x03, 0x80, 0xa0, 0x5a, 0x78,
	0xea, 0x08, 0xa5, 0xa2, 0x57, 0x4f, 0xa2, 0x28, 0x0f, 0x50, 0x54, 0x25,
	0x0b, 0x00, 0xd1, 0xc0, 0x81, 0x40, 0x81, 0x80, 0x81, 0x00, 0x8b, 0xc0,
	0x95, 0x00, 0xb3, 0x00, 0x3b, 0x80, 0x6f, 0xc2, 0x00, 0xa0, 0xa0, 0xa0,
	0x55, 0x50, 0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1e,
	0x0c, 0xdf, 0x80, 0xa0, 0x70, 0x38, 0x40, 0x40, 0x30, 0x40, 0x35, 0x00,
	0x20, 0x2f, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x41,
	0x56, 0x54, 0x20, 0x47, 0x43, 0x35, 0x37, 0x33, 0x0a, 0x20, 0x20, 0x20,
	0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0xf0, 0x1e, 0xde, 0x3c, 0x00, 0x0a,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0xe1, 0x02, 0x03, 0x3c, 0xf0,
	0x54, 0x10, 0x1f, 0x5a, 0x3f, 0x05, 0x14, 0x04, 0x13, 0x12, 0x11, 0x03,
	0x02, 0x01, 0x22, 0x21, 0x20, 0x16, 0x15, 0x07, 0x06, 0x23, 0x0f, 0x07,
	0x07, 0x83, 0x4f, 0x00, 0x00, 0x67, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x38,
	0x3c, 0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78, 0x80, 0x03, 0xe2, 0x00, 0xcf,
	0xe3, 0x05, 0xc0, 0x00, 0xe3, 0x06, 0x05, 0x01, 0x9e, 0xe8, 0x00, 0x78,
	0xa0, 0xa0, 0x67, 0x50, 0x08, 0x20, 0x98, 0x04, 0x55, 0x50, 0x21, 0x00,
	0x00, 0x1e, 0x56, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50, 0x30, 0x20,
	0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x02,
};

/*
 * 1080p240 profile: 4K and 1440p timings removed, 1080p240 promoted to the
 * preferred slot.  VIC 90 (2560x1080) is dropped as well so the advertised
 * maximum is a true 1080p mode.
 */
static const u8 g_gc573_edid_1080p240[GC573_EDID_SIZE] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x06, 0xd8, 0x36, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x1c, 0x01, 0x03, 0x80, 0xa0, 0x5a, 0x78,
	0xea, 0x08, 0xa5, 0xa2, 0x57, 0x4f, 0xa2, 0x28, 0x0f, 0x50, 0x54, 0x25,
	0x0b, 0x00, 0xd1, 0xc0, 0x81, 0x40, 0x81, 0x80, 0x81, 0x00, 0x8b, 0xc0,
	0x95, 0x00, 0xb3, 0x00, 0x3b, 0x80, 0x0c, 0xdf, 0x80, 0xa0, 0x70, 0x38,
	0x40, 0x40, 0x30, 0x40, 0x35, 0x00, 0x20, 0x2f, 0x21, 0x00, 0x00, 0x1e,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x41,
	0x56, 0x54, 0x20, 0x47, 0x43, 0x35, 0x37, 0x33, 0x0a, 0x20, 0x20, 0x20,
	0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0xf0, 0x1e, 0xde, 0x3c, 0x00, 0x0a,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0xf0, 0x02, 0x03, 0x3b, 0xf0,
	0x53, 0x10, 0x1f, 0x3f, 0x05, 0x14, 0x04, 0x13, 0x12, 0x11, 0x03, 0x02,
	0x01, 0x22, 0x21, 0x20, 0x16, 0x15, 0x07, 0x06, 0x23, 0x0f, 0x07, 0x07,
	0x83, 0x4f, 0x00, 0x00, 0x67, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x38, 0x3c,
	0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78, 0x80, 0x03, 0xe2, 0x00, 0xcf, 0xe3,
	0x05, 0xc0, 0x00, 0xe3, 0x06, 0x05, 0x01, 0xfc, 0x7e, 0x80, 0x88, 0x70,
	0x38, 0x12, 0x40, 0x18, 0x20, 0x35, 0x00, 0x20, 0x2f, 0x21, 0x00, 0x00,
	0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xf4,
};

/* Active profile; resolved (and materialised) on first use. */
static const u8 *g_active_edid;
static const char *g_active_edid_name = GC573_EDID_PROFILE_DEFAULT;
static u8 g_custom_edid[GC573_EDID_SIZE];
static bool g_edid_programmed;
/* --raw-edid: load custom files bit-exact instead of unifying their audio. */
static bool g_edid_raw_custom;

static int edid_validate(const u8 *edid)
{
	static const u8 header[8] = {
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
	};

	if (memcmp(edid, header, sizeof(header)) != 0) {
		pr_err("EDID: invalid base block header");
		return -EINVAL;
	}
	if (edid[126] == 0 || edid[128] != 0x02) {
		pr_err("EDID: missing CEA-861 extension block");
		return -EINVAL;
	}
	for (unsigned int block = 0; block < 2; block++) {
		u8 sum = 0;

		for (unsigned int i = 0; i < 128; i++)
			sum += edid[block * 128 + i];
		if (sum) {
			pr_err("EDID: block %u checksum mismatch", block);
			return -EINVAL;
		}
	}
	return 0;
}

/*
 * Every built-in profile advertises L-PCM (8 ch) plus Dolby Digital (AC-3)
 * and DTS 5.1 in the CTA-861 Audio Data Block.  A console may therefore emit
 * decoded LPCM or IEC 61937 bursts at any time without a manual profile
 * switch; gc573_user_streamer decides per block what the carrier holds.
 */
static const u8 g_gc573_edid_audio_sads[] = {
	0x0f, 0x07, 0x07,	/* LPCM, 8 ch, 32/44.1/48 kHz, 24-bit */
	0x15, 0x07, 0x50,	/* AC-3, 6 ch, 32/44.1/48 kHz, 640 kbit/s */
	0x3d, 0x06, 0xc0,	/* DTS, 6 ch, 44.1/48 kHz, 1536 kbit/s */
};

static u8 g_gc573_default_edid_unified[GC573_EDID_SIZE];
static u8 g_gc573_edid_1440p120_unified[GC573_EDID_SIZE];
static u8 g_gc573_edid_1080p240_unified[GC573_EDID_SIZE];
static bool g_gc573_default_edid_ready;
static bool g_gc573_edid_1440p120_ready;
static bool g_gc573_edid_1080p240_ready;

#define EDID_CTA_OFFSET			128U
#define EDID_CTA_DATA_OFFSET		132U
#define EDID_CTA_CHECKSUM_OFFSET	255U
#define EDID_CTA_AUDIO_TAG		1U
#define EDID_CTA_EXTENDED_TAG		7U
#define EDID_CTA_EXT_VIDEO_CAPABILITY	0x00U
#define EDID_CTA_AUDIO_LEN		(sizeof(g_gc573_edid_audio_sads))
#define EDID_CTA_BLOCK_LEN(len)		(1U + (len))
#define EDID_DTD_SIZE			18U

/*
 * Rewrite the CTA-861 data block collection of @src into @dst with the
 * unified audio block; every other block keeps its order and content.
 *
 * The audio block grows by six bytes (two extra SADs), which shifts the
 * following data blocks and the detailed timings.  The default profile has
 * only five spare bytes, so when a DTD would no longer fit the optional Video
 * Capability Data Block (extended tag 0x00) is dropped to reclaim three bytes.
 * Should even that be insufficient, the trailing DTD is skipped.
 */
static int edid_build_unified_profile(const u8 *src, u8 *dst)
{
	unsigned int src_dtd_offset = src[EDID_CTA_OFFSET + 2];
	unsigned int data_end = EDID_CTA_OFFSET + src_dtd_offset;
	unsigned int new_audio_len = EDID_CTA_BLOCK_LEN(EDID_CTA_AUDIO_LEN);
	unsigned int audio_off = 0;
	unsigned int audio_len = 0;
	unsigned int vcdb_off = 0;
	unsigned int vcdb_len = 0;
	unsigned int num_dtds = 0;
	unsigned int base_len;
	unsigned int src_off;
	unsigned int dst_off;
	unsigned int dtd;
	unsigned int sum;
	bool drop_vcdb = false;

	if (src_dtd_offset < 4 || data_end > EDID_CTA_CHECKSUM_OFFSET)
		return -EINVAL;

	for (src_off = EDID_CTA_DATA_OFFSET; src_off < data_end; ) {
		unsigned int tag = src[src_off] >> 5;
		unsigned int len = src[src_off] & 0x1f;

		if (src_off + 1 + len > data_end)
			return -EINVAL;
		if (tag == EDID_CTA_AUDIO_TAG && !audio_off) {
			audio_off = src_off;
			audio_len = len;
		} else if (tag == EDID_CTA_EXTENDED_TAG && len >= 1 &&
			   src[src_off + 1] == EDID_CTA_EXT_VIDEO_CAPABILITY) {
			vcdb_off = src_off;
			vcdb_len = len;
		}
		src_off += 1 + len;
	}

	/*
	 * A richer audio block (E-AC-3, TrueHD, ...) is replaced by the unified
	 * one; that shrinks the collection and is always safe here.
	 */

	/* Count the detailed timings that follow the data block collection. */
	for (dtd = data_end; dtd + EDID_DTD_SIZE <= EDID_CTA_CHECKSUM_OFFSET;
	     dtd += EDID_DTD_SIZE) {
		if (!src[dtd] && !src[dtd + 1])
			break;
		num_dtds++;
	}

	/*
	 * Budget for the rebuilt collection: replace the existing audio block
	 * (usually +6 bytes) or append a new one (10 bytes).  Custom EDIDs from
	 * CRU or scaler tools often carry no audio block at all, so the unified
	 * block is appended instead of rejecting the file.
	 */
	base_len = data_end - EDID_CTA_DATA_OFFSET;
	if (audio_off)
		base_len -= EDID_CTA_BLOCK_LEN(audio_len);
	base_len += new_audio_len;

	/*
	 * Keep every detailed timing: reclaim the optional Video Capability Data
	 * Block (extended tag 0x00) before giving up.  If the remainder still does
	 * not fit, the caller falls back to the unmodified EDID.
	 */
	if (EDID_CTA_DATA_OFFSET + base_len +
	    num_dtds * EDID_DTD_SIZE > EDID_CTA_CHECKSUM_OFFSET) {
		if (!vcdb_off)
			return -EINVAL;
		drop_vcdb = true;
		base_len -= EDID_CTA_BLOCK_LEN(vcdb_len);
	}
	if (EDID_CTA_DATA_OFFSET + base_len +
	    num_dtds * EDID_DTD_SIZE > EDID_CTA_CHECKSUM_OFFSET)
		return -EINVAL;

	memcpy(dst, src, GC573_EDID_SIZE);

	/* Rebuild the collection so the audio block can change length. */
	dst_off = EDID_CTA_DATA_OFFSET;
	for (src_off = EDID_CTA_DATA_OFFSET; src_off < data_end; ) {
		unsigned int len = src[src_off] & 0x1f;

		if (audio_off && src_off == audio_off) {
			dst[dst_off] = (u8)((EDID_CTA_AUDIO_TAG << 5) |
					    (EDID_CTA_AUDIO_LEN & 0x1f));
			memcpy(dst + dst_off + 1, g_gc573_edid_audio_sads,
			       EDID_CTA_AUDIO_LEN);
			dst_off += new_audio_len;
		} else if (drop_vcdb && src_off == vcdb_off) {
			/* Optional block dropped to keep every detailed timing. */
		} else {
			memcpy(dst + dst_off, src + src_off, 1 + len);
			dst_off += 1 + len;
		}
		src_off += 1 + len;
	}

	/* No audio block in the source: append the unified one. */
	if (!audio_off) {
		dst[dst_off] = (u8)((EDID_CTA_AUDIO_TAG << 5) |
				    (EDID_CTA_AUDIO_LEN & 0x1f));
		memcpy(dst + dst_off + 1, g_gc573_edid_audio_sads,
		       EDID_CTA_AUDIO_LEN);
		dst_off += new_audio_len;
	}

	if (dst_off != EDID_CTA_DATA_OFFSET + base_len ||
	    dst_off > EDID_CTA_CHECKSUM_OFFSET)
		return -EINVAL;

	dst[EDID_CTA_OFFSET + 2] = (u8)(dst_off - EDID_CTA_OFFSET);
	memset(dst + dst_off, 0, EDID_CTA_CHECKSUM_OFFSET - dst_off);

	for (dtd = data_end; dtd + EDID_DTD_SIZE <= EDID_CTA_CHECKSUM_OFFSET;
	     dtd += EDID_DTD_SIZE) {
		if (!src[dtd] && !src[dtd + 1])
			break;
		if (dst_off + EDID_DTD_SIZE > EDID_CTA_CHECKSUM_OFFSET)
			return -EINVAL;
		memcpy(dst + dst_off, src + dtd, EDID_DTD_SIZE);
		dst_off += EDID_DTD_SIZE;
	}

	sum = 0;
	for (unsigned int i = EDID_CTA_OFFSET; i < EDID_CTA_CHECKSUM_OFFSET; i++)
		sum += dst[i];
	dst[EDID_CTA_CHECKSUM_OFFSET] = (u8)(0x100U - (sum & 0xff));

	return edid_validate(dst);
}

static int edid_load_file(const char *path)
{
	u8 buffer[GC573_EDID_SIZE];
	u8 unified[GC573_EDID_SIZE];
	const u8 *selected = buffer;
	FILE *file;
	size_t bytes;
	int ret;

	file = fopen(path, "rb");
	if (!file) {
		pr_err("EDID: cannot open '%s': %d", path, errno);
		return -errno;
	}
	bytes = fread(buffer, 1, sizeof(buffer), file);
	fclose(file);
	if (bytes != sizeof(buffer)) {
		pr_err("EDID: '%s' has %zu bytes, expected %u", path, bytes,
		       GC573_EDID_SIZE);
		return -EINVAL;
	}

	ret = edid_validate(buffer);
	if (ret < 0)
		return ret;

	/*
	 * Custom EDIDs from CRU or scaler tools usually advertise L-PCM only, so
	 * a console greys out Dolby/DTS.  Rewrite the CTA audio block (or append
	 * one) unless the caller asked for bit-exact passthrough.
	 */
	if (g_edid_raw_custom) {
		printf("[EDID] Loading '%s' bit-exact (--raw-edid)\n", path);
	} else if (edid_build_unified_profile(buffer, unified) == 0) {
		selected = unified;
		printf("[EDID] Injected unified AC-3/DTS audio descriptors into custom EDID '%s'\n",
		       path);
	} else {
		printf("[EDID] Using raw audio block from '%s' (unification bypassed: no space for SADs)\n",
		       path);
	}

	if (memcmp(selected, g_custom_edid, sizeof(buffer)) != 0) {
		memcpy(g_custom_edid, selected, sizeof(buffer));
		g_edid_programmed = false;
	}
	g_active_edid = g_custom_edid;
	g_active_edid_name = path;
	return 0;
}

static int edid_use_profile(const u8 *table, const char *name)
{
	if (g_active_edid != table)
		g_edid_programmed = false;
	g_active_edid = table;
	g_active_edid_name = name;
	return 0;
}

static int edid_use_unified(const u8 *base, u8 *table, bool *built,
			    const char *name)
{
	int ret;

	if (!*built) {
		ret = edid_build_unified_profile(base, table);
		if (ret < 0) {
			pr_err("EDID: cannot build unified profile '%s'", name);
			return ret;
		}
		*built = true;
	}

	return edid_use_profile(table, name);
}

/* Resolve the active table, materialising the unified default on first use. */
static const u8 *edid_current(void)
{
	if (!g_active_edid) {
		if (edid_use_unified(g_gc573_default_edid,
				     g_gc573_default_edid_unified,
				     &g_gc573_default_edid_ready,
				     GC573_EDID_PROFILE_DEFAULT) < 0)
			return g_gc573_default_edid;
	}

	return g_active_edid;
}

int gc573_user_edid_select(const char *profile_name, const char *custom_path)
{
	const char *name = profile_name;

	if (custom_path && *custom_path)
		return edid_load_file(custom_path);
	if (!name || !*name)
		return edid_use_unified(g_gc573_default_edid,
					g_gc573_default_edid_unified,
					&g_gc573_default_edid_ready,
					GC573_EDID_PROFILE_DEFAULT);

	if (!strcasecmp(name, "default") || !strcasecmp(name, "auto") ||
	    !strcasecmp(name, "4k60"))
		return edid_use_unified(g_gc573_default_edid,
					g_gc573_default_edid_unified,
					&g_gc573_default_edid_ready,
					GC573_EDID_PROFILE_DEFAULT);
	if (!strcasecmp(name, GC573_EDID_PROFILE_1440P120) ||
	    !strcasecmp(name, "1440p"))
		return edid_use_unified(g_gc573_edid_1440p120,
					g_gc573_edid_1440p120_unified,
					&g_gc573_edid_1440p120_ready,
					GC573_EDID_PROFILE_1440P120);
	if (!strcasecmp(name, GC573_EDID_PROFILE_1080P240) ||
	    !strcasecmp(name, "1080p"))
		return edid_use_unified(g_gc573_edid_1080p240,
					g_gc573_edid_1080p240_unified,
					&g_gc573_edid_1080p240_ready,
					GC573_EDID_PROFILE_1080P240);
	if (!strcasecmp(name, GC573_EDID_PROFILE_BITSTREAM) ||
	    !strcasecmp(name, "ac3") || !strcasecmp(name, "dts")) {
		/*
		 * Backward compatibility: AC-3/DTS are advertised by every
		 * profile now, so the legacy alias selects the unified default.
		 */
		pr_info("EDID: '%s' is a legacy alias; using the unified default profile",
			name);
		return edid_use_unified(g_gc573_default_edid,
					g_gc573_default_edid_unified,
					&g_gc573_default_edid_ready,
					GC573_EDID_PROFILE_DEFAULT);
	}

	/* Anything that looks like a file is treated as a custom EDID. */
	if (strchr(name, '/') || strstr(name, ".bin") || strstr(name, ".edid"))
		return edid_load_file(name);

	pr_err("EDID: unknown profile '%s'", name);
	return -EINVAL;
}

const char *gc573_user_edid_active_name(void)
{
	return g_active_edid_name;
}

void gc573_user_edid_set_raw_custom(bool enable)
{
	g_edid_raw_custom = enable;
}

static inline int it6805_write_reg(void *bar0, u8 reg, u8 val)
{
	return gc573_user_i2c_write(bar0, GC573_I2C_BUS_PRIMARY, IT6805_I2C_ADDR,
				    reg, 1, &val, 1);
}

static inline int it6805_read_reg(void *bar0, u8 reg, u8 *val)
{
	return gc573_user_i2c_read(bar0, GC573_I2C_BUS_PRIMARY, IT6805_I2C_ADDR,
				   reg, 1, val, 1);
}

static inline int it6805_update_bits(void *bar0, u8 reg, u8 mask, u8 val)
{
	u8 cur = 0;
	int ret = it6805_read_reg(bar0, reg, &cur);
	if (ret < 0)
		return ret;
	u8 new_val = (cur & ~mask) | (val & mask);
	if (new_val == cur)
		return 0;
	return it6805_write_reg(bar0, reg, new_val);
}

static u8 it6805_find_hdmi_physical_address(const u8 *edid, size_t size)
{
	if (size != 256 || edid[128] != 0x02 ||
	    edid[129] != 0x03 || edid[130] <= 4)
		return 0;

	size_t data_end = 128 + edid[130];
	if (data_end > size)
		return 0;

	for (size_t offset = 132; offset < data_end; ) {
		u8 length = edid[offset] & 0x1f;
		size_t next = offset + length + 1;

		if (next > data_end)
			return 0;
		if ((edid[offset] >> 5) == 0x03 && length >= 5 &&
		    edid[offset + 1] == 0x03 &&
		    edid[offset + 2] == 0x0c &&
		    edid[offset + 3] == 0x00)
			return offset + 4;
		offset = next;
	}

	return 0;
}

static int it6805_write_edid_byte(void *bar0, unsigned int offset, u8 value)
{
	int ret = gc573_user_i2c_write(bar0, GC573_I2C_BUS_PRIMARY,
				       IT6805_EDID_I2C_ADDR, offset, 1,
				       &value, 1);
	if (ret < 0)
		pr_err("EDID write failed at offset %u: %d", offset, ret);
	return ret;
}

int gc573_user_program_edid(void *bar0)
{
	u8 offset;
	u8 block_checksum[2];
	u8 port_checksum[2];
	u8 sum;
	bool hpd_high = false;
	int ret;

	if (!bar0)
		return -EINVAL;
	if (g_edid_programmed)
		return 0;

	pr_info("Programming GC573 256-byte EDID profile '%s' into IT6805 DDC RAM...",
		g_active_edid_name);

	/* 1. Pull HPD low so the console acknowledges the EDID refresh. */
	gc573_user_set_gpio(bar0, GC573_GPIO_INPUT_HPD, false, 120);
	if (gc573_user_get_gpio(bar0, GC573_GPIO_INPUT_HPD, &hpd_high) == 0 &&
	    hpd_high)
		pr_warn("HPD did not go low before EDID programming!");

	/* 2. Configure DDC access. */
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0x4b, 0xa9);
	if (ret < 0)
		goto out;

	/* 3. Program Block 0 (bytes 0..126). */
	sum = 0;
	for (int i = 0; i < 127; i++) {
		u8 b = g_active_edid[i];

		ret = it6805_write_edid_byte(bar0, i, b);
		if (ret < 0)
			goto out;
		sum += b;
	}
	block_checksum[0] = -sum;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0xc9, block_checksum[0]);
	if (ret < 0)
		goto out;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT1_CTRL);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0xc9, block_checksum[0]);
	if (ret < 0)
		goto out;

	/* 4. Program Block 1 (bytes 128..254). */
	sum = 0;
	for (int i = 0; i < 127; i++) {
		u8 b = g_active_edid[128 + i];

		ret = it6805_write_edid_byte(bar0, 128 + i, b);
		if (ret < 0)
			goto out;
		sum += b;
	}
	block_checksum[1] = -sum;

	/* 5. Calculate HDMI VSDB physical address and per-port checksums. */
	offset = it6805_find_hdmi_physical_address(g_active_edid, 256);
	if (!offset) {
		pr_err("Failed to locate HDMI physical address in EDID!");
		ret = -EINVAL;
		goto out;
	}

	port_checksum[0] = block_checksum[1] +
			   g_active_edid[offset] +
			   g_active_edid[offset + 1] - 0x10;
	port_checksum[1] = block_checksum[1] +
			   g_active_edid[offset] +
			   g_active_edid[offset + 1] - 0x20;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0xc6, offset);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0xc7, 0x10);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0xc8, 0x00);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0xca, port_checksum[0]);
	if (ret < 0)
		goto out;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT1_CTRL);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0xc7, 0x20);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0xc8, 0x00);
	if (ret < 0)
		goto out;
	ret = it6805_write_reg(bar0, 0xca, port_checksum[1]);
	if (ret < 0)
		goto out;

	/* 6. Verify the EDID RAM contents before latching them. */
	unsigned int mismatches = 0;
	unsigned int first = 0;

	ret = gc573_user_verify_edid(bar0, &mismatches, &first);
	if (ret < 0) {
		pr_err("EDID verification failed: %d mismatches (first at %u)",
		       mismatches, first);
		goto out;
	}
	pr_info("EDID RAM verified: 256 bytes match the built-in table.");

	/* 7. Latch EDID DDC RAM in IT6805 hardware. */
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		goto out;
	ret = it6805_update_bits(bar0, 0xc5, 0x01, 0x00);
	if (ret < 0)
		goto out;
	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT1_CTRL);
	if (ret < 0)
		goto out;
	ret = it6805_update_bits(bar0, 0xc5, 0x01, 0x00);
	if (ret < 0)
		goto out;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		goto out;
	ret = it6805_update_bits(bar0, 0xc5, 0x10, 0x10);
	if (ret < 0)
		goto out;
	usleep(2000);
	ret = it6805_update_bits(bar0, 0xc5, 0x10, 0x00);
	if (ret < 0)
		goto out;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_CAOF_PORT1_CTRL);
	if (ret < 0)
		goto out;
	ret = it6805_update_bits(bar0, 0xc5, 0x10, 0x10);
	if (ret < 0)
		goto out;
	usleep(2000);
	ret = it6805_update_bits(bar0, 0xc5, 0x10, 0x00);
	if (ret < 0)
		goto out;

	ret = gc573_user_it6805_set_bank(bar0, IT6805_BANK_0);
	if (ret < 0)
		goto out;

	/* 8. Enable video input port 0. */
	ret = it6805_write_reg(bar0, 0x22, 0x10);
	if (ret < 0)
		goto out;

	pr_info("[✓] EDID programmed and latched (DDC checksums 0x%02x / 0x%02x, offset %u).",
		port_checksum[0], port_checksum[1], offset);

out:
	/*
	 * Always release HPD after programming.  The console polls DDC only
	 * while HPD is high, so a failed write must not leave it stuck low.
	 */
	gc573_user_set_gpio(bar0, GC573_GPIO_INPUT_HPD, true, 150);
	if (gc573_user_get_gpio(bar0, GC573_GPIO_INPUT_HPD, &hpd_high) == 0 &&
	    !hpd_high)
		pr_err("HPD readback is still low after EDID programming!");

	if (ret == 0)
		g_edid_programmed = true;

	return ret;
}

int gc573_user_program_edid_by_name(void *bar0, const char *profile_name,
				    const char *custom_path)
{
	int ret;

	ret = gc573_user_edid_select(profile_name, custom_path);
	if (ret < 0)
		return ret;

	return gc573_user_program_edid(bar0);
}

int gc573_user_read_edid(void *bar0, u8 *edid)
{
	if (!bar0 || !edid)
		return -EINVAL;

	for (unsigned int i = 0; i < GC573_EDID_SIZE; i++) {
		int ret = gc573_user_i2c_read(bar0, GC573_I2C_BUS_PRIMARY,
					      IT6805_EDID_I2C_ADDR, i, 1,
					      &edid[i], 1);
		if (ret < 0) {
			pr_err("EDID readback failed at offset %u: %d", i, ret);
			return ret;
		}
	}

	return 0;
}

int gc573_user_edid_compare(const u8 *edid, size_t size,
			    unsigned int *mismatches,
			    unsigned int *first_offset)
{
	const u8 *active = edid_current();
	unsigned int count = 0;
	unsigned int first = 0;

	if (!edid || size != GC573_EDID_SIZE)
		return -EINVAL;

	for (unsigned int i = 0; i < GC573_EDID_SIZE; i++) {
		/* The IT6805 generates the block checksums itself. */
		if ((i & 0x7f) == 0x7f)
			continue;
		if (edid[i] == active[i])
			continue;
		if (!count)
			first = i;
		count++;
	}

	if (mismatches)
		*mismatches = count;
	if (first_offset)
		*first_offset = first;

	return count ? -EIO : 0;
}

int gc573_user_verify_edid(void *bar0, unsigned int *mismatches,
			   unsigned int *first_offset)
{
	u8 edid[GC573_EDID_SIZE];
	int ret;

	ret = gc573_user_read_edid(bar0, edid);
	if (ret < 0)
		return ret;

	return gc573_user_edid_compare(edid, sizeof(edid), mismatches,
				       first_offset);
}

const u8 *gc573_user_edid_data(void)
{
	return edid_current();
}

bool gc573_user_edid_advertises_bitstream(void)
{
	const u8 *edid = edid_current();
	unsigned int dtd_offset = edid[EDID_CTA_OFFSET + 2];
	unsigned int data_end = EDID_CTA_OFFSET + dtd_offset;

	if (edid[EDID_CTA_OFFSET] != 0x02 || edid[EDID_CTA_OFFSET + 1] < 3 ||
	    dtd_offset < 4 || data_end > EDID_CTA_CHECKSUM_OFFSET)
		return false;

	for (unsigned int off = EDID_CTA_DATA_OFFSET; off < data_end; ) {
		unsigned int tag = edid[off] >> 5;
		unsigned int len = edid[off] & 0x1f;

		if (off + 1 + len > data_end)
			return false;
		if (tag != EDID_CTA_AUDIO_TAG) {
			off += 1 + len;
			continue;
		}

		/* Format code 1 is L-PCM; anything else is compressed audio. */
		for (unsigned int sad = 0; sad + 3 <= len; sad += 3) {
			if ((edid[off + 1 + sad] >> 3) != 0x01)
				return true;
		}
		return false;
	}

	return false;
}

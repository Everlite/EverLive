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
	0x21, 0x00, 0x00, 0x1e, 0xfc, 0x7e, 0x80, 0x88, 0x70, 0x38, 0x12, 0x40,
	0x18, 0x20, 0x35, 0x00, 0x20, 0x2f, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xff,
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
	0x00, 0x1e, 0xfc, 0x7e, 0x80, 0x88, 0x70, 0x38, 0x12, 0x40, 0x18, 0x20,
	0x35, 0x00, 0x20, 0x2f, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xfb,
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

/* Active profile; the built-in default is used until one is selected. */
static const u8 *g_active_edid = g_gc573_default_edid;
static const char *g_active_edid_name = GC573_EDID_PROFILE_DEFAULT;
static u8 g_custom_edid[GC573_EDID_SIZE];
static bool g_edid_programmed;

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
 * Bitstream profile: the default video/colour blocks plus AC-3 and DTS short
 * audio descriptors in the CTA-861 Audio Data Block.  The audio block grows
 * by two SADs (6 bytes), so the remaining data blocks shift and the CTA DTD
 * offset moves with them.  The trailing 1080p144 detailed timing no longer
 * fits in the 128-byte extension and is dropped; 4K60, 1440p120, 1440p144 and
 * all CTA VICs (including 1080p120) stay advertised.
 */
static const u8 g_gc573_edid_bitstream_sads[] = {
	0x0f, 0x07, 0x07,	/* LPCM, 8 ch, 32/44.1/48 kHz, 24-bit */
	0x15, 0x07, 0x50,	/* AC-3, 6 ch, 32/44.1/48 kHz, 640 kbit/s */
	0x3d, 0x06, 0xc0,	/* DTS, 6 ch, 44.1/48 kHz, 1536 kbit/s */
};

static u8 g_gc573_edid_bitstream[GC573_EDID_SIZE];
static bool g_gc573_edid_bitstream_built;

#define EDID_CTA_OFFSET		128U
#define EDID_CTA_DATA_OFFSET	132U
#define EDID_CTA_CHECKSUM_OFFSET	255U
#define EDID_CTA_AUDIO_TAG	1U
#define EDID_CTA_AUDIO_LEN	(sizeof(g_gc573_edid_bitstream_sads))
#define EDID_CTA_BLOCK_LEN(len)	(1U + (len))
#define EDID_DTD_SIZE		18U

static int edid_build_bitstream_profile(void)
{
	const u8 *src = g_gc573_default_edid;
	u8 *dst = g_gc573_edid_bitstream;
	unsigned int dtd_offset;
	unsigned int data_end;
	unsigned int audio_off = 0;
	unsigned int audio_len = 0;
	unsigned int audio_end;
	unsigned int new_block_len;
	unsigned int delta;
	unsigned int src_off;
	unsigned int dst_off;
	unsigned int sum = 0;

	if (g_gc573_edid_bitstream_built)
		return 0;

	memcpy(dst, src, GC573_EDID_SIZE);

	dtd_offset = src[EDID_CTA_OFFSET + 2];
	data_end = EDID_CTA_OFFSET + dtd_offset;
	if (dtd_offset < 4 || data_end > EDID_CTA_CHECKSUM_OFFSET)
		return -EINVAL;

	/* Locate the Audio Data Block (tag 1) in the data block collection. */
	for (unsigned int off = EDID_CTA_DATA_OFFSET; off < data_end;) {
		unsigned int tag = src[off] >> 5;
		unsigned int len = src[off] & 0x1f;

		if (off + 1 + len > data_end)
			return -EINVAL;
		if (tag == EDID_CTA_AUDIO_TAG) {
			audio_off = off;
			audio_len = len;
			break;
		}
		off += 1 + len;
	}
	if (!audio_off || audio_len > EDID_CTA_AUDIO_LEN)
		return -EINVAL;

	new_block_len = EDID_CTA_BLOCK_LEN(EDID_CTA_AUDIO_LEN);
	delta = new_block_len - EDID_CTA_BLOCK_LEN(audio_len);
	if (data_end + delta > EDID_CTA_CHECKSUM_OFFSET)
		return -EINVAL;

	audio_end = audio_off + 1 + audio_len;

	/* Keep the data blocks before the audio block in place. */
	memcpy(dst + EDID_CTA_DATA_OFFSET, src + EDID_CTA_DATA_OFFSET,
	       audio_off - EDID_CTA_DATA_OFFSET);

	/* Write the enlarged Audio Data Block. */
	dst[audio_off] = (u8)((EDID_CTA_AUDIO_TAG << 5) |
			      (EDID_CTA_AUDIO_LEN & 0x1f));
	memcpy(dst + audio_off + 1, g_gc573_edid_bitstream_sads,
	       EDID_CTA_AUDIO_LEN);

	/* Move the remaining data blocks behind the larger audio block. */
	memcpy(dst + audio_off + new_block_len, src + audio_end,
	       data_end - audio_end);

	/* New DTD offset, then clear the DTD area. */
	dst[EDID_CTA_OFFSET + 2] = (u8)(dtd_offset + delta);
	memset(dst + data_end + delta, 0,
	       EDID_CTA_CHECKSUM_OFFSET - (data_end + delta));

	/* Copy as many detailed timings as still fit before the checksum. */
	src_off = data_end;
	dst_off = data_end + delta;
	while (src_off + EDID_DTD_SIZE <= EDID_CTA_CHECKSUM_OFFSET &&
	       dst_off + EDID_DTD_SIZE <= EDID_CTA_CHECKSUM_OFFSET &&
	       (src[src_off] || src[src_off + 1])) {
		memcpy(dst + dst_off, src + src_off, EDID_DTD_SIZE);
		src_off += EDID_DTD_SIZE;
		dst_off += EDID_DTD_SIZE;
	}

	for (unsigned int i = EDID_CTA_OFFSET;
	     i < EDID_CTA_CHECKSUM_OFFSET; i++)
		sum += dst[i];
	dst[EDID_CTA_CHECKSUM_OFFSET] = (u8)(0x100U - (sum & 0xff));

	if (edid_validate(dst) < 0)
		return -EINVAL;

	g_gc573_edid_bitstream_built = true;
	return 0;
}

static int edid_load_file(const char *path)
{
	u8 buffer[GC573_EDID_SIZE];
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

	if (memcmp(buffer, g_custom_edid, sizeof(buffer)) != 0) {
		memcpy(g_custom_edid, buffer, sizeof(buffer));
		g_edid_programmed = false;
	}
	g_active_edid = g_custom_edid;
	g_active_edid_name = path;
	pr_info("EDID: loaded custom profile '%s'", path);
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

int gc573_user_edid_select(const char *profile_name, const char *custom_path)
{
	const char *name = profile_name;

	if (custom_path && *custom_path)
		return edid_load_file(custom_path);
	if (!name || !*name)
		return edid_use_profile(g_gc573_default_edid,
					GC573_EDID_PROFILE_DEFAULT);

	if (!strcasecmp(name, "default") || !strcasecmp(name, "auto") ||
	    !strcasecmp(name, "4k60"))
		return edid_use_profile(g_gc573_default_edid,
					GC573_EDID_PROFILE_DEFAULT);
	if (!strcasecmp(name, GC573_EDID_PROFILE_1440P120) ||
	    !strcasecmp(name, "1440p"))
		return edid_use_profile(g_gc573_edid_1440p120,
					GC573_EDID_PROFILE_1440P120);
	if (!strcasecmp(name, GC573_EDID_PROFILE_1080P240) ||
	    !strcasecmp(name, "1080p"))
		return edid_use_profile(g_gc573_edid_1080p240,
					GC573_EDID_PROFILE_1080P240);
	if (!strcasecmp(name, GC573_EDID_PROFILE_BITSTREAM) ||
	    !strcasecmp(name, "ac3") || !strcasecmp(name, "dts")) {
		int ret = edid_build_bitstream_profile();

		if (ret < 0) {
			pr_err("EDID: cannot build bitstream profile");
			return ret;
		}
		return edid_use_profile(g_gc573_edid_bitstream,
					GC573_EDID_PROFILE_BITSTREAM);
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
	unsigned int count = 0;
	unsigned int first = 0;

	if (!edid || size != GC573_EDID_SIZE)
		return -EINVAL;

	for (unsigned int i = 0; i < GC573_EDID_SIZE; i++) {
		/* The IT6805 generates the block checksums itself. */
		if ((i & 0x7f) == 0x7f)
			continue;
		if (edid[i] == g_active_edid[i])
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
	return g_active_edid;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * IT68051/IT6802 HDMI receiver initialization, signal detection, EDID handling,
 * color conversion, and receiver debug controls.
 */
#include "gc570d.h"

int gc570d_receiver_read8(struct gc570d_dev *gc,
				 const struct gc570d_i2c_bus *bus,
				 u8 address, u8 *value, u32 *last_irq,
				 u32 *last_status)
{
	return gc570d_i2c_read8(gc, bus, GC570D_I2C_RECEIVER_ADDR8,
				address, value, last_irq, last_status);
}

int gc570d_receiver_write8(struct gc570d_dev *gc,
				  const struct gc570d_i2c_bus *bus,
				  u8 address, u8 value)
{
	return gc570d_i2c_write8(gc, bus, GC570D_I2C_RECEIVER_ADDR8,
				 address, value);
}

int gc570d_receiver_update8(struct gc570d_dev *gc,
				   const struct gc570d_i2c_bus *bus,
				   u8 address, u8 mask, u8 value)
{
	u32 last_irq;
	u32 last_status;
	u8 old_value;
	int ret;

	ret = gc570d_receiver_read8(gc, bus, address, &old_value,
				    &last_irq, &last_status);
	if (ret)
		return ret;

	return gc570d_receiver_write8(gc, bus, address,
				      (old_value & ~mask) | (value & mask));
}

struct gc570d_receiver_init_entry {
	u8 address;
	u8 mask;
	u8 value;
};

/* Base IT6802 table at AVXGC570D_x64.sys 0x14014c740. */
static const struct gc570d_receiver_init_entry gc570d_it6802_init_table[] = {
	{ 0x0f, 0x03, 0x00 }, { 0x10, 0xff, 0x08 },
	{ 0x0f, 0x03, 0x00 }, { 0x34, 0xff, 0xe1 },
	{ 0x10, 0xff, 0x17 }, { 0x11, 0xff, 0x1f },
	{ 0x18, 0xff, 0x1f }, { 0x12, 0xff, 0xf8 },
	{ 0x10, 0xff, 0x10 }, { 0x11, 0xff, 0xa0 },
	{ 0x18, 0xff, 0xa0 }, { 0x12, 0xff, 0x00 },
	{ 0x0f, 0x03, 0x01 }, { 0xb0, 0x03, 0x00 },
	{ 0x0f, 0x03, 0x00 }, { 0x17, 0xc0, 0x80 },
	{ 0x1e, 0xc0, 0x00 }, { 0x16, 0x08, 0x08 },
	{ 0x1d, 0x08, 0x08 }, { 0x2b, 0xff, 0x07 },
	{ 0x31, 0xff, 0x09 }, { 0x49, 0xff, 0x09 },
	{ 0x35, 0x1e, 0x14 }, { 0x4b, 0x1e, 0x14 },
	{ 0x54, 0xff, 0x11 }, { 0x6a, 0xff, 0x81 },
	{ 0x74, 0xff, 0xa0 }, { 0x50, 0x1f, 0x12 },
	{ 0x65, 0x0c, 0x00 }, { 0x7a, 0x80, 0x80 },
	{ 0x85, 0x02, 0x02 }, { 0xc0, 0x43, 0x40 },
	{ 0x87, 0xff, 0xa9 }, { 0x71, 0x08, 0x00 },
	{ 0x37, 0xff, 0xa6 }, { 0x4d, 0xff, 0xa6 },
	{ 0x67, 0x80, 0x00 }, { 0x7a, 0x50, 0x50 },
	{ 0x77, 0x80, 0x00 }, { 0x0f, 0x03, 0x01 },
	{ 0xc0, 0x8c, 0x08 }, { 0x0f, 0x03, 0x00 },
	{ 0x7e, 0x40, 0x40 }, { 0x52, 0x20, 0x20 },
	{ 0x53, 0xcf, 0x4f }, { 0x58, 0xff, 0x33 },
	{ 0x59, 0xff, 0xaa }, { 0x25, 0xff, 0x1f },
	{ 0x3d, 0xff, 0x1f }, { 0x27, 0xff, 0x1f },
	{ 0x28, 0xff, 0x1f }, { 0x29, 0xff, 0x1f },
	{ 0x3f, 0xff, 0x1f }, { 0x40, 0xff, 0x1f },
	{ 0x41, 0xff, 0x1f }, { 0x0f, 0x03, 0x01 },
	{ 0xbc, 0xff, 0x06 }, { 0xcc, 0xff, 0x00 },
	{ 0xc6, 0x07, 0x03 }, { 0xb5, 0x03, 0x03 },
	{ 0xb8, 0x80, 0x00 }, { 0xb6, 0x07, 0x03 },
	{ 0x10, 0xff, 0x00 }, { 0x11, 0xff, 0x00 },
	{ 0x12, 0xff, 0x00 }, { 0x13, 0xff, 0x00 },
	{ 0x28, 0xff, 0x00 }, { 0x29, 0xff, 0x00 },
	{ 0x2a, 0xff, 0x00 }, { 0x2b, 0xff, 0x00 },
	{ 0x2c, 0xff, 0x00 }, { 0x0f, 0x03, 0x00 },
	{ 0x22, 0xff, 0x00 }, { 0x3a, 0xff, 0x00 },
	{ 0x26, 0xff, 0x00 }, { 0x3e, 0xff, 0x00 },
	{ 0x63, 0xff, 0x3f }, { 0x73, 0x08, 0x00 },
	{ 0x60, 0x40, 0x00 }, { 0x2a, 0x01, 0x00 },
	{ 0x42, 0x01, 0x00 }, { 0x77, 0x0c, 0x08 },
};

/*
 * CVDecITE68051 base table at AVXGC570D_x64.sys 0x14014cd00.
 * It contains 116 (register, mask, value) triples followed by 0xff.
 */
static const u8 gc570d_it68051_init_table[] = {
	0x0f, 0xff, 0x00, 0x22, 0xff, 0x08, 0x22, 0xff, 0x17, 0x23, 0xff, 0x1f, 0x2b, 0xff, 0x1f, 0x24,
	0xff, 0xf8, 0x22, 0xff, 0x10, 0x23, 0xff, 0xa0, 0x2b, 0xff, 0xa0, 0x24, 0xff, 0x00, 0x34, 0xff,
	0x00, 0x0f, 0xff, 0x03, 0xaa, 0xff, 0xec, 0x0f, 0xff, 0x00, 0x0f, 0xff, 0x03, 0xac, 0xff, 0x40,
	0x0f, 0xff, 0x00, 0x3a, 0xff, 0x89, 0x49, 0xff, 0xe1, 0x43, 0xff, 0x01, 0x0f, 0xff, 0x04, 0x43,
	0xff, 0x01, 0x3a, 0xff, 0x89, 0x0f, 0xff, 0x03, 0xa8, 0xff, 0x0b, 0x0f, 0xff, 0x00, 0x4f, 0xff,
	0x84, 0x44, 0xff, 0x19, 0x46, 0xff, 0x15, 0x47, 0xff, 0x88, 0xd9, 0xff, 0x00, 0xf0, 0xff, 0x78,
	0xf1, 0xff, 0x10, 0x0f, 0xff, 0x03, 0x3a, 0xff, 0x02, 0x0f, 0xff, 0x00, 0x28, 0xff, 0x88, 0x6e,
	0xff, 0x80, 0x77, 0xff, 0x87, 0x7b, 0xff, 0x00, 0x86, 0xff, 0x00, 0x0f, 0xff, 0x00, 0x36, 0xff,
	0x06, 0x8f, 0xff, 0x41, 0x0f, 0xff, 0x01, 0xc0, 0xff, 0x42, 0xc4, 0x70, 0x00, 0xc4, 0x80, 0x00,
	0xc5, 0xff, 0x00, 0xc6, 0xff, 0x00, 0xc7, 0xff, 0x00, 0xc8, 0xff, 0x00, 0xc9, 0xff, 0x99, 0xca,
	0xff, 0x99, 0x0f, 0xff, 0x00, 0x86, 0x0c, 0x08, 0x81, 0x80, 0x80, 0x0f, 0x07, 0x01, 0x10, 0xff,
	0x00, 0x11, 0xff, 0x00, 0x12, 0xff, 0x00, 0x13, 0xff, 0x00, 0x28, 0xff, 0x00, 0x29, 0xff, 0x00,
	0x2a, 0xff, 0x00, 0x2b, 0xff, 0x00, 0x2c, 0xff, 0x00, 0xc0, 0xc0, 0x40, 0x0f, 0x07, 0x03, 0xe3,
	0xff, 0x07, 0x27, 0xff, 0x9f, 0x28, 0xff, 0x9f, 0x29, 0xff, 0x9f, 0xa7, 0x40, 0x40, 0x0f, 0x07,
	0x07, 0xe3, 0xff, 0x07, 0x27, 0xff, 0x9f, 0x28, 0xff, 0x9f, 0x29, 0xff, 0x9f, 0xa7, 0x40, 0x40,
	0x0f, 0x07, 0x00, 0xf8, 0xff, 0xc3, 0xf8, 0xff, 0xa5, 0x0f, 0x07, 0x01, 0x5f, 0xff, 0x04, 0x58,
	0xff, 0x12, 0x58, 0xff, 0x02, 0x5f, 0xff, 0x00, 0x0f, 0x07, 0x00, 0xf8, 0xff, 0xff, 0x0f, 0x07,
	0x05, 0x20, 0x03, 0x01, 0x0f, 0x07, 0x00, 0x0f, 0x07, 0x04, 0x3c, 0x20, 0x00, 0x0f, 0x07, 0x00,
	0x91, 0x40, 0x40, 0x0f, 0x07, 0x03, 0xf0, 0xff, 0xc0, 0x0f, 0x07, 0x00, 0x21, 0x40, 0x40, 0xce,
	0x30, 0x00, 0x0f, 0x07, 0x04, 0xce, 0x30, 0x00, 0x42, 0xe0, 0xc0, 0x0f, 0x07, 0x00, 0x42, 0xe0,
	0xc0, 0x7b, 0x10, 0x10, 0x3c, 0x21, 0x00, 0x3b, 0xff, 0x23, 0xf6, 0xff, 0x08, 0x0f, 0x07, 0x04,
	0x3c, 0x21, 0x00, 0x3b, 0xff, 0x23, 0x0f, 0x07, 0x00, 0x59, 0xff, 0x00, 0xff,
};

/* Exact customer_edidGC570D_4k.bin payload bundled by AVerMedia. */
const u8 gc570d_it68051_edid[256] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
	0x06, 0xd8, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x08, 0x1e, 0x01, 0x03, 0x80, 0xa0, 0x5a, 0x78,
	0xea, 0x08, 0xa5, 0xa2, 0x57, 0x4f, 0xa2, 0x28,
	0x0f, 0x50, 0x54, 0x24, 0x0b, 0x00, 0xd1, 0xc0,
	0x3b, 0x80, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x08, 0xe8,
	0x00, 0x30, 0xf2, 0x70, 0x5a, 0x80, 0xb0, 0x58,
	0x8a, 0x00, 0x6d, 0x55, 0x21, 0x00, 0x00, 0x1e,
	0x0c, 0xdf, 0x80, 0xa0, 0x70, 0x38, 0x40, 0x40,
	0x30, 0x40, 0x35, 0x00, 0x20, 0x2f, 0x21, 0x00,
	0x00, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x41,
	0x56, 0x54, 0x20, 0x47, 0x43, 0x35, 0x37, 0x30,
	0x2d, 0x44, 0x0a, 0x20, 0x00, 0x00, 0x00, 0xfd,
	0x00, 0x32, 0xf0, 0x1e, 0xde, 0x3c, 0x00, 0x0a,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x9a,
	0x02, 0x03, 0x49, 0xf1, 0x58, 0x61, 0x60, 0x5f,
	0x5e, 0x5d, 0x10, 0x1f, 0x5a, 0x3f, 0x05, 0x14,
	0x04, 0x13, 0x11, 0x03, 0x02, 0x01, 0x22, 0x21,
	0x20, 0x16, 0x15, 0x07, 0x06, 0x23, 0x0f, 0x07,
	0x07, 0x83, 0x4f, 0x00, 0x00, 0x6d, 0x03, 0x0c,
	0x00, 0x10, 0x00, 0x38, 0x3c, 0x20, 0x00, 0x60,
	0x01, 0x02, 0x03, 0x67, 0xd8, 0x5d, 0xc4, 0x01,
	0x78, 0x80, 0x03, 0xe2, 0x00, 0xcf, 0xe3, 0x05,
	0xc0, 0x00, 0xe2, 0x0f, 0x03, 0xe3, 0x06, 0x05,
	0x01, 0x6f, 0xc2, 0x00, 0xa0, 0xa0, 0xa0, 0x55,
	0x50, 0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21,
	0x00, 0x00, 0x1e, 0x9e, 0xe8, 0x00, 0x78, 0xa0,
	0xa0, 0x67, 0x50, 0x08, 0x20, 0x98, 0x04, 0x55,
	0x50, 0x21, 0x00, 0x00, 0x1e, 0xfc, 0x7e, 0x80,
	0x88, 0x70, 0x38, 0x12, 0x40, 0x18, 0x20, 0x35,
	0x00, 0x20, 0x2f, 0x21, 0x00, 0x00, 0x1e, 0x81,
};

/* Exact customer_edidGC570D_fhd.bin payload bundled by AVerMedia. */
static const u8 gc570d_it6802_edid[] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
	0x06, 0xd8, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x08, 0x1e, 0x01, 0x03, 0x80, 0xa0, 0x5a, 0x78,
	0xea, 0x76, 0x90, 0xa8, 0x54, 0x4d, 0x9f, 0x25,
	0x0e, 0x50, 0x54, 0x20, 0x08, 0x00, 0xd1, 0xc0,
	0x3b, 0x80, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a,
	0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
	0x45, 0x00, 0xc4, 0x8e, 0x21, 0x00, 0x00, 0x1e,
	0x01, 0x1d, 0x00, 0x72, 0x51, 0xd0, 0x1e, 0x20,
	0x6e, 0x28, 0x55, 0x00, 0xc4, 0x8e, 0x21, 0x00,
	0x00, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x41,
	0x56, 0x54, 0x20, 0x47, 0x43, 0x35, 0x37, 0x30,
	0x2d, 0x44, 0x0a, 0x20, 0x00, 0x00, 0x00, 0xfd,
	0x00, 0x18, 0x3d, 0x0f, 0x4b, 0x11, 0x00, 0x0a,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x8e,
	0x02, 0x03, 0x29, 0xf1, 0x52, 0x10, 0x05, 0x04,
	0x03, 0x02, 0x20, 0x11, 0x01, 0x1f, 0x12, 0x13,
	0x14, 0x21, 0x22, 0x06, 0x15, 0x07, 0x16, 0x23,
	0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0x66,
	0x03, 0x0c, 0x00, 0x20, 0x00, 0x00, 0xe2, 0x00,
	0x4f, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d,
	0x40, 0x58, 0x2c, 0x45, 0x00, 0xc4, 0x8e, 0x21,
	0x00, 0x00, 0x1c, 0x02, 0x3a, 0x80, 0x18, 0x71,
	0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0xc4,
	0x8e, 0x21, 0x00, 0x00, 0x1e, 0x02, 0x3a, 0x80,
	0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45,
	0x00, 0xc4, 0x8e, 0x21, 0x00, 0x00, 0x1e, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e,
};

/* IT6802 RGB-to-YUV coefficients used by SetColorSpaceConvert(). */
static const u8 gc570d_csc_601_limited[] = {
	0x00, 0x80, 0x00, 0xb2, 0x04, 0x65, 0x02,
	0xe9, 0x00, 0x93, 0x3c, 0x18, 0x04, 0x55,
	0x3f, 0x49, 0x3d, 0x9f, 0x3e, 0x18, 0x04,
};

static const u8 gc570d_csc_601_full[] = {
	0x10, 0x80, 0x00, 0x09, 0x04, 0x0e, 0x02,
	0xc9, 0x00, 0x0f, 0x3d, 0x84, 0x03, 0x6d,
	0x3f, 0xab, 0x3d, 0xd1, 0x3e, 0x84, 0x03,
};

static const u8 gc570d_csc_709_limited[] = {
	0x00, 0x80, 0x00, 0xb8, 0x05, 0xb4, 0x01,
	0x94, 0x00, 0x4a, 0x3c, 0x17, 0x04, 0x9f,
	0x3f, 0xd9, 0x3c, 0x10, 0x3f, 0x17, 0x04,
};

static const u8 gc570d_csc_709_full[] = {
	0x10, 0x80, 0x00, 0xe4, 0x04, 0x77, 0x01,
	0x7f, 0x00, 0xd0, 0x3c, 0x83, 0x03, 0xad,
	0x3f, 0x4b, 0x3d, 0x32, 0x3f, 0x84, 0x03,
};

static int gc570d_it6802_edid_verify(struct gc570d_dev *gc, size_t *bad_byte)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	u32 last_irq;
	u32 last_status;
	u8 value;
	size_t block;
	size_t i;
	int ret;

	for (block = 0; block < 2; block++) {
		for (i = 0; i < 127; i++) {
			size_t address = block * 128 + i;

			ret = gc570d_i2c_read8(gc, bus,
					       GC570D_I2C_EDID_ADDR8, address,
					       &value, &last_irq, &last_status);
			if (ret || value != gc570d_it6802_edid[address]) {
				*bad_byte = address;
				return ret ? ret : -EILSEQ;
			}
		}
	}

	return 0;
}

int gc570d_it6802_program_edid(struct gc570d_dev *gc)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	size_t bad_byte = 0;
	size_t block;
	size_t i;
	int ret;

	/* IT6802 stores 127 payload bytes per block; checksums are registers. */
	for (block = 0; block < 2; block++) {
		for (i = 0; i < 127; i++) {
			size_t address = block * 128 + i;

			ret = gc570d_i2c_write8(gc, bus,
						GC570D_I2C_EDID_ADDR8, address,
						gc570d_it6802_edid[address]);
			if (ret) {
				dev_err(&gc->pdev->dev,
					"IT6802 EDID write failed at byte %zu: %d\n",
					address, ret);
				return ret;
			}
		}
	}

	/* EDID RAM patch/checksum registers used by IT6802 EDIDRAMInitial(). */
	ret = gc570d_receiver_write8(gc, bus, 0xc1, 0xa3);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0xc2, 0x20);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0xc3, 0x00);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0xc4, gc570d_it6802_edid[127]);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0xc5, gc570d_it6802_edid[255]);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0xc6, 0xa3);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0xc7, 0x30);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0xc8, gc570d_it6802_edid[127]);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0xc9,
				    gc570d_it6802_edid[255] - 0x10);
	if (ret)
		return ret;

	ret = gc570d_it6802_edid_verify(gc, &bad_byte);
	if (ret) {
		dev_err(&gc->pdev->dev,
			"IT6802 EDID verification failed at byte %zu: %d\n",
			bad_byte, ret);
		return ret;
	}

	dev_info(&gc->pdev->dev,
		 "IT6802 AVerMedia FHD EDID programmed and verified, port0 checksums=0x%02x/0x%02x\n",
		 gc570d_it6802_edid[127], gc570d_it6802_edid[255]);
	return 0;
}

static ssize_t gc570d_it6802_edid_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it6802_program_edid(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it6802_edid_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it6802_edid_write,
	.llseek = noop_llseek,
};

int gc570d_it6802_link_status(struct gc570d_dev *gc, bool *source_5v,
				      bool *scdt)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	u32 last_irq;
	u32 last_status;
	u8 reg0a;
	int ret;

	ret = gc570d_receiver_read8(gc, bus, 0x0a, &reg0a,
				    &last_irq, &last_status);
	if (ret)
		return ret;

	*source_5v = reg0a & BIT(0);
	*scdt = reg0a & BIT(7);
	return 0;
}

static int gc570d_it6802_set_hpd(struct gc570d_dev *gc, bool high)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	u32 last_irq;
	u32 last_status;
	u8 reg0a;
	u8 hpd = 0;
	int restore_ret;
	int ret;

	ret = gc570d_receiver_read8(gc, bus, 0x0a, &reg0a,
				    &last_irq, &last_status);
	if (ret)
		return ret;

	/* Without source 5 V, the official driver leaves the HPD output tri-stated. */
	if (reg0a & BIT(0))
		hpd = high ? 0x03 : 0x01;

	ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x03, 0x01);
	if (ret)
		return ret;
	ret = gc570d_receiver_update8(gc, bus, 0xb0, 0x03, hpd);
	restore_ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x03, 0x00);
	return ret ? ret : restore_ret;
}

int gc570d_it6802_pulse_hpd(struct gc570d_dev *gc)
{
	int ret;

	ret = gc570d_it6802_set_hpd(gc, false);
	if (ret) {
		dev_err(&gc->pdev->dev,
			"IT6802 failed to drive HPD low: %d\n", ret);
		return ret;
	}
	msleep(300);
	ret = gc570d_it6802_set_hpd(gc, true);
	if (ret) {
		dev_err(&gc->pdev->dev,
			"IT6802 failed to drive HPD high: %d\n", ret);
		return ret;
	}

	dev_info(&gc->pdev->dev, "IT6802 HPD low/high pulse completed\n");
	return 0;
}

static ssize_t gc570d_it6802_hpd_write(struct file *file,
				       const char __user *buffer,
				       size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it6802_pulse_hpd(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it6802_hpd_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it6802_hpd_write,
	.llseek = noop_llseek,
};

static int gc570d_it6802_edid_status_show(struct seq_file *s, void *unused)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	struct gc570d_dev *gc = s->private;
	size_t bad_byte = 0;
	u8 c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0;
	u8 c6 = 0, c7 = 0, c8 = 0, c9 = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	int ret;

	mutex_lock(&gc->capture_lock);

	ret = gc570d_it6802_edid_verify(gc, &bad_byte);
	if (ret)
		goto out_unlock;
	ret = gc570d_receiver_read8(gc, bus, 0xc1, &c1,
				    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc2, &c2,
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc3, &c3,
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc4, &c4,
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc5, &c5,
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc6, &c6,
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc7, &c7,
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc8, &c8,
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc9, &c9,
					    &last_irq, &last_status);

out_unlock:
	mutex_unlock(&gc->capture_lock);

	seq_printf(s,
		   "verified=%s bad_byte=%zu c1=%02x c2=%02x c3=%02x c4=%02x c5=%02x c6=%02x c7=%02x c8=%02x c9=%02x error=%d\n",
		   ret ? "no" : "yes", bad_byte,
		   c1, c2, c3, c4, c5, c6, c7, c8, c9, ret);
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_it6802_edid_status);

static int gc570d_it6802_video_format_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	static const u8 registers[] = {
		0x0a, 0x51, 0x53, 0x64, 0x65, 0x6a, 0x84, 0x90, 0x99,
		0x9a, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3,
		0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
	};
	u8 values[ARRAY_SIZE(registers)];
	u32 last_irq = 0;
	u32 last_status = 0;
	u16 hactive;
	u16 htotal;
	u16 vactive;
	u16 vtotal;
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = gc570d_receiver_read8(gc, bus, registers[i], &values[i],
					   &last_irq, &last_status);
		if (ret) {
			seq_printf(s,
				   "error=%d register=0x%02x irq=0x%08x status=0x%08x\n",
				   ret, registers[i], last_irq, last_status);
			return 0;
		}
	}

	htotal = ((values[11] & 0x3f) << 8) | values[10];
	hactive = ((values[13] & 0x3f) << 8) | values[12];
	vtotal = ((values[18] & 0x0f) << 8) | values[17];
	vactive = ((values[18] & 0xf0) << 4) | values[19];

	seq_printf(s,
		   "scdt=%s port=%u hactive=%u htotal=%u vactive=%u vtotal=%u interlaced=%s pclk_divider=0x%02x\n",
		   values[0] & BIT(7) ? "yes" : "no", values[1] & 0x01,
		   hactive, htotal, vactive, vtotal,
		   values[8] & BIT(1) ? "yes" : "no", values[9]);
	seq_printf(s,
		   "output 53=%02x 64=%02x 65=%02x 6a=%02x 84=%02x\n",
		   values[2], values[3], values[4], values[5], values[6]);
	seq_printf(s,
		   "raw 0a=%02x 51=%02x 90=%02x 99=%02x 9a=%02x 9c=%02x 9d=%02x 9e=%02x 9f=%02x a0=%02x a1=%02x a2=%02x a3=%02x a4=%02x a5=%02x a6=%02x a7=%02x a8=%02x\n",
		   values[0], values[1], values[7], values[8], values[9],
		   values[10], values[11], values[12], values[13], values[14],
		   values[15], values[16], values[17], values[18], values[19],
		   values[20], values[21], values[22]);
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_it6802_video_format);

static unsigned int gc570d_it6802_audio_rate(u8 code)
{
	switch (code) {
	case 0x0:
		return 44100;
	case 0x2:
		return 48000;
	case 0x3:
		return 32000;
	case 0x4:
		return 88200;
	case 0x6:
		return 24000;
	case 0xa:
		return 96000;
	case 0xc:
		return 176400;
	case 0xe:
		return 192000;
	default:
		return 0;
	}
}

static int gc570d_it6802_audio_status_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	u32 last_irq = 0;
	u32 last_status = 0;
	u8 reg0a = 0;
	u8 regaa = 0;
	u8 regab = 0;
	u8 regae = 0;
	u8 reg10 = 0;
	u8 reg52 = 0;
	u8 reg54 = 0;
	u8 reg7d = 0;
	u8 rate_code;
	unsigned int rate;
	int ret;

	ret = gc570d_receiver_read8(gc, bus, 0x0a, &reg0a,
				    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xaa, &regaa,
					   &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xab, &regab,
					   &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xae, &regae,
					   &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0x10, &reg10,
					   &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0x52, &reg52,
					   &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0x54, &reg54,
					   &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0x7d, &reg7d,
					   &last_irq, &last_status);
	if (ret) {
		seq_printf(s, "error=%d irq=0x%08x status=0x%08x\n",
			   ret, last_irq, last_status);
		return 0;
	}

	rate_code = regae & 0x0f;
	rate = gc570d_it6802_audio_rate(rate_code);
	seq_printf(s,
		   "scdt=%s audio_valid=%s lpcm=%s hbr=%s dsd=%s channels_code=%u rate_code=0x%x rate_hz=%u\n",
		   reg0a & BIT(7) ? "yes" : "no",
		   regaa & BIT(7) ? "yes" : "no",
		   (!(regaa & (BIT(6) | BIT(5))) && !(regab & BIT(0))) ?
			"yes" : "no",
		   regaa & BIT(6) ? "yes" : "no",
		   regaa & BIT(5) ? "yes" : "no",
		   regaa & 0x0f, rate_code, rate);
	seq_printf(s,
		   "raw 0a=%02x aa=%02x ab=%02x ae=%02x 10=%02x 52=%02x 54=%02x 7d=%02x\n",
		   reg0a, regaa, regab, regae, reg10, reg52, reg54, reg7d);
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_it6802_audio_status);

int gc570d_it6802_audio_output_set(struct gc570d_dev *gc, bool enable)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	u32 last_irq;
	u32 last_status;
	u8 regaa;
	u8 regab;
	u8 regae;
	u8 infoframe;
	int ret;
	int i;

	if (!enable) {
		ret = gc570d_receiver_update8(gc, bus, 0x52, 0x1f, 0x1f);
		if (ret)
			return ret;
		dev_info(&gc->pdev->dev, "IT6802 audio output disabled\n");
		return 0;
	}

	ret = gc570d_receiver_read8(gc, bus, 0xaa, &regaa,
				    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xab, &regab,
					   &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xae, &regae,
					   &last_irq, &last_status);
	if (ret)
		return ret;
	if (!(regaa & BIT(7))) {
		dev_dbg(&gc->pdev->dev,
			"IT6802 audio output requires valid audio\n");
		return -ENODATA;
	}
	if ((regaa & (BIT(6) | BIT(5))) || (regab & BIT(0))) {
		dev_dbg(&gc->pdev->dev,
			"IT6802 audio output currently supports LPCM only\n");
		return -EOPNOTSUPP;
	}
	if (!gc570d_it6802_audio_rate(regae & 0x0f)) {
		dev_dbg(&gc->pdev->dev,
			"IT6802 reported an unknown audio rate code\n");
		return -EINVAL;
	}

	/* RequestAudio and WaitForReady transitions from the official driver. */
	ret = gc570d_receiver_update8(gc, bus, 0x52, 0x1f, 0x1f);
	if (!ret)
		ret = gc570d_receiver_update8(gc, bus, 0x54, 0x70, 0x10);
	if (!ret)
		ret = gc570d_receiver_update8(gc, bus, 0x7d, 0x10, 0x00);
	if (!ret)
		ret = gc570d_receiver_update8(gc, bus, 0x7d, 0x20, 0x20);
	if (!ret)
		ret = gc570d_receiver_update8(gc, bus, 0x10, BIT(1), BIT(1));
	if (!ret)
		ret = gc570d_receiver_update8(gc, bus, 0x10, BIT(1), 0x00);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0x7b, &infoframe,
					   &last_irq, &last_status);
	for (i = 0; !ret && i < 4; i++)
		ret = gc570d_receiver_write8(gc, bus, 0x7b, infoframe);
	if (!ret)
		ret = gc570d_receiver_update8(gc, bus, 0x7d, 0x10, 0x10);
	if (!ret)
		ret = gc570d_receiver_update8(gc, bus, 0x7d, 0x10, 0x00);
	if (ret)
		goto mute;

	msleep(210);
	ret = gc570d_receiver_update8(gc, bus, 0x52, 0x1f, 0x00);
	if (ret)
		goto mute;

	dev_info(&gc->pdev->dev,
		 "IT6802 LPCM audio output enabled, channels_code=%u rate=%u\n",
		 regaa & 0x0f, gc570d_it6802_audio_rate(regae & 0x0f));
	return 0;

mute:
	gc570d_receiver_update8(gc, bus, 0x52, 0x1f, 0x1f);
	return ret;
}

static ssize_t gc570d_it6802_audio_output_enable_write(
	struct file *file, const char __user *buffer, size_t count,
	loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &enable);
	if (ret)
		return ret;
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it6802_audio_output_set(gc, enable);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it6802_audio_output_enable_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it6802_audio_output_enable_write,
	.llseek = noop_llseek,
};

static int gc570d_it6802_read_page(struct gc570d_dev *gc, u8 page,
				    u8 address, u8 *value)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	u32 last_irq;
	u32 last_status;
	int restore_ret;
	int ret;

	ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x03, page);
	if (ret)
		return ret;
	ret = gc570d_receiver_read8(gc, bus, address, value,
				   &last_irq, &last_status);
	restore_ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x03, 0x00);
	return ret ? ret : restore_ret;
}

/*
 * The IT6802 does not expose a directly measured frame rate.  Its AVI VIC
 * identifies the CEA mode, which is sufficient for the 50/60 Hz reporting
 * distinction this driver needs.  Zero means "unknown"; callers then fall
 * back to the configured capture interval instead of inventing a cadence.
 */
static u16 gc570d_vic_frame_rate(u8 vic)
{
	switch (vic) {
	case 1 ... 16:
	case 35 ... 36:
		return 60;
	case 17 ... 31:
	case 37 ... 39:
		return 50;
	case 32:
	case 60:
		return 24;
	case 33:
	case 61:
		return 25;
	case 34:
	case 62:
		return 30;
	case 40 ... 45:
	case 64:
		return 100;
	case 46 ... 51:
	case 63:
		return 120;
	case 52 ... 55:
		return 200;
	case 56 ... 59:
		return 240;
	default:
		return 0;
	}
}

int gc570d_it6802_frame_rate(struct gc570d_dev *gc, u16 *frame_rate)
{
	u8 vic;
	int ret;

	if (!frame_rate)
		return -EINVAL;
	ret = gc570d_it6802_read_page(gc, 2, 0x18, &vic);
	if (ret)
		return ret;
	*frame_rate = gc570d_vic_frame_rate(vic);
	return 0;
}

static int gc570d_it6802_write_csc(struct gc570d_dev *gc,
				    const u8 *matrix, size_t size)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	u32 last_irq;
	u32 last_status;
	u8 value;
	int restore_ret;
	size_t i;
	int ret;

	ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x03, 0x01);
	if (ret)
		return ret;
	for (i = 0; i < size; i++) {
		ret = gc570d_receiver_write8(gc, bus, 0x70 + i, matrix[i]);
		if (ret)
			break;
	}
	for (i = 0; !ret && i < size; i++) {
		ret = gc570d_receiver_read8(gc, bus, 0x70 + i, &value,
					   &last_irq, &last_status);
		if (!ret && value != matrix[i]) {
			dev_err(&gc->pdev->dev,
				"IT6802 CSC verify failed at 0x%02zx: expected=0x%02x actual=0x%02x\n",
				0x70 + i, matrix[i], value);
			ret = -EILSEQ;
		}
	}
	restore_ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x03, 0x00);
	return ret ? ret : restore_ret;
}

int gc570d_it6802_format_init(struct gc570d_dev *gc)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	const u8 *matrix = NULL;
	u8 avi_colorspace;
	u8 avi_colorimetry;
	u8 avi_quantization;
	u8 avi_vic;
	u8 input_mode;
	bool limited;
	bool bt709;
	int ret;

	ret = gc570d_it6802_read_page(gc, 2, 0x15, &avi_colorspace);
	if (ret)
		return ret;
	ret = gc570d_it6802_read_page(gc, 2, 0x16, &avi_colorimetry);
	if (ret)
		return ret;
	ret = gc570d_it6802_read_page(gc, 2, 0x17, &avi_quantization);
	if (ret)
		return ret;
	ret = gc570d_it6802_read_page(gc, 2, 0x18, &avi_vic);
	if (ret)
		return ret;

	input_mode = (avi_colorspace >> 5) & 0x03;
	if ((avi_colorimetry & 0xc0) == 0x80) {
		bt709 = true;
	} else if ((avi_colorimetry & 0xc0) == 0x40) {
		bt709 = false;
	} else {
		/* SetColorimetryByInfoFrame() fallback for an unspecified AVI C field. */
		if (avi_vic >= 60)
			bt709 = true;
		else
			bt709 = !(0x0fff3c787fe6ffceULL &
				  BIT_ULL(avi_vic));
	}
	limited = ((avi_quantization >> 2) & 0x03) == 1;
	gc->video_input_frame_rate = gc570d_vic_frame_rate(avi_vic);

	/* The official driver clears reg71[2] before selecting the physical output mode. */
	ret = gc570d_receiver_update8(gc, bus, 0x71, BIT(2), 0);
	if (ret)
		return ret;

	if (input_mode == 0) {
		if (bt709)
			matrix = limited ? gc570d_csc_709_limited :
					   gc570d_csc_709_full;
		else
			matrix = limited ? gc570d_csc_601_limited :
					   gc570d_csc_601_full;
		ret = gc570d_it6802_write_csc(gc, matrix,
					       ARRAY_SIZE(gc570d_csc_601_full));
		if (ret)
			return ret;
		ret = gc570d_receiver_update8(gc, bus, 0x65, 0x03, 0x02);
	} else {
		ret = gc570d_receiver_update8(gc, bus, 0x65, 0x03, 0x00);
	}
	if (ret)
		return ret;

	ret = gc570d_receiver_update8(gc, bus, 0x67, 0x07, 0x00);
	if (ret)
		return ret;
	ret = gc570d_receiver_update8(gc, bus, 0x51, 0x80, 0x00);
	if (ret)
		return ret;
	/* The official driver uses a 24-bit YUV444 bus here; VIP performs YUY2 packing. */
	ret = gc570d_receiver_update8(gc, bus, 0x65, 0x30, 0x20);
	if (ret)
		return ret;

	dev_info(&gc->pdev->dev,
		 "IT6802 output format initialized: input=%u vic=%u bt709=%u limited=%u output=YUV444\n",
		 input_mode, avi_vic, bt709, limited);
	return 0;
}

static ssize_t gc570d_it6802_format_init_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it6802_format_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it6802_format_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it6802_format_init_write,
	.llseek = noop_llseek,
};

int gc570d_it6802_output_enable(struct gc570d_dev *gc)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	u32 last_irq;
	u32 last_status;
	u8 link_status;
	u8 video_mode;
	u8 color_clock = 0;
	int ret;

	ret = gc570d_receiver_read8(gc, bus, 0x0a, &link_status,
					   &last_irq, &last_status);
	if (ret)
		return ret;
	if (!(link_status & BIT(7))) {
		dev_dbg(&gc->pdev->dev,
			"IT6802 output enable requires SCDT\n");
		return -ENOLINK;
	}

	ret = gc570d_receiver_read8(gc, bus, 0x99, &video_mode,
					   &last_irq, &last_status);
	if (ret)
		return ret;
	if ((video_mode & 0xf0) == 0x50)
		color_clock = 0x04;
	else if ((video_mode & 0xf0) == 0x60)
		color_clock = 0x08;

	/* Stable-signal transition in CVDecITE6802::SetState(10). */
	ret = gc570d_receiver_update8(gc, bus, 0x65, 0x0c, color_clock);
	if (ret)
		return ret;
	ret = gc570d_receiver_update8(gc, bus, 0x64, 0x80, 0x80);
	if (ret)
		return ret;
	ret = gc570d_receiver_update8(gc, bus, 0x64, 0x80, 0x00);
	if (ret)
		return ret;
	msleep(50);
	ret = gc570d_receiver_update8(gc, bus, 0x53, 0x0f, 0x00);
	if (ret)
		return ret;
	ret = gc570d_receiver_update8(gc, bus, 0x52, 0x1f, 0x1f);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0x84, 0x8f);
	if (ret)
		return ret;
	ret = gc570d_receiver_write8(gc, bus, 0x6a, 0x81);
	if (ret)
		return ret;

	dev_info(&gc->pdev->dev,
		 "IT6802 stable output transition completed, mode=0x%02x\n",
		 video_mode);
	return 0;
}

static ssize_t gc570d_it6802_output_enable_write(struct file *file,
						 const char __user *buffer,
						 size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it6802_output_enable(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it6802_output_enable_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it6802_output_enable_write,
	.llseek = noop_llseek,
};

int gc570d_it68051_apply_base_table(struct gc570d_dev *gc)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	size_t i;
	int ret;

	for (i = 0; i + 2 < ARRAY_SIZE(gc570d_it68051_init_table); i += 3) {
		u8 address = gc570d_it68051_init_table[i];
		u8 mask = gc570d_it68051_init_table[i + 1];
		u8 value = gc570d_it68051_init_table[i + 2];

		if (address == 0xff)
			break;
		ret = gc570d_receiver_update8(gc, bus, address, mask, value);
		if (ret) {
			dev_err(&gc->pdev->dev,
				"IT68051 base init failed at entry %zu register 0x%02x: %d\n",
				i / 3, address, ret);
			/* Best-effort page-0 recovery for later diagnostics. */
			gc570d_receiver_update8(gc, bus, 0x0f, 0x07, 0x00);
			return ret;
		}
	}

	ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x07, 0x00);
	if (!ret)
		dev_info(&gc->pdev->dev,
			 "IT68051 base table completed: %zu entries, page 0 restored\n",
			 i / 3);

	return ret;
}

static ssize_t gc570d_it68051_init_write(struct file *file,
					  const char __user *buffer,
					  size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it68051_apply_base_table(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it68051_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it68051_init_write,
	.llseek = noop_llseek,
};

int gc570d_it68051_calibrate(struct gc570d_dev *gc)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	u32 last_irq;
	u32 last_status;
	u8 reg08 = 0;
	u8 reg0d = 0;
	u8 port0_59 = 0;
	u8 port0_5a = 0;
	u8 port1_59 = 0;
	u8 port1_5a = 0;
	unsigned int poll;
	int restore_ret;
	int ret;

#define IT68051_UPDATE(_reg, _mask, _value) do { \
	ret = gc570d_receiver_update8(gc, bus, (_reg), (_mask), (_value)); \
	if (ret) \
		goto out_restore_page; \
} while (0)

	/* CVDecITE68051::Init() calls this exact two-port CAOF sequence. */
	IT68051_UPDATE(0x0f, 0x07, 0x03);
	IT68051_UPDATE(0x3a, 0x80, 0x00);
	IT68051_UPDATE(0xa0, 0x80, 0x80);
	IT68051_UPDATE(0xa1, 0x80, 0x80);
	IT68051_UPDATE(0xa2, 0x80, 0x80);
	IT68051_UPDATE(0xa4, 0x08, 0x08);
	IT68051_UPDATE(0x3b, 0xc0, 0x00);
	IT68051_UPDATE(0xa7, 0x10, 0x10);
	IT68051_UPDATE(0x48, 0x80, 0x80);
	IT68051_UPDATE(0x0f, 0x07, 0x00);
	IT68051_UPDATE(0x29, 0x01, 0x01);
	IT68051_UPDATE(0x2a, 0x41, 0x41);
	msleep(10);
	IT68051_UPDATE(0x2a, 0x40, 0x00);
	IT68051_UPDATE(0x24, 0x04, 0x04);
	IT68051_UPDATE(0x25, 0xff, 0x00);
	IT68051_UPDATE(0x26, 0xff, 0x00);
	IT68051_UPDATE(0x27, 0xff, 0x00);
	IT68051_UPDATE(0x28, 0xff, 0x00);
	IT68051_UPDATE(0x3c, 0x10, 0x00);

	IT68051_UPDATE(0x0f, 0x07, 0x07);
	IT68051_UPDATE(0x3a, 0x80, 0x00);
	IT68051_UPDATE(0xa0, 0x80, 0x80);
	IT68051_UPDATE(0xa1, 0x80, 0x80);
	IT68051_UPDATE(0xa2, 0x80, 0x80);
	IT68051_UPDATE(0xa4, 0x08, 0x08);
	IT68051_UPDATE(0x3b, 0xc0, 0x00);
	IT68051_UPDATE(0xa7, 0x10, 0x10);
	IT68051_UPDATE(0x48, 0x80, 0x80);
	IT68051_UPDATE(0x0f, 0x07, 0x00);
	IT68051_UPDATE(0x32, 0x41, 0x41);
	msleep(10);
	IT68051_UPDATE(0x32, 0x40, 0x00);
	IT68051_UPDATE(0x2c, 0x04, 0x04);
	IT68051_UPDATE(0x2d, 0xff, 0x00);
	IT68051_UPDATE(0x2e, 0xff, 0x00);
	IT68051_UPDATE(0x2f, 0xff, 0x00);
	IT68051_UPDATE(0x30, 0xff, 0x00);
	IT68051_UPDATE(0x0f, 0x07, 0x04);
	IT68051_UPDATE(0x3c, 0x10, 0x00);
	IT68051_UPDATE(0x0f, 0x07, 0x03);
	IT68051_UPDATE(0x3a, 0x80, 0x80);
	IT68051_UPDATE(0x0f, 0x07, 0x07);
	IT68051_UPDATE(0x3a, 0x80, 0x80);
	IT68051_UPDATE(0x0f, 0x07, 0x00);

	for (poll = 0; poll < 36; poll++) {
		ret = gc570d_receiver_read8(gc, bus, 0x08, &reg08,
					    &last_irq, &last_status);
		if (ret)
			goto out_restore_page;
		ret = gc570d_receiver_read8(gc, bus, 0x0d, &reg0d,
					    &last_irq, &last_status);
		if (ret)
			goto out_restore_page;
		if ((reg08 & 0x30) && (reg0d & 0x30))
			break;
		if ((poll + 1) % 6 == 0) {
			if (!(reg08 & 0x30)) {
				IT68051_UPDATE(0x2a, 0x40, 0x40);
				msleep(10);
				IT68051_UPDATE(0x2a, 0x40, 0x00);
			}
			if (!(reg0d & 0x30)) {
				IT68051_UPDATE(0x32, 0x40, 0x40);
				msleep(10);
				IT68051_UPDATE(0x32, 0x40, 0x00);
			}
		}
		msleep(10);
	}

	if (!(reg08 & 0x30)) {
		IT68051_UPDATE(0x0f, 0x07, 0x03);
		IT68051_UPDATE(0x3a, 0x80, 0x00);
		IT68051_UPDATE(0x0f, 0x07, 0x00);
		IT68051_UPDATE(0x2a, 0x40, 0x40);
		msleep(10);
		IT68051_UPDATE(0x2a, 0x40, 0x00);
	}
	if (!(reg0d & 0x30)) {
		IT68051_UPDATE(0x0f, 0x07, 0x07);
		IT68051_UPDATE(0x3a, 0x80, 0x00);
		IT68051_UPDATE(0x0f, 0x07, 0x00);
		IT68051_UPDATE(0x32, 0x40, 0x40);
		msleep(10);
		IT68051_UPDATE(0x32, 0x40, 0x00);
	}

	IT68051_UPDATE(0x0f, 0x07, 0x03);
	ret = gc570d_receiver_read8(gc, bus, 0x5a, &port0_5a,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x59, &port0_59,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	IT68051_UPDATE(0x0f, 0x07, 0x07);
	ret = gc570d_receiver_read8(gc, bus, 0x5a, &port1_5a,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x59, &port1_59,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;

	IT68051_UPDATE(0x0f, 0x07, 0x00);
	IT68051_UPDATE(0x08, 0x30, 0x30);
	IT68051_UPDATE(0x0d, 0x30, 0x30);
	IT68051_UPDATE(0x29, 0x01, 0x00);
	IT68051_UPDATE(0x24, 0x04, 0x00);
	IT68051_UPDATE(0x3c, 0x10, 0x10);
	IT68051_UPDATE(0x2c, 0x04, 0x00);
	IT68051_UPDATE(0x0f, 0x07, 0x04);
	IT68051_UPDATE(0x3c, 0x10, 0x10);
	IT68051_UPDATE(0x0f, 0x07, 0x03);
	IT68051_UPDATE(0x3a, 0x80, 0x00);
	IT68051_UPDATE(0xa0, 0x80, 0x00);
	IT68051_UPDATE(0xa1, 0x80, 0x00);
	IT68051_UPDATE(0xa2, 0x80, 0x00);
	IT68051_UPDATE(0x0f, 0x07, 0x07);
	IT68051_UPDATE(0x3a, 0x80, 0x00);
	IT68051_UPDATE(0xa0, 0x80, 0x00);
	IT68051_UPDATE(0xa1, 0x80, 0x00);
	IT68051_UPDATE(0xa2, 0x80, 0x00);
	IT68051_UPDATE(0x0f, 0x07, 0x00);

	dev_info(&gc->pdev->dev,
		 "IT68051 CAOF completed: polls=%u reg08=0x%02x reg0d=0x%02x port0=%02x:%02x port1=%02x:%02x\n",
		 poll, reg08, reg0d, port0_5a, port0_59,
		 port1_5a, port1_59);
	ret = 0;

out_restore_page:
	restore_ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x07, 0x00);
	if (!ret)
		ret = restore_ret;
#undef IT68051_UPDATE
	return ret;
}

static ssize_t gc570d_it68051_calibrate_write(struct file *file,
					      const char __user *buffer,
					      size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it68051_calibrate(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it68051_calibrate_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it68051_calibrate_write,
	.llseek = noop_llseek,
};

static int gc570d_it68051_measure_cpoclk(struct gc570d_dev *gc, u32 *cpoclk)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	u32 last_irq;
	u32 last_status;
	u32 value;
	u8 offset = 0;
	u8 reg60;
	u8 a61;
	u8 a62;
	u8 b61;
	u8 b62;
	u8 c61;
	u8 c62;
	u8 d61;
	u8 d62;
	unsigned int poll;
	int restore_ret;
	int ret;

#define IT68051_WRITE(_reg, _value) do { \
	ret = gc570d_receiver_write8(gc, bus, (_reg), (_value)); \
	if (ret) \
		goto out_restore_page; \
} while (0)
#define IT68051_READ(_reg, _value) do { \
	ret = gc570d_receiver_read8(gc, bus, (_reg), &(_value), \
				    &last_irq, &last_status); \
	if (ret) \
		goto out_restore_page; \
} while (0)
#define IT68051_PAGE(_page) do { \
	ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x07, (_page)); \
	if (ret) \
		goto out_restore_page; \
} while (0)

	IT68051_PAGE(0);
	IT68051_WRITE(0xf8, 0xc3);
	IT68051_WRITE(0xf8, 0xa5);
	IT68051_WRITE(0x34, 0x00);
	IT68051_PAGE(1);
	IT68051_WRITE(0x5f, 0x04);
	IT68051_WRITE(0x5f, 0x05);
	IT68051_WRITE(0x58, 0x12);
	IT68051_WRITE(0x58, 0x02);
	IT68051_READ(0x60, reg60);
	if (reg60 != 0x19) {
		IT68051_WRITE(0xf8, 0xc3);
		IT68051_WRITE(0xf8, 0xa5);
		IT68051_WRITE(0x5f, 0x04);
		IT68051_WRITE(0x58, 0x12);
		IT68051_WRITE(0x58, 0x02);
		for (poll = 0; poll < 50; poll++) {
			IT68051_READ(0x60, reg60);
			if (reg60 == 0x19)
				break;
			msleep(1);
		}
		msleep(10);
		IT68051_PAGE(0);
		ret = gc570d_receiver_update8(gc, bus, 0xcf, 0x01, 0x01);
		if (ret)
			goto out_restore_page;
		IT68051_PAGE(1);
	}

	IT68051_WRITE(0x57, 0x01);
	IT68051_WRITE(0x50, 0x00);
	IT68051_WRITE(0x51, 0x00);
	IT68051_WRITE(0x54, 0x04);
	IT68051_READ(0x61, a61);
	IT68051_READ(0x62, a62);
	IT68051_WRITE(0x50, 0x00);
	IT68051_WRITE(0x51, 0x01);
	IT68051_WRITE(0x54, 0x04);
	IT68051_READ(0x61, b61);
	IT68051_READ(0x62, b62);
	if (a61 == 0xff && a62 == 0xff && b61 == 0x00 && b62 == 0x00)
		offset = 4;

	IT68051_WRITE(0x50, offset);
	IT68051_WRITE(0x51, 0xb0);
	IT68051_WRITE(0x54, 0x04);
	IT68051_READ(0x61, c61);
	IT68051_READ(0x62, c62);
	IT68051_WRITE(0x50, offset);
	IT68051_WRITE(0x51, 0xb1);
	IT68051_WRITE(0x54, 0x04);
	IT68051_READ(0x61, d61);
	IT68051_READ(0x62, d62);

	value = ((u32)d61 << 16) | ((u32)c62 << 8) | c61;
	if ((d62 & 0xc0) == 0xc0)
		value /= 100;
	if (value < 0x6f54 || value - 0x6f54 >= 0x4a39)
		value = 38000;
	*cpoclk = value;

	IT68051_WRITE(0x5f, 0x00);
	IT68051_PAGE(0);
	IT68051_WRITE(0xf8, 0x00);
	ret = 0;

out_restore_page:
	restore_ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x07, 0x00);
	if (!ret)
		ret = restore_ret;
#undef IT68051_PAGE
#undef IT68051_READ
#undef IT68051_WRITE
	return ret;
}

int gc570d_it68051_timing_init(struct gc570d_dev *gc)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	u32 rclk;
	u32 timer;
	u32 fraction;
	u32 cpoclk = 0;
	u8 revision;
	u8 short_timer;
	u8 value;
	u32 last_irq;
	u32 last_status;
	int restore_ret;
	int ret;

#define IT68051_TIMING_UPDATE(_reg, _mask, _value) do { \
	ret = gc570d_receiver_update8(gc, bus, (_reg), (_mask), (_value)); \
	if (ret) \
		goto out_restore_page; \
} while (0)
#define IT68051_TIMING_WRITE(_reg, _value) do { \
	ret = gc570d_receiver_write8(gc, bus, (_reg), (_value)); \
	if (ret) \
		goto out_restore_page; \
} while (0)

	/* The official driver: tristate both video outputs before clock setup. */
	IT68051_TIMING_UPDATE(0x0f, 0x07, 0x01);
	IT68051_TIMING_WRITE(0xc5, 0xff);
	IT68051_TIMING_WRITE(0xc6, 0xff);
	IT68051_TIMING_UPDATE(0x0f, 0x07, 0x00);

	ret = gc570d_receiver_read8(gc, bus, 0x04, &revision,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_measure_cpoclk(gc, &cpoclk);
	if (ret)
		goto out_restore_page;

	IT68051_TIMING_UPDATE(0x0f, 0x07, 0x03);
	IT68051_TIMING_UPDATE(0xaa, 0x1f, 0x0c);
	IT68051_TIMING_UPDATE(0x0f, 0x07, 0x00);

	rclk = cpoclk / 2;
	timer = rclk + rclk / 10;
	fraction = ((timer % 1000) * 0x100) / 1000;
	IT68051_TIMING_UPDATE(0x91, 0x3f, min_t(u32, timer / 1000, 0x3f));
	IT68051_TIMING_WRITE(0x92, min_t(u32, fraction, 0xff));

	short_timer = min_t(u32, rclk / 100, 0xff);
	if (revision == 0xb0) {
		IT68051_TIMING_UPDATE(0x0f, 0x07, 0x03);
		IT68051_TIMING_WRITE(0xfa, short_timer);
	} else if (revision == 0xb1) {
		IT68051_TIMING_UPDATE(0x0f, 0x07, 0x01);
		IT68051_TIMING_WRITE(0xfd, short_timer);
		IT68051_TIMING_UPDATE(0xfe, 0x20, 0x00);
		IT68051_TIMING_UPDATE(0xfe, 0x0f, 0x0c);
		IT68051_TIMING_UPDATE(0xfe, 0x10, 0x10);
		IT68051_TIMING_UPDATE(0xfe, 0x80, 0x80);
	}

	IT68051_TIMING_UPDATE(0x0f, 0x07, 0x00);
	value = min_t(u32, cpoclk / 0x138, 0xff);
	IT68051_TIMING_WRITE(0x45, value);
	value = min_t(u32, (cpoclk / 0x138) / 5, 0xff);
	IT68051_TIMING_WRITE(0x44, value);
	value = min_t(u32, cpoclk / 0x910, 0xff);
	IT68051_TIMING_WRITE(0x46, value);
	value = min_t(u32, cpoclk / 0x14c0, 0xff);
	IT68051_TIMING_WRITE(0x47, value);

	dev_info(&gc->pdev->dev,
		 "IT68051 timing initialized: CPOCLK=%u kHz RCLK=%u kHz timer=%u.%03u MHz revision=0x%02x\n",
		 cpoclk, rclk, timer / 1000, timer % 1000, revision);
	ret = 0;

out_restore_page:
	restore_ret = gc570d_receiver_update8(gc, bus, 0x0f, 0x07, 0x00);
	if (!ret)
		ret = restore_ret;
#undef IT68051_TIMING_WRITE
#undef IT68051_TIMING_UPDATE
	return ret;
}

static ssize_t gc570d_it68051_timing_init_write(struct file *file,
						const char __user *buffer,
						size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it68051_timing_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it68051_timing_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it68051_timing_init_write,
	.llseek = noop_llseek,
};

static int gc570d_it68051_edid_verify(struct gc570d_dev *gc,
				      size_t *bad_byte)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	u32 last_irq;
	u32 last_status;
	u8 value;
	size_t block;
	size_t i;
	int ret;

	for (block = 0; block < 2; block++) {
		for (i = 0; i < 127; i++) {
			size_t address = block * 128 + i;

			ret = gc570d_i2c_read8(gc, bus, GC570D_I2C_EDID_ADDR8,
					       address, &value, &last_irq,
					       &last_status);
			if (ret || value != gc570d_it68051_edid[address]) {
				*bad_byte = address;
				return ret ? ret : -EILSEQ;
			}
		}
	}

	return 0;
}

int gc570d_it68051_select_page(struct gc570d_dev *gc, u8 page)
{
	return gc570d_receiver_update8(gc, &gc570d_receiver_buses[0],
				       0x0f, 0x07, page);
}

int gc570d_it68051_program_edid(struct gc570d_dev *gc)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	const u8 vsdb_address = 0xa9;
	u8 block0_checksum = 0;
	u8 block1_checksum = 0;
	u8 port1_checksum;
	size_t bad_byte = 0;
	size_t block;
	size_t i;
	int restore_ret;
	int ret = 0;

	/* The official driver writes 127 payload bytes; IT68051 holds checksums in c9/ca. */
	for (i = 0; i < 127; i++) {
		block0_checksum -= gc570d_it68051_edid[i];
		block1_checksum -= gc570d_it68051_edid[128 + i];
	}
	if (block0_checksum != gc570d_it68051_edid[127] ||
	    block1_checksum != gc570d_it68051_edid[255])
		return -EILSEQ;

	/* The official driver enables the internal EDID RAM at write address 0xa8. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x00, 0x88, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0x4b, 0xa9);
	if (ret)
		goto out_restore_page;

	for (block = 0; block < 2; block++) {
		for (i = 0; i < 127; i++) {
			size_t address = block * 128 + i;

			ret = gc570d_i2c_write8(gc, bus, GC570D_I2C_EDID_ADDR8,
						address,
						gc570d_it68051_edid[address]);
			if (ret) {
				dev_err(&gc->pdev->dev,
					"IT68051 EDID write failed at byte %zu: %d\n",
					address, ret);
				goto out_restore_page;
			}
		}
	}

	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xc9, block0_checksum);
	if (ret)
		goto out_restore_page;

	ret = gc570d_it68051_select_page(gc, 4);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xc9, block0_checksum);
	if (ret)
		goto out_restore_page;

	/* Shared CTA block: port 0 is 1.0.0.0 and port 1 is 2.0.0.0. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xc6, vsdb_address);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xc7, 0x10);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xc8, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xca, block1_checksum);
	if (ret)
		goto out_restore_page;

	port1_checksum = block1_checksum - 0x10;
	ret = gc570d_it68051_select_page(gc, 4);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xc7, 0x20);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xc8, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xca, port1_checksum);
	if (ret)
		goto out_restore_page;

	ret = gc570d_it68051_edid_verify(gc, &bad_byte);
	if (ret)
		dev_err(&gc->pdev->dev,
			"IT68051 EDID verification failed at byte %zu: %d\n",
			bad_byte, ret);
	if (ret)
		goto out_restore_page;

	/* The official driver latches the updated shared EDID independently on both ports. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x01, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 4);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x01, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x10, 0x10);
	if (ret)
		goto out_restore_page;
	msleep(1);
	ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x10, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 4);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x10, 0x10);
	if (ret)
		goto out_restore_page;
	msleep(1);
	ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x10, 0x00);
	if (ret)
		goto out_restore_page;

	/* The official driver with the defaults selected by the official driver. */
	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_page;
	/* The official driver clears the output-width latch before selecting it. */
	ret = gc570d_receiver_update8(gc, bus, 0xc0, 0x01, 0x00);
	if (ret)
		goto out_restore_page;

	/* The official driver: dual-clock mode; initialization output is YUV444. */
	ret = gc570d_receiver_update8(gc, bus, 0xc0, 0x06, 0x02);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc1, 0x02, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc1, 0x20, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x6b, 0x0c, 0x08);

out_restore_page:
	restore_ret = gc570d_it68051_select_page(gc, 0);
	if (!ret)
		ret = restore_ret;
	if (!ret)
		dev_info(&gc->pdev->dev,
			 "IT68051 4K EDID programmed and verified, checksums=0x%02x/0x%02x port1=0x%02x\n",
			 block0_checksum, block1_checksum, port1_checksum);
	return ret;
}

static ssize_t gc570d_it68051_edid_write(struct file *file,
					 const char __user *buffer,
					 size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it68051_program_edid(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it68051_edid_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it68051_edid_write,
	.llseek = noop_llseek,
};

int gc570d_it68051_pulse_hpd(struct gc570d_dev *gc)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	u32 last_irq;
	u32 last_status;
	u8 reg13;
	int ret;

	/* Record the port-0 state used by the official driver before driving HPD. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		return ret;
	ret = gc570d_receiver_read8(gc, bus, 0x13, &reg13,
				    &last_irq, &last_status);
	if (ret)
		return ret;
	/* The official driver maps port-0 HPD to bridge GPIO bank 0, bit 7. */
	mutex_lock(&gc->i2c_lock);
	gc570d_set_reset_bit(gc, 7, false);
	mutex_unlock(&gc->i2c_lock);
	msleep(300);
	mutex_lock(&gc->i2c_lock);
	gc570d_set_reset_bit(gc, 7, true);
	mutex_unlock(&gc->i2c_lock);
	msleep(2);

	dev_info(&gc->pdev->dev,
		 "IT68051 HPD low/high pulse completed, source reg13=0x%02x\n",
		 reg13);
	return 0;
}

static ssize_t gc570d_it68051_hpd_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it68051_pulse_hpd(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it68051_hpd_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it68051_hpd_write,
	.llseek = noop_llseek,
};


/* Reproduce the official driver's STATEV_VidStable output setup. */
int gc570d_it68051_video_output_on(struct gc570d_dev *gc)
{
	static const u8 yuv_bt2020_to_rgb_full_csc[] = {
		0x04, 0x00, 0xa7, 0x4f, 0x09, 0xcc, 0x3a, 0x7e,
		0x3e, 0x4f, 0x09, 0x69, 0x0d, 0x0b, 0x00, 0x4f,
		0x09, 0xfe, 0x3f, 0x1d, 0x11, 0x00,
	};
	static const u8 yuv_bt709_to_rgb_full_csc[] = {
		0x04, 0x00, 0xa7, 0x4f, 0x09, 0xbb, 0x3b, 0x4c,
		0x3e, 0x4f, 0x09, 0x58, 0x0e, 0x00, 0x00, 0x4f,
		0x09, 0x00, 0x00, 0xec, 0x10, 0x00,
	};
	static const u8 yuv_bt601_to_rgb_full_csc[] = {
		0x04, 0x00, 0xa7, 0x4f, 0x09, 0x81, 0x39, 0xdd,
		0x3c, 0x4f, 0x09, 0xc4, 0x0c, 0x00, 0x00, 0x4f,
		0x09, 0x00, 0x00, 0x17, 0x10, 0x00,
	};
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	u32 last_irq;
	u32 last_status;
	u8 mode;
	u8 avi15;
	u8 avi16;
	u8 avi17;
	u8 input_color;
	u8 output_color;
	u8 colorimetry;
	u8 extended_colorimetry;
	bool hdr_bt2020;
	u8 reg19 = 0;
	u8 c5;
	u8 c6;
	u8 avmute_packet;
	u8 avmute_control;
	struct gc570d_splitter_rx_timing rx_timing = { 0 };
	bool dual_pixel;
	unsigned int i;
	int restore_ret;
	int ret;

	if ((gc->hdmi1_auto_phase && !READ_ONCE(gc->hdmi1_auto_ready)) ||
	    (READ_ONCE(gc->splitter_pump_thread) &&
	     (gc->splitter_video_stable_pending ||
	      !(gc->splitter_worker_completed_mask & BIT(1)) ||
	      (gc->splitter_worker_state4_mask & BIT(1))))) {
		WRITE_ONCE(gc->video0_receiver_output_valid, false);
		return -ENOLINK;
	}

	/* Require a locked IT68051 signal before querying the splitter or enabling output. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x19, &reg19,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	if (!(reg19 & BIT(7))) {
		WRITE_ONCE(gc->video0_receiver_output_valid, false);
		ret = -ENOLINK;
		goto out_restore_page;
	}
	/* The official driver: stable-state marker. */
	ret = gc570d_receiver_write8(gc, bus, 0x90, 0x8f);
	if (ret)
		goto out_restore_page;

	/* The official driver: current AVI input color. */
	ret = gc570d_it68051_select_page(gc, 2);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x15, &avi15,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x16, &avi16,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x17, &avi17,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	input_color = (avi15 >> 5) & 0x03;
	colorimetry = avi16 >> 6;
	extended_colorimetry = (avi17 >> 4) & 0x07;
	hdr_bt2020 = colorimetry == 3 &&
		(extended_colorimetry == 5 || extended_colorimetry == 6);

	/*
	 * The official driver selects single-pixel for sub-1921-wide signals below
	 * 150001 kHz.  It selects dual-pixel for YUV420, wider signals, and
	 * 1920-wide/high-rate input.  Re-evaluate this on every stable source;
	 * retaining 1440p's dual-pixel state at 720p leaves VIP0 without data.
	 */
	ret = gc570d_splitter_measure_rx_timing(gc, input_color, &rx_timing);
	if (ret)
		goto out_restore_page;
	if (rx_timing.hactive < 640 || rx_timing.hactive > 4096 ||
	    (rx_timing.hactive & 1) ||
	    rx_timing.vactive < 240 || rx_timing.vactive > 2160) {
		WRITE_ONCE(gc->video0_receiver_output_valid, false);
		ret = -ENOLINK;
		goto out_restore_page;
	}
	dual_pixel = input_color == 3 || rx_timing.hactive > 1920 ||
		(rx_timing.pixel_clock >= 150001 &&
		 (rx_timing.hactive >= 1920 || rx_timing.vactive >= 2160));
	if (READ_ONCE(gc->video0_receiver_output_valid) &&
	    rx_timing.hactive && rx_timing.vactive &&
	    gc->video0_receiver_width == rx_timing.hactive &&
	    gc->video0_receiver_height == rx_timing.vactive &&
	    gc->video0_receiver_interlaced == !!(rx_timing.flags & BIT(0)) &&
	    gc->video0_receiver_input_color == input_color &&
	    gc->video0_receiver_colorimetry == colorimetry &&
	    gc->video0_receiver_extended_colorimetry ==
		extended_colorimetry &&
	    gc->video0_receiver_dual_pixel == dual_pixel) {
		u8 cur4f = 0;

		ret = gc570d_it68051_select_page(gc, 0);
		if (!ret)
			ret = gc570d_receiver_read8(gc, bus, 0x4f, &cur4f,
						    &last_irq, &last_status);
		if (!ret && !(cur4f & BIT(5))) {
			dev_dbg(&gc->pdev->dev,
				"IT68051 stable output retained: %ux%u input=%u C=%u EC=%u pixel_mode=%s pclk=%u\n",
				 rx_timing.hactive, rx_timing.vactive,
				 input_color, colorimetry,
				 extended_colorimetry,
				 dual_pixel ? "dual" : "single",
				 rx_timing.pixel_clock);
			goto out_restore_page;
		}
		WRITE_ONCE(gc->video0_receiver_output_valid, false);
	}

	WRITE_ONCE(gc->video0_receiver_output_valid, false);
	if (READ_ONCE(gc->audio0_prepared) && READ_ONCE(gc->audio0_running)) {
		WRITE_ONCE(gc->audio0_recovering, true);
		wake_up_interruptible(&gc->audio0_wait);
	}

	WRITE_ONCE(gc->video0_receiver_output_valid, false);
	if (READ_ONCE(gc->audio0_prepared) && READ_ONCE(gc->audio0_running)) {
		WRITE_ONCE(gc->audio0_recovering, true);
		wake_up_interruptible(&gc->audio0_wait);
	}
	if (READ_ONCE(gc->video0_dma_active)) {
		WRITE_ONCE(gc->video0_reconfigure_pending, true);
		wake_up_interruptible(&gc->video0_wait);
	}

	/* The official driver prologue. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x64, 0x04, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x64, 0x02, 0x02);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x64, 0x02, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 5);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x20, 0x40, 0x40);
	if (ret)
		goto out_restore_page;

	/* The official driver: clock mode; initialization output is YUV444. */
	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc0, 0x06, 0x02);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc1, 0x02,
				      dual_pixel ? 0x02 : 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc1, 0x20, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x6b, 0x0c, 0x08);
	if (ret)
		goto out_restore_page;

	/* The official driver: single- or dual-pixel output. */
	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc0, 0x01,
				      dual_pixel ? 0x01 : 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 5);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xd1, 0x01, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xd1, 0x0c, 0x04);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xda, 0x10, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xd0, 0xf3);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xbd, 0x30,
				      dual_pixel ? 0x10 : 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xbe, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xfe, 0x10, 0x10);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0xc4,
				     dual_pixel && input_color != 3 ?
				     0x10 : 0x00);
	if (ret)
		goto out_restore_page;

	/* End of the official driver: latch the selected even/odd pixel mode. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x64, 0x04, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x64, 0x02, 0x02);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x64, 0x02, 0x00);
	if (ret)
		goto out_restore_page;

	/*
	 * The official driver enables the high-clock HDMI output path when the
	 * measured pixel clock, after the reg1b divisor, exceeds 25 MHz.
	 * Every locked 1440p/2160p mode is above that threshold.
	 */
	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xb0, 0x01, 0x01);
	if (ret)
		goto out_restore_page;

	/*
	 * The official driver returns a nonzero policy for an HDR DRM packet paired
	 * with AVI extended-colorimetry 5/6.  For a YUV input the official driver uses
	 * ~-(policy != 0) & 2, hence that nonzero policy selects output color 0
	 * (RGB), not 2 (YCbCr444).  The current PS5 mode was also confirmed
	 * visually as HDR, so its BT.2020 AVI tuple is sufficient for this bounded
	 * reconstruction even though the cached DRM packet is not directly
	 * addressable through the IT68051 register interface.
	 *
	 * For 4K60 SDR BT.709 YCbCr 4:2:0 (input_color == 3), Windows keeps the
	 * IT68051 output in YCbCr 4:4:4 (output_color = 2) and uses the Xilinx
	 * 0x33 path.  For HDR BT.2020 and 4:2:2/4:4:4 SDR YCbCr inputs, drive
	 * the IT68051 output bus as RGB444 (output_color = 0) with the hardware
	 * YCbCr-to-RGB color space converter enabled.
	 */
	/*
	 * For 4K60 SDR BT.709 YCbCr 4:2:0 (input_color == 3), Windows keeps the
	 * IT68051 output in YCbCr 4:4:4 (output_color = 2) and uses the Xilinx
	 * 0x33 path.  For HDR BT.2020 and 4:2:2/4:4:4 SDR YCbCr inputs (such as
	 * 480p/576p/720p/1080i/1080p YCbCr upscalers and consoles), drive the
	 * IT68051 output bus as RGB444 (output_color = 0) with the hardware
	 * YCbCr-to-RGB CSC (whose 0x04/-16 offset and 0x094f/1.164 gain convert
	 * limited-range YCbCr to full-range RGB).
	 */
	output_color = (input_color == 3 && !hdr_bt2020) ? 2 : 0;
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x6b, 0x31,
				      input_color << 4);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x6b, 0x0c,
				      output_color << 2);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0x6e, 0xa0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_write8(gc, bus, 0x86, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x6c, 0x03,
				      output_color == 0 && input_color != 0 ?
				      0x03 : 0x00);
	if (ret)
		goto out_restore_page;
	if (output_color == 0 && input_color != 0) {
		/* Select BT.2020 HDR, BT.601 SD, or BT.709 HD YCbCr-to-RGB CSC matrix. */
		bool bt601 = colorimetry == 1 ||
			(colorimetry == 0 && rx_timing.vactive <= 576 &&
			 !(rx_timing.flags & BIT(0)));
		const u8 *csc = hdr_bt2020 ? yuv_bt2020_to_rgb_full_csc :
			(bt601 ? yuv_bt601_to_rgb_full_csc :
			 yuv_bt709_to_rgb_full_csc);

		ret = gc570d_it68051_select_page(gc, 1);
		if (ret)
			goto out_restore_page;
		for (i = 0; i < ARRAY_SIZE(yuv_bt2020_to_rgb_full_csc);
		     i++) {
			ret = gc570d_receiver_write8(gc, bus, 0x70 + i, csc[i]);
			if (ret)
				goto out_restore_page;
		}
	} else {
		/* The no-conversion branch of the official driver clears page-1 reg85. */
		ret = gc570d_it68051_select_page(gc, 1);
		if (ret)
			goto out_restore_page;
		ret = gc570d_receiver_write8(gc, bus, 0x85, 0x00);
		if (ret)
			goto out_restore_page;
	}

	/* The official driver: assert AVMute while the output is restarted. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x4f, 0xa0, 0xa0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x80, 0x80);
	if (ret)
		goto out_restore_page;

	/* Apply the official driver's pre-output enable sequence. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x22, 0x01, 0x01);
	if (ret)
		goto out_restore_page;
	usleep_range(1000, 2000);
	ret = gc570d_receiver_update8(gc, bus, 0x22, 0x01, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x10, 0x02, 0x02);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0x12, 0x80, 0x80);
	if (ret)
		goto out_restore_page;

	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x80, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_update8(gc, bus, 0xc6, 0x80, 0x00);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0xc0, &mode,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;

	if (mode & BIT(0)) {
		ret = gc570d_receiver_write8(gc, bus, 0xc6, 0x00);
	} else if ((mode & 0xc0) == 0x00 || (mode & 0xc0) == 0x40) {
		ret = gc570d_receiver_write8(gc, bus, 0xc5, 0x38);
	} else if ((mode & 0xc0) == 0x80) {
		ret = gc570d_receiver_write8(gc, bus, 0xc5, 0x23);
	}
	if (ret)
		goto out_restore_page;

	ret = gc570d_receiver_read8(gc, bus, 0xc5, &c5,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0xc6, &c6,
				    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;

	/*
	 * The official driver's STATEV_VidStable path asserts AVMute in page-0
	 * register 0x4f.  On the following periodic state pass, it tests bit 5 and
	 * releases AVMute when the receiver reports that the source is not
	 * sending a Set AVMute packet (page-0 register 0xaa bit 3 clear).
	 * Without this deferred release the receiver emits valid timing filled
	 * with legal-range black, so DMA completes normally but captures only
	 * 10:80:10:80 pixels.
	 */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	for (i = 0; i < 5; i++) {
		ret = gc570d_receiver_read8(gc, bus, 0xaa, &avmute_packet,
					    &last_irq, &last_status);
		if (ret || !(avmute_packet & BIT(3)))
			break;
		msleep(10);
	}
	if (ret)
		goto out_restore_page;
	if (!(avmute_packet & BIT(3))) {
		/* Exact HDMI/dual-pixel branch of the official driver. */
		ret = gc570d_it68051_select_page(gc, 1);
		if (ret)
			goto out_restore_page;
		ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x01, 0x01);
		if (ret)
			goto out_restore_page;
		ret = gc570d_receiver_update8(gc, bus, 0xc5, 0x01, 0x00);
		if (ret)
			goto out_restore_page;
		if (mode & BIT(0)) {
			ret = gc570d_receiver_update8(gc, bus, 0xc5,
						      0x01, 0x01);
			if (ret)
				goto out_restore_page;
		}
		ret = gc570d_it68051_select_page(gc, 0);
		if (ret)
			goto out_restore_page;
		ret = gc570d_receiver_update8(gc, bus, 0x4f, 0xa0, 0xa0);
		if (ret)
			goto out_restore_page;
		ret = gc570d_receiver_update8(gc, bus, 0x4f, 0xa0, 0x80);
		if (ret)
			goto out_restore_page;
	}
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0x4f, &avmute_control,
				    &last_irq, &last_status);
	if (!ret)
		dev_info(&gc->pdev->dev,
			 "IT68051 official-driver stable video enabled: AVI=%02x/%02x/%02x input=%u C=%u EC=%u hdr_bt2020=%s output=%u pixel_mode=%s pclk=%u hactive=%u vactive=%u page1 c0=%02x c5=%02x c6=%02x source_avmute=%s page0_4f=%02x\n",
			 avi15, avi16, avi17, input_color, colorimetry,
			 extended_colorimetry, hdr_bt2020 ? "yes" : "no",
			 output_color, dual_pixel ? "dual" : "single",
			 rx_timing.pixel_clock, rx_timing.hactive,
			 rx_timing.vactive, mode, c5, c6,
			 avmute_packet & BIT(3) ? "yes" : "no",
			 avmute_control);
	if (!ret && (c5 & BIT(7))) {
		ret = -EAGAIN;
		goto out_restore_page;
	}
	if (!ret) {
		gc->video0_receiver_width = rx_timing.hactive;
		gc->video0_receiver_height = rx_timing.vactive;
		gc->video0_receiver_interlaced = !!(rx_timing.flags & BIT(0));
		gc->video0_receiver_input_color = input_color;
		gc->video0_receiver_colorimetry = colorimetry;
		gc->video0_receiver_extended_colorimetry =
			extended_colorimetry;
		gc->video0_receiver_dual_pixel = dual_pixel;
		WRITE_ONCE(gc->video0_receiver_output_valid, true);
	}

out_restore_page:
	restore_ret = gc570d_it68051_select_page(gc, 0);
	if (!ret)
		ret = restore_ret;
	return ret;
}

static ssize_t gc570d_it68051_video_output_on_write(struct file *file,
						     const char __user *buffer,
						     size_t count,
						     loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it68051_video_output_on(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it68051_video_output_on_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it68051_video_output_on_write,
	.llseek = noop_llseek,
};

static int gc570d_it68051_edid_status_show(struct seq_file *s, void *unused)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	struct gc570d_dev *gc = s->private;
	size_t bad_byte = 0;
	u8 page0[4] = { 0 };
	u8 page4[3] = { 0 };
	u32 last_irq;
	u32 last_status;
	int restore_ret;
	int ret;

	mutex_lock(&gc->capture_lock);
	ret = gc570d_it68051_edid_verify(gc, &bad_byte);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0xc6, &page0[0], &last_irq,
				    &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc7, &page0[1],
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc8, &page0[2],
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xca, &page0[3],
					    &last_irq, &last_status);
	if (ret)
		goto out_restore_page;
	ret = gc570d_it68051_select_page(gc, 4);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0xc7, &page4[0], &last_irq,
				    &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xc8, &page4[1],
					    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0xca, &page4[2],
					    &last_irq, &last_status);

out_restore_page:
	restore_ret = gc570d_it68051_select_page(gc, 0);
	if (!ret)
		ret = restore_ret;
	mutex_unlock(&gc->capture_lock);
	seq_printf(s,
		   "verified=%s bad_byte=%zu page0 c6=%02x c7=%02x c8=%02x ca=%02x page4 c7=%02x c8=%02x ca=%02x error=%d\n",
		   ret ? "no" : "yes", bad_byte, page0[0], page0[1],
		   page0[2], page0[3], page4[0], page4[1], page4[2], ret);
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_it68051_edid_status);

int gc570d_it6802_init(struct gc570d_dev *gc)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[1];
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(gc570d_it6802_init_table); i++) {
		const struct gc570d_receiver_init_entry *entry =
			&gc570d_it6802_init_table[i];

		ret = gc570d_receiver_update8(gc, bus, entry->address,
					      entry->mask, entry->value);
		if (ret) {
			dev_err(&gc->pdev->dev,
				"IT6802 init failed at entry %zu register 0x%02x: %d\n",
				i, entry->address, ret);
			return ret;
		}
	}

	/* Tail of CVDecITE6802::Init after the base table. */
	ret = gc570d_receiver_update8(gc, bus, 0xc0, 0x20, 0x20);
	if (ret)
		return ret;
	msleep(1);
	ret = gc570d_receiver_update8(gc, bus, 0xc0, 0x20, 0x00);
	if (ret)
		return ret;
	ret = gc570d_receiver_update8(gc, bus, 0x51, 0x01, 0x00);
	if (ret)
		return ret;

	dev_info(&gc->pdev->dev,
		 "IT6802 minimal initialization table completed\n");
	return 0;
}

static ssize_t gc570d_it6802_init_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_it6802_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_it6802_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_it6802_init_write,
	.llseek = noop_llseek,
};

static int gc570d_receiver_registers_show(struct seq_file *s, void *unused)
{
	struct gc570d_dev *gc = s->private;
	size_t channel;

	seq_puts(s, "# GC570D HDMI receiver identification and raw link status\n");
	for (channel = 0; channel < ARRAY_SIZE(gc570d_receiver_buses);
	     channel++) {
		const struct gc570d_i2c_bus *bus =
			&gc570d_receiver_buses[channel];
		u8 signature[4] = { 0 };
		u8 link_status[3] = { 0 };
		static const u8 link_registers[] = { 0x0a, 0x0b, 0x51 };
		u8 revision = 0;
		u32 last_irq = 0;
		u32 last_status = 0;
		int ret = 0;
		u8 address;

		for (address = 0; address < ARRAY_SIZE(signature); address++) {
			ret = gc570d_receiver_read8(gc, bus,
						   address, &signature[address],
						   &last_irq, &last_status);
			if (ret)
				break;
		}
		if (!ret)
			ret = gc570d_receiver_read8(gc, bus,
						   4, &revision, &last_irq,
						   &last_status);
		if (!ret) {
			for (address = 0; address < ARRAY_SIZE(link_registers);
			     address++) {
				ret = gc570d_receiver_read8(gc, bus,
							   link_registers[address],
							   &link_status[address],
							   &last_irq,
							   &last_status);
				if (ret)
					break;
			}
		}

		if (ret) {
			seq_printf(s,
				   "channel=%zu bus=%zu slave=0x48 error=%d irq=0x%08x status=0x%08x\n",
				   channel, channel, ret, last_irq, last_status);
			continue;
		}

		seq_printf(s,
			   "channel=%zu receiver=%s bus=%zu slave=0x48 id=%02x:%02x:%02x:%02x match=%s revision=0x%02x reg0a=0x%02x reg0b=0x%02x reg51=0x%02x irq=0x%08x status=0x%08x\n",
			   channel, bus->receiver, channel, signature[0],
			   signature[1], signature[2], signature[3],
			   signature[0] == 0x54 && signature[1] == 0x49 &&
			   signature[2] == bus->expected_device &&
			   signature[3] == 0x68 ? "yes" : "no",
			   revision, link_status[0], link_status[1],
			   link_status[2], last_irq, last_status);
	}

	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_receiver_registers);

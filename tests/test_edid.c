// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_edid.c — Unit tests for GC570D IT68051 EDID bit-for-bit integrity,
 * dynamic 1440p60 bridge EDID generation, and CTA-861 block merging.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* Mocks for hardware/kernel functions called by driver source files */
bool auto_hdmi1 = false;
bool copy_edid = false;
int gc570d_i2c_read8(void *gc, const void *bus, uint8_t addr, uint8_t reg, uint8_t *val, void *a, void *b) { return 0; }
int gc570d_i2c_read_buf(void *gc, const void *bus, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) { return 0; }
int gc570d_i2c_write8(void *gc, const void *bus, uint8_t addr, uint8_t reg, uint8_t val) { return 0; }
void *gc570d_kthread_create(void *fn, void *data, const char *fmt, ...) { return 0; }
void gc570d_log_msg(const char *lvl, const char *fmt, ...) {}
uint8_t gc570d_probe_buses[3];
uint8_t gc570d_receiver_buses[2];
uint8_t gc570d_splitter_bus;
bool gc570d_subsystem_busy(void *gc) { return false; }
bool kthread_should_stop(void) { return false; }
int kthread_stop(void *t) { return 0; }
void wake_up_interruptible(void *q) {}
int wake_up_process(void *p) { return 0; }
void gc570d_set_reset_bit(void *gc, uint32_t bit, bool assert_high) {}

extern const u8 gc570d_it68051_edid[256];
void gc570d_splitter_build_bridge_edid(u8 *edid);
void gc570d_splitter_merge_cta_data_blocks(u8 *dst_cta, const u8 *src_cta);

static int failures = 0;

static void check(bool condition, const char *name, const char *detail)
{
	if (condition) {
		printf("  [PASS] %s\n", name);
	} else {
		printf("  [FAIL] %s: %s\n", name, detail ? detail : "assertion failed");
		failures++;
	}
}

static u8 calc_checksum(const u8 *block)
{
	u32 sum = 0;
	for (int i = 0; i < 128; i++)
		sum += block[i];
	return (u8)(sum & 0xff);
}

static void test_it68051_edid_bit_for_bit(void)
{
	printf("\n--- Test: IT68051 EDID Bit-for-Bit Integrity ---\n");

	/* 1. Header */
	static const u8 edid_header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	check(memcmp(gc570d_it68051_edid, edid_header, 8) == 0,
	      "EDID Header 00FFFFFFFFFFFF00", "Header mismatch");

	/* 2. Manufacturer ID: "AVT" (0x06, 0xd8) and Product code (0x50, 0x00) */
	check(gc570d_it68051_edid[8] == 0x06 && gc570d_it68051_edid[9] == 0xd8,
	      "Manufacturer ID matches AVT (06D8h)", "Manufacturer ID mismatch");
	check(gc570d_it68051_edid[10] == 0x50 && gc570d_it68051_edid[11] == 0x00,
	      "Product ID matches GC570D (0050h)", "Product ID mismatch");

	/* 3. Block 0 Checksum: byte 127 must be 0x9a, sum mod 256 == 0 */
	check(gc570d_it68051_edid[127] == 0x9a,
	      "Block 0 Checksum byte is 0x9a", "Checksum byte != 0x9a");
	check(calc_checksum(gc570d_it68051_edid) == 0,
	      "Block 0 sum mod 256 == 0", "Block 0 sum mod 256 != 0");

	/* 4. Block 1 Tag and Revision */
	check(gc570d_it68051_edid[128] == 0x02,
	      "Block 1 Tag is 0x02 (CTA-861)", "Block 1 tag != 0x02");
	check(gc570d_it68051_edid[129] == 0x03,
	      "Block 1 Revision is 0x03", "Block 1 revision != 0x03");

	/* 5. Block 1 Checksum: byte 255 must be 0x81, sum mod 256 == 0 */
	check(gc570d_it68051_edid[255] == 0x81,
	      "Block 1 Checksum byte is 0x81 (Upstream OpenGC570D match)", "Checksum byte != 0x81");
	check(calc_checksum(gc570d_it68051_edid + 128) == 0,
	      "Block 1 sum mod 256 == 0", "Block 1 sum mod 256 != 0");

	/* 6. Fourth DTD is 1080p144 (starts with 0xfc, 0x7e, 0x80) */
	check(gc570d_it68051_edid[237] == 0xfc && gc570d_it68051_edid[238] == 0x7e &&
	      gc570d_it68051_edid[239] == 0x80,
	      "Fourth DTD is original 1080p144 (fc7e80h)", "Fourth DTD mismatch");
}

static void test_bridge_edid_generation(void)
{
	printf("\n--- Test: Dynamic 1440p60 Bridge EDID Generation ---\n");

	u8 bridge_edid[256];
	memset(bridge_edid, 0, sizeof(bridge_edid));

	gc570d_splitter_build_bridge_edid(bridge_edid);

	/* 1. Block 0 must be 100% bit-for-bit identical to gc570d_it68051_edid */
	check(memcmp(bridge_edid, gc570d_it68051_edid, 128) == 0,
	      "Bridge EDID Block 0 unchanged", "Block 0 was modified");

	/* 2. Block 1 bytes 0..108 must be 100% identical */
	check(memcmp(bridge_edid + 128, gc570d_it68051_edid + 128, 109) == 0,
	      "Bridge EDID CTA data blocks & DTDs 1-3 unchanged", "Bytes 0..108 modified");

	/* 3. Fourth DTD (offset 237) replaced with 1440p60 (241.5 MHz = 0x5e56) */
	static const u8 exp_1440p60[18] = {
		0x56, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50,
		0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00,
		0x00, 0x18,
	};
	check(memcmp(bridge_edid + 237, exp_1440p60, 18) == 0,
	      "Fourth DTD correctly replaced with 1440p60 CVT-RB", "DTD 4 mismatch");

	/* 4. Recalculated Checksum must be 0x88 and sum mod 256 == 0 */
	check(bridge_edid[255] == 0x88,
	      "Bridge EDID Block 1 checksum is 0x88", "Checksum != 0x88");
	check(calc_checksum(bridge_edid + 128) == 0,
	      "Bridge EDID Block 1 sum mod 256 == 0", "Sum mod 256 != 0");
}

static void test_cta_block_merging(void)
{
	printf("\n--- Test: CTA Extension Data Block Merging ---\n");

	u8 dst_cta[128];
	u8 src_cta[128];
	memset(dst_cta, 0, sizeof(dst_cta));
	memset(src_cta, 0, sizeof(src_cta));

	/* Base destination CTA block: 1 video data block (4 bytes), d=8 */
	dst_cta[0] = 0x02; /* CTA extension tag */
	dst_cta[1] = 0x03; /* Revision */
	dst_cta[2] = 0x08; /* Offset to DTDs = 8 */
	dst_cta[3] = 0x00; /* Flags */
	dst_cta[4] = 0x43; /* Video Data Block: tag 2, len 3 */
	dst_cta[5] = 0x10; /* 1080p60 */
	dst_cta[6] = 0x1f; /* 1080p50 */
	dst_cta[7] = 0x04; /* 720p60 */

	/* Base source CTA block: HDR Static Metadata (tag 7, ext 6, len 3) */
	src_cta[0] = 0x02;
	src_cta[1] = 0x03;
	src_cta[2] = 0x08;
	src_cta[3] = 0x00;
	src_cta[4] = 0xe3; /* Tag 7, len 3 */
	src_cta[5] = 0x06; /* Extended tag 6: HDR Static Metadata */
	src_cta[6] = 0x05; /* EOTF: Traditional SDR + SMPTE ST 2084 (HDR10) */
	src_cta[7] = 0x01; /* Static Metadata Descriptor type 1 */

	gc570d_splitter_merge_cta_data_blocks(dst_cta, src_cta);

	/* After merge, dst_cta[2] should have expanded by 4 bytes (8 + 4 = 12) */
	check(dst_cta[2] == 12,
	      "Destination DTD offset updated to 12 after merging HDR block",
	      "DTD offset not updated");
	check(dst_cta[8] == 0xe3 && dst_cta[9] == 0x06 && dst_cta[10] == 0x05,
	      "HDR Static Metadata block successfully merged into destination",
	      "HDR block missing or corrupted");
}

int main(void)
{
	printf("=== Running OpenGC570D EDID & CTA Unit Tests ===\n");
	test_it68051_edid_bit_for_bit();
	test_bridge_edid_generation();
	test_cta_block_merging();

	if (failures == 0) {
		printf("\n>>> All EDID tests passed successfully! <<<\n");
		return 0;
	} else {
		printf("\n>>> %d EDID test(s) failed! <<<\n", failures);
		return 1;
	}
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_gc573_edid.c — Bounds and validation tests for the GC573 userspace
 * EDID parser (gc573_user_edid_validate).
 *
 * The engine only consumes a fixed 256-byte image, but the validator is the
 * gate in front of --raw-edid, so truncated buffers, bogus block counts and
 * malformed CTA data collections must all be rejected before any I2C traffic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>

#include "gc573_user_edid.h"

/* Stubs for the hardware paths pulled in by gc573_user_edid.c.  Validation
 * never calls them, and the test only exercises the pure parser. */
int gc573_user_set_gpio(void *bar0, unsigned int pin, bool high,
			unsigned int delay_ms)
{
	(void)bar0;
	(void)pin;
	(void)high;
	(void)delay_ms;
	return 0;
}

int gc573_user_get_gpio(void *bar0, unsigned int pin, bool *high)
{
	(void)bar0;
	(void)pin;
	if (high)
		*high = false;
	return 0;
}

int gc573_user_i2c_read(void *bar0, unsigned int bus, u16 addr, u32 subaddr,
			unsigned int subaddr_len, u8 *data,
			unsigned int data_len)
{
	(void)bar0;
	(void)bus;
	(void)addr;
	(void)subaddr;
	(void)subaddr_len;
	(void)data;
	(void)data_len;
	return -1;
}

int gc573_user_i2c_write(void *bar0, unsigned int bus, u16 addr, u32 subaddr,
			 unsigned int subaddr_len, const u8 *data,
			 unsigned int data_len)
{
	(void)bar0;
	(void)bus;
	(void)addr;
	(void)subaddr;
	(void)subaddr_len;
	(void)data;
	(void)data_len;
	return -1;
}

int gc573_user_it6805_set_bank(void *bar0, u8 bank)
{
	(void)bar0;
	(void)bank;
	return -1;
}

static int failures;

static void check(bool condition, const char *name, const char *detail)
{
	if (condition) {
		printf("  [PASS] %s\n", name);
	} else {
		printf("  [FAIL] %s: %s\n", name,
		       detail ? detail : "assertion failed");
		failures++;
	}
}

static void fix_checksum(u8 *block)
{
	u8 sum = 0;

	for (unsigned int i = 0; i < 127; i++)
		sum += block[i];
	block[127] = (u8)(0x100U - sum);
}

/* Build a minimal but valid 256-byte EDID: base block plus one CTA-861
 * extension with no data blocks and no DTDs. */
static void build_valid_edid(u8 edid[GC573_EDID_SIZE])
{
	memset(edid, 0, GC573_EDID_SIZE);
	edid[0] = 0x00;
	edid[1] = 0xff;
	edid[2] = 0xff;
	edid[3] = 0xff;
	edid[4] = 0xff;
	edid[5] = 0xff;
	edid[6] = 0xff;
	edid[7] = 0x00;
	edid[126] = 1;		/* one extension block */

	edid[128] = 0x02;	/* CTA-861 extension tag */
	edid[129] = 0x03;	/* revision */
	edid[130] = 0x04;	/* no data blocks, DTDs start at offset 4 */

	fix_checksum(edid);
	fix_checksum(edid + 128);
}

static void expect_reject(const u8 *edid, size_t len, const char *name)
{
	check(gc573_user_edid_validate(edid, len) < 0, name,
	      "validator accepted a malformed image");
}

static void expect_accept(const u8 *edid, size_t len, const char *name)
{
	check(gc573_user_edid_validate(edid, len) == 0, name,
	      "validator rejected a valid image");
}

static void test_lengths(void)
{
	u8 edid[GC573_EDID_SIZE];

	printf("\n--- Test: length and NULL handling ---\n");
	build_valid_edid(edid);

	expect_reject(NULL, 0, "NULL buffer rejected");
	expect_reject(edid, 0, "zero-length buffer rejected");
	expect_reject(edid, 127, "127-byte buffer rejected");
	expect_reject(edid, 128, "single 128-byte block rejected");
	expect_reject(edid, 255, "255-byte buffer rejected");
	expect_accept(edid, GC573_EDID_SIZE, "256-byte image accepted");
}

static void test_block_count(void)
{
	u8 edid[GC573_EDID_SIZE];
	u8 three[384];

	printf("\n--- Test: extension block count ---\n");

	build_valid_edid(edid);
	edid[126] = 0;
	fix_checksum(edid);
	expect_reject(edid, GC573_EDID_SIZE,
		      "edid[126] == 0 rejected for a 256-byte image");

	build_valid_edid(edid);
	edid[126] = 0xff;
	fix_checksum(edid);
	expect_reject(edid, GC573_EDID_SIZE,
		      "edid[126] == 0xff rejected");

	memset(three, 0, sizeof(three));
	build_valid_edid(three);
	three[126] = 2;
	fix_checksum(three);
	fix_checksum(three + 128);
	fix_checksum(three + 256);
	expect_reject(three, sizeof(three),
		      "three-block image exceeds supported block count");
}

static void test_header_and_tags(void)
{
	u8 edid[GC573_EDID_SIZE];

	printf("\n--- Test: header, extension tag and checksums ---\n");

	build_valid_edid(edid);
	edid[0] = 0x01;
	fix_checksum(edid);
	expect_reject(edid, GC573_EDID_SIZE, "bad base-block header rejected");

	build_valid_edid(edid);
	edid[128] = 0x03;	/* video timing extension, not CTA-861 */
	fix_checksum(edid + 128);
	expect_reject(edid, GC573_EDID_SIZE, "bad extension tag rejected");

	build_valid_edid(edid);
	edid[10] ^= 0x01;
	expect_reject(edid, GC573_EDID_SIZE,
		      "corrupt block 0 checksum rejected");

	build_valid_edid(edid);
	edid[140] ^= 0x01;
	expect_reject(edid, GC573_EDID_SIZE,
		      "corrupt block 1 checksum rejected");
}

static char *write_temp_edid(const u8 *data, size_t len)
{
	char path[] = "/tmp/everlive-edid-XXXXXX";
	int fd = mkstemp(path);
	ssize_t written;

	if (fd < 0)
		return NULL;
	written = write(fd, data, len);
	close(fd);
	if (written != (ssize_t)len) {
		unlink(path);
		return NULL;
	}
	return strdup(path);
}

static void test_file_loading(void)
{
	u8 edid[GC573_EDID_SIZE];
	u8 oversized[384];
	char *path;
	int ret;

	printf("\n--- Test: EDID file loading ---\n");
	build_valid_edid(edid);

	path = write_temp_edid(edid, 255);
	if (path) {
		ret = gc573_user_edid_select(NULL, path);
		check(ret < 0, "truncated 255-byte file rejected",
		      "loader accepted a truncated file");
		unlink(path);
		free(path);
	} else {
		check(false, "truncated file fixture", "mkstemp/write failed");
	}

	memset(oversized, 0, sizeof(oversized));
	memcpy(oversized, edid, sizeof(edid));
	path = write_temp_edid(oversized, 257);
	if (path) {
		ret = gc573_user_edid_select(NULL, path);
		check(ret < 0, "oversized 257-byte file rejected",
		      "loader accepted and truncated an oversized file");
		unlink(path);
		free(path);
	} else {
		check(false, "oversized file fixture", "mkstemp/write failed");
	}

	path = write_temp_edid(oversized, sizeof(oversized));
	if (path) {
		ret = gc573_user_edid_select(NULL, path);
		check(ret < 0, "three-block file rejected",
		      "loader accepted more blocks than the hardware supports");
		unlink(path);
		free(path);
	} else {
		check(false, "three-block file fixture", "mkstemp/write failed");
	}

	path = write_temp_edid(edid, sizeof(edid));
	if (path) {
		ret = gc573_user_edid_select(NULL, path);
		check(ret == 0, "valid 256-byte file accepted",
		      "loader rejected a valid file");
		unlink(path);
		free(path);
	} else {
		check(false, "valid file fixture", "mkstemp/write failed");
	}
}

static void test_cta_bounds(void)
{
	u8 edid[GC573_EDID_SIZE];

	printf("\n--- Test: CTA-861 DTD offset and data-block bounds ---\n");

	build_valid_edid(edid);
	edid[130] = 0x03;
	fix_checksum(edid + 128);
	expect_reject(edid, GC573_EDID_SIZE, "CTA DTD offset < 4 rejected");

	build_valid_edid(edid);
	edid[130] = 0x80;	/* 128, outside the 128-byte block */
	fix_checksum(edid + 128);
	expect_reject(edid, GC573_EDID_SIZE, "CTA DTD offset > 127 rejected");

	/* Valid collection with one 4-byte Video Data Block. */
	build_valid_edid(edid);
	edid[130] = 0x08;
	edid[132] = 0x43;	/* tag 2 (video), length 3 */
	edid[133] = 0x10;
	edid[134] = 0x1f;
	edid[135] = 0x04;
	fix_checksum(edid + 128);
	expect_accept(edid, GC573_EDID_SIZE,
		      "well-formed CTA data block accepted");

	/* Same block now claims 7 payload bytes but only 3 remain. */
	build_valid_edid(edid);
	edid[130] = 0x08;
	edid[132] = 0x47;	/* tag 2, length 7 */
	fix_checksum(edid + 128);
	expect_reject(edid, GC573_EDID_SIZE,
		      "overflowing CTA data-block length rejected");

	/* A zero-length block is legal and must still terminate. */
	build_valid_edid(edid);
	edid[130] = 0x05;
	edid[132] = 0x00;
	fix_checksum(edid + 128);
	expect_accept(edid, GC573_EDID_SIZE,
		      "zero-length CTA data block accepted");
}

int main(void)
{
	printf("=== Running GC573 EDID validation tests ===\n");
	test_lengths();
	test_block_count();
	test_header_and_tags();
	test_cta_bounds();
	test_file_loading();

	if (failures == 0) {
		printf("\n>>> All GC573 EDID tests passed successfully! <<<\n");
		return 0;
	}
	printf("\n>>> %d GC573 EDID test(s) failed! <<<\n", failures);
	return 1;
}

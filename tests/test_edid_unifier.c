/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_edid_unifier.c — Unit tests for GC573 EDID Audio-Unifier Profile Builder
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../tools/opengc573-userd/gc573-user-compat.h"
#include "../tools/opengc573-userd/gc573_user_edid.h"

/* Stub I2C & HW dependencies for standalone compilation of gc573_user_edid */
int gc573_user_i2c_write(void *bar0, unsigned int bus, u16 addr,
			 u32 subaddr, unsigned int subaddr_len,
			 const u8 *data, unsigned int data_len)
{
	(void)bar0; (void)bus; (void)addr; (void)subaddr; (void)subaddr_len;
	(void)data; (void)data_len;
	return 0;
}

int gc573_user_i2c_read(void *bar0, unsigned int bus, u16 addr,
			u32 subaddr, unsigned int subaddr_len,
			u8 *data, unsigned int data_len)
{
	(void)bar0; (void)bus; (void)addr; (void)subaddr; (void)subaddr_len;
	(void)data; (void)data_len;
	return 0;
}

int gc573_user_set_gpio(void *bar0, unsigned int pin, bool high, unsigned int delay_ms)
{
	(void)bar0; (void)pin; (void)high; (void)delay_ms;
	return 0;
}

int gc573_user_get_gpio(void *bar0, unsigned int pin, bool *high)
{
	(void)bar0; (void)pin;
	if (high)
		*high = false;
	return 0;
}

int gc573_user_it6805_select_bank(void *bar0, u8 bank)
{
	(void)bar0; (void)bank;
	return 0;
}

int gc573_user_it6805_set_bank(void *bar0, u8 bank)
{
	(void)bar0; (void)bank;
	return 0;
}

#include "../tools/opengc573-userd/gc573_user_edid.c"

static int g_failures = 0;

#define TEST_ASSERT(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
		g_failures++; \
	} else { \
		printf("  [PASS] %s\n", msg); \
	} \
} while (0)

static void test_default_edid_unification(void)
{
	u8 unified[GC573_EDID_SIZE];
	int ret;

	printf("Testing default GC573 EDID audio unification...\n");
	ret = edid_build_unified_profile(g_gc573_default_edid, unified);
	TEST_ASSERT(ret == 0, "edid_build_unified_profile on default EDID succeeds");
	TEST_ASSERT(edid_validate(unified) == 0, "unified default EDID checksums valid");

	/* Verify header is untouched */
	TEST_ASSERT(unified[0] == 0x00 && unified[7] == 0x00, "EDID magic header preserved");

	/* Verify CTA extension tag */
	TEST_ASSERT(unified[128] == 0x02, "Extension block tag is CTA-861 (0x02)");

	/* Verify audio data block was inserted */
	bool found_audio = false;
	unsigned int dtd_off = unified[128 + 2];
	unsigned int data_end = 128 + dtd_off;
	for (unsigned int off = 132; off < data_end; ) {
		unsigned int tag = unified[off] >> 5;
		unsigned int len = unified[off] & 0x1f;
		if (tag == 1 /* Audio tag */) {
			found_audio = true;
			TEST_ASSERT(len == sizeof(g_gc573_edid_audio_sads),
				    "Audio block length matches unified SADs (9 bytes)");
			TEST_ASSERT(memcmp(unified + off + 1, g_gc573_edid_audio_sads,
					   sizeof(g_gc573_edid_audio_sads)) == 0,
				    "Unified SAD descriptors match LPCM/AC3/DTS");
		}
		off += 1 + len;
	}
	TEST_ASSERT(found_audio, "Audio block present in unified default EDID");
}

static void test_1440p120_edid_unification(void)
{
	u8 unified[GC573_EDID_SIZE];
	int ret;

	printf("Testing 1440p120 GC573 EDID audio unification...\n");
	ret = edid_build_unified_profile(g_gc573_edid_1440p120, unified);
	TEST_ASSERT(ret == 0, "edid_build_unified_profile on 1440p120 EDID succeeds");
	TEST_ASSERT(edid_validate(unified) == 0, "unified 1440p120 EDID checksums valid");
}

static void test_1080p240_edid_unification(void)
{
	u8 unified[GC573_EDID_SIZE];
	int ret;

	printf("Testing 1080p240 GC573 EDID audio unification...\n");
	ret = edid_build_unified_profile(g_gc573_edid_1080p240, unified);
	TEST_ASSERT(ret == 0, "edid_build_unified_profile on 1080p240 EDID succeeds");
	TEST_ASSERT(edid_validate(unified) == 0, "unified 1080p240 EDID checksums valid");
}

static void test_corrupted_edid_rejection(void)
{
	u8 bad_edid[GC573_EDID_SIZE];
	u8 unified[GC573_EDID_SIZE];

	printf("Testing invalid/corrupted EDID handling...\n");
	memcpy(bad_edid, g_gc573_default_edid, GC573_EDID_SIZE);

	/* Corrupt checksum */
	bad_edid[127] ^= 0xff;
	TEST_ASSERT(edid_validate(bad_edid) != 0, "edid_validate rejects corrupt block 0");

	/* Corrupt dtd offset */
	bad_edid[127] ^= 0xff; /* restore */
	bad_edid[128 + 2] = 2; /* invalid dtd offset (< 4) */
	TEST_ASSERT(edid_build_unified_profile(bad_edid, unified) != 0,
		    "edid_build_unified_profile rejects out-of-bounds dtd offset");
}

int main(void)
{
	printf("==================================================\n");
	printf(" GC573 EDID Audio-Unifier Test Suite\n");
	printf("==================================================\n");

	test_default_edid_unification();
	test_1440p120_edid_unification();
	test_1080p240_edid_unification();
	test_corrupted_edid_rejection();

	printf("==================================================\n");
	if (g_failures == 0) {
		printf(" ALL GC573 EDID UNIFIER TESTS PASSED!\n");
		return 0;
	} else {
		printf(" %d TESTS FAILED!\n", g_failures);
		return 1;
	}
}

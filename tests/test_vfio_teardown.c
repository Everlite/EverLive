// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_vfio_teardown.c — Bounded DMA teardown and BAR0 validation tests.
 *
 * Built twice by the tests Makefile, once with -DENGINE_GC573 and once with
 * -DENGINE_HWS, because the two engines have incompatible compat headers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

#ifdef ENGINE_HWS
#include "hws_vfio.h"
#define ENGINE_NAME		"hws"
#define VFIO_CTX		struct hws_vfio_ctx
#define VFIO_REGION		struct hws_vfio_dma_region
#define VFIO_MAX_REGIONS	HWS_VFIO_MAX_DMA_REGIONS
#define VFIO_BAR0_MAX		HWS_VFIO_BAR0_MAX_SIZE
#define vfio_cleanup		hws_vfio_cleanup
#define vfio_prepare_inherit	hws_vfio_prepare_inherit
#define vfio_validate_bar0	hws_vfio_validate_bar0_size
#else
#include "gc573_vfio.h"
#define ENGINE_NAME		"gc573"
#define VFIO_CTX		struct gc573_vfio_ctx
#define VFIO_REGION		struct gc573_vfio_dma_region
#define VFIO_MAX_REGIONS	GC573_VFIO_MAX_DMA_REGIONS
#define VFIO_BAR0_MAX		GC573_VFIO_BAR0_MAX_SIZE
#define vfio_cleanup		gc573_vfio_cleanup
#define vfio_prepare_inherit	gc573_vfio_prepare_inherit
#define vfio_validate_bar0	gc573_vfio_validate_bar0_size
#endif

#define TEST_PAGE_SIZE		4096UL

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

static void reset_ctx(VFIO_CTX *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->container_fd = -1;
	ctx->group_fd = -1;
	ctx->device_fd = -1;
	ctx->irq_efd = -1;
}

static void fill_regions(VFIO_CTX *ctx, unsigned int count,
			 unsigned int stored_count)
{
	unsigned int i;

	for (i = 0; i < count && i < VFIO_MAX_REGIONS; i++) {
		ctx->dma_regions[i].vaddr = (void *)(uintptr_t)(0x1000 + i);
		ctx->dma_regions[i].iova = 0x10000000ULL + (uint64_t)i * 0x10000;
		ctx->dma_regions[i].size = TEST_PAGE_SIZE;
	}
	/* Deliberately overstate the count to prove the loops are bounded. */
	ctx->nr_dma_regions = stored_count;
}

static bool regions_are_clear(const VFIO_CTX *ctx)
{
	unsigned int i;

	for (i = 0; i < VFIO_MAX_REGIONS; i++) {
		const VFIO_REGION *region = &ctx->dma_regions[i];

		if (region->vaddr || region->size || region->iova)
			return false;
	}
	return true;
}

static void check_teardown_result(VFIO_CTX *ctx, const char *operation)
{
	char detail[160];

	check(ctx->nr_dma_regions == 0,
	      "teardown zeroes nr_dma_regions",
	      operation);
	snprintf(detail, sizeof(detail), "%s left entries populated", operation);
	check(regions_are_clear(ctx), "teardown clears every DMA entry", detail);
}

static void test_teardown(const char *operation)
{
	VFIO_CTX ctx;

	printf("\n--- %s: %s bounded teardown ---\n", ENGINE_NAME, operation);

	/* Exactly full table. */
	reset_ctx(&ctx);
	fill_regions(&ctx, VFIO_MAX_REGIONS, VFIO_MAX_REGIONS);
	if (strcmp(operation, "prepare_inherit") == 0)
		vfio_prepare_inherit(&ctx);
	else
		vfio_cleanup(&ctx);
	check_teardown_result(&ctx, operation);

	/* Out-of-range count must be clamped, not read past the array. */
	reset_ctx(&ctx);
	fill_regions(&ctx, VFIO_MAX_REGIONS, VFIO_MAX_REGIONS + 7);
	if (strcmp(operation, "prepare_inherit") == 0)
		vfio_prepare_inherit(&ctx);
	else
		vfio_cleanup(&ctx);
	check_teardown_result(&ctx, operation);

	/* Empty table. */
	reset_ctx(&ctx);
	if (strcmp(operation, "prepare_inherit") == 0)
		vfio_prepare_inherit(&ctx);
	else
		vfio_cleanup(&ctx);
	check_teardown_result(&ctx, operation);
}

static void test_injected_unmap_failure(void)
{
	VFIO_CTX ctx;

	printf("\n--- %s: injected VFIO_IOMMU_UNMAP_DMA failure ---\n",
	       ENGINE_NAME);

	reset_ctx(&ctx);
	fill_regions(&ctx, 4, 4);

	check(setenv("EVERLIVE_TEST_FAIL_UNMAP_DMA", "1", 1) == 0,
	      "fault-injection hook enabled", "setenv failed");
	vfio_cleanup(&ctx);
	check_teardown_result(&ctx, "cleanup with injected failure");

	reset_ctx(&ctx);
	fill_regions(&ctx, 4, 4);
	vfio_prepare_inherit(&ctx);
	check_teardown_result(&ctx, "prepare_inherit with injected failure");

	unsetenv("EVERLIVE_TEST_FAIL_UNMAP_DMA");
}

static void test_bar0_validation(void)
{
	const size_t page = TEST_PAGE_SIZE;
	const size_t bar = 16UL * 1024 * 1024;
	size_t out = 0;
	int ret;

	printf("\n--- %s: BAR0 size validation ---\n", ENGINE_NAME);

	ret = vfio_validate_bar0(page, page, page, VFIO_BAR0_MAX, &out);
	check(ret == 0 && out == page,
	      "accepts a normal page-sized BAR", "unexpected rejection");

	out = 0;
	ret = vfio_validate_bar0(bar, bar, page, VFIO_BAR0_MAX, &out);
	check(ret == 0 && out == bar,
	      "accepts matching page-aligned sizes", "unexpected rejection");

	out = 0;
	ret = vfio_validate_bar0(0, bar, page, VFIO_BAR0_MAX, &out);
	check(ret == -EINVAL, "rejects zero VFIO size", "expected -EINVAL");

	out = 0;
	ret = vfio_validate_bar0(bar, 0, page, VFIO_BAR0_MAX, &out);
	check(ret == -EINVAL, "rejects zero sysfs size", "expected -EINVAL");

	out = 0;
	ret = vfio_validate_bar0(bar + 1, bar, page, VFIO_BAR0_MAX, &out);
	check(ret == -EINVAL, "rejects non-page-aligned VFIO size",
	      "expected -EINVAL");

	out = 0;
	ret = vfio_validate_bar0(bar, bar + 1, page, VFIO_BAR0_MAX, &out);
	check(ret == -EINVAL, "rejects non-page-aligned sysfs size",
	      "expected -EINVAL");

	out = 0;
	ret = vfio_validate_bar0(VFIO_BAR0_MAX, VFIO_BAR0_MAX + page, page,
				 VFIO_BAR0_MAX, &out);
	check(ret == -E2BIG, "rejects an absurdly large BAR", "expected -E2BIG");

	out = 0;
	ret = vfio_validate_bar0(bar, bar / 2, page, VFIO_BAR0_MAX, &out);
	check(ret == -EINVAL,
	      "rejects VFIO size larger than sysfs size", "expected -EINVAL");

	out = 0;
	ret = vfio_validate_bar0(bar / 2, bar, page, VFIO_BAR0_MAX, &out);
	check(ret == 0 && out == bar / 2,
	      "accepts a VFIO window smaller than sysfs", "unexpected rejection");

	out = 0;
	ret = vfio_validate_bar0(bar, bar, page, VFIO_BAR0_MAX, NULL);
	check(ret == -EINVAL, "rejects a NULL output pointer", "expected -EINVAL");

	out = 0;
	ret = vfio_validate_bar0(bar, bar, 0, VFIO_BAR0_MAX, &out);
	check(ret == -EINVAL, "rejects a zero page size", "expected -EINVAL");
}

int main(void)
{
	printf("=== %s VFIO teardown/BAR0 tests ===\n", ENGINE_NAME);

	test_teardown("cleanup");
	test_teardown("prepare_inherit");
	test_injected_unmap_failure();
	test_bar0_validation();

	if (failures) {
		printf("\n%s: %d failure(s)\n", ENGINE_NAME, failures);
		return 1;
	}
	printf("\n%s: all tests passed\n", ENGINE_NAME);
	return 0;
}

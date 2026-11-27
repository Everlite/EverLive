// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_osd_utf8.c — Bounded UTF-8 stepping for the OSD glyph walker.
 *
 * Truncated multi-byte sequences used to make the width/draw loops copy and
 * advance past the terminating NUL.  Run this under ASan (`make asan`) to
 * prove the decoder never reads outside the string.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "everlive_utf8.h"

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

static void expect_step(const char *text, size_t expected, const char *name)
{
	check(everlive_utf8_next(text) == expected, name,
	      "unexpected sequence length");
}

/* Walking with the helper must consume exactly strlen() bytes and stop at NUL
 * without ever advancing past it. */
static void expect_walk(const char *text, const char *name)
{
	const char *p = text;
	size_t steps = 0;

	while (*p) {
		size_t n = everlive_utf8_next(p);

		if (n == 0) {
			check(false, name, "decoder returned 0 before NUL");
			return;
		}
		if (n > strlen(p)) {
			check(false, name, "decoder stepped past the terminator");
			return;
		}
		p += n;
		steps++;
		if (steps > strlen(text) + 1) {
			check(false, name, "decoder failed to terminate");
			return;
		}
	}
	check(p == text + strlen(text), name,
	      "walker did not land on the terminating NUL");
}

int main(void)
{
	printf("=== Running OSD UTF-8 decoder tests ===\n");

	printf("\n--- Test: single-byte and end of string ---\n");
	expect_step("", 0, "empty string returns 0");
	expect_step("A", 1, "ASCII returns 1");
	expect_step("\x7f", 1, "ASCII DEL returns 1");

	printf("\n--- Test: valid multi-byte sequences ---\n");
	expect_step("\xC3\xA9", 2, "2-byte e-acute returns 2");
	expect_step("\xE2\x80\xA6", 3, "3-byte ellipsis returns 3");
	expect_step("\xF0\x9F\x98\x80", 4, "4-byte emoji returns 4");

	printf("\n--- Test: truncated sequences fall back to one byte ---\n");
	expect_step("\xC3", 1, "truncated 2-byte lead returns 1");
	expect_step("\xE2\x80", 1, "truncated 3-byte lead returns 1");
	expect_step("\xF0\x9F\x98", 1, "truncated 4-byte lead returns 1");

	printf("\n--- Test: invalid continuations fall back to one byte ---\n");
	expect_step("\xC3\x41", 1, "2-byte lead + ASCII returns 1");
	expect_step("\xE2\x41\xA6", 1, "3-byte lead + ASCII returns 1");
	expect_step("\xF0\x9F\x41\x80", 1, "4-byte lead + ASCII returns 1");
	expect_step("\x80", 1, "stray continuation byte returns 1");
	expect_step("\xF8\x80\x80\x80", 1, "invalid 5-byte lead returns 1");

	printf("\n--- Test: full walk never passes the terminator ---\n");
	expect_walk("", "empty walk");
	expect_walk("EverLive", "ASCII walk");
	expect_walk("\xC3", "truncated 2-byte walk");
	expect_walk("\xE2\x80", "truncated 3-byte walk");
	expect_walk("\xF0\x9F\x98", "truncated 4-byte walk");
	expect_walk("\xC3\xA9\xE2\x80\xA6\xF0\x9F\x98\x80",
		    "mixed valid walk");
	expect_walk("A\xC3" "B\xE2\x80" "C\xF0\x9F\x98",
		    "truncated sequences mid-string walk");

	if (failures == 0) {
		printf("\n>>> All OSD UTF-8 tests passed successfully! <<<\n");
		return 0;
	}
	printf("\n>>> %d OSD UTF-8 test(s) failed! <<<\n", failures);
	return 1;
}

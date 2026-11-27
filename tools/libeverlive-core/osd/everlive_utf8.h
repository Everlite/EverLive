/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Bounded UTF-8 stepping shared by the OSD renderer and its unit tests.
 *
 * The OSD walks user-supplied strings one glyph at a time.  A naive
 * first-byte-only decoder can return a sequence length that runs past the
 * terminating NUL when the string is truncated ("\xC3", "\xE2\x80", ...),
 * which makes the following memcpy()/pointer increment read out of bounds.
 * everlive_utf8_next() verifies every expected continuation byte first and
 * falls back to a single byte for truncated or malformed input.
 */

#ifndef EVERLIVE_UTF8_H
#define EVERLIVE_UTF8_H

#include <stddef.h>

/*
 * everlive_utf8_next - Length of the UTF-8 sequence starting at @p
 *
 * Returns 0 at the end of the string.  Invalid, overlong or truncated
 * sequences (including a NUL before the expected continuation bytes)
 * consume exactly one byte so callers can never step past the terminator.
 */
static inline size_t everlive_utf8_next(const char *p)
{
	const unsigned char *s = (const unsigned char *)p;
	size_t n;
	size_t i;

	if (!s || s[0] == '\0')
		return 0;
	if (s[0] < 0x80)
		return 1;
	if ((s[0] & 0xe0) == 0xc0)
		n = 2;
	else if ((s[0] & 0xf0) == 0xe0)
		n = 3;
	else if ((s[0] & 0xf8) == 0xf0)
		n = 4;
	else
		return 1;	/* stray continuation byte or invalid lead */

	for (i = 1; i < n; i++) {
		if (s[i] == '\0' || (s[i] & 0xc0) != 0x80)
			return 1;
	}

	return n;
}

#endif /* EVERLIVE_UTF8_H */

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC573_USER_LED_H
#define GC573_USER_LED_H

#include <stdint.h>
#include <stdbool.h>

#define RAINBOW_SATURATION	150U	/* 0..255, ~59 % */
#define RAINBOW_VALUE_MIN	150U	/* 0..255, ~59 % */
#define RAINBOW_VALUE_MAX	205U	/* 0..255, ~80 % */

/*
 * HSV -> RGB, integer only.
 *
 * @hue is a 0..1535 circle (6 sectors * 256), @sat and @val are 0..255.
 * Every intermediate fits in 32 bits: three multiplies, three divides and a
 * switch.
 */
static inline void hsv_to_rgb(uint32_t hue, uint8_t sat, uint8_t val,
			      uint8_t *r, uint8_t *g, uint8_t *b)
{
	uint32_t region = (hue >> 8) % 6U;
	uint32_t rem = hue & 0xffU;
	uint8_t p = (uint8_t)((val * (255U - sat)) / 255U);
	uint8_t q = (uint8_t)((val * (255U - (sat * rem) / 255U)) / 255U);
	uint8_t t = (uint8_t)((val *
			      (255U - (sat * (255U - rem)) / 255U)) / 255U);

	switch (region) {
	case 0: *r = val; *g = t;   *b = p;   break;
	case 1: *r = q;   *g = val; *b = p;   break;
	case 2: *r = p;   *g = val; *b = t;   break;
	case 3: *r = p;   *g = q;   *b = val; break;
	case 4: *r = t;   *g = p;   *b = val; break;
	default:*r = val; *g = p;   *b = q;   break;
	}
}

/*
 * Advance the cycle by @elapsed_ms and return the next RGB triplet.
 *
 * The phase comes from a monotonic clock, so scheduling jitter in the 50 ms
 * loop never accumulates: one revolution always takes @cycle_ms.
 *
 * Brightness follows a slow triangle wave (one gentle breath per half
 * revolution) between RAINBOW_VALUE_MIN and RAINBOW_VALUE_MAX.
 */
static inline void rainbow_tick(uint64_t elapsed_ms, uint32_t cycle_ms,
				uint8_t *r, uint8_t *g, uint8_t *b)
{
	uint32_t pos = cycle_ms ? (uint32_t)(elapsed_ms % cycle_ms) : 0U;
	uint32_t hue = (uint32_t)(((uint64_t)pos * 1536U) / cycle_ms);
	uint32_t half = cycle_ms / 2U;
	uint32_t level, breath, val;

	if (!half)
		half = 1U;

	level = (uint32_t)(((uint64_t)(pos % half) * 255U) / half);
	breath = (level <= 127U) ? level * 2U : (255U - level) * 2U;
	val = RAINBOW_VALUE_MIN +
	      ((RAINBOW_VALUE_MAX - RAINBOW_VALUE_MIN) * breath) / 255U;

	hsv_to_rgb(hue, RAINBOW_SATURATION, (uint8_t)val, r, g, b);
}

/*
 * Calculate 5 phase-shifted RGB colors across the card zones for the multi-zone Wave effect.
 */
static inline void wave_tick(uint64_t elapsed_ms, uint32_t cycle_ms, uint32_t colors[5])
{
	for (int i = 0; i < 5; i++) {
		uint64_t zone_elapsed = elapsed_ms + ((uint64_t)i * cycle_ms) / 5U;
		uint8_t r, g, b;
		rainbow_tick(zone_elapsed, cycle_ms, &r, &g, &b);
		colors[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
	}
}

#endif /* GC573_USER_LED_H */

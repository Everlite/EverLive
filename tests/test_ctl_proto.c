/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_ctl_proto.c — Unit tests for the everlive-ctl request helpers.
 *
 * These exercise the production serialization/validation code directly, so a
 * change in everlive_ctl_proto.c cannot silently drift away from the tests.
 */

#include "everlive_ctl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_PASS(desc) printf("  [PASS] %s\n", (desc))

static void test_build_request(void)
{
	char req[512];
	char tiny[8];

	printf("Testing everlive_ctl_build_request...\n");

	assert(everlive_ctl_build_request("status.get", "{}", req,
					  sizeof(req)) == 0);
	assert(strcmp(req,
		      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"status.get\","
		      "\"params\":{}}\n") == 0);
	TEST_PASS("status.get request is well formed");

	assert(everlive_ctl_build_request("hotplug", "{\"pulse\":true}", req,
					  sizeof(req)) == 0);
	assert(strstr(req, "\"method\":\"hotplug\"") != NULL);
	assert(strstr(req, "\"params\":{\"pulse\":true}") != NULL);
	TEST_PASS("hotplug request preserves params");

	/* NULL / empty params become an empty JSON object. */
	assert(everlive_ctl_build_request("status.get", NULL, req,
					  sizeof(req)) == 0);
	assert(strstr(req, "\"params\":{}") != NULL);
	TEST_PASS("NULL params default to {}");

	/* Truncation must be reported, not silently accepted. */
	assert(everlive_ctl_build_request("status.get", "{}", tiny,
					  sizeof(tiny)) == -1);
	TEST_PASS("Too-small buffer is rejected");
}

static void test_parse_double(void)
{
	double v;

	printf("Testing everlive_ctl_parse_double...\n");

	assert(everlive_ctl_parse_double("85", &v) == 0 && v == 85.0);
	assert(everlive_ctl_parse_double("-0.5", &v) == 0 && v == -0.5);
	assert(everlive_ctl_parse_double("1e3", &v) == 0 && v == 1000.0);
	TEST_PASS("Valid decimal numbers accepted");

	assert(everlive_ctl_parse_double("nan", &v) == -1);
	assert(everlive_ctl_parse_double("inf", &v) == -1);
	assert(everlive_ctl_parse_double("-inf", &v) == -1);
	assert(everlive_ctl_parse_double("1e999", &v) == -1);
	TEST_PASS("NaN, infinity and overflow rejected");

	assert(everlive_ctl_parse_double("abc", &v) == -1);
	assert(everlive_ctl_parse_double("12abc", &v) == -1);
	assert(everlive_ctl_parse_double("\"; rm -rf /", &v) == -1);
	assert(everlive_ctl_parse_double("", &v) == -1);
	assert(everlive_ctl_parse_double(NULL, &v) == -1);
	TEST_PASS("Garbage and embedded JSON rejected");
}

static void test_parse_hex_rgb(void)
{
	unsigned long rgb;

	printf("Testing everlive_ctl_parse_hex_rgb...\n");

	assert(everlive_ctl_parse_hex_rgb("FF8020", &rgb) == 0);
	assert(rgb == 0xff8020UL);
	assert(everlive_ctl_parse_hex_rgb("ff8020", &rgb) == 0);
	assert(rgb == 0xff8020UL);
	assert(everlive_ctl_parse_hex_rgb("0", &rgb) == 0 && rgb == 0);
	TEST_PASS("Valid RRGGBB values accepted");

	assert(everlive_ctl_parse_hex_rgb("GGGGGG", &rgb) == -1);
	assert(everlive_ctl_parse_hex_rgb("1000000", &rgb) == -1);
	assert(everlive_ctl_parse_hex_rgb("-1", &rgb) == -1);
	assert(everlive_ctl_parse_hex_rgb("", &rgb) == -1);
	assert(everlive_ctl_parse_hex_rgb(NULL, &rgb) == -1);
	TEST_PASS("Garbage, overflow and negative values rejected");
}

static void test_json_escape(void)
{
	char out[128];
	char tiny[8];

	printf("Testing everlive_ctl_json_escape...\n");

	everlive_ctl_json_escape("plain", out, sizeof(out));
	assert(strcmp(out, "plain") == 0);

	everlive_ctl_json_escape("a\"b\\c\nd\te", out, sizeof(out));
	assert(strcmp(out, "a\\\"b\\\\c\\nd\\te") == 0);
	TEST_PASS("quotes, backslashes and whitespace escaped");

	everlive_ctl_json_escape("\x01", out, sizeof(out));
	assert(strcmp(out, "\\u0001") == 0);
	TEST_PASS("control bytes escaped as \\uXXXX");

	everlive_ctl_json_escape("abcdefghijklmnop", tiny, sizeof(tiny));
	assert(memchr(tiny, '\0', sizeof(tiny)) != NULL);
	TEST_PASS("Truncated output stays NUL-terminated");
}

static void test_response_has_error(void)
{
	printf("Testing everlive_ctl_response_has_error...\n");

	assert(everlive_ctl_response_has_error(
		"{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-1}}"));
	assert(everlive_ctl_response_has_error("{ \"error\" : {} }"));
	assert(everlive_ctl_response_has_error("{\"error\":null}"));

	/* The word "error" inside a value must not count as a failure. */
	assert(!everlive_ctl_response_has_error(
		"{\"result\":{\"message\":\"No error detected\"}}"));
	assert(!everlive_ctl_response_has_error(
		"{\"result\":{\"status\":\"error\"}}"));
	assert(!everlive_ctl_response_has_error("{\"result\":{}}"));
	assert(!everlive_ctl_response_has_error(NULL));
	TEST_PASS("Only an \"error\" key is treated as a failure");
}

static void test_command_payloads(void)
{
	char params[256];
	char req[512];
	double ev;
	double percent;
	unsigned long rgb;

	printf("Testing numeric command payloads...\n");

	/* hdr-ev */
	assert(everlive_ctl_parse_double("-0.5", &ev) == 0);
	assert(everlive_ctl_format_exposure(ev, params, sizeof(params)) == 0);
	assert(strcmp(params, "{\"exposure\":-0.5}") == 0);
	assert(everlive_ctl_build_request("hdr.exposure", params, req,
					  sizeof(req)) == 0);
	assert(strstr(req, "\"exposure\":-0.5") != NULL);
	TEST_PASS("hdr-ev payload uses the parsed value");

	/* volume: 85% -> 0.85 gain */
	assert(everlive_ctl_parse_double("85", &percent) == 0);
	assert(everlive_ctl_format_volume(percent, params,
					  sizeof(params)) == 0);
	assert(strcmp(params, "{\"gain\":0.85}") == 0);
	assert(everlive_ctl_format_volume(150.0, params, sizeof(params)) == 0);
	assert(everlive_ctl_format_volume(-1.0, params, sizeof(params)) == -1);
	assert(everlive_ctl_format_volume(151.0, params, sizeof(params)) == -1);
	TEST_PASS("volume gain is scaled and range-checked");

	/* led: FF8020 -> 255,128,32 */
	assert(everlive_ctl_parse_hex_rgb("FF8020", &rgb) == 0);
	assert(everlive_ctl_format_led(rgb, params, sizeof(params)) == 0);
	assert(strcmp(params, "{\"r\":255,\"g\":128,\"b\":32}") == 0);
	TEST_PASS("led payload decomposes RRGGBB correctly");
}

int main(void)
{
	printf("==================================================\n");
	printf(" everlive-ctl Protocol & Validation Tests          \n");
	printf("==================================================\n");

	test_build_request();
	test_parse_double();
	test_parse_hex_rgb();
	test_json_escape();
	test_response_has_error();
	test_command_payloads();

	printf("==================================================\n");
	printf(" ALL EVERLIVE-CTL PROTOCOL TESTS PASSED!\n");
	printf("==================================================\n");
	return 0;
}

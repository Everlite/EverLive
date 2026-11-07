/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_ipc_json.c — Unit tests for GC573 Hand-rolled JSON Scanner and RPC Formatter
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include "../tools/opengc573-userd/gc573-user-compat.h"
#include "../tools/opengc573-userd/gc573_user_ipc.c"

static int g_failures = 0;

#define TEST_ASSERT(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
		g_failures++; \
	} else { \
		printf("  [PASS] %s\n", msg); \
	} \
} while (0)

static void test_json_get_string(void)
{
	char out[64];
	const char *json = "{\"jsonrpc\":\"2.0\",\"method\":\"led.set\",\"params\":{\"mode\":\"wave\"}}";

	printf("Testing json_get_string...\n");
	TEST_ASSERT(json_get_string(json, "method", out, sizeof(out)) && strcmp(out, "led.set") == 0,
		    "Extracts top-level string 'method'");
	TEST_ASSERT(json_get_string(json, "mode", out, sizeof(out)) && strcmp(out, "wave") == 0,
		    "Extracts nested string 'mode'");
	TEST_ASSERT(!json_get_string(json, "missing", out, sizeof(out)),
		    "Fails cleanly on missing key");
}

static void test_json_get_long(void)
{
	long val = 0;
	const char *json = "{\"id\":42,\"code\":-32700,\"zero\":0}";

	printf("Testing json_get_long...\n");
	TEST_ASSERT(json_get_long(json, "id", &val) && val == 42,
		    "Extracts positive integer 'id': 42");
	TEST_ASSERT(json_get_long(json, "code", &val) && val == -32700,
		    "Extracts negative integer 'code': -32700");
	TEST_ASSERT(json_get_long(json, "zero", &val) && val == 0,
		    "Extracts zero integer: 0");
	TEST_ASSERT(!json_get_long(json, "nonexistent", &val),
		    "Fails cleanly on non-existent key");
}

static void test_json_get_bool(void)
{
	bool val = false;
	const char *json = "{\"active\":true,\"disabled\":false,\"num\":1}";

	printf("Testing json_get_bool...\n");
	TEST_ASSERT(json_get_bool(json, "active", &val) && val == true,
		    "Extracts boolean true");
	TEST_ASSERT(json_get_bool(json, "disabled", &val) && val == false,
		    "Extracts boolean false");
	TEST_ASSERT(!json_get_bool(json, "num", &val),
		    "Rejects non-boolean value");
}

static void test_json_get_double(void)
{
	double val = 0.0;
	const char *json = "{\"exposure\":-1.5,\"gain\":0.85}";

	printf("Testing json_get_double...\n");
	TEST_ASSERT(json_get_double(json, "exposure", &val) && val == -1.5,
		    "Extracts negative float 'exposure': -1.5");
	TEST_ASSERT(json_get_double(json, "gain", &val) && val > 0.84 && val < 0.86,
		    "Extracts positive float 'gain': 0.85");
	TEST_ASSERT(!json_get_double(json, "nonexistent", &val),
		    "Fails cleanly on missing float key");
}

static void test_json_escape_into(void)
{
	char dst[128];

	printf("Testing json_escape_into...\n");
	json_escape_into(dst, sizeof(dst), "Simple string");
	TEST_ASSERT(strcmp(dst, "Simple string") == 0, "Leaves plain ASCII unchanged");

	json_escape_into(dst, sizeof(dst), "Hello \"World\"\\Test\nNew\tTab");
	TEST_ASSERT(strcmp(dst, "Hello \\\"World\\\"\\\\Test\\u000aNew\\u0009Tab") == 0,
		    "Escapes quotes, backslashes, and control characters");
}

static void test_json_format_id(void)
{
	char dst[64];

	printf("Testing json_format_id...\n");
	json_format_id(dst, sizeof(dst), NULL);
	TEST_ASSERT(strcmp(dst, "null") == 0, "Formats NULL id as null");

	json_format_id(dst, sizeof(dst), "null");
	TEST_ASSERT(strcmp(dst, "null") == 0, "Formats 'null' string as null");

	json_format_id(dst, sizeof(dst), "1234");
	TEST_ASSERT(strcmp(dst, "1234") == 0, "Preserves numeric integer id 1234");

	json_format_id(dst, sizeof(dst), "-1");
	TEST_ASSERT(strcmp(dst, "-1") == 0, "Preserves negative integer id -1");

	json_format_id(dst, sizeof(dst), "\"client-req-99\"");
	TEST_ASSERT(strcmp(dst, "\"client-req-99\"") == 0, "Preserves and sanitizes quoted string id");

	json_format_id(dst, sizeof(dst), "raw-id");
	TEST_ASSERT(strcmp(dst, "\"raw-id\"") == 0, "Encapsulates unquoted string id");

	json_format_id(dst, sizeof(dst), "\"injection\n\"");
	TEST_ASSERT(strstr(dst, "\\u000a") != NULL, "Re-escapes dangerous newline injection in string id");
}

int main(void)
{
	printf("==================================================\n");
	printf(" GC573 IPC JSON Scanner Test Suite\n");
	printf("==================================================\n");

	test_json_get_string();
	test_json_get_long();
	test_json_get_double();
	test_json_get_bool();
	test_json_escape_into();
	test_json_format_id();

	printf("==================================================\n");
	if (g_failures == 0) {
		printf(" ALL GC573 IPC JSON TESTS PASSED!\n");
		return 0;
	} else {
		printf(" %d TESTS FAILED!\n", g_failures);
		return 1;
	}
}

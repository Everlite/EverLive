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
	const char *json_dup_val = "{\"title\":\"mode\",\"mode\":\"wave\"}";
	const char *json_param_method = "{\"jsonrpc\":\"2.0\",\"params\":{\"text\":\"method\"},\"method\":\"led.set\"}";

	printf("Testing json_get_string...\n");
	TEST_ASSERT(json_get_string(json, "method", out, sizeof(out)) && strcmp(out, "led.set") == 0,
		    "Extracts top-level string 'method'");
	TEST_ASSERT(json_get_string(json, "mode", out, sizeof(out)) && strcmp(out, "wave") == 0,
		    "Extracts nested string 'mode'");
	TEST_ASSERT(json_get_string(json_dup_val, "mode", out, sizeof(out)) && strcmp(out, "wave") == 0,
		    "Distinguishes object key from prior string value matching key name");
	TEST_ASSERT(json_get_string(json_param_method, "method", out, sizeof(out)) && strcmp(out, "led.set") == 0,
		    "Extracts top-level method even if params contains identical string value");
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

static void test_json_find_value(void)
{
	char out[64];
	const char *json_num = "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"ping\"}";
	const char *json_str = "{\"jsonrpc\":\"2.0\",\"id\":\"req-abc\",\"method\":\"status.get\"}";
	const char *json_null = "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"led.set\"}";

	printf("Testing json_find_value...\n");
	TEST_ASSERT(json_find_value(json_num, "method", out, sizeof(out)) && strcmp(out, "ping") == 0,
		    "Extracts method string from numeric ID request");
	TEST_ASSERT(json_find_value(json_num, "id", out, sizeof(out)) && strcmp(out, "42") == 0,
		    "Extracts numeric ID: 42");

	TEST_ASSERT(json_find_value(json_str, "method", out, sizeof(out)) && strcmp(out, "status.get") == 0,
		    "Extracts method string from string ID request");
	TEST_ASSERT(json_find_value(json_str, "id", out, sizeof(out)) && strcmp(out, "req-abc") == 0,
		    "Extracts string ID: req-abc");

	TEST_ASSERT(json_find_value(json_null, "id", out, sizeof(out)) && strcmp(out, "null") == 0,
		    "Extracts null ID: null");
	TEST_ASSERT(!json_find_value(json_num, "missing", out, sizeof(out)),
		    "Fails cleanly on missing key in json_find_value");
}

static void test_json_negative_edge_cases(void)
{
	char out[64];
	long lval = 0;
	double dval = 0.0;
	bool bval = false;
	const char *valid_json = "{\"str\":\"hello\",\"num\":123,\"flag\":true,\"flt\":3.14}";

	printf("Testing JSON negative edge cases...\n");

	/* NULL pointer handling */
	TEST_ASSERT(!json_get_string(NULL, "str", out, sizeof(out)), "json_get_string rejects NULL json");
	TEST_ASSERT(!json_get_string(valid_json, NULL, out, sizeof(out)), "json_get_string rejects NULL key");
	TEST_ASSERT(!json_get_string(valid_json, "str", NULL, sizeof(out)), "json_get_string rejects NULL output");
	TEST_ASSERT(!json_get_string(valid_json, "str", out, 0), "json_get_string rejects 0 outsz");

	TEST_ASSERT(!json_get_long(NULL, "num", &lval), "json_get_long rejects NULL json");
	TEST_ASSERT(!json_get_long(valid_json, NULL, &lval), "json_get_long rejects NULL key");
	TEST_ASSERT(!json_get_long(valid_json, "num", NULL), "json_get_long rejects NULL output");

	TEST_ASSERT(!json_get_double(NULL, "flt", &dval), "json_get_double rejects NULL json");
	TEST_ASSERT(!json_get_double(valid_json, NULL, &dval), "json_get_double rejects NULL key");
	TEST_ASSERT(!json_get_double(valid_json, "flt", NULL), "json_get_double rejects NULL output");

	TEST_ASSERT(!json_get_bool(NULL, "flag", &bval), "json_get_bool rejects NULL json");
	TEST_ASSERT(!json_get_bool(valid_json, NULL, &bval), "json_get_bool rejects NULL key");
	TEST_ASSERT(!json_get_bool(valid_json, "flag", NULL), "json_get_bool rejects NULL output");

	TEST_ASSERT(!json_find_value(NULL, "id", out, sizeof(out)), "json_find_value rejects NULL json");
	TEST_ASSERT(!json_find_value(valid_json, NULL, out, sizeof(out)), "json_find_value rejects NULL key");
	TEST_ASSERT(!json_find_value(valid_json, "id", NULL, sizeof(out)), "json_find_value rejects NULL output");
	TEST_ASSERT(!json_find_value(valid_json, "id", out, 0), "json_find_value rejects 0 outsz");

	/* Malformed JSON input */
	TEST_ASSERT(!json_get_string("", "str", out, sizeof(out)), "Rejects empty JSON string");
	TEST_ASSERT(!json_get_string("not json at all", "str", out, sizeof(out)), "Rejects non-JSON string");
	TEST_ASSERT(!json_get_string("{\"unclosed\":", "unclosed", out, sizeof(out)), "Rejects unclosed JSON");
	TEST_ASSERT(!json_get_string("[1, 2, 3]", "str", out, sizeof(out)), "Rejects array root JSON");
	TEST_ASSERT(!json_get_string("{unquoted_key: 1}", "unquoted_key", out, sizeof(out)), "Rejects unquoted keys");

	/* Type mismatches */
	TEST_ASSERT(!json_get_string(valid_json, "num", out, sizeof(out)),
		    "Type mismatch: json_get_string rejects primitive number");
	TEST_ASSERT(!json_get_long(valid_json, "str", &lval),
		    "Type mismatch: json_get_long rejects string value");
	TEST_ASSERT(!json_get_double(valid_json, "str", &dval),
		    "Type mismatch: json_get_double rejects string value");
	TEST_ASSERT(!json_get_bool(valid_json, "str", &bval),
		    "Type mismatch: json_get_bool rejects string value");
	TEST_ASSERT(!json_get_bool(valid_json, "num", &bval),
		    "Type mismatch: json_get_bool rejects number value");

	/* Destination buffer too small for string */
	char small_buf[4];
	TEST_ASSERT(!json_get_string(valid_json, "str", small_buf, sizeof(small_buf)),
		    "Safely rejects string when destination buffer is too small");

	/* json_escape_into negative edge cases */
	json_escape_into(NULL, 10, "test"); /* Should not crash */
	json_escape_into(out, 0, "test");    /* Should not crash */
	json_escape_into(out, sizeof(out), NULL);
	TEST_ASSERT(out[0] == '\0', "json_escape_into handles NULL src cleanly");

	/* json_format_id negative edge cases */
	json_format_id(NULL, 10, "id"); /* Should not crash */
	json_format_id(out, 0, "id");   /* Should not crash */
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
	test_json_find_value();
	test_json_negative_edge_cases();

	printf("==================================================\n");
	if (g_failures == 0) {
		printf(" ALL GC573 IPC JSON TESTS PASSED!\n");
		return 0;
	} else {
		printf(" %d TESTS FAILED!\n", g_failures);
		return 1;
	}
}

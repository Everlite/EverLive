/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_audio_probe.c — Unit tests for the everlive_display audio probe
 *
 * These tests cover the pure routing logic: node-id/name validation and the
 * native pw-dump parser.  They need no PipeWire, no SDL and no privileges.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "../tools/everlive/everlive_audio_probe.h"

static int g_failures = 0;

#define TEST_ASSERT(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
		g_failures++; \
	} else { \
		printf("  [PASS] %s\n", msg); \
	} \
} while (0)

/* Valid GC573 source (node 41), a line-in source that must be ignored (42),
 * an unrelated sink (40) and the EverLive playback stream (77). */
static const char *graph_fixture =
	"["
	"{\"id\":40,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
	"\"media.class\":\"Audio/Sink\","
	"\"node.name\":\"alsa_output.pci-0000_00_1f.3.analog-stereo\","
	"\"alsa.id\":\"A4K\"}}},"
	"{\"id\":41,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
	"\"media.class\":\"Audio/Source\","
	"\"node.name\":\"alsa_input.pci-0000_85_00.0.analog-stereo\","
	"\"node.description\":\"AVerMedia Live Gamer 4K Pro\","
	"\"alsa.driver_name\":\"open_gc573\","
	"\"alsa.id\":\"A4K\","
	"\"alsa.card_name\":\"AVerMedia Live Gamer 4K\","
	"\"alsa.name\":\"Digital\","
	"\"card.profile.device\":0}}},"
	"{\"id\":42,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
	"\"media.class\":\"Audio/Source\","
	"\"node.name\":\"alsa_input.pci-0000_85_00.0.pro-input-1\","
	"\"node.description\":\"AVerMedia Live Gamer 4K Pro Line In\","
	"\"alsa.driver_name\":\"open_gc573\","
	"\"alsa.id\":\"A4K\","
	"\"alsa.name\":\"Line In\","
	"\"alsa.device\":1}}},"
	"{\"id\":77,\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
	"\"media.class\":\"Stream/Output/Audio\","
	"\"node.name\":\"EverLive\","
	"\"node.description\":\"EverLive Game Audio\","
	"\"application.name\":\"EverLive\","
	"\"application.id\":\"everlive\"}}}"
	"]";

static void test_node_id(void)
{
	uint32_t id = 0;

	printf("Testing audio_parse_node_id...\n");
	TEST_ASSERT(audio_parse_node_id("1", &id) && id == 1, "Accepts 1");
	TEST_ASSERT(audio_parse_node_id("42", &id) && id == 42, "Accepts 42");
	TEST_ASSERT(audio_parse_node_id("4294967295", &id) && id == UINT32_MAX,
		    "Accepts UINT32_MAX");

	TEST_ASSERT(!audio_parse_node_id(NULL, &id), "Rejects NULL");
	TEST_ASSERT(!audio_parse_node_id("", &id), "Rejects empty");
	TEST_ASSERT(!audio_parse_node_id("0", &id), "Rejects id 0");
	TEST_ASSERT(!audio_parse_node_id("-1", &id), "Rejects negative");
	TEST_ASSERT(!audio_parse_node_id("+1", &id), "Rejects plus sign");
	TEST_ASSERT(!audio_parse_node_id(" 42", &id), "Rejects leading space");
	TEST_ASSERT(!audio_parse_node_id("42 ", &id), "Rejects trailing space");
	TEST_ASSERT(!audio_parse_node_id("12abc", &id), "Rejects suffix junk");
	TEST_ASSERT(!audio_parse_node_id("0x10", &id), "Rejects hex");
	TEST_ASSERT(!audio_parse_node_id("1.0", &id), "Rejects decimal point");
	TEST_ASSERT(!audio_parse_node_id("1; touch /tmp/pwned", &id),
		    "Rejects shell payload");
	TEST_ASSERT(!audio_parse_node_id("99999999999999999999", &id),
		    "Rejects overflow");
}

static void test_node_name(void)
{
	char long_name[AUDIO_NODE_NAME_MAX + 8];

	printf("Testing audio_validate_node_name...\n");
	TEST_ASSERT(audio_validate_node_name("alsa_input.pci-0000_85_00.0.analog-stereo"),
		    "Accepts a real ALSA node name");
	TEST_ASSERT(audio_validate_node_name("EverLive"), "Accepts EverLive");
	TEST_ASSERT(audio_validate_node_name("everlive-audio"), "Accepts everlive-audio");
	TEST_ASSERT(audio_validate_node_name("gc573-audio"), "Accepts gc573-audio");
	TEST_ASSERT(audio_validate_node_name("a_b:c.d-e+f"), "Accepts allowlisted punctuation");

	TEST_ASSERT(!audio_validate_node_name(NULL), "Rejects NULL");
	TEST_ASSERT(!audio_validate_node_name(""), "Rejects empty");
	TEST_ASSERT(!audio_validate_node_name("1; touch /tmp/pwned"),
		    "Rejects semicolon payload");
	TEST_ASSERT(!audio_validate_node_name("a\"b"), "Rejects double quote");
	TEST_ASSERT(!audio_validate_node_name("a`b"), "Rejects backtick");
	TEST_ASSERT(!audio_validate_node_name("$(id)"), "Rejects command substitution");
	TEST_ASSERT(!audio_validate_node_name("a'b"), "Rejects single quote");
	TEST_ASSERT(!audio_validate_node_name("a\nb"), "Rejects newline");
	TEST_ASSERT(!audio_validate_node_name("a b"), "Rejects whitespace");
	TEST_ASSERT(!audio_validate_node_name("a\\b"), "Rejects backslash");
	TEST_ASSERT(!audio_validate_node_name("a|b"), "Rejects pipe");
	TEST_ASSERT(!audio_validate_node_name("\xc3\xbc" "ber"), "Rejects non-ASCII");

	memset(long_name, 'a', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';
	TEST_ASSERT(!audio_validate_node_name(long_name), "Rejects overlong name");
}

static void test_volume_format(void)
{
	char buf[16];

	printf("Testing audio_format_volume...\n");
	TEST_ASSERT(audio_format_volume(1.0f, buf, sizeof(buf)) == 4 &&
		    strcmp(buf, "1.00") == 0, "Formats 1.0 as 1.00");
	TEST_ASSERT(audio_format_volume(0.0f, buf, sizeof(buf)) == 4 &&
		    strcmp(buf, "0.00") == 0, "Formats 0.0 as 0.00");
	TEST_ASSERT(audio_format_volume(0.375f, buf, sizeof(buf)) == 4 &&
		    strcmp(buf, "0.38") == 0, "Rounds 0.375 to 0.38");
	TEST_ASSERT(audio_format_volume(1.5f, buf, sizeof(buf)) == 4 &&
		    strcmp(buf, "1.50") == 0, "Formats overdrive 1.5");
	TEST_ASSERT(audio_format_volume(1.0f, buf, 3) == 0, "Rejects tiny buffer");
}

static void test_graph_probe(void)
{
	struct audio_graph graph;

	printf("Testing audio_probe_graph...\n");
	TEST_ASSERT(audio_probe_graph(graph_fixture, strlen(graph_fixture), &graph),
		    "Parses the fixture graph");
	TEST_ASSERT(graph.have_source && graph.source.id == 41 &&
		    strcmp(graph.source.name,
			   "alsa_input.pci-0000_85_00.0.analog-stereo") == 0,
		    "Finds the GC573 HDMI source");
	TEST_ASSERT(graph.have_stream && graph.stream.id == 77 &&
		    strcmp(graph.stream.name, "EverLive") == 0,
		    "Finds the EverLive stream");

	TEST_ASSERT(audio_graph_find_name_by_id(graph_fixture, strlen(graph_fixture),
					       41, graph.source.name,
					       sizeof(graph.source.name)) &&
		    strcmp(graph.source.name,
			   "alsa_input.pci-0000_85_00.0.analog-stereo") == 0,
		    "Resolves id 41 to node.name");
	TEST_ASSERT(!audio_graph_find_name_by_id(graph_fixture, strlen(graph_fixture),
						999, graph.source.name,
						sizeof(graph.source.name)),
		    "Rejects unknown id");
}

static void test_graph_negative(void)
{
	struct audio_graph graph;
	const char *line_only =
		"[{\"id\":42,\"info\":{\"props\":{"
		"\"media.class\":\"Audio/Source\","
		"\"node.name\":\"alsa_input.pci-0000_85_00.0.pro-input-1\","
		"\"alsa.driver_name\":\"open_gc573\","
		"\"alsa.name\":\"Line In\","
		"\"card.profile.device\":1}}}]";
	const char *hostile_name =
		"[{\"id\":43,\"info\":{\"props\":{"
		"\"media.class\":\"Audio/Source\","
		"\"node.name\":\"1; touch /tmp/pwned\","
		"\"alsa.driver_name\":\"open_gc573\","
		"\"card.profile.device\":0}}}]";
	const char *quoted_name =
		"[{\"id\":44,\"info\":{\"props\":{"
		"\"media.class\":\"Audio/Source\","
		"\"node.name\":\"alsa_input.\\\"; touch /tmp/pwned; \\\"\","
		"\"alsa.driver_name\":\"open_gc573\","
		"\"card.profile.device\":0}}}]";
	const char *device_string =
		"[{\"id\":45,\"info\":{\"props\":{"
		"\"media.class\":\"Audio/Source\","
		"\"node.name\":\"alsa_input.pci-0000_85_00.0.analog-stereo\","
		"\"alsa.driver_name\":\"open_gc573\","
		"\"card.profile.device\":\"0\"}}}]";
	const char *device_absent =
		"[{\"id\":46,\"info\":{\"props\":{"
		"\"media.class\":\"Audio/Source\","
		"\"node.name\":\"alsa_input.pci-0000_85_00.0.analog-stereo\","
		"\"alsa.driver_name\":\"open_gc573\"}}}]";
	const char *device_bogus =
		"[{\"id\":47,\"info\":{\"props\":{"
		"\"media.class\":\"Audio/Source\","
		"\"node.name\":\"alsa_input.pci-0000_85_00.0.analog-stereo\","
		"\"alsa.driver_name\":\"open_gc573\","
		"\"card.profile.device\":\"first\"}}}]";
	const char *device_null =
		"[{\"id\":49,\"info\":{\"props\":{"
		"\"media.class\":\"Audio/Source\","
		"\"node.name\":\"alsa_input.pci-0000_85_00.0.analog-stereo\","
		"\"alsa.driver_name\":\"open_gc573\","
		"\"card.profile.device\":null}}}]";
	const char *stream_input =
		"[{\"id\":78,\"info\":{\"props\":{"
		"\"media.class\":\"Stream/Input/Audio\","
		"\"node.name\":\"EverLive\","
		"\"application.name\":\"EverLive\"}}}]";
	const char *card_name_match =
		"[{\"id\":48,\"info\":{\"props\":{"
		"\"media.class\":\"Audio/Source\","
		"\"node.name\":\"alsa_input.pci-0000_85_00.0.analog-stereo\","
		"\"alsa.card_name\":\"AVerMedia LIVE GAMER 4K\"}}}]";
	const char *truncated =
		"[{\"id\":41,\"info\":{\"props\":{\"media.class\":\"Audio/Sou";

	printf("Testing audio_probe_graph edge cases...\n");
	TEST_ASSERT(audio_probe_graph(line_only, strlen(line_only), &graph) &&
		    !graph.have_source, "Ignores the GC573 line-in source");
	TEST_ASSERT(audio_probe_graph(hostile_name, strlen(hostile_name), &graph) &&
		    !graph.have_source, "Rejects a shell-payload node name");
	TEST_ASSERT(audio_probe_graph(quoted_name, strlen(quoted_name), &graph) &&
		    !graph.have_source, "Rejects a quote-injection node name");
	TEST_ASSERT(audio_probe_graph(device_string, strlen(device_string), &graph) &&
		    graph.have_source, "Accepts device as string \"0\"");
	TEST_ASSERT(audio_probe_graph(device_absent, strlen(device_absent), &graph) &&
		    graph.have_source, "Accepts absent device as 0");
	TEST_ASSERT(audio_probe_graph(device_bogus, strlen(device_bogus), &graph) &&
		    !graph.have_source, "Rejects a non-numeric device");
	TEST_ASSERT(audio_probe_graph(device_null, strlen(device_null), &graph) &&
		    graph.have_source, "Treats a null device as 0");
	TEST_ASSERT(audio_probe_graph(stream_input, strlen(stream_input), &graph) &&
		    !graph.have_stream, "Ignores input-side stream nodes");
	TEST_ASSERT(audio_probe_graph(card_name_match, strlen(card_name_match), &graph) &&
		    graph.have_source, "Matches case-insensitively on card name");

	TEST_ASSERT(!audio_probe_graph(truncated, strlen(truncated), &graph),
		    "Rejects truncated JSON");
	TEST_ASSERT(!audio_probe_graph("", 0, &graph), "Rejects empty input");
	TEST_ASSERT(!audio_probe_graph(graph_fixture, AUDIO_PROBE_MAX_JSON + 1, &graph),
		    "Rejects oversized input before reading it");
}

int main(void)
{
	printf("==================================================\n");
	printf(" everlive_display Audio Probe Tests               \n");
	printf("==================================================\n");

	test_node_id();
	test_node_name();
	test_volume_format();
	test_graph_probe();
	test_graph_negative();

	printf("==================================================\n");
	if (g_failures == 0) {
		printf(" ALL AUDIO PROBE TESTS PASSED!\n");
		return 0;
	}

	printf(" %d TESTS FAILED!\n", g_failures);
	return 1;
}

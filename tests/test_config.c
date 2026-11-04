// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_config.c — Unit tests for GC570D runtime configuration parser.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "gc570d-config.h"

static int failures = 0;

static void check(bool condition, const char *name, const char *detail)
{
	if (condition) {
		printf("  [PASS] %s\n", name);
	} else {
		printf("  [FAIL] %s: %s\n", name, detail ? detail : "assertion failed");
		failures++;
	}
}

static void test_config_parsing(void)
{
	printf("\n--- Test: Runtime Configuration File Parsing ---\n");

	char tmp_path[] = "/tmp/gc570d_test_conf_XXXXXX";
	int fd = mkstemp(tmp_path);
	check(fd >= 0, "Created temporary config file", "mkstemp failed");

	const char *content =
		"# GC570D Test Configuration\n"
		"; Comment style 2\n"
		"\n"
		"   width   =   2560   \n"
		"height = 1440\n"
		"hdmi1_poll_ms = 25\n"
		"hdmi2_poll_ms = 75\n"
		"enable_led = off\n"
		"enable_audio = 0\n"
		"edid_bridge = false\n";

	ssize_t written = write(fd, content, strlen(content));
	close(fd);
	check(written == (ssize_t)strlen(content), "Wrote test config content", "write failed");

	int ret = gc570d_load_config(tmp_path);
	check(ret == 0, "gc570d_load_config returned 0 (success)", "load failed");

	check(gc570d_cfg.width == 2560, "width correctly set to 2560", "width mismatch");
	check(gc570d_cfg.height == 1440, "height correctly set to 1440", "height mismatch");
	check(gc570d_cfg.hdmi1_poll_ms == 25, "hdmi1_poll_ms correctly set to 25", "hdmi1_poll_ms mismatch");
	check(gc570d_cfg.hdmi2_poll_ms == 75, "hdmi2_poll_ms correctly set to 75", "hdmi2_poll_ms mismatch");
	check(gc570d_cfg.enable_led == false, "enable_led correctly set to false ('off')", "enable_led mismatch");
	check(gc570d_cfg.enable_audio == false, "enable_audio correctly set to false ('0')", "enable_audio mismatch");
	check(gc570d_cfg.edid_bridge_mode == false, "edid_bridge_mode correctly set to false ('false')", "edid_bridge mismatch");

	unlink(tmp_path);
}

static void test_missing_config_fallback(void)
{
	printf("\n--- Test: Missing Configuration Fallback ---\n");

	/* Reset to known values */
	gc570d_cfg.width = 1920;
	gc570d_cfg.height = 1080;
	gc570d_cfg.enable_led = true;

	int ret = gc570d_load_config("/nonexistent/gc570d.conf");
	check(ret == 0, "Non-existent config file returns 0 without crashing", "error on missing file");
	check(gc570d_cfg.width == 1920 && gc570d_cfg.height == 1080,
	      "Default width/height preserved", "defaults modified");
	check(gc570d_cfg.enable_led == true,
	      "Default enable_led preserved", "defaults modified");
}

static void test_invalid_values_rejection(void)
{
	printf("\n--- Test: Invalid & Out-of-Range Value Rejection ---\n");

	/* Reset to known baseline */
	gc570d_cfg.width = 1920;
	gc570d_cfg.height = 1080;
	gc570d_cfg.hdmi1_poll_ms = 50;
	gc570d_cfg.hdmi2_poll_ms = 50;
	gc570d_cfg.enable_led = true;

	char tmp_path[] = "/tmp/gc570d_test_conf_inv_XXXXXX";
	int fd = mkstemp(tmp_path);
	check(fd >= 0, "Created temporary invalid config file", "mkstemp failed");

	const char *content =
		"# Non-numeric strings (must be rejected)\n"
		"width = auto\n"
		"height = 1080p\n"
		"\n"
		"# Out of range values (must be rejected)\n"
		"hdmi1_poll_ms = 2\n"
		"hdmi2_poll_ms = 9999\n"
		"\n"
		"# Unrecognized boolean (must be rejected)\n"
		"enable_led = unknown_state\n";

	ssize_t written = write(fd, content, strlen(content));
	close(fd);
	check(written == (ssize_t)strlen(content), "Wrote invalid config content", "write failed");

	int ret = gc570d_load_config(tmp_path);
	check(ret == 0, "gc570d_load_config returned 0 (success)", "load failed");

	check(gc570d_cfg.width == 1920,
	      "Non-numeric 'width = auto' rejected (1920 preserved)", "width corrupted");
	check(gc570d_cfg.height == 1080,
	      "Trailing unit 'height = 1080p' rejected (1080 preserved)", "height corrupted");
	check(gc570d_cfg.hdmi1_poll_ms == 50,
	      "Under-range 'hdmi1_poll_ms = 2' rejected (50 preserved)", "poll ms corrupted");
	check(gc570d_cfg.hdmi2_poll_ms == 50,
	      "Over-range 'hdmi2_poll_ms = 9999' rejected (50 preserved)", "poll ms corrupted");
	check(gc570d_cfg.enable_led == true,
	      "Invalid boolean 'enable_led = unknown_state' rejected (true preserved)", "led corrupted");

	unlink(tmp_path);
}

int main(void)
{
	printf("=== Running OpenGC570D Runtime Config Unit Tests ===\n");
	test_config_parsing();
	test_missing_config_fallback();
	test_invalid_values_rejection();

	if (failures == 0) {
		printf("\n>>> All Config tests passed successfully! <<<\n");
		return 0;
	} else {
		printf("\n>>> %d Config test(s) failed! <<<\n", failures);
		return 1;
	}
}


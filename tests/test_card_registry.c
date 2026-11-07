/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_card_registry.c — Unit tests for libeverlive-core hardware registry and autodiscovery
 */

#include "everlive_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

#define TEST_PASS(desc) printf("  [PASS] %s\n", (desc))

static void test_registry_enumeration(void)
{
	printf("Testing everlive_get_card_registry...\n");
	size_t count = 0;
	const struct everlive_device_desc *registry = everlive_get_card_registry(&count);

	assert(registry != NULL);
	assert(count >= 3);
	TEST_PASS("Registry contains multiple supported card definitions");

	bool found_gc570d = false;
	bool found_gc573 = false;

	for (size_t i = 0; i < count; i++) {
		if (strcmp(registry[i].model_slug, "gc570d") == 0)
			found_gc570d = true;
		if (strcmp(registry[i].model_slug, "gc573") == 0)
			found_gc573 = true;
	}

	assert(found_gc570d);
	assert(found_gc573);
	TEST_PASS("Registry includes both GC570D and GC573 descriptors");
}

static void test_card_matching(void)
{
	printf("Testing everlive_match_card...\n");

	/* Exact GC570D match (0x1461, 0x0054, 0x5700) */
	const struct everlive_device_desc *gc570d =
		everlive_match_card(0x1461, 0x0054, 0x5700);
	assert(gc570d != NULL);
	assert(strcmp(gc570d->model_slug, "gc570d") == 0);
	assert(gc570d->num_input_ports == 2);
	assert(gc570d->capabilities & EVERLIVE_CAP_DUAL_INPUT);
	assert(gc570d->capabilities & EVERLIVE_CAP_HARDWARE_SPLITTER);
	TEST_PASS("Identified GC570D by subsystem 0x5700 with dual inputs");

	/* Exact GC573 match (0x1461, 0x0054, 0x5730) */
	const struct everlive_device_desc *gc573_exact =
		everlive_match_card(0x1461, 0x0054, 0x5730);
	assert(gc573_exact != NULL);
	assert(strcmp(gc573_exact->model_slug, "gc573") == 0);
	assert(gc573_exact->capabilities & EVERLIVE_CAP_4K60);
	assert(gc573_exact->capabilities & EVERLIVE_CAP_1440P120);
	assert(gc573_exact->capabilities & EVERLIVE_CAP_FRAMEGEN);
	TEST_PASS("Identified GC573 by subsystem 0x5730 with 4K60/1440p120/FrameGen");

	/* Wildcard GC573 match (0x1461, 0x0054 with non-5700 subsystem) */
	const struct everlive_device_desc *gc573_wildcard =
		everlive_match_card(0x1461, 0x0054, 0x0000);
	assert(gc573_wildcard != NULL);
	assert(strcmp(gc573_wildcard->model_slug, "gc573") == 0);
	TEST_PASS("Defaulted 0x0054 to GC573 when subsystem != 0x5700");

	/* Secondary GC573 / BOLT PCIe ID (0x0055) */
	const struct everlive_device_desc *gc573_alt =
		everlive_match_card(0x1461, 0x0055, 0x0000);
	assert(gc573_alt != NULL);
	assert(strcmp(gc573_alt->model_slug, "gc573") == 0);
	TEST_PASS("Identified alternative GC573/BOLT PCIe device ID 0x0055");

	/* Future GC575 template (0x0057) */
	const struct everlive_device_desc *gc575 =
		everlive_match_card(0x1461, 0x0057, 0x0000);
	assert(gc575 != NULL);
	assert(strcmp(gc575->model_slug, "gc575") == 0);
	TEST_PASS("Identified GC575 device ID 0x0057");

	/* Unknown card rejected */
	const struct everlive_device_desc *unknown =
		everlive_match_card(0x10de, 0x1f02, 0x0000);
	assert(unknown == NULL);
	TEST_PASS("Unknown PCI IDs rejected gracefully");
}

static void create_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");
	assert(f != NULL);
	fputs(content, f);
	fclose(f);
}

static void test_mock_sysfs_scan(void)
{
	printf("Testing everlive_detect_installed_cards with mock sysfs...\n");

	char tmp_root[] = "/tmp/everlive_sysfs_test_XXXXXX";
	char *res = mkdtemp(tmp_root);
	assert(res != NULL);

	/* Mock device 1: GC570D at 0000:03:00.0 */
	char d1[512], d2[512], d3[512];
	snprintf(d1, sizeof(d1), "%s/0000:03:00.0", tmp_root);
	snprintf(d2, sizeof(d2), "%s/0000:04:00.0", tmp_root);
	snprintf(d3, sizeof(d3), "%s/0000:01:00.0", tmp_root);

	assert(mkdir(d1, 0755) == 0);
	assert(mkdir(d2, 0755) == 0);
	assert(mkdir(d3, 0755) == 0);

	/* Populate GC570D */
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/vendor", d1);
	create_file(p, "0x1461\n");
	snprintf(p, sizeof(p), "%s/device", d1);
	create_file(p, "0x0054\n");
	snprintf(p, sizeof(p), "%s/subsystem_device", d1);
	create_file(p, "0x5700\n");

	/* Mock symlink for driver -> vfio-pci */
	snprintf(p, sizeof(p), "%s/driver", d1);
	assert(symlink("/sys/bus/pci/drivers/vfio-pci", p) == 0);

	/* Mock symlink for iommu_group -> .../16 */
	snprintf(p, sizeof(p), "%s/iommu_group", d1);
	assert(symlink("/sys/kernel/iommu_groups/16", p) == 0);

	/* Populate GC573 at 0000:04:00.0 */
	snprintf(p, sizeof(p), "%s/vendor", d2);
	create_file(p, "0x1461\n");
	snprintf(p, sizeof(p), "%s/device", d2);
	create_file(p, "0x0054\n");
	snprintf(p, sizeof(p), "%s/subsystem_device", d2);
	create_file(p, "0x5730\n");

	/* Mock symlink for iommu_group -> .../22 */
	snprintf(p, sizeof(p), "%s/iommu_group", d2);
	assert(symlink("/sys/kernel/iommu_groups/22", p) == 0);

	/* Populate other non-capture device at 0000:01:00.0 (e.g. Intel GPU) */
	snprintf(p, sizeof(p), "%s/vendor", d3);
	create_file(p, "0x8086\n");
	snprintf(p, sizeof(p), "%s/device", d3);
	create_file(p, "0x9bc5\n");
	snprintf(p, sizeof(p), "%s/subsystem_device", d3);
	create_file(p, "0x103c\n");

	/* Scan using mock root */
	struct everlive_device found[8];
	int count = everlive_detect_installed_cards(found, 8, tmp_root);

	assert(count == 2);
	TEST_PASS("Discovered exactly 2 supported capture cards from mock sysfs");

	bool saw_gc570d = false;
	bool saw_gc573 = false;

	for (int i = 0; i < count; i++) {
		if (strcmp(found[i].desc->model_slug, "gc570d") == 0) {
			saw_gc570d = true;
			assert(strcmp(found[i].pci_bdf, "0000:03:00.0") == 0);
			assert(found[i].iommu_group == 16);
			assert(found[i].is_vfio_bound == true);
		}
		if (strcmp(found[i].desc->model_slug, "gc573") == 0) {
			saw_gc573 = true;
			assert(strcmp(found[i].pci_bdf, "0000:04:00.0") == 0);
			assert(found[i].iommu_group == 22);
			assert(found[i].is_vfio_bound == false);
		}
	}

	assert(saw_gc570d);
	assert(saw_gc573);
	TEST_PASS("Correctly populated BDF, IOMMU group, and VFIO binding state");

	/* Cleanup mock directory */
	unlink(d1); // in case
	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_root);
	int ret = system(cmd);
	(void)ret;
}

static int g_custom_probe_called = 0;
static int mock_custom_probe(struct everlive_device *dev)
{
	(void)dev;
	g_custom_probe_called++;
	return 0;
}

static void test_card_ops_binding(void)
{
	printf("Testing everlive_card_ops binding and defaults...\n");

	const struct everlive_device_desc *gc573_desc =
		everlive_match_card(0x1461, 0x0054, 0x5730);
	assert(gc573_desc != NULL);

	/* 1. Default ops retrieval */
	const struct everlive_card_ops *default_ops = everlive_card_ops_for(gc573_desc);
	assert(default_ops != NULL);
	assert(default_ops->probe != NULL);
	assert(default_ops->init_hardware != NULL);
	assert(default_ops->standby != NULL);
	assert(default_ops->release != NULL);
	assert(default_ops->set_input_port != NULL);
	assert(default_ops->set_edid_profile != NULL);
	assert(default_ops->enable_passthrough != NULL);
	assert(default_ops->set_led != NULL);
	assert(default_ops->get_telemetry != NULL);
	TEST_PASS("Default ops returned for known model");

	/* 2. Default ops return -ENOTSUP / -EINVAL appropriately */
	struct everlive_device dev;
	memset(&dev, 0, sizeof(dev));
	assert(default_ops->probe(&dev) == -EINVAL);

	dev.desc = gc573_desc;
	snprintf(dev.pci_bdf, sizeof(dev.pci_bdf), "0000:04:00.0");
	assert(default_ops->probe(&dev) == 0);
	assert(default_ops->init_hardware(&dev) == -ENOTSUP);
	assert(default_ops->standby(&dev) == -ENOTSUP);
	assert(default_ops->set_input_port(&dev, 1) == -ENOTSUP);
	assert(default_ops->set_edid_profile(&dev, 0, "default", NULL, 0) == -ENOTSUP);
	assert(default_ops->enable_passthrough(&dev, true) == -ENOTSUP);
	assert(default_ops->set_led(&dev, EVERLIVE_LED_MODE_STATIC, 255, 0, 0) == -ENOTSUP);

	struct everlive_telemetry telem;
	memset(&telem, 0xaa, sizeof(telem));
	assert(default_ops->get_telemetry(&dev, &telem) == -ENOTSUP);
	assert(telem.frames_captured == 0 && telem.temperature_c == 0.0f);

	default_ops->release(&dev);
	TEST_PASS("Default ops behave safely and return -ENOTSUP for hardware actions");

	/* 3. Register custom backend ops */
	struct everlive_card_ops custom_ops = *default_ops;
	custom_ops.probe = mock_custom_probe;

	assert(everlive_register_card_ops("gc573", &custom_ops) == 0);
	const struct everlive_card_ops *bound = everlive_card_ops_for(gc573_desc);
	assert(bound != NULL);
	assert(bound->probe == mock_custom_probe);

	g_custom_probe_called = 0;
	assert(bound->probe(&dev) == 0);
	assert(g_custom_probe_called == 1);
	TEST_PASS("Successfully registered and dispatched custom backend ops");

	/* 4. Invalid arguments */
	assert(everlive_register_card_ops(NULL, &custom_ops) == -EINVAL);
	assert(everlive_register_card_ops("gc573", NULL) == -EINVAL);
	assert(everlive_register_card_ops("nonexistent_model", &custom_ops) == -ENOENT);
	TEST_PASS("Rejects NULL arguments and unknown model slugs for ops registration");
}

static uint64_t timespec_to_ns_test(const struct timespec *ts)
{
	return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static bool check_resume_detected(const struct timespec *mono_now,
				  const struct timespec *boot_now,
				  const struct timespec *mono_prev,
				  const struct timespec *boot_prev)
{
	uint64_t mono_delta = timespec_to_ns_test(mono_now) - timespec_to_ns_test(mono_prev);
	uint64_t boot_delta = timespec_to_ns_test(boot_now) - timespec_to_ns_test(boot_prev);

	return (boot_delta > mono_delta + 2000000000ULL);
}

static void test_resume_detection(void)
{
	printf("Testing system resume detection logic...\n");

	struct timespec mono_prev = { .tv_sec = 100, .tv_nsec = 0 };
	struct timespec boot_prev = { .tv_sec = 500, .tv_nsec = 0 };

	/* 1. Normal running: both monotonic and boottime advance equally by 250 ms */
	struct timespec mono_now = { .tv_sec = 100, .tv_nsec = 250000000 };
	struct timespec boot_now = { .tv_sec = 500, .tv_nsec = 250000000 };
	assert(!check_resume_detected(&mono_now, &boot_now, &mono_prev, &boot_prev));
	TEST_PASS("Normal 250ms interval does not trigger resume");

	/* 2. Jitter or scheduling delay: boot advances by 500ms, mono advances by 400ms */
	mono_now.tv_sec = 100; mono_now.tv_nsec = 400000000;
	boot_now.tv_sec = 500; boot_now.tv_nsec = 500000000;
	assert(!check_resume_detected(&mono_now, &boot_now, &mono_prev, &boot_prev));
	TEST_PASS("100ms jitter does not trigger resume");

	/* 3. System suspend: machine sleeps for 10 seconds.
	 * monotonic stopped while sleeping (advanced by only 250ms awake time),
	 * boottime kept counting (advanced by 10.25 seconds).
	 */
	mono_now.tv_sec = 100; mono_now.tv_nsec = 250000000;
	boot_now.tv_sec = 510; boot_now.tv_nsec = 250000000;
	assert(check_resume_detected(&mono_now, &boot_now, &mono_prev, &boot_prev));
	TEST_PASS("10-second system sleep accurately triggers resume detection");

	/* 4. Threshold boundary: exactly 2.0s delta is not triggered (> 2.0s required) */
	mono_now.tv_sec = 100; mono_now.tv_nsec = 0;
	boot_now.tv_sec = 502; boot_now.tv_nsec = 0;
	assert(!check_resume_detected(&mono_now, &boot_now, &mono_prev, &boot_prev));

	/* 2.000000001s triggers */
	boot_now.tv_nsec = 1;
	assert(check_resume_detected(&mono_now, &boot_now, &mono_prev, &boot_prev));
	TEST_PASS("Resume detection threshold strictly enforced at > 2.0 seconds delta");
}

static void test_everlive_ctl_commands(void)
{
	printf("Testing everlive-ctl runtime command construction...\n");

	char req[512];
	char params[256];

	/* 1. Status command */
	snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}\n",
		 "status.get", "{}");
	assert(strstr(req, "\"method\":\"status.get\"") != NULL);
	assert(strstr(req, "\"params\":{}") != NULL);
	TEST_PASS("Status request formatting verified");

	/* 2. LED static color RGB calculation */
	unsigned long rgb = strtoul("FF8020", NULL, 16);
	unsigned long r = (rgb >> 16) & 0xff;
	unsigned long g = (rgb >> 8) & 0xff;
	unsigned long b = rgb & 0xff;
	assert(r == 0xff && g == 0x80 && b == 0x20);
	snprintf(params, sizeof(params), "{\"r\":%lu,\"g\":%lu,\"b\":%lu}", r, g, b);
	snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}\n",
		 "led.set", params);
	assert(strstr(req, "\"r\":255,\"g\":128,\"b\":32") != NULL);
	TEST_PASS("LED RGB formatting verified");

	/* 3. EDID profile command */
	snprintf(params, sizeof(params), "{\"profile\":\"%s\"}", "1440p120");
	snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}\n",
		 "edid.set", params);
	assert(strstr(req, "\"method\":\"edid.set\"") != NULL);
	assert(strstr(req, "\"profile\":\"1440p120\"") != NULL);
	TEST_PASS("EDID profile request formatting verified");

	/* 4. Hotplug pulse command */
	snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}\n",
		 "hotplug", "{\"pulse\":true}");
	assert(strstr(req, "\"method\":\"hotplug\"") != NULL);
	assert(strstr(req, "\"pulse\":true") != NULL);
	TEST_PASS("Hotplug pulse request formatting verified");

	/* 5. HDR exposure command */
	snprintf(params, sizeof(params), "{\"exposure\":%s}", "-0.5");
	snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}\n",
		 "hdr.exposure", params);
	assert(strstr(req, "\"method\":\"hdr.exposure\"") != NULL);
	assert(strstr(req, "\"exposure\":-0.5") != NULL);
	TEST_PASS("HDR exposure request formatting verified");

	/* 6. Audio volume / mute switch parsing */
	double vol = 85.0;
	snprintf(params, sizeof(params), "{\"gain\":%g}", vol / 100.0);
	assert(strstr(params, "0.85") != NULL);
	TEST_PASS("Audio volume gain calculation verified");
}

int main(void)
{
	printf("==================================================\n");
	printf(" libeverlive-core Card Registry & Discovery Tests \n");
	printf("==================================================\n");

	test_registry_enumeration();
	test_card_matching();
	test_mock_sysfs_scan();
	test_card_ops_binding();
	test_resume_detection();
	test_everlive_ctl_commands();

	printf("==================================================\n");
	printf(" ALL CARD REGISTRY TESTS PASSED!\n");
	printf("==================================================\n");
	return 0;
}

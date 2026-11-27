/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit tests for everlive_doctor (mocked sysfs/dev/shm/path prerequisites).
 */

#include "everlive_doctor.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void mkdir_p(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	for (size_t i = 1; i < len; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			(void)mkdir(tmp, 0755);
			tmp[i] = '/';
		}
	}
	(void)mkdir(tmp, mode);
	(void)chmod(tmp, mode);
}

static void write_str(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");

	assert(f != NULL);
	fputs(s, f);
	fclose(f);
}

static void make_exec(const char *path)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0755);

	assert(fd >= 0);
	close(fd);
}

static const struct everlive_doctor_check *
find_check(const struct everlive_doctor_report *r, const char *id)
{
	for (size_t i = 0; i < r->count; i++) {
		if (strcmp(r->checks[i].id, id) == 0)
			return &r->checks[i];
	}
	return NULL;
}

static void rm_rf(const char *path)
{
	char cmd[PATH_MAX + 32];

	snprintf(cmd, sizeof(cmd), "rm -rf -- '%s'", path);
	(void)system(cmd);
}

int main(void)
{
	char root[] = "/tmp/everlive_doctor_test_XXXXXX";
	char sys_root[PATH_MAX], dev_root[PATH_MAX], shm_root[PATH_MAX];
	char bin_dir[PATH_MAX], app_dir[PATH_MAX], run_dir[PATH_MAX];
	char p[PATH_MAX], target[PATH_MAX];
	struct everlive_doctor_env env;
	struct everlive_doctor_report report;
	const struct everlive_doctor_check *chk;
	int rc;

	assert(mkdtemp(root) != NULL);

	snprintf(sys_root, sizeof(sys_root), "%s/sys", root);
	snprintf(dev_root, sizeof(dev_root), "%s/dev", root);
	snprintf(shm_root, sizeof(shm_root), "%s/shm", root);
	snprintf(bin_dir, sizeof(bin_dir), "%s/bin", root);
	snprintf(app_dir, sizeof(app_dir), "%s/app", root);
	snprintf(run_dir, sizeof(run_dir), "%s/run", root);

	/* 1. Build a completely healthy mocked environment */
	snprintf(p, sizeof(p), "%s/kernel/iommu_groups/25", sys_root);
	mkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/bus/pci/drivers/vfio-pci", sys_root);
	mkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/module/v4l2loopback", sys_root);
	mkdir_p(p, 0755);

	snprintf(p, sizeof(p), "%s/bus/pci/devices/0000:0a:00.0", sys_root);
	mkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/bus/pci/devices/0000:0a:00.0/vendor", sys_root);
	write_str(p, "0x1461\n");
	snprintf(p, sizeof(p), "%s/bus/pci/devices/0000:0a:00.0/device", sys_root);
	write_str(p, "0x0054\n");
	snprintf(p, sizeof(p), "%s/bus/pci/devices/0000:0a:00.0/subsystem_device", sys_root);
	write_str(p, "0x0000\n");

	snprintf(p, sizeof(p), "%s/bus/pci/devices/0000:0a:00.0/driver", sys_root);
	snprintf(target, sizeof(target), "%s/bus/pci/drivers/vfio-pci", sys_root);
	assert(symlink(target, p) == 0);

	snprintf(p, sizeof(p), "%s/bus/pci/devices/0000:0a:00.0/iommu_group", sys_root);
	snprintf(target, sizeof(target), "%s/kernel/iommu_groups/25", sys_root);
	assert(symlink(target, p) == 0);

	snprintf(p, sizeof(p), "%s/vfio", dev_root);
	mkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/vfio/25", dev_root);
	{
		int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0660);
		assert(fd >= 0);
		close(fd);
		assert(chmod(p, 0660) == 0);
	}
	snprintf(p, sizeof(p), "%s/video10", dev_root);
	make_exec(p);

	mkdir_p(shm_root, 0755);
	mkdir_p(bin_dir, 0755);
	snprintf(p, sizeof(p), "%s/ffmpeg", bin_dir);
	make_exec(p);
	snprintf(p, sizeof(p), "%s/wpctl", bin_dir);
	make_exec(p);
	snprintf(p, sizeof(p), "%s/tools/opengc573-userd", app_dir);
	mkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/tools/opengc573-userd/gc573_user_streamer", app_dir);
	make_exec(p);
	mkdir_p(run_dir, 0700);

	everlive_doctor_env_defaults(&env);
	env.sys_root = sys_root;
	env.dev_root = dev_root;
	env.shm_root = shm_root;
	env.path_env = bin_dir;
	env.app_libdir = app_dir;
	env.runtime_dir = run_dir;
	env.use_mock_uid = true;
	env.euid = geteuid();
	env.mock_in_video_group = 1;
	env.mock_memlock_bytes = 1024LL * 1024LL * 1024LL;

	rc = everlive_doctor_collect(&env, &report);
	assert(rc == 0);
	assert(report.fails == 0);
	assert(report.warns == 0);

	/* 2. Test failures: unbound driver, missing video group, missing ffmpeg, bad shm */
	snprintf(p, sizeof(p), "%s/bus/pci/devices/0000:0a:00.0/driver", sys_root);
	assert(unlink(p) == 0);
	snprintf(p, sizeof(p), "%s/ffmpeg", bin_dir);
	assert(unlink(p) == 0);
	snprintf(p, sizeof(p), "%s/gc573_v4l2_shm", shm_root);
	{
		int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		assert(fd >= 0);
		close(fd);
		assert(chmod(p, 0644) == 0);
	}
	env.mock_in_video_group = 0;

	rc = everlive_doctor_collect(&env, &report);
	assert(rc == 1);
	assert(report.fails >= 4);

	chk = find_check(&report, "hardware.binding");
	assert(chk && chk->status == EVERLIVE_CHECK_FAIL);
	assert(strstr(chk->fix, "everlive-vfio-bind") != NULL);

	chk = find_check(&report, "user.video_group");
	assert(chk && chk->status == EVERLIVE_CHECK_FAIL);

	chk = find_check(&report, "deps.ffmpeg");
	assert(chk && chk->status == EVERLIVE_CHECK_FAIL);

	chk = find_check(&report, "ipc.framestore_shm");
	assert(chk && chk->status == EVERLIVE_CHECK_FAIL);

	/* 3. Verify JSON formatting runs cleanly */
	FILE *devnull = fopen("/dev/null", "w");
	assert(devnull != NULL);
	assert(everlive_doctor_run(&env, true, true, devnull) == 1);
	fclose(devnull);

	rm_rf(root);
	printf("All everlive_doctor tests passed!\n");
	return 0;
}

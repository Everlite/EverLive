/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_doctor.c — Host prerequisite & health checks for `everlive-ctl doctor`
 */

#include "everlive_doctor.h"
#include "everlive_card.h"
#include "everlive_ipc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static void doctor_add(struct everlive_doctor_report *r, const char *id,
		       enum everlive_check_status status,
		       const char *detail_fmt, const char *fix_fmt, ...)
{
	if (!r || r->count >= EVERLIVE_DOCTOR_MAX_CHECKS)
		return;

	struct everlive_doctor_check *c = &r->checks[r->count++];
	memset(c, 0, sizeof(*c));
	snprintf(c->id, sizeof(c->id), "%s", id ? id : "unknown");
	c->status = status;
	if (status == EVERLIVE_CHECK_FAIL)
		r->fails++;
	else if (status == EVERLIVE_CHECK_WARN)
		r->warns++;

	if (detail_fmt) {
		va_list ap;

		va_start(ap, fix_fmt);
		vsnprintf(c->detail, sizeof(c->detail), detail_fmt, ap);
		va_end(ap);
	}
	if (fix_fmt)
		snprintf(c->fix, sizeof(c->fix), "%s", fix_fmt);
}

static bool read_line_trimmed(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return false;
	if (!fgets(buf, (int)size, f)) {
		fclose(f);
		return false;
	}
	fclose(f);
	size_t len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
			   buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
		buf[--len] = '\0';
	}
	return true;
}

static bool path_exists_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool dir_has_entries(const char *path, int *count_out)
{
	DIR *d = opendir(path);
	int count = 0;

	if (!d) {
		if (count_out)
			*count_out = 0;
		return false;
	}
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		count++;
	}
	closedir(d);
	if (count_out)
		*count_out = count;
	return count > 0;
}

static bool find_exe_in_path(const char *path_env, const char *name,
			     char *out, size_t out_size)
{
	if (!path_env || !*path_env || !name || !*name)
		return false;

	const char *p = path_env;
	while (*p) {
		const char *colon = strchr(p, ':');
		size_t dir_len = colon ? (size_t)(colon - p) : strlen(p);
		if (dir_len > 0 && dir_len + 1 + strlen(name) + 1 <= PATH_MAX) {
			char cand[PATH_MAX];

			memcpy(cand, p, dir_len);
			cand[dir_len] = '/';
			strcpy(cand + dir_len + 1, name);
			if (access(cand, X_OK) == 0) {
				if (out && out_size > 0)
					snprintf(out, out_size, "%s", cand);
				return true;
			}
		}
		if (!colon)
			break;
		p = colon + 1;
	}
	return false;
}

static bool user_in_video_group(const struct everlive_doctor_env *env, uid_t uid)
{
	if (env->mock_in_video_group >= 0)
		return env->mock_in_video_group != 0;
	if (uid == 0)
		return true;

	struct group *grp = getgrnam("video");
	if (!grp)
		return false;
	if (getegid() == grp->gr_gid)
		return true;

	int ngroups = getgroups(0, NULL);
	if (ngroups <= 0)
		return false;
	gid_t *groups = calloc((size_t)ngroups, sizeof(gid_t));
	if (!groups)
		return false;
	bool found = false;
	if (getgroups(ngroups, groups) >= 0) {
		for (int i = 0; i < ngroups; i++) {
			if (groups[i] == grp->gr_gid) {
				found = true;
				break;
			}
		}
	}
	free(groups);
	return found;
}

static bool binary_has_caps(const char *path)
{
	char xattr_buf[128];

	if (!path || access(path, X_OK) != 0)
		return false;
	return getxattr(path, "security.capability", xattr_buf, sizeof(xattr_buf)) > 0;
}

static void check_iommu(const struct everlive_doctor_env *env,
			struct everlive_doctor_report *r)
{
	char path[PATH_MAX];
	int groups = 0;

	snprintf(path, sizeof(path), "%s/kernel/iommu_groups", env->sys_root);
	if (dir_has_entries(path, &groups)) {
		doctor_add(r, "kernel.iommu", EVERLIVE_CHECK_PASS,
			   "IOMMU enabled (%d groups in %s)", "", groups, path);
	} else {
		doctor_add(r, "kernel.iommu", EVERLIVE_CHECK_FAIL,
			   "IOMMU disabled or no groups in %s",
			   "Enable VT-d / AMD-Vi in BIOS and add iommu=pt (or intel_iommu=on) to kernel cmdline",
			   path);
	}
}

static void check_vfio_module(const struct everlive_doctor_env *env,
			      struct everlive_doctor_report *r)
{
	char drv[PATH_MAX];
	char mod[PATH_MAX];

	snprintf(drv, sizeof(drv), "%s/bus/pci/drivers/vfio-pci", env->sys_root);
	snprintf(mod, sizeof(mod), "%s/module/vfio_pci", env->sys_root);
	if (path_exists_dir(drv) || path_exists_dir(mod)) {
		doctor_add(r, "kernel.vfio_pci", EVERLIVE_CHECK_PASS,
			   "vfio-pci driver is loaded", "");
	} else {
		doctor_add(r, "kernel.vfio_pci", EVERLIVE_CHECK_FAIL,
			   "vfio-pci driver is not loaded",
			   "sudo modprobe vfio-pci");
	}
}

static const char *match_card_slug(const char *vendor, const char *device,
				   const char *subsys)
{
	if (strcmp(vendor, "0x1461") == 0) {
		if (strcmp(device, "0x0054") == 0)
			return strcmp(subsys, "0x5700") == 0 ? "gc570d" : "gc573";
		if (strcmp(device, "0x0055") == 0)
			return "gc573";
	}
	if ((strcmp(vendor, "0x8888") == 0 &&
	     (strcmp(device, "0x8581") == 0 ||
	      strcmp(device, "0x85a1") == 0 ||
	      strcmp(device, "0x8591") == 0) &&
	     strcmp(subsys, "0x0007") == 0) ||
	    (strcmp(vendor, "0x1f33") == 0 && strcmp(device, "0x8581") == 0 &&
	     strcmp(subsys, "0x0007") == 0)) {
		return "hwsuhd";
	}
	return NULL;
}

static void check_cards_and_vfio_nodes(const struct everlive_doctor_env *env,
				       struct everlive_doctor_report *r)
{
	char pci_dir[PATH_MAX];
	DIR *d;
	int found = 0;

	snprintf(pci_dir, sizeof(pci_dir), "%s/bus/pci/devices", env->sys_root);
	d = opendir(pci_dir);
	if (!d) {
		doctor_add(r, "hardware.cards", EVERLIVE_CHECK_FAIL,
			   "Cannot scan PCI bus at %s: %s",
			   "Verify sysfs is mounted at /sys",
			   pci_dir, strerror(errno));
		return;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char p[PATH_MAX + 512], vendor[32], device[32], subsys[32] = "";
		snprintf(p, sizeof(p), "%s/%s/vendor", pci_dir, ent->d_name);
		if (!read_line_trimmed(p, vendor, sizeof(vendor)))
			continue;
		snprintf(p, sizeof(p), "%s/%s/device", pci_dir, ent->d_name);
		if (!read_line_trimmed(p, device, sizeof(device)))
			continue;
		snprintf(p, sizeof(p), "%s/%s/subsystem_device", pci_dir, ent->d_name);
		(void)read_line_trimmed(p, subsys, sizeof(subsys));

		const char *slug = match_card_slug(vendor, device, subsys);
		if (!slug)
			continue;

		found++;
		char drv_link[PATH_MAX + 512], drv_target[PATH_MAX];
		snprintf(drv_link, sizeof(drv_link), "%s/%s/driver", pci_dir, ent->d_name);
		ssize_t n = readlink(drv_link, drv_target, sizeof(drv_target) - 1);
		const char *drv_name = "unbound";
		if (n > 0) {
			drv_target[n] = '\0';
			const char *slash = strrchr(drv_target, '/');
			drv_name = slash ? slash + 1 : drv_target;
		}

		bool vfio_bound = strcmp(drv_name, "vfio-pci") == 0;
		if (vfio_bound) {
			doctor_add(r, "hardware.binding", EVERLIVE_CHECK_PASS,
				   "%s (%s) bound to vfio-pci", "", slug, ent->d_name);
		} else {
			char fix[512];
			snprintf(fix, sizeof(fix),
				 "sudo %.128s/everlive-vfio-bind --card %s --bdf %.64s --bind",
				 env->libexec_dir, slug, ent->d_name);
			doctor_add(r, "hardware.binding", EVERLIVE_CHECK_FAIL,
				   "%s (%s) is bound to '%s' instead of vfio-pci",
				   fix, slug, ent->d_name, drv_name);
		}

		char iommu_link[PATH_MAX + 512], iommu_target[PATH_MAX];
		snprintf(iommu_link, sizeof(iommu_link), "%s/%s/iommu_group",
			 pci_dir, ent->d_name);
		n = readlink(iommu_link, iommu_target, sizeof(iommu_target) - 1);
		if (n > 0) {
			iommu_target[n] = '\0';
			const char *grp = strrchr(iommu_target, '/');
			grp = grp ? grp + 1 : iommu_target;
			char vfio_node[PATH_MAX];
			struct stat st;
			snprintf(vfio_node, sizeof(vfio_node), "%s/vfio/%s",
				 env->dev_root, grp);
			if (stat(vfio_node, &st) == 0) {
				if ((st.st_mode & 0660) == 0660) {
					doctor_add(r, "vfio.group_node", EVERLIVE_CHECK_PASS,
						   "%s present (mode %04o)", "",
						   vfio_node, (unsigned)(st.st_mode & 0777));
				} else {
					char fix[512];
					snprintf(fix, sizeof(fix),
						 "sudo udevadm control --reload-rules && sudo udevadm trigger %.256s/%.64s",
						 pci_dir, ent->d_name);
					doctor_add(r, "vfio.group_node", EVERLIVE_CHECK_FAIL,
						   "%s has restrictive mode %04o",
						   fix, vfio_node, (unsigned)(st.st_mode & 0777));
				}
			} else if (vfio_bound) {
				char fix[512];
				snprintf(fix, sizeof(fix),
					 "sudo %.128s/everlive-vfio-bind --card %s --bdf %.64s --bind",
					 env->libexec_dir, slug, ent->d_name);
				doctor_add(r, "vfio.group_node", EVERLIVE_CHECK_FAIL,
					   "Missing VFIO group node %s: %s",
					   fix, vfio_node, strerror(errno));
			}
		}
	}
	closedir(d);

	if (found == 0) {
		doctor_add(r, "hardware.cards", EVERLIVE_CHECK_FAIL,
			   "No supported EverLive capture card found in %s",
			   "Check PCIe slot seating and run: lspci -nn | grep -iE '1461|8888|1f33'",
			   pci_dir);
	}
}

static void check_permissions_and_memlock(const struct everlive_doctor_env *env,
					  uid_t uid,
					  struct everlive_doctor_report *r)
{
	if (user_in_video_group(env, uid)) {
		doctor_add(r, "user.video_group", EVERLIVE_CHECK_PASS,
			   "User (uid=%u) has video group access", "", (unsigned)uid);
	} else {
		doctor_add(r, "user.video_group", EVERLIVE_CHECK_FAIL,
			   "User (uid=%u) is not in the 'video' group",
			   "sudo usermod -aG video \"$USER\" && log out/in",
			   (unsigned)uid);
	}

	char streamer_bin[PATH_MAX];
	snprintf(streamer_bin, sizeof(streamer_bin),
		 "%s/tools/opengc573-userd/gc573_user_streamer", env->app_libdir);
	bool has_cap = binary_has_caps(streamer_bin);

	long long memlock_bytes = env->mock_memlock_bytes;
	if (memlock_bytes < 0) {
		struct rlimit rl;
		if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
			memlock_bytes = (rl.rlim_cur == RLIM_INFINITY)
				? (long long)(1024ULL * 1024ULL * 1024ULL * 1024ULL)
				: (long long)rl.rlim_cur;
		} else {
			memlock_bytes = 0;
		}
	}

	if (has_cap || memlock_bytes >= (long long)(512ULL * 1024ULL * 1024ULL)) {
		doctor_add(r, "user.memlock", EVERLIVE_CHECK_PASS,
			   "VFIO memlock ready (%s, RLIMIT_MEMLOCK=%lld MB)", "",
			   has_cap ? "cap_ipc_lock=+ep" : "no file cap",
			   memlock_bytes / (1024LL * 1024LL));
	} else {
		char fix[256];
		snprintf(fix, sizeof(fix), "sudo %s/everlive-setcap", env->libexec_dir);
		doctor_add(r, "user.memlock", EVERLIVE_CHECK_FAIL,
			   "RLIMIT_MEMLOCK is %lld MB and %s lacks cap_ipc_lock",
			   fix, memlock_bytes / (1024LL * 1024LL), streamer_bin);
	}
}

static void check_binaries_and_tools(const struct everlive_doctor_env *env,
				     struct everlive_doctor_report *r)
{
	char bin[PATH_MAX];
	bool gc573_ok = false;
	bool gc570d_ok = false;

	snprintf(bin, sizeof(bin), "%s/tools/opengc573-userd/gc573_user_streamer",
		 env->app_libdir);
	gc573_ok = access(bin, X_OK) == 0 ||
		   find_exe_in_path(env->path_env, "gc573_user_streamer", NULL, 0);

	snprintf(bin, sizeof(bin), "%s/tools/opengc570d-userd/opengc570d-userd",
		 env->app_libdir);
	gc570d_ok = access(bin, X_OK) == 0 ||
		    find_exe_in_path(env->path_env, "opengc570d-userd", NULL, 0);

	if (gc573_ok || gc570d_ok) {
		doctor_add(r, "install.engines", EVERLIVE_CHECK_PASS,
			   "Engine binaries resolved (gc573=%s, gc570d=%s)", "",
			   gc573_ok ? "ok" : "missing", gc570d_ok ? "ok" : "missing");
	} else {
		doctor_add(r, "install.engines", EVERLIVE_CHECK_FAIL,
			   "No EverLive engine binary found in %s or PATH",
			   "Run: sudo make install  (or ./install.sh)",
			   env->app_libdir);
	}

	if (find_exe_in_path(env->path_env, "ffmpeg", bin, sizeof(bin))) {
		doctor_add(r, "deps.ffmpeg", EVERLIVE_CHECK_PASS,
			   "ffmpeg found at %s", "", bin);
	} else {
		doctor_add(r, "deps.ffmpeg", EVERLIVE_CHECK_FAIL,
			   "ffmpeg not found in PATH",
			   "Install ffmpeg via your distribution package manager");
	}

	if (find_exe_in_path(env->path_env, "wpctl", bin, sizeof(bin))) {
		doctor_add(r, "deps.pipewire", EVERLIVE_CHECK_PASS,
			   "PipeWire CLI (wpctl) found at %s", "", bin);
	} else {
		doctor_add(r, "deps.pipewire", EVERLIVE_CHECK_WARN,
			   "wpctl not found in PATH (audio routing may be limited)",
			   "Install wireplumber / pipewire");
	}
}

static void check_loopback_and_runtime(const struct everlive_doctor_env *env,
				       uid_t uid,
				       struct everlive_doctor_report *r)
{
	char mod[PATH_MAX], vdev[PATH_MAX];

	snprintf(mod, sizeof(mod), "%s/module/v4l2loopback", env->sys_root);
	snprintf(vdev, sizeof(vdev), "%s/video10", env->dev_root);
	if (path_exists_dir(mod) || access(vdev, F_OK) == 0) {
		doctor_add(r, "video.v4l2loopback", EVERLIVE_CHECK_PASS,
			   "v4l2loopback active (%s)", "",
			   access(vdev, F_OK) == 0 ? vdev : mod);
	} else {
		doctor_add(r, "video.v4l2loopback", EVERLIVE_CHECK_WARN,
			   "v4l2loopback module / %s not present (optional for Hub-only use)",
			   "sudo modprobe v4l2loopback video_nr=10 card_label=\"EverLive Capture\" exclusive_caps=1",
			   vdev);
	}

	char rdir[PATH_MAX];
	if (env->runtime_dir && env->runtime_dir[0]) {
		struct stat st;
		if (lstat(env->runtime_dir, &st) == 0 && S_ISDIR(st.st_mode) &&
		    st.st_uid == uid && (st.st_mode & 0777) == 0700) {
			doctor_add(r, "ipc.runtime_dir", EVERLIVE_CHECK_PASS,
				   "Private runtime dir %s (0700)", "", env->runtime_dir);
		} else {
			doctor_add(r, "ipc.runtime_dir", EVERLIVE_CHECK_FAIL,
				   "Runtime dir %s missing or insecure permissions",
				   "Ensure XDG_RUNTIME_DIR is a 0700 directory owned by your user",
				   env->runtime_dir);
		}
	} else if (everlive_ipc_runtime_dir(rdir, sizeof(rdir)) == 0) {
		char sock[PATH_MAX + 32];
		snprintf(sock, sizeof(sock), "%s/%s", rdir, EVERLIVE_IPC_SOCKET_NAME);
		doctor_add(r, "ipc.runtime_dir", EVERLIVE_CHECK_PASS,
			   "Runtime dir %s (%s)", "", rdir,
			   access(sock, F_OK) == 0 ? "socket active" : "socket idle");
	} else {
		doctor_add(r, "ipc.runtime_dir", EVERLIVE_CHECK_FAIL,
			   "No private 0700 runtime directory available",
			   "Ensure XDG_RUNTIME_DIR is set and owned by your user with mode 0700");
	}

	DIR *sd = opendir(env->shm_root);
	if (sd) {
		struct dirent *ent;
		int bad_shm = 0;

		while ((ent = readdir(sd)) != NULL) {
			if (!strstr(ent->d_name, "_v4l2_shm"))
				continue;
			char spath[PATH_MAX];
			struct stat st;
			snprintf(spath, sizeof(spath), "%s/%s", env->shm_root, ent->d_name);
			if (lstat(spath, &st) == 0 &&
			    (st.st_uid != uid || (st.st_mode & 0777) != 0600)) {
				bad_shm++;
				doctor_add(r, "ipc.framestore_shm", EVERLIVE_CHECK_FAIL,
					   "SHM %s has uid=%u mode=%04o (expected uid=%u mode=0600)",
					   "Remove stale SHM: rm -f /dev/shm/*_v4l2_shm",
					   spath, (unsigned)st.st_uid,
					   (unsigned)(st.st_mode & 0777), (unsigned)uid);
			}
		}
		closedir(sd);
		if (bad_shm == 0) {
			doctor_add(r, "ipc.framestore_shm", EVERLIVE_CHECK_PASS,
				   "No foreign or permissive *_v4l2_shm objects in %s", "",
				   env->shm_root);
		}
	}
}

void everlive_doctor_env_defaults(struct everlive_doctor_env *env)
{
	if (!env)
		return;
	memset(env, 0, sizeof(*env));
	env->sys_root = "/sys";
	env->dev_root = "/dev";
	env->shm_root = "/dev/shm";
	env->path_env = getenv("PATH");
	env->app_libdir = "/usr/local/lib/everlive";
	env->libexec_dir = "/usr/local/libexec";
	env->mock_in_video_group = -1;
	env->mock_memlock_bytes = -1;
}

int everlive_doctor_collect(const struct everlive_doctor_env *in_env,
			    struct everlive_doctor_report *report)
{
	struct everlive_doctor_env env;

	if (!report)
		return -EINVAL;
	memset(report, 0, sizeof(*report));

	everlive_doctor_env_defaults(&env);
	if (in_env) {
		if (in_env->sys_root) env.sys_root = in_env->sys_root;
		if (in_env->dev_root) env.dev_root = in_env->dev_root;
		if (in_env->shm_root) env.shm_root = in_env->shm_root;
		if (in_env->path_env) env.path_env = in_env->path_env;
		if (in_env->app_libdir) env.app_libdir = in_env->app_libdir;
		if (in_env->libexec_dir) env.libexec_dir = in_env->libexec_dir;
		if (in_env->runtime_dir) env.runtime_dir = in_env->runtime_dir;
		if (in_env->use_mock_uid) {
			env.use_mock_uid = true;
			env.euid = in_env->euid;
		}
		if (in_env->mock_in_video_group >= 0)
			env.mock_in_video_group = in_env->mock_in_video_group;
		if (in_env->mock_memlock_bytes >= 0)
			env.mock_memlock_bytes = in_env->mock_memlock_bytes;
	}

	uid_t uid = env.use_mock_uid ? env.euid : geteuid();

	check_iommu(&env, report);
	check_vfio_module(&env, report);
	check_cards_and_vfio_nodes(&env, report);
	check_permissions_and_memlock(&env, uid, report);
	check_binaries_and_tools(&env, report);
	check_loopback_and_runtime(&env, uid, report);

	return report->fails > 0 ? 1 : 0;
}

static const char *status_tag(enum everlive_check_status st)
{
	switch (st) {
	case EVERLIVE_CHECK_PASS: return "PASS";
	case EVERLIVE_CHECK_INFO: return "INFO";
	case EVERLIVE_CHECK_WARN: return "WARN";
	case EVERLIVE_CHECK_FAIL: return "FAIL";
	}
	return "INFO";
}

void everlive_doctor_print_text(const struct everlive_doctor_report *report,
				bool verbose, FILE *out)
{
	if (!report || !out)
		return;

	fprintf(out, "=== EverLive Doctor ===\n");
	for (size_t i = 0; i < report->count; i++) {
		const struct everlive_doctor_check *c = &report->checks[i];
		if (!verbose && c->status == EVERLIVE_CHECK_PASS)
			fprintf(out, "[PASS] %-22s %s\n", c->id, c->detail);
		else {
			fprintf(out, "[%s] %-22s %s\n",
				status_tag(c->status), c->id, c->detail);
			if (c->fix[0])
				fprintf(out, "       Fix: %s\n", c->fix);
		}
	}
	fprintf(out, "\nSummary: %d error(s), %d warning(s) across %zu checks.\n",
		report->fails, report->warns, report->count);
}

static void print_json_escaped(FILE *out, const char *s)
{
	fputc('"', out);
	for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
		switch (*p) {
		case '"':  fputs("\\\"", out); break;
		case '\\': fputs("\\\\", out); break;
		case '\n': fputs("\\n", out); break;
		case '\r': fputs("\\r", out); break;
		case '\t': fputs("\\t", out); break;
		default:
			if (*p < 0x20)
				fprintf(out, "\\u%04x", *p);
			else
				fputc(*p, out);
		}
	}
	fputc('"', out);
}

void everlive_doctor_print_json(const struct everlive_doctor_report *report,
				FILE *out)
{
	if (!report || !out)
		return;

	fprintf(out, "{\"ok\":%s,\"errors\":%d,\"warnings\":%d,\"checks\":[",
		report->fails == 0 ? "true" : "false",
		report->fails, report->warns);
	for (size_t i = 0; i < report->count; i++) {
		const struct everlive_doctor_check *c = &report->checks[i];
		if (i > 0)
			fputc(',', out);
		fputs("{\"id\":", out);
		print_json_escaped(out, c->id);
		fputs(",\"status\":", out);
		switch (c->status) {
		case EVERLIVE_CHECK_PASS: fputs("\"pass\"", out); break;
		case EVERLIVE_CHECK_INFO: fputs("\"info\"", out); break;
		case EVERLIVE_CHECK_WARN: fputs("\"warn\"", out); break;
		case EVERLIVE_CHECK_FAIL: fputs("\"fail\"", out); break;
		}
		fputs(",\"detail\":", out);
		print_json_escaped(out, c->detail);
		fputs(",\"fix\":", out);
		print_json_escaped(out, c->fix);
		fputc('}', out);
	}
	fputs("]}\n", out);
}

int everlive_doctor_run(const struct everlive_doctor_env *env,
			bool json_mode, bool verbose, FILE *out)
{
	struct everlive_doctor_report report;
	int rc = everlive_doctor_collect(env, &report);

	if (json_mode)
		everlive_doctor_print_json(&report, out ? out : stdout);
	else
		everlive_doctor_print_text(&report, verbose, out ? out : stdout);

	return rc;
}

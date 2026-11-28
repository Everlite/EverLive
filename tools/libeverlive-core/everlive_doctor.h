/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_doctor.h — Host prerequisite & health checks for `everlive-ctl doctor`
 */

#ifndef EVERLIVE_DOCTOR_H
#define EVERLIVE_DOCTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum everlive_check_status {
	EVERLIVE_CHECK_PASS = 0,
	EVERLIVE_CHECK_INFO = 1,
	EVERLIVE_CHECK_WARN = 2,
	EVERLIVE_CHECK_FAIL = 3,
};

struct everlive_doctor_env {
	const char *sys_root;		/* default "/sys" */
	const char *dev_root;		/* default "/dev" */
	const char *shm_root;		/* default "/dev/shm" */
	const char *path_env;		/* default getenv("PATH") */
	const char *app_libdir;		/* default "/usr/local/lib/everlive" */
	const char *libexec_dir;	/* default "/usr/local/libexec" */
	const char *runtime_dir;	/* optional override for IPC runtime dir */
	uid_t euid;			/* default geteuid() when !use_mock_uid */
	bool use_mock_uid;
	int mock_in_video_group;	/* -1 = check real groups, 0/1 = override */
	long long mock_memlock_bytes;	/* -1 = check real RLIMIT_MEMLOCK */
};

struct everlive_doctor_check {
	char id[48];
	enum everlive_check_status status;
	char detail[512];
	char fix[512];
};

#define EVERLIVE_DOCTOR_MAX_CHECKS 32

struct everlive_doctor_report {
	struct everlive_doctor_check checks[EVERLIVE_DOCTOR_MAX_CHECKS];
	size_t count;
	int fails;
	int warns;
};

void everlive_doctor_env_defaults(struct everlive_doctor_env *env);

int everlive_doctor_collect(const struct everlive_doctor_env *env,
			    struct everlive_doctor_report *report);

void everlive_doctor_print_text(const struct everlive_doctor_report *report,
				bool verbose, FILE *out);

void everlive_doctor_print_json(const struct everlive_doctor_report *report,
				FILE *out);

int everlive_doctor_run(const struct everlive_doctor_env *env,
			bool json_mode, bool verbose, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_DOCTOR_H */

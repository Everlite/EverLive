// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_ipc.c — Runtime directory, peer credential and lock-file tests
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "everlive_ipc.h"

static void make_temp_dir(char *buf, size_t size)
{
	snprintf(buf, size, "/tmp/everlive-ipc-test-XXXXXX");
	assert(mkdtemp(buf) != NULL);
}

static void test_private_dir_and_socket_path(void)
{
	char dir[128];
	char path[EVERLIVE_IPC_PATH_MAX];
	struct stat st;

	printf("[*] IPC private runtime directory...\n");
	make_temp_dir(dir, sizeof(dir));
	assert(setenv("XDG_RUNTIME_DIR", dir, 1) == 0);

	assert(everlive_ipc_runtime_dir(path, sizeof(path)) == 0);
	assert(strcmp(path, dir) == 0);
	assert(everlive_ipc_prepare_runtime_dir(path, sizeof(path)) == 0);
	assert(everlive_ipc_prepare_socket_path(path, sizeof(path), NULL) == 0);
	assert(strstr(path, "/everlive.sock") != NULL);
	assert(lstat(dir, &st) == 0);
	assert(st.st_uid == geteuid());
	assert((st.st_mode & 07777) == 0700);

	chmod(dir, 0755);
	assert(everlive_ipc_runtime_dir(path, sizeof(path)) == -EACCES);
	assert(everlive_ipc_prepare_runtime_dir(path, sizeof(path)) == -EACCES);
	chmod(dir, 0700);
	assert(everlive_ipc_runtime_dir(path, sizeof(path)) == 0);

	unsetenv("XDG_RUNTIME_DIR");
	assert(rmdir(dir) == 0);
}

static void test_symlink_and_non_dir_rejected(void)
{
	char target[128];
	char path[EVERLIVE_IPC_PATH_MAX];
	char link[] = "/tmp/everlive-ipc-link-XXXXXX";
	char file[] = "/tmp/everlive-ipc-file-XXXXXX";
	int fd;

	printf("[*] IPC rejects symlinks and non-directories...\n");
	make_temp_dir(target, sizeof(target));

	fd = mkstemp(link);
	assert(fd >= 0);
	close(fd);
	assert(unlink(link) == 0);
	assert(symlink(target, link) == 0);
	assert(setenv("XDG_RUNTIME_DIR", link, 1) == 0);
	assert(everlive_ipc_runtime_dir(path, sizeof(path)) == -ELOOP);
	assert(everlive_ipc_prepare_runtime_dir(path, sizeof(path)) == -ELOOP);
	unlink(link);

	fd = mkstemp(file);
	assert(fd >= 0);
	close(fd);
	assert(setenv("XDG_RUNTIME_DIR", file, 1) == 0);
	assert(everlive_ipc_runtime_dir(path, sizeof(path)) == -ENOTDIR);
	assert(everlive_ipc_prepare_runtime_dir(path, sizeof(path)) == -ENOTDIR);
	unlink(file);

	unsetenv("XDG_RUNTIME_DIR");
	assert(rmdir(target) == 0);
}

static void test_subdir_create_and_validate(void)
{
	char dir[128];
	char sub[EVERLIVE_IPC_PATH_MAX];
	struct stat st;

	printf("[*] IPC engine subdirectory...\n");
	make_temp_dir(dir, sizeof(dir));
	assert(setenv("XDG_RUNTIME_DIR", dir, 1) == 0);

	assert(everlive_ipc_prepare_runtime_subdir("gc570d", sub,
						   sizeof(sub)) == 0);
	assert(strcmp(sub, dir) != 0);
	assert(strstr(sub, "/gc570d") != NULL);
	assert(lstat(sub, &st) == 0);
	assert(st.st_uid == geteuid());
	assert((st.st_mode & 07777) == 0700);

	chmod(sub, 0777);
	assert(everlive_ipc_prepare_runtime_subdir("gc570d", sub,
						   sizeof(sub)) == -EACCES);
	chmod(sub, 0700);
	assert(everlive_ipc_prepare_runtime_subdir("gc570d", sub,
						   sizeof(sub)) == 0);

	assert(rmdir(sub) == 0);
	unsetenv("XDG_RUNTIME_DIR");
	assert(rmdir(dir) == 0);
}

static void test_precreated_permissive_dir_rejected(void)
{
	char name[64];
	char dir[EVERLIVE_IPC_PATH_MAX];
	char path[EVERLIVE_IPC_PATH_MAX];
	struct stat st;
	int ret;

	printf("[*] IPC pre-created permissive fallback/subdir rejected...\n");
	snprintf(name, sizeof(name), "everlive-ipc-precreated-%d", (int)getpid());

	unsetenv("XDG_RUNTIME_DIR");
	ret = everlive_ipc_runtime_subdir(name, dir, sizeof(dir));
	assert(ret == 0);
	if (lstat(dir, &st) == 0) {
		assert(S_ISDIR(st.st_mode));
		assert(chmod(dir, 0700) == 0);
		assert(rmdir(dir) == 0);
	}
	assert(lstat(dir, &st) < 0 && errno == ENOENT);

	assert(mkdir(dir, 0700) == 0);
	assert(chmod(dir, 0777) == 0);
	assert(everlive_ipc_runtime_subdir(name, path, sizeof(path)) == -EACCES);
	assert(everlive_ipc_prepare_runtime_subdir(name, path,
						   sizeof(path)) == -EACCES);
	assert(chmod(dir, 0755) == 0);
	assert(everlive_ipc_prepare_runtime_subdir(name, path,
						   sizeof(path)) == -EACCES);
	assert(chmod(dir, 0700) == 0);
	assert(everlive_ipc_prepare_runtime_subdir(name, path,
						   sizeof(path)) == 0);
	assert(rmdir(dir) == 0);
}

static void test_precreated_base_fallback_rejected(void)
{
	char run[64];
	char dir[64];
	char path[EVERLIVE_IPC_PATH_MAX];
	struct stat st;

	printf("[*] IPC pre-created base fallback rejected...\n");
	snprintf(run, sizeof(run), "/run/user/%u", (unsigned int)geteuid());
	if (lstat(run, &st) == 0) {
		printf("    (skipped: /run/user/%u is available)\n",
		       (unsigned int)geteuid());
		return;
	}

	snprintf(dir, sizeof(dir), "/tmp/everlive-%u",
		 (unsigned int)geteuid());
	if (lstat(dir, &st) == 0) {
		printf("    (skipped: %s already exists)\n", dir);
		return;
	}

	assert(mkdir(dir, 0700) == 0);
	assert(chmod(dir, 0777) == 0);
	unsetenv("XDG_RUNTIME_DIR");
	assert(everlive_ipc_runtime_dir(path, sizeof(path)) == -EACCES);
	assert(everlive_ipc_prepare_runtime_dir(path, sizeof(path)) == -EACCES);
	assert(chmod(dir, 0700) == 0);
	assert(everlive_ipc_prepare_runtime_dir(path, sizeof(path)) == 0);
	assert(rmdir(dir) == 0);
}

static void test_owner_mismatch_when_root(void)
{
	char dir[128];
	char path[EVERLIVE_IPC_PATH_MAX];

	printf("[*] IPC foreign-owned runtime directory...\n");
	make_temp_dir(dir, sizeof(dir));
	if (geteuid() != 0) {
		printf("    (skipped: requires root to chown a foreign UID)\n");
		assert(rmdir(dir) == 0);
		return;
	}

	assert(chown(dir, 65534, 65534) == 0);
	assert(setenv("XDG_RUNTIME_DIR", dir, 1) == 0);
	assert(everlive_ipc_runtime_dir(path, sizeof(path)) == -EACCES);
	assert(everlive_ipc_prepare_runtime_dir(path, sizeof(path)) == -EACCES);
	unsetenv("XDG_RUNTIME_DIR");
	assert(chown(dir, geteuid(), getegid()) == 0);
	assert(rmdir(dir) == 0);
}

static void test_debug_control_env(void)
{
	printf("[*] EVERLIVE_DEBUG_CONTROL parsing...\n");
	unsetenv("EVERLIVE_DEBUG_CONTROL");
	assert(!everlive_ipc_debug_control_enabled());
	assert(setenv("EVERLIVE_DEBUG_CONTROL", "0", 1) == 0);
	assert(!everlive_ipc_debug_control_enabled());
	assert(setenv("EVERLIVE_DEBUG_CONTROL", "1", 1) == 0);
	assert(everlive_ipc_debug_control_enabled());
	unsetenv("EVERLIVE_DEBUG_CONTROL");
}

static void test_peer_identity(void)
{
	uid_t uid = (uid_t)-1;
	pid_t pid = -1;
	int sv[2];

	printf("[*] IPC SO_PEERCRED helper...\n");
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	assert(everlive_ipc_peer_identity(sv[0], &uid, &pid) == 0);
	assert(uid == getuid());
	assert(pid == getpid());
	close(sv[0]);
	close(sv[1]);
}

static void test_socket_lock(void)
{
	char dir[128];
	char path[EVERLIVE_IPC_PATH_MAX];
	char lock_path[EVERLIVE_IPC_PATH_MAX + 8];
	int lock_fd = -1;
	int second_fd = -1;

	printf("[*] IPC socket lock file...\n");
	make_temp_dir(dir, sizeof(dir));
	assert(setenv("XDG_RUNTIME_DIR", dir, 1) == 0);
	assert(snprintf(path, sizeof(path), "%s/everlive.sock", dir) > 0);
	assert(snprintf(lock_path, sizeof(lock_path), "%s.lock", path) > 0);

	assert(everlive_ipc_acquire_socket_lock(path, &lock_fd) == 0);
	assert(everlive_ipc_acquire_socket_lock(path, &second_fd) == -EADDRINUSE);
	assert(second_fd == -1);
	close(lock_fd);
	assert(unlink(lock_path) == 0);

	unsetenv("XDG_RUNTIME_DIR");
	assert(rmdir(dir) == 0);
}

static void test_error_message(void)
{
	char msg[512];

	printf("[*] IPC actionable error text...\n");
	assert(everlive_ipc_describe_error(-EACCES, "/tmp/example", msg,
					   sizeof(msg)) == 0);
	assert(strstr(msg, "/tmp/example") != NULL);
	assert(strstr(msg, "0700") != NULL);
}

int main(void)
{
	printf("=== Running IPC security tests ===\n");
	test_private_dir_and_socket_path();
	test_symlink_and_non_dir_rejected();
	test_subdir_create_and_validate();
	test_precreated_permissive_dir_rejected();
	test_precreated_base_fallback_rejected();
	test_owner_mismatch_when_root();
	test_debug_control_env();
	test_peer_identity();
	test_socket_lock();
	test_error_message();
	printf("=== All IPC security tests passed! ===\n");
	return 0;
}

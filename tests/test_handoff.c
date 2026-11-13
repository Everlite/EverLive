// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_handoff.c — Cross-user card handoff protocol (everlive_handoff.c)
 *
 * Holder and requester run as two processes of the same user, which the
 * holder always authorises; the group check needs a second account and is
 * not covered here.
 */
#include "everlive_handoff.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_requested;

static void on_request(void *user)
{
	(void)user;
	g_requested = 1;
}

static void card_name(char *buf, size_t size, const char *tag)
{
	snprintf(buf, size, "test-%s-%d", tag, (int)getpid());
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static int connect_retry(const char *card, struct everlive_handoff_peer *peer)
{
	int fd = -ENOENT;

	for (int i = 0; i < 200 && fd == -ENOENT; i++) {
		fd = everlive_handoff_connect(card, peer);
		if (fd == -ENOENT)
			sleep_ms(10);
	}
	return fd;
}

/*
 * Holder process.  Serves @card in @role, yields when asked, then (engines
 * only) waits for the requester to finish and reports it through the exit
 * status: 0 = saw the requester leave, 3 = never asked, 4 = no lease.
 */
static pid_t spawn_holder(const char *card, int role, int linger_ms)
{
	pid_t pid = fork();

	assert(pid >= 0);
	if (pid != 0)
		return pid;

	if (everlive_handoff_serve(card, NULL, role, on_request, NULL) < 0)
		_exit(2);
	for (int i = 0; i < linger_ms / 10 && !g_requested; i++)
		sleep_ms(10);
	if (!g_requested) {
		everlive_handoff_stop();
		_exit(3);
	}
	if (role == EVERLIVE_HANDOFF_ROLE_KEEPER)
		_exit(0);	/* a keeper yields by exiting */

	struct everlive_handoff_peer peer;
	int fd;

	if (!everlive_handoff_pending(&peer) || peer.uid != getuid())
		_exit(5);
	fd = everlive_handoff_release();
	if (fd < 0)
		_exit(4);
	for (int i = 0; i < 500; i++) {
		if (everlive_handoff_wait(fd, 10))
			_exit(0);
	}
	_exit(6);
}

static int reap(pid_t pid)
{
	int status = 0;

	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status));
	return WEXITSTATUS(status);
}

static void test_no_holder(void)
{
	char card[64];

	card_name(card, sizeof(card), "none");
	assert(everlive_handoff_connect(card, NULL) == -ENOENT);
}

static void test_engine_yields_to_take(void)
{
	struct everlive_handoff_peer holder;
	char card[64];
	pid_t pid;
	int fd;

	card_name(card, sizeof(card), "take");
	pid = spawn_holder(card, EVERLIVE_HANDOFF_ROLE_ENGINE, 3000);
	fd = connect_retry(card, &holder);
	assert(fd >= 0);
	assert(holder.uid == getuid());
	assert(holder.pid == pid);
	assert(holder.name[0] != '\0');

	assert(everlive_handoff_request(fd, false, 3000) ==
	       EVERLIVE_HANDOFF_RELEASED);
	/* The name is free again for the new holder. */
	assert(everlive_handoff_connect(card, NULL) == -ENOENT);
	/* The paused engine is still there, waiting on the lease. */
	assert(everlive_handoff_wait(fd, 50) == 0);

	close(fd);
	assert(reap(pid) == 0);
}

static void test_engine_refuses_resume(void)
{
	char card[64];
	pid_t pid;
	int fd;

	card_name(card, sizeof(card), "resume");
	pid = spawn_holder(card, EVERLIVE_HANDOFF_ROLE_ENGINE, 300);
	fd = connect_retry(card, NULL);
	assert(fd >= 0);

	assert(everlive_handoff_request(fd, true, 3000) == -EBUSY);
	/* Still capturing: the watch stays quiet until the holder exits. */
	assert(everlive_handoff_wait(fd, 50) == 0);
	assert(reap(pid) == 3);
	assert(everlive_handoff_wait(fd, 1000) == 1);
	close(fd);
}

static void test_keeper_yields_to_resume(void)
{
	char card[64];
	pid_t pid;
	int fd;

	card_name(card, sizeof(card), "keeper");
	pid = spawn_holder(card, EVERLIVE_HANDOFF_ROLE_KEEPER, 3000);
	fd = connect_retry(card, NULL);
	assert(fd >= 0);

	assert(everlive_handoff_request(fd, true, 3000) ==
	       EVERLIVE_HANDOFF_GONE);
	close(fd);
	assert(reap(pid) == 0);
}

static void test_garbage_is_ignored(void)
{
	char card[64];
	pid_t pid;
	int fd;

	card_name(card, sizeof(card), "junk");
	pid = spawn_holder(card, EVERLIVE_HANDOFF_ROLE_ENGINE, 400);
	fd = connect_retry(card, NULL);
	assert(fd >= 0);
	assert(write(fd, "GIVE ME THE CARD\n", 17) == 17);
	/* Dropped without yielding. */
	assert(everlive_handoff_wait(fd, 1000) == 1);
	close(fd);
	assert(reap(pid) == 3);
}

static void test_state_file(void)
{
	struct everlive_handoff_peer holder = { .uid = 1234, .pid = 1 };
	char dir[] = "/tmp/everlive-handoff-test-XXXXXX";
	char path[256];
	char line[128];
	bool saw_holder = false;
	FILE *f;

	assert(mkdtemp(dir) != NULL);
	setenv("XDG_RUNTIME_DIR", dir, 1);
	snprintf(holder.name, sizeof(holder.name), "someone");

	assert(everlive_handoff_state_write("0000:01:00.0", &holder) == 0);
	assert(everlive_handoff_state_path(path, sizeof(path)) == 0);
	f = fopen(path, "r");
	assert(f != NULL);
	while (fgets(line, sizeof(line), f)) {
		if (strcmp(line, "holder=someone\n") == 0)
			saw_holder = true;
	}
	fclose(f);
	assert(saw_holder);

	everlive_handoff_state_clear();
	assert(access(path, F_OK) != 0);
	assert(rmdir(dir) == 0);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);

	test_no_holder();
	test_engine_yields_to_take();
	test_engine_refuses_resume();
	test_keeper_yields_to_resume();
	test_garbage_is_ignored();
	test_state_file();

	printf("test_handoff: OK\n");
	return 0;
}

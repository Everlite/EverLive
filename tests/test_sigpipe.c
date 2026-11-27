// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_sigpipe.c — Closed-pipe survival check.
 *
 * Every engine installs SIG_IGN for SIGPIPE at startup and handles EPIPE at
 * the write()/send() site.  This test reproduces that contract in a child
 * process: writing to a pipe whose reader has gone must return EPIPE rather
 * than terminate the writer.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(bool condition, const char *name, const char *detail)
{
	if (condition) {
		printf("  [PASS] %s\n", name);
	} else {
		printf("  [FAIL] %s: %s\n", name,
		       detail ? detail : "assertion failed");
		failures++;
	}
}

/*
 * Run @ignore_sigpipe in a child, close the pipe reader in the parent, then
 * let the child write.  Returns the child wait status.
 */
static int run_closed_pipe_writer(bool ignore_sigpipe)
{
	int data[2];
	int sync[2];
	pid_t pid;
	int status = 0;

	if (pipe(data) < 0 || pipe(sync) < 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(data[0]);
		close(data[1]);
		close(sync[0]);
		close(sync[1]);
		return -1;
	}

	if (pid == 0) {
		char gate;
		ssize_t n;
		int err;

		close(data[0]);
		close(sync[1]);
		if (ignore_sigpipe)
			signal(SIGPIPE, SIG_IGN);
		else
			signal(SIGPIPE, SIG_DFL);

		/* Wait until the parent has closed the read end. */
		(void)read(sync[0], &gate, 1);
		close(sync[0]);

		errno = 0;
		n = write(data[1], "x", 1);
		err = errno;
		close(data[1]);

		if (n < 0 && err == EPIPE)
			_exit(0);
		_exit(1);
	}

	close(data[0]);	/* reader gone: next write must fail with EPIPE */
	close(data[1]);
	close(sync[0]);
	(void)write(sync[1], "g", 1);
	close(sync[1]);

	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return status;
}

int main(void)
{
	int status;

	printf("=== Running closed-pipe SIGPIPE tests ===\n");

	status = run_closed_pipe_writer(true);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "ignoring SIGPIPE turns a closed-pipe write into EPIPE and survives",
	      "writer did not observe EPIPE");

	/*
	 * Contrast case: with the default disposition the same write must kill
	 * the writer.  This proves the first result comes from SIG_IGN rather
	 * than the kernel quietly returning EPIPE in every configuration.
	 */
	status = run_closed_pipe_writer(false);
	check(WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE,
	      "default SIGPIPE disposition terminates the writer",
	      "writer was not terminated by SIGPIPE");

	if (failures == 0) {
		printf("\n>>> All closed-pipe tests passed successfully! <<<\n");
		return 0;
	}
	printf("\n>>> %d closed-pipe test(s) failed! <<<\n", failures);
	return 1;
}

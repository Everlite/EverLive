// SPDX-License-Identifier: GPL-2.0-only
/*
 * everlive_handoff.c — Cross-user capture card handoff
 *
 * See everlive_handoff.h for the protocol.
 */
#include "everlive_handoff.h"
#include "everlive_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SO_PEERGROUPS
#define SO_PEERGROUPS 59
#endif

#define HANDOFF_PREFIX		"everlive-handoff-"
#define HANDOFF_MAGIC		"EVERLIVE-HANDOFF 1 "
#define HANDOFF_LINE_MAX	64
#define HANDOFF_MAX_WATCHERS	8
#define HANDOFF_MAX_GROUPS	1024
#define HANDOFF_HELLO_MS	3000
#define HANDOFF_REBIND_MS	1000

static struct {
	pthread_mutex_t lock;
	pthread_t thread;
	bool thread_running;
	int listen_fd;
	int wake_fd;
	int role;
	char card_id[64];
	bool have_node;
	bool node_any;
	bool node_group;
	gid_t node_gid;
	everlive_handoff_cb cb;
	void *user;
	int pending_fd;
	struct everlive_handoff_peer pending_peer;
	int watchers[HANDOFF_MAX_WATCHERS];
	unsigned int nr_watchers;
} hs = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.listen_fd = -1,
	.wake_fd = -1,
	.pending_fd = -1,
};

static socklen_t handoff_addr(struct sockaddr_un *addr, const char *card_id)
{
	int n;

	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	/* Abstract namespace: sun_path[0] stays NUL, the name is not a path. */
	n = snprintf(addr->sun_path + 1, sizeof(addr->sun_path) - 1,
		     HANDOFF_PREFIX "%s", card_id);
	if (n <= 0 || (size_t)n >= sizeof(addr->sun_path) - 1)
		return 0;
	return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 +
			   (size_t)n);
}

static void fill_peer(struct everlive_handoff_peer *peer,
		      const struct ucred *cred)
{
	struct passwd pw;
	struct passwd *res = NULL;
	char buf[1024];

	peer->uid = (unsigned int)cred->uid;
	peer->pid = (int)cred->pid;
	if (getpwuid_r(cred->uid, &pw, buf, sizeof(buf), &res) == 0 && res &&
	    res->pw_name && res->pw_name[0])
		snprintf(peer->name, sizeof(peer->name), "%s", res->pw_name);
	else
		snprintf(peer->name, sizeof(peer->name), "uid %u",
			 (unsigned int)cred->uid);
}

static int peer_cred(int fd, struct ucred *cred)
{
	socklen_t len = sizeof(*cred);

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, cred, &len) < 0)
		return -errno;
	return 0;
}

static int send_line(int fd, const char *line)
{
	size_t len = strlen(line);

	if (send(fd, line, len, MSG_NOSIGNAL) != (ssize_t)len)
		return -EIO;
	return 0;
}

/*
 * Read one newline-terminated line, byte by byte so a following line stays in
 * the socket.  Returns the length (without the newline), 0 on EOF, -ETIMEDOUT
 * or another negative errno.
 */
static int read_line(int fd, char *buf, size_t size, int timeout_ms)
{
	size_t used = 0;

	while (used + 1 < size) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		char c;
		ssize_t n;
		int ret = poll(&pfd, 1, timeout_ms);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0)
			return -ETIMEDOUT;
		n = recv(fd, &c, 1, MSG_DONTWAIT);
		if (n == 0)
			return 0;
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return errno == ECONNRESET ? 0 : -errno;
		}
		if (c == '\n') {
			buf[used] = '\0';
			return (int)used;
		}
		buf[used++] = c;
	}
	return -EMSGSIZE;
}

/* ---- Holder side ------------------------------------------------------ */

/*
 * "May this peer ask for the card?"  Approximated by "could it open the VFIO
 * node itself": same user, root, world-accessible node, or a member of the
 * node's group.  Users who only reach the node through an ACL are not
 * recognised and keep today's behaviour (the card stays with its holder).
 */
static bool peer_allowed(int fd, const struct ucred *cred)
{
	gid_t groups[HANDOFF_MAX_GROUPS];
	socklen_t len = sizeof(groups);
	size_t count, i;

	if (cred->uid == getuid() || cred->uid == 0)
		return true;
	if (!hs.have_node)
		return false;
	if (hs.node_any)
		return true;
	if (!hs.node_group)
		return false;
	if (cred->gid == hs.node_gid)
		return true;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERGROUPS, groups, &len) < 0)
		return false;
	count = len / sizeof(groups[0]);
	for (i = 0; i < count; i++) {
		if (groups[i] == hs.node_gid)
			return true;
	}
	return false;
}

static int bind_listener(void)
{
	struct sockaddr_un addr;
	socklen_t len = handoff_addr(&addr, hs.card_id);
	int fd;

	if (!len)
		return -ENAMETOOLONG;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -errno;
	if (bind(fd, (struct sockaddr *)&addr, len) < 0 || listen(fd, 8) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
}

/* Returns true when the request was accepted and the thread should stop. */
static bool handle_client(int fd)
{
	struct ucred cred;
	char line[HANDOFF_LINE_MAX];
	bool resume;
	int n;

	if (peer_cred(fd, &cred) < 0)
		goto drop;
	if (!peer_allowed(fd, &cred)) {
		(void)send_line(fd, "DENIED\n");
		goto drop;
	}

	n = read_line(fd, line, sizeof(line), HANDOFF_HELLO_MS);
	if (n <= 0 || strncmp(line, HANDOFF_MAGIC, strlen(HANDOFF_MAGIC)) != 0)
		goto drop;
	if (strcmp(line + strlen(HANDOFF_MAGIC), "take") == 0)
		resume = false;
	else if (strcmp(line + strlen(HANDOFF_MAGIC), "resume") == 0)
		resume = true;
	else
		goto drop;

	if (resume && hs.role == EVERLIVE_HANDOFF_ROLE_ENGINE) {
		/* Keep capturing; the peer learns from EOF when we are done. */
		if (send_line(fd, "BUSY\n") < 0)
			goto drop;
		pthread_mutex_lock(&hs.lock);
		if (hs.nr_watchers < HANDOFF_MAX_WATCHERS) {
			hs.watchers[hs.nr_watchers++] = fd;
			fd = -1;
		}
		pthread_mutex_unlock(&hs.lock);
		if (fd >= 0)
			goto drop;
		return false;
	}

	if (send_line(fd, "OK\n") < 0)
		goto drop;

	pthread_mutex_lock(&hs.lock);
	hs.pending_fd = fd;
	fill_peer(&hs.pending_peer, &cred);
	pthread_mutex_unlock(&hs.lock);

	if (hs.cb)
		hs.cb(hs.user);
	return true;

drop:
	close(fd);
	return false;
}

static bool wake_requested(int timeout_ms)
{
	struct pollfd pfd = { .fd = hs.wake_fd, .events = POLLIN };

	return poll(&pfd, 1, timeout_ms) > 0;
}

static void *handoff_thread(void *arg)
{
	(void)arg;

	for (;;) {
		struct pollfd pfds[2];
		int listen_fd;
		int fd;

		pthread_mutex_lock(&hs.lock);
		listen_fd = hs.listen_fd;
		pthread_mutex_unlock(&hs.lock);

		if (listen_fd < 0) {
			/* The previous holder may still own the name. */
			fd = bind_listener();
			if (fd < 0) {
				if (wake_requested(HANDOFF_REBIND_MS))
					break;
				continue;
			}
			pthread_mutex_lock(&hs.lock);
			hs.listen_fd = fd;
			pthread_mutex_unlock(&hs.lock);
			listen_fd = fd;
		}

		pfds[0].fd = listen_fd;
		pfds[0].events = POLLIN;
		pfds[1].fd = hs.wake_fd;
		pfds[1].events = POLLIN;
		if (poll(pfds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfds[1].revents)
			break;
		if (!(pfds[0].revents & POLLIN))
			continue;

		fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0)
			continue;
		if (handle_client(fd))
			break;
	}
	return NULL;
}

int everlive_handoff_serve(const char *card_id, const char *access_node,
			   int role, everlive_handoff_cb cb, void *user)
{
	struct stat st;
	int ret;

	if (!card_id || !card_id[0] || strlen(card_id) >= sizeof(hs.card_id))
		return -EINVAL;
	if (hs.thread_running)
		return 0;

	snprintf(hs.card_id, sizeof(hs.card_id), "%s", card_id);
	hs.role = role;
	hs.cb = cb;
	hs.user = user;
	hs.have_node = false;
	if (access_node && stat(access_node, &st) == 0) {
		hs.have_node = true;
		hs.node_any = (st.st_mode & (S_IROTH | S_IWOTH)) ==
			      (S_IROTH | S_IWOTH);
		hs.node_group = (st.st_mode & (S_IRGRP | S_IWGRP)) ==
				(S_IRGRP | S_IWGRP);
		hs.node_gid = st.st_gid;
	}

	hs.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (hs.wake_fd < 0)
		return -errno;

	ret = pthread_create(&hs.thread, NULL, handoff_thread, NULL);
	if (ret != 0) {
		close(hs.wake_fd);
		hs.wake_fd = -1;
		return -ret;
	}
	hs.thread_running = true;
	return 0;
}

bool everlive_handoff_pending(struct everlive_handoff_peer *peer)
{
	bool pending;

	pthread_mutex_lock(&hs.lock);
	pending = hs.pending_fd >= 0;
	if (pending && peer)
		*peer = hs.pending_peer;
	pthread_mutex_unlock(&hs.lock);
	return pending;
}

static void stop_thread(void)
{
	if (hs.thread_running) {
		uint64_t one = 1;

		if (write(hs.wake_fd, &one, sizeof(one)) < 0) {
			/* Nothing to do: the thread also exits on its own. */
		}
		pthread_join(hs.thread, NULL);
		hs.thread_running = false;
	}
	if (hs.wake_fd >= 0) {
		close(hs.wake_fd);
		hs.wake_fd = -1;
	}

	pthread_mutex_lock(&hs.lock);
	if (hs.listen_fd >= 0) {
		close(hs.listen_fd);
		hs.listen_fd = -1;
	}
	while (hs.nr_watchers)
		close(hs.watchers[--hs.nr_watchers]);
	pthread_mutex_unlock(&hs.lock);
}

int everlive_handoff_release(void)
{
	int fd;

	/* Free the name first so the new holder can bind it straight away. */
	stop_thread();

	pthread_mutex_lock(&hs.lock);
	fd = hs.pending_fd;
	hs.pending_fd = -1;
	pthread_mutex_unlock(&hs.lock);

	if (fd >= 0 && send_line(fd, "RELEASED\n") < 0) {
		close(fd);
		fd = -1;
	}
	return fd;
}

void everlive_handoff_stop(void)
{
	stop_thread();

	pthread_mutex_lock(&hs.lock);
	if (hs.pending_fd >= 0) {
		close(hs.pending_fd);
		hs.pending_fd = -1;
	}
	pthread_mutex_unlock(&hs.lock);
}

/* ---- Requester side --------------------------------------------------- */

int everlive_handoff_connect(const char *card_id,
			     struct everlive_handoff_peer *holder)
{
	struct sockaddr_un addr;
	struct ucred cred;
	socklen_t len;
	int fd;
	int ret;

	if (!card_id || !card_id[0])
		return -EINVAL;
	len = handoff_addr(&addr, card_id);
	if (!len)
		return -ENAMETOOLONG;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	if (connect(fd, (struct sockaddr *)&addr, len) < 0) {
		int err = errno;

		close(fd);
		return (err == ECONNREFUSED || err == ENOENT) ? -ENOENT : -err;
	}

	ret = peer_cred(fd, &cred);
	if (ret < 0) {
		close(fd);
		return ret;
	}
	if (holder)
		fill_peer(holder, &cred);
	return fd;
}

int everlive_handoff_request(int fd, bool resume, int timeout_ms)
{
	char line[HANDOFF_LINE_MAX];
	int n;

	if (send_line(fd, resume ? HANDOFF_MAGIC "resume\n"
				 : HANDOFF_MAGIC "take\n") < 0)
		return -EAGAIN;

	n = read_line(fd, line, sizeof(line), HANDOFF_HELLO_MS);
	if (n <= 0)
		return -EAGAIN;
	if (strcmp(line, "DENIED") == 0)
		return -EACCES;
	if (strcmp(line, "BUSY") == 0)
		return -EBUSY;
	if (strcmp(line, "OK") != 0)
		return -EAGAIN;

	n = read_line(fd, line, sizeof(line), timeout_ms);
	if (n == 0)
		return EVERLIVE_HANDOFF_GONE;
	if (n == -ETIMEDOUT)
		return -ETIMEDOUT;
	if (n < 0)
		return -EAGAIN;
	if (strcmp(line, "RELEASED") != 0)
		return -EAGAIN;
	return EVERLIVE_HANDOFF_RELEASED;
}

int everlive_handoff_wait(int fd, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	char buf[64];
	ssize_t n;

	if (fd < 0)
		return 1;
	if (poll(&pfd, 1, timeout_ms) <= 0)
		return 0;
	n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
	if (n > 0)
		return 0;
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return 0;
	return 1;
}

/* ---- Paused-state marker ---------------------------------------------- */

int everlive_handoff_state_path(char *buf, size_t size)
{
	char dir[EVERLIVE_IPC_PATH_MAX];
	int ret = everlive_ipc_runtime_dir(dir, sizeof(dir));
	int n;

	if (ret < 0)
		return ret;
	n = snprintf(buf, size, "%s/%s", dir, EVERLIVE_HANDOFF_STATE_NAME);
	if (n < 0 || (size_t)n >= size)
		return -ENAMETOOLONG;
	return 0;
}

int everlive_handoff_state_write(const char *card_id,
				 const struct everlive_handoff_peer *holder)
{
	char dir[EVERLIVE_IPC_PATH_MAX];
	char path[EVERLIVE_IPC_PATH_MAX + 32];
	char tmp[EVERLIVE_IPC_PATH_MAX + 40];
	FILE *f;
	int ret;

	ret = everlive_ipc_runtime_dir(dir, sizeof(dir));
	if (ret < 0)
		return ret;
	if (mkdir(dir, 0700) < 0 && errno != EEXIST)
		return -errno;
	ret = everlive_handoff_state_path(path, sizeof(path));
	if (ret < 0)
		return ret;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	f = fopen(tmp, "we");
	if (!f)
		return -errno;
	fprintf(f, "pid=%d\nholder_uid=%u\nholder=%s\ncard=%s\n",
		(int)getpid(), holder ? holder->uid : 0U,
		holder ? holder->name : "", card_id ? card_id : "");
	if (fclose(f) != 0 || rename(tmp, path) < 0) {
		int err = errno;

		unlink(tmp);
		return -err;
	}
	return 0;
}

void everlive_handoff_state_clear(void)
{
	char path[EVERLIVE_IPC_PATH_MAX + 32];

	if (everlive_handoff_state_path(path, sizeof(path)) == 0)
		unlink(path);
}

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_handoff.h — Cross-user capture card handoff
 *
 * VFIO gives a card to exactly one process.  When two users on the same
 * machine both want to capture, the second one would otherwise have to ask
 * the first to shut everything down by hand.  This module lets whichever
 * process holds the card be asked to release it:
 *
 *   - The holder listens on an abstract Unix socket named after the card
 *     ("everlive-handoff-<pci bdf>").  Abstract names need no shared
 *     directory and vanish with the process.
 *   - A requester connects and sends one line.  The holder checks the peer
 *     with SO_PEERCRED / SO_PEERGROUPS: only users who could open the card's
 *     VFIO node themselves may ask.
 *   - An engine that yields keeps the connection open.  The requester holds
 *     its end for as long as it captures, so EOF on that connection — clean
 *     exit or crash alike — tells the paused engine the card is free again.
 *
 * Two request modes exist.  "take" is a user starting capture: an engine
 * yields to it.  "resume" is a paused engine coming back by itself: an engine
 * that is actively capturing refuses it and the caller keeps waiting, so an
 * automatic resume never interrupts somebody else's session.  An idle keeper
 * (LED daemon) yields to both by exiting.
 *
 * Wire format, newline-terminated ASCII:
 *
 *     requester: EVERLIVE-HANDOFF 1 take|resume
 *     holder:    OK          then later RELEASED (or EOF when it exits)
 *                BUSY        resume refused; connection stays open as a watch
 *                DENIED      peer may not access the card
 */
#ifndef EVERLIVE_HANDOFF_H
#define EVERLIVE_HANDOFF_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVERLIVE_HANDOFF_STATE_NAME	"everlive.paused"

enum everlive_handoff_role {
	/* Active capture: yields to "take", refuses "resume". */
	EVERLIVE_HANDOFF_ROLE_ENGINE = 0,
	/* Idle holder (LED keeper): yields to every request. */
	EVERLIVE_HANDOFF_ROLE_KEEPER = 1,
};

/* everlive_handoff_request() results (>= 0); errors are negative errno. */
#define EVERLIVE_HANDOFF_RELEASED	0	/* card free; fd is the lease */
#define EVERLIVE_HANDOFF_GONE		1	/* card free; holder exited */

struct everlive_handoff_peer {
	unsigned int uid;
	int pid;
	char name[64];	/* login name, or "uid <n>" */
};

/*
 * Called once from the handoff thread when a request has been accepted.
 * Keep it async-safe in spirit: set a flag and return.
 */
typedef void (*everlive_handoff_cb)(void *user);

/* ---- Holder side ------------------------------------------------------ */

/**
 * everlive_handoff_serve - Start answering handoff requests for a card
 * @card_id: Stable card identifier (PCI BDF)
 * @access_node: Device node whose permissions decide who may ask
 *               (e.g. "/dev/vfio/30"); NULL restricts to the own UID
 * @role: enum everlive_handoff_role
 * @cb: Invoked when a request was accepted
 * @user: Opaque pointer for @cb
 *
 * If the socket name is still held (a predecessor that is shutting down),
 * the thread keeps retrying once per second.  Returns 0 or a negative errno;
 * a failure only means this process cannot be asked to yield.
 */
int everlive_handoff_serve(const char *card_id, const char *access_node,
			   int role, everlive_handoff_cb cb, void *user);

/**
 * everlive_handoff_pending - True once a request has been accepted
 * @peer: Optional; receives the requester's identity
 */
bool everlive_handoff_pending(struct everlive_handoff_peer *peer);

/**
 * everlive_handoff_release - Tell the requester the card is free
 *
 * Call after the hardware has been closed.  Frees the socket name for the
 * new holder and returns the connection to the requester (wait on it with
 * everlive_handoff_wait()), or -1 when no request was pending.
 */
int everlive_handoff_release(void);

/**
 * everlive_handoff_stop - Stop serving and drop every connection
 *
 * Safe to call when never started and safe to call twice.
 */
void everlive_handoff_stop(void);

/* ---- Requester side --------------------------------------------------- */

/**
 * everlive_handoff_connect - Find the current holder of a card
 *
 * Returns a connected fd and fills @holder, -ENOENT when nobody is
 * listening, or another negative errno.
 */
int everlive_handoff_connect(const char *card_id,
			     struct everlive_handoff_peer *holder);

/**
 * everlive_handoff_request - Ask the holder to release the card
 * @fd: From everlive_handoff_connect()
 * @resume: Use the non-preempting "resume" mode
 * @timeout_ms: How long to wait for the release after it was accepted
 *
 * Returns
 *   EVERLIVE_HANDOFF_RELEASED  keep @fd open for as long as the card is used
 *   EVERLIVE_HANDOFF_GONE      holder exited; close @fd
 *   -EBUSY      resume refused; @fd stays valid, EOF on it means "try again"
 *   -EACCES     not permitted
 *   -EAGAIN     holder is already yielding to someone else, or went away
 *   -ETIMEDOUT  holder accepted but did not release in time
 * The caller closes @fd in every case except RELEASED and -EBUSY.
 */
int everlive_handoff_request(int fd, bool resume, int timeout_ms);

/**
 * everlive_handoff_wait - Wait for the other end of a handoff connection
 *
 * Returns 1 when the peer is gone, 0 on timeout or when interrupted by a
 * signal (so callers can re-check their flags).
 */
int everlive_handoff_wait(int fd, int timeout_ms);

/* ---- Paused-state marker for UIs --------------------------------------- */

/*
 * A paused engine publishes <runtime_dir>/everlive.paused so the Control Hub
 * can show who has the card and offer to take it back (SIGUSR1 to @pid).
 */
int everlive_handoff_state_path(char *buf, size_t size);
int everlive_handoff_state_write(const char *card_id,
				 const struct everlive_handoff_peer *holder);
void everlive_handoff_state_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_HANDOFF_H */

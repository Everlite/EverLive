/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_log.h — Line-buffered, timestamped stdout/stderr for the
 * GC573 streamer, so streamer.log shows when each step happened.
 */

#ifndef GC573_USER_LOG_H
#define GC573_USER_LOG_H

/**
 * gc573_user_log_init - Make stdout/stderr line-buffered and, unless
 * EVERLIVE_LOG_TIMESTAMPS=0, prefix every line with the time since start
 * as "[  12.345] ".  Call once from main(), after argument parsing
 * (so --help stays plain) and before the startup banner.
 */
void gc573_user_log_init(void);

/** Wall-clock time gc573_user_log_init() ran, for the log banner. */
const char *gc573_user_log_start_time(void);

#endif /* GC573_USER_LOG_H */

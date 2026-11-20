/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hws_log.h — Line-buffered, timestamped stdout/stderr for the
 * HWS UHD streamer, so streamer.log shows when each step happened.
 */

#ifndef HWS_LOG_H
#define HWS_LOG_H

/**
 * hws_log_init - Make stdout/stderr line-buffered and, unless
 * EVERLIVE_LOG_TIMESTAMPS=0, prefix every line with the time since start
 * as "[  12.345] ".  Call once from main(), after argument parsing
 * (so --help stays plain) and before the startup banner.
 */
void hws_log_init(void);

/** Wall-clock time hws_log_init() ran, for the log banner. */
const char *hws_log_start_time(void);

#endif /* HWS_LOG_H */

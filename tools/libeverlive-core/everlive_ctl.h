/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_ctl.h — JSON-RPC request helpers for everlive-ctl
 *
 * These helpers are stateless and socket-free so they can be unit tested
 * without a running capture engine.
 */

#ifndef EVERLIVE_CTL_H
#define EVERLIVE_CTL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Serialize a JSON-RPC 2.0 request.  Returns 0 on success, -1 if the result
 * does not fit in @out. */
int everlive_ctl_build_request(const char *method, const char *params,
			       char *out, size_t out_size);

/* Escape @in for embedding inside a JSON string literal. */
void everlive_ctl_json_escape(const char *in, char *out, size_t out_size);

/* True when the response has a top-level-style "error" key (quoted key
 * followed by a colon).  Ignores the word "error" appearing in values. */
bool everlive_ctl_response_has_error(const char *json);

/* Parse a finite base-10 double, rejecting trailing garbage and NaN/Inf.
 * Returns 0 on success, -1 on invalid input. */
int everlive_ctl_parse_double(const char *value, double *out);

/* Parse a 24-bit RRGGBB colour given in hex.  Returns 0 on success. */
int everlive_ctl_parse_hex_rgb(const char *value, unsigned long *out);

/* Build the "params" object for the numeric runtime commands.  Each returns 0
 * on success or -1 on invalid input / insufficient buffer space. */
int everlive_ctl_format_exposure(double ev, char *out, size_t out_size);
int everlive_ctl_format_volume(double percent, char *out, size_t out_size);
int everlive_ctl_format_led(unsigned long rgb, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_CTL_H */

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_ctl_proto.c — Stateless JSON-RPC request helpers for everlive-ctl
 *
 * Kept separate from everlive_ctl.c so the serialization and validation logic
 * can be exercised by the unit tests without a control socket.
 */

#include "everlive_ctl.h"
#include "everlive_json.h"

#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int everlive_ctl_build_request(const char *method, const char *params,
			       char *out, size_t out_size)
{
	int n;

	if (!method || !out || out_size == 0)
		return -1;

	n = snprintf(out, out_size,
		     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}\n",
		     method, params && *params ? params : "{}");
	return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

void everlive_ctl_json_escape(const char *in, char *out, size_t out_size)
{
	everlive_json_escape_into(out, out_size, in);
}

bool everlive_ctl_response_has_error(const char *json)
{
	return everlive_json_has_top_level_key(json, "error");
}

int everlive_ctl_parse_double(const char *value, double *out)
{
	char *end = NULL;
	double v;

	if (!value || !*value || !out)
		return -1;

	errno = 0;
	v = strtod(value, &end);
	if (end == value || *end != '\0' || errno == ERANGE)
		return -1;

	/* NaN and +/-Inf are not valid JSON numbers. */
	if (!(v >= -DBL_MAX && v <= DBL_MAX))
		return -1;

	*out = v;
	return 0;
}

int everlive_ctl_parse_hex_rgb(const char *value, unsigned long *out)
{
	char *end = NULL;
	unsigned long v;

	if (!value || !*value || !out)
		return -1;

	errno = 0;
	v = strtoul(value, &end, 16);
	if (end == value || *end != '\0' || errno == ERANGE || v > 0xffffffUL)
		return -1;

	*out = v;
	return 0;
}

int everlive_ctl_format_exposure(double ev, char *out, size_t out_size)
{
	int n = snprintf(out, out_size, "{\"exposure\":%g}", ev);

	return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

int everlive_ctl_format_volume(double percent, char *out, size_t out_size)
{
	int n;

	if (!(percent >= 0.0 && percent <= 150.0))
		return -1;

	n = snprintf(out, out_size, "{\"gain\":%g}", percent / 100.0);
	return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

int everlive_ctl_format_led(unsigned long rgb, char *out, size_t out_size)
{
	int n;

	if (rgb > 0xffffffUL)
		return -1;

	n = snprintf(out, out_size, "{\"r\":%lu,\"g\":%lu,\"b\":%lu}",
		     (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
	return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_ctl_proto.c — Stateless JSON-RPC request helpers for everlive-ctl
 *
 * Kept separate from everlive_ctl.c so the serialization and validation logic
 * can be exercised by the unit tests without a control socket.
 */

#include "everlive_ctl.h"

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
	size_t o = 0;

	if (!out || out_size == 0)
		return;

	if (!in) {
		out[0] = '\0';
		return;
	}

	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		if (o + 8 >= out_size)
			break;
		switch (*p) {
		case '"':
			out[o++] = '\\';
			out[o++] = '"';
			break;
		case '\\':
			out[o++] = '\\';
			out[o++] = '\\';
			break;
		case '\n':
			out[o++] = '\\';
			out[o++] = 'n';
			break;
		case '\r':
			out[o++] = '\\';
			out[o++] = 'r';
			break;
		case '\t':
			out[o++] = '\\';
			out[o++] = 't';
			break;
		default:
			if (*p < 0x20)
				o += (size_t)snprintf(out + o, out_size - o,
						      "\\u%04x", *p);
			else
				out[o++] = (char)*p;
			break;
		}
	}
	out[o] = '\0';
}

bool everlive_ctl_response_has_error(const char *json)
{
	const char *p = json;

	if (!json)
		return false;

	while ((p = strstr(p, "\"error\"")) != NULL) {
		const char *q = p + 7; /* strlen("\"error\"") */

		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
			q++;
		if (*q == ':')
			return true;
		p = q;
	}

	return false;
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

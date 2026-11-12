/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_json.c — jsmn-backed JSON-RPC 2.0 scanners and formatters
 *
 * The wire protocol is newline-delimited JSON-RPC 2.0 over AF_UNIX.  Requests
 * are flat objects, so a token scan is enough; a full parser would add a
 * dependency for no benefit.  Unknown fields are ignored and the raw "id"
 * token is echoed back verbatim.
 */

#include "everlive_json.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The jsmntok_t layout depends on JSMN_PARENT_LINKS.  It MUST match
 * third_party/jsmn.o, which defines the implementation; this file only needs
 * the declarations plus the same struct layout.
 */
#define JSMN_PARENT_LINKS
#define JSMN_HEADER
#include "third_party/jsmn.h"

typedef enum {
	JSON_GET_STRING,
	JSON_GET_LONG,
	JSON_GET_DOUBLE,
	JSON_GET_BOOL,
	JSON_GET_RAW
} json_get_type;

/*
 * Find a key in the JSON token tree.  Direct children of the root object are
 * matched first so a value string that happens to equal the key name (for
 * example {"params":{"text":"method"},"method":"led.set"}) can never shadow
 * the real top-level key.
 *
 * With JSMN_PARENT_LINKS a key token's parent is the containing object, while
 * a value token's parent is the preceding key token.  Matching on
 * parent == 0 therefore selects root-level keys only.
 */
static int json_token_find_key(const jsmntok_t *tokens, int tok_count,
			       const char *key, const char *json)
{
	const size_t klen = strlen(key);

	/* Pass 1: keys of the root object (token 0). */
	for (int i = 1; i < tok_count; i++) {
		size_t len;

		if (tokens[i].type != JSMN_STRING || tokens[i].parent != 0)
			continue;
		len = (size_t)(tokens[i].end - tokens[i].start);
		if (len == klen && memcmp(json + tokens[i].start, key, klen) == 0) {
			if (i + 1 < tok_count)
				return i + 1;
		}
	}

	/* Pass 2: keys of any nested object (e.g. inside "params"). */
	for (int i = 1; i < tok_count; i++) {
		size_t len;

		if (tokens[i].type != JSMN_STRING)
			continue;
		if (tokens[i].parent <= 0 ||
		    tokens[tokens[i].parent].type != JSMN_OBJECT)
			continue;
		len = (size_t)(tokens[i].end - tokens[i].start);
		if (len == klen && memcmp(json + tokens[i].start, key, klen) == 0) {
			if (i + 1 < tok_count)
				return i + 1;
		}
	}

	return -1;
}

static bool json_token_to_string(const jsmntok_t *tok, const char *json,
				 char *out, size_t outsz)
{
	int len;

	if (!tok || tok->type != JSMN_STRING)
		return false;
	len = tok->end - tok->start;
	if (len < 0 || (size_t)len >= outsz)
		return false;
	memcpy(out, json + tok->start, (size_t)len);
	out[len] = '\0';
	return true;
}

static bool json_token_to_long(const jsmntok_t *tok, const char *json,
			       long *out)
{
	char buf[64];
	char *end;
	long val;
	int len;

	if (!tok || tok->type != JSMN_PRIMITIVE || !out)
		return false;
	len = tok->end - tok->start;
	if (len < 0 || len >= (int)sizeof(buf))
		return false;
	memcpy(buf, json + tok->start, (size_t)len);
	buf[len] = '\0';
	errno = 0;
	val = strtol(buf, &end, 10);
	if (errno || end == buf || *end != '\0')
		return false;
	*out = val;
	return true;
}

static bool json_token_to_double(const jsmntok_t *tok, const char *json,
				 double *out)
{
	char buf[64];
	char *end;
	double val;
	int len;

	if (!tok || tok->type != JSMN_PRIMITIVE || !out)
		return false;
	len = tok->end - tok->start;
	if (len < 0 || len >= (int)sizeof(buf))
		return false;
	memcpy(buf, json + tok->start, (size_t)len);
	buf[len] = '\0';
	errno = 0;
	val = strtod(buf, &end);
	if (errno || end == buf || *end != '\0' || !isfinite(val))
		return false;
	*out = val;
	return true;
}

static bool json_token_to_bool(const jsmntok_t *tok, const char *json,
			       bool *out)
{
	int len;

	if (!tok || tok->type != JSMN_PRIMITIVE || !out)
		return false;
	len = tok->end - tok->start;
	if (len == 4 && strncmp(json + tok->start, "true", 4) == 0) {
		*out = true;
		return true;
	}
	if (len == 5 && strncmp(json + tok->start, "false", 5) == 0) {
		*out = false;
		return true;
	}
	return false;
}

static bool json_token_to_raw(const jsmntok_t *tok, const char *json,
			      char *out, size_t outsz)
{
	int len;

	if (!tok || !json || !out || outsz == 0)
		return false;
	len = tok->end - tok->start;
	if (len < 0 || (size_t)len >= outsz)
		return false;
	memcpy(out, json + tok->start, (size_t)len);
	out[len] = '\0';
	return true;
}

static bool json_parse_request(const char *input, size_t len,
			       const char *key, char *out, size_t outsz,
			       json_get_type type, void *num_out)
{
	jsmn_parser parser;
	jsmntok_t tokens[EVERLIVE_JSON_MAX_TOKENS];
	const jsmntok_t *val;
	int tok_count;
	int val_idx;

	if (!input || !key || len >= EVERLIVE_JSON_MAX_INPUT)
		return false;

	jsmn_init(&parser);
	tok_count = jsmn_parse(&parser, input, len, tokens,
			       EVERLIVE_JSON_MAX_TOKENS);
	if (tok_count < 1 || tokens[0].type != JSMN_OBJECT)
		return false;

	val_idx = json_token_find_key(tokens, tok_count, key, input);
	if (val_idx < 0)
		return false;

	val = &tokens[val_idx];
	switch (type) {
	case JSON_GET_STRING:
		return json_token_to_string(val, input, out, outsz);
	case JSON_GET_LONG:
		return json_token_to_long(val, input, (long *)num_out);
	case JSON_GET_DOUBLE:
		return json_token_to_double(val, input, (double *)num_out);
	case JSON_GET_BOOL:
		return json_token_to_bool(val, input, (bool *)num_out);
	case JSON_GET_RAW:
		return json_token_to_raw(val, input, out, outsz);
	}
	return false;
}

bool everlive_json_get_string(const char *json, const char *key,
			      char *out, size_t outsz)
{
	if (!json || !key || !out || outsz == 0)
		return false;
	return json_parse_request(json, strlen(json), key, out, outsz,
				  JSON_GET_STRING, NULL);
}

bool everlive_json_get_long(const char *json, const char *key, long *out)
{
	if (!json || !key || !out)
		return false;
	return json_parse_request(json, strlen(json), key, NULL, 0,
				  JSON_GET_LONG, out);
}

bool everlive_json_get_double(const char *json, const char *key, double *out)
{
	if (!json || !key || !out)
		return false;
	return json_parse_request(json, strlen(json), key, NULL, 0,
				  JSON_GET_DOUBLE, out);
}

bool everlive_json_get_bool(const char *json, const char *key, bool *out)
{
	if (!json || !key || !out)
		return false;
	return json_parse_request(json, strlen(json), key, NULL, 0,
				  JSON_GET_BOOL, out);
}

bool everlive_json_get_raw(const char *json, const char *key,
			   char *out, size_t outsz)
{
	if (!json || !key || !out || outsz == 0)
		return false;
	return json_parse_request(json, strlen(json), key, out, outsz,
				  JSON_GET_RAW, NULL);
}

bool everlive_json_find_value(const char *json, const char *key,
			      char *out, size_t outsz)
{
	if (!json || !key || !out || outsz == 0)
		return false;

	/* "method" is a string; everything else keeps its raw token form. */
	if (strcmp(key, "method") == 0)
		return everlive_json_get_string(json, key, out, outsz);

	return everlive_json_get_raw(json, key, out, outsz);
}

/* Quote-then-colon scan used only when jsmn cannot fit the document. */
static bool json_scan_key(const char *json, const char *key)
{
	const size_t klen = strlen(key);
	const char *p = json;

	while ((p = strstr(p, "\"")) != NULL) {
		const char *q;

		p++;
		if (strncmp(p, key, klen) != 0 || p[klen] != '"')
			continue;
		q = p + klen + 1;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
			q++;
		if (*q == ':')
			return true;
	}

	return false;
}

bool everlive_json_has_top_level_key(const char *json, const char *key)
{
	jsmn_parser parser;
	jsmntok_t tokens[EVERLIVE_JSON_RESPONSE_TOKENS];
	int tok_count;

	if (!json || !key || !*key)
		return false;

	jsmn_init(&parser);
	tok_count = jsmn_parse(&parser, json, strlen(json), tokens,
			       EVERLIVE_JSON_RESPONSE_TOKENS);
	if (tok_count < 1 || tokens[0].type != JSMN_OBJECT)
		return json_scan_key(json, key);

	for (int i = 1; i < tok_count; i++) {
		size_t len;

		if (tokens[i].type != JSMN_STRING || tokens[i].parent != 0)
			continue;
		len = (size_t)(tokens[i].end - tokens[i].start);
		if (len == strlen(key) &&
		    memcmp(json + tokens[i].start, key, len) == 0)
			return true;
	}

	return false;
}

void everlive_json_escape_into(char *dst, size_t dstsz, const char *src)
{
	const unsigned char *p;
	size_t o = 0;

	if (!dst || dstsz == 0)
		return;

	if (!src) {
		dst[0] = '\0';
		return;
	}

	for (p = (const unsigned char *)src; *p; p++) {
		const char *esc = NULL;

		switch (*p) {
		case '"':
			esc = "\\\"";
			break;
		case '\\':
			esc = "\\\\";
			break;
		case '\n':
			esc = "\\n";
			break;
		case '\r':
			esc = "\\r";
			break;
		case '\t':
			esc = "\\t";
			break;
		default:
			break;
		}

		if (esc) {
			if (o + 2 >= dstsz)
				break;
			dst[o++] = esc[0];
			dst[o++] = esc[1];
		} else if (*p < 0x20) {
			char ubuf[7];

			if (o + 6 >= dstsz)
				break;
			snprintf(ubuf, sizeof(ubuf), "\\u%04x", *p);
			memcpy(dst + o, ubuf, 6);
			o += 6;
		} else {
			if (o + 1 >= dstsz)
				break;
			dst[o++] = (char)*p;
		}
	}

	dst[o] = '\0';
}

/* Canonical JSON integer: -?(0|[1-9][0-9]*). */
static bool json_is_canonical_int(const char *s)
{
	if (!s || !*s)
		return false;
	if (*s == '-')
		s++;
	if (!*s)
		return false;
	if (*s == '0')
		return s[1] == '\0';
	if (*s < '1' || *s > '9')
		return false;
	for (s++; *s; s++) {
		if (*s < '0' || *s > '9')
			return false;
	}
	return true;
}

void everlive_json_format_id(char *dst, size_t dstsz, const char *id)
{
	char inner[EVERLIVE_JSON_ID_MAX];
	const char *body = id;
	size_t len;
	size_t elen;

	if (!dst || dstsz == 0)
		return;

	if (!id || !*id || strcmp(id, "null") == 0) {
		snprintf(dst, dstsz, "null");
		return;
	}

	if (json_is_canonical_int(id)) {
		snprintf(dst, dstsz, "%s", id);
		return;
	}

	if (dstsz < 3) {
		snprintf(dst, dstsz, "null");
		return;
	}

	len = strlen(id);
	if (id[0] == '"' && len >= 2 && id[len - 1] == '"') {
		size_t in_len = len - 2;

		if (in_len >= sizeof(inner))
			in_len = sizeof(inner) - 1;
		memcpy(inner, id + 1, in_len);
		inner[in_len] = '\0';
		body = inner;
	}

	dst[0] = '"';
	everlive_json_escape_into(dst + 1, dstsz - 2, body);
	elen = strlen(dst);
	dst[elen] = '"';
	dst[elen + 1] = '\0';
}

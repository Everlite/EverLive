/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_json.h — Flat-object JSON-RPC 2.0 helpers for EverLive
 *
 * Stateless, allocation-free scanners built on the vendored jsmn parser.
 * Every function is re-entrant: all scratch space lives on the caller's
 * stack, so the GC573 control thread and the GC570D daemon loop may call
 * these while the render/DMA paths are live.
 */

#ifndef EVERLIVE_JSON_H
#define EVERLIVE_JSON_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Request payloads are small.  Responses (status.get) are ~135 tokens today;
 * a 128-token budget returns JSMN_ERROR_NOMEM, so responses get 512.
 */
#define EVERLIVE_JSON_MAX_TOKENS	256U
#define EVERLIVE_JSON_RESPONSE_TOKENS	512U
#define EVERLIVE_JSON_MAX_INPUT		(64U * 1024U)
#define EVERLIVE_JSON_ID_MAX		48U

/*
 * Typed getters.  Return false when the key is absent, the document is
 * malformed, or the value type does not match.
 */
bool everlive_json_get_string(const char *json, const char *key,
			      char *out, size_t outsz);
bool everlive_json_get_long(const char *json, const char *key, long *out);
bool everlive_json_get_double(const char *json, const char *key, double *out);
bool everlive_json_get_bool(const char *json, const char *key, bool *out);
bool everlive_json_get_raw(const char *json, const char *key,
			   char *out, size_t outsz);

/*
 * Compatibility helper used by the IPC dispatchers: "method" is extracted as
 * an unquoted string, every other key as its raw token (number, null, or the
 * unquoted contents of a string).  Preserves `id` echo semantics.
 */
bool everlive_json_find_value(const char *json, const char *key,
			      char *out, size_t outsz);

/*
 * True when the root object has @key as a direct child.  Replaces substring
 * scanning for JSON-RPC error detection.  Falls back to a quote+colon scan
 * when the document cannot be parsed (e.g. a truncated reply).
 */
bool everlive_json_has_top_level_key(const char *json, const char *key);

/*
 * Escape @src as the body of a JSON string literal.  Always NUL-terminates
 * and never writes past dstsz-1.  dst == NULL or dstsz == 0 is a no-op;
 * src == NULL writes an empty string.
 */
void everlive_json_escape_into(char *dst, size_t dstsz, const char *src);

/*
 * Normalise a JSON-RPC "id" token for echoing: NULL/empty/"null" -> null,
 * canonical integers are emitted verbatim, everything else is quoted and
 * re-escaped.  dstsz < 3 yields "null".
 */
void everlive_json_format_id(char *dst, size_t dstsz, const char *id);

#ifdef __cplusplus
}
#endif

#endif /* EVERLIVE_JSON_H */

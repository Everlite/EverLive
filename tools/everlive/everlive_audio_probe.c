// SPDX-License-Identifier: GPL-2.0-only
/*
 * everlive_audio_probe.c - Native PipeWire graph probing for everlive_display
 *
 * Parses `pw-dump Node` with the vendored jsmn scanner.  No shell, no Python,
 * no locale-dependent text formats.  The parsing rules mirror the previous
 * Python probe byte for byte so that audio routing behaves identically.
 */

#include "everlive_audio_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* jsmn.o in tools/libeverlive-core/third_party defines JSMN_PARENT_LINKS
 * itself; the struct layout must match on this side as well. */
#define JSMN_PARENT_LINKS
#define JSMN_HEADER
#include "../libeverlive-core/third_party/jsmn.h"

/* A 4 MiB document cannot realistically exceed this; the cap makes a hostile
 * or corrupt dump fail fast instead of allocating gigabytes of tokens. */
#define AUDIO_PROBE_MAX_TOKENS (1u << 20)

#define AUDIO_PROP_SHORT 64
#define AUDIO_PROP_LONG 256

struct node_props {
    char media_class[AUDIO_PROP_SHORT];
    char node_name[AUDIO_NODE_NAME_MAX];
    char node_desc[AUDIO_PROP_LONG];
    char driver[AUDIO_PROP_SHORT];
    char alsa_id[AUDIO_PROP_SHORT];
    char card_name[AUDIO_PROP_LONG];
    char alsa_name[AUDIO_PROP_LONG];
    char app_name[AUDIO_PROP_SHORT];
    char app_id[AUDIO_PROP_SHORT];
    bool dev_present;
    bool dev_ok;
    uint32_t dev;
};

/* ------------------------------------------------------------------------- */
/* Validation                                                                */
/* ------------------------------------------------------------------------- */

static bool audio_parse_decimal(const char *text, uint32_t *out, bool allow_zero)
{
    uint64_t value = 0;

    if (!text || !*text || !out)
        return false;

    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10u + (uint64_t)(*p - '0');
        if (value > UINT32_MAX)
            return false;
    }

    if (value == 0 && !allow_zero)
        return false;

    *out = (uint32_t)value;
    return true;
}

bool audio_parse_node_id(const char *text, uint32_t *out)
{
    /* Id 0 is the PipeWire core object, never a node. */
    return audio_parse_decimal(text, out, false);
}

bool audio_validate_node_name(const char *name)
{
    size_t len;

    if (!name)
        return false;

    len = strlen(name);
    if (len == 0 || len >= AUDIO_NODE_NAME_MAX)
        return false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == ':' || c == '+' || c == '-';
        if (!ok)
            return false;
    }

    return true;
}

size_t audio_format_volume(float volume, char *buf, size_t bufsz)
{
    int written;

    if (!buf || bufsz == 0)
        return 0;

    written = snprintf(buf, bufsz, "%.2f", (double)volume);
    if (written < 0 || (size_t)written >= bufsz)
        return 0;

    return (size_t)written;
}

/* ------------------------------------------------------------------------- */
/* JSON helpers                                                             */
/* ------------------------------------------------------------------------- */

static jsmntok_t *audio_json_tokens(const char *json, size_t len, int *out_count)
{
    jsmn_parser parser;
    jsmntok_t *tokens;
    int count;
    int parsed;

    if (!json || len == 0 || len > AUDIO_PROBE_MAX_JSON)
        return NULL;

    jsmn_init(&parser);
    count = jsmn_parse(&parser, json, len, NULL, 0);
    if (count <= 0 || (unsigned)count > AUDIO_PROBE_MAX_TOKENS)
        return NULL;

    /* A little headroom so the second pass can never hit NOMEM. */
    tokens = calloc((size_t)count + 8u, sizeof(*tokens));
    if (!tokens)
        return NULL;

    jsmn_init(&parser);
    parsed = jsmn_parse(&parser, json, len, tokens, (unsigned)count + 8u);
    if (parsed <= 0) {
        free(tokens);
        return NULL;
    }

    *out_count = parsed;
    return tokens;
}

/* Returns the index of the value token for @key inside @obj, or -1. */
static int audio_token_key(const char *json, const jsmntok_t *tokens, int count,
                           int obj, const char *key)
{
    size_t klen;

    if (obj < 0 || obj >= count || tokens[obj].type != JSMN_OBJECT)
        return -1;

    klen = strlen(key);
    for (int i = obj + 1; i < count; i++) {
        const jsmntok_t *tok = &tokens[i];

        if (tok->type != JSMN_STRING || tok->parent != obj)
            continue;
        if ((size_t)(tok->end - tok->start) != klen)
            continue;
        if (memcmp(json + tok->start, key, klen) != 0)
            continue;

        if (i + 1 < count && tokens[i + 1].parent == i)
            return i + 1;
        return -1;
    }

    return -1;
}

/* Decodes a JSON string token into @out.  Returns false on overflow or on an
 * escape sequence we do not understand (including non-ASCII \u escapes). */
static bool audio_token_string(const char *json, const jsmntok_t *tok,
                               char *out, size_t outsz)
{
    size_t written = 0;

    if (!tok || tok->type != JSMN_STRING || !out || outsz == 0)
        return false;

    for (int i = tok->start; i < tok->end; i++) {
        unsigned char c = (unsigned char)json[i];

        if (c == '\\') {
            if (++i >= tok->end)
                return false;

            switch (json[i]) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned value = 0;

                if (i + 4 >= tok->end)
                    return false;

                for (int k = 0; k < 4; k++) {
                    int h = (unsigned char)json[++i];

                    value <<= 4;
                    if (h >= '0' && h <= '9')
                        value |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        value |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        value |= (unsigned)(h - 'A' + 10);
                    else
                        return false;
                }

                /* Keep names/properties ASCII; anything else is rejected. */
                if (value == 0 || value > 0x7f)
                    return false;
                c = (unsigned char)value;
                break;
            }
            default:
                return false;
            }
        }

        if (written + 1 >= outsz)
            return false;
        out[written++] = (char)c;
    }

    out[written] = '\0';
    return true;
}

static bool audio_token_uint(const char *json, const jsmntok_t *tok,
                             uint32_t *out, bool allow_zero)
{
    char buf[16];
    int len;

    if (!tok || (tok->type != JSMN_PRIMITIVE && tok->type != JSMN_STRING))
        return false;

    len = tok->end - tok->start;
    if (len <= 0 || (size_t)len >= sizeof(buf))
        return false;

    memcpy(buf, json + tok->start, (size_t)len);
    buf[len] = '\0';

    return audio_parse_decimal(buf, out, allow_zero);
}

static void audio_prop_string(const char *json, const jsmntok_t *tokens, int count,
                              int props, const char *key, char *out, size_t outsz)
{
    int tok = audio_token_key(json, tokens, count, props, key);

    out[0] = '\0';
    if (tok < 0)
        return;

    (void)audio_token_string(json, &tokens[tok], out, outsz);
}

static void audio_prop_device(const char *json, const jsmntok_t *tokens, int count,
                              int props, struct node_props *np)
{
    int tok = audio_token_key(json, tokens, count, props, "card.profile.device");

    if (tok < 0)
        tok = audio_token_key(json, tokens, count, props, "alsa.device");
    if (tok < 0)
        return;

    np->dev_present = true;

    /* PipeWire may emit null for an unset device; the old probe treated that
     * the same as device 0 (HDMI). */
    if (tokens[tok].type == JSMN_PRIMITIVE &&
        tokens[tok].end - tokens[tok].start == 4 &&
        memcmp(json + tokens[tok].start, "null", 4) == 0) {
        np->dev_ok = true;
        np->dev = 0;
        return;
    }

    np->dev_ok = audio_token_uint(json, &tokens[tok], &np->dev, true);
}

static bool audio_ci_contains(const char *haystack, const char *needle)
{
    size_t hlen;
    size_t nlen;

    if (!haystack || !needle)
        return false;

    nlen = strlen(needle);
    if (nlen == 0)
        return true;

    hlen = strlen(haystack);
    if (hlen < nlen)
        return false;

    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;

        while (j < nlen) {
            unsigned char a = (unsigned char)haystack[i + j];
            unsigned char b = (unsigned char)needle[j];

            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b - 'A' + 'a');
            if (a != b)
                break;
            j++;
        }

        if (j == nlen)
            return true;
    }

    return false;
}

static void audio_node_props(const char *json, const jsmntok_t *tokens, int count,
                             int obj, struct node_props *np)
{
    int info;
    int props;

    memset(np, 0, sizeof(*np));

    info = audio_token_key(json, tokens, count, obj, "info");
    if (info < 0 || tokens[info].type != JSMN_OBJECT)
        return;

    props = audio_token_key(json, tokens, count, info, "props");
    if (props < 0 || tokens[props].type != JSMN_OBJECT)
        return;

    audio_prop_string(json, tokens, count, props, "media.class",
                      np->media_class, sizeof(np->media_class));
    audio_prop_string(json, tokens, count, props, "node.name",
                      np->node_name, sizeof(np->node_name));
    audio_prop_string(json, tokens, count, props, "node.description",
                      np->node_desc, sizeof(np->node_desc));
    audio_prop_string(json, tokens, count, props, "alsa.driver_name",
                      np->driver, sizeof(np->driver));
    audio_prop_string(json, tokens, count, props, "alsa.id",
                      np->alsa_id, sizeof(np->alsa_id));
    audio_prop_string(json, tokens, count, props, "alsa.card_name",
                      np->card_name, sizeof(np->card_name));
    audio_prop_string(json, tokens, count, props, "alsa.name",
                      np->alsa_name, sizeof(np->alsa_name));
    audio_prop_string(json, tokens, count, props, "application.name",
                      np->app_name, sizeof(np->app_name));
    audio_prop_string(json, tokens, count, props, "application.id",
                      np->app_id, sizeof(np->app_id));
    audio_prop_device(json, tokens, count, props, np);
}

/* Mirrors everlive_viewer.find_pipewire_source_node() / the old embedded
 * Python probe: GC573 identity and the HDMI (device 0) input, never line-in. */
static bool audio_node_is_gc573_hdmi(const struct node_props *np)
{
    bool is_gc573;
    bool is_hdmi;

    if (strcmp(np->media_class, "Audio/Source") != 0)
        return false;

    is_gc573 = strcmp(np->driver, "open_gc573") == 0 ||
               strcmp(np->alsa_id, "A4K") == 0 ||
               strcmp(np->alsa_id, "GC573") == 0 ||
               audio_ci_contains(np->card_name, "live gamer 4k") ||
               audio_ci_contains(np->node_desc, "live gamer 4k") ||
               audio_ci_contains(np->node_name, "gc573") ||
               audio_ci_contains(np->node_desc, "gc573") ||
               audio_ci_contains(np->alsa_name, "gc573");

    is_hdmi = (!np->dev_present || (np->dev_ok && np->dev == 0)) &&
              !audio_ci_contains(np->node_desc, "line") &&
              !audio_ci_contains(np->alsa_name, "line") &&
              strstr(np->node_name, "input-1") == NULL;

    return is_gc573 && is_hdmi;
}

static bool audio_node_is_everlive_stream(const struct node_props *np)
{
    if (strcmp(np->media_class, "Stream/Output/Audio") != 0)
        return false;

    if (strstr(np->node_name, "input.") != NULL)
        return false;

    if (strcmp(np->app_name, "EverLive") == 0 ||
        strcmp(np->app_id, "everlive") == 0)
        return true;

    return audio_ci_contains(np->node_name, "everlive") ||
           audio_ci_contains(np->node_name, "gc573-audio");
}

/* ------------------------------------------------------------------------- */
/* Graph probing                                                            */
/* ------------------------------------------------------------------------- */

bool audio_probe_graph(const char *json, size_t len, struct audio_graph *out)
{
    jsmntok_t *tokens;
    int count = 0;

    if (!out)
        return false;

    memset(out, 0, sizeof(*out));
    tokens = audio_json_tokens(json, len, &count);
    if (!tokens)
        return false;

    if (tokens[0].type != JSMN_ARRAY) {
        free(tokens);
        return false;
    }

    for (int i = 1; i < count; i++) {
        const jsmntok_t *tok = &tokens[i];
        struct node_props np;
        uint32_t id;
        int id_tok;

        if (tok->type != JSMN_OBJECT || tok->parent != 0)
            continue;

        id_tok = audio_token_key(json, tokens, count, i, "id");
        if (id_tok < 0 || !audio_token_uint(json, &tokens[id_tok], &id, false))
            continue;

        audio_node_props(json, tokens, count, i, &np);

        if (!out->have_source && audio_node_is_gc573_hdmi(&np) &&
            audio_validate_node_name(np.node_name)) {
            out->source.id = id;
            snprintf(out->source.name, sizeof(out->source.name), "%s", np.node_name);
            out->have_source = true;
        }

        if (!out->have_stream && audio_node_is_everlive_stream(&np) &&
            audio_validate_node_name(np.node_name)) {
            out->stream.id = id;
            snprintf(out->stream.name, sizeof(out->stream.name), "%s", np.node_name);
            out->have_stream = true;
        }

        if (out->have_source && out->have_stream)
            break;
    }

    free(tokens);
    return true;
}

bool audio_graph_find_name_by_id(const char *json, size_t len, uint32_t id,
                                 char *name, size_t name_sz)
{
    jsmntok_t *tokens;
    int count = 0;
    bool found = false;

    if (!name || name_sz == 0)
        return false;
    name[0] = '\0';

    tokens = audio_json_tokens(json, len, &count);
    if (!tokens || tokens[0].type != JSMN_ARRAY) {
        free(tokens);
        return false;
    }

    for (int i = 1; i < count; i++) {
        const jsmntok_t *tok = &tokens[i];
        struct node_props np;
        uint32_t current;
        int id_tok;

        if (tok->type != JSMN_OBJECT || tok->parent != 0)
            continue;

        id_tok = audio_token_key(json, tokens, count, i, "id");
        if (id_tok < 0 || !audio_token_uint(json, &tokens[id_tok], &current, false))
            continue;
        if (current != id)
            continue;

        audio_node_props(json, tokens, count, i, &np);
        if (audio_validate_node_name(np.node_name)) {
            snprintf(name, name_sz, "%s", np.node_name);
            found = true;
        }
        break;
    }

    free(tokens);
    return found;
}

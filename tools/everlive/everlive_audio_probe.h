/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_audio_probe.h - Native PipeWire graph probing for everlive_display
 *
 * The display renderer must not route audio through a shell.  This module
 * contains the pure, dependency-free pieces of the audio routing logic:
 *
 *   - strict validation of node IDs and node names received from argv or
 *     PipeWire metadata (nothing that reaches execvp may come from here
 *     unchecked),
 *   - native parsing of `pw-dump Node` JSON into the capture source and the
 *     EverLive playback stream,
 *   - formatting of volume values for `wpctl set-volume`.
 *
 * All functions are re-entrant and allocation-free except audio_probe_graph()
 * and audio_graph_find_name_by_id(), which allocate a temporary jsmn token
 * array for the duration of the call.
 */

#ifndef EVERLIVE_AUDIO_PROBE_H
#define EVERLIVE_AUDIO_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Longest node.name we accept.  PipeWire names are DNS-like and far shorter. */
#define AUDIO_NODE_NAME_MAX 256

/* Hard cap for a `pw-dump Node` document.  A busy desktop stays well below
 * 1 MiB; anything above this is either corrupt or hostile. */
#define AUDIO_PROBE_MAX_JSON (4u * 1024u * 1024u)

struct audio_node {
    uint32_t id;
    char name[AUDIO_NODE_NAME_MAX];
};

struct audio_graph {
    bool have_source;
    struct audio_node source;
    bool have_stream;
    struct audio_node stream;
};

/* Parses a decimal object id strictly: digits only, in (0, UINT32_MAX]. */
bool audio_parse_node_id(const char *text, uint32_t *out);

/* Conservative allowlist for PipeWire node names that may be passed to an
 * external tool: [A-Za-z0-9._:+-], 1..AUDIO_NODE_NAME_MAX-1 bytes. */
bool audio_validate_node_name(const char *name);

/* Formats a volume for `wpctl set-volume` ("%.2f").  Returns the number of
 * bytes written (excluding NUL), or 0 when the buffer is too small. */
size_t audio_format_volume(float volume, char *buf, size_t bufsz);

/* Parses `pw-dump Node` JSON.  Finds the first GC573 HDMI Audio/Source and the
 * first EverLive output stream.  Returns true when the document parsed, false
 * on malformed/oversized input; inspect out->have_source/have_stream for the
 * result. */
bool audio_probe_graph(const char *json, size_t len, struct audio_graph *out);

/* Resolves a PipeWire object id to its node.name. */
bool audio_graph_find_name_by_id(const char *json, size_t len, uint32_t id,
                                 char *name, size_t name_sz);

#endif /* EVERLIVE_AUDIO_PROBE_H */

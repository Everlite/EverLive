/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc575_xu.h — AVerMedia GC575 private UVC extension-unit transport
 */

#ifndef GC575_XU_H
#define GC575_XU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC575_XU_UNIT		4
#define GC575_XU_SEL_LEN	2
#define GC575_XU_SEL_DATA	1
#define GC575_XU_MAX_PAYLOAD	512

/*
 * Build an AT frame.  service 1 uses marker 0xa1, service 0 uses 0xa0.
 * Returns the frame length, or 0 when @out is too small.
 */
size_t gc575_at_build(uint8_t service, uint32_t command,
		      const uint8_t *payload, size_t payload_len,
		      uint8_t *out, size_t out_size);

/*
 * Send one AT frame through the private XU and read the response.
 * Returns 0 on success, a negative errno-style value on failure.
 */
int gc575_xu_exchange(int fd, uint8_t unit,
		      const uint8_t *frame, size_t frame_len,
		      uint8_t *response, size_t response_size,
		      size_t *response_len);

/* AT b0: select USB descriptor profile 0 or 1. */
int gc575_xu_set_profile(int fd, uint8_t unit, uint32_t index);

/* AT b1: read the active USB descriptor profile. */
int gc575_xu_get_profile(int fd, uint8_t unit, uint32_t *index);

/* AT 23: read the NUL-terminated system information string. */
int gc575_xu_get_info(int fd, uint8_t unit, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* GC575_XU_H */

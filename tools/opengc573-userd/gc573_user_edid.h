/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_edid.h — Userspace EDID programming for IT6805 HDMI Receiver
 */

#ifndef GC573_USER_EDID_H
#define GC573_USER_EDID_H

#include "gc573-user-compat.h"

#define GC573_EDID_SIZE		256

/**
 * gc573_user_program_edid - Write standard GC573 EDID table to IT6805 DDC RAM
 * @bar0: Pointer to mapped BAR0 memory
 *
 * Programs the 256-byte EDID (1440p120, 4K60, 1080p240, HDR10, 8-channel LPCM)
 * into the IT6805 receiver and asserts HPD so the console immediately locks on.
 */
int gc573_user_program_edid(void *bar0);

/**
 * gc573_user_read_edid - Read the EDID RAM back from the IT6805
 * @bar0: Pointer to mapped BAR0 memory
 * @edid: Buffer receiving GC573_EDID_SIZE bytes
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_read_edid(void *bar0, u8 *edid);

/**
 * gc573_user_verify_edid - Compare the EDID RAM against the built-in table
 * @bar0: Pointer to mapped BAR0 memory
 * @mismatches: Optional number of differing bytes (checksum bytes excluded)
 * @first_offset: Optional offset of the first mismatch
 *
 * Returns 0 when the EDID RAM matches, -EIO on mismatch, other negative
 * errno on I2C failure.
 */
int gc573_user_verify_edid(void *bar0, unsigned int *mismatches,
			   unsigned int *first_offset);

/**
 * gc573_user_edid_compare - Compare a raw EDID buffer against the built-in table
 * @edid: Buffer with GC573_EDID_SIZE bytes
 * @size: Buffer size
 * @mismatches: Optional number of differing bytes (checksum bytes excluded)
 * @first_offset: Optional offset of the first mismatch
 *
 * Returns 0 when the buffer matches, -EIO on mismatch, -EINVAL on bad size.
 */
int gc573_user_edid_compare(const u8 *edid, size_t size,
			    unsigned int *mismatches,
			    unsigned int *first_offset);

/**
 * gc573_user_edid_data - Return the built-in 256-byte GC573 EDID table
 *
 * The returned pointer is valid for the lifetime of the process.
 */
const u8 *gc573_user_edid_data(void);

#endif /* GC573_USER_EDID_H */

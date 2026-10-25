/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_edid.h — Userspace EDID programming for IT6805 HDMI Receiver
 */

#ifndef GC573_USER_EDID_H
#define GC573_USER_EDID_H

#include "gc573-user-compat.h"

#define GC573_EDID_SIZE		256

/* Selectable 256-byte EDID profiles advertised to the HDMI source. */
#define GC573_EDID_PROFILE_DEFAULT	"default"
#define GC573_EDID_PROFILE_1440P120	"1440p120"
#define GC573_EDID_PROFILE_1080P240	"1080p240"
/*
 * Legacy alias: every built-in profile advertises AC-3 and DTS 5.1 next to
 * multi-channel L-PCM, so "bitstream" now resolves to the unified default.
 */
#define GC573_EDID_PROFILE_BITSTREAM	"bitstream"

/**
 * gc573_user_program_edid - Write the selected EDID table to IT6805 DDC RAM
 * @bar0: Pointer to mapped BAR0 memory
 *
 * Programs the active 256-byte EDID (see gc573_user_edid_select()) into the
 * IT6805 receiver and asserts HPD so the console immediately locks on.  Every
 * built-in profile advertises 4K60, 1440p120, 1080p240, HDR10, 8-channel
 * L-PCM, Dolby Digital (AC-3) and DTS 5.1, so the source may send either
 * decoded PCM or IEC 61937 bursts.  Repeated calls in the same process are
 * no-ops once the same table has been latched.
 */
int gc573_user_program_edid(void *bar0);

/**
 * gc573_user_edid_select - Resolve and select an EDID profile
 * @profile_name: "default", "1440p120", "1080p240", "bitstream" or a path
 *                to a .bin/.edid
 * @custom_path: Optional explicit path to a 256-byte EDID file (wins over
 *               @profile_name)
 *
 * Reads a custom file into the internal buffer when one is given.  No
 * hardware access is performed, so this can run before IT6805 init.  Returns
 * 0 on success, -EINVAL for unknown profiles or malformed files, or a
 * negative errno when the file cannot be read.
 */
int gc573_user_edid_select(const char *profile_name, const char *custom_path);

/**
 * gc573_user_edid_active_name - Name of the currently selected profile
 *
 * Returns the canonical profile name, or the custom file path.  The pointer
 * is valid until the next gc573_user_edid_select().
 */
const char *gc573_user_edid_active_name(void);

/**
 * gc573_user_program_edid_by_name - Select a profile and program it
 * @bar0: Pointer to mapped BAR0 memory
 * @profile_name: Profile name or path, see gc573_user_edid_select()
 * @custom_path: Optional explicit EDID file path
 *
 * Must be called before the source-facing HDMI handshake is released.
 */
int gc573_user_program_edid_by_name(void *bar0, const char *profile_name,
				    const char *custom_path);

/**
 * gc573_user_read_edid - Read the EDID RAM back from the IT6805
 * @bar0: Pointer to mapped BAR0 memory
 * @edid: Buffer receiving GC573_EDID_SIZE bytes
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_user_read_edid(void *bar0, u8 *edid);

/**
 * gc573_user_verify_edid - Compare the EDID RAM against the selected table
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
 * gc573_user_edid_compare - Compare a raw EDID buffer against the selected table
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
 * gc573_user_edid_data - Return the currently selected 256-byte EDID table
 *
 * The returned pointer is valid for the lifetime of the process.
 */
const u8 *gc573_user_edid_data(void);

/**
 * gc573_user_edid_advertises_bitstream - Active EDID advertises compressed audio?
 *
 * Scans the CTA-861 Audio Data Block for Short Audio Descriptors whose format
 * code is not L-PCM (AC-3, DTS, ...).  Built-in profiles always return true;
 * a custom EDID only when it really advertises a compressed format.
 */
bool gc573_user_edid_advertises_bitstream(void);

#endif /* GC573_USER_EDID_H */

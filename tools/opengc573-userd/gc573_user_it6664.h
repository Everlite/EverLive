/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_it6664.h — Userspace IT6664 HDMI splitter bring-up
 *
 * The GC573 routes the HDMI input through the IT6664 splitter:
 *   source -> IT6664 RX port 0 -> TX port 1 -> IT6805 port 0 (capture)
 *                              -> TX port 2 -> HDMI passthrough
 *
 * The source (console/PC) only sees EDID and HPD from the IT6664 RX path,
 * so the splitter must be unlocked and its upstream EDID published before
 * the IT6805 can ever lock to a signal.
 */

#ifndef GC573_USER_IT6664_H
#define GC573_USER_IT6664_H

#include "gc573-user-compat.h"

struct gc573_user_it6664_status {
	bool five_volt_present;	/* RX 0x13 bit 0: source 5V detected */
	bool scdt_locked;	/* RX 0x19 bit 7: receiver sync detected */
	bool tx1_hpd;		/* TX1 0x03 bit 0: IT6805 HPD seen */
	bool tx2_hpd;		/* TX2 0x03 bit 0: passthrough monitor HPD */
	u8 rx_status13;
	u8 rx_status14;
	u8 rx_scdt;
	u8 tx1_status;
	u8 tx2_status;
};

/**
 * gc573_user_it6664_init - Unlock and initialize the IT6664 splitter
 * @bar0: Pointer to mapped BAR0 memory
 *
 * Mirrors the kernel driver's gc555_it6664_init(): switch reset/unlock,
 * reference-clock calibration, RX CAOF calibration, RX register setup,
 * TX path reset and switch routing.  RX HPD is left low.
 */
int gc573_user_it6664_init(void *bar0);

/**
 * gc573_user_it6664_enable_source - Power TX1 and publish the upstream EDID
 * @bar0: Pointer to mapped BAR0 memory
 *
 * Must be called after gc573_user_it6805_init() so the IT6805 EDID RAM and
 * its HPD line are ready.  Powers on TX1 when the downstream HPD is present,
 * powers TX2 (HDMI OUT passthrough) when a monitor is attached, programs the
 * 256-byte capture EDID into the IT6664 upstream EDID RAM and raises the
 * source-facing HPD.
 */
int gc573_user_it6664_enable_source(void *bar0);

/**
 * gc573_user_it6664_poll - Refresh splitter state and follow RX lock
 * @bar0: Pointer to mapped BAR0 memory
 * @status: Optional state snapshot
 *
 * When the IT6664 RX locks it powers TX1 again so the kernel-equivalent AFE
 * calibration runs with the measured pixel clock, drives the splitter SCDT
 * GPIO like the kernel driver does, and keeps the optional TX2 passthrough
 * output armed for as long as RX sync is present, regardless of the HDMI OUT
 * HPD level (monitors lower HPD while another input is active).
 */
int gc573_user_it6664_poll(void *bar0,
			   struct gc573_user_it6664_status *status);

#endif /* GC573_USER_IT6664_H */

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_rx_eq.h — HDMI 2.0 receiver equalisation and TMDS
 * character-error (CED) monitoring for the GC573 userspace driver.
 *
 * The GC573 has two ITE HDMI receivers in series that share the same
 * receiver core and EQ register layout:
 *   - the IT6664 splitter's input (the card's HDMI IN connector), and
 *   - the IT6805 capture receiver fed by IT6664 TX1.
 * The IT6664 re-encodes the stream, so input errors are invisible to the
 * IT6805's counters; each receiver is measured and tuned separately.
 */

#ifndef GC573_USER_RX_EQ_H
#define GC573_USER_RX_EQ_H

#include "gc573-user-compat.h"
#include <stdbool.h>

#define GC573_RX_TMDS_CHANNELS	3

enum gc573_rx_port {
	GC573_RX_INPUT = 0,	/* IT6664 RX: the card's HDMI IN */
	GC573_RX_CAPTURE,	/* IT6805 RX: internal hop from IT6664 TX1 */
	GC573_RX_PORT_COUNT,
};

/* Per-channel character error counts over one measurement window. */
struct gc573_rx_ced {
	bool valid[GC573_RX_TMDS_CHANNELS];	/* counter reported valid */
	u16 count[GC573_RX_TMDS_CHANNELS];	/* errors, saturates at 0x7fff */
	u32 window_ms;
};

struct gc573_rx_eq_result {
	bool ran;		/* false: not HDMI 2.0, disabled, or clean */
	bool ced_valid;		/* CED counters reported valid counts */
	bool before_valid;	/* baseline window had valid counts */
	bool auto_eq_done;	/* hardware auto-EQ reported completion */
	bool dfe_applied;	/* DFE taps derived from auto-EQ were applied */
	bool swept;		/* manual per-channel RS sweep was needed */
	bool skew_trained;	/* skew retraining improved on skew 0 */
	bool kept_auto;		/* sweep lost to plain auto-EQ; kept that */
	u8 rs[GC573_RX_TMDS_CHANNELS];	/* final RS per channel (B/G/R) */
	u32 errors_before;	/* summed CED over the baseline window */
	u32 errors_after;	/* summed CED over the final window */
	u32 ch_errors_after[GC573_RX_TMDS_CHANNELS];
	u32 elapsed_ms;
};

const char *gc573_rx_port_name(enum gc573_rx_port port);

/**
 * gc573_rx_link_is_hdmi2 - True when the capture receiver sees a scrambled
 * HDMI 2.0 (>340 MHz TMDS) link.  The IT6664 retransmits at the input rate,
 * so this also describes the input link.
 */
bool gc573_rx_link_is_hdmi2(void *bar0);

/** Blocking CED measurement over @window_ms. */
int gc573_rx_ced_measure(void *bar0, enum gc573_rx_port port,
			 unsigned int window_ms, struct gc573_rx_ced *out);

/**
 * gc573_rx_ced_poll - Non-blocking CED measurement for the streaming loop.
 * The first call arms the counters and returns -EAGAIN; each later call
 * latches the window since the previous call into @out and re-arms.
 */
int gc573_rx_ced_poll(void *bar0, enum gc573_rx_port port, u64 now_ms,
		      struct gc573_rx_ced *out);

/** Forget any armed CED windows (call when the signal is lost). */
void gc573_rx_ced_reset(void);

/**
 * gc573_rx_eq_tune - Equalise one receiver on an HDMI 2.0 link
 *
 * Measures CED first and leaves an error-free link untouched.  Otherwise
 * runs the receiver's hardware auto-EQ, applies the result (plus DFE taps
 * where supported) and, if errors remain, sweeps the RS (equaliser
 * strength) per TMDS channel and keeps the setting with the fewest errors.
 * No-op on HDMI 1.x links or when EVERLIVE_NO_AUTO_EQ=1 is set.  Blocks for
 * up to roughly 1.5 s; the link may briefly lose SCDT while EQ runs.
 */
int gc573_rx_eq_tune(void *bar0, enum gc573_rx_port port,
		     struct gc573_rx_eq_result *result);

#endif /* GC573_USER_RX_EQ_H */

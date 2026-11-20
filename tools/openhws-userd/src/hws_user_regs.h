/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hws_user_regs.h — BAR0 register map of the HWS UHD capture family
 * (ACASIS VS2583, AVMatrix VC12-4K and siblings, PCI 8888:8581 et al.).
 *
 * Written from documented register behaviour; no third-party driver code.
 *
 * The card is an Artix-7 behind a Xilinx AXI-PCIe bridge.  All registers are
 * 32 bits wide.  The capture block sits at BAR0 + 0x4000; "capture register
 * n" means BAR0 + 0x4000 + 4 * n.
 */

#ifndef HWS_USER_REGS_H
#define HWS_USER_REGS_H

/* ---- PCIe bridge ------------------------------------------------------ */

/* Interrupt enable; 0x3FFFF enables every source the card has. */
#define HWS_BR_INT_ENABLE		0x0134
#define HWS_BR_INT_ENABLE_ALL		0x0003FFFFu

/* PHY status: bit 0 = 5 GT/s, bits 2:1 = width (1 x2, 2 x4, 3 x8). */
#define HWS_BR_LINK_STATUS		0x0144

/*
 * AXI-to-PCIe address translation, entry i at 0x208 + 8 * i: upper 32 bits of
 * the host bus address at +0, the 512 MiB-aligned lower part at +4.  Entry i
 * backs the card-side window starting at (i + 1) * 0x20000000.
 */
#define HWS_BR_XLAT_HI(i)		(0x0208 + 8 * (i))
#define HWS_BR_XLAT_LO(i)		(0x020C + 8 * (i))
#define HWS_XLAT_ENTRIES		4

#define HWS_WINDOW_SIZE			0x20000000ULL	/* 512 MiB */
#define HWS_WINDOW_MASK			(HWS_WINDOW_SIZE - 1)

/* ---- Capture block ---------------------------------------------------- */

#define HWS_CAP_BASE			0x4000
#define HWS_CAP(n)			(HWS_CAP_BASE + 4 * (n))

/*
 * Register 0: read = global status, write = run control.
 *   bit 0  capture core running
 *   bit 2  card raised the interrupt (check before reading register 1)
 *   0xFFFFFFFF = card gone (surprise removal / link down)
 */
#define HWS_REG_STATUS			HWS_CAP(0)
#define HWS_STATUS_RUNNING		(1u << 0)
#define HWS_STATUS_IRQ			(1u << 2)
#define HWS_STATUS_GONE			0xFFFFFFFFu

#define HWS_CTL_RESET			0x00000000u
#define HWS_CTL_STOP			0x00000010u
#define HWS_CTL_START_A			0x80000000u
#define HWS_CTL_START_B			0x80FFFFFFu
#define HWS_CTL_RUN			0x00000013u

/*
 * Register 1: interrupt status, write the handled bits back to acknowledge.
 *   bit 0   video half done (channel 0; toggle in register 32)
 *   bit 8   HDMI audio packet done (channel 0; toggle in register 40)
 *   bit 12  line-in audio packet done (8591 only)
 */
#define HWS_REG_INT_STATUS		HWS_CAP(1)
#define HWS_INT_VIDEO0			(1u << 0)
#define HWS_INT_AUDIO0			(1u << 8)
#define HWS_INT_LINEIN			(1u << 12)
#define HWS_INT_ACK_MASK		0x00FFFFFFu

#define HWS_REG_VIDEO_ENABLE		HWS_CAP(2)	/* bit ch */
#define HWS_REG_AUDIO_ENABLE		HWS_CAP(3)	/* bit ch */
#define HWS_REG_SIGNAL			HWS_CAP(5)	/* bit ch present, bit 8+ch interlaced */
#define HWS_REG_HDCP			HWS_CAP(8)	/* bit ch: source uses HDCP */

/* Register 9: longest DMA run in 16-byte units (caps a stale-length run). */
#define HWS_REG_DMA_MAX			HWS_CAP(9)

/* Registers 16..19: card address of video quarter q (window q). */
#define HWS_REG_QUARTER_ADDR(q)		HWS_CAP(16 + (q))
/* Register 24: HDMI audio ring address of channel 0 (window 1). */
#define HWS_REG_AUDIO_ADDR		HWS_CAP(24)

/* Register 32 bit 0: which half the engine is filling (1 = bottom). */
#define HWS_REG_VIDEO_TOGGLE		HWS_CAP(32)
/* Register 40 bit 0: which audio packet the engine is filling. */
#define HWS_REG_AUDIO_TOGGLE		HWS_CAP(40)

/* Registers 50..53: length of quarter q in 16-byte units. */
#define HWS_REG_QUARTER_LEN(q)		HWS_CAP(50 + (q))

/*
 * Register 88: device info.  bits 15:8 FPGA version, 23:16 sub-version,
 * 25:24 port id, 31:28 output format (0 = YUYV).
 */
#define HWS_REG_DEVINFO			HWS_CAP(88)

/* Register 90: input size, width in 15:0, height in 31:16 (per field). */
#define HWS_REG_INPUT_SIZE		HWS_CAP(90)
/* Register 91: saturation<<24 | hue<<16 | contrast<<8 | brightness. */
#define HWS_REG_PICTURE			HWS_CAP(91)
/* Register 110: input frame rate in whole fps (FPGA 122 and later). */
#define HWS_REG_INPUT_FPS		HWS_CAP(110)

/* ---- Geometry --------------------------------------------------------- */

#define HWS_DMA_BLOCK			2048u	/* the card moves 2 KiB bursts */
#define HWS_MAX_WIDTH			4096u
#define HWS_MAX_HEIGHT			2160u

/*
 * HDMI audio: two 4096-byte packets (1024 frames of S16LE stereo, 21.3 ms at
 * 48 kHz).  The toggle names the packet being filled, so the finished one is
 * the other: toggle 0 -> packet 1, toggle 1 -> packet 0.
 */
#define HWS_AUDIO_PACKET_BYTES		4096u
#define HWS_AUDIO_RATE			48000u
#define HWS_AUDIO_CHANNELS		2u

#endif /* HWS_USER_REGS_H */

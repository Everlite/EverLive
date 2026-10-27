/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_diag.c — Userspace VFIO diagnostic tool for AVerMedia GC573
 *
 * Initializes the card exactly like the userspace streamer/daemon, then
 * dumps the IT6805 HDMI receiver state and verifies the EDID RAM contents.
 */

#include "gc573_vfio.h"
#include "gc573_user_i2c.h"
#include "gc573_user_hw.h"
#include "gc573_user_edid.h"
#include "gc573_user_it6805.h"
#include "gc573_user_it6664.h"
#include <unistd.h>

#define IT6805_I2C_ADDR		0x48
#define IT6805_EDID_I2C_ADDR	0x54

#define IT6664_SWITCH_ADDR	0x2c
#define IT6664_RX_ADDR		0x38
#define IT6664_EDID_ADDR	0x6c
#define IT6664_TX_COMMON_ADDR	0x4b
#define IT6664_TX0_ADDR		0x34
#define IT6664_TX1_ADDR		0x35
#define IT6664_TX2_ADDR		0x36

static int it6805_read(void *bar0, u8 bank, u8 reg, u8 *val)
{
	if (gc573_user_it6805_set_bank(bar0, bank) < 0)
		return -1;
	return gc573_user_i2c_read(bar0, GC573_I2C_BUS_PRIMARY,
				   IT6805_I2C_ADDR, reg, 1, val, 1);
}

static int it6664_read(void *bar0, u8 addr, u8 reg, u8 *val)
{
	return gc573_user_i2c_read(bar0, GC573_I2C_BUS_PRIMARY, addr,
				   reg, 1, val, 1);
}

static void dump_it6664(void *bar0)
{
	static const struct {
		u8 addr;
		u8 reg;
		const char *name;
	} regs[] = {
		{ IT6664_SWITCH_ADDR, 0x00, "SW ID0" },
		{ IT6664_SWITCH_ADDR, 0x01, "SW ID1" },
		{ IT6664_SWITCH_ADDR, 0x02, "SW ID2" },
		{ IT6664_SWITCH_ADDR, 0x03, "SW ID3" },
		{ IT6664_RX_ADDR, 0x00, "RX ID0" },
		{ IT6664_RX_ADDR, 0x01, "RX ID1" },
		{ IT6664_RX_ADDR, 0x02, "RX ID2" },
		{ IT6664_RX_ADDR, 0x03, "RX ID3" },
		{ IT6664_RX_ADDR, 0x08, "RX CAOF" },
		{ IT6664_RX_ADDR, 0x13, "RX SYS13" },
		{ IT6664_RX_ADDR, 0x14, "RX SYS14" },
		{ IT6664_RX_ADDR, 0x19, "RX SCDT" },
		{ IT6664_RX_ADDR, 0x26, "RX HPD" },
		{ IT6664_EDID_ADDR, 0x00, "EDID[0]" },
		{ IT6664_EDID_ADDR, 0x7f, "EDID[127]" },
		{ IT6664_EDID_ADDR, 0x80, "EDID[128]" },
		{ IT6664_EDID_ADDR, 0xff, "EDID[255]" },
		{ IT6664_TX_COMMON_ADDR, 0x50, "TXCOM50" },
		{ IT6664_TX0_ADDR, 0x03, "TX0 STS" },
		{ IT6664_TX1_ADDR, 0x03, "TX1 STS" },
		{ IT6664_TX2_ADDR, 0x03, "TX2 STS" },
		{ IT6664_TX1_ADDR, 0xc0, "TX1 MODE" },
	};

	printf("\n--- IT6664 register dump ---\n");
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		u8 val = 0;
		int ret = it6664_read(bar0, regs[i].addr, regs[i].reg, &val);

		if (ret < 0)
			printf("  0x%02x 0x%02x %-8s : <I2C error>\n",
			       regs[i].addr, regs[i].reg, regs[i].name);
		else
			printf("  0x%02x 0x%02x %-8s : 0x%02x (%u)\n",
			       regs[i].addr, regs[i].reg, regs[i].name,
			       val, val);
	}
}

static void dump_it6805(void *bar0)
{
	static const struct {
		u8 bank;
		u8 reg;
		const char *name;
	} regs[] = {
		{ 0, 0x00, "ID0" }, { 0, 0x01, "ID1" },
		{ 0, 0x02, "ID2" }, { 0, 0x03, "ID3" },
		{ 0, 0x04, "REV" }, { 0, 0x13, "SYS13" },
		{ 0, 0x14, "SYS14" }, { 0, 0x15, "SYS15" },
		{ 0, 0x19, "SCDT" }, { 0, 0x1a, "IRQ1A" },
		{ 0, 0x1b, "IRQ1B" }, { 0, 0x40, "MUTE" },
		{ 0, 0x4b, "EDIDCTL" }, { 0, 0x98, "DEPTH" },
		{ 0, 0xaa, "AVMUTE" }, { 3, 0x3a, "PHY3" },
		{ 7, 0x3a, "PHY7" }, { 3, 0x5a, "CAOF3H" },
		{ 3, 0x59, "CAOF3L" }, { 7, 0x5a, "CAOF7H" },
		{ 7, 0x59, "CAOF7L" },
	};

	printf("\n--- IT6805 register dump ---\n");
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		u8 val = 0;
		int ret = it6805_read(bar0, regs[i].bank, regs[i].reg, &val);

		if (ret < 0)
			printf("  bank%u 0x%02x %-8s : <I2C error>\n",
			       regs[i].bank, regs[i].reg, regs[i].name);
		else
			printf("  bank%u 0x%02x %-8s : 0x%02x (%u)\n",
			       regs[i].bank, regs[i].reg, regs[i].name,
			       val, val);
	}
}

static int verify_edid(void *bar0)
{
	u8 data[GC573_EDID_SIZE];
	unsigned int mismatches = 0;
	int errors = 0;

	for (int i = 0; i < GC573_EDID_SIZE; i++) {
		if (gc573_user_i2c_read(bar0, GC573_I2C_BUS_PRIMARY,
					IT6805_EDID_I2C_ADDR, i, 1,
					&data[i], 1) < 0) {
			errors++;
			data[i] = 0xff;
		}
	}

	printf("\n--- EDID RAM readback (0x%02x) ---\n", IT6805_EDID_I2C_ADDR);
	printf("  Header: %02x %02x %02x %02x %02x %02x %02x %02x\n",
	       data[0], data[1], data[2], data[3],
	       data[4], data[5], data[6], data[7]);
	printf("  Bytes 126..129: %02x %02x %02x %02x\n",
	       data[126], data[127], data[128], data[129]);
	printf("  Bytes 252..255: %02x %02x %02x %02x\n",
	       data[252], data[253], data[254], data[255]);
	printf("  I2C read errors: %d\n", errors);

	/* Compare against the EDID table known to gc573_user_program_edid(). */
	unsigned int first = 0;
	int ret = gc573_user_edid_compare(data, sizeof(data), &mismatches,
					  &first);

	printf("  Compare result: %d (mismatches %u, first offset %u)\n",
	       ret, mismatches, first);
	return ret;
}

int main(int argc, char *argv[])
{
	const char *pci_addr = (argc > 1) ? argv[1] : NULL;
	struct gc573_vfio_ctx ctx;

	printf("==================================================================\n");
	printf("  OpenGC573 Userspace VFIO Diagnostic (IT6805 / EDID / HPD)       \n");
	printf("==================================================================\n\n");

	int ret = gc573_vfio_init(&ctx, pci_addr);
	if (ret < 0) {
		fprintf(stderr, "[ERROR] VFIO init failed: %d\n", ret);
		return 1;
	}

	printf("\n--- FPGA registers before init ---\n");
	printf("  Version       : 0x%08x\n", readl((u8 *)ctx.bar0 + 0x00000));
	printf("  GPIO          : 0x%08x\n", readl((u8 *)ctx.bar0 + 0x00040));
	printf("  Video status  : 0x%08x\n", readl((u8 *)ctx.bar0 + 0x01004));
	printf("  Resolution    : %u x %u\n",
	       readl((u8 *)ctx.bar0 + 0x01008) & 0xffff,
	       readl((u8 *)ctx.bar0 + 0x0100c) & 0xffff);
	printf("  DDR status    : 0x%08x\n", readl((u8 *)ctx.bar0 + 0x0107c));

	gc573_user_hw_init(ctx.bar0);
	printf("\n--- IT6664 state before init ---");
	dump_it6664(ctx.bar0);
	ret = gc573_user_it6664_init(ctx.bar0);
	printf("\n[diag] gc573_user_it6664_init() = %d\n", ret);
	ret = gc573_user_it6805_init(ctx.bar0);
	printf("\n[diag] gc573_user_it6805_init() = %d\n", ret);
	ret = gc573_user_it6664_enable_source(ctx.bar0);
	printf("\n[diag] gc573_user_it6664_enable_source() = %d\n", ret);
	verify_edid(ctx.bar0);

	printf("\n--- FPGA registers after init ---\n");
	printf("  GPIO          : 0x%08x (HPD bit2=%u)\n",
	       readl((u8 *)ctx.bar0 + 0x00040),
	       !!(readl((u8 *)ctx.bar0 + 0x00040) & BIT(2)));
	printf("  Video status  : 0x%08x\n", readl((u8 *)ctx.bar0 + 0x01004));
	printf("  Resolution    : %u x %u\n",
	       readl((u8 *)ctx.bar0 + 0x01008) & 0xffff,
	       readl((u8 *)ctx.bar0 + 0x0100c) & 0xffff);

	dump_it6805(ctx.bar0);
	dump_it6664(ctx.bar0);

	for (int i = 0; i < 10; i++) {
		struct gc573_user_video_signal sig = {0};
		struct gc573_user_it6664_status it6664 = {0};

		sleep(1);
		gc573_user_it6664_poll(ctx.bar0, &it6664);
		gc573_user_it6805_poll_signal(ctx.bar0, &sig);
		printf("[poll %d] 5V=%u SCDT=%u %ux%u@%u (%u kHz) | IT6664 5V=%u RXSCDT=%u TX1=%02x/%u TX2=%02x/%u | GPIO=0x%08x\n",
		       i + 1, sig.five_volt_present, sig.scdt_locked,
		       sig.width, sig.height, sig.frame_rate_hz,
		       sig.pixel_clock_khz, it6664.five_volt_present,
		       it6664.scdt_locked, it6664.tx1_status,
		       it6664.tx1_hpd, it6664.tx2_status,
		       it6664.tx2_hpd,
		       readl((u8 *)ctx.bar0 + 0x00040));
	}

	gc573_user_set_led_rgb(ctx.bar0, 0x00, 0xcc, 0xff);
	gc573_vfio_cleanup(&ctx);
	printf("\n[diag] done.\n");
	return 0;
}

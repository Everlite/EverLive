/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_probe.c — Userspace VFIO hardware probe tool for AVerMedia GC573
 */

#include "gc573_vfio.h"
#include "gc573_user_i2c.h"
#include "gc573_user_hw.h"

#define GC555_BRIDGE_REG_VERSION	0x00000
#define GC555_BRIDGE_REG_RESET		0x0000c
#define GC555_BRIDGE_REG_IRQ_STATUS	0x00010
#define GC555_BRIDGE_REG_GPIO		0x00040
#define GC555_BRIDGE_REG_VIDEO_STATUS	0x01004
#define GC555_BRIDGE_REG_VIDEO_WIDTH	0x01008
#define GC555_BRIDGE_REG_VIDEO_HEIGHT	0x0100c
#define GC555_BRIDGE_REG_DDR_STATUS	0x0107c

int main(int argc, char *argv[])
{
	const char *pci_addr = (argc > 1) ? argv[1] : NULL;
	struct gc573_vfio_ctx ctx;

	printf("====================================================\n");
	printf("  OpenGC573 Userspace Driver Hardware Probe (VFIO)  \n");
	printf("====================================================\n\n");

	int ret = gc573_vfio_init(&ctx, pci_addr);
	if (ret < 0) {
		fprintf(stderr, "\n[FATAL] Failed to initialize GC573 VFIO interface: %d\n", ret);
		fprintf(stderr, "Make sure the card is bound to vfio-pci:\n");
		fprintf(stderr, "  sudo ./tools/opengc573-userd/vfio_pci_bind.sh --bind\n\n");
		return 1;
	}

	printf("\n--- Reading FPGA Registers via Userspace MMIO ---\n");
	u32 version = readl((u8 *)ctx.bar0 + GC555_BRIDGE_REG_VERSION);
	u32 vid_status = readl((u8 *)ctx.bar0 + GC555_BRIDGE_REG_VIDEO_STATUS);
	u32 vid_w = readl((u8 *)ctx.bar0 + GC555_BRIDGE_REG_VIDEO_WIDTH);
	u32 vid_h = readl((u8 *)ctx.bar0 + GC555_BRIDGE_REG_VIDEO_HEIGHT);
	u32 ddr_status = readl((u8 *)ctx.bar0 + GC555_BRIDGE_REG_DDR_STATUS);
	u32 gpio = readl((u8 *)ctx.bar0 + GC555_BRIDGE_REG_GPIO);

	printf("  FPGA Firmware Version : 0x%08x\n", version);
	printf("  DDR Memory Status     : 0x%08x\n", ddr_status);
	printf("  GPIO Register         : 0x%08x\n", gpio);
	printf("  Video Engine Status   : 0x%08x\n", vid_status);
	printf("  Reported Resolution   : %u x %u\n", vid_w, vid_h);

	printf("\n--- Initializing FPGA Rails, Link Reset and LED ---\n");
	gc573_user_hw_init(ctx.bar0);

	/* Set LED to solid Cyan to visually confirm Userspace control */
	gc573_user_set_led_rgb(ctx.bar0, 0x00, 0xcc, 0xff);
	printf("  [✓] Set Onboard RGB LED to solid Cyan (Userspace Active)\n");

	printf("\n--- Probing I2C Devices ---\n");
	u8 it6805_id[4] = {0};
	ret = gc573_user_i2c_read(ctx.bar0, GC573_I2C_BUS_PRIMARY, GC573_I2C_ADDR_IT6805,
				  0x00, 1, it6805_id, sizeof(it6805_id));
	if (ret == 0) {
		printf("  [✓] IT6805 HDMI 2.0 Receiver responded at 0x%02x: %02x %02x %02x %02x\n",
		       GC573_I2C_ADDR_IT6805, it6805_id[0], it6805_id[1], it6805_id[2], it6805_id[3]);
	} else {
		printf("  [!] IT6805 probe returned %d (might need GPIO reset pulse)\n", ret);
	}

	/* Probe IT6664 (HDMI Transmitter / Passthrough) on Primary Bus (0x5b) */
	u8 it6664_id[4] = {0};
	ret = gc573_user_i2c_read(ctx.bar0, GC573_I2C_BUS_PRIMARY, GC573_I2C_ADDR_IT6664,
				  0x00, 1, it6664_id, sizeof(it6664_id));
	if (ret == 0) {
		printf("  [✓] IT6664 HDMI Transmitter responded at 0x%02x: %02x %02x %02x %02x\n",
		       GC573_I2C_ADDR_IT6664, it6664_id[0], it6664_id[1], it6664_id[2], it6664_id[3]);
	} else {
		printf("  [!] IT6664 probe returned %d\n", ret);
	}

	/* Probe RGB LED MCU on Secondary Bus (0x20) */
	u8 mcu_id[4] = {0};
	ret = gc573_user_i2c_read(ctx.bar0, GC573_I2C_BUS_SECONDARY, GC573_I2C_ADDR_MCU_LED,
				  0x00, 1, mcu_id, sizeof(mcu_id));
	if (ret == 0) {
		printf("  [✓] RGB LED Controller responded at 0x%02x: %02x %02x %02x %02x\n",
		       GC573_I2C_ADDR_MCU_LED, mcu_id[0], mcu_id[1], mcu_id[2], mcu_id[3]);
	} else {
		printf("  [!] RGB LED MCU probe returned %d\n", ret);
	}

	printf("\n--- Testing Userspace IOMMU DMA Buffer Allocation ---\n");
	size_t test_dma_size = 4 * 1024 * 1024; /* 4 MB (fits within default 8MB user memlock limit) */
	void *dma_buf = mmap(NULL, test_dma_size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (dma_buf != MAP_FAILED) {
		uint64_t test_iova = 0x20000000ULL;
		ret = gc573_vfio_map_dma(&ctx, dma_buf, test_dma_size, test_iova);
		if (ret == 0) {
			printf("  [✓] IOMMU DMA Mapping succeeded (4 MB at IOVA 0x%lx)\n",
			       (unsigned long)test_iova);
			gc573_vfio_unmap_dma(&ctx, test_iova, test_dma_size);
		} else {
			printf("  [!] IOMMU DMA Mapping failed: %d\n", ret);
		}
		munmap(dma_buf, test_dma_size);
	} else {
		printf("  [!] Failed to allocate userspace test memory\n");
	}

	printf("\n--- Testing MSI Interrupt EventFD Setup ---\n");
	ret = gc573_vfio_setup_msi(&ctx);
	if (ret == 0) {
		printf("  [✓] Hardware MSI interrupt line wired to userspace eventfd: %d\n", ctx.irq_efd);
	} else {
		printf("  [!] Interrupt setup failed: %d\n", ret);
	}

	printf("\n--- Cleaning up and Releasing Hardware ---\n");
	gc573_vfio_cleanup(&ctx);

	printf("\n[SUCCESS] Userspace driver probe sequence completed.\n\n");
	return 0;
}

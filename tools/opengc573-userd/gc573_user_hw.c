/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_hw.c — Userspace FPGA bridge, GPIO and HDMI link initialization
 */

#include "gc573_user_hw.h"
#include "gc573_user_i2c.h"

int gc573_user_set_gpio(void *bar0, unsigned int pin, bool high, unsigned int delay_ms)
{
	if (!bar0 || pin >= 32)
		return -EINVAL;

	u8 *base = (u8 *)bar0;
	u32 val = readl(base + GC573_REG_GPIO);
	if (high)
		val |= BIT(pin);
	else
		val &= ~BIT(pin);

	writel(val, base + GC573_REG_GPIO);

	if (delay_ms)
		msleep(delay_ms);

	return 0;
}

int gc573_user_get_gpio(void *bar0, unsigned int pin, bool *high)
{
	if (!bar0 || !high || pin >= 32)
		return -EINVAL;

	u8 *base = (u8 *)bar0;
	u32 val = readl(base + GC573_REG_GPIO);
	*high = (val & BIT(pin)) != 0;
	return 0;
}

int gc573_user_bridge_reset(void *bar0)
{
	static const u32 reset_bits[] = { BIT(0), BIT(8), BIT(9) };
	u8 *base = (u8 *)bar0;

	if (!bar0)
		return -EINVAL;

	for (size_t i = 0; i < sizeof(reset_bits) / sizeof(reset_bits[0]); i++) {
		u32 completion_bit = (reset_bits[i] == BIT(0)) ? BIT(3) : reset_bits[i];
		u32 status = 0;

		writel(reset_bits[i], base + 0x0000c);

		for (int retry = 0; retry <= 10; retry++) {
			usleep_range(1000, 2000);
			status = readl(base + 0x0000c);
			if (status & completion_bit)
				break;
		}

		if (!(status & completion_bit)) {
			pr_warn("Bridge reset bit 0x%x timed out, status 0x%x",
				reset_bits[i], status);
		}
		msleep(30);
	}
	return 0;
}

int gc573_user_bridge_wait_for_ddr(void *bar0)
{
	u8 *base = (u8 *)bar0;

	if (!bar0)
		return -EINVAL;

	u32 status = readl(base + GC573_REG_DDR_STATUS);
	if (status & BIT(0)) {
		pr_info("FPGA DDR sampler locked (status 0x%08x)", status);
		return 0;
	}

	for (int attempt = 0; attempt < 3; attempt++) {
		writel(0x8, base + GC573_REG_DDR_STATUS);
		writel(0x1, base + GC573_REG_DDR_STATUS);

		msleep(2000);
		status = readl(base + GC573_REG_DDR_STATUS);
		if (status & BIT(0)) {
			pr_info("FPGA DDR sampler locked on attempt %d (status 0x%08x)",
				attempt + 1, status);
			return 0;
		}
	}

	pr_warn("FPGA DDR sampler init timed out (status 0x%08x)", status);
	return -ETIMEDOUT;
}

int gc573_user_hw_init(void *bar0)
{
	if (!bar0)
		return -EINVAL;

	u8 *base = (u8 *)bar0;
	pr_info("Initializing FPGA bridge, GPIO rails and HDMI link...");

	/* 1. Bridge hardware reset: pulse core & DMA reset bits on 0x0000c */
	gc573_user_bridge_reset(bar0);

	/* 2. Wait for and prime the FPGA DDR sampler gate (0x0107c) */
	gc573_user_bridge_wait_for_ddr(bar0);

	/* 3. Assert Splitter Ready 0 (GPIO 4) and clear Splitter SCDT until sync (GPIO 3) */
	gc573_user_set_gpio(bar0, 4, true, 10);
	gc573_user_set_gpio(bar0, 3, false, 10);

	/* 4. Stop FPGA RGB LED hardware sequencer (MMIO 0x0804 = 0) */
	writel(0, base + 0x0804);

	/* 5. Configure FPGA I2C mode */
	writel(0x3d, base + 0x00120);
	writel(0x00, base + 0x00124);
	writel(0x80, base + 0x00128);
	msleep(100);

	/* Set I2C divisors (312 for 400kHz) */
	u32 divisor = 312;
	writel(divisor, base + 0x00180);
	writel(divisor, base + 0x001c0);
	writel(divisor, base + 0x00120);
	usleep_range(10000, 11000);

	/* 6. Initialize VIP reset bits (0x50000) and Audio DMA (0x00008) */
	writel(0x3, base + 0x50000);
	writel(0x0, base + 0x00008);

	/* 7. Enable FPGA bridge host IRQ routing so PCIe MSI triggers */
	writel(0x1fff | BIT(13), base + 0x00010);
	writel(0x1b33 | BIT(13), base + 0x0001c);

	/* 3. Link: Prepare Splitter */
	msleep(200);
	gc573_user_set_gpio(bar0, GC573_GPIO_SPLITTER_READY0, true, 2);
	gc573_user_set_gpio(bar0, GC573_GPIO_SPLITTER_READY1, true, 2);
	gc573_user_set_gpio(bar0, GC573_GPIO_SPLITTER_ENABLE, false, 2);
	gc573_user_set_gpio(bar0, GC573_GPIO_SPLITTER_RESET, true, 5);
	gc573_user_set_gpio(bar0, GC573_GPIO_SPLITTER_RESET, false, 10);
	gc573_user_set_gpio(bar0, GC573_GPIO_SPLITTER_RESET, true, 5);

	/* 4. Link: Reset Receiver (IT6805) via GPIO 8 pulse */
	gc573_user_set_gpio(bar0, GC573_GPIO_RECEIVER_RESET, true, 5);
	gc573_user_set_gpio(bar0, GC573_GPIO_RECEIVER_RESET, false, 10);
	gc573_user_set_gpio(bar0, GC573_GPIO_RECEIVER_RESET, true, 10);

	/* 5. Select Video Source 0 */
	u32 src_val = readl(base + GC573_REG_VIDEO_SOURCE);
	src_val = (src_val & ~BIT(6)) | BIT(0);
	writel(src_val, base + GC573_REG_VIDEO_SOURCE);

	/* 6. Assert HDMI HPD (Hot-Plug Detect) to console */
	gc573_user_set_gpio(bar0, GC573_GPIO_INPUT_HPD, true, 50);

	pr_info("Hardware reset sequence complete. HDMI HPD asserted.");
	return 0;
}

/* Vendor bridge write ends with lock or [rsp],0 so a later store cannot pass it. */
static void led_write(u8 *base, u32 offset, u32 val)
{
	writel(val, base + offset);
	asm volatile("lock orl $0, (%%rsp)" ::: "memory", "cc");
}

int gc573_user_bridge_restore_for_led(void *bar0)
{
	u8 *base;
	u32 reset;

	if (!bar0)
		return -EINVAL;

	base = (u8 *)bar0;
	reset = readl(base + 0x0000c);
	/* Bit 0 is the request. Completion is bit 3. */
	if (!(reset & BIT(3))) {
		for (int i = 0; i < 50 && !(reset & BIT(3)); i++) {
			usleep_range(1000, 2000);
			reset = readl(base + 0x0000c);
		}
	}
	gc573_user_bridge_wait_for_ddr(bar0);
	writel(0, base + 0x0804);
	writel(3, base + 0x50000);
	writel(0, base + 0x00008);
	pr_info("LED bridge reset 0x%08x ddr 0x%08x vip 0x%08x",
		readl(base + 0x0000c), readl(base + 0x0107c),
		readl(base + 0x50000));
	return 0;
}

static void gc573_led_program(void *bar0, u8 r, u8 g, u8 b, bool verbose)
{
	u8 *base = (u8 *)bar0;
	u32 hold;

	/* Profile 241, same constant-colour program as gc573_fpga_write_rgb(). */
	hold = 0xc0010000 | ((u32)r << 8) | (u32)r;

	led_write(base, 0x0804, 0);
	usleep_range(1000, 2000);
	led_write(base, 0x0800, 0x0000000f);
	for (int i = 0; i < 5; i++) {
		u32 btn = 0x0808 + (u32)i * 24;

		led_write(base, btn + 0, 3);
		led_write(base, btn + 4, 3);
		led_write(base, btn + 8, 4);
		led_write(base, btn + 12, 4);
		led_write(base, btn + 16, 5);
		led_write(base, btn + 20, 5);
	}
	led_write(base, 0x2008, 0x00000000);
	led_write(base, 0x200c, hold);
	led_write(base, 0x2010, 0xc0010000 | ((u32)g << 8) | (u32)g);
	led_write(base, 0x2014, 0xc0010000 | ((u32)b << 8) | (u32)b);
	if (verbose)
		pr_info("LED script before go 0x%08x 0x%08x 0x%08x",
			readl(base + 0x2008), readl(base + 0x200c),
			readl(base + 0x2010));
	led_write(base, 0x0804, 0x1f);
	if (verbose)
		pr_info("LED profile %s divider 0x%08x button 0x%08x enable 0x%08x",
			(r | g | b) ? "color" : "off",
			readl(base + 0x0800), readl(base + 0x0808),
			readl(base + 0x0804));
}

int gc573_user_set_led_rgb(void *bar0, u8 r, u8 g, u8 b)
{
	if (!bar0)
		return -EINVAL;
	gc573_led_program(bar0, r, g, b, true);
	return 0;
}

void gc573_user_led_refresh(void *bar0, u8 r, u8 g, u8 b)
{
	if (!bar0)
		return;
	gc573_led_program(bar0, r, g, b, false);
}

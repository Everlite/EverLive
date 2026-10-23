/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_i2c.c — Userspace I2C master driver over FPGA BAR0 MMIO
 */

#include "gc573_user_i2c.h"

#define GC555_I2C_REG_DIVISOR		0x00
#define GC555_I2C_REG_ADDRESS		0x04
#define GC555_I2C_REG_SUBADDR_LEN	0x08
#define GC555_I2C_REG_WIDTH		0x0c
#define GC555_I2C_REG_SUBADDR		0x10
#define GC555_I2C_REG_COUNT		0x14
#define GC555_I2C_REG_TX_FIFO		0x18
#define GC555_I2C_REG_RX_FIFO		0x1c
#define GC555_I2C_REG_COMMAND		0x20
#define GC555_I2C_REG_STATUS		0x24

#define GC555_I2C_COMMAND_WRITE		0x4
#define GC555_I2C_COMMAND_READ		0x8
#define GC555_I2C_COMMAND_FIFO_RESET	0x10
#define GC555_I2C_STATUS_DONE		(BIT(0) | BIT(2))
#define GC555_I2C_STATUS_ERROR		BIT(1)
#define GC555_I2C_STATUS_TERMINAL	(GC555_I2C_STATUS_DONE | GC555_I2C_STATUS_ERROR)

static inline u32 gc573_i2c_bus_base(unsigned int bus)
{
	return (bus == GC573_I2C_BUS_SECONDARY) ? 0x01c0 : 0x0180;
}

static inline int gc573_i2c_subaddr_code(unsigned int len)
{
	switch (len) {
	case 1: return 0;
	case 2: return 1;
	case 3: return 2;
	case 4: return 3;
	default: return -EINVAL;
	}
}

void gc573_user_i2c_init(void *bar0)
{
	/* Divisor = (125000 / 400) / 2 = ~156 for 400kHz */
	u32 divisor = 156;
	writel(divisor, (u8 *)bar0 + 0x0180 + GC555_I2C_REG_DIVISOR);
	writel(divisor, (u8 *)bar0 + 0x01c0 + GC555_I2C_REG_DIVISOR);
}

int gc573_user_i2c_read(void *bar0, unsigned int bus, u16 addr,
			u32 subaddr, unsigned int subaddr_len,
			u8 *data, unsigned int data_len)
{
	if (!bar0 || !data || !data_len)
		return -EINVAL;

	int code = gc573_i2c_subaddr_code(subaddr_len);
	if (code < 0)
		return code;

	u32 base = gc573_i2c_bus_base(bus);
	u8 *reg = (u8 *)bar0 + base;
	u32 slave = addr << 1;

	writel(slave | 1, reg + GC555_I2C_REG_ADDRESS);
	writel(code, reg + GC555_I2C_REG_SUBADDR_LEN);
	writel(subaddr, reg + GC555_I2C_REG_SUBADDR);
	writel(GC555_I2C_COMMAND_FIFO_RESET, reg + GC555_I2C_REG_COMMAND);
	writel(0, reg + GC555_I2C_REG_WIDTH);
	writel(data_len, reg + GC555_I2C_REG_COUNT);

	/* Trigger read command */
	writel(GC555_I2C_COMMAND_READ, reg + GC555_I2C_REG_COMMAND);

	/* Poll status */
	u8 status = 0;
	int timeout = 1000;
	while (timeout-- > 0) {
		status = readb(reg + GC555_I2C_REG_STATUS);
		if (status & GC555_I2C_STATUS_TERMINAL)
			break;
		usleep(200);
	}

	if (!(status & GC555_I2C_STATUS_DONE)) {
		pr_debug("I2C read timed out (status 0x%02x) on addr 0x%02x reg 0x%x", status, addr, subaddr);
		return (status & GC555_I2C_STATUS_ERROR) ? -EIO : -ETIMEDOUT;
	}

	/* Reset FIFO pointer for reading */
	writel(GC555_I2C_COMMAND_FIFO_RESET, reg + GC555_I2C_REG_COMMAND);

	for (unsigned int i = 0; i < data_len; i++) {
		data[i] = (u8)readl(reg + GC555_I2C_REG_RX_FIFO);
	}

	return 0;
}

int gc573_user_i2c_write(void *bar0, unsigned int bus, u16 addr,
			 u32 subaddr, unsigned int subaddr_len,
			 const u8 *data, unsigned int data_len)
{
	if (!bar0 || !data || !data_len)
		return -EINVAL;

	int code = gc573_i2c_subaddr_code(subaddr_len);
	if (code < 0)
		return code;

	u32 base = gc573_i2c_bus_base(bus);
	u8 *reg = (u8 *)bar0 + base;
	u32 slave = addr << 1;

	writel(slave, reg + GC555_I2C_REG_ADDRESS);
	writel(code, reg + GC555_I2C_REG_SUBADDR_LEN);
	writel(subaddr, reg + GC555_I2C_REG_SUBADDR);
	writel(GC555_I2C_COMMAND_FIFO_RESET, reg + GC555_I2C_REG_COMMAND);
	writel(0, reg + GC555_I2C_REG_WIDTH);
	writel(data_len, reg + GC555_I2C_REG_COUNT);

	for (unsigned int i = 0; i < data_len; i++) {
		writel(data[i], reg + GC555_I2C_REG_TX_FIFO);
	}

	/* Trigger write command */
	writel(GC555_I2C_COMMAND_WRITE, reg + GC555_I2C_REG_COMMAND);

	/* Poll status */
	u8 status = 0;
	int timeout = 1000;
	while (timeout-- > 0) {
		status = readb(reg + GC555_I2C_REG_STATUS);
		if (status & GC555_I2C_STATUS_TERMINAL)
			break;
		usleep(200);
	}

	if (!(status & GC555_I2C_STATUS_DONE)) {
		pr_debug("I2C write timed out (status 0x%02x) on addr 0x%02x reg 0x%x", status, addr, subaddr);
		return (status & GC555_I2C_STATUS_ERROR) ? -EIO : -ETIMEDOUT;
	}

	return 0;
}

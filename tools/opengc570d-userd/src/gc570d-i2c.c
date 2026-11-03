// SPDX-License-Identifier: GPL-2.0-only
/*
 * Internal I2C-controller transactions used to communicate with the HDMI
 * receivers, video splitter, and LED controller.
 */
#include "gc570d.h"

const struct gc570d_i2c_bus gc570d_receiver_buses[] = {
	{ GC570D_REG_I2C0_BASE, GC570D_IRQ_I2C0, 0x05, "IT68051" },
	{ GC570D_REG_I2C1_BASE, GC570D_IRQ_I2C1, 0x02, "IT6802" },
};

const struct gc570d_i2c_bus gc570d_splitter_bus = {
	GC570D_REG_I2C0_BASE, GC570D_IRQ_I2C0, 0x00, "VideoSplitter",
};

/* The official driver routes the GC570D enclosure LED controller through bridge I2C2. */
const struct gc570d_i2c_bus gc570d_led_bus = {
	GC570D_REG_I2C2_BASE, GC570D_IRQ_I2C2, 0x00, "GC570D LED",
};

const struct gc570d_i2c_bus gc570d_probe_buses[3] = {
	{ GC570D_REG_I2C0_BASE, GC570D_IRQ_I2C0, 0x00, "I2C0" },
	{ GC570D_REG_I2C1_BASE, GC570D_IRQ_I2C1, 0x00, "I2C1" },
	{ GC570D_REG_I2C2_BASE, GC570D_IRQ_I2C2, 0x00, "I2C2" },
};

int gc570d_i2c_read8(struct gc570d_dev *gc,
			    const struct gc570d_i2c_bus *bus, u8 slave_addr8,
			    u8 address, u8 *value, u32 *last_irq,
			    u32 *last_status)
{
	u32 irq_status;
	u32 transaction_status;
	int ret;

	mutex_lock(&gc->i2c_lock);
	writel(GC570D_I2C_RX_CLOCK_VALUE,
	       gc->bar0 + bus->base + GC570D_I2C_CLOCK);
	gc570d_control_irq_prepare(gc, bus->irq);
	writel(slave_addr8 | 1,
	       gc->bar0 + bus->base + GC570D_I2C_SLAVE);
	writel(0, gc->bar0 + bus->base + GC570D_I2C_ADDR_LEN);
	writel(0, gc->bar0 + bus->base + GC570D_I2C_WIDTH);
	writel(address, gc->bar0 + bus->base + GC570D_I2C_ADDRESS);
	writel(1, gc->bar0 + bus->base + GC570D_I2C_COUNT);
	writel(GC570D_I2C_CLEAR_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);
	writel(GC570D_I2C_READ_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);

	ret = gc570d_control_irq_wait(gc, bus->irq, &irq_status);
	
	if (last_irq)
		*last_irq = irq_status;
	
	transaction_status = readl(gc->bar0 + bus->base + GC570D_I2C_STATUS);
	if (last_status)
		*last_status = transaction_status;

	if (ret) {
		writel(GC570D_I2C_CLEAR_COMMAND,
		       gc->bar0 + bus->base + GC570D_I2C_COMMAND);
		gc570d_control_irq_finish(gc, bus->irq);
		goto out_unlock;
	}

	gc570d_control_irq_finish(gc, bus->irq);
	if (!(transaction_status & GC570D_I2C_READ_OK)) {
		writel(GC570D_I2C_CLEAR_COMMAND,
		       gc->bar0 + bus->base + GC570D_I2C_COMMAND);
		ret = -EIO;
		goto out_unlock;
	}

	writel(GC570D_I2C_CLEAR_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);
	
	if (value)
		*value = readl(gc->bar0 + bus->base + GC570D_I2C_READ_FIFO);

out_unlock:
	mutex_unlock(&gc->i2c_lock);
	return ret;
}

int gc570d_i2c_read_buf(struct gc570d_dev *gc,
			       const struct gc570d_i2c_bus *bus, u8 slave_addr8,
			       u8 address, u8 *values, u8 count,
			       u32 *last_irq, u32 *last_status)
{
	u32 irq_status;
	u32 transaction_status;
	u8 i;
	int ret;

	if (!count || count > 32 || !values)
		return -EINVAL;

	mutex_lock(&gc->i2c_lock);
	writel(GC570D_I2C_RX_CLOCK_VALUE,
	       gc->bar0 + bus->base + GC570D_I2C_CLOCK);
	gc570d_control_irq_prepare(gc, bus->irq);
	writel(slave_addr8 | 1,
	       gc->bar0 + bus->base + GC570D_I2C_SLAVE);
	writel(0, gc->bar0 + bus->base + GC570D_I2C_ADDR_LEN);
	writel(0, gc->bar0 + bus->base + GC570D_I2C_WIDTH);
	writel(address, gc->bar0 + bus->base + GC570D_I2C_ADDRESS);
	writel(count, gc->bar0 + bus->base + GC570D_I2C_COUNT);
	writel(GC570D_I2C_CLEAR_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);
	writel(GC570D_I2C_READ_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);

	ret = gc570d_control_irq_wait(gc, bus->irq, &irq_status);
	
	if (last_irq)
		*last_irq = irq_status;
	
	transaction_status = readl(gc->bar0 + bus->base + GC570D_I2C_STATUS);
	if (last_status)
		*last_status = transaction_status;

	if (ret)
		goto out_clear;

	if (!(transaction_status & GC570D_I2C_READ_OK)) {
		ret = -EIO;
		goto out_clear;
	}

	writel(GC570D_I2C_CLEAR_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);
	for (i = 0; i < count; i++)
		values[i] = readl(gc->bar0 + bus->base + GC570D_I2C_READ_FIFO);
	goto out_finish;

out_clear:
	writel(GC570D_I2C_CLEAR_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);
	
out_finish:
	gc570d_control_irq_finish(gc, bus->irq);
	mutex_unlock(&gc->i2c_lock);
	return ret;
}

int gc570d_i2c_write8(struct gc570d_dev *gc,
			     const struct gc570d_i2c_bus *bus, u8 slave_addr8,
			     u8 address, u8 value)
{
	u32 irq_status;
	u32 status;
	int ret;

	mutex_lock(&gc->i2c_lock);
	writel(GC570D_I2C_RX_CLOCK_VALUE,
	       gc->bar0 + bus->base + GC570D_I2C_CLOCK);
	gc570d_control_irq_prepare(gc, bus->irq);
	writel(slave_addr8,
	       gc->bar0 + bus->base + GC570D_I2C_SLAVE);
	writel(0, gc->bar0 + bus->base + GC570D_I2C_ADDR_LEN);
	writel(address, gc->bar0 + bus->base + GC570D_I2C_ADDRESS);
	writel(GC570D_I2C_CLEAR_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);
	writel(0, gc->bar0 + bus->base + GC570D_I2C_WIDTH);
	writel(1, gc->bar0 + bus->base + GC570D_I2C_COUNT);
	writel(value, gc->bar0 + bus->base + GC570D_I2C_WRITE_FIFO);
	writel(GC570D_I2C_WRITE_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);

	ret = gc570d_control_irq_wait(gc, bus->irq, &irq_status);
	status = readl(gc->bar0 + bus->base + GC570D_I2C_STATUS);
	gc570d_control_irq_finish(gc, bus->irq);
	if (!ret && !(status & GC570D_I2C_WRITE_OK))
		ret = -EIO;
	writel(GC570D_I2C_CLEAR_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);

	mutex_unlock(&gc->i2c_lock);
	return ret;
}

int gc570d_i2c_write_buf(struct gc570d_dev *gc,
				const struct gc570d_i2c_bus *bus,
				u8 slave_addr8, u8 address,
				const u8 *values, u8 count)
{
	u32 irq_status;
	u32 status;
	unsigned int i;
	int ret;

	if (!count || count > 32 || !values)
		return -EINVAL;

	mutex_lock(&gc->i2c_lock);
	writel(GC570D_I2C_RX_CLOCK_VALUE,
	       gc->bar0 + bus->base + GC570D_I2C_CLOCK);
	gc570d_control_irq_prepare(gc, bus->irq);
	writel(slave_addr8,
	       gc->bar0 + bus->base + GC570D_I2C_SLAVE);
	writel(0, gc->bar0 + bus->base + GC570D_I2C_ADDR_LEN);
	writel(address, gc->bar0 + bus->base + GC570D_I2C_ADDRESS);
	writel(GC570D_I2C_CLEAR_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);
	writel(0, gc->bar0 + bus->base + GC570D_I2C_WIDTH);
	writel(count, gc->bar0 + bus->base + GC570D_I2C_COUNT);
	for (i = 0; i < count; i++)
		writel(values[i],
		       gc->bar0 + bus->base + GC570D_I2C_WRITE_FIFO);
	writel(GC570D_I2C_WRITE_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);

	ret = gc570d_control_irq_wait(gc, bus->irq, &irq_status);
	status = readl(gc->bar0 + bus->base + GC570D_I2C_STATUS);
	gc570d_control_irq_finish(gc, bus->irq);
	if (!ret && !(status & GC570D_I2C_WRITE_OK))
		ret = -EIO;
	writel(GC570D_I2C_CLEAR_COMMAND,
	       gc->bar0 + bus->base + GC570D_I2C_COMMAND);

	mutex_unlock(&gc->i2c_lock);
	return ret;
}
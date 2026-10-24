// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#include "gc573.h"

#define GC555_BRIDGE_REG_VERSION	0x00000
#define GC555_BRIDGE_REG_AUDIO_DMA	0x00008
#define GC555_BRIDGE_REG_RESET		0x0000c
#define GC555_BRIDGE_REG_IRQ_STATUS	0x00010
#define GC555_BRIDGE_REG_IRQ_ENABLE	0x0001c
#define GC555_BRIDGE_REG_GPIO		0x00040
#define GC555_BRIDGE_REG_I2C_MODE	0x00120
#define GC555_BRIDGE_REG_I2C_CTRL0	0x00124
#define GC555_BRIDGE_REG_I2C_CTRL1	0x00128
#define GC555_BRIDGE_REG_I2C0_DIVISOR	0x00180
#define GC555_BRIDGE_REG_I2C1_DIVISOR	0x001c0
#define GC555_BRIDGE_REG_VIDEO_STATUS	0x01004
#define GC555_BRIDGE_REG_VIDEO_WIDTH	0x01008
#define GC555_BRIDGE_REG_VIDEO_HEIGHT	0x0100c
#define GC555_BRIDGE_REG_DDR_STATUS	0x0107c
#define GC555_BRIDGE_REG_AUDIO_DIVISOR	0x010a0
#define GC555_BRIDGE_REG_VIP_RESET	0x50000
#define GC555_BRIDGE_REG_LAST		0x60afc

#define GC555_BRIDGE_MIN_REG_SIZE	(GC555_BRIDGE_REG_LAST + sizeof(u32))
#define GC555_BRIDGE_RESET_MASK		(BIT(0) | BIT(8) | BIT(9))
#define GC555_BRIDGE_IRQ_STATUS_MASK	((u32)(0x1fff | BIT(13)))
#define GC555_BRIDGE_GC555_IRQ_ENABLE	((u32)(0x1b33 | BIT(13)))
#define GC555_BRIDGE_GC555_IRQ_MIN_VERSION	0x19081601
#define GC555_BRIDGE_I2C_BUS_KHZ	400U
#define GC555_BRIDGE_I2C_CLOCK_KHZ	125000U
#define GC555_BRIDGE_AUDIO_CLOCK_HZ	100000000U
#define GC555_BRIDGE_AUDIO_RATE_STEP_HZ	100U
#define GC555_BRIDGE_AUDIO_RATE_ATTEMPTS	5

static bool gc555_bridge_range_valid(struct gc555_dev *gc555, u32 offset,
				     size_t width)
{
	resource_size_t size;

	if (!gc555 || !gc555->bridge.regs)
		return false;

	size = gc555->bridge.regs_size;
	return size >= width && offset <= size - width;
}

int gc555_bridge_read(struct gc555_dev *gc555, u32 offset, u32 *value)
{
	if (!value)
		return -EINVAL;
	if (offset & (sizeof(*value) - 1))
		return -EINVAL;
	if (!gc555_bridge_range_valid(gc555, offset, sizeof(*value)))
		return -ERANGE;

	*value = readl(gc555->bridge.regs + offset);
	return 0;
}

int gc555_bridge_read8(struct gc555_dev *gc555, u32 offset, u8 *value)
{
	if (!value)
		return -EINVAL;
	if (!gc555_bridge_range_valid(gc555, offset, sizeof(*value)))
		return -ERANGE;

	*value = readb(gc555->bridge.regs + offset);
	return 0;
}

int gc555_bridge_write(struct gc555_dev *gc555, u32 offset, u32 value)
{
	if (offset & (sizeof(value) - 1))
		return -EINVAL;
	if (!gc555_bridge_range_valid(gc555, offset, sizeof(value)))
		return -ERANGE;

	writel(value, gc555->bridge.regs + offset);
	return 0;
}

int gc555_bridge_get_frame_info(struct gc555_dev *gc555,
				struct gc555_bridge_frame_info *frame_info)
{
	struct gc555_bridge_frame_info sample = {};
	u32 height;
	u32 width;
	u32 field_height;
	int ret;

	if (!frame_info)
		return -EINVAL;
	if (!gc555_bridge_is_ready(gc555))
		return -ENODEV;

	ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_VIDEO_STATUS,
				&sample.status);
	if (!ret)
		ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_VIDEO_WIDTH,
					&width);
	if (!ret)
		ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_VIDEO_HEIGHT,
					&height);
	if (ret)
		return ret;

	sample.width = width & 0xffff;
	sample.height = height & 0xffff;
	field_height = height >> 16;
	sample.interlaced = field_height != 0;
	if (sample.interlaced) {
		if (sample.height && sample.height < field_height)
			field_height = sample.height;
		sample.height = field_height * 2U;
	}

	*frame_info = sample;
	return 0;
}

int gc555_bridge_get_audio_rate(struct gc555_dev *gc555, u32 *rate_hz)
{
	unsigned int attempt;

	if (!rate_hz)
		return -EINVAL;
	if (!gc555_bridge_is_ready(gc555))
		return -ENODEV;

	for (attempt = 0; attempt < GC555_BRIDGE_AUDIO_RATE_ATTEMPTS;
	     attempt++) {
		u32 divisor;
		u32 measured_rate;
		int ret;

		ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_AUDIO_DIVISOR,
					&divisor);
		if (ret)
			return ret;
		if (!divisor)
			continue;

		measured_rate = GC555_BRIDGE_AUDIO_CLOCK_HZ / divisor;
		measured_rate = DIV_ROUND_CLOSEST(
			measured_rate, GC555_BRIDGE_AUDIO_RATE_STEP_HZ) *
			GC555_BRIDGE_AUDIO_RATE_STEP_HZ;
		switch (measured_rate) {
		case 32000:
		case 44100:
		case 48000:
		case 96000:
		case 192000:
			*rate_hz = measured_rate;
			return 0;
		default:
			break;
		}
	}

	return -EAGAIN;
}

bool gc555_bridge_is_ready(struct gc555_dev *gc555)
{
	return gc555 && gc555->bridge.ready;
}

bool gc555_bridge_is_accessible(struct gc555_dev *gc555)
{
	u32 version;

	return !gc555_bridge_read(gc555, GC555_BRIDGE_REG_VERSION, &version) &&
	       version != U32_MAX;
}

int gc555_bridge_set_gpio(struct gc555_dev *gc555, unsigned int pin,
			  bool high, unsigned int delay_ms)
{
	u32 value;
	int ret;

	if (!gc555 || pin >= 32)
		return -EINVAL;

	mutex_lock(&gc555->bridge.gpio_lock);
	ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_GPIO, &value);
	if (!ret) {
		if (high)
			value |= BIT(pin);
		else
			value &= ~BIT(pin);
		ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_GPIO, value);
	}
	mutex_unlock(&gc555->bridge.gpio_lock);

	if (!ret && delay_ms)
		msleep(delay_ms);

	return ret;
}

int gc555_bridge_get_gpio(struct gc555_dev *gc555, unsigned int pin,
			  bool *high)
{
	u32 value;
	int ret;

	if (!gc555 || !high || pin >= 32)
		return -EINVAL;

	ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_GPIO, &value);
	if (ret)
		return ret;

	*high = value & BIT(pin);
	return 0;
}

static int gc555_bridge_reset(struct gc555_dev *gc555)
{
	static const u32 reset_bits[] = { BIT(0), BIT(8), BIT(9) };
	int result = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(reset_bits); i++) {
		u32 completion_bit;
		u32 status = 0;
		unsigned int retry;
		int ret;

		if (!(GC555_BRIDGE_RESET_MASK & reset_bits[i]))
			continue;

		/* Core reset completion uses bit 3, not the request bit. */
		completion_bit = reset_bits[i] == BIT(0) ? BIT(3) :
							      reset_bits[i];
		ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_RESET,
					 reset_bits[i]);
		if (ret)
			return ret;

		for (retry = 0; retry <= 10; retry++) {
			usleep_range(1000, 2000);
			ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_RESET,
						&status);
			if (ret)
				return ret;
			if (status & completion_bit)
				break;
		}

		/* DDR readiness below is the authoritative post-reset gate. */
		if (!(status & completion_bit)) {
			dev_warn(gc555->dev,
				 "reset bit %#x timed out, status %#x\n",
				 reset_bits[i], status);
			result = -ETIMEDOUT;
		}
		msleep(30);
	}

	return result;
}

static int gc555_bridge_wait_for_ddr(struct gc555_dev *gc555)
{
	u32 status;
	unsigned int attempt;
	int ret;

	ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_DDR_STATUS, &status);
	if (ret || (status & BIT(0)))
		return ret;

	for (attempt = 0; attempt < 3; attempt++) {
		ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_DDR_STATUS,
					 0x8);
		if (ret)
			return ret;
		ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_DDR_STATUS,
					 0x1);
		if (ret)
			return ret;

		msleep(2000);
		ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_DDR_STATUS,
					&status);
		if (ret || (status & BIT(0)))
			return ret;
	}

	dev_err(gc555->dev, "DDR initialization timed out, status %#x\n",
		status);
	return -ETIMEDOUT;
}

static int gc555_bridge_init_controller(struct gc555_dev *gc555)
{
	u32 divisor = GC555_BRIDGE_I2C_CLOCK_KHZ /
		      GC555_BRIDGE_I2C_BUS_KHZ;
	u32 version;
	int ret;

	ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_VERSION, &version);
	if (ret)
		return ret;

	ret = gc555_bridge_set_gpio(gc555, 3, true, 100);
	if (ret)
		return ret;

	/* GC573: Stop FPGA RGB LED hardware sequencer (MMIO 0x804 = 0) */
	ret = gc555_bridge_write(gc555, 0x0804, 0);
	if (ret)
		return ret;

	ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_I2C_MODE, 0x3d);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_I2C_CTRL0, 0);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_I2C_CTRL1, 0x80);
	if (ret)
		return ret;

	msleep(100);

	ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_I2C0_DIVISOR,
				 divisor);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_I2C1_DIVISOR,
				 divisor);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, 0x00120, divisor);
	if (ret)
		return ret;

	usleep_range(10000, 11000);

	ret = gc555_bridge_set_host_irq_routing(gc555, false);
	if (ret)
		return ret;

	dev_dbg(gc555->dev, "bridge version %#x, I2C divisor %u\n",
		version, divisor);
	return 0;
}

static int gc555_bridge_start(struct gc555_dev *gc555)
{
	int reset_ret;
	int ret;

	gc555->bridge.ready = false;
	ret = gc555_bridge_set_host_irq_routing(gc555, false);
	if (ret)
		return ret;

	reset_ret = gc555_bridge_reset(gc555);
	if (reset_ret && reset_ret != -ETIMEDOUT)
		return reset_ret;

	ret = gc555_bridge_wait_for_ddr(gc555);
	if (ret)
		return ret;
	ret = gc555_bridge_init_controller(gc555);
	if (ret)
		return ret;

	/* Initialize both VIP reset bits before DMA setup. */
	ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_VIP_RESET, 0x3);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_AUDIO_DMA, 0);
	if (ret)
		return ret;

	if (reset_ret)
		dev_warn(gc555->dev,
			 "bridge continued after reset completion timeout\n");

	gc555->bridge.ready = true;
	return 0;
}

int gc555_bridge_init(struct gc555_dev *gc555, void __iomem *regs,
		      resource_size_t regs_size)
{
	int ret;

	if (!gc555 || !regs)
		return -EINVAL;
	if (regs_size < GC555_BRIDGE_MIN_REG_SIZE)
		return dev_err_probe(gc555->dev, -ENODEV,
				     "BAR0 is too small: %pa bytes\n",
				     &regs_size);

	gc555->bridge.regs = regs;
	gc555->bridge.regs_size = regs_size;
	gc555->bridge.ready = false;
	mutex_init(&gc555->bridge.gpio_lock);

	/*
	 * Keep FPGA host interrupts masked until the PCI IRQ vector and handler
	 * are installed. The bridge does not reliably begin delivering MSI when
	 * routing is enabled before the host programs the MSI capability.
	 */
	ret = gc555_bridge_start(gc555);
	if (!ret)
		return 0;

	gc555_bridge_write(gc555, GC555_BRIDGE_REG_AUDIO_DMA, 0);
	dev_err(gc555->dev, "bridge initialization failed: %d\n", ret);
	return ret;
}

void gc555_bridge_suspend(struct gc555_dev *gc555)
{
	if (!gc555 || !gc555->bridge.regs)
		return;

	gc555_bridge_set_host_irq_routing(gc555, false);
	gc555_bridge_write(gc555, GC555_BRIDGE_REG_AUDIO_DMA, 0);
	gc555->bridge.ready = false;
}

int gc555_bridge_resume(struct gc555_dev *gc555)
{
	int ret;

	if (!gc555 || !gc555->bridge.regs)
		return -ENODEV;

	/* Child I2C replay runs by polling until the bridge is fully restored. */
	ret = gc555_bridge_start(gc555);
	if (ret)
		dev_err(gc555->dev, "bridge resume failed: %d\n", ret);

	return ret;
}

int gc555_bridge_resume_complete(struct gc555_dev *gc555)
{
	if (!gc555_bridge_is_ready(gc555))
		return -ENODEV;

	return gc555_bridge_set_host_irq_routing(gc555, true);
}

int gc555_bridge_restore_host_irq_routing(struct gc555_dev *gc555)
{
	if (!gc555 || !gc555->bridge.regs)
		return -ENODEV;

	/* A rejected suspend leaves the initialized children operational. */
	gc555->bridge.ready = true;

	return gc555_bridge_set_host_irq_routing(gc555, true);
}

int gc555_bridge_set_host_irq_routing(struct gc555_dev *gc555, bool enable)
{
	u32 routing;
	u32 version;
	int ret;

	if (!gc555 || !gc555->bridge.regs)
		return -ENODEV;

	ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_VERSION, &version);
	if (ret)
		return ret;
	if (!enable && version >= GC555_BRIDGE_GC555_IRQ_MIN_VERSION) {
		ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_IRQ_ENABLE, 0);
		if (ret)
			return ret;
	}
	if (!enable) {
		WRITE_ONCE(gc555->bridge.host_irq_routing_enabled, false);
		/* Block new child transactions before their workers are drained. */
		gc555->bridge.ready = false;
	}

	ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_IRQ_STATUS,
				 GC555_BRIDGE_IRQ_STATUS_MASK);
	if (ret)
		return ret;

	if (enable && version >= GC555_BRIDGE_GC555_IRQ_MIN_VERSION) {
		ret = gc555_bridge_write(gc555, GC555_BRIDGE_REG_IRQ_ENABLE,
					 GC555_BRIDGE_GC555_IRQ_ENABLE);
		if (ret)
			return ret;
	}

	if (version >= GC555_BRIDGE_GC555_IRQ_MIN_VERSION) {
		ret = gc555_bridge_read(gc555, GC555_BRIDGE_REG_IRQ_ENABLE,
					&routing);
		if (ret)
			return ret;
		if (routing != (enable ? GC555_BRIDGE_GC555_IRQ_ENABLE : 0))
			dev_dbg(gc555->dev,
				"host irq enable: got %#x, expected %#x\n",
				routing,
				enable ? GC555_BRIDGE_GC555_IRQ_ENABLE : 0);
	}
	if (enable)
		WRITE_ONCE(gc555->bridge.host_irq_routing_enabled, true);

	return 0;
}

bool gc555_bridge_host_irq_routing_enabled(struct gc555_dev *gc555)
{
	return gc555 && READ_ONCE(gc555->bridge.host_irq_routing_enabled);
}

void gc555_bridge_cleanup(struct gc555_dev *gc555)
{
	if (!gc555 || !gc555->bridge.regs)
		return;

	gc555_bridge_suspend(gc555);
	gc555->bridge.regs = NULL;
	gc555->bridge.regs_size = 0;
}

void gc555_bridge_mark_disconnected(struct gc555_dev *gc555)
{
	if (!gc555)
		return;

	gc555->bridge.ready = false;
	WRITE_ONCE(gc555->bridge.host_irq_routing_enabled, false);
	gc555->bridge.regs = NULL;
	gc555->bridge.regs_size = 0;
}

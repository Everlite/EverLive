// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/version.h>

#include "gc573.h"

#define GC555_REG_INTERRUPT_STATUS	0x010
#define GC555_REG_INTERRUPT_ENABLE	0x01c
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
#define GC555_I2C_STATUS_TERMINAL	(GC555_I2C_STATUS_DONE | \
					 GC555_I2C_STATUS_ERROR)
#define GC555_I2C_MAX_ATTEMPTS		4
#define GC555_I2C_POLL_COUNT		2000
#define GC555_I2C_MAX_DATA_LEN		U8_MAX

static inline u32 gc555_i2c_bus_base(unsigned int bus)
{
	switch (bus) {
	case GC555_I2C_BUS_PRIMARY:
		return 0x0180;
	case GC555_I2C_BUS_SECONDARY:
		return 0x01c0;
	case GC555_I2C_BUS_TERTIARY:
		return 0x0120;
	default:
		return 0x0180;
	}
}

static inline u32 gc555_i2c_bus_irq(unsigned int bus)
{
	switch (bus) {
	case GC555_I2C_BUS_PRIMARY:
		return BIT(11);
	case GC555_I2C_BUS_SECONDARY:
		return BIT(12);
	case GC555_I2C_BUS_TERTIARY:
		return BIT(13);
	default:
		return 0;
	}
}

#define GC555_I2C_IRQ(bus)		gc555_i2c_bus_irq(bus)

struct gc555_i2c_bus {
	struct i2c_adapter adapter;
	struct gc555_dev *gc555;
	unsigned int index;
	struct completion completion;
	spinlock_t irq_lock;
	u8 completion_status;
	bool waiting;
};

struct gc555_i2c {
	struct mutex lock; /* Serializes both bridge I2C engines. */
	struct gc555_i2c_bus buses[GC555_I2C_BUS_COUNT];
	unsigned int registered_buses;
};

static int gc555_i2c_subaddr_code(unsigned int len)
{
	switch (len) {
	case 1:
		return 0;
	case 2:
		return 1;
	case 3:
		return 2;
	case 4:
		return 3;
	default:
		return -EINVAL;
	}
}

static void gc555_i2c_report_timeout(struct gc555_dev *gc555, u32 base,
				     unsigned int bus, bool read, u16 addr,
				     u32 subaddr)
{
	if (gc555_bridge_is_ready(gc555)) {
		u32 command = 0;
		u32 divisor = 0;
		u32 interrupt_enable = 0;
		u32 interrupt_status = 0;
		u32 status_word = 0;

		gc555_bridge_read(gc555, GC555_REG_INTERRUPT_STATUS,
				  &interrupt_status);
		gc555_bridge_read(gc555, GC555_REG_INTERRUPT_ENABLE,
				  &interrupt_enable);
		gc555_bridge_read(gc555, base + GC555_I2C_REG_COMMAND,
				  &command);
		gc555_bridge_read(gc555, base + GC555_I2C_REG_STATUS,
				  &status_word);
		gc555_bridge_read(gc555, base + GC555_I2C_REG_DIVISOR,
				  &divisor);
		dev_warn_ratelimited(
			gc555->dev,
			"I2C%u %s addr %#02x reg %#x timed out: command %#x status %#x divisor %u irq %#x enable %#x\n",
			bus, read ? "read" : "write", addr, subaddr, command,
			status_word, divisor, interrupt_status, interrupt_enable);
	}
}

static int gc555_i2c_wait(struct gc555_i2c_bus *bus, u32 base,
			  bool irq_wait, bool read, u16 addr, u32 subaddr,
			  u8 *status)
{
	struct gc555_dev *gc555 = bus->gc555;
	unsigned int poll;
	int ret;

	for (poll = 0; poll < GC555_I2C_POLL_COUNT; poll++) {
		if (irq_wait && try_wait_for_completion(&bus->completion)) {
			*status = READ_ONCE(bus->completion_status);
			if (*status & GC555_I2C_STATUS_TERMINAL)
				return 0;
		}

		/* Completion and error flags occupy the low status byte. */
		ret = gc555_bridge_read8(gc555, base + GC555_I2C_REG_STATUS,
					 status);
		if (ret)
			return ret;
		if (*status & GC555_I2C_STATUS_TERMINAL) {
			if (irq_wait) {
				unsigned long flags;

				spin_lock_irqsave(&bus->irq_lock, flags);
				if (bus->waiting) {
					bus->waiting = false;
					gc555_bridge_write(
						gc555,
						GC555_REG_INTERRUPT_STATUS,
						GC555_I2C_IRQ(bus->index));
				}
				spin_unlock_irqrestore(&bus->irq_lock,
						       flags);
			}
			return 0;
		}
		usleep_range(1000, 2000);
	}

	if (irq_wait) {
		unsigned long flags;

		spin_lock_irqsave(&bus->irq_lock, flags);
		bus->waiting = false;
		gc555_bridge_write(gc555, GC555_REG_INTERRUPT_STATUS,
				   GC555_I2C_IRQ(bus->index));
		spin_unlock_irqrestore(&bus->irq_lock, flags);
	}
	gc555_i2c_report_timeout(gc555, base, bus->index, read, addr,
				 subaddr);
	return -ETIMEDOUT;
}

static int gc555_i2c_attempt(struct gc555_dev *gc555, unsigned int bus,
			     bool read, u16 addr, u32 subaddr,
			     unsigned int subaddr_len, u8 *data,
			     unsigned int data_len)
{
	struct gc555_i2c_bus *i2c_bus = &gc555->i2c->buses[bus];
	u32 base = gc555_i2c_bus_base(bus);
	u32 slave = addr << 1;
	u8 status = 0;
	unsigned int i;
	bool irq_wait;
	unsigned long flags;
	int code;
	int ret;

	code = gc555_i2c_subaddr_code(subaddr_len);
	if (code < 0)
		return code;

	ret = gc555_bridge_write(gc555, base + GC555_I2C_REG_ADDRESS,
				 read ? slave | 1 : slave);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, base + GC555_I2C_REG_SUBADDR_LEN,
				 code);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, base + GC555_I2C_REG_SUBADDR,
				 subaddr);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, base + GC555_I2C_REG_COMMAND,
				 GC555_I2C_COMMAND_FIFO_RESET);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, base + GC555_I2C_REG_WIDTH, 0);
	if (ret)
		return ret;
	ret = gc555_bridge_write(gc555, base + GC555_I2C_REG_COUNT,
				 data_len);
	if (ret)
		return ret;

	if (!read) {
		for (i = 0; i < data_len; i++) {
			ret = gc555_bridge_write(gc555,
						 base + GC555_I2C_REG_TX_FIFO,
						 data[i]);
			if (ret)
				return ret;
		}
	}

	irq_wait = READ_ONCE(gc555->dma) != NULL;
	if (irq_wait) {
		spin_lock_irqsave(&i2c_bus->irq_lock, flags);
		i2c_bus->completion_status = 0;
		reinit_completion(&i2c_bus->completion);
		i2c_bus->waiting = true;
		spin_unlock_irqrestore(&i2c_bus->irq_lock, flags);
	}

	ret = gc555_bridge_write(gc555, base + GC555_I2C_REG_COMMAND,
				 read ? GC555_I2C_COMMAND_READ :
					GC555_I2C_COMMAND_WRITE);
	if (ret) {
		if (irq_wait) {
			spin_lock_irqsave(&i2c_bus->irq_lock, flags);
			i2c_bus->waiting = false;
			spin_unlock_irqrestore(&i2c_bus->irq_lock, flags);
		}
		return ret;
	}

	ret = gc555_i2c_wait(i2c_bus, base, irq_wait, read, addr, subaddr,
			     &status);
	if (ret)
		return ret;
	if (!(status & GC555_I2C_STATUS_DONE))
		return status & GC555_I2C_STATUS_ERROR ? -EREMOTEIO : -EIO;

	if (!read)
		return 0;

	ret = gc555_bridge_write(gc555, base + GC555_I2C_REG_COMMAND,
				 GC555_I2C_COMMAND_FIFO_RESET);
	if (ret)
		return ret;

	for (i = 0; i < data_len; i++) {
		u32 value;

		ret = gc555_bridge_read(gc555, base + GC555_I2C_REG_RX_FIFO,
					&value);
		if (ret)
			return ret;
		data[i] = value;
	}

	return 0;
}

u32 gc555_i2c_irq(struct gc555_dev *gc555, u32 irq_status)
{
	struct gc555_i2c *i2c;
	u32 acknowledged = 0;
	unsigned int i;

	if (!gc555)
		return 0;

	i2c = READ_ONCE(gc555->i2c);
	if (!i2c)
		return 0;

	for (i = 0; i < GC555_I2C_BUS_COUNT; i++) {
		struct gc555_i2c_bus *bus = &i2c->buses[i];
		unsigned long flags;
		u32 irq_bit = gc555_i2c_bus_irq(i);
		u32 base = gc555_i2c_bus_base(i);
		u8 status = 0;
		bool wake = false;

		if (!(irq_status & irq_bit))
			continue;

		spin_lock_irqsave(&bus->irq_lock, flags);
		if (bus->waiting) {
			gc555_bridge_read8(gc555,
					base + GC555_I2C_REG_STATUS, &status);
			bus->completion_status = status;
			bus->waiting = false;
			wake = true;
		}
		gc555_bridge_write(gc555, GC555_REG_INTERRUPT_STATUS,
				   irq_bit);
		acknowledged |= irq_bit;
		if (wake)
			complete(&bus->completion);
		spin_unlock_irqrestore(&bus->irq_lock, flags);
	}

	return acknowledged;
}

static int gc555_i2c_hw_xfer(struct gc555_dev *gc555, unsigned int bus,
			     bool read, u16 addr, u32 subaddr,
			     unsigned int subaddr_len, u8 *data,
			     unsigned int data_len)
{
	unsigned int attempt;
	int ret = -EIO;

	if (!gc555_bridge_is_ready(gc555))
		return -ESHUTDOWN;
	if (bus >= GC555_I2C_BUS_COUNT || addr > 0x7f || !data ||
	    !data_len || data_len > GC555_I2C_MAX_DATA_LEN)
		return -EINVAL;

	for (attempt = 0; attempt < GC555_I2C_MAX_ATTEMPTS; attempt++) {
		ret = gc555_i2c_attempt(gc555, bus, read, addr, subaddr,
					subaddr_len, data, data_len);
		if (ret != -ETIMEDOUT && ret != -EREMOTEIO)
			break;
	}

	return ret;
}

static int gc555_i2c_validate_msg(const struct i2c_msg *msg)
{
	const u16 supported_flags = I2C_M_RD | I2C_M_DMA_SAFE;

	if (!msg->buf || !msg->len || msg->addr > 0x7f)
		return -EINVAL;
	if (msg->flags & ~supported_flags)
		return -EOPNOTSUPP;
	if ((msg->flags & I2C_M_RD) && msg->len > GC555_I2C_MAX_DATA_LEN)
		return -EINVAL;
	if (!(msg->flags & I2C_M_RD) &&
	    msg->len > GC555_I2C_MAX_DATA_LEN + 1)
		return -EINVAL;

	return 0;
}

static u32 gc555_i2c_decode_subaddr(const u8 *data, unsigned int len)
{
	u32 subaddr = 0;
	unsigned int i;

	for (i = 0; i < len; i++)
		subaddr = (subaddr << 8) | data[i];

	return subaddr;
}

static int gc555_i2c_xfer(struct i2c_adapter *adapter,
			  struct i2c_msg *msgs, int num)
{
	struct gc555_i2c_bus *bus = i2c_get_adapdata(adapter);
	struct gc555_i2c *i2c = bus->gc555->i2c;
	int ret = 0;
	int i;

	if (!msgs || num <= 0)
		return -EINVAL;

	mutex_lock(&i2c->lock);
	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];

		ret = gc555_i2c_validate_msg(msg);
		if (ret)
			break;

		if (!(msg->flags & I2C_M_RD) && i + 1 < num &&
		    (msgs[i + 1].flags & I2C_M_RD)) {
			struct i2c_msg *read_msg = &msgs[i + 1];
			u32 subaddr;

			ret = gc555_i2c_validate_msg(read_msg);
			if (ret)
				break;
			if (read_msg->addr != msg->addr || msg->len > 4) {
				ret = -EOPNOTSUPP;
				break;
			}

			subaddr = gc555_i2c_decode_subaddr(msg->buf, msg->len);
			ret = gc555_i2c_hw_xfer(bus->gc555, bus->index, true,
						msg->addr, subaddr, msg->len,
						read_msg->buf, read_msg->len);
			if (ret)
				break;
			i++;
			continue;
		}

		if (msg->flags & I2C_M_RD) {
			ret = -EOPNOTSUPP;
			break;
		}
		if (msg->len < 2) {
			ret = -EOPNOTSUPP;
			break;
		}

		ret = gc555_i2c_hw_xfer(bus->gc555, bus->index, false,
					msg->addr, msg->buf[0], 1,
					msg->buf + 1, msg->len - 1);
		if (ret)
			break;
	}
	mutex_unlock(&i2c->lock);

	return ret ? ret : num;
}

static u32 gc555_i2c_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm gc555_i2c_algorithm = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	.xfer = gc555_i2c_xfer,
#else
	.master_xfer = gc555_i2c_xfer,
#endif
	.functionality = gc555_i2c_functionality,
};

static const struct i2c_adapter_quirks gc555_i2c_quirks = {
	.flags = I2C_AQ_COMB_WRITE_THEN_READ | I2C_AQ_NO_ZERO_LEN,
	.max_write_len = GC555_I2C_MAX_DATA_LEN + 1,
	.max_read_len = GC555_I2C_MAX_DATA_LEN,
	.max_comb_1st_msg_len = sizeof(u32),
	.max_comb_2nd_msg_len = GC555_I2C_MAX_DATA_LEN,
};

int gc555_i2c_init(struct gc555_dev *gc555)
{
	struct gc555_i2c *i2c;
	unsigned int i;
	int ret;

	if (!gc555_bridge_is_ready(gc555))
		return -ENODEV;

	i2c = devm_kzalloc(gc555->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	mutex_init(&i2c->lock);
	gc555->i2c = i2c;

	for (i = 0; i < GC555_I2C_BUS_COUNT; i++) {
		struct gc555_i2c_bus *bus = &i2c->buses[i];

		bus->gc555 = gc555;
		bus->index = i;
		init_completion(&bus->completion);
		spin_lock_init(&bus->irq_lock);
		bus->adapter.owner = THIS_MODULE;
		bus->adapter.algo = &gc555_i2c_algorithm;
		bus->adapter.quirks = &gc555_i2c_quirks;
		bus->adapter.dev.parent = gc555->dev;
		snprintf(bus->adapter.name, sizeof(bus->adapter.name),
			 "gc555 bridge I2C %u", i);
		i2c_set_adapdata(&bus->adapter, bus);

		ret = i2c_add_adapter(&bus->adapter);
		if (ret) {
			gc555_i2c_cleanup(gc555);
			return dev_err_probe(gc555->dev, ret,
					     "failed to register I2C bus %u\n",
					     i);
		}
		i2c->registered_buses++;
	}

	return 0;
}

void gc555_i2c_cleanup(struct gc555_dev *gc555)
{
	struct gc555_i2c *i2c;

	if (!gc555 || !gc555->i2c)
		return;

	i2c = gc555->i2c;
	while (i2c->registered_buses) {
		unsigned int index = --i2c->registered_buses;

		i2c_del_adapter(&i2c->buses[index].adapter);
	}
	gc555->i2c = NULL;
}

struct i2c_adapter *
gc555_i2c_get_adapter(struct gc555_dev *gc555,
		      enum gc555_i2c_bus_id bus)
{
	struct gc555_i2c *i2c;

	if (!gc555 || (unsigned int)bus >= GC555_I2C_BUS_COUNT)
		return NULL;

	i2c = gc555->i2c;
	if (!i2c || (unsigned int)bus >= i2c->registered_buses)
		return NULL;

	return &i2c->buses[bus].adapter;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * HDMI splitter and passthrough state machine, including EDID, HPD, TMDS/SCDC,
 * output format, equalization, and transmitter diagnostics.
 */
#include "gc570d.h"

static int gc570d_splitter_status_show(struct seq_file *s, void *unused)
{
	static const u8 identity_registers[] = {
		0x00, 0x01, 0x02, 0x03, 0x05, 0x15, 0x60,
		0x08, 0x0a, 0x0e, 0x0f, 0x10, 0xf0, 0xf1,
	};
	static const u8 irq_registers[] = { 0x06, 0x07 };
	static const u8 page1_registers[] = { 0x21, 0x22, 0x23 };
	static const u8 registers[] = {
		0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12,
		0x13, 0x14, 0x15, 0x19, 0x1a, 0x1b, 0x1d,
	};
	struct gc570d_dev *gc = s->private;
	u8 identity[ARRAY_SIZE(identity_registers)] = { 0 };
	u8 irq_values[ARRAY_SIZE(irq_registers)] = { 0 };
	u8 page1_values[ARRAY_SIZE(page1_registers)] = { 0 };
	u8 values[ARRAY_SIZE(registers)] = { 0 };
	u32 last_irq = 0;
	u32 last_status = 0;
	size_t i;
	int ret;

	mutex_lock(&gc->capture_lock);
	for (i = 0; i < ARRAY_SIZE(identity_registers); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58,
				       identity_registers[i], &identity[i],
				       &last_irq, &last_status);
		if (ret) {
			mutex_unlock(&gc->capture_lock);
			seq_printf(s,
				   "main slave8=0x58 bus=0 register=0x%02x error=%d irq=0x%08x status=0x%08x\n",
				   identity_registers[i], ret, last_irq,
				   last_status);
			return 0;
		}
	}
	for (i = 0; i < ARRAY_SIZE(irq_registers); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58,
				       irq_registers[i], &irq_values[i],
				       &last_irq, &last_status);
		if (ret) {
			mutex_unlock(&gc->capture_lock);
			return 0;
		}
	}

	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x0f,
				(identity[10] & ~BIT(0)) | BIT(0));
	if (!ret) {
		for (i = 0; i < ARRAY_SIZE(page1_registers); i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58,
					       page1_registers[i], &page1_values[i],
					       &last_irq, &last_status);
			if (ret)
				break;
		}
		gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x0f,
				      identity[10]);
	}
	if (ret) {
		mutex_unlock(&gc->capture_lock);
		return 0;
	}

	seq_printf(s,
		   "main slave8=0x58 bus=0 id=%02x:%02x:%02x:%02x reg05=0x%02x revision=0x%02x firmware=0x%02x firmware_ready=%s irq=0x%08x status=0x%08x\n",
		   identity[0], identity[1], identity[2], identity[3],
		   identity[4], identity[5], identity[6],
		   identity[6] == 0x19 ? "yes" : "no", last_irq,
		   last_status);
	seq_printf(s,
		   "main raw 08=%02x 0a=%02x 0e=%02x 0f=%02x 10=%02x f0=%02x f1=%02x\n",
		   identity[7], identity[8], identity[9], identity[10],
		   identity[11], identity[12], identity[13]);
	seq_printf(s,
		   "main irq raw 06=%02x 07=%02x page1 21=%02x 22=%02x 23=%02x\n",
		   irq_values[0], irq_values[1], page1_values[0],
		   page1_values[1], page1_values[2]);

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       registers[i], &values[i], &last_irq,
				       &last_status);
		if (ret) {
			mutex_unlock(&gc->capture_lock);
			seq_printf(s,
				   "detail slave8=0x70 bus=0 register=0x%02x error=%d irq=0x%08x status=0x%08x\n",
				   registers[i], ret, last_irq, last_status);
			return 0;
		}
	}
	mutex_unlock(&gc->capture_lock);

	seq_printf(s,
		   "detail slave8=0x70 bus=0 gpio40=0x%08x r13=0x%02x r19=0x%02x\n",
		   readl(gc->bar0 + GC570D_REG_XILINX_RESET), values[8],
		   values[11]);
	seq_printf(s,
		   "power5v=%s hdmi=%s clock_detect=%s clock_valid=%s clock_stable=%s ipll_stable=%s symbol_lock=%s opll_lock=%s opll_speed=%s scdt=%s\n",
		   values[8] & BIT(0) ? "yes" : "no",
		   values[8] & BIT(1) ? "yes" : "no",
		   values[8] & BIT(2) ? "yes" : "no",
		   values[8] & BIT(3) ? "yes" : "no",
		   values[8] & BIT(4) ? "yes" : "no",
		   values[8] & BIT(5) ? "yes" : "no",
		   values[8] & BIT(7) ? "yes" : "no",
		   values[11] & BIT(4) ? "yes" : "no",
		   values[11] & BIT(5) ? "high" : "low",
		   values[11] & BIT(7) ? "yes" : "no");
	seq_printf(s,
		   "raw 05=%02x 06=%02x 07=%02x 08=%02x 09=%02x 10=%02x 11=%02x 12=%02x 13=%02x 14=%02x 15=%02x 19=%02x 1a=%02x 1b=%02x 1d=%02x irq=0x%08x status=0x%08x\n",
		   values[0], values[1], values[2], values[3], values[4],
		   values[5], values[6], values[7], values[8], values[9],
		   values[10], values[11], values[12], values[13], values[14],
		   last_irq, last_status);
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_splitter_status);

static int gc570d_splitter_probe_show(struct seq_file *s, void *unused)
{
	/*
	 * VideoSplitter addresses are already 8-bit wire values.  Object byte
	 * b86 is a two-address port stride, producing the adjacent values below.
	 * Keep 0x90/e0 as known ACK controls, not as splitter identities.
	 */
	static const u8 addresses[] = {
		0x58, 0x5a, 0x68, 0x6a, 0x6c, 0x6e,
		0x70, 0x72, 0x74, 0x76, 0x90, 0x96, 0xe0,
	};
	struct gc570d_dev *gc = s->private;
	size_t bus_index;
	size_t address_index;

	seq_printf(s, "gpio40=0x%08x register=0x05 control=0x90\n",
		   readl(gc->bar0 + GC570D_REG_XILINX_RESET));
	for (bus_index = 0; bus_index < ARRAY_SIZE(gc570d_probe_buses);
	     bus_index++) {
		const struct gc570d_i2c_bus *bus =
			&gc570d_probe_buses[bus_index];

		for (address_index = 0; address_index < ARRAY_SIZE(addresses);
		     address_index++) {
			u32 last_irq = 0;
			u32 last_status = 0;
			u8 value = 0;
			int ret;

			ret = gc570d_i2c_read8(gc, bus, addresses[address_index],
					       0x05, &value, &last_irq,
					       &last_status);
			seq_printf(s,
				   "bus=%zu slave8=0x%02x ack=%s value=0x%02x error=%d irq=0x%08x status=0x%08x\n",
				   bus_index, addresses[address_index],
				   ret ? "no" : "yes", value, ret,
				   last_irq, last_status);
		}
	}
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_splitter_probe);

static int gc570d_splitter_update8(struct gc570d_dev *gc, u8 slave_addr8,
				   u8 address, u8 mask, u8 value)
{
	u32 last_irq;
	u32 last_status;
	u8 old_value;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave_addr8,
			       address, &old_value, &last_irq, &last_status);
	if (ret)
		return ret;

	return gc570d_i2c_write8(gc, &gc570d_splitter_bus, slave_addr8,
				 address,
				 (old_value & ~mask) | (value & mask));
}

int gc570d_splitter_core_preamble(struct gc570d_dev *gc)
{
	static const u8 identity_registers[] = { 0x00, 0x01, 0x02, 0x03 };
	static const u8 verify_registers[] = { 0x08, 0x0e, 0x10, 0xf0, 0xf1 };
	static const u8 auxiliary_addresses[] = { 0x68, 0x70, 0x96 };
	u8 identity[ARRAY_SIZE(identity_registers)] = { 0 };
	u8 verify[ARRAY_SIZE(verify_registers)] = { 0 };
	u32 last_irq = 0;
	u32 last_status = 0;
	u8 firmware;
	u8 probe_value;
	const char *stage = "identity";
	size_t i;
	int ret;

#define SPLITTER_READ(_reg, _value) do { \
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, (_reg), \
			       (_value), &last_irq, &last_status); \
	if (ret) \
		goto out; \
} while (0)
#define SPLITTER_WRITE(_reg, _value) do { \
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, (_reg), \
				(_value)); \
	if (ret) \
		goto out; \
} while (0)
#define SPLITTER_UPDATE(_reg, _mask, _value) do { \
	ret = gc570d_splitter_update8(gc, 0x58, (_reg), (_mask), (_value)); \
	if (ret) \
		goto out; \
} while (0)

	for (i = 0; i < ARRAY_SIZE(identity_registers); i++)
		SPLITTER_READ(identity_registers[i], &identity[i]);
	stage = "firmware-before-reset";
	SPLITTER_READ(0x60, &firmware);
	if (identity[0] != 0x54 || identity[1] != 0x49 ||
	    (identity[2] != 0x63 && identity[2] != 0x64) ||
	    identity[3] != 0x66) {
		ret = -ENODEV;
		goto out;
	}
	if (firmware != 0x19) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* Enter the official driver's state-2 reset sequence. */
	stage = "page-select-0f";
	SPLITTER_WRITE(0x0f, 0x00);
	stage = "logical-reset-assert-0a";
	SPLITTER_WRITE(0x0a, 0x01);
	stage = "logical-reset-release-0a";
	SPLITTER_WRITE(0x0a, 0x00);
	/* Repeated NACK probes inhibit recovery; match the official driver 30-ms tick. */
	stage = "firmware-after-quiet-delay";
	msleep(30);
	SPLITTER_READ(0x60, &firmware);
	if (firmware != 0x19) {
		ret = -EIO;
		goto out;
	}
	dev_info(&gc->pdev->dev,
		 "VideoSplitter core recovered after logical reset: quiet_ms=30 firmware=0x%02x\n",
		 firmware);
	stage = "write-10";
	SPLITTER_WRITE(0x10, 0x6e);
	stage = "write-f0-first";
	SPLITTER_WRITE(0xf0, 0x71);
	stage = "update-0e";
	SPLITTER_UPDATE(0x0e, 0x07, 0x00);
	stage = "update-08";
	SPLITTER_UPDATE(0x08, 0x0f, 0x0f);
	stage = "write-f0-second";
	SPLITTER_WRITE(0xf0, 0x71);
	stage = "write-f1";
	SPLITTER_WRITE(0xf1, 0x97);

	stage = "verify";
	for (i = 0; i < ARRAY_SIZE(verify_registers); i++)
		SPLITTER_READ(verify_registers[i], &verify[i]);
	dev_info(&gc->pdev->dev,
		 "VideoSplitter core preamble completed: id=%02x:%02x:%02x:%02x firmware=%02x verify 08=%02x 0e=%02x 10=%02x f0=%02x f1=%02x\n",
		 identity[0], identity[1], identity[2], identity[3], firmware,
		 verify[0], verify[1], verify[2], verify[3], verify[4]);

	for (i = 0; i < ARRAY_SIZE(auxiliary_addresses); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
				       auxiliary_addresses[i], 0x05,
				       &probe_value, &last_irq, &last_status);
		dev_info(&gc->pdev->dev,
			 "VideoSplitter post-preamble probe slave8=0x%02x ack=%s value=0x%02x error=%d\n",
			 auxiliary_addresses[i], ret ? "no" : "yes",
			 ret ? 0 : probe_value, ret);
	}
	ret = 0;

out:
	if (ret)
		dev_err(&gc->pdev->dev,
			"VideoSplitter core preamble failed at %s: %d\n",
			stage, ret);
#undef SPLITTER_READ
#undef SPLITTER_WRITE
#undef SPLITTER_UPDATE
	return ret;
}

static ssize_t gc570d_splitter_core_preamble_write(struct file *file,
						    const char __user *buffer,
						    size_t count,
						    loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_core_preamble(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_core_preamble_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_core_preamble_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_clock_init(struct gc570d_dev *gc)
{
	static const u8 setup_registers[] = {
		0x0f, 0x5f, 0x5f, 0x58, 0x58, 0x57,
	};
	static const u8 setup_values[] = {
		0x00, 0x04, 0x05, 0x12, 0x02, 0x01,
	};
	u16 initial[2] = { 0 };
	u16 sample[2] = { 0 };
	u32 last_irq = 0;
	u32 last_status = 0;
	u32 sipdata;
	u32 delta;
	u32 rclk;
	u32 time_lo_max;
	u16 seed;
	u8 value;
	u8 low;
	u8 high;
	u8 verify96[3];
	u8 verify58[2];
	const char *stage = "validate-preamble";
	bool cleanup_needed = false;
	size_t i;
	int cleanup_ret;
	int ret;

#define SPLITTER_CLOCK_READ(_slave, _reg, _value) do { \
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, (_slave), (_reg), \
			       (_value), &last_irq, &last_status); \
	if (ret) \
		goto out_cleanup; \
} while (0)
#define SPLITTER_CLOCK_WRITE(_slave, _reg, _value) do { \
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, (_slave), (_reg), \
				(_value)); \
	if (ret) \
		goto out_cleanup; \
} while (0)
#define SPLITTER_CLOCK_UPDATE(_slave, _reg, _mask, _value) do { \
	ret = gc570d_splitter_update8(gc, (_slave), (_reg), (_mask), (_value)); \
	if (ret) \
		goto out_cleanup; \
} while (0)

	SPLITTER_CLOCK_READ(0x58, 0x08, &value);
	if ((value & 0x0f) != 0x0f) {
		ret = -EAGAIN;
		goto out_cleanup;
	}
	SPLITTER_CLOCK_READ(0x58, 0x10, &value);
	if (value != 0x6e) {
		ret = -EAGAIN;
		goto out_cleanup;
	}
	stage = "validate-0x96";
	SPLITTER_CLOCK_READ(0x96, 0x05, &value);

	stage = "measurement-unlock";
	SPLITTER_CLOCK_WRITE(0x58, 0xff, 0xc3);
	cleanup_needed = true;
	SPLITTER_CLOCK_WRITE(0x58, 0xff, 0xa5);
	for (i = 0; i < ARRAY_SIZE(setup_registers); i++)
		SPLITTER_CLOCK_WRITE(0x58, setup_registers[i], setup_values[i]);

	stage = "initial-measurement";
	for (i = 0; i < ARRAY_SIZE(initial); i++) {
		SPLITTER_CLOCK_WRITE(0x58, 0x50, 0x00);
		SPLITTER_CLOCK_WRITE(0x58, 0x51, i);
		SPLITTER_CLOCK_WRITE(0x58, 0x54, 0x04);
		SPLITTER_CLOCK_READ(0x58, 0x61, &low);
		SPLITTER_CLOCK_READ(0x58, 0x62, &high);
		initial[i] = low | (high << 8);
	}

	seed = initial[0] != 0xffff || initial[1] != 0 ? 0x00b0 : 0x04b0;
	stage = "rclk-measurement";
	for (i = 0; i < ARRAY_SIZE(sample); i++) {
		seed += i;
		SPLITTER_CLOCK_WRITE(0x58, 0x50, (seed >> 8) & 0x0f);
		SPLITTER_CLOCK_WRITE(0x58, 0x51, seed & 0xff);
		SPLITTER_CLOCK_WRITE(0x58, 0x54, 0x04);
		SPLITTER_CLOCK_READ(0x58, 0x61, &low);
		SPLITTER_CLOCK_READ(0x58, 0x62, &high);
		sample[i] = low | (high << 8);
	}

	sipdata = ((sample[1] & 0xff) << 16) | sample[0];
	if ((sample[1] & 0xc000) == 0xc000)
		sipdata /= 100;
	delta = sipdata < 22000 ? 22000 - sipdata : sipdata - 22000;
	rclk = delta < 12001 ? sipdata : 22000;
	gc->splitter_rclk = rclk;
	time_lo_max = rclk * 10;

	stage = "program-derived-timing";
	SPLITTER_CLOCK_WRITE(0x96, 0x11, time_lo_max & 0xff);
	SPLITTER_CLOCK_WRITE(0x96, 0x12, (time_lo_max >> 8) & 0xff);
	SPLITTER_CLOCK_UPDATE(0x96, 0x13, 0x03,
				(time_lo_max >> 16) & 0x03);
	SPLITTER_CLOCK_UPDATE(0x58, 0x1e, 0x3f, (rclk / 1000) & 0x3f);
	SPLITTER_CLOCK_WRITE(0x58, 0x1f,
			       ((rclk % 1000) * 256) / 1000);

	stage = "verify-derived-timing";
	for (i = 0; i < ARRAY_SIZE(verify96); i++)
		SPLITTER_CLOCK_READ(0x96, 0x11 + i, &verify96[i]);
	for (i = 0; i < ARRAY_SIZE(verify58); i++)
		SPLITTER_CLOCK_READ(0x58, 0x1e + i, &verify58[i]);
	dev_info(&gc->pdev->dev,
		 "VideoSplitter RCLK initialized: initial=%04x/%04x samples=%04x/%04x sipdata=%u rclk=%u time_lo_max=%u verify96=%02x:%02x:%02x verify58=%02x:%02x\n",
		 initial[0], initial[1], sample[0], sample[1], sipdata, rclk,
		 time_lo_max, verify96[0], verify96[1], verify96[2],
		 verify58[0], verify58[1]);
	ret = 0;

out_cleanup:
	if (!cleanup_needed)
		goto out_log_error;
	cleanup_ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0x5f, 0x00);
	if (!ret && cleanup_ret) {
		stage = "measurement-cleanup";
		ret = cleanup_ret;
	}
	cleanup_ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0x0f, 0x00);
	if (!ret && cleanup_ret) {
		stage = "measurement-cleanup";
		ret = cleanup_ret;
	}
	cleanup_ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0xff, 0xff);
	if (!ret && cleanup_ret) {
		stage = "measurement-cleanup";
		ret = cleanup_ret;
	}
out_log_error:
	if (ret)
		dev_err(&gc->pdev->dev,
			"VideoSplitter RCLK initialization failed at %s: %d\n",
			stage, ret);

#undef SPLITTER_CLOCK_READ
#undef SPLITTER_CLOCK_WRITE
#undef SPLITTER_CLOCK_UPDATE
	return ret;
}

static ssize_t gc570d_splitter_clock_init_write(struct file *file,
						 const char __user *buffer,
						 size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_clock_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_clock_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_clock_init_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_route_init(struct gc570d_dev *gc)
{
	static const u8 route_registers[] = {
		0x50, 0x2c, 0x2d, 0x2e, 0x2f,
	};
	u8 verify[ARRAY_SIZE(route_registers)] = { 0 };
	u32 last_irq = 0;
	u32 last_status = 0;
	u8 value;
	const char *stage = "validate-rclk";
	size_t i;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x1e,
			       &value, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(value & 0x3f)) {
		ret = -EAGAIN;
		goto out;
	}
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x96, 0x13,
			       &value, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(value & 0x03)) {
		ret = -EAGAIN;
		goto out;
	}

	stage = "update-96-50";
	ret = gc570d_splitter_update8(gc, 0x96, 0x50, 0x04, 0x00);
	if (ret)
		goto out;
	stage = "write-96-2c";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x96, 0x2c, 0x69);
	if (ret)
		goto out;
	stage = "write-96-2d";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x96, 0x2d, 0x6b);
	if (ret)
		goto out;
	stage = "write-96-2e";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x96, 0x2e, 0x6d);
	if (ret)
		goto out;
	stage = "write-96-2f";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x96, 0x2f, 0x6f);
	if (ret)
		goto out;

	stage = "verify-route";
	for (i = 0; i < ARRAY_SIZE(route_registers); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x96,
				       route_registers[i], &verify[i],
				       &last_irq, &last_status);
		if (ret)
			goto out;
	}
	if ((verify[0] & 0x04) || verify[1] != 0x69 ||
	    verify[2] != 0x6b || verify[3] != 0x6d ||
	    verify[4] != 0x6f) {
		ret = -EILSEQ;
		goto out;
	}
	dev_info(&gc->pdev->dev,
		 "VideoSplitter auxiliary route initialized: verify96 50=%02x 2c=%02x 2d=%02x 2e=%02x 2f=%02x\n",
		 verify[0], verify[1], verify[2], verify[3], verify[4]);
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter auxiliary route failed at %s: %d\n", stage, ret);
	return ret;
}

static ssize_t gc570d_splitter_route_init_write(struct file *file,
						 const char __user *buffer,
						 size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_route_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_route_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_route_init_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_output_preamble(struct gc570d_dev *gc)
{
	static const u8 route_registers[] = { 0x2c, 0x2d, 0x2e, 0x2f };
	static const u8 route_values[] = { 0x69, 0x6b, 0x6d, 0x6f };
	u8 verify[3];
	u32 last_irq = 0;
	u32 last_status = 0;
	u8 value;
	const char *stage = "validate-route";
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(route_registers); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x96,
				       route_registers[i], &value,
				       &last_irq, &last_status);
		if (ret)
			goto out;
		if (value != route_values[i]) {
			ret = -EAGAIN;
			goto out;
		}
	}

#define SPLITTER_OUTPUT_WRITE(_stage, _reg, _value) do { \
	stage = (_stage); \
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, (_reg), \
				(_value)); \
	if (ret) \
		goto out; \
} while (0)

	SPLITTER_OUTPUT_WRITE("write-22-reset", 0x22, 0x08);
	SPLITTER_OUTPUT_WRITE("write-23-reset", 0x23, 0x01);
	SPLITTER_OUTPUT_WRITE("write-22-start", 0x22, 0x17);
	SPLITTER_OUTPUT_WRITE("write-24-start", 0x24, 0xf8);
	msleep(10);
	SPLITTER_OUTPUT_WRITE("write-23-release", 0x23, 0xa0);
	SPLITTER_OUTPUT_WRITE("write-22-stop", 0x22, 0x00);
	SPLITTER_OUTPUT_WRITE("write-24-stop", 0x24, 0x00);

	stage = "verify-output-preamble";
	for (i = 0; i < ARRAY_SIZE(verify); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       0x22 + i, &verify[i],
				       &last_irq, &last_status);
		if (ret)
			goto out;
	}
	if (verify[0] != 0x00 || verify[1] != 0xa0 || verify[2] != 0x00) {
		ret = -EILSEQ;
		goto out;
	}
	dev_info(&gc->pdev->dev,
		 "VideoSplitter output preamble completed: verify70 22=%02x 23=%02x 24=%02x\n",
		 verify[0], verify[1], verify[2]);
#undef SPLITTER_OUTPUT_WRITE
	return 0;

out:
#undef SPLITTER_OUTPUT_WRITE
	dev_err(&gc->pdev->dev,
		"VideoSplitter output preamble failed at %s: %d\n", stage, ret);
	return ret;
}

static ssize_t gc570d_splitter_output_preamble_write(struct file *file,
						       const char __user *buffer,
						       size_t count,
						       loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_output_preamble(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_output_preamble_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_output_preamble_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_post_reset_init(struct gc570d_dev *gc)
{
	u8 verify08 = 0;
	u8 verify24 = 0;
	u8 verify29 = 0;
	u8 verify3c = 0;
	u8 verifyce = 0;
	u8 caof_value = 0;
	u8 value = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "validate-output-preamble";
	unsigned int polls;
	int cleanup_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x22,
			       &value, &last_irq, &last_status);
	if (ret)
		goto out;
	if (value != 0x00) {
		ret = -EAGAIN;
		goto out;
	}
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x23,
			       &value, &last_irq, &last_status);
	if (ret)
		goto out;
	if (value != 0xa0) {
		ret = -EAGAIN;
		goto out;
	}
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x24,
			       &value, &last_irq, &last_status);
	if (ret)
		goto out;
	if (value != 0x00) {
		ret = -EAGAIN;
		goto out;
	}

#define SPLITTER_POST_UPDATE(_stage, _reg, _mask, _value) do { \
	stage = (_stage); \
	ret = gc570d_splitter_update8(gc, 0x70, (_reg), (_mask), (_value)); \
	if (ret) \
		goto out_restore_bank; \
} while (0)
#define SPLITTER_POST_WRITE(_stage, _reg, _value) do { \
	stage = (_stage); \
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, (_reg), \
				(_value)); \
	if (ret) \
		goto out_restore_bank; \
} while (0)
#define SPLITTER_POST_READ(_stage, _reg, _value) do { \
	stage = (_stage); \
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, (_reg), \
			       &(_value), &last_irq, &last_status); \
	if (ret) \
		goto out_restore_bank; \
} while (0)

	SPLITTER_POST_UPDATE("bank-0-initial", 0x0f, 0x03, 0x00);
	SPLITTER_POST_UPDATE("enable-29", 0x29, 0x01, 0x01);
	SPLITTER_POST_UPDATE("configure-2a", 0x2a, 0x41, 0x41);
	SPLITTER_POST_UPDATE("bank-3-initial", 0x0f, 0x03, 0x03);
	SPLITTER_POST_UPDATE("clear-3a", 0x3a, 0x80, 0x00);
	SPLITTER_POST_UPDATE("clear-3b", 0x3b, 0xc0, 0x00);
	SPLITTER_POST_UPDATE("set-a0", 0xa0, 0x80, 0x80);
	SPLITTER_POST_UPDATE("set-a1", 0xa1, 0x80, 0x80);
	SPLITTER_POST_UPDATE("set-a2", 0xa2, 0x80, 0x80);
	SPLITTER_POST_UPDATE("set-a7", 0xa7, 0x10, 0x10);
	SPLITTER_POST_UPDATE("set-48", 0x48, 0x80, 0x80);
	SPLITTER_POST_UPDATE("bank-0-caof", 0x0f, 0x03, 0x00);
	SPLITTER_POST_UPDATE("clear-2a-40", 0x2a, 0x40, 0x00);
	SPLITTER_POST_UPDATE("set-24-04", 0x24, 0x04, 0x04);
	SPLITTER_POST_WRITE("clear-25", 0x25, 0x00);
	SPLITTER_POST_WRITE("clear-26", 0x26, 0x00);
	SPLITTER_POST_WRITE("clear-27", 0x27, 0x00);
	SPLITTER_POST_WRITE("clear-28", 0x28, 0x00);
	SPLITTER_POST_UPDATE("clear-3c-10", 0x3c, 0x10, 0x00);
	SPLITTER_POST_UPDATE("bank-3-caof", 0x0f, 0x03, 0x03);
	SPLITTER_POST_UPDATE("set-3a-caof", 0x3a, 0x80, 0x80);
	SPLITTER_POST_UPDATE("bank-0-poll", 0x0f, 0x03, 0x00);

	SPLITTER_POST_READ("caof-initial-read", 0x08, value);
	for (polls = 0; polls < 31 && !(value & 0x30); polls++) {
		SPLITTER_POST_READ("caof-poll", 0x08, value);
		if (polls > 2) {
			SPLITTER_POST_UPDATE("caof-restart-set", 0x2a,
					     0x40, 0x40);
			SPLITTER_POST_UPDATE("caof-restart-clear", 0x2a,
					     0x40, 0x00);
		}
		/* FIX: Prevent tight infinite looping timeout hardware race conditions */
		msleep(10);
	}
	caof_value = value;
	if (!(value & 0x30)) {
		SPLITTER_POST_UPDATE("bank-3-caof-timeout", 0x0f, 0x03,
				     0x03);
		SPLITTER_POST_UPDATE("clear-3a-caof-timeout", 0x3a, 0x80,
				     0x00);
		SPLITTER_POST_UPDATE("bank-0-caof-timeout", 0x0f, 0x03,
				     0x00);
		SPLITTER_POST_UPDATE("caof-timeout-restart-set", 0x2a,
				     0x40, 0x40);
		SPLITTER_POST_UPDATE("caof-timeout-restart-clear", 0x2a,
				     0x40, 0x00);
	}

	SPLITTER_POST_UPDATE("bank-3-final", 0x0f, 0x03, 0x03);
	SPLITTER_POST_READ("read-5a", 0x5a, value);
	SPLITTER_POST_READ("read-59-first", 0x59, value);
	SPLITTER_POST_READ("read-59-second", 0x59, value);
	SPLITTER_POST_UPDATE("clear-3a-final", 0x3a, 0x80, 0x00);
	SPLITTER_POST_UPDATE("clear-a0", 0xa0, 0x80, 0x00);
	SPLITTER_POST_UPDATE("clear-a1", 0xa1, 0x80, 0x00);
	SPLITTER_POST_UPDATE("clear-a2", 0xa2, 0x80, 0x00);
	SPLITTER_POST_UPDATE("bank-0-final", 0x0f, 0x03, 0x00);
	SPLITTER_POST_UPDATE("set-08-30", 0x08, 0x30, 0x30);
	SPLITTER_POST_UPDATE("clear-29", 0x29, 0x01, 0x00);
	SPLITTER_POST_UPDATE("clear-24-04", 0x24, 0x04, 0x00);
	SPLITTER_POST_UPDATE("set-3c-10", 0x3c, 0x10, 0x10);
	SPLITTER_POST_UPDATE("clear-ce-20", 0xce, 0x20, 0x00);

	SPLITTER_POST_READ("verify-08", 0x08, verify08);
	SPLITTER_POST_READ("verify-24", 0x24, verify24);
	SPLITTER_POST_READ("verify-29", 0x29, verify29);
	SPLITTER_POST_READ("verify-3c", 0x3c, verify3c);
	SPLITTER_POST_READ("verify-ce", 0xce, verifyce);
	/* reg08[5:4] are status bits cleared by writing ones, not latches. */
	if ((verify08 & 0x30) || (verify24 & 0x04) || (verify29 & 0x01) ||
	    !(verify3c & 0x10) || (verifyce & 0x20)) {
		ret = -EILSEQ;
		stage = "verify-final";
		goto out_restore_bank;
	}

	dev_info(&gc->pdev->dev,
		 "VideoSplitter post-reset initialization completed: polls=%u caof=0x%02x verify70 08=%02x 24=%02x 29=%02x 3c=%02x ce=%02x\n",
		 polls, caof_value, verify08, verify24, verify29, verify3c,
		 verifyce);
#undef SPLITTER_POST_READ
#undef SPLITTER_POST_WRITE
#undef SPLITTER_POST_UPDATE
	return 0;

out_restore_bank:
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (cleanup_ret)
		dev_warn(&gc->pdev->dev,
			 "VideoSplitter failed to restore bank 0: %d\n",
			 cleanup_ret);
out:
#undef SPLITTER_POST_READ
#undef SPLITTER_POST_WRITE
#undef SPLITTER_POST_UPDATE
	dev_err(&gc->pdev->dev,
		"VideoSplitter post-reset initialization failed at %s: %d\n",
		stage, ret);
	return ret;
}

static ssize_t gc570d_splitter_post_reset_init_write(struct file *file,
						       const char __user *buffer,
						       size_t count,
						       loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_post_reset_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_post_reset_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_post_reset_init_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_output_mode_init(struct gc570d_dev *gc)
{
	struct gc570d_splitter_reg_op {
		u8 reg;
		u8 mask;
		u8 value;
		bool direct;
	};
	static const struct gc570d_splitter_reg_op operations[] = {
		{ 0x56, 0xff, 0xff, true  },
		{ 0x57, 0xff, 0xff, true  },
		{ 0x0f, 0x03, 0x03, false },
		{ 0xa8, 0x08, 0x08, false },
		{ 0xa7, 0x40, 0x40, false },
		{ 0x26, 0x20, 0x00, false },
		/* The official driver sets object+0xb85 to 1 for this path. */
		{ 0x27, 0xff, 0x9f, false },
		{ 0x28, 0xff, 0x9f, false },
		{ 0x29, 0xff, 0x9f, false },
		{ 0x0f, 0x03, 0x00, false },
		{ 0x28, 0x59, 0x59, false },
		{ 0x2a, 0x01, 0x01, false },
		{ 0x43, 0x02, 0x00, false },
		{ 0x44, 0x3f, 0x19, false },
		{ 0x3c, 0x01, 0x00, false },
		{ 0x45, 0xff, 0xdf, true  },
		{ 0x46, 0x3f, 0x15, false },
		{ 0x47, 0xff, 0x88, false },
		{ 0x49, 0xff, 0xe1, true  },
		{ 0x23, 0xff, 0xa0, true  },
		{ 0x53, 0xff, 0x0f, true  },
		{ 0xe3, 0xff, 0x04, true  },
		{ 0xce, 0x80, 0x00, false },
		{ 0x3c, 0x20, 0x00, false },
		{ 0x0f, 0x03, 0x03, false },
		{ 0xe3, 0x01, 0x01, false },
		{ 0xe3, 0x06, 0x03, false },
		{ 0xf0, 0xff, 0xa0, true  },
		{ 0x0f, 0x03, 0x00, false },
		{ 0x28, 0x88, 0x88, false },
		{ 0x3b, 0x20, 0x20, false },
		{ 0x26, 0xff, 0xff, true  },
		{ 0x42, 0x20, 0x00, false },
	};
	static const u8 bank0_registers[] = {
		0x23, 0x26, 0x28, 0x2a, 0x3b, 0x3c, 0x42, 0x43,
		0x44, 0x45, 0x46, 0x47, 0x49, 0x53, 0x56, 0x57,
		0xce, 0xe3,
	};
	static const u8 bank3_registers[] = {
		0x26, 0x27, 0x28, 0x29, 0xa7, 0xa8, 0xe3, 0xf0,
	};
	u8 bank0[ARRAY_SIZE(bank0_registers)] = { 0 };
	u8 bank3[ARRAY_SIZE(bank3_registers)] = { 0 };
	u32 last_irq = 0;
	u32 last_status = 0;
	u8 value;
	const char *stage = "validate-post-reset";
	size_t i;
	int cleanup_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x24,
			       &value, &last_irq, &last_status);
	if (ret)
		goto out;
	if (value & 0x04) {
		ret = -EAGAIN;
		goto out;
	}
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x3c,
			       &value, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(value & 0x10)) {
		ret = -EAGAIN;
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(operations); i++) {
		stage = "apply-operation";
		if (operations[i].direct)
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
						 operations[i].reg,
						 operations[i].value);
		else
			ret = gc570d_splitter_update8(gc, 0x70,
						      operations[i].reg,
						      operations[i].mask,
						      operations[i].value);
		if (ret) {
			dev_err(&gc->pdev->dev,
				"VideoSplitter output-mode operation %zu reg=%02x mask=%02x value=%02x failed: %d\n",
				i, operations[i].reg, operations[i].mask,
				operations[i].value, ret);
			goto out_restore_bank;
		}
	}

	stage = "verify-bank-0";
	for (i = 0; i < ARRAY_SIZE(bank0_registers); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       bank0_registers[i], &bank0[i],
				       &last_irq, &last_status);
		if (ret)
			goto out_restore_bank;
	}
	if (bank0[0] != 0xa0 || bank0[1] != 0xff ||
	    (bank0[2] & 0xd9) != 0xd9 || !(bank0[3] & 0x01) ||
	    !(bank0[4] & 0x20) || (bank0[5] & 0x21) ||
	    (bank0[6] & 0x20) || (bank0[7] & 0x02) ||
	    (bank0[8] & 0x3f) != 0x19 || bank0[9] != 0xdf ||
	    (bank0[10] & 0x3f) != 0x15 || bank0[11] != 0x88 ||
	    bank0[12] != 0xe1 || bank0[13] != 0x0f ||
	    bank0[14] != 0xff || bank0[15] != 0xff ||
	    (bank0[16] & 0x80) || bank0[17] != 0x04) {
		ret = -EILSEQ;
		goto out_restore_bank;
	}

	stage = "select-bank-3-verify";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
	if (ret)
		goto out_restore_bank;
	stage = "verify-bank-3";
	for (i = 0; i < ARRAY_SIZE(bank3_registers); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       bank3_registers[i], &bank3[i],
				       &last_irq, &last_status);
		if (ret)
			goto out_restore_bank;
	}
	if ((bank3[0] & 0x20) || bank3[1] != 0x9f ||
	    bank3[2] != 0x9f || bank3[3] != 0x9f ||
	    !(bank3[4] & 0x40) || !(bank3[5] & 0x08) ||
	    (bank3[6] & 0x07) != 0x03 || bank3[7] != 0xa0) {
		ret = -EILSEQ;
		goto out_restore_bank;
	}

	stage = "restore-bank-0";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		goto out_restore_bank;

	dev_info(&gc->pdev->dev,
		 "VideoSplitter output mode initialized: bank0 23=%02x 26=%02x 28=%02x 3b=%02x 3c=%02x 44=%02x 53=%02x e3=%02x bank3 27=%02x 28=%02x 29=%02x a7=%02x a8=%02x e3=%02x f0=%02x\n",
		 bank0[0], bank0[1], bank0[2], bank0[4], bank0[5],
		 bank0[8], bank0[13], bank0[17], bank3[1], bank3[2],
		 bank3[3], bank3[4], bank3[5], bank3[6], bank3[7]);
	return 0;

out_restore_bank:
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (cleanup_ret)
		dev_warn(&gc->pdev->dev,
			 "VideoSplitter output mode failed to restore bank 0: %d\n",
			 cleanup_ret);
out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter output mode initialization failed at %s: %d\n",
		stage, ret);
	return ret;
}

static ssize_t gc570d_splitter_output_mode_init_write(struct file *file,
							const char __user *buffer,
							size_t count,
							loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_output_mode_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_output_mode_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_output_mode_init_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_output_followup(struct gc570d_dev *gc)
{
	u8 hpd_initial = 0;
	u8 verify26 = 0;
	u8 verify53 = 0;
	u8 verify54 = 0;
	u8 verify55 = 0;
	u8 verify57 = 0;
	u8 verifyc5 = 0;
	u8 verify_main0a = 0;
	u8 bank3[5] = { 0 };
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "validate-output-mode";
	size_t i;
	int cleanup_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x26,
			       &verify26, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x28,
			       &verify53, &last_irq, &last_status);
	if (ret)
		goto out;
	if (verify26 != 0xff || (verify53 & 0xd9) != 0xd9) {
		ret = -EAGAIN;
		goto out;
	}

#define SPLITTER_FOLLOW_UPDATE(_stage, _slave, _reg, _mask, _value) do { \
	stage = (_stage); \
	ret = gc570d_splitter_update8(gc, (_slave), (_reg), (_mask), (_value)); \
	if (ret) \
		goto out_restore_bank; \
} while (0)
#define SPLITTER_FOLLOW_WRITE(_stage, _reg, _value) do { \
	stage = (_stage); \
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, (_reg), \
				(_value)); \
	if (ret) \
		goto out_restore_bank; \
} while (0)
#define SPLITTER_FOLLOW_READ(_stage, _slave, _reg, _value) do { \
	stage = (_stage); \
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, (_slave), (_reg), \
			       &(_value), &last_irq, &last_status); \
	if (ret) \
		goto out_restore_bank; \
} while (0)

	SPLITTER_FOLLOW_UPDATE("clear-53", 0x70, 0x53, 0xe0, 0x00);
	SPLITTER_FOLLOW_UPDATE("clear-54", 0x70, 0x54, 0xff, 0x00);
	SPLITTER_FOLLOW_UPDATE("clear-55-low", 0x70, 0x55, 0x07, 0x00);
	SPLITTER_FOLLOW_UPDATE("clear-57-low", 0x70, 0x57, 0x0f, 0x00);
	SPLITTER_FOLLOW_UPDATE("pulse-c5-set", 0x70, 0xc5, 0x10, 0x10);
	SPLITTER_FOLLOW_UPDATE("pulse-c5-clear", 0x70, 0xc5, 0x10, 0x00);
	SPLITTER_FOLLOW_UPDATE("main-pulse-set", 0x58, 0x0a, 0x04, 0x04);
	msleep(1);
	SPLITTER_FOLLOW_UPDATE("main-pulse-clear", 0x58, 0x0a, 0x04, 0x00);

	/* Initial state+0x15 is zero here, so the official driver calls helper(..., 0). */
	SPLITTER_FOLLOW_UPDATE("select-bank-3-hpd", 0x70, 0x0f, 0x03, 0x03);
	SPLITTER_FOLLOW_READ("read-hpd", 0x70, 0xab, hpd_initial);
	if (hpd_initial == 0xca) {
		SPLITTER_FOLLOW_WRITE("hpd-transition", 0xab, 0x4a);
		SPLITTER_FOLLOW_WRITE("hpd-low", 0xab, 0x00);
		SPLITTER_FOLLOW_WRITE("clear-ac", 0xac, 0x00);
	}
	SPLITTER_FOLLOW_UPDATE("select-bank-0-helper", 0x70, 0x0f, 0x03,
				 0x00);
	SPLITTER_FOLLOW_WRITE("write-26-helper", 0x26, 0xff);
	SPLITTER_FOLLOW_WRITE("write-55-helper", 0x55, 0x00);

	SPLITTER_FOLLOW_UPDATE("select-bank-3-final", 0x70, 0x0f, 0x03,
				 0x03);
	SPLITTER_FOLLOW_WRITE("write-27-final", 0x27, 0x9f);
	SPLITTER_FOLLOW_WRITE("write-28-final", 0x28, 0x9f);
	SPLITTER_FOLLOW_WRITE("write-29-final", 0x29, 0x9f);
	SPLITTER_FOLLOW_WRITE("write-20-final", 0x20, 0x1b);
	SPLITTER_FOLLOW_WRITE("write-21-final", 0x21, 0x03);
	SPLITTER_FOLLOW_UPDATE("select-bank-0-final", 0x70, 0x0f, 0x03,
				 0x00);

	SPLITTER_FOLLOW_READ("verify-26", 0x70, 0x26, verify26);
	SPLITTER_FOLLOW_READ("verify-53", 0x70, 0x53, verify53);
	SPLITTER_FOLLOW_READ("verify-54", 0x70, 0x54, verify54);
	SPLITTER_FOLLOW_READ("verify-55", 0x70, 0x55, verify55);
	SPLITTER_FOLLOW_READ("verify-57", 0x70, 0x57, verify57);
	SPLITTER_FOLLOW_READ("verify-c5", 0x70, 0xc5, verifyc5);
	SPLITTER_FOLLOW_READ("verify-main-0a", 0x58, 0x0a, verify_main0a);
	if (verify26 != 0xff || (verify53 & 0xe0) || verify54 != 0x00 ||
	    verify55 != 0x00 || (verify57 & 0x0f) || (verifyc5 & 0x10) ||
	    (verify_main0a & 0x04)) {
		ret = -EILSEQ;
		stage = "verify-bank-0";
		goto out_restore_bank;
	}

	SPLITTER_FOLLOW_UPDATE("select-bank-3-verify", 0x70, 0x0f, 0x03,
				 0x03);
	for (i = 0; i < ARRAY_SIZE(bank3); i++)
		SPLITTER_FOLLOW_READ("verify-bank-3", 0x70,
				     i < 3 ? 0x27 + i : 0x20 + i - 3,
				     bank3[i]);
	if (bank3[0] != 0x9f || bank3[1] != 0x9f ||
	    bank3[2] != 0x9f || bank3[3] != 0x1b || bank3[4] != 0x03) {
		ret = -EILSEQ;
		stage = "verify-bank-3-values";
		goto out_restore_bank;
	}
	SPLITTER_FOLLOW_UPDATE("restore-bank-0", 0x70, 0x0f, 0x03, 0x00);

	dev_info(&gc->pdev->dev,
		 "VideoSplitter output follow-up completed: hpd_initial=%02x bank0 26=%02x 53=%02x 54=%02x 55=%02x 57=%02x c5=%02x main0a=%02x bank3 27=%02x 28=%02x 29=%02x 20=%02x 21=%02x\n",
		 hpd_initial, verify26, verify53, verify54, verify55, verify57,
		 verifyc5, verify_main0a, bank3[0], bank3[1], bank3[2],
		 bank3[3], bank3[4]);
#undef SPLITTER_FOLLOW_READ
#undef SPLITTER_FOLLOW_WRITE
#undef SPLITTER_FOLLOW_UPDATE
	return 0;

out_restore_bank:
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (cleanup_ret)
		dev_warn(&gc->pdev->dev,
			 "VideoSplitter follow-up failed to restore bank 0: %d\n",
			 cleanup_ret);
out:
#undef SPLITTER_FOLLOW_READ
#undef SPLITTER_FOLLOW_WRITE
#undef SPLITTER_FOLLOW_UPDATE
	dev_err(&gc->pdev->dev,
		"VideoSplitter output follow-up failed at %s: %d\n",
		stage, ret);
	return ret;
}

static ssize_t gc570d_splitter_output_followup_write(struct file *file,
						       const char __user *buffer,
						       size_t count,
						       loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_output_followup(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_output_followup_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_output_followup_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_aux_enable_pulse(struct gc570d_dev *gc)
{
	u8 verify_c5 = 0;
	u8 verify_96_20 = 0;
	u8 aux_value = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	int aux_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x55,
			       &verify_c5, &last_irq, &last_status);
	if (ret)
		goto out;
	if (verify_c5 != 0x00) {
		ret = -EAGAIN;
		goto out;
	}

	ret = gc570d_splitter_update8(gc, 0x70, 0xc5, 0x01, 0x01);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x96, 0x20, 0x02);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x96, 0x20, 0x00);
	if (ret)
		goto out;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0xc5,
			       &verify_c5, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x96, 0x20,
			       &verify_96_20, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(verify_c5 & 0x01) || verify_96_20 != 0x00) {
		ret = -EILSEQ;
		goto out;
	}

	aux_ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x68, 0x00,
				   &aux_value, &last_irq, &last_status);
	dev_info(&gc->pdev->dev,
		 "VideoSplitter auxiliary enable pulse completed: c5=%02x 96_20=%02x aux68_ack=%s aux68_00=%02x aux_error=%d irq=0x%08x status=0x%08x\n",
		 verify_c5, verify_96_20, aux_ret ? "no" : "yes", aux_value,
		 aux_ret, last_irq, last_status);
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter auxiliary enable pulse failed: %d\n", ret);
	return ret;
}

static ssize_t gc570d_splitter_aux_enable_pulse_write(struct file *file,
							const char __user *buffer,
							size_t count,
							loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_aux_enable_pulse(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_aux_enable_pulse_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_aux_enable_pulse_write,
	.llseek = noop_llseek,
};

static int gc570d_splitter_aux_update8(struct gc570d_dev *gc, u8 port,
					 u8 reg, u8 mask, u8 value)
{
	return gc570d_splitter_update8(gc, 0x68 + 2 * port, reg, mask, value);
}

static int gc570d_splitter_aux_write8(struct gc570d_dev *gc, u8 port,
					u8 reg, u8 value)
{
	return gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x68 + 2 * port,
				 reg, value);
}

static int gc570d_splitter_aux_analog_setup(struct gc570d_dev *gc, u8 port)
{
	int ret;

	ret = gc570d_splitter_aux_write8(gc, port, 0x01, 0x06);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x94, 0x01, 0x01);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x94, 0x01, 0x00);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_write8(gc, port, 0x01, 0x04);
	if (ret)
		return ret;
	return gc570d_splitter_aux_write8(gc, port, 0x01, 0x00);
}

static int gc570d_splitter_aux_port_setup(struct gc570d_dev *gc, u8 port)
{
	static const u8 zero_registers[] = { 0x19, 0x1a, 0x1b, 0x1c };
	size_t i;
	int ret;

	ret = gc570d_splitter_aux_analog_setup(gc, port);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x01, 0x20, 0x20);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x01, 0x20, 0x00);
	if (ret)
		return ret;
	ret = gc570d_splitter_update8(gc, 0x58, 0x0c, 1U << (port + 4),
				       1U << (port + 4));
	if (ret)
		return ret;
	ret = gc570d_splitter_update8(gc, 0x58, 0x0c, 1U << (port + 4), 0x00);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x18, 0x80, 0x00);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(zero_registers); i++) {
		ret = gc570d_splitter_aux_write8(gc, port, zero_registers[i],
						 0x00);
		if (ret)
			return ret;
	}
	ret = gc570d_splitter_aux_update8(gc, port, 0x35, 0x10, 0x10);
	if (ret)
		return ret;
	return gc570d_splitter_aux_update8(gc, port, 0x35, 0x10, 0x00);
}

static int gc570d_splitter_aux_power_down(struct gc570d_dev *gc, u8 port)
{
	static const struct {
		u8 reg;
		u8 mask;
		u8 value;
	} operations[] = {
		{ 0x18, 0xdc, 0x00 },
		{ 0x19, 0x07, 0x00 },
		{ 0x1a, 0xff, 0x00 },
		{ 0x1b, 0xff, 0x00 },
		{ 0x1c, 0xff, 0x00 },
		{ 0x84, 0x60, 0x60 },
		{ 0x84, 0x80, 0x00 },
		{ 0x86, 0x08, 0x00 },
		{ 0x88, 0x03, 0x03 },
		{ 0x01, 0x26, 0x26 },
	};
	u8 ignored;
	u32 last_irq;
	u32 last_status;
	size_t i;
	int ret;

	ret = gc570d_splitter_update8(gc, 0x58, 0x08, 1U << port, 0x00);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(operations); i++) {
		ret = gc570d_splitter_aux_update8(gc, port, operations[i].reg,
						  operations[i].mask,
						  operations[i].value);
		if (ret)
			return ret;
	}

	/* Initial software port state is zero, so the official driver skips its state branches. */
	ret = gc570d_splitter_update8(gc, 0x58, 0x0d,
				      0x03U << (port * 2), 0x00);
	if (ret)
		return ret;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a, 0x03,
			       &ignored, &last_irq, &last_status);
	if (ret)
		return ret;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6c, 0x03,
			       &ignored, &last_irq, &last_status);
	if (ret)
		return ret;
	return gc570d_splitter_aux_port_setup(gc, port);
}

static int gc570d_splitter_aux_table(struct gc570d_dev *gc, u8 port)
{
	static const struct {
		u8 reg;
		u8 mask;
		u8 value;
	} operations[] = {
		{ 0x08, 0x1c, 0x00 },
		{ 0x02, 0x02, 0x02 },
		{ 0x41, 0x01, 0x00 },
		{ 0xc0, 0x01, 0x01 },
		{ 0x34, 0xc0, 0x80 },
		{ 0x35, 0x03, 0x00 },
		{ 0x3a, 0xfc, 0x90 },
		{ 0x93, 0xff, 0x40 },
		{ 0x94, 0x3e, 0x26 },
		{ 0xc0, 0x10, 0x10 },
		{ 0xc1, 0x04, 0x00 },
		{ 0xc3, 0x0f, 0x01 },
		{ 0x18, 0x03, 0x03 },
	};
	static const struct {
		u8 reg;
		u8 value;
	} writes[] = {
		{ 0x19, 0x07 },
		{ 0x1a, 0x03 },
		{ 0x1b, 0xff },
		{ 0x1c, 0x03 },
		{ 0x88, 0x54 },
		{ 0x8a, 0x00 },
		{ 0x8b, 0x07 },
	};
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(operations); i++) {
		ret = gc570d_splitter_aux_update8(gc, port, operations[i].reg,
						  operations[i].mask,
						  operations[i].value);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(writes); i++) {
		ret = gc570d_splitter_aux_write8(gc, port, writes[i].reg,
						 writes[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

int gc570d_splitter_aux_ports_init(struct gc570d_dev *gc)
{
	u8 verify[4][4] = { { 0 } };
	u8 verify_main08 = 0;
	u8 verify_main0d = 0;
	u8 verify_96_15 = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "validate-aux-visible";
	u8 port;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x68, 0x00,
			       &verify[0][0], &last_irq, &last_status);
	if (ret)
		goto out;

	stage = "power-down-port-0";
	ret = gc570d_splitter_aux_power_down(gc, 0);
	if (ret)
		goto out;
	stage = "power-down-port-3";
	ret = gc570d_splitter_aux_power_down(gc, 3);
	if (ret)
		goto out;

	for (port = 1; port < 3; port++) {
		stage = port == 1 ? "prepare-port-1" : "prepare-port-2";
		ret = gc570d_splitter_aux_update8(gc, port, 0xc1, 0x03, 0x03);
		if (ret)
			goto out;
		ret = gc570d_splitter_aux_update8(gc, port, 0x01, 0x03, 0x01);
		if (ret)
			goto out;
		ret = gc570d_splitter_aux_update8(gc, port, 0x01, 0x01, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_aux_table(gc, port);
		if (ret)
			goto out;
		ret = gc570d_splitter_aux_port_setup(gc, port);
		if (ret)
			goto out;
		ret = gc570d_splitter_aux_power_down(gc, port);
		if (ret)
			goto out;
	}

	stage = "enable-96-15";
	ret = gc570d_splitter_update8(gc, 0x96, 0x15, 0x08, 0x08);
	if (ret)
		goto out;

	stage = "verify";
	for (port = 0; port < 4; port++) {
		static const u8 registers[] = { 0x01, 0x18, 0x19, 0x84 };
		size_t i;

		for (i = 0; i < ARRAY_SIZE(registers); i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
					       0x68 + 2 * port, registers[i],
					       &verify[port][i], &last_irq,
					       &last_status);
			if (ret)
				goto out;
		}
		if ((verify[port][0] & 0x26) != 0x00 ||
		    (verify[port][1] & 0xdf) != 0x03 ||
		    verify[port][2] != 0x00 ||
		    (verify[port][3] & 0xe0) != 0x60) {
			dev_err(&gc->pdev->dev,
				"VideoSplitter internal channel %u verify mismatch: 01=%02x 18=%02x 19=%02x 84=%02x\n",
				port, verify[port][0], verify[port][1],
				verify[port][2], verify[port][3]);
			ret = -EILSEQ;
			goto out;
		}
	}
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x08,
			       &verify_main08, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x0d,
			       &verify_main0d, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x96, 0x15,
			       &verify_96_15, &last_irq, &last_status);
	if (ret)
		goto out;
	if ((verify_main08 & 0x0f) || verify_main0d != 0x00 ||
	    !(verify_96_15 & 0x08)) {
		dev_err(&gc->pdev->dev,
			"VideoSplitter auxiliary final verify mismatch: main08=%02x main0d=%02x 96_15=%02x\n",
			verify_main08, verify_main0d, verify_96_15);
		ret = -EILSEQ;
		goto out;
	}

	dev_info(&gc->pdev->dev,
		 "VideoSplitter auxiliary internal channels initialized: main08=%02x main0d=%02x 96_15=%02x ch0=%02x/%02x/%02x/%02x ch1=%02x/%02x/%02x/%02x ch2=%02x/%02x/%02x/%02x ch3=%02x/%02x/%02x/%02x\n",
		 verify_main08, verify_main0d, verify_96_15,
		 verify[0][0], verify[0][1], verify[0][2], verify[0][3],
		 verify[1][0], verify[1][1], verify[1][2], verify[1][3],
		 verify[2][0], verify[2][1], verify[2][2], verify[2][3],
		 verify[3][0], verify[3][1], verify[3][2], verify[3][3]);
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter auxiliary internal channels initialization failed at %s: %d\n",
		stage, ret);
	return ret;
}

static ssize_t gc570d_splitter_aux_ports_init_write(struct file *file,
						      const char __user *buffer,
						      size_t count,
						      loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_aux_ports_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_aux_ports_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_aux_ports_init_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_irq_init(struct gc570d_dev *gc)
{
	static const struct {
		u8 reg;
		u8 mask;
		u8 value;
	} main_setup[] = {
		{ 0x08, 0x0f, 0x0f }, { 0x0d, 0xff, 0x00 },
		{ 0x6b, 0x3c, 0x00 }, { 0x6c, 0x38, 0x20 },
		{ 0x18, 0x10, 0x10 }, { 0x0f, 0x01, 0x01 },
		{ 0x10, 0x49, 0x41 }, { 0x1d, 0x80, 0x80 },
		{ 0x20, 0x78, 0x78 }, { 0x0f, 0x01, 0x00 },
		{ 0x19, 0x3f, 0x0f },
	};
	static const struct {
		u8 reg;
		u8 mask;
		u8 value;
	} aux_setup[] = {
		{ 0x41, 0x01, 0x00 },
		{ 0xc1, 0x01, 0x01 },
		{ 0x88, 0x01, 0x01 },
	};
	static const struct {
		u8 reg;
		u8 mask;
		u8 value;
	} main_finish[] = {
		{ 0x1a, 0x01, 0x00 }, { 0x19, 0x10, 0x00 },
		{ 0x19, 0x10, 0x10 }, { 0x1a, 0x01, 0x01 },
	};
	static const u8 verify_regs[] = {
		0x08, 0x0d, 0x6b, 0x6c, 0x18, 0x10,
		0x1d, 0x20, 0x19, 0x07, 0x1a,
	};
	u8 verify[ARRAY_SIZE(verify_regs)] = { 0 };
	u8 aux_verify[4][3] = { { 0 } };
	u8 ignored;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "read-initial-status";
	u8 port;
	size_t i;
	int ret;

	/* Match the read-only prefix of the official driver's state-2 path. */
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x68, 0x03,
			       &ignored, &last_irq, &last_status);
	if (ret)
		goto out;
	for (i = 0; i < 3; i++) {
		static const u8 regs[] = { 0x84, 0x86, 0x88 };

		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x68,
				       regs[i], &ignored, &last_irq, &last_status);
		if (ret)
			goto out;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6e,
				       regs[i], &ignored, &last_irq, &last_status);
		if (ret)
			goto out;
	}

	for (i = 0; i < ARRAY_SIZE(main_setup); i++) {
		stage = "main-setup";
		ret = gc570d_splitter_update8(gc, 0x58, main_setup[i].reg,
					      main_setup[i].mask,
					      main_setup[i].value);
		if (ret)
			goto out;
	}

	stage = "main-set-2b";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x2b, 0xff);
	if (ret)
		goto out;
	stage = "main-set-2d";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x2d, 0x0f);
	if (ret)
		goto out;
	stage = "main-set-2e";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x2e, 0xff);
	if (ret)
		goto out;
	stage = "main-set-30";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x30, 0x0f);
	if (ret)
		goto out;
	stage = "main-clear-6d";
	ret = gc570d_splitter_update8(gc, 0x58, 0x6d, 0x30, 0x00);
	if (ret)
		goto out;

	msleep(10);
	for (port = 0; port < 4; port++) {
		for (i = 0; i < ARRAY_SIZE(aux_setup); i++) {
			stage = "aux-irq-setup";
			ret = gc570d_splitter_aux_update8(gc, port,
						  aux_setup[i].reg,
						  aux_setup[i].mask,
						  aux_setup[i].value);
			if (ret)
				goto out;
		}
	}

	for (i = 0; i < ARRAY_SIZE(main_finish); i++) {
		stage = "main-finish";
		ret = gc570d_splitter_update8(gc, 0x58, main_finish[i].reg,
					      main_finish[i].mask,
					      main_finish[i].value);
		if (ret)
			goto out;
		if (i == 1) {
			stage = "main-set-1c";
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
						0x58, 0x1c, 0x2f);
			if (ret)
				goto out;
		} else if (i == 2) {
			stage = "main-set-07";
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
						0x58, 0x07, 0x1f);
			if (ret)
				goto out;
		}
	}

	stage = "verify";
	for (i = 0; i < ARRAY_SIZE(verify_regs); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58,
				       verify_regs[i], &verify[i],
				       &last_irq, &last_status);
		if (ret)
			goto out;
	}
	for (port = 0; port < 4; port++) {
		static const u8 regs[] = { 0x41, 0xc1, 0x88 };

		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
					       0x68 + 2 * port, regs[i],
					       &aux_verify[port][i],
					       &last_irq, &last_status);
			if (ret)
				goto out;
		}
		if ((aux_verify[port][0] & 0x01) ||
		    !(aux_verify[port][1] & 0x01) ||
		    !(aux_verify[port][2] & 0x01)) {
			ret = -EILSEQ;
			goto out;
		}
	}
	if ((verify[0] & 0x0f) != 0x0f || verify[1] != 0x00 ||
	    (verify[2] & 0x3c) != 0x00 || (verify[3] & 0x38) != 0x20 ||
	    !(verify[4] & 0x10) || !(verify[6] & 0x80) ||
	    (verify[8] & 0x3f) != 0x1f || verify[9] != 0x00 ||
	    !(verify[10] & 0x01)) {
		dev_err(&gc->pdev->dev,
			"VideoSplitter state-2 main verify mismatch: 08=%02x 0d=%02x 6b=%02x 6c=%02x 18=%02x 10=%02x 1d=%02x 20=%02x 19=%02x 07=%02x 1a=%02x\n",
			verify[0], verify[1], verify[2], verify[3], verify[4],
			verify[5], verify[6], verify[7], verify[8], verify[9],
			verify[10]);
		ret = -EILSEQ;
		goto out;
	}

	dev_info(&gc->pdev->dev,
		 "VideoSplitter state-2 interrupt setup completed: main 08=%02x 0d=%02x 6b=%02x 6c=%02x 18=%02x 10=%02x 1d=%02x 20=%02x 19=%02x 07=%02x 1a=%02x aux0=%02x/%02x/%02x aux1=%02x/%02x/%02x aux2=%02x/%02x/%02x aux3=%02x/%02x/%02x\n",
		 verify[0], verify[1], verify[2], verify[3], verify[4],
		 verify[5], verify[6], verify[7], verify[8], verify[9],
		 verify[10], aux_verify[0][0], aux_verify[0][1],
		 aux_verify[0][2], aux_verify[1][0], aux_verify[1][1],
		 aux_verify[1][2], aux_verify[2][0], aux_verify[2][1],
		 aux_verify[2][2], aux_verify[3][0], aux_verify[3][1],
		 aux_verify[3][2]);
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter state-2 interrupt setup failed at %s: %d\n",
		stage, ret);
	return ret;
}

static ssize_t gc570d_splitter_irq_init_write(struct file *file,
					       const char __user *buffer,
					       size_t count,
					       loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_irq_init(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_irq_init_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_irq_init_write,
	.llseek = noop_llseek,
};

struct gc570d_splitter_clock_sample {
	u32 initial_counter;
	u32 counter_sum;
	u32 divisor;
	u8 divider;
	u8 depth_code;
};

int gc570d_splitter_measure_rx_timing(
	struct gc570d_dev *gc, u8 input_color,
	struct gc570d_splitter_rx_timing *timing)
{
	static const u8 timing_regs[] = {
		0x9c, 0x9b, 0x9e, 0x9d, 0xa1, 0xa0, 0x9f, 0xaa,
		0xa3, 0xa2, 0xa5, 0xa4, 0xa8, 0xa7, 0xa6,
	};
	u8 values[ARRAY_SIZE(timing_regs)] = { 0 };
	u32 counter_sum = 0;
	u32 denominator;
	u32 line_rate;
	u32 pixel_clock;
	u32 tmds_clock;
	u32 last_irq = 0;
	u32 last_status = 0;
	u8 reg99;
	u8 reg9a;
	u8 reg98;
	u8 discard;
	size_t i;
	int ret;

	if (!gc->splitter_rclk)
		return -EAGAIN;

	/* The official driver first flushes the receiver's measurement pipeline,
	 * then averages one hundred 10-bit clock-counter samples. Preserve that
	 * order exactly; unlike the transmitter counter this path requires no
	 * format or output writes.
	 */
	for (i = 0; i < 100; i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       0x48, &discard, &last_irq, &last_status);
		if (ret)
			return ret;
	}
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x43,
			       &discard, &last_irq, &last_status);
	if (ret)
		return ret;
	for (i = 0; i < 100; i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       0x9a, &reg9a, &last_irq, &last_status);
		if (ret)
			return ret;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       0x99, &reg99, &last_irq, &last_status);
		if (ret)
			return ret;
		counter_sum += ((reg9a & 0x03) << 8) | reg99;
	}
	for (i = 0; i < ARRAY_SIZE(timing_regs); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       timing_regs[i], &values[i], &last_irq,
				       &last_status);
		if (ret)
			return ret;
	}
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x98,
			       &reg98, &last_irq, &last_status);
	if (ret)
		return ret;

	denominator = counter_sum ? counter_sum : 1;
	pixel_clock = (u32)(((u64)gc->splitter_rclk * 0xc800 /
			     denominator) * 107 / 100);
	tmds_clock = pixel_clock;
	timing->depth_code = (reg98 >> 4) & 0x03;
	if (timing->depth_code == 1)
		tmds_clock = tmds_clock * 125 / 100;
	else if (timing->depth_code == 2)
		tmds_clock = tmds_clock * 150 / 100;

	timing->counter_sum = counter_sum;
	timing->pixel_clock = pixel_clock;
	timing->tmds_clock = tmds_clock;
	timing->htotal = ((values[0] & 0x3f) << 8) | values[1];
	timing->hactive = ((values[2] & 0x3f) << 8) | values[3];
	timing->hfront = ((values[4] & 0xf0) << 4) | values[5];
	timing->hsync = ((values[4] & 0x01) << 8) | values[6];
	timing->vtotal = ((values[8] & 0x3f) << 8) | values[9];
	timing->vactive = ((values[10] & 0x3f) << 8) | values[11];
	timing->vfront = ((values[12] & 0xf0) << 4) | values[13];
	timing->vsync = ((values[12] & 0x01) << 8) | values[14];
	timing->flags = values[7];
	if (input_color == 3)
		timing->hactive *= 2;
	line_rate = pixel_clock * 1000 /
		(timing->htotal ? timing->htotal : 1);
	timing->frame_rate = line_rate /
		(timing->vtotal ? timing->vtotal : 1);
	return 0;
}

static int gc570d_splitter_measure_channel_clock(
	struct gc570d_dev *gc, u8 port, u32 *pclk_out, u32 *vclk_out,
	struct gc570d_splitter_clock_sample *sample)
{
	u32 counter_sum = 0;
	u32 counter;
	u32 divisor;
	u32 last_irq = 0;
	u32 last_status = 0;
	u32 pclk;
	u8 divider = 7;
	u8 reg06;
	u8 reg07;
	u8 reg98;
	u8 value;
	size_t i;
	int ret;

	if (!gc->splitter_rclk)
		return -EAGAIN;

	/* The official driver selects the counter prescaler from one sample, then
	 * averages ten gated measurements before deriving PCLK and VCLK. These
	 * are measurement-control writes only; no transmitter format or output
	 * register is programmed here.
	 */
	ret = gc570d_splitter_aux_update8(gc, port, 0x07, 0x80, 0x80);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	ret = gc570d_splitter_aux_update8(gc, port, 0x07, 0x80, 0x00);
	if (ret)
		return ret;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x68 + 2 * port,
			       0x06, &reg06, &last_irq, &last_status);
	if (ret)
		return ret;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x68 + 2 * port,
			       0x07, &reg07, &last_irq, &last_status);
	if (ret)
		return ret;
	counter = (((reg07 & 0x0f) << 8) | reg06) * 2;
	if (counter > 0x3ff)
		divider = 0;
	else if (counter > 0x1ff)
		divider = 1;
	else if (counter > 0xff)
		divider = 2;
	else if (counter > 0x7f)
		divider = 3;
	else if (counter > 0x3f)
		divider = 4;
	else if (counter > 0x1f)
		divider = 5;
	else if (counter > 0x0f)
		divider = 6;

	for (i = 0; i < 10; i++) {
		ret = gc570d_splitter_aux_update8(gc, port, 0x07, 0xf0,
						  (divider + 8) << 4);
		if (ret)
			return ret;
		ret = gc570d_splitter_aux_update8(gc, port, 0x07, 0xf0,
						  divider << 4);
		if (ret)
			return ret;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
				       0x68 + 2 * port, 0x06, &reg06,
				       &last_irq, &last_status);
		if (ret)
			return ret;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
				       0x68 + 2 * port, 0x07, &reg07,
				       &last_irq, &last_status);
		if (ret)
			return ret;
		counter_sum += (((reg07 & 0x0f) << 8) | reg06) * 2;
	}

	divisor = counter_sum / (10U << divider);
	if (!divisor)
		divisor = 1;
	pclk = (u32)((((u64)gc->splitter_rclk << 12) / divisor) * 107 / 100);
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x98,
			       &reg98, &last_irq, &last_status);
	if (ret)
		return ret;
	value = (reg98 >> 4) & 0x03;
	if (value == 1)
		pclk = pclk * 5 / 4;
	else if (value == 2)
		pclk = pclk * 3 / 2;

	*pclk_out = pclk;
	*vclk_out = pclk * 107 / 100;
	if (sample) {
		sample->initial_counter = counter;
		sample->counter_sum = counter_sum;
		sample->divisor = divisor;
		sample->divider = divider;
		sample->depth_code = value;
	}
	return 0;
}

static int gc570d_splitter_aux_power_up_common(struct gc570d_dev *gc, u8 port)
{
	static const struct {
		u8 reg;
		u8 mask;
		u8 value;
	} power_up[] = {
		{ 0xc1, 0xf0, 0x80 }, { 0x86, 0x08, 0x08 },
		{ 0x84, 0xe0, 0x00 }, { 0x88, 0x03, 0x01 },
		{ 0x84, 0x80, 0x80 }, { 0x02, 0x01, 0x00 },
		{ 0x19, 0x07, 0x07 }, { 0xaf, 0xff, 0x00 },
	};
	size_t i;
	int ret;

	ret = gc570d_splitter_update8(gc, 0x58, 0x08, BIT(port), BIT(port));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(power_up); i++) {
		ret = gc570d_splitter_aux_update8(gc, port, power_up[i].reg,
						  power_up[i].mask,
						  power_up[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

static int gc570d_splitter_scdt_channel_setup(struct gc570d_dev *gc, u8 port,
					       u32 *pclk_out, u32 *vclk_out)
{
	u32 pclk;
	u32 vclk;
	u8 reg87;
	u8 reg89;
	u8 reg8b;
	u8 reg;
	int ret;

	if (!gc->splitter_rclk)
		return -EAGAIN;

	ret = gc570d_splitter_aux_power_up_common(gc, port);
	if (ret)
		return ret;

	/* The official driver sets the receiver-stable state before running the
	 * SCDT-ON power-up sequence.  That makes its conditional half
	 * routine active: clear all six saved transmitter IRQ bytes and issue
	 * its two direct aux01=0 writes before measuring/programming the clock.
	 */
	for (reg = 0x10; reg <= 0x15; reg++) {
		ret = gc570d_splitter_aux_write8(gc, port, reg, 0x00);
		if (ret)
			return ret;
	}
	ret = gc570d_splitter_aux_write8(gc, port, 0x01, 0x00);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_write8(gc, port, 0x01, 0x00);
	if (ret)
		return ret;

	ret = gc570d_splitter_measure_channel_clock(gc, port, &pclk, &vclk,
						     NULL);
	if (ret)
		return ret;

	ret = gc570d_splitter_aux_update8(gc, port, 0xaf, 0xc0, 0x00);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x84, 0x07,
						  vclk > 100000 ? 0x04 : 0x03);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x88, 0x04,
						  vclk > 162000 ? 0x04 : 0x00);
	if (ret)
		return ret;
	if (port == 2) {
		reg87 = vclk < 150001 ? 0x05 : 0x0d;
		reg8b = vclk < 150001 ? 0x03 : 0x09;
	} else if (vclk < 150001) {
		reg87 = 0x03;
		reg8b = 0x03;
	} else if (vclk < 310001) {
		reg87 = 0x09;
		reg8b = 0x09;
	} else if (vclk < 375001) {
		reg87 = 0x0d;
		reg8b = 0x0b;
	} else {
		reg87 = 0x0e;
		reg8b = 0x0d;
	}
	reg89 = vclk < 150001 ? 0x80 : (vclk < 310001 ? 0x21 : 0x25);
	ret = gc570d_splitter_aux_update8(gc, port, 0x87, 0x1f, reg87);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x89, 0xbf, reg89);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x8a, 0x0f, 0x00);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x8b, 0x0f, reg8b);
	if (ret)
		return ret;

	msleep(100);
	ret = gc570d_splitter_aux_analog_setup(gc, port);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x18, 0x80, 0x80);
	if (ret)
		return ret;

	*pclk_out = pclk;
	*vclk_out = vclk;
	return 0;
}

/*
 * Synchronous form of the low-rate TMDS branch of the official driver
 * state machine.  This is a clock-band distinction, not an HDMI
 * protocol-version claim: 2160p60 YCbCr 4:2:0 can use this band before the
 * later HDMI-2.0/SCDC output setup. The official driver starts equalization
 * when its receiver state reaches 2 with reg13.bit4 set and reg14.bit6 clear,
 * then consumes IRQ07.bit6/bit7 and finalizes the three lane coefficients.
 * Keeping the bounded wait in the driver avoids the old
 * userspace dispatcher, whose repeated debugfs writes disturbed the W1C IRQ
 * snapshots and blanked HDMI OUT.
 */
static int gc570d_splitter_tmds_low_rate_equalize(struct gc570d_dev *gc)
{
	u8 detail13 = 0;
	u8 detail14 = 0;
	u8 irq07 = 0;
	u8 lane_raw[3] = { 0 };
	u8 lane_ok[3] = { 0 };
	u8 lane_coeff[3] = { 0x9f, 0x9f, 0x9f };
	u8 lane_status[3] = { 0 };
	u8 fallback = 0;
	u8 attempt;
	u8 lane;
	unsigned int poll;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "read-receiver-state";
	bool success;
	int cleanup_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x14,
			       &detail14, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
			       &detail13, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(detail13 & 0x10))
		return 0;
	if (detail14 & 0x40)
		return -EOPNOTSUPP;

	for (;;) {
		attempt = ++gc->splitter_eq_retry;
		gc->splitter_eq_state = 0;

		/* Start low-rate TMDS equalization as the official driver does. */
		stage = "clear-eq-irqs";
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x07, 0xff);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x23, 0xb0);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x23, 0xa0);
		if (ret)
			goto out;

		stage = "program-low-rate-tmds-eq";
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x2c, 0x00);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x2d, 0x00);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x20, 0x36);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x21, 0x0e);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x26, 0x00);
		if (ret)
			goto out;
		for (lane = 0; lane < 3; lane++) {
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
						0x70, 0x27 + lane, 0x1f);
			if (ret)
				goto out;
		}
		ret = gc570d_splitter_update8(gc, 0x70, 0x22, 0x38, 0x38);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x22, 0x04, 0x04);
		if (ret)
			goto out;
		msleep(1);
		ret = gc570d_splitter_update8(gc, 0x70, 0x22, 0x04, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
		if (ret)
			goto out;

		/* The official driver's 20-ms pump changes state on EQ done or EQ fail. */
		stage = "wait-low-rate-tmds-eq-irq";
		irq07 = 0;
		for (poll = 0; poll < 50; poll++) {
			msleep(20);
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
					       0x70, 0x07, &irq07,
					       &last_irq, &last_status);
			if (ret)
				goto out;
			if (irq07 & 0xc0)
				break;
		}
		if (!(irq07 & 0xc0)) {
			ret = -ETIMEDOUT;
			goto out;
		}
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x07, irq07);
		if (ret)
			goto out;
		gc->splitter_eq_state = 3;

		/* Sample all lanes and choose the fallback as the official driver does. */
		stage = "read-low-rate-tmds-eq-result";
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
		if (ret)
			goto out;
		for (lane = 0; lane < 3; lane++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
					       0x70, 0xd5 + lane,
					       &lane_raw[lane],
					       &last_irq, &last_status);
			if (ret)
				goto out;
			lane_raw[lane] &= 0x7f;
		}
		for (lane = 0; lane < 3; lane++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
					       0x70, 0xd0,
					       &lane_status[lane],
					       &last_irq, &last_status);
			if (ret)
				goto out;
			lane_ok[lane] =
				((lane_status[lane] >> (lane * 2)) & 0x03) == 0x03;
		}
		success = lane_ok[0] || lane_ok[1] || lane_ok[2];
		if (success) {
			for (lane = 0; lane < 3; lane++) {
				if (lane_ok[lane]) {
					fallback = lane_raw[lane];
					break;
				}
			}
			for (lane = 0; lane < 3; lane++)
				lane_coeff[lane] =
					(lane_ok[lane] ? lane_raw[lane] : fallback) -
					0x80;
		} else {
			memset(lane_coeff, 0x9f, sizeof(lane_coeff));
		}

		/* Low-rate TMDS state-3 finalizer from the official driver. */
		stage = "apply-low-rate-tmds-eq-result";
		ret = gc570d_splitter_update8(gc, 0x70, 0x22, 0x38, 0x00);
		if (ret)
			goto out;
		for (lane = 0; lane < 3; lane++) {
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
						0x70, 0x27 + lane,
						lane_coeff[lane]);
			if (ret)
				goto out;
		}
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0xe9, 0x80);
		if (ret)
			goto out;
		msleep(10);
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
		if (ret)
			goto out;

		if (success || attempt >= 3) {
			gc->splitter_eq_state = 4;
			gc->splitter_eq_retry = 0;
		} else {
			gc->splitter_eq_state = 2;
		}
		ret = gc570d_splitter_update8(gc, 0x70, 0x53, 0x20, 0x20);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x05, 0x20, 0x20);
		if (ret)
			goto out;

		dev_info(&gc->pdev->dev,
			 "VideoSplitter low-rate TMDS EQ attempt %u completed: irq07=%02x status=%02x/%02x/%02x raw=%02x/%02x/%02x coefficients=%02x/%02x/%02x success=%s state=%u\n",
			 attempt, irq07, lane_status[0], lane_status[1],
			 lane_status[2], lane_raw[0], lane_raw[1], lane_raw[2],
			 lane_coeff[0], lane_coeff[1], lane_coeff[2],
			 success ? "yes" : "no", gc->splitter_eq_state);

		if (gc->splitter_eq_state == 4)
			return 0;
		msleep(20);
	}

out:
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (!ret)
		ret = cleanup_ret;
	dev_err(&gc->pdev->dev,
		"VideoSplitter low-rate TMDS EQ failed at %s: %d state=%u retry=%u receiver=%02x/%02x irq07=%02x\n",
		stage, ret, gc->splitter_eq_state, gc->splitter_eq_retry,
		detail13, detail14, irq07);
	return ret;
}

int gc570d_splitter_source_power_event(struct gc570d_dev *gc,
					      bool quiet_idle)
{
	static const u8 irq_regs[] = {
		0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12,
		0x13, 0x14, 0x15, 0x19, 0x1a, 0x1b, 0x1d,
	};
	u8 irq_values[ARRAY_SIZE(irq_regs)] = { 0 };
	u8 aux03[4] = { 0 };
	u8 final_main05 = 0;
	u8 final_detail05 = 0;
	u8 final_detail10 = 0;
	u8 final_detail13 = 0;
	u8 final_detail19 = 0;
	u8 final_bank = 0;
	u8 scdt_ports = 0;
	u8 port;
	u32 pclk[4] = { 0 };
	u32 vclk[4] = { 0 };
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "read-irq-snapshot";
	size_t i;
	int cleanup_ret;
	int ret;

	for (i = 0; i < ARRAY_SIZE(irq_regs); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       irq_regs[i], &irq_values[i],
				       &last_irq, &last_status);
		if (ret)
			goto out;
	}
	/* The official driver also accepts RX HDMI-mode (bit 4), ECC (bit 5), and
	 * deskew (bit 6). At this initial dispatch the official driver acknowledges ECC
	 * with the common W1C snapshot; its optional recovery branch depends
	 * on output-state caches which are still clear. Bit 6 and IRQ07 bits
	 * 0..2 only update the official driver's diagnostic counters. IRQ08 and IRQ09 have
	 * no later hardware branch in the official driver either: the official driver reads and
	 * writes them back solely as part of the common W1C acknowledgement.
	 * IRQ11 bit 3 is Color-Depth Detect and bit 2 is Vendor-Specific
	 * InfoFrame Detect.  the official driver accepts their observed combined 0x0c
	 * snapshot: bit 3 refreshes its depth cache and bit 2 parses the VSIF.
	 * The other packet-notification bits are likewise acknowledged here; the
	 * payload only updates the official driver software caches at this stage.
	 * IRQ10 is dispatched bitwise by the official driver: bit 2 is Video-Mode-Changed,
	 * bit 1 is SCDT, and bit 0 has no hardware branch.  In particular the
	 * live 0x03 snapshot must not be rejected merely because it also carries
	 * that no-op bit 0.
	 */
	/* If the source was already connected before IRQ latches were cleared,
	 * synthesize the initial power/clock/SCDT flags from live status 0x13/0x19.
	 */
	if (!(irq_values[0] & 0x7f) && !(irq_values[5] & 0x06) &&
	    !gc->splitter_pclk[1] && (irq_values[8] & 0x01)) {
		irq_values[0] |= 0x01;
		if (irq_values[8] & 0x08)
			irq_values[0] |= 0x04;
		if ((irq_values[8] & 0x10) && (irq_values[11] & 0x80))
			irq_values[5] |= 0x02;
	}

	if ((!(irq_values[0] & 0x7f) &&
	     !((irq_values[5] & 0x06) && (irq_values[8] & 0x10))) ||
	    (irq_values[0] & 0x80) ||
	    (irq_values[2] & ~0x07) ||
	    !(irq_values[8] & 0x01) ||
	    ((irq_values[5] & 0x02) && !(irq_values[11] & 0x80))) {
		ret = -ENODATA;
		goto out;
	}

	/* Start a new official-driver dispatch snapshot only after a real receiver IRQ
	 * has been validated.  The official driver's 20-ms pump samples an idle receiver
	 * on every pass; an idle sample must not erase the live per-port state
	 * retained by the preceding Video-Stable worker.  Packet-only IRQs also
	 * retain that state: only source power, SCDT, or video-mode transitions
	 * invalidate the per-port stable/clock cache.
	 */
	gc->splitter_main_timer_serviced = false;
	if ((irq_values[0] & BIT(0)) || (irq_values[5] & 0x06)) {
		gc->splitter_video_stable_pending = false;
		gc->splitter_channel_active_mask = 0;
		gc->splitter_video_stable_mask = 0;
		gc->splitter_worker_state4_mask = 0;
		memset(gc->splitter_pclk, 0, sizeof(gc->splitter_pclk));
		memset(gc->splitter_vclk, 0, sizeof(gc->splitter_vclk));
	}
	if (irq_values[0] & BIT(0)) {
		gc->splitter_eq_state = 0;
		gc->splitter_eq_retry = 0;
	}

	/* The official driver acknowledges detailed IRQ registers 05 through 11. */
	stage = "ack-detail-irq";
	for (i = 0; i < 7; i++) {
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					irq_regs[i], irq_values[i]);
		if (ret)
			goto out;
	}
	/* The official driver acknowledges IRQ12 after consuming its packet notifications.
	 * They only update software InfoFrame/HDR caches at this stage.
	 */
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
				0x12, irq_values[7]);
	if (ret)
		goto out;

	if (irq_values[0] & 0x01) {
		stage = "source-power-toggle";
		ret = gc570d_splitter_update8(gc, 0x58, 0x0c, 0x04, 0x04);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x10, 0x40, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x10, 0x40, 0x40);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x0c, 0x04, 0x00);
		if (ret)
			goto out;

		/* Current HDMI path of the official driver; the freshly cleared EDID
		 * flags intentionally keep its conditional HPD helper inactive.
		 */
		stage = "select-bank-0";
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
		if (ret)
			goto out;
		if ((irq_values[8] & 0x41) != 0x01) {
			ret = -EPROTO;
			goto out;
		}
		stage = "configure-hdmi-power-path";
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x3a, 0x06, 0x02);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x29, 0x01, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x26, 0x0c, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x69, 0x2f, 0x00);
		if (ret)
			goto out;
	}

	/* With HDCP disabled the first receiver IRQ can combine power, clock-on,
	 * clock-stable, and the reserved bit 3. Reproduce the stable-clock branch
	 * of the official driver after its common W1C snapshot acknowledgement.
	 */
	if (irq_values[0] & 0x04) {
		stage = "clock-stable-reset";
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x05, 0x04);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x53, 0xe0, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x54, 0xff, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x55, 0x07, 0x00);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x05, 0xe8);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
					0x06, 0xfe);
		if (ret)
			goto out;
		if (!(irq_values[8] & 0x08)) {
			stage = "clock-unstable-receiver";
			ret = gc570d_splitter_update8(gc, 0x70, 0x54,
						      0x01, 0x00);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x23,
						      0x10, 0x10);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x23,
						      0x10, 0x00);
			if (ret)
				goto out;
			for (port = 0; port < 4; port++) {
				ret = gc570d_splitter_aux_update8(gc, port, 0x88,
							  0x03, 0x03);
				if (ret)
					goto out;
			}
			ret = gc570d_splitter_update8(gc, 0x70, 0x23,
						      0x02, 0x02);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x23,
						      0x02, 0x00);
			if (ret)
				goto out;
			goto handle_symbol_irq;
		}

		stage = "clock-stable-receiver";
		ret = gc570d_splitter_update8(gc, 0x70, 0x23, 0x02, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x54, 0x01, 0x01);
		if (ret)
			goto out;

		stage = "clock-stable-recovery";
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
		if (ret)
			goto out;
		for (i = 0x27; i <= 0x29; i++) {
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70,
						i, 0x87);
			if (ret)
				goto out;
		}
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
		if (ret)
			goto out;

		stage = "clock-stable-timer";
		ret = gc570d_splitter_update8(gc, 0x58, 0x1a, 0x02, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x19, 0x20, 0x00);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0x1d, 0x85);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x19, 0x20, 0x20);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0x07, 0x2f);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x1a, 0x02, 0x02);
		if (ret)
			goto out;
	}

handle_symbol_irq:
	/* IRQ06 bit 0 follows every IRQ05 branch in the official driver. The retained
	 * 05=47/06=f1/07=07 snapshot is an unstable-clock and symbol-lock
	 * transition plus diagnostic-only deskew reports.
	 */
	if (irq_values[1] & 0x01) {
		stage = "symbol-lock-event";
		if (irq_values[8] & 0x80) {
			ret = gc570d_splitter_update8(gc, 0x70, 0x53, 0xe0, 0xe0);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x55, 0x07, 0x07);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x57, 0x0f, 0x0f);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x5d, 0x06, 0x06);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x5e, 0x08, 0x08);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x5f, 0x01, 0x01);
			if (ret)
				goto out;
			/* The official driver moves an idle low-rate TMDS equalizer to state 2 on
			 * symbol lock.  A completed state remains completed until a
			 * real source-power transition resets it above.
			 */
			if (!gc->splitter_eq_state)
				gc->splitter_eq_state = 2;
		} else {
			ret = gc570d_splitter_update8(gc, 0x70, 0x53, 0xe0, 0x00);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x55, 0x07, 0x00);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x5d, 0x06, 0x00);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x5e, 0x08, 0x00);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x5f, 0x01, 0x00);
			if (ret)
				goto out;
		}
	}

	/* IRQ10 bit 2 is the official driver Video-Mode-Changed branch.  Preserve the
	 * additional identical update at the start of the SCDT branch below when
	 * a snapshot contains both bits (the observed 0x06 case).
	 */
	if (irq_values[5] & 0x04) {
		stage = "video-mode-change";
		ret = gc570d_splitter_update8(gc, 0x70, 0x40, 0x03, 0x00);
		if (ret)
			goto out;
	}

	/* IRQ10 bit 1 is SCDT.  When detail19 bit 7 is set, this is the first
	 * official-driver branch that resets the receiver video FIFO and derives
	 * the active downstream transmitter clock from live measurements.
	 */
	if (irq_values[5] & 0x02) {
		stage = "scdt-on-receiver-reset";
		ret = gc570d_splitter_update8(gc, 0x70, 0x40, 0x03, 0x00);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0x0b, 0xff);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0x0b, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x4e, 0x0f, 0x0f);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x0c, 0x08, 0x08);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x0c, 0x08, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x4e, 0x0f, 0x00);
		if (ret)
			goto out;

		for (port = 1; port < 3; port++) {
			stage = "scdt-read-channel-presence";
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
					       0x68 + 2 * port, 0x03,
					       &aux03[port], &last_irq,
					       &last_status);
			if (ret)
				goto out;
			if (!(aux03[port] & 0x01))
				continue;
			scdt_ports |= BIT(port);
			stage = "scdt-channel-setup";
			ret = gc570d_splitter_scdt_channel_setup(gc, port,
							       &pclk[port],
							       &vclk[port]);
			if (ret)
				goto out;
		}
		if (!scdt_ports) {
			ret = -ENOLINK;
			goto out;
		}

		stage = "scdt-final-routing";
		ret = gc570d_splitter_update8(gc, 0x58, 0x67, 0x0f, 0x00);
		if (ret)
			goto out;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0x68, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0xa7, 0x40,
						  (irq_values[9] & 0x01) << 6);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
		if (ret)
			goto out;
	}

	/* The official driver invokes the official driver immediately after the receiver
	 * IRQ dispatcher and before the auxiliary output worker.  The observed
	 * PS5 state bd/38 selects its low-rate TMDS branch.
	 */
	if (gc->splitter_eq_state == 2 && (irq_values[8] & 0x10) &&
	    !(irq_values[9] & 0x40)) {
		stage = "low-rate-tmds-equalization";
		ret = gc570d_splitter_tmds_low_rate_equalize(gc);
		if (ret)
			goto out;
	}

	stage = "sample-internal-links";
	for (port = 0; port < 4; port++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
				       0x68 + 2 * port, 0x03, &aux03[port],
				       &last_irq, &last_status);
		if (ret)
			goto out;
	}

	stage = "verify-event-result";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &final_main05, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x05,
			       &final_detail05, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x10,
			       &final_detail10, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
			       &final_detail13, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x19,
			       &final_detail19, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x0f,
			       &final_bank, &last_irq, &last_status);
	if (ret)
		goto out;
	if ((final_detail05 & 0x01) || !(final_detail13 & 0x01) ||
	    (final_bank & 0x03)) {
		ret = -EILSEQ;
		goto out;
	}
	if (irq_values[0] & BIT(0)) {
		for (port = 0; port < 4; port++) {
			gc->splitter_pclk[port] = 0;
			gc->splitter_vclk[port] = 0;
		}
	}
	if (scdt_ports)
		gc->splitter_channel_active_mask |= scdt_ports;
	for (port = 0; port < 4; port++) {
		if (scdt_ports & BIT(port)) {
			gc->splitter_pclk[port] = pclk[port];
			gc->splitter_vclk[port] = vclk[port];
		}
	}

	dev_info(&gc->pdev->dev,
		 "VideoSplitter source/clock event completed: irq05=%02x irq06=%02x irq07=%02x irq10=%02x irq13=%02x irq19=%02x final main05=%02x detail05=%02x detail10=%02x detail13=%02x detail19=%02x bank=%02x aux03=%02x/%02x/%02x/%02x scdt_ports=%02x pclk1=%u vclk1=%u pclk2=%u vclk2=%u hpd_deferred=yes\n",
		 irq_values[0], irq_values[1], irq_values[2], irq_values[5],
		 irq_values[8], irq_values[11],
		 final_main05, final_detail05, final_detail10, final_detail13,
		 final_detail19, final_bank, aux03[0], aux03[1], aux03[2],
		 aux03[3], scdt_ports, pclk[1], vclk[1], pclk[2], vclk[2]);
	return 0;

out:
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (!ret)
		ret = cleanup_ret;
	if (!quiet_idle || ret != -ENODATA)
		dev_err(&gc->pdev->dev,
			"VideoSplitter source-5-V event failed at %s: %d irq05=%02x irq06=%02x irq07=%02x irq10=%02x irq13=%02x irq19=%02x final main05=%02x detail05=%02x detail13=%02x bank=%02x\n",
			stage, ret, irq_values[0], irq_values[1], irq_values[2],
			irq_values[5], irq_values[8], irq_values[11],
			final_main05, final_detail05, final_detail13,
			final_bank);
	return ret;
}

static ssize_t gc570d_splitter_source_power_event_write(struct file *file,
						  const char __user *buffer,
						  size_t count,
						  loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_source_power_event(gc, false);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_source_power_event_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_source_power_event_write,
	.llseek = noop_llseek,
};

static int gc570d_splitter_hpd_high(struct gc570d_dev *gc)
{
	u8 source13 = 0;
	u8 aux1_03 = 0;
	u8 aux2_03 = 0;
	u8 initial_ab = 0;
	u8 final_ab = 0;
	u8 final26 = 0;
	u8 final55 = 0;
	u8 final_bank = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "validate-source";
	int cleanup_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
			       &source13, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(source13 & 0x01)) {
		ret = -ENOLINK;
		goto out;
	}

	stage = "validate-downstream";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a, 0x03,
			       &aux1_03, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6c, 0x03,
			       &aux2_03, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(aux1_03 & 0x01) && !(aux2_03 & 0x01)) {
		ret = -ENOLINK;
		goto out;
	}

	/* Exact HDMI branch of the official driver. */
	stage = "select-bank-3";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0xab,
			       &initial_ab, &last_irq, &last_status);
	if (ret)
		goto out;
	if (initial_ab != 0xca) {
		stage = "raise-hpd";
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
					0x70, 0xab, 0xca);
		if (ret)
			goto out;
	}
	stage = "select-bank-0";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		goto out;
	stage = "enable-hdmi-hpd-path";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x26, 0x00);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x55, 0xff);
	if (ret)
		goto out;

	msleep(100);
	stage = "verify";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0xab,
			       &final_ab, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x26,
			       &final26, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x55,
			       &final55, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x0f,
			       &final_bank, &last_irq, &last_status);
	if (ret)
		goto out;
	if (final_ab != 0xca || final26 != 0x00 || final55 != 0xff ||
	    (final_bank & 0x03)) {
		ret = -EILSEQ;
		goto out;
	}

	dev_info(&gc->pdev->dev,
		 "VideoSplitter HPD high completed: source13=%02x downstream03=%02x/%02x bank3_ab=%02x->%02x bank0_26=%02x 55=%02x\n",
		 source13, aux1_03, aux2_03, initial_ab, final_ab,
		 final26, final55);
	return 0;

out:
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (!ret)
		ret = cleanup_ret;
	dev_err(&gc->pdev->dev,
		"VideoSplitter HPD high failed at %s: %d source13=%02x downstream03=%02x/%02x bank3_ab=%02x->%02x bank0_26=%02x 55=%02x\n",
		stage, ret, source13, aux1_03, aux2_03, initial_ab, final_ab,
		final26, final55);
	return ret;
}

static ssize_t gc570d_splitter_hpd_high_write(struct file *file,
					       const char __user *buffer,
					       size_t count,
					       loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_hpd_high(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_hpd_high_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_hpd_high_write,
	.llseek = noop_llseek,
};

static int gc570d_splitter_ddc_recover(struct gc570d_dev *gc, u8 port)
{
	u8 status = 0;
	u8 pass;
	u32 last_irq = 0;
	u32 last_status = 0;
	int ret;

	ret = gc570d_splitter_aux_update8(gc, port, 0x35, BIT(4), BIT(4));
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x35, BIT(4), 0x00);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x28, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x28, BIT(0), 0x00);
	if (ret)
		return ret;

	/* The official driver issues this abort/reset command twice and waits for
	 * either completion or a classified DDC error after each pass.
	 */
	for (pass = 0; pass < 2; pass++) {
		ret = gc570d_splitter_aux_write8(gc, port, 0x2e, 0x0f);
		if (ret && ret != -EIO)
			return ret;
		ret = read_poll_timeout(gc570d_i2c_read8, ret,
					!ret && (status & 0xb8), 1000, 200000,
					false, gc, &gc570d_splitter_bus,
					0x68 + 2 * port, 0x2f, &status,
					&last_irq, &last_status);
		if (ret)
			return ret;
	}
	return 0;
}

static int gc570d_splitter_read_edid_chunk(struct gc570d_dev *gc, u8 port,
					    u8 block, u8 offset, u8 *values,
					    u8 *ddc_status, u32 *last_irq,
					    u32 *last_status,
					    const char **stage)
{
	const u8 slave = 0x68 + 2 * port;
	int cleanup_ret;
	int trigger_ret;
	int ret;

	*stage = "enable-ddc";
	ret = gc570d_splitter_aux_update8(gc, port, 0x28, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x19, BIT(2), BIT(2));
	if (ret)
		goto out_cleanup;
	ret = gc570d_splitter_aux_update8(gc, port, 0x1d, BIT(3), BIT(3));
	if (ret)
		goto out_cleanup;

	/* The official driver configures one 32-byte EDID read from DDC address
	 * 0x50 (0xa0 in the transmitter's 8-bit-address convention).
	 */
	*stage = "program-ddc";
	ret = gc570d_splitter_aux_write8(gc, port, 0x2e, 0x09);
	if (ret)
		goto out_cleanup;
	ret = gc570d_splitter_aux_write8(gc, port, 0x29, 0xa0);
	if (ret)
		goto out_cleanup;
	ret = gc570d_splitter_aux_write8(gc, port, 0x2a,
					(block << 7) + offset);
	if (ret)
		goto out_cleanup;
	ret = gc570d_splitter_aux_write8(gc, port, 0x2b, 32);
	if (ret)
		goto out_cleanup;
	ret = gc570d_splitter_aux_write8(gc, port, 0x2c, 0x00);
	if (ret)
		goto out_cleanup;
	ret = gc570d_splitter_aux_write8(gc, port, 0x2d, block >> 1);
	if (ret)
		goto out_cleanup;

	*stage = "start-ddc";
	trigger_ret = gc570d_splitter_aux_write8(gc, port, 0x2e, 0x03);
	if (trigger_ret && trigger_ret != -EIO) {
		ret = trigger_ret;
		goto out_cleanup;
	}
	/* The trigger write can report outer-I2C EIO after the inner DDC
	 * engine has already accepted it. The official driver ignores that write result and
	 * judges the operation from reg2f after a 15-ms settling interval.
	 */
	msleep(15);
	ret = read_poll_timeout(gc570d_i2c_read8, ret,
				!ret && (*ddc_status & 0xb8), 1000, 200000,
				false, gc, &gc570d_splitter_bus, slave, 0x2f,
				ddc_status, last_irq, last_status);
	if (ret)
		goto out_cleanup;
	if (!(*ddc_status & BIT(7))) {
		ret = -EIO;
		goto out_cleanup;
	}

	*stage = "read-ddc-fifo";
	ret = gc570d_i2c_read_buf(gc, &gc570d_splitter_bus, slave, 0x30,
				  values, 32, last_irq, last_status);

out_cleanup:
	cleanup_ret = gc570d_splitter_aux_update8(gc, port, 0x28,
						  BIT(0), 0x00);
	if (!ret && cleanup_ret) {
		*stage = "disable-ddc";
		ret = cleanup_ret;
	}
	return ret;
}

static int gc570d_splitter_read_downstream_edid(struct gc570d_dev *gc,
						u8 port, u8 block, u8 *edid)
{
	const u8 slave = 0x68 + 2 * port;
	u8 ddc_status = 0;
	u8 presence = 0;
	u8 attempt = 0;
	u8 offset = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "validate-port";
	int ret;

	if (port < 1 || port > 2)
		return -EINVAL;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave, 0x03,
			       &presence, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(presence & BIT(0))) {
		ret = -ENOLINK;
		goto out;
	}

	/* The official driver retries the whole 128-byte block three times. */
	for (attempt = 0; attempt < 3; attempt++) {
		for (offset = 0; offset < 128; offset += 32) {
			ret = gc570d_splitter_read_edid_chunk(gc, port, block,
							       offset,
							       edid + offset,
							       &ddc_status,
							       &last_irq,
							       &last_status,
							       &stage);
			if (ret)
				break;
		}
		if (!ret)
			return 0;

		stage = "recover-ddc";
		ret = gc570d_splitter_ddc_recover(gc, port);
		if (ret)
			break;
		if (attempt == 2) {
			stage = "ddc-retries-exhausted";
			ret = -EIO;
			break;
		}
	}

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter downstream EDID read failed at %s: %d port=%u block=%u attempt=%u offset=%u presence=%02x ddc_status=%02x irq=0x%08x status=0x%08x\n",
		stage, ret, port, block, attempt + 1, offset, presence, ddc_status,
		last_irq, last_status);
	return ret;
}

static int gc570d_splitter_edid_read(struct gc570d_dev *gc)
{
	static const u8 header[] = { 0x00, 0xff, 0xff, 0xff,
				     0xff, 0xff, 0xff, 0x00 };
	u8 (*edid)[128];
	u8 block_count;
	u8 block;
	u8 checksum = 0;
	u8 presence[3] = { 0 };
	u8 presence_mask = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	u8 port;
	size_t i;
	int ret;

	/*
	 * The official driver builds the downstream-presence mask before Copy_Mode,
	 * and the official driver selects its highest set port when the GC570D feature
	 * bit is active.  Do not assume that the one physical passthrough sink
	 * always appears on auxiliary instance 1: with HDMI IN 1 selected the
	 * live hardware exposes it on instance 2 (wire address 0x6c).
	 */
	for (port = 1; port <= 2; port++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
				       0x68 + 2 * port, 0x03, &presence[port],
				       &last_irq, &last_status);
		if (ret)
			return ret;
		if (presence[port] & BIT(0))
			presence_mask |= BIT(port);
	}
	if (!presence_mask) {
		dev_err(&gc->pdev->dev,
			"VideoSplitter downstream EDID read found no present port: aux03=%02x/%02x irq=0x%08x status=0x%08x\n",
			presence[1], presence[2], last_irq, last_status);
		return -ENOLINK;
	}
	port = presence_mask & BIT(2) ? 2 : 1;

	edid = kmalloc_array(4, 128, GFP_KERNEL);
	if (!edid)
		return -ENOMEM;

	ret = gc570d_splitter_read_downstream_edid(gc, port, 0, edid[0]);
	if (ret)
		goto out_free;
	for (i = 0; i < sizeof(edid[0]); i++)
		checksum += edid[0][i];
	if (memcmp(edid[0], header, sizeof(header)) || checksum) {
		dev_err(&gc->pdev->dev,
			"VideoSplitter downstream EDID invalid: header=%*ph checksum=%02x extensions=%u\n",
			(int)sizeof(header), edid[0], checksum, edid[0][0x7e]);
		ret = -EILSEQ;
		goto out_free;
	}

	/* The official driver reads all three extension blocks when byte 0x7e is
	 * three.  Bound this diagnostic to the same four 128-byte blocks so a
	 * malformed sink cannot grow the transaction indefinitely.
	 */
	block_count = min_t(u8, edid[0][0x7e] + 1, 4);
	for (block = 1; block < block_count; block++) {
		checksum = 0;
		ret = gc570d_splitter_read_downstream_edid(gc, port, block,
							      edid[block]);
		if (ret)
			goto out_free;
		for (i = 0; i < sizeof(edid[block]); i++)
			checksum += edid[block][i];
		if (checksum) {
			dev_err(&gc->pdev->dev,
				"VideoSplitter downstream EDID block invalid: port=%u block=%u tag=%02x checksum=%02x\n",
				port, block, edid[block][0], checksum);
			ret = -EILSEQ;
			goto out_free;
		}
	}

	dev_info(&gc->pdev->dev,
		 "VideoSplitter downstream EDID read: port=%u presence_mask=%02x aux03=%02x/%02x header=%*ph manufacturer=%02x%02x product=%02x%02x version=%u.%u extensions=%u blocks=%u checksum=%02x\n",
		 port, presence_mask, presence[1], presence[2],
		 (int)sizeof(header), edid[0], edid[0][8], edid[0][9],
		 edid[0][10], edid[0][11], edid[0][18], edid[0][19],
		 edid[0][0x7e], block_count, edid[0][0x7f]);
	for (block = 0; block < block_count; block++) {
		for (i = 0; i < sizeof(edid[block]); i += 16)
			dev_info(&gc->pdev->dev,
				 "VideoSplitter downstream EDID block=%u offset=%02zx data=%16ph\n",
				 block, i, edid[block] + i);
	}
	ret = 0;

out_free:
	kfree(edid);
	return ret;
}

static ssize_t gc570d_splitter_edid_read_write(struct file *file,
						const char __user *buffer,
						size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_edid_read(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_edid_read_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_edid_read_write,
	.llseek = noop_llseek,
};

/*
 * The official driver returns these two blocks for GC570D splitter port 1.  The
 * The official driver Copy_Mode path does not issue downstream DDC on that port: it
 * copies this image from the driver's .data section and publishes it through
 * the splitter's internal EDID slave (0xd8).
 */
static const u8 gc570d_splitter_windows_edid[256] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
	0x26, 0x85, 0x05, 0x68, 0x21, 0x43, 0x65, 0x87,
	0x2e, 0x19, 0x01, 0x03, 0x80, 0x3e, 0x22, 0x78,
	0xea, 0x08, 0xa5, 0xa2, 0x57, 0x4f, 0xa2, 0x28,
	0x0f, 0x50, 0x54, 0xa5, 0x4b, 0x00, 0xd1, 0xc0,
	0xa9, 0x40, 0x81, 0x80, 0x81, 0x00, 0x71, 0x4f,
	0xe1, 0x00, 0x01, 0x01, 0x01, 0x01, 0x08, 0xe8,
	0x00, 0x30, 0xf2, 0x70, 0x5a, 0x80, 0xb0, 0x58,
	0x8a, 0x00, 0x6d, 0x55, 0x21, 0x00, 0x00, 0x1e,
	0x00, 0x00, 0x00, 0xff, 0x00, 0x49, 0x54, 0x36,
	0x38, 0x30, 0x35, 0x45, 0x56, 0x42, 0x34, 0x30,
	0x39, 0x36, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x49,
	0x54, 0x45, 0x36, 0x38, 0x30, 0x35, 0x31, 0x20,
	0x44, 0x45, 0x4d, 0x4f, 0x00, 0x00, 0x00, 0xfd,
	0x00, 0x1d, 0x4b, 0x1f, 0x8c, 0x3c, 0x00, 0x0a,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0xc2,
	0x02, 0x03, 0x3c, 0xf1, 0x56, 0x65, 0x66, 0x61,
	0x60, 0x5f, 0x5e, 0x5d, 0x62, 0x63, 0x64, 0x10,
	0x1f, 0x20, 0x05, 0x14, 0x04, 0x13, 0x12, 0x11,
	0x03, 0x02, 0x01, 0x23, 0x09, 0x07, 0x07, 0x83,
	0x01, 0x00, 0x00, 0x6d, 0x03, 0x0c, 0x00, 0x10,
	0x00, 0x00, 0x3c, 0x20, 0x00, 0x60, 0x01, 0x02,
	0x03, 0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78, 0x80,
	0x03, 0xe2, 0x0f, 0x0f, 0x04, 0x74, 0x00, 0x30,
	0xf2, 0x70, 0x5a, 0x80, 0xb0, 0x58, 0x8a, 0x00,
	0x6d, 0x55, 0x21, 0x00, 0x00, 0x1e, 0x02, 0x3a,
	0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
	0x45, 0x00, 0xe0, 0x0e, 0x11, 0x00, 0x00, 0x1e,
	0x01, 0x1d, 0x00, 0x72, 0x51, 0xd0, 0x1e, 0x20,
	0x6e, 0x28, 0x55, 0x00, 0x40, 0xb4, 0x10, 0x00,
	0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56,
};

/*
 * The official driver loads customer_edidGC570D_4k.bin without rewriting its video
 * modes.  Keep the splitter-facing copy byte-for-byte identical too.  In
 * particular, the official driver advertises VICs 97/96 in the regular VDB and marks them
 * as 4:2:0-capable with the following Y420 capability map; replacing that pair
 * with a Y420-only VDB makes the profile syntactically valid but changes what
 * real sources such as the PS5 report for 2160p60 HDR.  Input-format and link
 * handling belongs in the driver implementation, not in an EDID restriction.
 */


static int
gc570d_splitter_windows_hpd(struct gc570d_dev *gc, bool high,
			    const char **stage)
{
	u8 ab = 0;
	u8 source13 = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	int ret;

	if (high) {
		*stage = "hpd-high-source";
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
				       &source13, &last_irq, &last_status);
		if (ret)
			return ret;
	}

	*stage = high ? "hpd-high-bank-3" : "hpd-low-bank-3";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
	if (ret)
		return ret;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0xab,
			       &ab, &last_irq, &last_status);
	if (ret)
		return ret;
	if (high) {
		if (ab != 0xca) {
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
						0x70, 0xab, 0xca);
			if (ret)
				return ret;
		}
	} else if (ab == 0xca) {
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
					0x70, 0xab, 0x4a);
		if (ret)
			return ret;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
					0x70, 0xab, 0x00);
		if (ret)
			return ret;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
					0x70, 0xac, 0x00);
		if (ret)
			return ret;
	}

	*stage = high ? "hpd-high-bank-0" : "hpd-low-bank-0";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		return ret;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x26,
				high ? 0x00 : 0xff);
	if (ret)
		return ret;
	return gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x55,
				 high ? 0xff : 0x00);
}

/*
 * The official driver is called by the official driver 20-ms worker on every pass.  With
 * the receiver-HDCP property at its zero-initialized default, the official driver selects
 * RXHDCP_OFF and applies these three register updates.  The first transition
 * from the zero-initialized software cache is followed by the exact low/high
 * HPD pulse used by the official driver.
 */
int gc570d_splitter_windows_receiver_hdcp_off(struct gc570d_dev *gc)
{
	u8 main0a = 0, main0c = 0, detail23 = 0, bank = 0;
	u32 last_irq = 0, last_status = 0;
	const char *stage = "validate-bank-0";
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x0f,
			       &bank, &last_irq, &last_status);
	if (ret)
		goto out;
	if (bank & 0x03) {
		ret = -EAGAIN;
		goto out;
	}

	stage = "disable-receiver-hdcp";
	ret = gc570d_splitter_update8(gc, 0x58, 0x0a, 0x04, 0x04);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x23, 0x42, 0x42);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x58, 0x0c, 0x04, 0x04);
	if (ret)
		goto out;

	stage = "receiver-hdcp-hpd-low";
	ret = gc570d_splitter_windows_hpd(gc, false, &stage);
	if (ret)
		goto out;
	msleep(100);
	stage = "receiver-hdcp-hpd-high";
	ret = gc570d_splitter_windows_hpd(gc, true, &stage);
	if (ret)
		goto out;

	stage = "verify-receiver-hdcp-off";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x0a,
			       &main0a, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x0c,
			       &main0c, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x23,
			       &detail23, &last_irq, &last_status);
	if (ret)
		goto out;
	if ((main0a & 0x04) != 0x04 || (main0c & 0x04) != 0x04 ||
	    (detail23 & 0x42) != 0x42) {
		ret = -EIO;
		goto out;
	}

	dev_info(&gc->pdev->dev,
		 "VideoSplitter official-driver receiver HDCP-off state completed: main0a=%02x main0c=%02x detail23=%02x hpd_pulse=low/100ms/high\n",
		 main0a, main0c, detail23);
	WRITE_ONCE(gc->splitter_receiver_hdcp_off, true);
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter official-driver receiver HDCP-off state failed at %s: %d bank=%02x main0a=%02x main0c=%02x detail23=%02x irq=0x%08x status=0x%08x\n",
		stage, ret, bank, main0a, main0c, detail23,
		last_irq, last_status);
	return ret;
}

/* The official driver reapplies the selected RXHDCP_OFF bits on every the official driver
 * state-4 pass.  Its low/high HPD pulse belongs only to the cached property
 * transition above; repeating that pulse would blank a working monitor every
 * 20 ms.
 */
static int
gc570d_splitter_windows_receiver_hdcp_off_maintain(struct gc570d_dev *gc)
{
	int ret;

	if (!READ_ONCE(gc->splitter_receiver_hdcp_off))
		return -EAGAIN;

	ret = gc570d_splitter_update8(gc, 0x58, 0x0a, 0x04, 0x04);
	if (ret)
		return ret;
	ret = gc570d_splitter_update8(gc, 0x70, 0x23, 0x42, 0x42);
	if (ret)
		return ret;
	return gc570d_splitter_update8(gc, 0x58, 0x0c, 0x04, 0x04);
}

static ssize_t
gc570d_splitter_windows_receiver_hdcp_off_write(struct file *file,
						 const char __user *buffer,
						 size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_windows_receiver_hdcp_off(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations
gc570d_splitter_windows_receiver_hdcp_off_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_windows_receiver_hdcp_off_write,
	.llseek = noop_llseek,
};

static void gc570d_splitter_windows_filter_cta(u8 *edid)
{
	size_t i;

	/* The official driver clears the HDMI VSDB 3D-present bit and the HDMI
	 * Forum VSDB deep-color-420 bit before Copy_Mode publishes block 1.
	 */
	for (i = 0; i + 5 < 127; i++) {
		if (edid[i] == 0x03 && edid[i + 1] == 0x0c &&
		    edid[i + 2] == 0x00)
			edid[i + 5] &= ~BIT(6);
	}
	for (i = 0; i + 6 < 127; i++) {
		if (edid[i] == 0xd8 && edid[i + 1] == 0x5d &&
		    edid[i + 2] == 0xc4)
			edid[i + 6] &= ~BIT(2);
	}
}

static int gc570d_splitter_cta_physical_address(const u8 *cta,
						 u8 *high, u8 *low)
{
	u8 end;
	u8 offset;

	if (cta[0] != 0x02 || cta[1] != 0x03)
		return -EILSEQ;
	end = cta[2] ? cta[2] : 127;
	if (end < 4 || end > 127)
		return -EILSEQ;

	for (offset = 4; offset < end; ) {
		u8 length = cta[offset] & 0x1f;

		if (offset + length >= end)
			return -EILSEQ;
		if ((cta[offset] >> 5) == 3 && length >= 5 &&
		    cta[offset + 1] == 0x03 &&
		    cta[offset + 2] == 0x0c &&
		    cta[offset + 3] == 0x00) {
			*high = cta[offset + 4];
			*low = cta[offset + 5];
			return 0;
		}
		offset += length + 1;
	}
	return -ENODATA;
}

static int gc570d_splitter_windows_edid_publish(struct gc570d_dev *gc,
						 bool bridge_default)
{
	u8 *live_buf = NULL;
	const u8 *publish_edid = gc570d_splitter_windows_edid;
	u8 checksum[2] = { 0 };
	u8 initial_bank = 0;
	u8 presence[3] = { 0 };
	u8 presence_mask = 0;
	u8 cache_c7 = 0;
	u8 cache_c8 = 0;
	u8 verify_c5 = 0;
	u8 verify34 = 0;
	u8 verify_main0f = 0;
	u8 verify_main10 = 0;
	u8 block;
	u8 port;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "select-port";
	size_t i;
	int cleanup_ret;
	int ret;

	for (port = 1; port <= 2; port++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
				       0x68 + 2 * port, 0x03, &presence[port],
				       &last_irq, &last_status);
		if (ret)
			goto out;
		if (presence[port] & BIT(0))
			presence_mask |= BIT(port);
	}
	if (!presence_mask) {
		ret = -ENOLINK;
		goto out;
	}
	port = presence_mask & BIT(2) ? 2 : 1;
	if (bridge_default) {
		publish_edid = gc570d_it68051_edid;
		/* The official driver caches the HDMI VSDB physical address and
		 * The official driver publishes those bytes through detail c7/c8 before
		 * loading the EDID RAM.  The bridge-limited profile preserves the
		 * The official driver HDMI VSDB, so preserve that parser-to-publication path too.
		 */
		stage = "parse-bridge-cta";
		ret = gc570d_splitter_cta_physical_address(publish_edid + 128,
						      &cache_c7,
						      &cache_c8);
		if (ret)
			goto out;
	}

	if (port == 2 && !bridge_default) {
		u8 block_count;
		u8 *cta_blk = NULL;

		live_buf = kzalloc(512, GFP_KERNEL);
		if (!live_buf) {
			ret = -ENOMEM;
			goto out;
		}
		stage = "read-live-edid-base";
		ret = gc570d_splitter_read_downstream_edid(gc, port, 0,
							   live_buf);
		if (ret)
			goto out;
		checksum[0] = 0;
		for (i = 0; i < 128; i++)
			checksum[0] += live_buf[i];
		if (memcmp(live_buf,
			   "\x00\xff\xff\xff\xff\xff\xff\x00", 8) ||
		    checksum[0] || !live_buf[0x7e]) {
			ret = -EILSEQ;
			goto out;
		}

		block_count = min_t(u8, live_buf[0x7e] + 1, 4);
		stage = "read-live-edid-extensions";
		for (block = 1; block < block_count; block++) {
			u8 *blk = live_buf + block * 128;

			ret = gc570d_splitter_read_downstream_edid(gc, port,
							       block, blk);
			if (ret)
				goto out;
			checksum[1] = 0;
			for (i = 0; i < 128; i++)
				checksum[1] += blk[i];
			if (checksum[1]) {
				ret = -EILSEQ;
				goto out;
			}
			if (!cta_blk && blk[0] == 0x02)
				cta_blk = blk;
		}
		if (!cta_blk) {
			ret = -EILSEQ;
			goto out;
		}
		stage = "parse-live-cta";
		ret = gc570d_splitter_cta_physical_address(cta_blk,
							   &cache_c7,
							   &cache_c8);
		if (ret)
			goto out;
		/*
		 * If the monitor uses a 4-block EDID with an 0xf0 block map in
		 * block 1, keep the original block-1 filter behavior; for
		 * standard 2-block EDIDs (where block 1 is CTA-861), filter
		 * block 1 directly so both 2-block and 4-block displays work.
		 *
		 * publish_edid is consumed below as a flat 256-byte buffer
		 * (indices 0..0xff).  live_edid[0] is only a 128-byte row,
		 * so copy the two edited blocks into a flat staging buffer
		 * to avoid an out-of-bounds access through the row pointer.
		 */
		gc570d_splitter_windows_filter_cta(live_buf + 128);
		live_buf[128 + 0x7f] = 0;
		for (i = 0; i < 127; i++)
			live_buf[128 + 0x7f] -= live_buf[128 + i];
		publish_edid = live_buf;
	}

	stage = "validate-edid";
	checksum[0] = 0;
	checksum[1] = 0;
	for (i = 0; i < 128; i++) {
		checksum[0] += publish_edid[i];
		checksum[1] += publish_edid[128 + i];
	}
	if (memcmp(publish_edid,
		   "\x00\xff\xff\xff\xff\xff\xff\x00", 8) ||
	    checksum[0] || checksum[1]) {
		ret = -EILSEQ;
		goto out;
	}

	stage = "validate-bank-0";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x0f,
			       &initial_bank, &last_irq, &last_status);
	if (ret)
		goto out;
	if (initial_bank & 0x03) {
		ret = -EAGAIN;
		goto out;
	}

	stage = "disable-edid-publication";
	ret = gc570d_splitter_update8(gc, 0x70, 0x34, BIT(0), 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0xc6, 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0xc7,
				cache_c7);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0xc8,
				cache_c8);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x4b, 0xd9);
	if (ret)
		goto out_restore_bank;

	stage = "write-copy-edid";
	for (i = 0; i < 127; i++) {
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0xd8, i,
					publish_edid[i]);
		if (ret)
			goto out_restore_bank;
	}
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0xc9,
				publish_edid[0x7f]);
	if (ret)
		goto out_restore_bank;
	for (i = 128; i < 255; i++) {
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0xd8, i,
					publish_edid[i]);
		if (ret)
			goto out_restore_bank;
	}
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0xca,
				publish_edid[0xff]);
	if (ret)
		goto out_restore_bank;

	stage = "publish-edid-state";
	ret = gc570d_splitter_update8(gc, 0x58, 0x0f, BIT(0), BIT(0));
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x58, 0x10, BIT(6), BIT(6));
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x58, 0x0f, BIT(0), 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x70, 0xc5, BIT(0), BIT(0));
	if (ret)
		goto out_restore_bank;

	ret = gc570d_splitter_windows_hpd(gc, false, &stage);
	if (ret)
		goto out_restore_bank;
	stage = "official-driver-output-follow-up";
	ret = gc570d_splitter_update8(gc, 0x70, 0x53, 0xe0, 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x70, 0x54, 0xff, 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x70, 0x55, 0x07, 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x70, 0x57, 0x0f, 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x70, 0xc5, BIT(4), BIT(4));
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x70, 0xc5, BIT(4), 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x58, 0x0a, BIT(2), BIT(2));
	if (ret)
		goto out_restore_bank;
	usleep_range(1000, 2000);
	ret = gc570d_splitter_update8(gc, 0x58, 0x0a, BIT(2), 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_windows_hpd(gc, false, &stage);
	if (ret)
		goto out_restore_bank;
	stage = "restore-output-bank-3";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x27, 0x9f);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x28, 0x9f);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x29, 0x9f);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x20, 0x1b);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x21, 0x03);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		goto out_restore_bank;

	stage = "official-driver-hpd-delay";
	msleep(500);
	ret = gc570d_splitter_windows_hpd(gc, true, &stage);
	if (ret)
		goto out_restore_bank;
	stage = "finish-publication";
	ret = gc570d_splitter_update8(gc, 0x70, 0xc5, BIT(0), 0x00);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_splitter_update8(gc, 0x70, 0x34, BIT(0), BIT(0));
	if (ret)
		goto out_restore_bank;

	stage = "verify-final";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x0f,
			       &verify_main0f, &last_irq, &last_status);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x10,
			       &verify_main10, &last_irq, &last_status);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0xc5,
			       &verify_c5, &last_irq, &last_status);
	if (ret)
		goto out_restore_bank;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x34,
			       &verify34, &last_irq, &last_status);
	if (ret)
		goto out_restore_bank;
	if ((verify_main0f & BIT(0)) || !(verify_main10 & BIT(6)) ||
	    (verify_c5 & BIT(0)) || !(verify34 & BIT(0))) {
		ret = -EILSEQ;
		goto out_restore_bank;
	}

	dev_info(&gc->pdev->dev,
		 "VideoSplitter official-driver %s EDID publication completed: port=%u presence_mask=%02x blocks=2 checksums=%02x/%02x c7=%02x c8=%02x main0f=%02x main10=%02x c5=%02x reg34=%02x\n",
		 bridge_default ? "bridge-limited 4K420 profile" : "Copy_Mode",
		 port, presence_mask, publish_edid[0x7f], publish_edid[0xff],
		 cache_c7, cache_c8, verify_main0f, verify_main10, verify_c5,
		 verify34);
	kfree(live_buf);
	return 0;

out_restore_bank:
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (!ret)
		ret = cleanup_ret;
out:
	kfree(live_buf);
	dev_err(&gc->pdev->dev,
		"VideoSplitter official-driver %s EDID publication failed at %s: %d presence_mask=%02x aux03=%02x/%02x irq=0x%08x status=0x%08x\n",
		bridge_default ? "bridge-limited 4K420 profile" : "Copy_Mode",
		stage, ret, presence_mask, presence[1], presence[2], last_irq,
		last_status);
	return ret;
}

static int gc570d_splitter_windows_edid_copy(struct gc570d_dev *gc)
{
	return gc570d_splitter_windows_edid_publish(gc, false);
}

static int gc570d_splitter_windows_bridge_edid(struct gc570d_dev *gc)
{
	return gc570d_splitter_windows_edid_publish(gc, true);
}

static ssize_t
gc570d_splitter_windows_edid_copy_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_windows_edid_copy(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_windows_edid_copy_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_windows_edid_copy_write,
	.llseek = noop_llseek,
};

static ssize_t
gc570d_splitter_windows_bridge_edid_write(struct file *file,
					  const char __user *buffer,
					  size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_windows_bridge_edid(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_windows_bridge_edid_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_windows_bridge_edid_write,
	.llseek = noop_llseek,
};

/*
 * Reproduce the official driver's port-2 IRQ10=HPD|RxSense path without
 * reordering its blocking EDID dispatcher. The driver consumes the saved
 * channel IRQ, handles HPD first, synchronously detects/publishes EDID, and
 * only then executes the RxSense power-down/power-up branch.
 */
int
gc570d_splitter_windows_channel2_connect(struct gc570d_dev *gc,
					  bool bridge_default)
{
	static const u8 irq_regs[] = { 0x10, 0x11, 0x12, 0x13, 0x14 };
	static const u8 verify_regs[] = { 0x03, 0xc1, 0x86, 0x84, 0x88, 0x19 };
	u8 irq_values[ARRAY_SIZE(irq_regs)] = { 0 };
	u8 port1_irqs[ARRAY_SIZE(irq_regs)] = { 0 };
	u8 verify[ARRAY_SIZE(verify_regs)] = { 0 };
	u8 main_snapshot = 0;
	u8 final_main05 = 0;
	u8 aux03_port1 = 0;
	u8 aux03_hpd = 0;
	u8 aux03_rxsense = 0;
	u8 stable_mask = 0;
	u8 detail13 = 0;
	u8 target_port = 2;
	u8 target_slave = 0x6c;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "read-main-status";
	size_t i;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &main_snapshot, &last_irq, &last_status);
	if (ret)
		goto out;

	stage = "read-port-presence";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a, 0x03,
			       &aux03_port1, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6c, 0x03,
			       &aux03_hpd, &last_irq, &last_status);
	if (ret)
		goto out;

	/* Relaxed check: Trigger if either HPD or RxSense fires */
	/* Always drain pending IRQ bytes on port 2 if main05 BIT(2) is set. */
	if (main_snapshot & BIT(2)) {
		stage = "read-port2-irq";
		for (i = 0; i < ARRAY_SIZE(irq_regs); i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6c,
					       irq_regs[i], &irq_values[i],
					       &last_irq, &last_status);
			if (ret)
				goto out;
		}
		stage = "ack-port2-irq";
		for (i = 0; i < ARRAY_SIZE(irq_regs); i++) {
			if (!irq_values[i])
				continue;
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x6c,
						irq_regs[i], irq_values[i]);
			if (ret)
				goto out;
		}
	}

	/*
	 * If the external HDMI OUT display on port 2 (0x6c) is unplugged or
	 * powered off ((aux03_hpd & 0x03) != 0x03), cleanly power down port 2.
	 * If the internal bridge EDID is already published (0x70:0x34 BIT(0))
	 * and copy_edid is not active, do NOT pulse upstream HPD or tear down
	 * port 1 (0x6a, internal IT68051) so active HDMI IN 1 capture continues
	 * uninterrupted when unplugging the passthrough monitor.
	 */
	if ((aux03_hpd & 0x03) != 0x03) {
		u8 edid_ctrl = 0;

		stage = "port2-disconnect-power-down";
		ret = gc570d_splitter_aux_power_down(gc, 2);
		if (ret)
			goto out;
		gc->splitter_channel_active_mask &= ~BIT(2);
		gc->splitter_video_stable_mask &= ~BIT(2);
		gc->splitter_worker_state4_mask &= ~BIT(2);
		gc->splitter_pclk[2] = 0;
		gc->splitter_vclk[2] = 0;

		if (!(aux03_port1 & BIT(0))) {
			ret = -ENOLINK;
			goto out;
		}
		gc->splitter_channel_active_mask |= BIT(1);

		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x34,
				       &edid_ctrl, &last_irq, &last_status);
		if (!ret && (edid_ctrl & BIT(0)) && bridge_default) {
			u8 aux88_p1 = 0;
			u8 auxc1_p1 = 0;

			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
					       0x13, &detail13, &last_irq,
					       &last_status);
			gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a, 0x88,
					 &aux88_p1, &last_irq, &last_status);
			gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a, 0xc1,
					 &auxc1_p1, &last_irq, &last_status);
			if (!ret && (detail13 & BIT(4)) &&
			    ((aux03_port1 & 0x85) != 0x85 ||
			     (aux88_p1 & 0x03) != 0x00 ||
			     (auxc1_p1 & BIT(0)))) {
				ret = gc570d_splitter_scdt_channel_setup(
					gc, 1, &gc->splitter_pclk[1],
					&gc->splitter_vclk[1]);
				if (!ret) {
					gc->splitter_video_stable_pending = true;
					gc->splitter_video_stable_mask |= BIT(1);
				}
			}
			dev_info(&gc->pdev->dev,
				 "VideoSplitter port-2 disconnect handled: port1_03=%02x port1_88=%02x port1_c1=%02x detail13=%02x capture_preserved=yes\n",
				 aux03_port1, aux88_p1, auxc1_p1, detail13);
			return 0;
		}

		target_port = 1;
		target_slave = 0x6a;
		bridge_default = true;
	}

	if (target_port == 1 && (main_snapshot & BIT(1))) {
		stage = "read-port1-irq";
		for (i = 0; i < ARRAY_SIZE(irq_regs); i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a,
					       irq_regs[i], &port1_irqs[i],
					       &last_irq, &last_status);
			if (ret)
				goto out;
		}
		stage = "ack-port1-irq";
		for (i = 0; i < ARRAY_SIZE(irq_regs); i++) {
			if (!port1_irqs[i])
				continue;
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x6a,
						irq_regs[i], port1_irqs[i]);
			if (ret)
				goto out;
		}
	}

	stage = "hpd-upstream-high";
	ret = gc570d_splitter_windows_hpd(gc, true, &stage);
	if (ret)
		goto out;

	stage = bridge_default ? "bridge-edid-dispatch" :
				 "copy-edid-dispatch";
	ret = gc570d_splitter_windows_edid_publish(gc, bridge_default);
	if (ret) {
		/* If Copy_Mode fails because the monitor was unplugged mid-read,
		 * fall back to the internal 4K60 bridge EDID for port 1.
		 */
		if (!bridge_default && (aux03_port1 & BIT(0))) {
			bridge_default = true;
			target_port = 1;
			target_slave = 0x6a;
			ret = gc570d_splitter_windows_edid_publish(gc, true);
		}
		if (ret)
			goto out;
	}

	stage = "rxsense-read-presence";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, target_slave, 0x03,
			       &aux03_rxsense, &last_irq, &last_status);
	if (ret)
		goto out;
	if (target_port == 2 && !(aux03_rxsense & BIT(1))) {
		/* Monitor lost RxSense during EDID publish; fall back to port 1. */
		ret = gc570d_splitter_aux_power_down(gc, 2);
		if (ret)
			goto out;
		gc->splitter_channel_active_mask &= ~BIT(2);
		gc->splitter_video_stable_mask &= ~BIT(2);
		gc->splitter_worker_state4_mask &= ~BIT(2);
		target_port = 1;
		target_slave = 0x6a;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, target_slave,
				       0x03, &aux03_rxsense, &last_irq,
				       &last_status);
		if (ret)
			goto out;
	}
	if (!(aux03_rxsense & BIT(0))) {
		ret = -ENOLINK;
		goto out;
	}

	stage = "rxsense-power-down";
	ret = gc570d_splitter_aux_power_down(gc, target_port);
	if (ret)
		goto out;

	stage = "rxsense-power-up-common";
	ret = gc570d_splitter_aux_power_up_common(gc, target_port);
	if (ret)
		goto out;

	stage = "rxsense-upstream-hpd-high";
	ret = gc570d_splitter_windows_hpd(gc, true, &stage);
	if (ret)
		goto out;

	stage = "verify-channel";
	for (i = 0; i < ARRAY_SIZE(verify_regs); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, target_slave,
				       verify_regs[i], &verify[i],
				       &last_irq, &last_status);
		if (ret)
			goto out;
	}
	if (!(verify[2] & 0x08) ||
	    (verify[3] & 0xe0) != 0x80 || (verify[4] & 0x03) != 0x01 ||
	    (verify[5] & 0x07) != 0x07) {
		ret = -EILSEQ;
		goto out;
	}

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &final_main05, &last_irq, &last_status);
	if (ret)
		goto out;

	/*
	 * Read the receiver status to see if the upstream source is already active.
	 * If the source is active (detail13 bit 4 is set), we MUST force the stable_mask
	 * so that the output worker runs immediately and establishes the downstream link.
	 * Without this, plugging in a monitor while the console is running results in
	 * no passthrough output because vclk remains uninitialized.
	 *
	 * When the console does not drop TMDS clock during the HPD pulse,
	 * source_power_event() will not fire a new SCDT event, so run
	 * scdt_channel_setup() directly on the newly powered port(s) before
	 * arming the Video-Stable output workers.
	 */
	gc->video0_receiver_output_valid = false;
	stage = "check-upstream-video-stable";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
			       &detail13, &last_irq, &last_status);
	if (!ret && (detail13 & BIT(4))) {
		if (aux03_port1 & BIT(0)) {
			(void)gc570d_splitter_scdt_channel_setup(
				gc, 1, &gc->splitter_pclk[1],
				&gc->splitter_vclk[1]);
			stable_mask |= BIT(1);
		}
		if (target_port == 2) {
			(void)gc570d_splitter_scdt_channel_setup(
				gc, 2, &gc->splitter_pclk[2],
				&gc->splitter_vclk[2]);
		}
		stable_mask |= BIT(target_port);
	}

	gc->splitter_main_timer_serviced = false;
	gc->splitter_channel_active_mask |= BIT(target_port);
	if (aux03_port1 & BIT(0))
		gc->splitter_channel_active_mask |= BIT(1);
	if (stable_mask) {
		gc->splitter_video_stable_pending = true;
		gc->splitter_video_stable_mask |= stable_mask;
	}

	dev_info(&gc->pdev->dev,
		 "VideoSplitter port-%u connect completed: profile=%s main05=%02x irq=%02x/%02x/%02x/%02x/%02x port1_03=%02x port2_03=%02x rxsense03=%02x final main05=%02x verify=%02x/%02x/%02x/%02x/%02x/%02x worker_pending=%s\n",
		 target_port, bridge_default ? "bridge-limited" : "copy",
		 main_snapshot, irq_values[0], irq_values[1], irq_values[2],
		 irq_values[3], irq_values[4], aux03_port1, aux03_hpd,
		 aux03_rxsense, final_main05, verify[0], verify[1], verify[2],
		 verify[3], verify[4], verify[5],
		 stable_mask ? "yes" : "no");
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter port-%u connect failed at %s: %d profile=%s main05=%02x irq=%02x/%02x/%02x/%02x/%02x port1_03=%02x port2_03=%02x rxsense03=%02x irq_status=0x%08x status=0x%08x\n",
		target_port, stage, ret,
		bridge_default ? "bridge-limited" : "copy", main_snapshot,
		irq_values[0], irq_values[1], irq_values[2], irq_values[3],
		irq_values[4], aux03_port1, aux03_hpd, aux03_rxsense,
		last_irq, last_status);
	return ret;
}

static ssize_t
gc570d_splitter_windows_channel2_connect_write(struct file *file,
						const char __user *buffer,
						size_t count, loff_t *position,
						bool bridge_default)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_windows_channel2_connect(gc,
							       bridge_default);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

static ssize_t
gc570d_splitter_windows_channel2_copy_connect_write(struct file *file,
						     const char __user *buffer,
						     size_t count, loff_t *position)
{
	return gc570d_splitter_windows_channel2_connect_write(file, buffer,
							      count, position, false);
}

static ssize_t
gc570d_splitter_windows_channel2_bridge_connect_write(struct file *file,
						       const char __user *buffer,
						       size_t count, loff_t *position)
{
	return gc570d_splitter_windows_channel2_connect_write(file, buffer,
							      count, position, true);
}

const struct file_operations
gc570d_splitter_windows_channel2_copy_connect_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_windows_channel2_copy_connect_write,
	.llseek = noop_llseek,
};

const struct file_operations
gc570d_splitter_windows_channel2_bridge_connect_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_windows_channel2_bridge_connect_write,
	.llseek = noop_llseek,
};

static int gc570d_splitter_channel_irq_on_mask(struct gc570d_dev *gc,
					       u8 requested_mask)
{
	static const struct {
		u8 reg;
		u8 mask;
		u8 value;
	} power_up[] = {
		{ 0xc1, 0xf0, 0x80 }, { 0x86, 0x08, 0x08 },
		{ 0x84, 0xe0, 0x00 }, { 0x88, 0x03, 0x01 },
		{ 0x84, 0x80, 0x80 }, { 0x02, 0x01, 0x00 },
		{ 0x19, 0x07, 0x07 }, { 0xaf, 0xff, 0x00 },
	};
	u8 irq_values[2][5] = { { 0 } };
	u8 verify[2][6] = { { 0 } };
	u8 main_snapshot = 0;
	u8 active_mask = 0;
	u8 stable_mask = 0;
	u8 final_main05 = 0;
	u8 port;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "read-channel-irq";
	size_t i;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &main_snapshot, &last_irq, &last_status);
	if (ret)
		goto out;
	active_mask = main_snapshot & requested_mask & (BIT(1) | BIT(2));
	if (!active_mask) {
		ret = -ENODATA;
		goto out;
	}

	/* Snapshot every downstream IRQ before changing either channel.  The
	 * The official driver dispatcher selects each channel independently from main05;
	 * an absent physical sink therefore must not make the other channel fail.
	 */
	for (port = 1; port < 3; port++) {
		u8 index = port - 1;
		u8 slave = 0x68 + 2 * port;

		for (i = 0; i < 5; i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave,
					       0x10 + i, &irq_values[index][i],
					       &last_irq, &last_status);
			if (ret)
				goto out;
		}
	}

	for (port = 1; port < 3; port++) {
		u8 index = port - 1;
		u8 slave = 0x68 + 2 * port;

		if (!(active_mask & BIT(port)))
			continue;
		/* The official driver handles IRQ10 bits 0 (HPD), 1 (RxSense), and
		 * 7 (Video Stable On) in that order from one saved snapshot.
		 * A newly attached live source therefore commonly reports 0x83;
		 * it is not a separate or stale interrupt.
		 */
		if (!irq_values[index][0] && !irq_values[index][1] &&
		    !irq_values[index][2] && !irq_values[index][3] &&
		    !irq_values[index][4])
			continue;
		if (!(irq_values[index][0] & 0x03) ||
		    (irq_values[index][0] & ~0x83) || irq_values[index][1] ||
		    irq_values[index][2] || irq_values[index][3] ||
		    irq_values[index][4]) {
			ret = -ENODATA;
			goto out;
		}
		if (irq_values[index][0] & BIT(7))
			stable_mask |= BIT(port);

		stage = "ack-channel-irq";
		for (i = 0; i < 5; i++) {
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus,
						slave, 0x10 + i,
						irq_values[index][i]);
			if (ret)
				goto out;
		}

		stage = "validate-channel-presence";
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave, 0x03,
				       &verify[index][0], &last_irq, &last_status);
		if (ret)
			goto out;
		if ((verify[index][0] & 0x03) != 0x03) {
			ret = -ENOLINK;
			goto out;
		}

		/* Current RxSense-ON branch: reset the baseline transmitter, then
		 * apply the official driver with freshly cleared the official driver software state.
		 */
		stage = "channel-power-down";
		ret = gc570d_splitter_aux_power_down(gc, port);
		if (ret)
			goto out;
		stage = "channel-main-enable";
		ret = gc570d_splitter_update8(gc, 0x58, 0x08,
					      1U << port, 1U << port);
		if (ret)
			goto out;
		for (i = 0; i < ARRAY_SIZE(power_up); i++) {
			stage = "channel-power-up";
			ret = gc570d_splitter_aux_update8(gc, port,
						  power_up[i].reg,
						  power_up[i].mask,
						  power_up[i].value);
			if (ret)
				goto out;
		}

		stage = "reassert-upstream-hpd";
		ret = gc570d_splitter_hpd_high(gc);
		if (ret)
			goto out;
	}

	msleep(100);
	stage = "verify";
	for (port = 1; port < 3; port++) {
		static const u8 regs[] = { 0x03, 0xc1, 0x86, 0x84, 0x88, 0x19 };
		u8 index = port - 1;
		u8 slave = 0x68 + 2 * port;

		if (!(active_mask & BIT(port)))
			continue;

		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave,
					       regs[i], &verify[index][i],
					       &last_irq, &last_status);
			if (ret)
				goto out;
		}
		if (!(verify[index][2] & 0x08) ||
		    (verify[index][3] & 0xe0) != 0x80 ||
		    (verify[index][4] & 0x03) != 0x01 ||
		    (verify[index][5] & 0x07) != 0x07) {
			ret = -EILSEQ;
			goto out;
		}
	}
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &final_main05, &last_irq, &last_status);
	if (ret)
		goto out;

	dev_info(&gc->pdev->dev,
		 "VideoSplitter downstream channel IRQs handled: main05=%02x active=%02x stable=%02x irq1=%02x/%02x/%02x/%02x/%02x irq2=%02x/%02x/%02x/%02x/%02x final main05=%02x ch1=%02x/%02x/%02x/%02x/%02x/%02x ch2=%02x/%02x/%02x/%02x/%02x/%02x worker_pending=%s\n",
		 main_snapshot, active_mask, stable_mask,
		 irq_values[0][0], irq_values[0][1], irq_values[0][2],
		 irq_values[0][3], irq_values[0][4], irq_values[1][0],
		 irq_values[1][1], irq_values[1][2], irq_values[1][3],
		 irq_values[1][4], final_main05, verify[0][0], verify[0][1],
		 verify[0][2], verify[0][3], verify[0][4], verify[0][5],
		 verify[1][0], verify[1][1], verify[1][2], verify[1][3],
		 verify[1][4], verify[1][5], stable_mask ? "yes" : "no");
	gc->splitter_main_timer_serviced = false;
	gc->splitter_channel_active_mask = active_mask;
	gc->splitter_video_stable_pending = stable_mask != 0;
	gc->splitter_video_stable_mask = stable_mask;
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter downstream channel IRQ handling failed at %s: %d main05=%02x active=%02x irq1=%02x/%02x/%02x/%02x/%02x irq2=%02x/%02x/%02x/%02x/%02x\n",
		stage, ret, main_snapshot, active_mask, irq_values[0][0], irq_values[0][1],
		irq_values[0][2], irq_values[0][3], irq_values[0][4],
		irq_values[1][0], irq_values[1][1], irq_values[1][2],
		irq_values[1][3], irq_values[1][4]);
	return ret;
}

static int gc570d_splitter_channel_irq_on(struct gc570d_dev *gc)
{
	return gc570d_splitter_channel_irq_on_mask(gc, BIT(1) | BIT(2));
}

static int gc570d_splitter_channel1_irq_on(struct gc570d_dev *gc)
{
	/* The official driver dispatches each channel from the saved main05 snapshot.
	 * Port 2 may therefore hold a different, valid IRQ while port 1 enters
	 * the RxSense-ON branch.  Limit this diagnostic entry point to the bit
	 * The official driver selected instead of treating the unrelated port-2 IRQ as an
	 * error.
	 */
	return gc570d_splitter_channel_irq_on_mask(gc, BIT(1));
}

static ssize_t gc570d_splitter_channel_irq_on_write(struct file *file,
						      const char __user *buffer,
						      size_t count,
						      loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_channel_irq_on(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_channel_irq_on_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_channel_irq_on_write,
	.llseek = noop_llseek,
};

static ssize_t gc570d_splitter_channel1_irq_on_write(struct file *file,
					       const char __user *buffer,
					       size_t count,
					       loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_channel1_irq_on(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_channel1_irq_on_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_channel1_irq_on_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_main_timer_irq(struct gc570d_dev *gc)
{
	static const u8 page1_registers[] = { 0x21, 0x22, 0x23 };
	u8 page1_values[ARRAY_SIZE(page1_registers)] = { 0 };
	u8 main05 = 0;
	u8 main06 = 0;
	u8 main07 = 0;
	u8 page = 0;
	u8 final05 = 0;
	u8 final06 = 0;
	u8 final07 = 0;
	u8 detail13 = 0;
	u8 detail14 = 0;
	u8 detail_e5 = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "read-main-status";
	size_t i;
	int restore_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &main05, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x06,
			       &main06, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x07,
			       &main07, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(main05 & 0x60) || main06 || !(main07 & 0x30) ||
	    (main07 & ~0x30)) {
		ret = -ENODATA;
		goto out;
	}

	stage = "ack-main-irq";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x06,
				main06);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x07,
				main07);
	if (ret)
		goto out;

	stage = "read-page";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x0f,
			       &page, &last_irq, &last_status);
	if (ret)
		goto out;
	stage = "ack-page1-irq";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x0f,
				page | BIT(0));
	if (ret)
		goto out;
	for (i = 0; i < ARRAY_SIZE(page1_registers); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58,
				       page1_registers[i], &page1_values[i],
				       &last_irq, &last_status);
		if (ret)
			break;
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					page1_registers[i], page1_values[i]);
		if (ret)
			break;
	}
	restore_ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					 0x0f, page);
	if (!ret)
		ret = restore_ret;
	if (ret)
		goto out;

	/* reg07 bit 4 is the official driver's timer-0 tick. Its software counters are
	 * clear in this staged path, so the hardware-equivalent action ends
	 * after acknowledging the main and page-1 interrupt snapshots.
	 */
	if (main07 & 0x20) {
		stage = "timer-1-status";
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x14,
				       &detail14, &last_irq, &last_status);
		if (ret)
			goto out;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
				       &detail13, &last_irq, &last_status);
		if (ret)
			goto out;

		if (!(detail14 & 0x38) && (detail13 & 0x10)) {
			stage = "timer-1-main-sequence";
			ret = gc570d_splitter_update8(gc, 0x58, 0x1a,
						      0x02, 0x00);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x58, 0x19,
						      0x20, 0x00);
			if (ret)
				goto out;
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
						0x1d, 0x81);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x58, 0x19,
						      0x20, 0x20);
			if (ret)
				goto out;
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
						0x07, 0xff);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x58, 0x1a,
						      0x02, 0x02);
			if (ret)
				goto out;

			stage = "timer-1-receiver-sequence";
			ret = gc570d_splitter_update8(gc, 0x70, 0x0f,
						      0x03, 0x03);
			if (ret)
				goto out;
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
						       0xe5, &detail_e5,
						       &last_irq, &last_status);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0xe5, 0x0c,
						      (detail_e5 & 0x0c) ? 0x00 : 0x0c);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0xe5,
						      0x10, 0x10);
			if (ret)
				goto out;
			ret = gc570d_splitter_update8(gc, 0x70, 0x0f,
						      0x03, 0x00);
			if (ret)
				goto out;
		}
	}

	msleep(20);
	stage = "verify";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &final05, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x06,
			       &final06, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x07,
			       &final07, &last_irq, &last_status);
	if (ret)
		goto out;
	if (final06) {
		ret = -EILSEQ;
		goto out;
	}
	if (!gc->splitter_main_timer_serviced) {
		dev_info(&gc->pdev->dev,
			 "VideoSplitter main timer IRQs handled: main=%02x/%02x/%02x page1=%02x/%02x/%02x detail13=%02x detail14=%02x e5=%02x final=%02x/%02x/%02x\n",
			 main05, main06, main07, page1_values[0], page1_values[1],
			 page1_values[2], detail13, detail14, detail_e5, final05,
			 final06, final07);
	}
	gc->splitter_main_timer_serviced = true;
	return 0;

out:
	gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	dev_err(&gc->pdev->dev,
		"VideoSplitter main timer IRQ handling failed at %s: %d main=%02x/%02x/%02x page1=%02x/%02x/%02x\n",
		stage, ret, main05, main06, main07, page1_values[0],
		page1_values[1], page1_values[2]);
	return ret;
}

static ssize_t gc570d_splitter_main_timer_irq_write(struct file *file,
						     const char __user *buffer,
						     size_t count,
						     loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_main_timer_irq(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_main_timer_irq_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_main_timer_irq_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_channel_video_stable_irq(struct gc570d_dev *gc)
{
	u8 irq_values[2][5] = { { 0 } };
	u8 final_irq[2][5] = { { 0 } };
	u8 aux03[2] = { 0 };
	u8 main05 = 0;
	u8 active_mask = 0;
	u8 detail13 = 0;
	u8 final_main05 = 0;
	u8 stable_mask = 0;
	u8 new_stable_mask = 0;
	u8 acked_mask = 0;
	u8 port;
	u32 last_irq = 0;
	u32 last_status = 0;
	bool timer_serviced = gc->splitter_main_timer_serviced;
	const char *stage = "read-main-status";
	size_t i;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &main05, &last_irq, &last_status);
	if (ret)
		goto out;
	active_mask = main05 & (BIT(1) | BIT(2));
	/* The official driver dispatches receiver, timer, port 1, and port 2 from one
	 * saved main05 snapshot, in that order.  Receiver bit 4 can therefore
	 * remain set in the value observed here after the staged receiver/timer
	 * calls; the official driver still dispatches both downstream bits from that same
	 * snapshot.  The serviced latch remains diagnostic state for the staged
	 * first-link test; the eventual periodic pump will own the whole snapshot.
	 */
	if (!active_mask) {
		ret = -ENODATA;
		goto out;
	}

	stage = "read-receiver-stability";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
			       &detail13, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(detail13 & BIT(4))) {
		ret = -ENOLINK;
		goto out;
	}

	stage = "read-channel-irq";
	for (port = 1; port < 3; port++) {
		u8 index = port - 1;
		u8 slave = 0x68 + 2 * port;

		for (i = 0; i < 5; i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave,
					       0x10 + i, &irq_values[index][i],
					       &last_irq, &last_status);
			if (ret)
				goto out;
		}
		if (!(active_mask & BIT(port)))
			continue;
		/* The official driver accepts a bundled HPD/RxSense/Video-Stable
		 * snapshot and dispatches each bit independently.  The port-1
		 * internal leg commonly reports 0x83 while the physical port 2
		 * asserts its 0x80 stable interrupt a little later.
		 */
		if (!irq_values[index][0] && !irq_values[index][1] &&
		    !irq_values[index][2] && !irq_values[index][3] &&
		    !irq_values[index][4])
			continue;
		acked_mask |= BIT(port);
		if (!(irq_values[index][0] & BIT(7)))
			continue;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave, 0x03,
				       &aux03[index], &last_irq, &last_status);
		if (ret)
			goto out;
		if ((aux03[index] & (BIT(7) | BIT(2) | BIT(0))) !=
		    (BIT(7) | BIT(2) | BIT(0)))
			continue;
		stable_mask |= BIT(port);
	}

	/* Common W1C prefix of the official driver. IRQ10 bit 7 only changes
	 * The official driver software state to Video-Stable=1; its output worker is a
	 * separate later stage.
	 */
	stage = "ack-video-stable-irq";
	for (port = 1; port < 3; port++) {
		u8 index = port - 1;
		u8 slave = 0x68 + 2 * port;

		if (!(acked_mask & BIT(port)))
			continue;
		for (i = 0; i < 5; i++) {
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, slave,
						0x10 + i, irq_values[index][i]);
			if (ret)
				goto out;
		}
	}

	msleep(20);
	stage = "verify";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &final_main05, &last_irq, &last_status);
	if (ret)
		goto out;
	for (port = 1; port < 3; port++) {
		u8 index = port - 1;
		u8 slave = 0x68 + 2 * port;

		if (!(acked_mask & BIT(port)))
			continue;
		for (i = 0; i < 5; i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave,
					       0x10 + i, &final_irq[index][i],
					       &last_irq, &last_status);
			if (ret)
				goto out;
		}
		if (final_irq[index][0] || final_irq[index][1] ||
		    final_irq[index][2] || final_irq[index][3] ||
		    final_irq[index][4]) {
			ret = -EILSEQ;
			goto out;
		}
	}
	/* A later port-2 stable interrupt may assert while this pass is
	 * acknowledging port 1.  the official driver leaves that new main bit for its next
	 * 20-ms dispatcher pass, so only require the ports actually acknowledged
	 * by this pass to have cleared.
	 */
	if (final_main05 & acked_mask) {
		ret = -EILSEQ;
		goto out;
	}
	{
		u8 p1_03 = 0;

		if (!gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a,
				      0x03, &p1_03, &last_irq,
				      &last_status) &&
		    (p1_03 & 0x85) == 0x85)
			stable_mask |= BIT(1);
	}
	new_stable_mask = stable_mask & ~gc->splitter_video_stable_mask;
	if (new_stable_mask) {
		gc->splitter_video_stable_pending = true;
		gc->splitter_video_stable_mask |= new_stable_mask;
	}
	/* The official driver only moves its per-port state to worker-pending on the
	 * first stable 0->1 transition.  The link/analog sequence itself raises
	 * another IRQ10.bit7; acknowledging that repeated level must not queue
	 * The official driver again or the transmitter is reset forever.
	 *
	 * Wait for port 2 when a downstream monitor is present on port 2;
	 * otherwise arm on port 1 for standalone capture.
	 */
	{
		u8 port2_present = 0;
		u8 required_bit = BIT(1);

		if (!gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6c, 0x03,
				      &port2_present, &last_irq, &last_status) &&
		    (port2_present & 0x03) == 0x03)
			required_bit = BIT(2);

		if (!(new_stable_mask & required_bit)) {
			gc->splitter_main_timer_serviced = false;
			if ((stable_mask & required_bit) ||
			    (gc->splitter_video_stable_mask & required_bit)) {
				dev_dbg(&gc->pdev->dev,
					"VideoSplitter repeated Video Stable IRQ (mask=%02x) acknowledged without rearming worker\n",
					stable_mask);
				return -EALREADY;
			}
			stage = "await-video-stable";
			ret = -EAGAIN;
			goto out;
		}
	}
	gc->splitter_main_timer_serviced = false;

	dev_info(&gc->pdev->dev,
		 "VideoSplitter Video Stable On IRQ handled: main05=%02x active=%02x stable=%02x timer_serviced=%s detail13=%02x aux03=%02x/%02x irq1=%02x/%02x/%02x/%02x/%02x irq2=%02x/%02x/%02x/%02x/%02x final main05=%02x worker_pending=yes\n",
		 main05, active_mask, stable_mask,
		 timer_serviced ? "yes" : "no", detail13, aux03[0], aux03[1],
		 irq_values[0][0], irq_values[0][1], irq_values[0][2],
		 irq_values[0][3], irq_values[0][4], irq_values[1][0],
		 irq_values[1][1], irq_values[1][2], irq_values[1][3],
		 irq_values[1][4], final_main05);
	return 0;

out:
	if (ret == -ENOLINK || ret == -ENODATA ||
	    ret == -EAGAIN || ret == -EALREADY) {
		dev_dbg(&gc->pdev->dev,
			"VideoSplitter Video Stable On IRQ deferred at %s: %d main05=%02x active=%02x timer_serviced=%s detail13=%02x aux03=%02x/%02x irq1=%02x/%02x/%02x/%02x/%02x irq2=%02x/%02x/%02x/%02x/%02x\n",
			stage, ret, main05, active_mask,
			gc->splitter_main_timer_serviced ? "yes" : "no",
			detail13, aux03[0], aux03[1],
			irq_values[0][0], irq_values[0][1], irq_values[0][2],
			irq_values[0][3], irq_values[0][4], irq_values[1][0],
			irq_values[1][1], irq_values[1][2], irq_values[1][3],
			irq_values[1][4]);
	} else {
		dev_warn_ratelimited(&gc->pdev->dev,
			"VideoSplitter Video Stable On IRQ failed at %s: %d main05=%02x active=%02x timer_serviced=%s detail13=%02x aux03=%02x/%02x irq1=%02x/%02x/%02x/%02x/%02x irq2=%02x/%02x/%02x/%02x/%02x\n",
			stage, ret, main05, active_mask,
			gc->splitter_main_timer_serviced ? "yes" : "no",
			detail13, aux03[0], aux03[1],
			irq_values[0][0], irq_values[0][1], irq_values[0][2],
			irq_values[0][3], irq_values[0][4], irq_values[1][0],
			irq_values[1][1], irq_values[1][2], irq_values[1][3],
			irq_values[1][4]);
	}
	return ret;
}

static ssize_t
gc570d_splitter_channel_video_stable_irq_write(struct file *file,
						const char __user *buffer,
						size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_channel_video_stable_irq(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations
gc570d_splitter_channel_video_stable_irq_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_channel_video_stable_irq_write,
	.llseek = noop_llseek,
};

int gc570d_splitter_stable_worker_probe(struct gc570d_dev *gc)
{
	static const u8 detail_regs[] = {
		0x13, 0x14, 0x19, 0x9d, 0x9e, 0x98,
	};
	static const u8 bank2_regs[] = { 0x15, 0x16, 0x17 };
	static const u8 aux_regs[] = {
		0x03, 0x06, 0x07, 0x18, 0x83, 0x84, 0x85, 0xc0,
	};
	static const char * const color_names[] = {
		"RGB444", "YCbCr422", "YCbCr444", "YCbCr420",
	};
	u8 detail[ARRAY_SIZE(detail_regs)] = { 0 };
	u8 bank2[ARRAY_SIZE(bank2_regs)] = { 0 };
	u8 aux[ARRAY_SIZE(aux_regs)] = { 0 };
	u8 live_aux03[2] = { 0 };
	u8 main05 = 0;
	u8 main07 = 0;
	u8 port = 0;
	u8 live_mask = 0;
	u8 selected_mask;
	u8 color;
	u8 depth_code;
	u16 timing_code;
	bool clock_cached;
	bool clock_measured = false;
	const char *vclk_band;
	u32 probe_pclk;
	u32 probe_vclk;
	struct gc570d_splitter_clock_sample clock_sample = { 0 };
	struct gc570d_splitter_rx_timing rx_timing = { 0 };
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "read-live-channel-state";
	size_t i;
	int cleanup_ret;
	int ret;

	for (port = 1; port < 3; port++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
				       0x68 + 2 * port, 0x03,
				       &live_aux03[port - 1], &last_irq,
				       &last_status);
		if (ret)
			goto out;
		if ((live_aux03[port - 1] & 0x85) == 0x85)
			live_mask |= BIT(port);
	}

	stage = "validate-live-worker";
	/* This entry point ports the port-2 invocation of the official driver.
	 * The official driver invokes the worker once per output, so a simultaneously stable
	 * port 1 is not an error and must not change the selected transmitter.
	 * Fall back to port 1 only when port 2 has no downstream sink connected.
	 */
	if (!live_mask) {
		ret = -ENODATA;
		goto out;
	}
	selected_mask = (live_mask & BIT(2)) ? BIT(2) : BIT(1);
	if (gc->splitter_video_stable_pending &&
	    !(gc->splitter_video_stable_mask & selected_mask)) {
		ret = -ESTALE;
		goto out;
	}
	for (port = 1; port < 3; port++) {
		if (selected_mask & BIT(port))
			break;
	}
	if (port >= 3) {
		ret = -ENODATA;
		goto out;
	}

	stage = "read-main-status";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &main05, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x07,
			       &main07, &last_irq, &last_status);
	if (ret)
		goto out;

	stage = "read-receiver-bank0";
	for (i = 0; i < ARRAY_SIZE(detail_regs); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       detail_regs[i], &detail[i],
				       &last_irq, &last_status);
		if (ret)
			goto out;
	}
	if ((detail[0] & 0x11) != 0x11 || !(detail[2] & 0x80)) {
		stage = "validate-live-receiver";
		ret = -ENOLINK;
		goto out;
	}

	stage = "select-receiver-bank2";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x02);
	if (ret)
		goto out;
	stage = "read-receiver-bank2";
	for (i = 0; i < ARRAY_SIZE(bank2_regs); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70,
				       bank2_regs[i], &bank2[i],
				       &last_irq, &last_status);
		if (ret)
			goto restore_bank;
	}
	stage = "restore-receiver-bank0";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		goto out;

	clock_cached = gc->splitter_vclk[port] != 0;
	probe_pclk = gc->splitter_pclk[port];
	probe_vclk = gc->splitter_vclk[port];
	if (!clock_cached) {
		stage = "measure-bridge-clock";
		ret = gc570d_splitter_measure_channel_clock(gc, port,
						     &probe_pclk,
						     &probe_vclk,
						     &clock_sample);
		if (ret)
			goto out;
		clock_measured = true;
	}

	stage = "read-channel-inputs";
	for (i = 0; i < ARRAY_SIZE(aux_regs); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
				       0x68 + 2 * port, aux_regs[i], &aux[i],
				       &last_irq, &last_status);
		if (ret)
			goto out;
	}

	color = (bank2[0] >> 5) & 0x03;
	depth_code = (detail[5] >> 4) & 0x03;
	timing_code = ((u16)(detail[4] & 0x3f) << 8) | detail[3];
	stage = "measure-receiver-timing";
	ret = gc570d_splitter_measure_rx_timing(gc, color, &rx_timing);
	if (ret)
		goto out;
	if (!probe_vclk)
		vclk_band = "zero-invalid";
	else if (probe_vclk < 150001)
		vclk_band = "lt150001";
	else if (probe_vclk < 310001)
		vclk_band = "150001-310000";
	else if (probe_vclk < 375001)
		vclk_band = "310001-375000";
	else if (probe_vclk <= 620999)
		vclk_band = "375001-620999";
	else
		vclk_band = "invalid-over620999";
	dev_info(&gc->pdev->dev,
		 "VideoSplitter stable worker inputs: port=%u pending=%s pending_mask=%02x live_mask=%02x live_aux03=%02x/%02x main=%02x/%02x detail13=%02x detail14=%02x detail19=%02x detail9d=%02x detail9e=%02x detail98=%02x bank2_15=%02x bank2_16=%02x bank2_17=%02x input_color=%s bank2_15_low=%u rx_hdmi_mode_bit=%s depth_code=%u timing_code=%04x timing_signature=%s rx_counter_sum=%u rx_pixel_clock=%u rx_tmds_clock=%u rx_htotal=%u rx_hactive=%u rx_hfront=%u rx_hsync=%u rx_vtotal=%u rx_vactive=%u rx_vfront=%u rx_vsync=%u rx_frame_rate=%u rx_interlaced=%s rx_tmds_over_bridge_guard=%s clock_cached=%s clock_measured=%s counter_initial=%u counter_sum=%u counter_divider=%u counter_divisor=%u counter_depth=%u pclk=%u vclk=%u vclk_band=%s gt270mhz=%s gt300mhz=%s gt340mhz=%s windows_vclk_guard_valid=%s aux03=%02x aux06=%02x aux07=%02x aux18=%02x aux83=%02x aux84=%02x aux85=%02x auxc0=%02x measurement_writes_only=yes format_prerequisite_applied=no no_output_writes=yes\n",
		 port, gc->splitter_video_stable_pending ? "yes" : "no",
		 gc->splitter_video_stable_mask, live_mask, live_aux03[0],
		 live_aux03[1], main05, main07, detail[0], detail[1],
		 detail[2], detail[3], detail[4], detail[5], bank2[0],
		 bank2[1], bank2[2], color_names[color], bank2[0] & 0x1f,
		 (detail[0] & BIT(1)) ? "yes" : "no", depth_code,
		 timing_code,
		 (((u16)(timing_code - 0x0f00)) & 0xfeff) == 0 ? "yes" : "no",
		 rx_timing.counter_sum, rx_timing.pixel_clock,
		 rx_timing.tmds_clock, rx_timing.htotal, rx_timing.hactive,
		 rx_timing.hfront, rx_timing.hsync, rx_timing.vtotal,
		 rx_timing.vactive, rx_timing.vfront, rx_timing.vsync,
		 rx_timing.frame_rate,
		 (rx_timing.flags & BIT(0)) ? "yes" : "no",
		 rx_timing.tmds_clock > 620999 ? "yes" : "no",
		 clock_cached ? "yes" : "no", clock_measured ? "yes" : "no",
		 clock_sample.initial_counter, clock_sample.counter_sum,
		 clock_sample.divider, clock_sample.divisor,
		 clock_sample.depth_code,
		 probe_pclk, probe_vclk, vclk_band,
		 probe_vclk > 270000 ? "yes" : "no",
		 probe_vclk > 300000 ? "yes" : "no",
		 probe_vclk > 340000 ? "yes" : "no",
		 (probe_vclk > 0 && probe_vclk <= 620999) ? "yes" : "no",
		 aux[0],
		 aux[1], aux[2], aux[3], aux[4], aux[5], aux[6], aux[7]);
	return 0;

restore_bank:
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (!ret)
		ret = cleanup_ret;
out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter stable worker input probe failed at %s: %d pending=%s mask=%02x live_mask=%02x live_aux03=%02x/%02x port=%u irq=0x%08x status=0x%08x\n",
		stage, ret, gc->splitter_video_stable_pending ? "yes" : "no",
		gc->splitter_video_stable_mask, live_mask, live_aux03[0],
		live_aux03[1], port, last_irq, last_status);
	return ret;
}

static ssize_t
gc570d_splitter_stable_worker_probe_write(struct file *file,
					  const char __user *buffer,
					  size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_stable_worker_probe(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_stable_worker_probe_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_stable_worker_probe_write,
	.llseek = noop_llseek,
};

/* The official driver treats the AVI color selector as receiver-wide and uses
 * it to program the matching CSC path before either transmitter worker runs.
 */
static int gc570d_splitter_windows_avi_csc(struct gc570d_dev *gc,
					   u8 *input_color_out)
{
	static const u8 rgb_preamble[3] = { 0x00, 0x80, 0x10 };
	static const u8 ycbcr_preamble[3] = { 0x00, 0x00, 0x00 };
	static const u8 rgb_matrix[2][18] = {
		{ 0xb2, 0x04, 0x65, 0x02, 0xe9, 0x00,
		  0x93, 0x3c, 0x18, 0x04, 0x55, 0x3f,
		  0x49, 0x3d, 0x9f, 0x3e, 0x18, 0x04 },
		{ 0xb8, 0x05, 0xb4, 0x01, 0x94, 0x00,
		  0x4a, 0x3c, 0x17, 0x04, 0x9f, 0x3f,
		  0xd9, 0x3c, 0x10, 0x3f, 0x17, 0x04 },
	};
	static const u8 ycbcr_matrix[2][18] = {
		{ 0x00, 0x08, 0x6b, 0x3a, 0x50, 0x3d,
		  0x00, 0x08, 0xf5, 0x0a, 0x02, 0x00,
		  0x00, 0x08, 0xfd, 0x3f, 0xda, 0x0d },
		{ 0x00, 0x08, 0x55, 0x3c, 0x88, 0x3e,
		  0x00, 0x08, 0x51, 0x0c, 0x00, 0x00,
		  0x00, 0x08, 0x00, 0x00, 0x84, 0x0e },
	};
	static const char * const color_name[] = {
		"RGB444", "YCbCr422", "YCbCr444", "YCbCr420",
	};
	const u8 *preamble;
	const u8 *matrix;
	u8 bank2_15 = 0, bank2_16 = 0, detail14 = 0;
	u8 input_color;
	u8 main6b;
	u8 main6c;
	u8 matrix_index;
	bool hdmi20;
	bool write_second_path;
	u32 last_irq = 0, last_status = 0;
	size_t i;
	int cleanup_ret;
	int ret;

	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x02);
	if (ret)
		return ret;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x15,
			       &bank2_15, &last_irq, &last_status);
	if (!ret)
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x16,
				       &bank2_16, &last_irq, &last_status);
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		return ret;
	if (cleanup_ret)
		return cleanup_ret;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x14,
			       &detail14, &last_irq, &last_status);
	if (ret)
		return ret;

	input_color = (bank2_15 >> 5) & 0x03;
	matrix_index = (bank2_16 & 0xc0) == 0x80;
	hdmi20 = !!(detail14 & BIT(7));
	preamble = input_color == 0 ? rgb_preamble : ycbcr_preamble;
	matrix = input_color == 0 ? rgb_matrix[matrix_index] :
					 ycbcr_matrix[matrix_index];
	/* The official driver programs both paths for 4:2:0 and for HDMI-2.0 RGB/4:4:4.
	 * Its HDMI-2.0 4:2:2 branch deliberately programs only path 0x70.
	 */
	write_second_path = input_color == 3 ||
			    (hdmi20 && (input_color == 0 || input_color == 2));
	if (write_second_path) {
		for (i = 0; i < 3; i++) {
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
						0x88 + i, preamble[i]);
			if (ret)
				return ret;
		}
		for (i = 0; i < 18; i++) {
			ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
						0x93 + i, matrix[i]);
			if (ret)
				return ret;
		}
	}
	for (i = 0; i < 3; i++) {
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0x70 + i, preamble[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < 18; i++) {
		ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58,
					0x73 + i, matrix[i]);
		if (ret)
			return ret;
	}

	/* Exact packed selector values produced by the official driver from the
	 * reset-state caches initialized by the official driver state-2 path.
	 */
	if (input_color == 3) {
		main6b = 0x28;
		main6c = 0x28;
	} else if (input_color == 0 && hdmi20) {
		main6b = 0x7a;
		main6c = 0x10;
	} else if (input_color == 2 && hdmi20) {
		main6b = 0x73;
		main6c = 0x18;
	} else if (input_color == 0) {
		main6b = 0x4a;
		main6c = 0x00;
	} else {
		main6b = 0x43;
		main6c = 0x00;
	}
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x6b,
				main6b);
	if (ret)
		return ret;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x58, 0x6c,
				main6c);
	if (ret)
		return ret;
	if (input_color_out)
		*input_color_out = input_color;

	dev_info(&gc->pdev->dev,
		 "VideoSplitter official-driver AVI/CSC propagation completed: input=%s transport=%s colorimetry=%s main6b=%02x main6c=%02x paths=%s\n",
		 color_name[input_color], hdmi20 ? "HDMI2.0" : "HDMI1.4",
		 matrix_index ? "ITU709" : "ITU601", main6b, main6c,
		 write_second_path ? "70/88" : "70");
	return 0;
}

/* Prefix of the official driver.  the official driver (SCDC/output enable) remains
 * deferred.
 */
static int gc570d_splitter_stable_worker_link_setup_port(struct gc570d_dev *gc,
							  u8 port)
{
	static const u8 verify_regs[] = {
		0x84, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x94,
		0xa0, 0xa3, 0xad, 0xaf, 0xc0, 0xc1,
	};
	u8 verify[ARRAY_SIZE(verify_regs)] = { 0 };
	u8 aux03_1 = 0, aux03_2 = 0, detail13 = 0, detail98 = 0;
	u8 bank2_15 = 0, input_color = 0, depth_code = 0, main0d = 0;
	u8 band87, band89, band8b;
	u8 slave = 0x68 + 2 * port;
	u32 pclk, vclk, last_irq = 0, last_status = 0;
	struct gc570d_splitter_clock_sample sample = { 0 };
	struct gc570d_splitter_rx_timing rx_timing = { 0 };
	const char *stage = "read-live-channel-state";
	size_t i;
	int cleanup_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a, 0x03,
			       &aux03_1, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6c, 0x03,
			       &aux03_2, &last_irq, &last_status);
	if (ret)
		goto out;
	stage = "validate-selected-port-stable";
	if (((port == 1 ? aux03_1 : aux03_2) & 0x85) != 0x85) {
		ret = -ENODATA;
		goto out;
	}

	stage = "read-receiver-format";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
			       &detail13, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x98,
			       &detail98, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x02);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x15,
			       &bank2_15, &last_irq, &last_status);
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		goto out;
	if (cleanup_ret) {
		ret = cleanup_ret;
		goto out;
	}
	stage = "validate-live-hdmi-format";
	if ((detail13 & 0x13) != 0x13) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	depth_code = (detail98 >> 4) & 0x03;
	input_color = (bank2_15 >> 5) & 0x03;

	/*
	 * The official driver performs this receiver timing/FIFO pulse before it
	 * chooses the per-port output format.  the official driver gates the pulse on its
	 * two software CP-state bytes not being 4 and main0d being zero.  The
	 * bridge profile disables HDCP, so both CP states are zero here; retain
	 * the observable main0d gate and the exact register order.
	 */
	stage = "read-format-prerequisite";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x0d,
			       &main0d, &last_irq, &last_status);
	if (ret)
		goto out;
	if (main0d == 0) {
		stage = "pulse-format-prerequisite";
		ret = gc570d_splitter_update8(gc, 0x58, 0x0b, 0xfc, 0xfc);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x0b, 0xfc, 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_update8(gc, 0x58, 0x4e, 0x0f, 0x0c);
		if (ret)
			goto out;
		msleep(10);
		ret = gc570d_splitter_update8(gc, 0x58, 0x4e, 0x0f, 0x00);
		if (ret)
			goto out;

		stage = "measure-format-prerequisite";
		ret = gc570d_splitter_measure_rx_timing(gc, 3, &rx_timing);
		if (ret)
			goto out;
	}

	stage = "propagate-windows-avi-csc";
	ret = gc570d_splitter_windows_avi_csc(gc, &input_color);
	if (ret)
		goto out;

	/*
	 * The official driver configures original/bypass output here.  The
	 * transmitter HDMI-mode bit follows receiver reg13 bit 1 and is
	 * suppressed for a DVI sink.  The parsed bridge EDID describes an HDMI
	 * sink, so the live receiver bit is authoritative here.
	 */
	stage = "apply-original-bypass-format";
	ret = gc570d_splitter_aux_update8(gc, port, 0xc0, 0x01,
					  detail13 & BIT(1) ? 0x01 : 0x00);
	if (ret)
		goto out;
	/* The official driver only clears auxc1[7:4] for an 8-bit original
	 * stream.  For 10/12-bit original bypass it preserves the receiver's
	 * color-depth encoding.  the official driver then mirrors the presence of
	 * deep color into auxc1[2].
	 */
	if (depth_code == 0) {
		ret = gc570d_splitter_aux_update8(gc, port, 0xc1, 0xf0, 0x00);
		if (ret)
			goto out;
	}
	ret = gc570d_splitter_aux_update8(gc, port, 0xc1, 0x04,
					  depth_code ? 0x04 : 0x00);
	if (ret)
		goto out;
	/* The official driver stores the selected mode in two packed main0d bits
	 * per transmitter.  Original/bypass is zero for both ports, but the
	 * field itself is port-specific: bits 3:2 for port 1 and 5:4 for port 2.
	 */
	ret = gc570d_splitter_update8(gc, 0x58, 0x0d,
				      0x03 << (port * 2), 0x00);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0xad, 0x81, 0x00);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0xa0, 0x30, 0x00);
	if (ret)
		goto out;
	/* The bridge EDID advertises PQ, so the official driver does not suppress HDR. */
	ret = gc570d_splitter_aux_update8(gc, port, 0xa3, 0x80, 0x00);
	if (ret)
		goto out;

	stage = "measure-formatted-link-clock";
	ret = gc570d_splitter_measure_channel_clock(gc, port, &pclk, &vclk,
					     &sample);
	if (ret)
		goto out;
	/* Exact four-band selection in the official driver.  Port 2 has a final
	 * override for reg87/reg8b, but the HDMI2 software decision still
	 * follows the 310001 threshold used later by the official driver.
	 */
	if (vclk < 150001) {
		band87 = 0x03;
		band89 = 0x80;
		band8b = 0x03;
	} else if (vclk < 310001) {
		band87 = 0x09;
		band89 = 0x21;
		band8b = 0x09;
	} else if (vclk < 375001) {
		band87 = 0x0d;
		band89 = 0x25;
		band8b = 0x0b;
	} else {
		band87 = 0x0e;
		band89 = 0x25;
		band8b = 0x0d;
	}
	if (port == 2) {
		band87 = vclk < 150001 ? 0x05 : 0x0d;
		band8b = vclk < 150001 ? 0x03 : 0x09;
	}
	stage = "apply-windows-link-band";
	ret = gc570d_splitter_aux_update8(gc, port, 0x84, 0x07,
					  vclk > 100000 ? 0x04 : 0x03);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0x88, 0x04,
					  vclk > 162000 ? 0x04 : 0x00);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0x87, 0x1f, band87);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0x89, 0xbf, band89);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0x8a, 0x0f, 0x00);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0x8b, 0x0f, band8b);
	if (ret)
		goto out;

	stage = "pulse-windows-analog-setup";
	/* The official driver waits 50 ms after link-band programming. */
	msleep(50);
	ret = gc570d_splitter_aux_analog_setup(gc, port);
	if (ret)
		goto out;

	stage = "verify-link-prefix";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x0d,
			       &main0d, &last_irq, &last_status);
	if (ret)
		goto out;
	for (i = 0; i < ARRAY_SIZE(verify_regs); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave,
				       verify_regs[i], &verify[i], &last_irq,
				       &last_status);
		if (ret)
			goto out;
	}
	dev_info(&gc->pdev->dev,
		 "VideoSplitter official-driver stable worker link prefix completed: port=%u input_color=%u depth_code=%u pclk=%u vclk=%u hdmi2=%s counter=%u/%u divider=%u divisor=%u main0d=%02x aux84=%02x aux87=%02x aux88=%02x aux89=%02x aux8a=%02x aux8b=%02x aux94=%02x auxa0=%02x auxa3=%02x auxad=%02x auxaf=%02x auxc0=%02x auxc1=%02x output_setup_deferred=yes scdc_deferred=yes\n",
		 port, input_color, depth_code, pclk, vclk,
		 vclk >= 310001 ? "yes" : "no",
		 sample.initial_counter, sample.counter_sum,
		 sample.divider, sample.divisor, main0d, verify[0], verify[1],
		 verify[2], verify[3], verify[4], verify[5], verify[6],
		 verify[7], verify[8], verify[9], verify[10], verify[11],
		 verify[12]);
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter official-driver stable worker link prefix failed at %s: %d aux03=%02x/%02x detail13=%02x detail98=%02x bank2_15=%02x irq=0x%08x status=0x%08x\n",
		stage, ret, aux03_1, aux03_2, detail13, detail98, bank2_15,
		last_irq, last_status);
	return ret;
}

static int gc570d_splitter_stable_worker_link_setup(struct gc570d_dev *gc)
{
	return gc570d_splitter_stable_worker_link_setup_port(gc, 2);
}

static ssize_t
gc570d_splitter_stable_worker_link_setup_write(struct file *file,
					       const char __user *buffer,
					       size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_stable_worker_link_setup(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations
gc570d_splitter_stable_worker_link_setup_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_stable_worker_link_setup_write,
	.llseek = noop_llseek,
};

static ssize_t
gc570d_splitter_stable_worker_port1_link_setup_write(struct file *file,
						       const char __user *buffer,
						       size_t count,
						       loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_stable_worker_link_setup_port(gc, 1);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations
gc570d_splitter_stable_worker_port1_link_setup_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_stable_worker_port1_link_setup_write,
	.llseek = noop_llseek,
};

/* The official driver transfers one SCDC byte at a time. The transmitter uses
 * 8-bit DDC address 0xa8, puts the byte count in reg2b, and uses reg30 as
 * its FIFO only for writes.  The command in reg2e is 0x01 for a write and
 * 0x00 for a read; both helpers keep the reg28 request gate cleared.
 */
static int gc570d_splitter_scdc_byte(struct gc570d_dev *gc, u8 port,
				     u8 offset, u8 *value, bool write,
				     u8 *ddc_status, u32 *last_irq,
				     u32 *last_status)
{
	const u8 slave = 0x68 + 2 * port;
	u8 aux03 = 0;
	int trigger_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave, 0x03,
			       &aux03, last_irq, last_status);
	if (ret)
		return ret;
	if (!(aux03 & BIT(0)))
		return -ENODATA;

	ret = gc570d_splitter_aux_update8(gc, port, 0x28, BIT(0), 0x00);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_write8(gc, port, 0x2e, 0x09);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_write8(gc, port, 0x29, 0xa8);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_write8(gc, port, 0x2a, offset);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_write8(gc, port, 0x2b, 0x01);
	if (ret)
		return ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x2c, 0x03, 0x00);
	if (ret)
		return ret;
	if (write) {
		ret = gc570d_splitter_aux_write8(gc, port, 0x30, *value);
		if (ret)
			return ret;
	}
	trigger_ret = gc570d_splitter_aux_write8(gc, port, 0x2e,
						  write ? 0x01 : 0x00);
	if (trigger_ret && trigger_ret != -EIO)
		return trigger_ret;
	ret = gc570d_splitter_aux_update8(gc, port, 0x28, BIT(0), 0x00);
	if (ret)
		return ret;

	/* The official driver ignores the trigger's outer-I2C result and
	 * accepts the normal transaction when reg2f bit 7 appears in its first
	 * 15-ms sample.  If that first sample misses, it takes a second sample,
	 * resets the DDC engine, and performs its exact two bounded recovery
	 * polls before reporting failure.
	 */
	msleep(15);
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave, 0x2f,
			       ddc_status, last_irq, last_status);
	if (ret)
		return ret;
	if (*ddc_status & BIT(7)) {
		if (!write)
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave,
					       0x30, value, last_irq, last_status);
		else
			ret = 0;
	} else {
		u8 recovery;
		unsigned int pass;
		unsigned int poll;

		msleep(15);
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave, 0x2f,
				       ddc_status, last_irq, last_status);
		if (ret)
			return ret;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave, 0x12,
				       &recovery, last_irq, last_status);
		if (ret)
			return ret;
		ret = gc570d_splitter_aux_update8(gc, port, 0x35,
						  BIT(4), BIT(4));
		if (ret)
			return ret;
		ret = gc570d_splitter_aux_update8(gc, port, 0x35,
						  BIT(4), 0x00);
		if (ret)
			return ret;
		ret = gc570d_splitter_aux_update8(gc, port, 0x28,
						  BIT(0), BIT(0));
		if (ret)
			return ret;
		ret = gc570d_splitter_aux_update8(gc, port, 0x28,
						  BIT(0), 0x00);
		if (ret)
			return ret;
		for (pass = 0; pass < 2; pass++) {
			ret = gc570d_splitter_aux_write8(gc, port, 0x2e, 0x0f);
			if (ret && ret != -EIO)
				return ret;
			for (poll = 0; poll < 200; poll++) {
				ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
						       slave, 0x2f, ddc_status,
						       last_irq, last_status);
				if (ret)
					return ret;
				if (*ddc_status & 0xb8)
					break;
				msleep(1);
			}
		}
		ret = trigger_ret ? trigger_ret : -EIO;
	}

	/* Both the official driver helpers clear the DDC request gate before returning. */
	trigger_ret = gc570d_splitter_aux_update8(gc, port, 0x28, BIT(0),
						   0x00);
	return ret ? ret : trigger_ret;
}

/*
 * Exact observable return contract of the official driver for HDMI 2.0:
 *
 * - publish source SCDC version 1 at offset 0x02 and read it back;
 * - publish TMDS_Config at offset 0x20;
 * - retry its read at most eleven times while downstream HPD is present;
 * - return the success state of the final DDC read.  A successful read whose
 *   low bits still mismatch is nevertheless a successful transaction after
 *   the eleventh attempt.  the official driver uses that return value to decide
 *   whether its legacy fallback is reached.
 *
 * The official driver ignores the result of each DDC write helper.  Keep that behavior:
 * only the read transaction controls the returned success flag.
 */
static bool gc570d_splitter_windows_scdc_hdmi2(struct gc570d_dev *gc,
						u8 port, u8 desired,
						u8 *readback, u8 *ddc_status,
						u8 *attempts, bool *retried,
						u32 *last_irq,
						u32 *last_status)
{
	u8 value = 0x01;
	bool read_ok = false;
	int ret;

	ret = gc570d_splitter_aux_update8(gc, port, 0x83, 0x08, 0x08);
	if (ret)
		return false;
	ret = gc570d_splitter_aux_update8(gc, port, 0xc0, 0x46, 0x46);
	if (ret)
		return false;
	msleep(50);
	ret = gc570d_splitter_aux_update8(gc, port, 0x3a, 0x03, 0x00);
	if (ret)
		return false;

	(void)gc570d_splitter_scdc_byte(gc, port, 0x02, &value, true,
					ddc_status, last_irq, last_status);
	value = 0;
	(void)gc570d_splitter_scdc_byte(gc, port, 0x02, &value, false,
					ddc_status, last_irq, last_status);

	value = desired;
	(void)gc570d_splitter_scdc_byte(gc, port, 0x20, &value, true,
					ddc_status, last_irq, last_status);

	for (*attempts = 1; *attempts <= 11; (*attempts)++) {
		value = 0;
		ret = gc570d_splitter_scdc_byte(gc, port, 0x20, &value, false,
						 ddc_status, last_irq, last_status);
		read_ok = !ret;
		if (!read_ok)
			*retried = true;
		else if ((value & 0x03) == desired)
			break;
		else
			*retried = true;
	}
	if (*attempts > 11)
		*attempts = 11;

	*readback = value;
	return read_ok;
}

/* Exact HDMI 1.4 branch of the official driver.  The function deliberately keeps
 * its the official driver return value false in this branch: the official driver uses that
 * value to reach the sub-340-MHz scrambling-off fallback after output lock.
 * The downstream SCDC write/read is still issued, but its result is only
 * useful as a diagnostic and never blocks legacy-TMDS output.
 */
static void gc570d_splitter_windows_scdc_hdmi14(struct gc570d_dev *gc,
						 u8 port, u8 *readback,
						 u8 *ddc_status, u8 *attempts,
						 bool *read_ok, u32 *last_irq,
						 u32 *last_status)
{
	u8 value = 0;
	int ret;

	*attempts = 1;
	*read_ok = false;

	ret = gc570d_splitter_aux_update8(gc, port, 0x83, 0x08, 0x00);
	if (ret)
		return;
	ret = gc570d_splitter_aux_update8(gc, port, 0xc0, 0x46, 0x00);
	if (ret)
		return;
	msleep(50);
	ret = gc570d_splitter_aux_update8(gc, port, 0x3a, 0x03, 0x00);
	if (ret)
		return;

	(void)gc570d_splitter_scdc_byte(gc, port, 0x20, &value, true,
					ddc_status, last_irq, last_status);
	value = 0;
	ret = gc570d_splitter_scdc_byte(gc, port, 0x20, &value, false,
					 ddc_status, last_irq, last_status);
	*readback = value;
	*read_ok = !ret;
}

/* The official driver marks every VCLK >= 310001 as HDMI 2.0,
 * including its >=375001 top band, so those links negotiate SCDC 0x20=03.
 * Lower clocks take the HDMI 1.4/no-scrambling branch above.
 */
static int gc570d_splitter_stable_worker_output_setup_port(struct gc570d_dev *gc,
							    u8 port)
{
	static const u8 verify_regs[] = {
		0x03, 0x18, 0x1a, 0x28, 0x2f, 0x83, 0x85,
		0x88, 0xc0, 0xc1, 0xc2, 0xc3,
	};
	u8 verify[ARRAY_SIZE(verify_regs)] = { 0 };
	u8 aux03_1 = 0, aux03_2 = 0, detail13 = 0, detail98 = 0;
	u8 bank2_15 = 0, input_color = 0, depth_code = 0;
	u8 ddc_status = 0, scdc_value = 0;
	u8 scdc_attempts = 0;
	u8 slave = 0x68 + 2 * port;
	bool scdc_retried = false;
	bool second_scdc_ok = false;
	bool legacy_scdc_read_ok = false;
	bool hdmi2;
	u32 pclk = 0, vclk = 0, last_irq = 0, last_status = 0;
	struct gc570d_splitter_clock_sample sample = { 0 };
	const char *stage = "read-live-state";
	size_t i;
	int cleanup_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a, 0x03,
			       &aux03_1, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6c, 0x03,
			       &aux03_2, &last_irq, &last_status);
	if (ret)
		goto out;
	/*
	 * The official driver runs immediately after the analog-reset pulse, while
	 * port 2 can legitimately report 0x9b instead of 0x9f.  the official driver checks
	 * the full low-nibble lock later in this routine; only presence and the
	 * stable indication are prerequisites at this boundary.
	 */
	stage = "validate-selected-port-after-link-prefix";
	if (((port == 1 ? aux03_1 : aux03_2) & 0x81) != 0x81) {
		ret = -ENODATA;
		goto out;
	}

	stage = "read-receiver-format";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
			       &detail13, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x98,
			       &detail98, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x02);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x15,
			       &bank2_15, &last_irq, &last_status);
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		goto out;
	if (cleanup_ret) {
		ret = cleanup_ret;
		goto out;
	}
	stage = "validate-live-hdmi-format";
	if ((detail13 & 0x13) != 0x13) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	depth_code = (detail98 >> 4) & 0x03;
	input_color = (bank2_15 >> 5) & 0x03;

	stage = "measure-link-clock";
	ret = gc570d_splitter_measure_channel_clock(gc, port, &pclk, &vclk,
					     &sample);
	if (ret)
		goto out;
	hdmi2 = vclk >= 310001;

	/* The official driver prefix and mode-selected the official driver state. */
	stage = "program-output-prefix";
	ret = gc570d_splitter_aux_update8(gc, port, 0x18, 0x0c, 0x0c);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_write8(gc, port, 0x85, 0x19);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0x1a, 0x0b, 0x0b);
	if (ret)
		goto out;
	stage = "first-windows-scdc-negotiation";
	scdc_value = 0;
	if (hdmi2)
		(void)gc570d_splitter_windows_scdc_hdmi2(gc, port, 0x03,
							 &scdc_value, &ddc_status,
							 &scdc_attempts,
							 &scdc_retried,
							 &last_irq, &last_status);
	else
		gc570d_splitter_windows_scdc_hdmi14(gc, port, &scdc_value,
							 &ddc_status, &scdc_attempts,
							 &legacy_scdc_read_ok,
							 &last_irq, &last_status);
	/* The official driver adds a separate 100-ms settling delay after the first
	 * The official driver call.  This is in addition to the 50-ms delay inside
	 * the SCDC helper itself and precedes every transmitter-enable write.
	 */
	msleep(100);

	stage = "enable-output";
	ret = gc570d_splitter_aux_update8(gc, port, 0xc1, 0x08, 0x00);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0xc1, 0x08, 0x08);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0xc2, 0x80, 0x80);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0xc3, 0x30, 0x30);
	if (ret)
		goto out;
	ret = gc570d_splitter_aux_update8(gc, port, 0x88, 0x03, 0x00);
	if (ret)
		goto out;
	msleep(100);

	/* The official driver repeats the SCDC decision after enabling the output. */
	stage = "second-windows-scdc-negotiation";
	scdc_value = 0;
	if (hdmi2)
		second_scdc_ok =
			gc570d_splitter_windows_scdc_hdmi2(gc, port, 0x03,
							      &scdc_value,
							      &ddc_status,
							      &scdc_attempts,
							      &scdc_retried,
							      &last_irq,
							      &last_status);
	else
		gc570d_splitter_windows_scdc_hdmi14(gc, port, &scdc_value,
							 &ddc_status, &scdc_attempts,
							 &legacy_scdc_read_ok,
							 &last_irq, &last_status);

	stage = "verify-output-clock";
	for (i = 0; i < ARRAY_SIZE(verify_regs); i++) {
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave,
				       verify_regs[i], &verify[i], &last_irq,
				       &last_status);
		if (ret)
			goto out;
	}
	/*
	 * The official driver gives an output with VCLK-not-stable one additional
	 * analog reset, forces auxc1[7:4]=8, waits 50 ms, and samples aux03
	 * again before deciding that the link is unavailable.
	 */
	if (!(verify[0] & BIT(3))) {
		stage = "recover-unstable-output-clock";
		ret = gc570d_splitter_aux_analog_setup(gc, port);
		if (ret)
			goto out;
		ret = gc570d_splitter_aux_update8(gc, port, 0xc1, 0xf0, 0x80);
		if (ret)
			goto out;
		msleep(50);
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave, 0x03,
				       &verify[0], &last_irq, &last_status);
		if (ret)
			goto out;
	}
	if ((verify[0] & 0x0f) != 0x0f) {
		ret = -EAGAIN;
		goto out;
	}
	/* A successful second the official driver returns before every fallback.  When
	 * it fails, the official driver only clears scrambling below 340 MHz; at or above
	 * 340 MHz it returns the port to Video-Stable state for a later retry.
	 */
	if (!second_scdc_ok) {
		if (vclk < 340000) {
			ret = gc570d_splitter_aux_update8(gc, port, 0xc0, 0x02,
						  0x00);
			if (ret)
				goto out;
		} else {
			stage = "retry-high-speed-scdc";
			ret = -EAGAIN;
			goto out;
		}
	}
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave, 0xc0,
			       &verify[8], &last_irq, &last_status);
	if (ret)
		goto out;

	/* The official driver acknowledges every remaining port IRQ after the
	 * stable-output routine returns, then the worker exits.
	 */
	stage = "acknowledge-worker-irqs";
	ret = gc570d_splitter_aux_write8(gc, port, 0x12, 0xff);
	if (ret)
		goto out;
	/* The official driver does not unmute the completed RXHDCP_OFF output in
	 * this invocation.  It leaves the per-port worker in software state 4;
	 * the next 20-ms the official driver pass clears aux91[4] and auxc1[0].
	 */
	if (READ_ONCE(gc->splitter_receiver_hdcp_off))
		gc->splitter_worker_state4_mask |= BIT(port);
	dev_info(&gc->pdev->dev,
		 "VideoSplitter official-driver stable worker output completed: port=%u input_color=%u depth_code=%u pclk=%u vclk=%u hdmi2=%s scdc_path=%s scdc20=%02x ddc_status=%02x scdc_attempts=%u scdc_retry=%s second_scdc_ok=%s legacy_scdc_read_ok=%s aux03=%02x aux18=%02x aux1a=%02x aux28=%02x aux2f=%02x aux83=%02x aux85=%02x aux88=%02x auxc0=%02x auxc1=%02x auxc2=%02x auxc3=%02x aux12_written=ff ratio=%s scrambling=%s worker_next=%s\n",
		 port, input_color, depth_code, pclk, vclk,
		 hdmi2 ? "yes" : "no",
		 hdmi2 ? "hdmi2" : "hdmi14",
		 scdc_value, ddc_status, scdc_attempts,
		 scdc_retried ? "yes" : "no", second_scdc_ok ? "yes" : "no",
		 legacy_scdc_read_ok ? "yes" : "no",
		 verify[0], verify[1],
		 verify[2], verify[3], verify[4], verify[5], verify[6],
		 verify[7], verify[8], verify[9], verify[10], verify[11],
		 hdmi2 ? "1/40" : "1/10",
		 hdmi2 ? (second_scdc_ok ? "on" : "fallback-off") : "off",
		 READ_ONCE(gc->splitter_receiver_hdcp_off) ? "state4" : "hdcp");
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter official-driver stable worker output failed at %s: %d aux03=%02x/%02x detail13=%02x detail98=%02x bank2_15=%02x pclk=%u vclk=%u scdc20=%02x ddc_status=%02x irq=0x%08x status=0x%08x\n",
		stage, ret, aux03_1, aux03_2, detail13, detail98, bank2_15,
		pclk, vclk, scdc_value, ddc_status, last_irq, last_status);
	return ret;
}

static int gc570d_splitter_stable_worker_output_setup(struct gc570d_dev *gc)
{
	return gc570d_splitter_stable_worker_output_setup_port(gc, 2);
}

static ssize_t
gc570d_splitter_stable_worker_output_setup_write(struct file *file,
						 const char __user *buffer,
						 size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_stable_worker_output_setup(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations
gc570d_splitter_stable_worker_output_setup_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_stable_worker_output_setup_write,
	.llseek = noop_llseek,
};

static ssize_t
gc570d_splitter_stable_worker_port1_output_setup_write(struct file *file,
						 const char __user *buffer,
						 size_t count,
						 loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;
	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_stable_worker_output_setup_port(gc, 1);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations
gc570d_splitter_stable_worker_port1_output_setup_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_stable_worker_port1_output_setup_write,
	.llseek = noop_llseek,
};

/* Implement the official driver's per-port software state 4.  It selects this
 * state after a stable output when RXHDCP is disabled, reaches it on the next
 * worker invocation, and releases the content path by
 * clearing aux91[4] and auxc1[0], in that order.
 */
static int
gc570d_splitter_stable_worker_state4_followup(struct gc570d_dev *gc,
					       u8 requested_mask)
{
	u8 aux91[2] = { 0 };
	u8 auxc1[2] = { 0 };
	u8 completed_mask = 0;
	u8 port;
	int ret;

	requested_mask &= BIT(1) | BIT(2);
	if (!requested_mask)
		return -ENODATA;

	for (port = 1; port < 3; port++) {
		u8 index = port - 1;
		u8 slave = 0x68 + 2 * port;
		u32 last_irq = 0;
		u32 last_status = 0;

		if (!(requested_mask & BIT(port)))
			continue;
		ret = gc570d_splitter_aux_update8(gc, port, 0x91,
						   BIT(4), 0x00);
		if (ret)
			goto out;
		ret = gc570d_splitter_aux_update8(gc, port, 0xc1,
						   BIT(0), 0x00);
		if (ret)
			goto out;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave,
				       0x91, &aux91[index], &last_irq,
				       &last_status);
		if (ret)
			goto out;
		ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, slave,
				       0xc1, &auxc1[index], &last_irq,
				       &last_status);
		if (ret)
			goto out;
		if ((aux91[index] & BIT(4)) || (auxc1[index] & BIT(0))) {
			ret = -EILSEQ;
			goto out;
		}
		completed_mask |= BIT(port);
	}

	gc->splitter_worker_state4_mask &= ~completed_mask;
	if (completed_mask & BIT(1)) {
		gc->video0_receiver_output_valid = false;
		(void)gc570d_it68051_video_output_on(gc);
	}
	dev_info(&gc->pdev->dev,
		 "VideoSplitter official-driver worker state-4 follow-up completed: requested=%02x completed=%02x aux91=%02x/%02x auxc1=%02x/%02x content_release=yes\n",
		 requested_mask, completed_mask, aux91[0], aux91[1],
		 auxc1[0], auxc1[1]);
	return 0;

out:
	gc->splitter_worker_state4_mask &= ~completed_mask;
	dev_err(&gc->pdev->dev,
		"VideoSplitter official-driver worker state-4 follow-up failed: port=%u requested=%02x completed=%02x aux91=%02x/%02x auxc1=%02x/%02x error=%d remaining=%02x\n",
		port, requested_mask, completed_mask, aux91[0], aux91[1],
		auxc1[0], auxc1[1], ret,
		gc->splitter_worker_state4_mask);
	return ret;
}

/* The official driver walks all four logical transmitters and invokes
 * The official driver only for ports 1 and 2, in that exact order.  Keep each
 * port's link prefix and output enable contiguous under capture_lock so a
 * newly latched main/channel IRQ cannot interleave the two the official driver workers.
 */
int
gc570d_splitter_stable_workers_windows_order(struct gc570d_dev *gc)
{
	u8 requested_mask;
	u8 completed_mask = 0;
	u8 p1_03 = 0;
	u32 last_irq = 0, last_status = 0;
	u8 port;
	int ret;

	/* Always include Port 1 (internal capture leg to IT68051) if present */
	if (!gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a, 0x03,
			      &p1_03, &last_irq, &last_status) &&
	    (p1_03 & 0x01)) {
		gc->splitter_video_stable_mask |= BIT(1);
	}

	requested_mask = gc->splitter_video_stable_mask & (BIT(1) | BIT(2));
	if (!gc->splitter_video_stable_pending || !requested_mask)
		return -ENODATA;

	for (port = 1; port < 3; port++) {
		if (!(requested_mask & BIT(port)))
			continue;

		if (!gc->splitter_pclk[port] || !gc->splitter_vclk[port]) {
			(void)gc570d_splitter_scdt_channel_setup(
				gc, port, &gc->splitter_pclk[port],
				&gc->splitter_vclk[port]);
		}

		ret = gc570d_splitter_stable_worker_link_setup_port(gc, port);
		if (ret)
			goto out;
		ret = gc570d_splitter_stable_worker_output_setup_port(gc, port);
		if (ret)
			goto out;
		completed_mask |= BIT(port);
	}

	gc->splitter_video_stable_pending = false;
	if (gc->splitter_worker_state4_mask) {
		(void)gc570d_splitter_stable_worker_state4_followup(
			gc, gc->splitter_worker_state4_mask);
	}
	dev_info(&gc->pdev->dev,
		 "VideoSplitter official-driver ordered stable workers completed: requested=%02x completed=%02x order=port1/port2 pending=no\n",
		 requested_mask, completed_mask);
	return 0;

out:
	dev_err(&gc->pdev->dev,
		"VideoSplitter official-driver ordered stable workers failed: port=%u requested=%02x completed=%02x error=%d pending=yes\n",
		port, requested_mask, completed_mask, ret);
	return ret;
}

static ssize_t
gc570d_splitter_stable_workers_windows_order_write(struct file *file,
						    const char __user *buffer,
						    size_t count,
						    loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_stable_workers_windows_order(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations
gc570d_splitter_stable_workers_windows_order_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_stable_workers_windows_order_write,
	.llseek = noop_llseek,
};

static bool gc570d_splitter_pump_transient_error(int ret)
{
	return ret == -ENODATA || ret == -EAGAIN || ret == -EALREADY ||
	       ret == -ENOLINK;
}

/* State 4 of the official driver state pump.  Preserve the single saved main05
 * snapshot and the official driver branch order: receiver, timers, the periodic port 1/2
 * workers, then RXHDCP_OFF maintenance.  The existing
 * handlers still validate their detailed W1C snapshots, so a level that
 * vanished between the saved read and a handler is a normal transient race.
 */
static int gc570d_splitter_windows_state_pump_once(struct gc570d_dev *gc)
{
	u8 main05 = 0;
	u8 worker_state4_mask = gc->splitter_worker_state4_mask;
	u32 last_irq = 0;
	u32 last_status = 0;
	u32 events = 0;
	int first_error = 0;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &main05, &last_irq, &last_status);
	if (ret)
		return ret;
	gc->splitter_pump_last_main05 = main05;

	if (main05 & BIT(4)) {
		ret = gc570d_splitter_source_power_event(gc, true);
		if (!ret)
			events++;
		else if (!gc570d_splitter_pump_transient_error(ret))
			first_error = ret;
	}

	if (main05 & 0x60) {
		ret = gc570d_splitter_main_timer_irq(gc);
		if (!ret)
			events++;
		else if (!first_error &&
			 !gc570d_splitter_pump_transient_error(ret))
			first_error = ret;
	}

	if (main05 & (BIT(1) | BIT(2))) {
		u8 irq10 = 0;

		/* Intercept downstream HPD/RxSense hotplugs before Video Stable drops them */
		if (main05 & BIT(2)) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6c, 0x10,
					       &irq10, &last_irq, &last_status);
			if (!ret && (irq10 & 0x03)) {
				ret = gc570d_splitter_windows_channel2_connect(gc, !copy_edid);
				if (!ret)
					events++;
				else if (!first_error &&
					 !gc570d_splitter_pump_transient_error(ret))
					first_error = ret;
			}
		}
		/* Also handle port 1 (internal IT68051) HPD/RxSense connect events */
		if (main05 & BIT(1)) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x6a, 0x10,
					       &irq10, &last_irq, &last_status);
			if (!ret && (irq10 & 0x03)) {
				ret = gc570d_splitter_channel1_irq_on(gc);
				if (!ret)
					events++;
				else if (!first_error &&
					 !gc570d_splitter_pump_transient_error(ret))
					first_error = ret;
			}
		}

		ret = gc570d_splitter_channel_video_stable_irq(gc);
		if (!ret)
			events++;
		else if (!first_error &&
			 !gc570d_splitter_pump_transient_error(ret))
			first_error = ret;
	}

	if (worker_state4_mask) {
		ret = gc570d_splitter_stable_worker_state4_followup(
			gc, worker_state4_mask);
		if (!ret)
			events++;
		else if (!first_error &&
			 !gc570d_splitter_pump_transient_error(ret))
			first_error = ret;
	}

	if (gc->splitter_video_stable_pending) {
		ret = gc570d_splitter_stable_workers_windows_order(gc);
		if (!ret)
			events++;
		else if (!first_error &&
			 !gc570d_splitter_pump_transient_error(ret))
			first_error = ret;
	}

	ret = gc570d_splitter_windows_receiver_hdcp_off_maintain(gc);
	if (ret && !first_error &&
	    !gc570d_splitter_pump_transient_error(ret))
		first_error = ret;

	gc->splitter_pump_events += events;
	return first_error;
}

static int gc570d_splitter_windows_state_pump_thread(void *data)
{
	struct gc570d_dev *gc = data;
	unsigned int idle_count = 0;
	unsigned int sleep_ms;
	int ret;

	dev_info(&gc->pdev->dev,
		 "VideoSplitter official-driver state pump started: interval_ms=20\n");
	while (!kthread_should_stop()) {
		mutex_lock(&gc->capture_lock);
		ret = gc570d_splitter_windows_state_pump_once(gc);
		gc->splitter_pump_cycles++;
		if (ret && ret != -EBUSY) {
			gc->splitter_pump_last_error = ret;
			dev_warn_ratelimited(&gc->pdev->dev,
				"VideoSplitter official-driver state pump pass failed: error=%d main05=%02x cycles=%u events=%u\n",
				ret, gc->splitter_pump_last_main05,
				gc->splitter_pump_cycles,
				gc->splitter_pump_events);
		}
		mutex_unlock(&gc->capture_lock);

		/*
		 * Back off when no hardware events are being processed.
		 * gc570d_splitter_windows_state_pump_once returns 0 only when
		 * at least one sub-handler succeeded; transient no-signal errors
		 * (-ENOLINK, -ENODATA, -EAGAIN) return non-zero with no events
		 * incremented.  Cap at 200 ms so a newly-plugged source is still
		 * detected promptly.
		 */
		if (ret == 0)
			idle_count = 0;
		else
			idle_count = min(idle_count + 1, 9u);
		sleep_ms = (idle_count < 5) ? 20 : 200;

		if (msleep_interruptible(sleep_ms) && kthread_should_stop())
			break;
	}
	dev_info(&gc->pdev->dev,
		 "VideoSplitter official-driver state pump stopped: cycles=%u events=%u last_main05=%02x last_error=%d\n",
		 gc->splitter_pump_cycles, gc->splitter_pump_events,
		 gc->splitter_pump_last_main05, gc->splitter_pump_last_error);
	return 0;
}

void gc570d_splitter_windows_state_pump_stop(struct gc570d_dev *gc)
{
	struct task_struct *thread;

	thread = xchg(&gc->splitter_pump_thread, NULL);
	if (thread)
		kthread_stop(thread);
}

int gc570d_splitter_windows_state_pump_start(struct gc570d_dev *gc)
{
	struct task_struct *thread;

	if (!READ_ONCE(gc->splitter_receiver_hdcp_off))
		return -EAGAIN;
	if (READ_ONCE(gc->splitter_pump_thread))
		return -EALREADY;

	gc->splitter_pump_last_main05 = 0;
	gc->splitter_pump_cycles = 0;
	gc->splitter_pump_events = 0;
	gc->splitter_pump_last_error = 0;
	thread = kthread_create(gc570d_splitter_windows_state_pump_thread, gc,
				"gc570d-splitter");
	if (IS_ERR(thread))
		return PTR_ERR(thread);
	WRITE_ONCE(gc->splitter_pump_thread, thread);
	wake_up_process(thread);
	return 0;
}

static ssize_t
gc570d_splitter_windows_state_pump_write(struct file *file,
					 const char __user *buffer,
					 size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;

	if (!requested) {
		gc570d_splitter_windows_state_pump_stop(gc);
		return count;
	}

	mutex_lock(&gc->capture_lock);
	ret = gc570d_splitter_windows_state_pump_start(gc);
	mutex_unlock(&gc->capture_lock);

	return ret ? ret : count;
}

const struct file_operations
gc570d_splitter_windows_state_pump_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_windows_state_pump_write,
	.llseek = noop_llseek,
};

static int gc570d_splitter_pump_status_show(struct seq_file *seq, void *unused)
{
	struct gc570d_dev *gc = seq->private;

	seq_printf(seq,
		   "running=%s cycles=%u events=%u last_main05=%02x last_error=%d auto_enabled=%s auto_phase=%u auto_ready=%s\n",
		   READ_ONCE(gc->splitter_pump_thread) ? "yes" : "no",
		   READ_ONCE(gc->splitter_pump_cycles),
		   READ_ONCE(gc->splitter_pump_events),
		   READ_ONCE(gc->splitter_pump_last_main05),
		   READ_ONCE(gc->splitter_pump_last_error),
		   auto_hdmi1 ? "yes" : "no",
		   READ_ONCE(gc->hdmi1_auto_phase),
		   READ_ONCE(gc->hdmi1_auto_ready) ? "yes" : "no");
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_splitter_pump_status);

static int gc570d_splitter_eq_start(struct gc570d_dev *gc)
{
	u8 main05 = 0;
	u8 detail05 = 0;
	u8 detail13 = 0;
	u8 detail14 = 0;
	u8 detail19 = 0;
	u8 final05 = 0;
	u8 final13 = 0;
	u8 final14 = 0;
	u8 final19 = 0;
	u8 final20 = 0;
	u8 final22 = 0;
	u8 final30 = 0;
	u8 final54 = 0;
	u8 final55 = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	const char *stage = "validate-state";
	int cleanup_ret;
	int ret;

	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x58, 0x05,
			       &main05, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x05,
			       &detail05, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
			       &detail13, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x14,
			       &detail14, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x19,
			       &detail19, &last_irq, &last_status);
	if (ret)
		goto out;
	if (!(main05 & 0x10) || !(detail05 & 0x04) ||
	    (detail13 & 0x1d) != 0x1d || (detail14 & 0xc0) != 0xc0 ||
	    (detail19 & 0x30) != 0x30) {
		ret = -ENODATA;
		goto out;
	}

	/* State-2 high-speed branch of the official driver, followed by the exact
	 * The official driver HDMI 2.0 EQ-start register sequence.
	 */
	stage = "arm-eq-event";
	ret = gc570d_splitter_update8(gc, 0x70, 0x53, 0x20, 0x00);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x05, 0x20, 0x20);
	if (ret)
		goto out;

	stage = "eq-bank3-preamble";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x20, 0x80, 0x00);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x22, 0x00);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		goto out;

	stage = "eq-trigger";
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x07, 0xff);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x23, 0xb0);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x23, 0xa0);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x3b, 0x07, 0x03);
	if (ret)
		goto out;

	stage = "eq-bank3-table";
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x03);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x26, 0x00);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x27, 0x1f);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x28, 0x1f);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x29, 0x1f);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x2d, 0x07, 0x00);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x30, 0x80);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x31, 0xb0);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x32, 0x43);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x33, 0x47);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x34, 0x4b);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x35, 0x53);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x36, 0xc0, 0x00);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x37, 0x0b);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x38, 0xf2);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x39, 0x0d);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x4a, 0x80, 0x00);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x4b, 0x80, 0x00);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x54, 0x80, 0x80);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x54, 0x38, 0x38);
	if (ret)
		goto out;
	ret = gc570d_i2c_write8(gc, &gc570d_splitter_bus, 0x70, 0x55, 0x40);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x22, 0x04, 0x04);
	if (ret)
		goto out;

	stage = "verify";
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x20,
			       &final20, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x22,
			       &final22, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x30,
			       &final30, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x54,
			       &final54, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x55,
			       &final55, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (ret)
		goto out;

	msleep(20);
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x05,
			       &final05, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x13,
			       &final13, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x14,
			       &final14, &last_irq, &last_status);
	if (ret)
		goto out;
	ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus, 0x70, 0x19,
			       &final19, &last_irq, &last_status);
	if (ret)
		goto out;

	dev_info(&gc->pdev->dev,
		 "VideoSplitter HDMI 2.0 EQ start completed: initial main05=%02x detail=%02x/%02x/%02x/%02x bank3 20=%02x 22=%02x 30=%02x 54=%02x 55=%02x final detail=%02x/%02x/%02x/%02x\n",
		 main05, detail05, detail13, detail14, detail19, final20,
		 final22, final30, final54, final55, final05, final13,
		 final14, final19);
	return 0;

out:
	cleanup_ret = gc570d_splitter_update8(gc, 0x70, 0x0f, 0x03, 0x00);
	if (!ret)
		ret = cleanup_ret;
	dev_err(&gc->pdev->dev,
		"VideoSplitter HDMI 2.0 EQ start failed at %s: %d main05=%02x detail=%02x/%02x/%02x/%02x\n",
		stage, ret, main05, detail05, detail13, detail14, detail19);
	return ret;
}

static ssize_t gc570d_splitter_eq_start_write(struct file *file,
					       const char __user *buffer,
					       size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool requested;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	mutex_lock(&gc->capture_lock);
	if (gc570d_subsystem_busy(gc))
		ret = -EBUSY;
	else
		ret = gc570d_splitter_eq_start(gc);
	mutex_unlock(&gc->capture_lock);
	return ret ? ret : count;
}

const struct file_operations gc570d_splitter_eq_start_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_splitter_eq_start_write,
	.llseek = noop_llseek,
};

static int gc570d_splitter_aux_status_show(struct seq_file *s, void *unused)
{
	static const u8 registers[] = {
		0x00, 0x01, 0x02, 0x03, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13,
		0x14, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x2e, 0x2f,
		0x34, 0x35, 0x3a, 0x42, 0x84, 0x86, 0x87, 0x88,
		0x89, 0x8a, 0x8b, 0x93, 0x94, 0xaf, 0xc0, 0xc1, 0xc3, 0xfd,
	};
	struct gc570d_dev *gc = s->private;
	u8 values[ARRAY_SIZE(registers)];
	u32 last_irq = 0;
	u32 last_status = 0;
	u8 port;
	size_t i;
	int ret;

	for (port = 0; port < 4; port++) {
		memset(values, 0, sizeof(values));
		for (i = 0; i < ARRAY_SIZE(registers); i++) {
			ret = gc570d_i2c_read8(gc, &gc570d_splitter_bus,
					       0x68 + 2 * port, registers[i],
					       &values[i], &last_irq, &last_status);
			if (ret) {
				seq_printf(s,
					   "channel=%u slave8=0x%02x error=%d irq=0x%08x status=0x%08x\n",
					   port, 0x68 + 2 * port, ret,
					   last_irq, last_status);
				break;
			}
		}
		if (i != ARRAY_SIZE(registers))
			continue;
		seq_printf(s, "channel=%u slave8=0x%02x", port,
			   0x68 + 2 * port);
		for (i = 0; i < ARRAY_SIZE(registers); i++)
			seq_printf(s, " %02x=%02x", registers[i], values[i]);
		seq_printf(s, " irq=0x%08x status=0x%08x\n",
			   last_irq, last_status);
	}
	return 0;
}

static int gc570d_splitter_aux_status_open(struct inode *inode,
					    struct file *file)
{
	return single_open(file, gc570d_splitter_aux_status_show,
			   inode->i_private);
}

const struct file_operations gc570d_splitter_aux_status_fops = {
	.owner = THIS_MODULE,
	.open = gc570d_splitter_aux_status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int gc570d_it68051_status_show(struct seq_file *s, void *unused)
{
	static const u8 registers[] = {
		0x0f, 0x19, 0x98, 0x9d, 0x9e, 0xa4, 0xa5,
		0xb0, 0xb1, 0xb2, 0xc0, 0xc1,
	};
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	struct gc570d_dev *gc = s->private;
	struct gc570d_splitter_rx_timing rx_timing = { 0 };
	u8 values[ARRAY_SIZE(registers)] = { 0 };
	u32 last_irq = 0;
	u32 last_status = 0;
	u32 width;
	u32 height;
	size_t i;
	int ret;

	/*
	 * This first HDMI IN 1 diagnostic is intentionally read-only.  the official driver
	 * selects IT68051 page 0 through register 0x0f before reading these
	 * fields, but changing pages here would make the probe stateful.  Refuse
	 * to interpret the remaining addresses if another page is active.
	 */
	ret = gc570d_receiver_read8(gc, bus, registers[0], &values[0],
				   &last_irq, &last_status);
	if (ret) {
		seq_printf(s,
			   "receiver=IT68051 channel=0 error=%d irq=0x%08x status=0x%08x\n",
			   ret, last_irq, last_status);
		return 0;
	}

	seq_printf(s, "receiver=IT68051 channel=0 page_reg=0x%02x page=%u\n",
		   values[0], values[0] & 0x07);
	if (values[0] & 0x07) {
		seq_puts(s,
			 "decoded=no reason=current-page-is-not-zero (no register was modified)\n");
		return 0;
	}

	for (i = 1; i < ARRAY_SIZE(registers); i++) {
		ret = gc570d_receiver_read8(gc, bus, registers[i], &values[i],
					   &last_irq, &last_status);
		if (ret) {
			seq_printf(s,
				   "register=0x%02x error=%d irq=0x%08x status=0x%08x\n",
				   registers[i], ret, last_irq, last_status);
			return 0;
		}
	}

	/* CVDecITE68051::GetVideoFormat() uses these page-0 fields. */
	width = values[3] | ((values[4] & 0x3f) << 8);
	height = values[5] | ((values[6] & 0x3f) << 8);
	seq_printf(s,
		   "lock=%s width=%u height=%u interlaced=%s audio_ready_bit=%u\n",
		   values[1] & BIT(7) ? "yes" : "no", width, height,
		   values[2] & BIT(1) ? "yes" : "no", values[8] >> 7);
	seq_printf(s,
		   "raw 19=%02x 98=%02x 9d=%02x 9e=%02x a4=%02x a5=%02x b0=%02x b1=%02x b2=%02x c0=%02x c1=%02x irq=0x%08x status=0x%08x\n",
		   values[1], values[2], values[3], values[4], values[5],
		   values[6], values[7], values[8], values[9], values[10],
		   values[11], last_irq, last_status);

	/*
	 * Geometry alone cannot distinguish the PS5 menu's 1440p60 timing from
	 * a game actively driving 1440p120.  Reuse the read-only the official driver timing
	 * measurement already used by the stable worker.  input_color only
	 * affects its reported active width for YCbCr420, never the calculated
	 * frame rate; IT68051 above remains authoritative for geometry.
	 */
	ret = gc570d_splitter_measure_rx_timing(gc, 0, &rx_timing);
	if (!ret)
		seq_printf(s,
			   "source_pixel_clock_khz=%u source_htotal=%u source_vtotal=%u source_frame_rate=%u\n",
			   rx_timing.pixel_clock, rx_timing.htotal,
			   rx_timing.vtotal, rx_timing.frame_rate);
	else
		seq_printf(s, "source_timing_error=%d\n", ret);

	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_it68051_status);

static unsigned int gc570d_it68051_audio_rate(u8 b5, u8 b6)
{
	u8 code = (b5 & 0x0f) | ((b6 >> 2) & 0x30);

	switch (code) {
	case 0x03:
		return 32000;
	case 0x00:
		return 44100;
	case 0x02:
		return 48000;
	case 0x08:
		return 88200;
	case 0x0a:
		return 96000;
	case 0x0c:
		return 176400;
	case 0x0e:
		return 192000;
	default:
		return 0;
	}
}

static unsigned int gc570d_it68051_audio_channels(u8 b1)
{
	u8 mask = b1 & 0x3f;

	/*
	 * The recovered vendor routine derives the channel count from the
	 * complete low six-bit layout mask as 2 * fls(mask).  Layout 0
	 * (mask 0x00) does not fit that formula; it is the value observed for
	 * FL/FR stereo on this hardware, so map it to 2 channels explicitly.
	 * Do not guess at partial masks outside the vendor mapping.
	 */
	if (!mask)
		return 2;
	return 2 * fls(mask);
}

static int gc570d_it68051_audio_status_show(struct seq_file *s, void *unused)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	struct gc570d_dev *gc = s->private;
	static const u8 page0_regs[] = {
		0x19, 0x10, 0x81, 0x8a, 0x8c, 0xb0, 0xb1, 0xb2, 0xb5, 0xb6,
	};
	u8 page0[ARRAY_SIZE(page0_regs)] = { 0 };
	u8 infoframe[8] = { 0 };
	u8 c7 = 0;
	u32 last_irq = 0;
	u32 last_status = 0;
	unsigned int channels;
	unsigned int rate;
	unsigned int i;
	int restore_ret;
	int ret;

	mutex_lock(&gc->capture_lock);

	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore;
	for (i = 0; i < ARRAY_SIZE(page0_regs); i++) {
		ret = gc570d_receiver_read8(gc, bus, page0_regs[i], &page0[i],
					   &last_irq, &last_status);
		if (ret)
			goto out_restore;
	}
	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore;
	ret = gc570d_receiver_read8(gc, bus, 0xc7, &c7, &last_irq,
				    &last_status);
	if (ret)
		goto out_restore;
	ret = gc570d_it68051_select_page(gc, 2);
	if (ret)
		goto out_restore;
	ret = gc570d_receiver_read8(gc, bus, 0x43, &infoframe[0],
				    &last_irq, &last_status);
	if (!ret)
		ret = gc570d_receiver_read8(gc, bus, 0x4a, &infoframe[1],
					    &last_irq, &last_status);
	for (i = 0; !ret && i < 6; i++)
		ret = gc570d_receiver_read8(gc, bus, 0x44 + i,
					    &infoframe[2 + i], &last_irq,
					    &last_status);

out_restore:
	restore_ret = gc570d_it68051_select_page(gc, 0);
	if (!ret)
		ret = restore_ret;

	mutex_unlock(&gc->capture_lock);

	channels = gc570d_it68051_audio_channels(page0[6]);
	rate = gc570d_it68051_audio_rate(page0[8], page0[9]);
	seq_printf(s,
		   "receiver=IT68051 channel=0 error=%d scdt=%s ready=%s lpcm=%s hbr=%s dsd=%s channels=%u rate=%u output=%s\n",
		   ret, page0[0] & BIT(7) ? "yes" : "no",
		   (page0[0] & BIT(7)) && (page0[6] & BIT(7)) ? "yes" : "no",
		   !(page0[5] & GENMASK(7, 4)) ? "yes" : "no",
		   page0[5] & BIT(6) ? "yes" : "no",
		   page0[5] & BIT(7) ? "yes" : "no", channels, rate,
		   c7 == 0 ? "enabled" : "muted");
	seq_printf(s,
		   "raw p0 19=%02x 10=%02x 81=%02x 8a=%02x 8c=%02x b0=%02x b1=%02x b2=%02x b5=%02x b6=%02x p1_c7=%02x infoframe=%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x irq=0x%08x status=0x%08x\n",
		   page0[0], page0[1], page0[2], page0[3], page0[4],
		   page0[5], page0[6], page0[7], page0[8], page0[9], c7,
		   infoframe[0], infoframe[1], infoframe[2], infoframe[3],
		   infoframe[4], infoframe[5], infoframe[6], infoframe[7],
		   last_irq, last_status);
	return 0;
}
GC570D_DEFINE_SHOW_ATTRIBUTE(gc570d_it68051_audio_status);

int gc570d_it68051_audio_output_set(struct gc570d_dev *gc,
						    bool enable)
{
	const struct gc570d_i2c_bus *bus = &gc570d_receiver_buses[0];
	u8 prev_b0 = 0;
	u8 prev_b1 = 0;
	u8 prev_b5 = 0;
	u8 prev_b6 = 0;
	bool prev_ready = false;
	bool saw_ready = false;
	u8 reg19;
	u8 reg10;
	u8 regb0;
	u8 regb1;
	u8 regb5;
	u8 regb6;
	u8 reg8a;
	u8 old_c7 = 0;
	u32 last_irq;
	u32 last_status;
	unsigned int channels = 0;
	unsigned int rate = 0;
	unsigned int i;
	int restore_ret;
	int ret;

	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_page;
	ret = gc570d_receiver_read8(gc, bus, 0xc7, &old_c7, &last_irq,
				    &last_status);
	if (ret)
		goto out_restore_page;
	if (!enable) {
		ret = gc570d_receiver_write8(gc, bus, 0xc7, 0x7f);
		goto out_restore_page;
	}

	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_page;
#define IT68051_AUDIO_READ(_reg, _value) \
	do { \
		ret = gc570d_receiver_read8(gc, bus, (_reg), &(_value), \
					   &last_irq, &last_status); \
		if (ret) \
			goto out_restore_output; \
	} while (0)
	IT68051_AUDIO_READ(0x19, reg19);
	IT68051_AUDIO_READ(0x10, reg10);
	IT68051_AUDIO_READ(0xb1, regb1);
	if (!(reg19 & BIT(7))) {
		dev_dbg(&gc->pdev->dev,
			"IT68051 audio SCDT not present: reg19=%02x regb1=%02x\n",
			reg19, regb1);
		ret = -ENODATA;
		goto out_restore_output;
	}

	/*
	 * Clear any sticky W1C audio-change event (0x10[7]) once at entry
	 * with a direct write (not read-modify-write).  When switching
	 * between multichannel (5.1/7.1) and 2.0 stereo LPCM, 0xb0/0xb1/
	 * 0xb5/0xb6 may still be settling while 0xb1[7] is asserted; poll
	 * up to 300 ms until a supported format holds in a single pass or
	 * two consecutive ready passes report the same unsupported format.
	 * If 0xb1[7] is not asserted prior to RequestAudio (e.g. cold start
	 * before the 0x8c[4] pulse), continue to RequestAudio and let the
	 * post-pulse check decide.
	 *
	 * Worst case, when no audio-ready is ever asserted while SCDT is
	 * present, this holds capture_lock for about 400 ms (300 ms poll,
	 * 80 ms WaitForReady, 20 ms W1C retry) plus I2C time.
	 */
	if (reg10 & BIT(7)) {
		ret = gc570d_receiver_write8(gc, bus, 0x10, BIT(7));
		if (ret)
			goto out_restore_output;
	}
	for (i = 0; i < 15; i++) {
		IT68051_AUDIO_READ(0x19, reg19);
		IT68051_AUDIO_READ(0xb0, regb0);
		IT68051_AUDIO_READ(0xb1, regb1);
		IT68051_AUDIO_READ(0xb5, regb5);
		IT68051_AUDIO_READ(0xb6, regb6);
		if (!(reg19 & BIT(7)))
			break;
		channels = gc570d_it68051_audio_channels(regb1);
		rate = gc570d_it68051_audio_rate(regb5, regb6);
		if (regb1 & BIT(7)) {
			saw_ready = true;
			if (!(regb0 & GENMASK(7, 4)) &&
			    channels >= 2 && channels <= 8 &&
			    rate == GC570D_AUDIO_RATE_HZ)
				break;
			if (prev_ready && regb0 == prev_b0 &&
			    regb1 == prev_b1 && regb5 == prev_b5 &&
			    regb6 == prev_b6)
				break;
			prev_ready = true;
			prev_b0 = regb0;
			prev_b1 = regb1;
			prev_b5 = regb5;
			prev_b6 = regb6;
		} else {
			prev_ready = false;
		}
		msleep(20);
	}
	if (!(reg19 & BIT(7))) {
		dev_dbg(&gc->pdev->dev,
			"IT68051 audio SCDT not present: reg19=%02x regb1=%02x\n",
			reg19, regb1);
		ret = -ENODATA;
		goto out_restore_output;
	}
	if (saw_ready && (regb1 & BIT(7))) {
		if (regb0 & GENMASK(7, 4)) {
			dev_warn_ratelimited(&gc->pdev->dev,
					     "IT68051 non-LPCM audio stream rejected: regb0=%02x regb1=%02x\n",
					     regb0, regb1);
			ret = -EOPNOTSUPP;
			goto out_restore_output;
		}
		if (channels < 2 || channels > 8 ||
		    rate != GC570D_AUDIO_RATE_HZ) {
			dev_warn_ratelimited(&gc->pdev->dev,
					     "IT68051 unsupported LPCM audio format: channels=%u rate=%u (regb1=%02x regb5=%02x regb6=%02x)\n",
					     channels, rate, regb1, regb5,
					     regb6);
			ret = -ERANGE;
			goto out_restore_output;
		}
	}

	/* RequestAudio: disable forced audio and mute the receiver pins. */
	ret = gc570d_receiver_update8(gc, bus, 0x81, 0x40, 0x00);
	if (ret)
		goto out_restore_output;
	ret = gc570d_it68051_select_page(gc, 1);
	if (ret)
		goto out_restore_output;
	ret = gc570d_receiver_write8(gc, bus, 0xc7, 0x7f);
	if (ret)
		goto out_restore_output;

	/* WaitForReady: exact page-0 0x8c[4] pulse, 80-ms wait, and optional 20-ms W1C clear. */
	ret = gc570d_it68051_select_page(gc, 0);
	if (ret)
		goto out_restore_output;
	if ((reg10 & BIT(7)) || !(regb1 & BIT(7))) {
		ret = gc570d_receiver_update8(gc, bus, 0x22, 0x02, 0x02);
		if (!ret)
			ret = gc570d_receiver_update8(gc, bus, 0x22, 0x02,
						      0x00);
		if (!ret && (reg10 & BIT(7)))
			ret = gc570d_receiver_write8(gc, bus, 0x10, 0x80);
		if (ret)
			goto out_restore_output;
	}
	ret = gc570d_receiver_update8(gc, bus, 0x8c, 0x10, 0x10);
	if (!ret)
		ret = gc570d_receiver_update8(gc, bus, 0x8c, 0x10, 0x00);
	if (ret)
		goto out_restore_output;
	msleep(80);
	IT68051_AUDIO_READ(0x10, reg10);
	if (reg10 & BIT(7)) {
		ret = gc570d_receiver_write8(gc, bus, 0x10, BIT(7));
		if (ret)
			goto out_restore_output;
		msleep(20);
	}
	IT68051_AUDIO_READ(0x19, reg19);
	IT68051_AUDIO_READ(0xb1, regb1);
	if (!(reg19 & BIT(7)) || !(regb1 & BIT(7))) {
		dev_dbg(&gc->pdev->dev,
			"IT68051 audio not ready after reset: reg19=%02x regb1=%02x\n",
			reg19, regb1);
		ret = -ENODATA;
		goto out_restore_output;
	}
	IT68051_AUDIO_READ(0xb0, regb0);
	IT68051_AUDIO_READ(0xb5, regb5);
	IT68051_AUDIO_READ(0xb6, regb6);
	channels = gc570d_it68051_audio_channels(regb1);
	rate = gc570d_it68051_audio_rate(regb5, regb6);
	if (regb0 & GENMASK(7, 4)) {
		dev_warn_ratelimited(&gc->pdev->dev,
				     "IT68051 non-LPCM audio stream rejected: regb0=%02x regb1=%02x\n",
				     regb0, regb1);
		ret = -EOPNOTSUPP;
		goto out_restore_output;
	}
	/*
	 * Allow 2-channel stereo as well as multichannel (5.1/7.1, up to 8ch)
	 * 48 kHz LPCM so surround-sound passthrough to HDMI OUT keeps HDMI IN 1
	 * I2S SD0 stereo capture (front left/right) active instead of failing
	 * with -ERANGE.
	 */
	if (channels < 2 || channels > 8 || rate != GC570D_AUDIO_RATE_HZ) {
		dev_warn_ratelimited(&gc->pdev->dev,
				     "IT68051 unsupported LPCM audio format: channels=%u rate=%u (regb1=%02x regb5=%02x regb6=%02x)\n",
				     channels, rate, regb1, regb5, regb6);
		ret = -ERANGE;
		goto out_restore_output;
	}

	/* AudioOn: the official driver reset pulse and four reg8a rewrites. */
	ret = gc570d_receiver_update8(gc, bus, 0x22, 0x02, 0x02);
	if (!ret)
		ret = gc570d_receiver_update8(gc, bus, 0x22, 0x02, 0x00);
	if (ret)
		goto out_restore_output;
	IT68051_AUDIO_READ(0x8a, reg8a);
	for (i = 0; i < 4; i++) {
		ret = gc570d_receiver_write8(gc, bus, 0x8a, reg8a);
		if (ret)
			goto out_restore_output;
	}
	ret = gc570d_it68051_select_page(gc, 1);
	if (!ret)
		ret = gc570d_receiver_write8(gc, bus, 0xc7, 0x00);
	if (!ret) {
		WRITE_ONCE(gc->audio0_channels, (u8)channels);
		WRITE_ONCE(gc->audio0_rate, (u16)rate);
		dev_info(&gc->pdev->dev,
			 "IT68051 LPCM output enabled: channels=%u rate=%u p1_c7=00\n",
			 channels, rate);
	}
	goto out_restore_page;

out_restore_output:
	if (!gc570d_it68051_select_page(gc, 1))
		gc570d_receiver_write8(gc, bus, 0xc7, old_c7);
out_restore_page:
	restore_ret = gc570d_it68051_select_page(gc, 0);
	if (!ret)
		ret = restore_ret;
#undef IT68051_AUDIO_READ
	return ret;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Multicolor LED-class integration and hardware effects for the GC570D
 * enclosure lighting.
 */
#include "gc570d.h"

struct gc570d_led_init_entry {
	u8 address;
	u8 value;
};

struct gc570d_led_rgb {
	u8 red;
	u8 green;
	u8 blue;
};

/* The official driver mode 0xf3 palette, followed by the requested white breath. */
static const struct gc570d_led_rgb gc570d_led_colorful_palette[] = {
	{ 0xff, 0x00, 0x00 }, /* red */
	{ 0xff, 0xff, 0x00 }, /* yellow */
	{ 0x00, 0xff, 0x00 }, /* green */
	{ 0x00, 0xff, 0xff }, /* cyan */
	{ 0x00, 0x00, 0xff }, /* blue */
	{ 0xff, 0x00, 0xff }, /* magenta */
	{ 0xff, 0xff, 0xff }, /* requested final white */
};

/* Controller initialization used by the official driver before its bulk clears. */
static const struct gc570d_led_init_entry gc570d_led_controller_init[] = {
	{ 0xfd, 0x0b }, { 0x0a, 0x00 }, { 0x01, 0x08 },
	{ 0x0d, 0xe4 }, { 0x0e, 0x01 }, { 0x14, 0x55 },
	{ 0x15, 0x00 }, { 0x18, 0xaa }, { 0x19, 0xaa },
	{ 0x1a, 0xaa }, { 0x0f, 0xbf }, { 0xfd, 0x00 },
};

static int gc570d_led_write8(struct gc570d_dev *gc, u8 address, u8 value)
{
	return gc570d_i2c_write8(gc, &gc570d_led_bus,
				 GC570D_I2C_LED_ADDR8, address, value);
}

static int gc570d_led_shutdown(struct gc570d_dev *gc)
{
	int ret;

	ret = gc570d_led_write8(gc, 0xfd, 0x0b);
	if (!ret)
		ret = gc570d_led_write8(gc, 0x0a, 0x00);
	if (!ret) {
		gc->led_active = false;
		gc->led_initialized = false;
	}
	return ret;
}

/*
 * The bridge I2C write FIFO only accepts up to 32 bytes per transaction
 * (see the guard in gc570d_i2c_write_buf).  The LED controller's
 * frame-zero and matrix-page registers are contiguous and auto-increment
 * on each byte, so split the caller's buffer into 32-byte chunks.
 */
static int gc570d_led_bulk_write(struct gc570d_dev *gc, u8 address,
				  const u8 *values, unsigned int count)
{
	unsigned int offset = 0;

	while (offset < count) {
		unsigned int chunk = min(count - offset, 32U);
		int ret;

		ret = gc570d_i2c_write_buf(gc, &gc570d_led_bus,
					   GC570D_I2C_LED_ADDR8,
					   address + offset,
					   values + offset, (u8)chunk);
		if (ret)
			return ret;
		offset += chunk;
	}
	return 0;
}

static int gc570d_led_hw_init(struct gc570d_dev *gc)
{
	u8 values[0xb4];
	unsigned int i;
	int ret;

	if (gc->led_initialized)
		return 0;

	for (i = 0; i < ARRAY_SIZE(gc570d_led_controller_init); i++) {
		ret = gc570d_led_write8(gc,
					gc570d_led_controller_init[i].address,
					gc570d_led_controller_init[i].value);
		if (ret) {
			dev_err(&gc->pdev->dev,
				"LED FAIL: controller init register 0x%02x: %d\n",
				gc570d_led_controller_init[i].address, ret);
			goto out_shutdown;
		}
	}

	memset(values, 0x00, 0xb4);
	ret = gc570d_led_bulk_write(gc, 0x00, values, 0xb4);
	if (ret) {
		dev_err(&gc->pdev->dev,
			"LED FAIL: clear frame-zero PWM registers: %d\n", ret);
		goto out_shutdown;
	}

	ret = gc570d_led_write8(gc, 0xfd, 0x0d);
	if (!ret) {
		memset(values, 0x55, 0x24);
		ret = gc570d_led_bulk_write(gc, 0x00, values, 0x24);
	}
	if (ret) {
		dev_err(&gc->pdev->dev,
			"LED FAIL: configure matrix page: %d\n", ret);
		goto out_shutdown;
	}

	ret = gc570d_led_write8(gc, 0xfd, 0x0b);
	if (!ret)
		ret = gc570d_led_write8(gc, 0x0a, 0x00);
	if (!ret)
		ret = gc570d_led_write8(gc, 0xfd, 0x0b);
	if (!ret)
		ret = gc570d_led_write8(gc, 0x09, 0x00);
	if (!ret)
		ret = gc570d_led_write8(gc, 0x08, 0x00);
	if (!ret)
		ret = gc570d_led_write8(gc, 0xfd, 0x0b);
	if (!ret)
		ret = gc570d_led_write8(gc, 0x0a, 0x01);
	if (!ret)
		ret = gc570d_led_write8(gc, 0xfd, 0x00);
	if (ret) {
		dev_err(&gc->pdev->dev,
			"LED FAIL: official-driver enable sequence: %d\n", ret);
		goto out_shutdown;
	}

	memset(values, 0xff, 0x12);
	ret = gc570d_led_bulk_write(gc, 0x00, values, 0x12);
	if (ret) {
		dev_err(&gc->pdev->dev,
			"LED FAIL: enable matrix locations: %d\n", ret);
		goto out_shutdown;
	}

	gc->led_active = true;
	gc->led_initialized = true;
	dev_info(&gc->pdev->dev,
		 "LED PASS: official-driver controller initialization completed\n");
	return 0;

out_shutdown:
	gc570d_led_shutdown(gc);
	return ret;
}

static int gc570d_led_hw_set_rgb(struct gc570d_dev *gc, u8 red, u8 green,
				 u8 blue)
{
	static const u8 red_registers[] = { 0x2a, 0x29, 0x28, 0x27, 0x26 };
	static const u8 green_registers[] = { 0x36, 0x35, 0x34, 0x33, 0x32 };
	static const u8 blue_registers[] = { 0x42, 0x41, 0x40, 0x3f, 0x3e };
	unsigned int i;
	int ret;

	ret = gc570d_led_write8(gc, 0xfd, 0x00);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(red_registers); i++) {
		ret = gc570d_led_write8(gc, red_registers[i], red);
		if (!ret)
			ret = gc570d_led_write8(gc, green_registers[i], green);
		if (!ret)
			ret = gc570d_led_write8(gc, blue_registers[i], blue);
		if (ret)
			return ret;
	}

	return 0;
}

static int gc570d_led_static_blue(struct gc570d_dev *gc)
{
	int ret;

	ret = gc570d_led_hw_init(gc);
	if (!ret)
		ret = gc570d_led_hw_set_rgb(gc, 0x00, 0x00, 0xff);
	if (ret)
		return ret;

	dev_info(&gc->pdev->dev,
		 "LED PASS: official-driver mode 0x04 static blue enabled\n");
	return 0;
}

static struct gc570d_dev *gc570d_led_to_gc(struct led_classdev *led_cdev)
{
	struct led_classdev_mc *mcled_cdev = lcdev_to_mccdev(led_cdev);

	return container_of(mcled_cdev, struct gc570d_dev, led_mcdev);
}

void gc570d_led_breath_work(struct work_struct *work)
{
	struct gc570d_dev *gc =
		container_of(to_delayed_work(work), struct gc570d_dev, led_work);
	u8 red, green, blue;
	int ret;

	mutex_lock(&gc->led_lock);
	if ((!gc->led_breathing && !gc->led_colorful) || gc->led_removing ||
	    READ_ONCE(gc->suspended))
		goto out_unlock;

	if (gc->led_colorful) {
		const struct gc570d_led_rgb *color =
			&gc570d_led_colorful_palette[gc->led_palette_index];
		u8 brightness = gc->led_mcdev.led_cdev.brightness;

		red = color->red * brightness / GC570D_LED_MAX_BRIGHTNESS;
		green = color->green * brightness / GC570D_LED_MAX_BRIGHTNESS;
		blue = color->blue * brightness / GC570D_LED_MAX_BRIGHTNESS;
		red = red * gc->led_step / GC570D_LED_BREATH_STEPS;
		green = green * gc->led_step / GC570D_LED_BREATH_STEPS;
		blue = blue * gc->led_step / GC570D_LED_BREATH_STEPS;
	} else {
		red = gc->led_subleds[0].brightness * gc->led_step /
			GC570D_LED_BREATH_STEPS;
		green = gc->led_subleds[1].brightness * gc->led_step /
			GC570D_LED_BREATH_STEPS;
		blue = gc->led_subleds[2].brightness * gc->led_step /
			GC570D_LED_BREATH_STEPS;
	}

	ret = gc570d_led_hw_set_rgb(gc, red, green, blue);
	if (ret) {
		gc->led_breathing = false;
		gc->led_colorful = false;
		dev_err(&gc->pdev->dev,
			"LED FAIL: breathing PWM update at step %u: %d\n",
			gc->led_step, ret);
		goto out_unlock;
	}

	if (!gc->led_descending) {
		if (gc->led_step == GC570D_LED_BREATH_STEPS)
			gc->led_descending = true;
		else
			gc->led_step++;
	} else if (gc->led_step == 1) {
		gc->led_descending = false;
		if (gc->led_colorful)
			gc->led_palette_index =
				(gc->led_palette_index + 1) %
				ARRAY_SIZE(gc570d_led_colorful_palette);
	} else {
		gc->led_step--;
	}

	/* Route slow blocking I2C operations to system_unbound_wq unless suspended. */
	if (!READ_ONCE(gc->suspended))
		mod_delayed_work(system_unbound_wq, &gc->led_work,
				 msecs_to_jiffies(GC570D_LED_BREATH_DELAY_MS));

out_unlock:
	mutex_unlock(&gc->led_lock);
}

static int gc570d_led_set_blocking(struct led_classdev *led_cdev,
				   enum led_brightness brightness)
{
	struct gc570d_dev *gc = gc570d_led_to_gc(led_cdev);
	int ret = 0;

	/* FIX: Check the removing flag before attempting to cancel work to prevent unregister deadlocks */
	if (READ_ONCE(gc->led_removing))
		return -ENODEV;

	cancel_delayed_work_sync(&gc->led_work);
	
	mutex_lock(&gc->led_lock);
	gc->led_breathing = false;
	gc->led_colorful = false;

	if (gc->led_removing)
		goto out_unlock;

	ret = led_mc_calc_color_components(&gc->led_mcdev, brightness);
	if (!ret)
		ret = gc570d_led_hw_init(gc);
	if (!ret)
		ret = gc570d_led_hw_set_rgb(gc,
					   gc->led_subleds[0].brightness,
					   gc->led_subleds[1].brightness,
					   gc->led_subleds[2].brightness);

out_unlock:
	mutex_unlock(&gc->led_lock);
	return ret;
}

/*
 * Set a static RGB colour, cancelling any breathing/colorful animation.
 * Public entry point for the JSON-RPC control plane.
 */
int gc570d_led_set_rgb(struct gc570d_dev *gc, u8 red, u8 green, u8 blue)
{
	int ret;

	if (!gc)
		return -EINVAL;
	if (READ_ONCE(gc->led_removing))
		return -ENODEV;

	cancel_delayed_work_sync(&gc->led_work);

	mutex_lock(&gc->led_lock);
	if (gc->led_removing) {
		ret = -ENODEV;
		goto out_unlock;
	}

	gc->led_breathing = false;
	gc->led_colorful = false;
	gc->led_subleds[0].brightness = red;
	gc->led_subleds[1].brightness = green;
	gc->led_subleds[2].brightness = blue;
	gc->led_subleds[0].intensity = red;
	gc->led_subleds[1].intensity = green;
	gc->led_subleds[2].intensity = blue;
	if (red || green || blue)
		gc->led_mcdev.led_cdev.brightness = GC570D_LED_MAX_BRIGHTNESS;
	else
		gc->led_mcdev.led_cdev.brightness = LED_OFF;

	ret = gc570d_led_hw_init(gc);
	if (!ret)
		ret = gc570d_led_hw_set_rgb(gc, red, green, blue);

out_unlock:
	mutex_unlock(&gc->led_lock);
	return ret;
}

/*
 * Select an LED effect by name: "off", "static", "breathing", "colorful".
 * The legacy spellings accepted by the sysfs attribute ("breathe",
 * "colorful-breathing") and the hub's "rainbow" alias are handled too.
 */
int gc570d_led_set_effect(struct gc570d_dev *gc, const char *effect)
{
	bool breathing;
	bool colorful;
	bool off = false;
	int ret;

	if (!gc || !effect)
		return -EINVAL;

	if (strcmp(effect, "colorful") == 0 ||
	    strcmp(effect, "colorful-breathing") == 0 ||
	    strcmp(effect, "rainbow") == 0) {
		breathing = false;
		colorful = true;
	} else if (strcmp(effect, "breathing") == 0 ||
		   strcmp(effect, "breathe") == 0) {
		breathing = true;
		colorful = false;
	} else if (strcmp(effect, "static") == 0) {
		breathing = false;
		colorful = false;
	} else if (strcmp(effect, "off") == 0) {
		breathing = false;
		colorful = false;
		off = true;
	} else {
		return -EINVAL;
	}

	if (READ_ONCE(gc->led_removing))
		return -ENODEV;

	cancel_delayed_work_sync(&gc->led_work);

	mutex_lock(&gc->led_lock);
	if (gc->led_removing) {
		ret = -ENODEV;
		goto out_unlock;
	}

	if (off) {
		gc->led_breathing = false;
		gc->led_colorful = false;
		gc->led_mcdev.led_cdev.brightness = LED_OFF;
		ret = gc570d_led_shutdown(gc);
		goto out_unlock;
	}

	/*
	 * When switching from 'off' (or zero brightness) to an active effect,
	 * restore default brightness so breathing and colorful effects do not
	 * scale all color channels to zero.
	 */
	if (gc->led_mcdev.led_cdev.brightness == LED_OFF)
		gc->led_mcdev.led_cdev.brightness = GC570D_LED_MAX_BRIGHTNESS;

	if (breathing &&
	    gc->led_subleds[0].intensity == 0 &&
	    gc->led_subleds[1].intensity == 0 &&
	    gc->led_subleds[2].intensity == 0) {
		gc->led_subleds[2].intensity = GC570D_LED_WINDOWS_BLUE;
	}

	ret = led_mc_calc_color_components(&gc->led_mcdev,
					   gc->led_mcdev.led_cdev.brightness);
	if (!ret)
		ret = gc570d_led_hw_init(gc);
	if (ret)
		goto out_unlock;

	gc->led_breathing = breathing;
	gc->led_colorful = colorful;
	if (breathing || colorful) {
		gc->led_step = 1;
		gc->led_descending = false;
		gc->led_palette_index = 0;
		/* Route slow blocking I2C operations to a dedicated workqueue. */
		mod_delayed_work(system_unbound_wq, &gc->led_work, 0);
	} else {
		ret = gc570d_led_hw_set_rgb(gc,
					    gc->led_subleds[0].brightness,
					    gc->led_subleds[1].brightness,
					    gc->led_subleds[2].brightness);
	}

out_unlock:
	mutex_unlock(&gc->led_lock);
	return ret;
}

static ssize_t gc570d_led_effect_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct gc570d_dev *gc = gc570d_led_to_gc(led_cdev);
	bool breathing;
	bool colorful;

	mutex_lock(&gc->led_lock);
	breathing = gc->led_breathing;
	colorful = gc->led_colorful;
	mutex_unlock(&gc->led_lock);

	return sysfs_emit(buf, "%s\n",
			  colorful ? "colorful" :
			  breathing ? "breathing" : "static");
}

static ssize_t gc570d_led_effect_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct gc570d_dev *gc = gc570d_led_to_gc(led_cdev);
	char effect[24];
	size_t len = count;
	int ret;

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
			   buf[len - 1] == ' ' || buf[len - 1] == '\t'))
		len--;
	if (len == 0 || len >= sizeof(effect))
		return -EINVAL;
	memcpy(effect, buf, len);
	effect[len] = '\0';

	ret = gc570d_led_set_effect(gc, effect);
	return ret ? ret : (ssize_t)count;
}

static DEVICE_ATTR(effect, 0644, gc570d_led_effect_show,
		   gc570d_led_effect_store);

int gc570d_led_register(struct gc570d_dev *gc)
{
	struct led_classdev *led_cdev = &gc->led_mcdev.led_cdev;
	int ret;

	gc->led_subleds[0].color_index = LED_COLOR_ID_RED;
	gc->led_subleds[0].intensity = 0x00;
	gc->led_subleds[1].color_index = LED_COLOR_ID_GREEN;
	gc->led_subleds[1].intensity = 0x00;
	gc->led_subleds[2].color_index = LED_COLOR_ID_BLUE;
	gc->led_subleds[2].intensity = GC570D_LED_WINDOWS_BLUE;
	gc->led_mcdev.num_colors = ARRAY_SIZE(gc->led_subleds);
	gc->led_mcdev.subled_info = gc->led_subleds;

	led_cdev->name = GC570D_LED_NAME;
	led_cdev->brightness = GC570D_LED_MAX_BRIGHTNESS;
	led_cdev->max_brightness = GC570D_LED_MAX_BRIGHTNESS;
	led_cdev->color = LED_COLOR_ID_RGB;
	led_cdev->brightness_set_blocking = gc570d_led_set_blocking;

	ret = led_classdev_multicolor_register(&gc->pdev->dev,
					       &gc->led_mcdev);
	if (ret)
		return ret;
	gc->led_registered = true;
	ret = device_create_file(led_cdev->dev, &dev_attr_effect);
	if (ret) {
		gc->led_removing = true;
		led_classdev_multicolor_unregister(&gc->led_mcdev);
		gc->led_registered = false;
		gc->led_removing = false;
		return ret;
	}
	gc->led_effect_created = true;

	mutex_lock(&gc->led_lock);
	led_cdev->brightness = GC570D_LED_MAX_BRIGHTNESS;
	gc->led_subleds[0].intensity = 0x00;
	gc->led_subleds[1].intensity = 0x00;
	gc->led_subleds[2].intensity = GC570D_LED_WINDOWS_BLUE;
	ret = led_mc_calc_color_components(&gc->led_mcdev,
					   led_cdev->brightness);
	if (!ret)
		ret = gc570d_led_hw_init(gc);
	if (!ret) {
		gc->led_step = 1;
		gc->led_descending = false;
		gc->led_colorful = true;
		gc->led_palette_index = 0;
		/* FIX: Route slow blocking I2C operations to a dedicated system_unbound_wq */
		mod_delayed_work(system_unbound_wq, &gc->led_work, 0);
	}
	mutex_unlock(&gc->led_lock);

	if (ret) {
		gc->led_removing = true;
		device_remove_file(led_cdev->dev, &dev_attr_effect);
		gc->led_effect_created = false;
		led_classdev_multicolor_unregister(&gc->led_mcdev);
		gc->led_registered = false;
		gc->led_removing = false;
		return ret;
	}

	dev_info(&gc->pdev->dev,
		 "LED PASS: %s registered; colorful breathing plus white enabled\n",
		 GC570D_LED_NAME);
	return 0;
}

void gc570d_led_unregister(struct gc570d_dev *gc)
{
	int ret;

	/* FIX: Set removing flag early and WITHOUT holding the lock so callbacks can abort */
	WRITE_ONCE(gc->led_removing, true);
	
	cancel_delayed_work_sync(&gc->led_work);

	mutex_lock(&gc->led_lock);
	gc->led_breathing = false;
	gc->led_colorful = false;
	mutex_unlock(&gc->led_lock);
	

	if (gc->led_registered) {
		if (gc->led_effect_created) {
			device_remove_file(gc->led_mcdev.led_cdev.dev,
					   &dev_attr_effect);
			gc->led_effect_created = false;
		}
		led_classdev_multicolor_unregister(&gc->led_mcdev);
		gc->led_registered = false;
	}

	mutex_lock(&gc->led_lock);
	if (gc->led_active) {
		ret = gc570d_led_shutdown(gc);
		if (ret)
			dev_warn(&gc->pdev->dev,
				 "LED shutdown during remove failed: %d\n", ret);
	}
	mutex_unlock(&gc->led_lock);
}

static ssize_t gc570d_led_basic_write(struct file *file,
				      const char __user *buffer,
				      size_t count, loff_t *position)
{
	struct gc570d_dev *gc = file->private_data;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(buffer, count, &enable);
	if (ret)
		return ret;

	if (READ_ONCE(gc->led_removing))
		return -ENODEV;

	cancel_delayed_work_sync(&gc->led_work);
	mutex_lock(&gc->led_lock);
	if (gc->led_removing) {
		mutex_unlock(&gc->led_lock);
		return -ENODEV;
	}
	gc->led_breathing = false;
	gc->led_colorful = false;
	if (enable) {
		gc->led_mcdev.led_cdev.brightness = GC570D_LED_MAX_BRIGHTNESS;
		gc->led_subleds[0].intensity = 0x00;
		gc->led_subleds[1].intensity = 0x00;
		gc->led_subleds[2].intensity = GC570D_LED_MAX_BRIGHTNESS;
		led_mc_calc_color_components(&gc->led_mcdev,
					     GC570D_LED_MAX_BRIGHTNESS);
		ret = gc570d_led_static_blue(gc);
	} else {
		gc->led_mcdev.led_cdev.brightness = LED_OFF;
		ret = gc570d_led_shutdown(gc);
	}
	mutex_unlock(&gc->led_lock);
	if (ret) {
		if (!enable)
			dev_err(&gc->pdev->dev,
				"LED FAIL: software shutdown: %d\n", ret);
		return ret;
	}

	if (!enable)
		dev_info(&gc->pdev->dev, "LED PASS: software shutdown completed\n");
	return count;
}

const struct file_operations gc570d_led_basic_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gc570d_led_basic_write,
	.llseek = noop_llseek,
};

void gc570d_led_suspend(struct gc570d_dev *gc)
{
	cancel_delayed_work_sync(&gc->led_work);
	mutex_lock(&gc->led_lock);
	gc->led_initialized = false;
	gc->led_active = false;
	mutex_unlock(&gc->led_lock);
}

int gc570d_led_resume(struct gc570d_dev *gc)
{
	int ret = 0;

	mutex_lock(&gc->led_lock);
	gc->led_initialized = false;
	gc->led_active = false;

	if (gc->led_removing) {
		mutex_unlock(&gc->led_lock);
		return 0;
	}

	ret = gc570d_led_hw_init(gc);
	if (ret) {
		dev_err(&gc->pdev->dev, "LED resume hw init failed: %d\n", ret);
		mutex_unlock(&gc->led_lock);
		return ret;
	}

	if (gc->led_colorful || gc->led_breathing) {
		gc->led_step = 1;
		gc->led_descending = false;
		/* FIX: Route slow blocking I2C operations to a dedicated system_unbound_wq */
		mod_delayed_work(system_unbound_wq, &gc->led_work, 0);
	} else {
		ret = gc570d_led_hw_set_rgb(gc,
					    gc->led_subleds[0].brightness,
					    gc->led_subleds[1].brightness,
					    gc->led_subleds[2].brightness);
	}

	mutex_unlock(&gc->led_lock);
	return ret;
}
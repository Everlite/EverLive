// SPDX-License-Identifier: GPL-2.0-only
/*
 * open_gc573 - Native Linux driver for AVerMedia Live Gamer 4K GC573 & BOLT GC555
 *
 * Derived from the gc555 driver authored by ostrich (https://github.com/ostrich/gc555).
 * Adapted, extended for GC573 PCIe, and maintained by Everlite and contributors (2026).
 *
 * Licensed under the GNU General Public License version 2 (GPL-2.0-only).
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#include "gc573.h"

#define GC555_BRIDGE_BAR		0
#define GC555_INPUT_EDID_SIZE		256

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static ssize_t
input_edid_read(struct file *file, struct kobject *kobj,
		const struct bin_attribute *attr, char *buf, loff_t offset,
		size_t count)
#else
static ssize_t
input_edid_read(struct file *file, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t offset,
		size_t count)
#endif
{
	struct gc555_dev *gc555 = dev_get_drvdata(kobj_to_dev(kobj));
	u8 edid[GC555_INPUT_EDID_SIZE];
	int ret;

	ret = gc555_link_get_input_edid(gc555, edid, sizeof(edid));
	if (ret)
		return ret;
	if (offset >= sizeof(edid))
		return 0;
	count = min_t(size_t, count, sizeof(edid) - offset);
	memcpy(buf, edid + offset, count);

	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static const struct bin_attribute input_edid_attr = {
#else
static struct bin_attribute input_edid_attr = {
#endif
	.attr = {
		.name = "input_edid",
		.mode = 0444,
	},
	.size = GC555_INPUT_EDID_SIZE,
	.read = input_edid_read,
};


static ssize_t hpd_pulse_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct gc555_dev *gc555 = pci_get_drvdata(pdev);
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val) {
		dev_info(dev, "triggering manual HDMI HPD re-sync pulse\n");
		gc555_link_set_input_hpd(gc555, false);
		msleep(200);
		gc555_link_set_input_hpd(gc555, true);
	}

	return count;
}
static DEVICE_ATTR_WO(hpd_pulse);

static int gc555_quiesce_host_irq(struct gc555_dev *gc555)
{
	int ret;

	ret = gc555_bridge_set_host_irq_routing(gc555, false);
	gc555_dma_synchronize_irq(gc555);

	return ret;
}

static void
gc555_restore_host_irq_after_failed_quiesce(struct gc555_dev *gc555)
{
	int ret;

	ret = gc555_bridge_restore_host_irq_routing(gc555);
	if (ret)
		dev_err(gc555->dev,
			"failed to restore host IRQ routing after suspend abort: %d\n",
			ret);
}

static void gc555_quiesce_host_irq_for_teardown(struct gc555_dev *gc555)
{
	int ret;

	ret = gc555_quiesce_host_irq(gc555);
	if (ret)
		dev_warn(gc555->dev,
			 "failed to quiesce host IRQ routing: %d\n", ret);
}

static int gc555_probe(struct pci_dev *pdev,
		       const struct pci_device_id *id)
{
	struct gc555_dev *gc555;
	void __iomem *regs;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable PCI device\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "32-bit DMA is unavailable\n");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	regs = pcim_iomap_region(pdev, GC555_BRIDGE_BAR, "gc555");
	if (IS_ERR(regs))
		return dev_err_probe(&pdev->dev, PTR_ERR(regs),
				     "failed to map BAR0\n");
#else
	ret = pcim_iomap_regions(pdev, BIT(GC555_BRIDGE_BAR), "gc555");
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to map BAR0\n");
	regs = pcim_iomap_table(pdev)[GC555_BRIDGE_BAR];
#endif

	gc555 = devm_kzalloc(&pdev->dev, sizeof(*gc555), GFP_KERNEL);
	if (!gc555)
		return -ENOMEM;

	gc555->dev = &pdev->dev;
	gc555->pdev = pdev;
	pci_set_drvdata(pdev, gc555);
	pci_set_master(pdev);

	ret = gc555_bridge_init(gc555, regs,
				pci_resource_len(pdev, GC555_BRIDGE_BAR));
	if (ret)
		goto clear_master;

	ret = gc555_fpga_init(gc555);
	if (ret)
		goto cleanup_bridge;

	ret = gc555_link_init(gc555);
	if (ret)
		goto cleanup_fpga;

	ret = gc555_i2c_init(gc555);
	if (ret)
		goto cleanup_fpga;

	if (pdev->subsystem_device == GC555_PCI_SUBDEVICE_ID ||
	    pdev->subsystem_device == GC573_PCI_SUBDEVICE_ID) {
		ret = gc555_led_init(gc555);
		if (ret)
			dev_warn(gc555->dev,
				 "RGB lighting unavailable: %d\n", ret);
	}

	ret = gc555_it6664_init(gc555);
	if (ret)
		goto cleanup_led;

	ret = sysfs_create_bin_file(&pdev->dev.kobj, &input_edid_attr);
	if (ret)
		goto cleanup_it6664;

	ret = device_create_file(&pdev->dev, &dev_attr_hpd_pulse);
	if (ret)
		goto remove_input_edid;
	if (sysfs_chmod_file(&pdev->dev.kobj, &dev_attr_hpd_pulse.attr, 0660))
		dev_warn(&pdev->dev, "Could not set permissions for hpd_pulse to 0660\n");

	ret = gc555_it6805_init(gc555);
	if (ret)
		goto remove_hpd_pulse;

	ret = gc555_dma_init(gc555);
	if (ret)
		goto cleanup_it6805;

	ret = gc555_video_dma_init(gc555);
	if (ret)
		goto quiesce_dma;

	ret = gc555_audio_init(gc555);
	if (ret)
		goto quiesce_video_dma;

	ret = gc555_video_init(gc555);
	if (ret)
		goto quiesce_audio;

	ret = gc555_bridge_resume_complete(gc555);
	if (ret)
		goto quiesce_video;

	dev_info(&pdev->dev,
		 "hardware transport, HDMI chips, and capture endpoints initialized\n");
	return 0;

quiesce_video:
	gc555_quiesce_host_irq_for_teardown(gc555);
	gc555_it6805_suspend(gc555);
	gc555_video_cleanup(gc555);
cleanup_audio:
	gc555_audio_cleanup(gc555);
cleanup_video_dma:
	gc555_video_dma_cleanup(gc555);
cleanup_dma:
	gc555_dma_cleanup(gc555);
cleanup_it6805:
	gc555_it6805_cleanup(gc555);
remove_hpd_pulse:
	device_remove_file(&pdev->dev, &dev_attr_hpd_pulse);
remove_input_edid:
	sysfs_remove_bin_file(&pdev->dev.kobj, &input_edid_attr);
cleanup_it6664:
	gc555_it6664_cleanup(gc555);
cleanup_led:
	gc555_led_cleanup(gc555);
	gc555_i2c_cleanup(gc555);
cleanup_fpga:
	gc555_fpga_cleanup(gc555);
cleanup_bridge:
	gc555_bridge_cleanup(gc555);
clear_master:
	pci_clear_master(pdev);
	return ret;

quiesce_audio:
	gc555_quiesce_host_irq_for_teardown(gc555);
	gc555_it6805_suspend(gc555);
	goto cleanup_audio;
quiesce_video_dma:
	gc555_quiesce_host_irq_for_teardown(gc555);
	goto cleanup_video_dma;
quiesce_dma:
	gc555_quiesce_host_irq_for_teardown(gc555);
	goto cleanup_dma;
}

static void gc555_remove(struct pci_dev *pdev)
{
	struct gc555_dev *gc555 = pci_get_drvdata(pdev);

	/* Surprise removal has already made BAR0 unsafe to access. */
	if (!gc555_bridge_is_accessible(gc555)) {
		gc555_dma_mark_device_lost(gc555);
		gc555_bridge_mark_disconnected(gc555);
		gc555_dma_synchronize_irq(gc555);
	} else {
		gc555_quiesce_host_irq_for_teardown(gc555);
	}

	gc555_it6805_suspend(gc555);
	gc555_it6664_suspend(gc555);
	gc555_led_suspend(gc555);
	device_remove_file(&pdev->dev, &dev_attr_hpd_pulse);
	sysfs_remove_bin_file(&pdev->dev.kobj, &input_edid_attr);

	gc555_video_cleanup(gc555);
	gc555_audio_cleanup(gc555);
	gc555_video_dma_cleanup(gc555);
	gc555_dma_cleanup(gc555);
	gc555_it6805_cleanup(gc555);
	gc555_it6664_cleanup(gc555);
	gc555_led_cleanup(gc555);
	gc555_i2c_cleanup(gc555);
	gc555_fpga_cleanup(gc555);
	gc555_bridge_cleanup(gc555);
	pci_clear_master(pdev);
}

static int gc555_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct gc555_dev *gc555 = pci_get_drvdata(pdev);
	int ret;

	ret = gc555_quiesce_host_irq(gc555);
	if (ret) {
		gc555_restore_host_irq_after_failed_quiesce(gc555);
		return ret;
	}

	gc555_video_suspend(gc555);
	gc555_audio_suspend(gc555);
	gc555_it6805_suspend(gc555);
	gc555_it6664_suspend(gc555);
	gc555_led_suspend(gc555);
	gc555_bridge_suspend(gc555);

	return 0;
}

static int gc555_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct gc555_dev *gc555 = pci_get_drvdata(pdev);
	int ret;

	pci_set_master(pdev);
	ret = gc555_bridge_resume(gc555);
	if (ret)
		return ret;
	ret = gc555_link_init(gc555);
	if (ret)
		goto quiesce_bridge;
	ret = gc555_led_resume(gc555);
	if (ret)
		dev_warn(gc555->dev,
			 "RGB lighting resume failed: %d\n", ret);
	ret = gc555_it6664_resume(gc555);
	if (ret)
		goto quiesce_led;
	ret = gc555_it6805_resume(gc555);
	if (ret)
		goto quiesce_it6664;

	gc555_audio_resume(gc555);
	gc555_video_resume(gc555);
	ret = gc555_bridge_resume_complete(gc555);
	if (ret)
		goto quiesce_all_children;
	return 0;

quiesce_all_children:
	gc555_quiesce_host_irq_for_teardown(gc555);
	gc555_video_suspend(gc555);
	gc555_audio_suspend(gc555);
	gc555_it6805_suspend(gc555);
suspend_it6664:
	gc555_it6664_suspend(gc555);
suspend_led:
	gc555_led_suspend(gc555);
suspend_bridge:
	gc555_bridge_suspend(gc555);
	dev_err(dev, "resume sequence failed: %d\n", ret);
	return ret;

quiesce_it6664:
	gc555_quiesce_host_irq_for_teardown(gc555);
	goto suspend_it6664;
quiesce_led:
	gc555_quiesce_host_irq_for_teardown(gc555);
	goto suspend_led;
quiesce_bridge:
	gc555_quiesce_host_irq_for_teardown(gc555);
	goto suspend_bridge;
}

static DEFINE_SIMPLE_DEV_PM_OPS(gc555_pm_ops, gc555_suspend, gc555_resume);

static const struct pci_device_id gc555_pci_ids[] = {
	{ PCI_DEVICE_SUB(GC555_PCI_VENDOR_ID, GC555_PCI_DEVICE_ID,
			 GC555_PCI_SUBVENDOR_ID, GC555_PCI_SUBDEVICE_ID) },
	{ PCI_DEVICE_SUB(GC555_PCI_VENDOR_ID, GC555_PCI_DEVICE_ID,
			 GC555_PCI_SUBVENDOR_ID, GC573_PCI_SUBDEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, gc555_pci_ids);

static struct pci_driver gc555_pci_driver = {
	.name = "open_gc573",
	.id_table = gc555_pci_ids,
	.probe = gc555_probe,
	.remove = gc555_remove,
	.driver.pm = pm_sleep_ptr(&gc555_pm_ops),
};
module_pci_driver(gc555_pci_driver);

MODULE_DESCRIPTION("AVerMedia Live Gamer 4K GC573 / BOLT GC555 open driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.4.0");

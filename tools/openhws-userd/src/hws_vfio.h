/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hws_vfio.h — VFIO PCI plumbing for the HWS UHD userspace engine
 */

#ifndef HWS_VFIO_H
#define HWS_VFIO_H

#include "hws_compat.h"

#define HWS_VFIO_MAX_DMA_REGIONS	32

/* Upper bound for a capture-card BAR0 MMIO window. */
#define HWS_VFIO_BAR0_MAX_SIZE	(256ULL * 1024 * 1024)

struct hws_vfio_dma_region {
	void *vaddr;
	size_t size;
	uint64_t iova;
};

struct hws_vfio_ctx {
	int container_fd;
	int group_fd;
	int device_fd;
	int iommu_group;
	char pci_addr[64];

	/* BAR0 MMIO mapping */
	void *bar0;
	size_t bar0_size;
	uint64_t bar0_offset;

	/* Userspace DMA regions mapped into the IOMMU */
	struct hws_vfio_dma_region dma_regions[HWS_VFIO_MAX_DMA_REGIONS];
	unsigned int nr_dma_regions;

	/* Interrupts */
	int irq_efd;

	/*
	 * The DMA interrupt status register is shared by the video completion
	 * and audio period engines.  Both run in different threads in the
	 * streamer, so every read/acknowledge pair must be serialized or a W1C
	 * write can swallow the other engine's pending bit.
	 */
	mutex_t irq_lock;
};

static inline void hws_vfio_irq_lock(struct hws_vfio_ctx *ctx)
{
	mutex_lock(&ctx->irq_lock);
}

static inline void hws_vfio_irq_unlock(struct hws_vfio_ctx *ctx)
{
	mutex_unlock(&ctx->irq_lock);
}

/**
 * hws_vfio_init - Initialize VFIO container, group and device
 * @ctx: Context structure to fill
 * @pci_addr: PCI BDF address (e.g. "0000:0a:00.0" or NULL for auto-detect)
 *
 * Returns 0 on success, negative errno on failure.
 */
int hws_vfio_init(struct hws_vfio_ctx *ctx, const char *pci_addr);

/**
 * hws_vfio_validate_bar0_size - Validate a BAR0 size against sysfs
 * @region_size: Size reported by VFIO_DEVICE_GET_REGION_INFO
 * @sysfs_size: Size derived from /sys/bus/pci/devices/<bdf>/resource
 * @page_size: System page size
 * @max_size: Largest acceptable BAR0 size
 * @out_size: Receives the validated mappable length
 *
 * Returns 0 on success, or a negative errno if either size is zero,
 * unaligned, implausible, or VFIO claims more than sysfs advertises.
 */
int hws_vfio_validate_bar0_size(size_t region_size, size_t sysfs_size,
				size_t page_size, size_t max_size,
				size_t *out_size);

/**
 * hws_vfio_resolve - Find the card without opening it
 * @pci_addr: PCI BDF address, or NULL for auto-detect
 * @bdf: Receives the PCI BDF address
 * @len: Size of @bdf
 * @iommu_group: Optional; receives the IOMMU group or -1
 *
 * Returns 0 on success, negative errno on failure.
 */
int hws_vfio_resolve(const char *pci_addr, char *bdf, size_t len,
		       int *iommu_group);

/**
 * hws_vfio_cleanup - Unmap memory and close all VFIO descriptors
 * @ctx: Context structure
 */
void hws_vfio_cleanup(struct hws_vfio_ctx *ctx);

/**
 * hws_vfio_prepare_inherit - Drop DMA, IRQs and the BAR mapping, but keep
 * the VFIO fds open across exec. Closing the last device fd function-resets
 * the FPGA and the bitstream starts the red blink.
 */
void hws_vfio_prepare_inherit(struct hws_vfio_ctx *ctx);

/**
 * hws_vfio_keep_fds - Clear FD_CLOEXEC on the VFIO descriptors only
 * @ctx: Context structure
 *
 * Used for an early daemon handoff: the parent keeps using its DMA, IRQ and
 * BAR while the child inherits the VFIO context.
 */
void hws_vfio_keep_fds(struct hws_vfio_ctx *ctx);

/**
 * hws_vfio_release_irq - Disable the device IRQ and close its eventfd
 * @ctx: Context structure
 */
void hws_vfio_release_irq(struct hws_vfio_ctx *ctx);

/**
 * hws_vfio_unmap_bar - Unmap BAR0 but keep the VFIO descriptors open
 * @ctx: Context structure
 */
void hws_vfio_unmap_bar(struct hws_vfio_ctx *ctx);

/**
 * hws_vfio_map_dma - Map userspace virtual buffer into IOMMU for hardware DMA
 * @ctx: Context structure
 * @vaddr: Virtual address of memory buffer (must be page-aligned)
 * @size: Size of buffer (must be multiple of PAGE_SIZE)
 * @iova: Desired IOVA address (e.g. 0x10000000)
 *
 * Returns 0 on success, negative errno on failure.
 */
int hws_vfio_map_dma(struct hws_vfio_ctx *ctx, void *vaddr, size_t size, uint64_t iova);

/**
 * hws_vfio_unmap_dma - Unmap DMA buffer from IOMMU
 * @ctx: Context structure
 * @iova: IOVA address
 * @size: Size of mapped buffer
 */
int hws_vfio_unmap_dma(struct hws_vfio_ctx *ctx, uint64_t iova, size_t size);

/**
 * hws_vfio_setup_msi - Setup MSI/INTx interrupt delivery to userspace eventfd
 * @ctx: Context structure
 *
 * Returns 0 on success, negative errno on failure.
 */
int hws_vfio_setup_msi(struct hws_vfio_ctx *ctx);

/* True when @bdf is one of this engine's cards (PCI ID and subsystem 0007). */
bool hws_vfio_is_supported(const char *bdf);

#endif /* HWS_VFIO_H */

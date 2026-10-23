/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_vfio.h — VFIO PCI hardware abstraction layer for GC573 userspace driver
 */

#ifndef GC573_VFIO_H
#define GC573_VFIO_H

#include "gc573-user-compat.h"

struct gc573_vfio_ctx {
	int container_fd;
	int group_fd;
	int device_fd;
	int iommu_group;
	char pci_addr[64];

	/* BAR0 MMIO mapping */
	void *bar0;
	size_t bar0_size;
	uint64_t bar0_offset;

	/* Userspace DMA Ring Buffer */
	void *dma_vaddr;
	size_t dma_size;
	uint64_t dma_iova;

	/* Interrupts */
	int irq_efd;
};

/**
 * gc573_vfio_init - Initialize VFIO container, group and device for GC573
 * @ctx: Context structure to fill
 * @pci_addr: PCI BDF address (e.g. "0000:0a:00.0" or NULL for auto-detect)
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_vfio_init(struct gc573_vfio_ctx *ctx, const char *pci_addr);

/**
 * gc573_vfio_cleanup - Unmap memory and close all VFIO descriptors
 * @ctx: Context structure
 */
void gc573_vfio_cleanup(struct gc573_vfio_ctx *ctx);

/**
 * gc573_vfio_map_dma - Map userspace virtual buffer into IOMMU for hardware DMA
 * @ctx: Context structure
 * @vaddr: Virtual address of memory buffer (must be page-aligned)
 * @size: Size of buffer (must be multiple of PAGE_SIZE)
 * @iova: Desired IOVA address (e.g. 0x10000000)
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_vfio_map_dma(struct gc573_vfio_ctx *ctx, void *vaddr, size_t size, uint64_t iova);

/**
 * gc573_vfio_unmap_dma - Unmap DMA buffer from IOMMU
 * @ctx: Context structure
 * @iova: IOVA address
 * @size: Size of mapped buffer
 */
int gc573_vfio_unmap_dma(struct gc573_vfio_ctx *ctx, uint64_t iova, size_t size);

/**
 * gc573_vfio_setup_msi - Setup MSI/INTx interrupt delivery to userspace eventfd
 * @ctx: Context structure
 *
 * Returns 0 on success, negative errno on failure.
 */
int gc573_vfio_setup_msi(struct gc573_vfio_ctx *ctx);

#endif /* GC573_VFIO_H */

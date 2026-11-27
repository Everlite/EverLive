/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_vfio.c — VFIO PCI hardware abstraction layer for GC573 userspace driver
 */

#include "gc573_vfio.h"
#include "everlive_card.h"
#include <sys/resource.h>
#include <sys/stat.h>

/*
 * Card discovery lives in libeverlive-core.  Only descriptors matched as
 * "gc573" are accepted: 1461:0054 with subsystem 0x5700 is a GC570D and
 * must never be claimed by this engine, and an unreadable subsystem file
 * is treated as "unknown" rather than guessed.
 */
static int find_gc573_pci_device(char *out_addr, size_t max_len, int *out_group)
{
	struct everlive_device cards[8];
	int count;
	int i;

	count = everlive_detect_installed_cards(cards, ARRAY_SIZE(cards), NULL);

	for (i = 0; i < count; i++) {
		if (strcmp(cards[i].desc->model_slug, "gc573") != 0)
			continue;
		if (cards[i].iommu_group < 0)
			continue;
		snprintf(out_addr, max_len, "%s", cards[i].pci_bdf);
		*out_group = cards[i].iommu_group;
		return 0;
	}

	pr_err("No AVerMedia GC573 device (1461:0054/0055) found on PCIe bus!");
	return -ENODEV;
}

int gc573_vfio_resolve(const char *pci_addr, char *bdf, size_t len,
		       int *iommu_group)
{
	char path[512];
	char link_buf[512];
	ssize_t n;
	char *base;

	if (!bdf || !len)
		return -EINVAL;
	if (!pci_addr || !pci_addr[0]) {
		int group = -1;
		int ret = find_gc573_pci_device(bdf, len, &group);

		if (ret == 0 && iommu_group)
			*iommu_group = group;
		return ret;
	}

	snprintf(bdf, len, "%s", pci_addr);
	if (iommu_group) {
		*iommu_group = -1;
		snprintf(path, sizeof(path),
			 "/sys/bus/pci/devices/%s/iommu_group", pci_addr);
		n = readlink(path, link_buf, sizeof(link_buf) - 1);
		if (n > 0) {
			link_buf[n] = '\0';
			base = strrchr(link_buf, '/');
			if (base)
				*iommu_group = atoi(base + 1);
		}
	}
	return 0;
}

static int gc573_vfio_read_sysfs_bar0_size(const char *bdf, size_t *out_size)
{
	char path[512];
	unsigned long long start, end, flags;
	struct stat st;
	FILE *f;

	if (!out_size)
		return -EINVAL;
	*out_size = 0;
	if (!bdf || !bdf[0])
		return -EINVAL;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", bdf);
	f = fopen(path, "r");
	if (f) {
		int matched = fscanf(f, "%llx %llx %llx", &start, &end, &flags);

		fclose(f);
		if (matched == 3 && flags != 0 && start <= end &&
		    end - start + 1 <= SIZE_MAX) {
			*out_size = (size_t)(end - start + 1);
			return 0;
		}
	}

	/* resource0 is only present while the BAR is enabled. */
	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/resource0", bdf);
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
		*out_size = (size_t)st.st_size;
		return 0;
	}

	return -ENODATA;
}

/*
 * Validate a VFIO-reported BAR0 size against the size advertised by sysfs.
 * Both sizes must be non-zero and page-aligned, the sysfs size must be sane
 * for a capture-card MMIO BAR, and VFIO must not claim more than sysfs says
 * exists.  @out_size receives the length that is safe to mmap().
 */
int gc573_vfio_validate_bar0_size(size_t region_size, size_t sysfs_size,
				  size_t page_size, size_t max_size,
				  size_t *out_size)
{
	if (!out_size || !page_size)
		return -EINVAL;
	if (!region_size || !sysfs_size)
		return -EINVAL;
	if (region_size % page_size || sysfs_size % page_size)
		return -EINVAL;
	if (sysfs_size > max_size)
		return -E2BIG;
	if (region_size > sysfs_size)
		return -EINVAL;

	*out_size = min(region_size, sysfs_size);
	return 0;
}

int gc573_vfio_init(struct gc573_vfio_ctx *ctx, const char *pci_addr)
{
	struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };

	/* Raise memlock limit so VFIO can pin large frame buffers without ENOMEM */
	if (setrlimit(RLIMIT_MEMLOCK, &rl) < 0) {
		if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
			rl.rlim_cur = rl.rlim_max;
			(void)setrlimit(RLIMIT_MEMLOCK, &rl);
		}
	}

	memset(ctx, 0, sizeof(*ctx));
	mutex_init(&ctx->irq_lock);
	ctx->container_fd = -1;
	ctx->group_fd = -1;
	ctx->device_fd = -1;
	ctx->irq_efd = -1;
	ctx->iommu_group = -1;

	if (pci_addr && pci_addr[0]) {
		snprintf(ctx->pci_addr, sizeof(ctx->pci_addr), "%s", pci_addr);
		char path[512];
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", ctx->pci_addr);
		char link_buf[512];
		ssize_t len = readlink(path, link_buf, sizeof(link_buf) - 1);
		if (len > 0) {
			link_buf[len] = '\0';
			char *base = strrchr(link_buf, '/');
			if (base)
				ctx->iommu_group = atoi(base + 1);
		}
	} else {
		int ret = find_gc573_pci_device(ctx->pci_addr, sizeof(ctx->pci_addr), &ctx->iommu_group);
		if (ret < 0)
			return ret;
	}

	if (ctx->iommu_group < 0) {
		pr_err("PCI device %s has no usable IOMMU group (is the IOMMU enabled?)",
		       ctx->pci_addr);
		return -ENODEV;
	}

	pr_info("Found GC573 at %s (IOMMU Group %d)", ctx->pci_addr, ctx->iommu_group);

	/* 1. Open VFIO Container */
	ctx->container_fd = open("/dev/vfio/vfio", O_RDWR);
	if (ctx->container_fd < 0) {
		int err = errno;

		pr_err("Cannot open /dev/vfio/vfio: %s (Is vfio module loaded?)",
		       strerror(err));
		return -err;
	}

	int api_version = ioctl(ctx->container_fd, VFIO_GET_API_VERSION);
	if (api_version != VFIO_API_VERSION) {
		int err = errno;

		if (api_version < 0) {
			pr_err("ioctl(VFIO_GET_API_VERSION) failed: %s",
			       strerror(err));
			gc573_vfio_cleanup(ctx);
			return -err;
		}
		pr_err("Unsupported VFIO API version!");
		gc573_vfio_cleanup(ctx);
		return -EINVAL;
	}

	int have_type1 = ioctl(ctx->container_fd, VFIO_CHECK_EXTENSION,
			       VFIO_TYPE1_IOMMU);
	if (have_type1 <= 0) {
		int err = errno;

		if (have_type1 < 0) {
			pr_err("ioctl(VFIO_CHECK_EXTENSION) failed: %s",
			       strerror(err));
			gc573_vfio_cleanup(ctx);
			return -err;
		}
		pr_err("VFIO_TYPE1_IOMMU extension is not supported by kernel!");
		gc573_vfio_cleanup(ctx);
		return -EINVAL;
	}

	/* 2. Open IOMMU Group */
	char group_path[64];
	snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", ctx->iommu_group);
	ctx->group_fd = open(group_path, O_RDWR);
	if (ctx->group_fd < 0) {
		int err = errno;

		if (err == EBUSY)
			pr_err("Cannot open %s: %s (another process is using the capture card)",
			       group_path, strerror(err));
		else
			pr_err("Cannot open %s: %s (Has vfio-pci claimed the device? Run /usr/local/libexec/everlive-vfio-bind --card gc573 --bind or everlive-ctl doctor)",
			       group_path, strerror(err));
		gc573_vfio_cleanup(ctx);
		return -err;
	}

	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	if (ioctl(ctx->group_fd, VFIO_GROUP_GET_STATUS, &group_status) < 0) {
		int err = errno;

		pr_err("ioctl(VFIO_GROUP_GET_STATUS) failed: %s", strerror(err));
		gc573_vfio_cleanup(ctx);
		return -err;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		pr_err("IOMMU group %d is not viable! Some devices in group might lack drivers.", ctx->iommu_group);
		gc573_vfio_cleanup(ctx);
		return -EBUSY;
	}

	/* Attach group to container */
	if (ioctl(ctx->group_fd, VFIO_GROUP_SET_CONTAINER, &ctx->container_fd) < 0) {
		int err = errno;

		pr_err("ioctl(VFIO_GROUP_SET_CONTAINER) failed: %s", strerror(err));
		gc573_vfio_cleanup(ctx);
		return -err;
	}

	/* Enable Type1 IOMMU */
	if (ioctl(ctx->container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
		int err = errno;

		pr_err("ioctl(VFIO_SET_IOMMU) failed: %s", strerror(err));
		gc573_vfio_cleanup(ctx);
		return -err;
	}

	/* 3. Get Device Descriptor */
	ctx->device_fd = ioctl(ctx->group_fd, VFIO_GROUP_GET_DEVICE_FD, ctx->pci_addr);
	if (ctx->device_fd < 0) {
		int err = errno;

		pr_err("ioctl(VFIO_GROUP_GET_DEVICE_FD, %s) failed: %s", ctx->pci_addr, strerror(err));
		gc573_vfio_cleanup(ctx);
		return -err;
	}

	/*
	 * The kernel driver calls pci_set_master(). Without bus master the
	 * FPGA can only DMA when something else happens to set the bit.
	 */
	{
		struct vfio_region_info cfg = {
			.argsz = sizeof(cfg),
			.index = VFIO_PCI_CONFIG_REGION_INDEX,
		};
		uint16_t command = 0;

		if (ioctl(ctx->device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg) == 0 &&
		    pread(ctx->device_fd, &command, sizeof(command),
			  cfg.offset + 0x04) == sizeof(command) &&
		    !(command & 0x0004)) {
			command |= 0x0006;
			if (pwrite(ctx->device_fd, &command, sizeof(command),
				   cfg.offset + 0x04) == sizeof(command))
				pr_info("PCI bus master enabled (command 0x%04x)",
					command);
		}
	}

	/* 4. Query BAR0 MMIO Region */
	struct vfio_region_info bar0_info = {
		.argsz = sizeof(bar0_info),
		.index = VFIO_PCI_BAR0_REGION_INDEX
	};
	if (ioctl(ctx->device_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info) < 0) {
		int err = errno;

		pr_err("ioctl(VFIO_DEVICE_GET_REGION_INFO, BAR0) failed: %s",
		       strerror(err));
		gc573_vfio_cleanup(ctx);
		return -err;
	}

	/*
	 * Never trust the VFIO size on its own: cross-check it against sysfs
	 * so a bad descriptor cannot make us mmap an absurd or unaligned
	 * window.  The validated length is also the one we must unmap later.
	 */
	{
		size_t sysfs_size = 0;
		size_t validated_size = 0;
		long page_size = sysconf(_SC_PAGESIZE);
		int ret;

		if (page_size <= 0) {
			pr_err("Cannot determine system page size");
			gc573_vfio_cleanup(ctx);
			return -EINVAL;
		}
		ret = gc573_vfio_read_sysfs_bar0_size(ctx->pci_addr, &sysfs_size);
		if (ret < 0) {
			pr_err("Cannot read BAR0 size for %s from sysfs: %s",
			       ctx->pci_addr, strerror(-ret));
			gc573_vfio_cleanup(ctx);
			return ret;
		}
		ret = gc573_vfio_validate_bar0_size(bar0_info.size, sysfs_size,
						    (size_t)page_size,
						    GC573_VFIO_BAR0_MAX_SIZE,
						    &validated_size);
		if (ret < 0) {
			pr_err("Refusing BAR0: VFIO size=%llu, sysfs size=%zu (%s)",
			       (unsigned long long)bar0_info.size, sysfs_size,
			       strerror(-ret));
			gc573_vfio_cleanup(ctx);
			return ret;
		}
		ctx->bar0_size = validated_size;
	}
	ctx->bar0_offset = bar0_info.offset;

	/* 5. Map BAR0 MMIO into Userspace Address Space */
	ctx->bar0 = mmap(NULL, ctx->bar0_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, ctx->device_fd, ctx->bar0_offset);
	if (ctx->bar0 == MAP_FAILED) {
		int err = errno;

		pr_err("mmap(BAR0, size=%zu) failed: %s", ctx->bar0_size, strerror(err));
		ctx->bar0 = NULL;
		gc573_vfio_cleanup(ctx);
		return -err;
	}

	pr_info("Successfully mapped GC573 BAR0 MMIO (%zu KB) at %p",
		ctx->bar0_size / 1024, ctx->bar0);

	return 0;
}

static void gc573_vfio_keep_across_exec(int fd)
{
	int flags;

	if (fd < 0)
		return;
	flags = fcntl(fd, F_GETFD);
	if (flags >= 0)
		fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

static void gc573_vfio_disable_irq(struct gc573_vfio_ctx *ctx)
{
	struct vfio_irq_set irq = {
		.argsz = sizeof(irq),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = VFIO_PCI_MSI_IRQ_INDEX,
		.start = 0,
		.count = 0,
	};

	if (!ctx || ctx->device_fd < 0)
		return;
	if (ioctl(ctx->device_fd, VFIO_DEVICE_SET_IRQS, &irq) < 0) {
		irq.index = VFIO_PCI_INTX_IRQ_INDEX;
		ioctl(ctx->device_fd, VFIO_DEVICE_SET_IRQS, &irq);
	}
}

void gc573_vfio_release_irq(struct gc573_vfio_ctx *ctx)
{
	if (!ctx)
		return;
	gc573_vfio_disable_irq(ctx);
	if (ctx->irq_efd >= 0) {
		close(ctx->irq_efd);
		ctx->irq_efd = -1;
	}
}

void gc573_vfio_unmap_bar(struct gc573_vfio_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->bar0 && ctx->bar0_size) {
		munmap(ctx->bar0, ctx->bar0_size);
		ctx->bar0 = NULL;
	}
}

/*
 * Unmap every tracked DMA region.  The loop is bounded by both the hardware
 * limit and the array size, and it always drops the entry even when the
 * kernel ioctl fails: a broken/unresponsive container must never turn
 * teardown into an infinite spin.
 */
static void gc573_vfio_unmap_all_dma(struct gc573_vfio_ctx *ctx)
{
	unsigned int n;
	int first_err = 0;

	if (!ctx)
		return;

	n = ctx->nr_dma_regions;
	if (n > GC573_VFIO_MAX_DMA_REGIONS)
		n = GC573_VFIO_MAX_DMA_REGIONS;

	for (unsigned int i = n; i > 0; i--) {
		struct gc573_vfio_dma_region *region = &ctx->dma_regions[i - 1];
		uint64_t iova = region->iova;
		size_t size = region->size;
		int ret = gc573_vfio_unmap_dma(ctx, iova, size);

		if (ret < 0 && !first_err)
			first_err = ret;

		memset(region, 0, sizeof(*region));
		ctx->nr_dma_regions = i - 1;
	}
	ctx->nr_dma_regions = 0;

	if (first_err)
		pr_warn("Failed to unmap one or more IOMMU DMA regions (first error: %s)",
			strerror(-first_err));
}

void gc573_vfio_prepare_inherit(struct gc573_vfio_ctx *ctx)
{
	if (!ctx)
		return;

	gc573_vfio_unmap_all_dma(ctx);
	gc573_vfio_release_irq(ctx);
	gc573_vfio_unmap_bar(ctx);
	gc573_vfio_keep_fds(ctx);
}

void gc573_vfio_keep_fds(struct gc573_vfio_ctx *ctx)
{
	if (!ctx)
		return;
	gc573_vfio_keep_across_exec(ctx->container_fd);
	gc573_vfio_keep_across_exec(ctx->group_fd);
	gc573_vfio_keep_across_exec(ctx->device_fd);
}

void gc573_vfio_cleanup(struct gc573_vfio_ctx *ctx)
{
	if (!ctx)
		return;

	gc573_vfio_unmap_all_dma(ctx);

	if (ctx->bar0 && ctx->bar0_size) {
		munmap(ctx->bar0, ctx->bar0_size);
		ctx->bar0 = NULL;
	}

	if (ctx->irq_efd >= 0) {
		close(ctx->irq_efd);
		ctx->irq_efd = -1;
	}

	if (ctx->device_fd >= 0) {
		close(ctx->device_fd);
		ctx->device_fd = -1;
	}

	if (ctx->group_fd >= 0) {
		close(ctx->group_fd);
		ctx->group_fd = -1;
	}

	if (ctx->container_fd >= 0) {
		close(ctx->container_fd);
		ctx->container_fd = -1;
	}

	pr_info("VFIO hardware context cleaned up.");
}

int gc573_vfio_map_dma(struct gc573_vfio_ctx *ctx, void *vaddr, size_t size, uint64_t iova)
{
	if (!ctx || ctx->container_fd < 0 || !vaddr || !size)
		return -EINVAL;

	struct vfio_iommu_type1_dma_map dma_map = {
		.argsz = sizeof(dma_map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr = (uint64_t)vaddr,
		.iova = iova,
		.size = size,
	};

	if (ctx->nr_dma_regions >= GC573_VFIO_MAX_DMA_REGIONS) {
		pr_err("Too many IOMMU DMA regions (max %u)",
		       GC573_VFIO_MAX_DMA_REGIONS);
		return -ENOSPC;
	}

	if (ioctl(ctx->container_fd, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		int err = errno;

		pr_err("ioctl(VFIO_IOMMU_MAP_DMA, size=%zu, iova=0x%lx) failed: %s",
		       size, (unsigned long)iova, strerror(err));
		return -err;
	}

	ctx->dma_regions[ctx->nr_dma_regions].vaddr = vaddr;
	ctx->dma_regions[ctx->nr_dma_regions].size = size;
	ctx->dma_regions[ctx->nr_dma_regions].iova = iova;
	ctx->nr_dma_regions++;

	pr_info("Mapped %zu KB DMA buffer (VA: %p -> IOVA: 0x%lx)",
		size / 1024, vaddr, (unsigned long)iova);
	return 0;
}

static bool gc573_vfio_test_fail_unmap_dma(void)
{
	const char *value = getenv("EVERLIVE_TEST_FAIL_UNMAP_DMA");

	return value && strcmp(value, "1") == 0;
}

int gc573_vfio_unmap_dma(struct gc573_vfio_ctx *ctx, uint64_t iova, size_t size)
{
	if (!ctx || !size)
		return -EINVAL;

	/*
	 * Hardware-free fault injection: lets the teardown paths be exercised
	 * without a real VFIO container fd.
	 */
	if (gc573_vfio_test_fail_unmap_dma()) {
		int err = EIO;

		pr_err("ioctl(VFIO_IOMMU_UNMAP_DMA, iova=0x%lx) failed: %s (injected)",
		       (unsigned long)iova, strerror(err));
		return -err;
	}

	if (ctx->container_fd < 0)
		return -EINVAL;

	struct vfio_iommu_type1_dma_unmap dma_unmap = {
		.argsz = sizeof(dma_unmap),
		.flags = 0,
		.iova = iova,
		.size = size,
	};

	if (ioctl(ctx->container_fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap) < 0) {
		int err = errno;

		pr_err("ioctl(VFIO_IOMMU_UNMAP_DMA, iova=0x%lx) failed: %s",
		       (unsigned long)iova, strerror(err));
		return -err;
	}

	for (unsigned int i = 0; i < ctx->nr_dma_regions; i++) {
		if (ctx->dma_regions[i].iova != iova ||
		    ctx->dma_regions[i].size != size)
			continue;

		ctx->dma_regions[i] =
			ctx->dma_regions[ctx->nr_dma_regions - 1];
		ctx->nr_dma_regions--;
		break;
	}

	return 0;
}

int gc573_vfio_setup_msi(struct gc573_vfio_ctx *ctx)
{
	if (!ctx || ctx->device_fd < 0)
		return -EINVAL;

	ctx->irq_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (ctx->irq_efd < 0) {
		int err = errno;

		pr_err("Failed to create eventfd for IRQ: %s", strerror(err));
		return -err;
	}

	char irq_set_buf[sizeof(struct vfio_irq_set) + sizeof(int)];
	struct vfio_irq_set *irq_set = (struct vfio_irq_set *)irq_set_buf;

	irq_set->argsz = sizeof(irq_set_buf);
	irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_MSI_IRQ_INDEX;
	irq_set->start = 0;
	irq_set->count = 1;
	*(int *)&irq_set->data = ctx->irq_efd;

	if (ioctl(ctx->device_fd, VFIO_DEVICE_SET_IRQS, irq_set) < 0) {
		int err = errno;

		pr_warn("MSI interrupt setup failed, trying INTx: %s", strerror(err));
		irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
		if (ioctl(ctx->device_fd, VFIO_DEVICE_SET_IRQS, irq_set) < 0) {
			int intx_err = errno;

			pr_err("INTx interrupt setup also failed: %s",
			       strerror(intx_err));
			close(ctx->irq_efd);
			ctx->irq_efd = -1;
			return -intx_err;
		}
		pr_info("Registered INTx interrupt handler on eventfd %d", ctx->irq_efd);
		return 0;
	}

	pr_info("Registered MSI interrupt handler on eventfd %d", ctx->irq_efd);
	return 0;
}

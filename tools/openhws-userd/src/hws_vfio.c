/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hws_vfio.c — VFIO PCI plumbing for the HWS UHD userspace engine
 */

#include "hws_vfio.h"
#include <dirent.h>
#include <sys/resource.h>

/*
 * Card discovery is a private sysfs scan: this engine is kept separate from
 * the shared card registry, so it only ever considers its own PCI IDs.
 */
static const struct { unsigned int vendor, device; } kHwsIds[] = {
	{ 0x8888, 0x8581 }, { 0x8888, 0x85a1 }, { 0x8888, 0x8591 }, { 0x1f33, 0x8581 },
};
#define HWS_SUBSYSTEM_DEVICE	0x0007

static int sysfs_hex(const char *dir, const char *attr, unsigned int *out)
{
	char path[512];
	FILE *f;
	int ok;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", dir, attr);
	f = fopen(path, "r");
	if (!f)
		return -1;
	ok = fscanf(f, "%x", out) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

static int read_iommu_group(const char *bdf)
{
	char path[512], link_buf[512], *base;
	ssize_t n;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);
	n = readlink(path, link_buf, sizeof(link_buf) - 1);
	if (n <= 0)
		return -1;
	link_buf[n] = '\0';
	base = strrchr(link_buf, '/');
	return base ? atoi(base + 1) : -1;
}

bool hws_vfio_is_supported(const char *bdf)
{
	unsigned int v, d, sd;

	if (sysfs_hex(bdf, "vendor", &v) || sysfs_hex(bdf, "device", &d) ||
	    sysfs_hex(bdf, "subsystem_device", &sd) || sd != HWS_SUBSYSTEM_DEVICE)
		return false;
	for (size_t i = 0; i < ARRAY_SIZE(kHwsIds); i++)
		if (kHwsIds[i].vendor == v && kHwsIds[i].device == d)
			return true;
	return false;
}

static int find_hws_pci_device(char *out_addr, size_t max_len, int *out_group)
{
	DIR *dir = opendir("/sys/bus/pci/devices");
	struct dirent *ent;
	char best[64] = "";

	if (!dir)
		return -ENODEV;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.' || !hws_vfio_is_supported(ent->d_name))
			continue;
		/* Lowest BDF first, so the choice is stable across boots. */
		if (!best[0] || strcmp(ent->d_name, best) < 0)
			snprintf(best, sizeof(best), "%.63s", ent->d_name);
	}
	closedir(dir);
	if (!best[0]) {
		pr_err("No HWS UHD capture card (8888:8581/85A1/8591, 1F33:8581, subsystem 0007) found");
		return -ENODEV;
	}
	snprintf(out_addr, max_len, "%s", best);
	*out_group = read_iommu_group(best);
	return 0;
}

int hws_vfio_resolve(const char *pci_addr, char *bdf, size_t len,
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
		int ret = find_hws_pci_device(bdf, len, &group);

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

int hws_vfio_init(struct hws_vfio_ctx *ctx, const char *pci_addr)
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
		int ret = find_hws_pci_device(ctx->pci_addr, sizeof(ctx->pci_addr), &ctx->iommu_group);
		if (ret < 0)
			return ret;
	}

	pr_info("Found HWS UHD card at %s (IOMMU Group %d)", ctx->pci_addr, ctx->iommu_group);

	/* 1. Open VFIO Container */
	ctx->container_fd = open("/dev/vfio/vfio", O_RDWR);
	if (ctx->container_fd < 0) {
		pr_err("Cannot open /dev/vfio/vfio: %s (Is vfio module loaded?)", strerror(errno));
		return -errno;
	}

	if (ioctl(ctx->container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
		pr_err("Unsupported VFIO API version!");
		hws_vfio_cleanup(ctx);
		return -EINVAL;
	}

	if (!ioctl(ctx->container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
		pr_err("VFIO_TYPE1_IOMMU extension is not supported by kernel!");
		hws_vfio_cleanup(ctx);
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
			pr_err("Cannot open %s: %s (Has vfio-pci claimed the device? Run hws-vfio-bind.sh --bind)",
			       group_path, strerror(err));
		hws_vfio_cleanup(ctx);
		return -err;
	}

	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	if (ioctl(ctx->group_fd, VFIO_GROUP_GET_STATUS, &group_status) < 0) {
		pr_err("ioctl(VFIO_GROUP_GET_STATUS) failed: %s", strerror(errno));
		hws_vfio_cleanup(ctx);
		return -errno;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		pr_err("IOMMU group %d is not viable! Some devices in group might lack drivers.", ctx->iommu_group);
		hws_vfio_cleanup(ctx);
		return -EBUSY;
	}

	/* Attach group to container */
	if (ioctl(ctx->group_fd, VFIO_GROUP_SET_CONTAINER, &ctx->container_fd) < 0) {
		pr_err("ioctl(VFIO_GROUP_SET_CONTAINER) failed: %s", strerror(errno));
		hws_vfio_cleanup(ctx);
		return -errno;
	}

	/* Enable Type1 IOMMU */
	if (ioctl(ctx->container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
		pr_err("ioctl(VFIO_SET_IOMMU) failed: %s", strerror(errno));
		hws_vfio_cleanup(ctx);
		return -errno;
	}

	/* 3. Get Device Descriptor */
	ctx->device_fd = ioctl(ctx->group_fd, VFIO_GROUP_GET_DEVICE_FD, ctx->pci_addr);
	if (ctx->device_fd < 0) {
		pr_err("ioctl(VFIO_GROUP_GET_DEVICE_FD, %s) failed: %s", ctx->pci_addr, strerror(errno));
		hws_vfio_cleanup(ctx);
		return -errno;
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
		pr_err("ioctl(VFIO_DEVICE_GET_REGION_INFO, BAR0) failed: %s", strerror(errno));
		hws_vfio_cleanup(ctx);
		return -errno;
	}

	ctx->bar0_size = bar0_info.size;
	ctx->bar0_offset = bar0_info.offset;

	/* 5. Map BAR0 MMIO into Userspace Address Space */
	ctx->bar0 = mmap(NULL, ctx->bar0_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, ctx->device_fd, ctx->bar0_offset);
	if (ctx->bar0 == MAP_FAILED) {
		pr_err("mmap(BAR0, size=%zu) failed: %s", ctx->bar0_size, strerror(errno));
		ctx->bar0 = NULL;
		hws_vfio_cleanup(ctx);
		return -errno;
	}

	pr_info("Mapped BAR0 MMIO (%zu KB) at %p",
		ctx->bar0_size / 1024, ctx->bar0);

	return 0;
}

static void hws_vfio_keep_across_exec(int fd)
{
	int flags;

	if (fd < 0)
		return;
	flags = fcntl(fd, F_GETFD);
	if (flags >= 0)
		fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

static void hws_vfio_disable_irq(struct hws_vfio_ctx *ctx)
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

void hws_vfio_release_irq(struct hws_vfio_ctx *ctx)
{
	if (!ctx)
		return;
	hws_vfio_disable_irq(ctx);
	if (ctx->irq_efd >= 0) {
		close(ctx->irq_efd);
		ctx->irq_efd = -1;
	}
}

void hws_vfio_unmap_bar(struct hws_vfio_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->bar0 && ctx->bar0_size) {
		munmap(ctx->bar0, ctx->bar0_size);
		ctx->bar0 = NULL;
	}
}

void hws_vfio_prepare_inherit(struct hws_vfio_ctx *ctx)
{
	if (!ctx)
		return;

	while (ctx->nr_dma_regions) {
		struct hws_vfio_dma_region *region =
			&ctx->dma_regions[ctx->nr_dma_regions - 1];

		hws_vfio_unmap_dma(ctx, region->iova, region->size);
	}
	hws_vfio_release_irq(ctx);
	hws_vfio_unmap_bar(ctx);
	hws_vfio_keep_fds(ctx);
}

void hws_vfio_keep_fds(struct hws_vfio_ctx *ctx)
{
	if (!ctx)
		return;
	hws_vfio_keep_across_exec(ctx->container_fd);
	hws_vfio_keep_across_exec(ctx->group_fd);
	hws_vfio_keep_across_exec(ctx->device_fd);
}

void hws_vfio_cleanup(struct hws_vfio_ctx *ctx)
{
	if (!ctx)
		return;

	while (ctx->nr_dma_regions) {
		struct hws_vfio_dma_region *region =
			&ctx->dma_regions[ctx->nr_dma_regions - 1];

		hws_vfio_unmap_dma(ctx, region->iova, region->size);
	}

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

int hws_vfio_map_dma(struct hws_vfio_ctx *ctx, void *vaddr, size_t size, uint64_t iova)
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

	if (ctx->nr_dma_regions >= HWS_VFIO_MAX_DMA_REGIONS) {
		pr_err("Too many IOMMU DMA regions (max %u)",
		       HWS_VFIO_MAX_DMA_REGIONS);
		return -ENOSPC;
	}

	if (ioctl(ctx->container_fd, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
		pr_err("ioctl(VFIO_IOMMU_MAP_DMA, size=%zu, iova=0x%lx) failed: %s",
		       size, (unsigned long)iova, strerror(errno));
		return -errno;
	}

	ctx->dma_regions[ctx->nr_dma_regions].vaddr = vaddr;
	ctx->dma_regions[ctx->nr_dma_regions].size = size;
	ctx->dma_regions[ctx->nr_dma_regions].iova = iova;
	ctx->nr_dma_regions++;

	pr_info("Mapped %zu KB DMA buffer (VA: %p -> IOVA: 0x%lx)",
		size / 1024, vaddr, (unsigned long)iova);
	return 0;
}

int hws_vfio_unmap_dma(struct hws_vfio_ctx *ctx, uint64_t iova, size_t size)
{
	if (!ctx || ctx->container_fd < 0 || !size)
		return -EINVAL;

	struct vfio_iommu_type1_dma_unmap dma_unmap = {
		.argsz = sizeof(dma_unmap),
		.flags = 0,
		.iova = iova,
		.size = size,
	};

	if (ioctl(ctx->container_fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap) < 0) {
		pr_err("ioctl(VFIO_IOMMU_UNMAP_DMA, iova=0x%lx) failed: %s",
		       (unsigned long)iova, strerror(errno));
		return -errno;
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

int hws_vfio_setup_msi(struct hws_vfio_ctx *ctx)
{
	if (!ctx || ctx->device_fd < 0)
		return -EINVAL;

	ctx->irq_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (ctx->irq_efd < 0) {
		pr_err("Failed to create eventfd for IRQ: %s", strerror(errno));
		return -errno;
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
		pr_warn("MSI interrupt setup failed, trying INTx: %s", strerror(errno));
		irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
		if (ioctl(ctx->device_fd, VFIO_DEVICE_SET_IRQS, irq_set) < 0) {
			pr_err("INTx interrupt setup also failed: %s", strerror(errno));
			close(ctx->irq_efd);
			ctx->irq_efd = -1;
			return -errno;
		}
		pr_info("Registered INTx interrupt handler on eventfd %d", ctx->irq_efd);
		return 0;
	}

	pr_info("Registered MSI interrupt handler on eventfd %d", ctx->irq_efd);
	return 0;
}

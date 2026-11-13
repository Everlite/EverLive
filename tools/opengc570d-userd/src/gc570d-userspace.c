// SPDX-License-Identifier: GPL-2.0-only
/*
 * OpenGC570D Userspace Driver Runtime (VFIO-PCI + PipeWire + Control Socket)
 *
 * Runs the complete GC570D hardware stack (I2C, IT68051, IT6802, VideoSplitter,
 * Xilinx HDR-to-SDR LUT preprocessor, VIP0/VIP1 scalers, DMA0/DMA1 video rings,
 * Audio0/Audio1 PCM rings, and RGB LED controller) entirely in userspace using
 * the stock Linux kernel's signed `vfio-pci` driver and `libpipewire-0.3`.
 */
#include "gc570d.h"
#include "everlive_framestore.h"
#include "gc570d_everlive.h"
#include "everlive_card.h"
#include "gc570d-jsonrpc.h"

#ifndef EVERLIVE_VERSION
#define EVERLIVE_VERSION "1.5.0"
#endif

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <linux/vfio.h>
#include <linux/pci_regs.h>

#ifdef HAVE_LIBPIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>
#endif

const int vb2_dma_contig_memops = 0;

/* Global singleton state for the userspace daemon */
static struct pci_dev g_pdev;
static struct gc570d_dev *g_gc;
static _Atomic bool g_daemon_quit = false;
static volatile bool g_reinit_requested = false;
static struct everlive_framestore *g_framestore = NULL;

/* Thread-local pointer to identify current kthread for kthread_should_stop() */
static __thread struct task_struct *tls_current_task = NULL;

/* =========================================================================
 * 1. Logging, Waitqueues, Completions, Kthreads, and Delayed Workqueues
 * ========================================================================= */

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void gc570d_log_msg(const char *level, const struct device *dev, const char *fmt, ...)
{
	struct timespec ts;
	va_list args;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	pthread_mutex_lock(&g_log_lock);
	fprintf(stderr, "[%5lld.%06ld] [gc570d %s] ",
		(long long)ts.tv_sec, ts.tv_nsec / 1000L, level);
	if (dev && dev->name)
		fprintf(stderr, "%s: ", dev->name);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
	pthread_mutex_unlock(&g_log_lock);
}

static void init_cond_monotonic(pthread_cond_t *cond)
{
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(cond, &attr);
	pthread_condattr_destroy(&attr);
}

void init_waitqueue_head(wait_queue_head_t *wq)
{
	pthread_mutex_init(&wq->lock, NULL);
	init_cond_monotonic(&wq->cond);
}

void wake_up_interruptible(wait_queue_head_t *wq)
{
	pthread_mutex_lock(&wq->lock);
	pthread_cond_broadcast(&wq->cond);
	pthread_mutex_unlock(&wq->lock);
}

void gc570d_wq_wait_ms(wait_queue_head_t *wq, unsigned long timeout_ms)
{
	struct timespec ts;
	u64 target_ns = ktime_get_ns() + (u64)timeout_ms * NSEC_PER_MSEC;

	ts.tv_sec = target_ns / NSEC_PER_SEC;
	ts.tv_nsec = target_ns % NSEC_PER_SEC;
	pthread_mutex_lock(&wq->lock);
	pthread_cond_timedwait(&wq->cond, &wq->lock, &ts);
	pthread_mutex_unlock(&wq->lock);
}

void init_completion(struct completion *x)
{
	pthread_mutex_init(&x->lock, NULL);
	init_cond_monotonic(&x->cond);
	x->done = 0;
}

void reinit_completion(struct completion *x)
{
	pthread_mutex_lock(&x->lock);
	x->done = 0;
	pthread_mutex_unlock(&x->lock);
}

void complete(struct completion *x)
{
	pthread_mutex_lock(&x->lock);
	x->done++;
	pthread_cond_broadcast(&x->cond);
	pthread_mutex_unlock(&x->lock);
}

bool try_wait_for_completion(struct completion *x)
{
	bool ret = false;
	pthread_mutex_lock(&x->lock);
	if (x->done > 0) {
		x->done--;
		ret = true;
	}
	pthread_mutex_unlock(&x->lock);
	return ret;
}

unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout_ms)
{
	u64 deadline_ns = ktime_get_ns() + (u64)timeout_ms * NSEC_PER_MSEC;
	unsigned long ret = 0;

	pthread_mutex_lock(&x->lock);
	while (x->done == 0) {
		struct timespec ts;
		u64 now_ns = ktime_get_ns();
		if (now_ns >= deadline_ns)
			break;
		ts.tv_sec = deadline_ns / NSEC_PER_SEC;
		ts.tv_nsec = deadline_ns % NSEC_PER_SEC;
		pthread_cond_timedwait(&x->cond, &x->lock, &ts);
	}
	if (x->done > 0) {
		x->done--;
		ret = 1;
	}
	pthread_mutex_unlock(&x->lock);
	return ret;
}

static void *kthread_trampoline(void *arg)
{
	struct task_struct *k = arg;
	tls_current_task = k;
	if (k->name[0])
		pthread_setname_np(pthread_self(), k->name);
	k->threadfn(k->data);
	return NULL;
}

struct task_struct *gc570d_kthread_create(int (*threadfn)(void *data), void *data,
					  const char *fmt, ...)
{
	struct task_struct *k = calloc(1, sizeof(*k));
	va_list args;

	if (!k)
		return ERR_PTR(-ENOMEM);
	k->threadfn = threadfn;
	k->data = data;
	atomic_store(&k->should_stop, false);
	atomic_store(&k->started, false);
	atomic_store(&k->refcount, 1);
	va_start(args, fmt);
	vsnprintf(k->name, sizeof(k->name), fmt, args);
	va_end(args);
	return k;
}

int wake_up_process(struct task_struct *k)
{
	if (!k || IS_ERR(k))
		return -EINVAL;
	if (atomic_exchange(&k->started, true))
		return 0;
	if (pthread_create(&k->thread, NULL, kthread_trampoline, k) != 0) {
		atomic_store(&k->started, false);
		return -EAGAIN;
	}
	return 1;
}

struct task_struct *get_task_struct(struct task_struct *k)
{
	if (k && !IS_ERR(k))
		atomic_fetch_add(&k->refcount, 1);
	return k;
}

void put_task_struct(struct task_struct *k)
{
	if (k && !IS_ERR(k)) {
		if (atomic_fetch_sub(&k->refcount, 1) == 1)
			free(k);
	}
}

int kthread_stop(struct task_struct *k)
{
	if (!k || IS_ERR(k))
		return -EINVAL;
	atomic_store(&k->should_stop, true);
	if (g_gc) {
		wake_up_interruptible(&g_gc->video_wait);
		wake_up_interruptible(&g_gc->video0_wait);
		wake_up_interruptible(&g_gc->audio_wait);
		wake_up_interruptible(&g_gc->audio0_wait);
	}
	if (atomic_load(&k->started))
		pthread_join(k->thread, NULL);
	put_task_struct(k);
	return g_reinit_requested ? 1 : 0;
}

bool kthread_should_stop(void)
{
	if (tls_current_task)
		return atomic_load(&tls_current_task->should_stop);
	return atomic_load(&g_daemon_quit);
}

static void *work_worker_thread(void *arg)
{
	struct work_struct *work = arg;

	pthread_mutex_lock(&work->lock);
	while (!work->shutting_down) {
		if (!work->pending) {
			pthread_cond_wait(&work->cond, &work->lock);
			continue;
		}
		u64 now_ns = ktime_get_ns();
		if (now_ns < work->due_ns) {
			struct timespec ts;
			ts.tv_sec = work->due_ns / NSEC_PER_SEC;
			ts.tv_nsec = work->due_ns % NSEC_PER_SEC;
			pthread_cond_timedwait(&work->cond, &work->lock, &ts);
			continue;
		}
		work->pending = false;
		work->running = true;
		pthread_mutex_unlock(&work->lock);

		work->func(work);

		pthread_mutex_lock(&work->lock);
		work->running = false;
		pthread_cond_broadcast(&work->cond);
	}
	pthread_mutex_unlock(&work->lock);
	return NULL;
}

void gc570d_init_work(struct work_struct *work, work_func_t func)
{
	int ret;

	memset(work, 0, sizeof(*work));
	work->func = func;
	pthread_mutex_init(&work->lock, NULL);
	init_cond_monotonic(&work->cond);
	ret = pthread_create(&work->thread, NULL, work_worker_thread, work);
	if (ret) {
		fprintf(stderr,
			"[gc570d ERR ] failed to create workqueue thread: %s\n",
			strerror(ret));
		pthread_cond_destroy(&work->cond);
		pthread_mutex_destroy(&work->lock);
		return;
	}
	work->initialized = true;
}

bool gc570d_schedule_work_delay(struct work_struct *work, unsigned long delay_ms)
{
	if (!work || !work->initialized)
		return false;
	pthread_mutex_lock(&work->lock);
	if (!work->shutting_down) {
		work->due_ns = ktime_get_ns() + (u64)delay_ms * NSEC_PER_MSEC;
		work->pending = true;
		pthread_cond_broadcast(&work->cond);
	}
	pthread_mutex_unlock(&work->lock);
	return true;
}

bool gc570d_cancel_work_sync(struct work_struct *work)
{
	bool was_pending = false;
	if (!work || !work->initialized)
		return false;
	pthread_mutex_lock(&work->lock);
	was_pending = work->pending;
	work->pending = false;
	pthread_cond_broadcast(&work->cond);
	while (work->running)
		pthread_cond_wait(&work->cond, &work->lock);
	pthread_mutex_unlock(&work->lock);
	return was_pending;
}

/*
 * Userspace-only teardown.  The kernel frees work_structs through devm after
 * cancel_work_sync(), because the kernel workqueue guarantees the callback
 * cannot run again once it returns.  Our emulation runs a dedicated pthread
 * per work_struct, so the thread must be told to exit and joined before the
 * containing device state is freed, otherwise it keeps waiting on a mutex and
 * condition variable that no longer exist.
 */
void gc570d_destroy_work(struct work_struct *work)
{
	if (!work || !work->initialized)
		return;

	pthread_mutex_lock(&work->lock);
	work->shutting_down = true;
	work->pending = false;
	work->initialized = false;
	pthread_cond_broadcast(&work->cond);
	pthread_mutex_unlock(&work->lock);

	pthread_join(work->thread, NULL);
	pthread_cond_destroy(&work->cond);
	pthread_mutex_destroy(&work->lock);
}

/* =========================================================================
 * 2. VFIO-PCI Device Discovery, BAR0 MMIO, IOMMU DMA Mapping, & Eventfd IRQ
 * ========================================================================= */

struct gc570d_dma_mapping {
	bool in_use;
	void *vaddr;
	dma_addr_t iova;
	size_t aligned_size;
};

#define GC570D_MAX_DMA_MAPPINGS 64
/*
 * Keep a bounded cache of freed mappings so repeated prepare/hw_free and
 * video-recovery cycles do not churn VFIO_IOMMU_MAP/UNMAP ioctls or consume
 * fresh IOVA space every time.  The kernel module's allocation call pattern
 * is intentionally left untouched; the recycling happens below its shim.
 */
#define GC570D_DMA_CACHE_BLOCKS 16
#define GC570D_DMA_CACHE_BYTES  (32u * 1024 * 1024)
static struct gc570d_dma_mapping g_dma_map_table[GC570D_MAX_DMA_MAPPINGS];
static unsigned int g_dma_cached_blocks;
static size_t g_dma_cached_bytes;
static pthread_mutex_t g_dma_map_lock = PTHREAD_MUTEX_INITIALIZER;
static dma_addr_t g_next_iova = 0x10000000ULL;

static size_t page_align_up(size_t size)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	if (!pg)
		pg = 4096;
	return (size + pg - 1) & ~(pg - 1);
}

/* Caller must hold g_dma_map_lock. */
static void gc570d_dma_unmap_locked(struct pci_dev *pdev,
				    struct gc570d_dma_mapping *m)
{
	if (pdev->vfio_container_fd >= 0) {
		struct vfio_iommu_type1_dma_unmap unmap = {
			.argsz = sizeof(unmap),
			.flags = 0,
			.iova  = m->iova,
			.size  = m->aligned_size,
		};
		ioctl(pdev->vfio_container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
	}
	munmap(m->vaddr, m->aligned_size);
	memset(m, 0, sizeof(*m));
}

/* Caller must hold g_dma_map_lock. */
static void gc570d_dma_cache_drain_locked(struct pci_dev *pdev)
{
	int i;

	for (i = 0; i < GC570D_MAX_DMA_MAPPINGS; i++) {
		struct gc570d_dma_mapping *m = &g_dma_map_table[i];

		if (!m->in_use && m->vaddr)
			gc570d_dma_unmap_locked(pdev, m);
	}
	g_dma_cached_blocks = 0;
	g_dma_cached_bytes = 0;
}

static void gc570d_dma_cache_drain(struct pci_dev *pdev)
{
	pthread_mutex_lock(&g_dma_map_lock);
	gc570d_dma_cache_drain_locked(pdev);
	pthread_mutex_unlock(&g_dma_map_lock);
}

void *dma_alloc_coherent(struct device *dev, size_t size,
			 dma_addr_t *dma_handle, gfp_t gfp)
{
	struct pci_dev *pdev = dev && dev->pdev ? dev->pdev : &g_pdev;
	size_t aligned = page_align_up(size ? size : 4096);
	void *vaddr;
	dma_addr_t iova;
	int slot = -1;
	int i;

	(void)gfp;
	pthread_mutex_lock(&g_dma_map_lock);

	/* Reuse a cached mapping of exactly the requested size. */
	for (i = 0; i < GC570D_MAX_DMA_MAPPINGS; i++) {
		struct gc570d_dma_mapping *m = &g_dma_map_table[i];

		if (!m->in_use && m->vaddr && m->aligned_size == aligned) {
			m->in_use = true;
			g_dma_cached_blocks--;
			g_dma_cached_bytes -= m->aligned_size;
			pthread_mutex_unlock(&g_dma_map_lock);
			/* dma_alloc_coherent() is documented as zeroed memory. */
			memset(m->vaddr, 0, aligned);
			if (dma_handle)
				*dma_handle = m->iova;
			return m->vaddr;
		}
	}

	for (i = 0; i < GC570D_MAX_DMA_MAPPINGS; i++) {
		if (!g_dma_map_table[i].in_use && !g_dma_map_table[i].vaddr) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		/* No pristine slot: evict one cached block to make room. */
		for (i = 0; i < GC570D_MAX_DMA_MAPPINGS; i++) {
			struct gc570d_dma_mapping *m = &g_dma_map_table[i];

			if (!m->in_use && m->vaddr) {
				g_dma_cached_blocks--;
				g_dma_cached_bytes -= m->aligned_size;
				gc570d_dma_unmap_locked(pdev, m);
				slot = i;
				break;
			}
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_dma_map_lock);
		return NULL;
	}

	vaddr = mmap(NULL, aligned, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (vaddr == MAP_FAILED) {
		pthread_mutex_unlock(&g_dma_map_lock);
		return NULL;
	}

	/* Fault in pages before VFIO_IOMMU_MAP_DMA pins them in the kernel */
	memset(vaddr, 0, aligned);

	iova = g_next_iova;
	g_next_iova += aligned + 0x10000ULL; /* 64KB guard page between IOVAs */

	if (pdev->vfio_container_fd >= 0) {
		struct vfio_iommu_type1_dma_map dma_map = {
			.argsz = sizeof(dma_map),
			.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
			.vaddr = (uintptr_t)vaddr,
			.iova  = iova,
			.size  = aligned,
		};
		if (ioctl(pdev->vfio_container_fd, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
			int err = errno;
			pthread_mutex_unlock(&g_dma_map_lock);
			fprintf(stderr,
				"[gc570d ERR ] VFIO_IOMMU_MAP_DMA failed (size=%zu iova=0x%llx): %s\n"
				"              Fix: sudo setcap cap_ipc_lock=+ep %s/opengc570d-userd\n",
				aligned, (unsigned long long)iova, strerror(err),
				"tools/opengc570d-userd");
			munmap(vaddr, aligned);
			return NULL;
		}
	}

	g_dma_map_table[slot].in_use = true;
	g_dma_map_table[slot].vaddr = vaddr;
	g_dma_map_table[slot].iova = iova;
	g_dma_map_table[slot].aligned_size = aligned;
	pthread_mutex_unlock(&g_dma_map_lock);

	if (dma_handle)
		*dma_handle = iova;
	return vaddr;
}

void dma_free_coherent(struct device *dev, size_t size,
		       void *cpu_addr, dma_addr_t dma_handle)
{
	struct pci_dev *pdev = dev && dev->pdev ? dev->pdev : &g_pdev;
	size_t aligned = page_align_up(size ? size : 4096);
	struct gc570d_dma_mapping *m = NULL;
	int i;

	if (!cpu_addr)
		return;

	pthread_mutex_lock(&g_dma_map_lock);
	for (i = 0; i < GC570D_MAX_DMA_MAPPINGS; i++) {
		struct gc570d_dma_mapping *cand = &g_dma_map_table[i];

		if (cand->in_use && cand->vaddr == cpu_addr &&
		    (!dma_handle || cand->iova == dma_handle)) {
			m = cand;
			break;
		}
	}
	if (!m) {
		pthread_mutex_unlock(&g_dma_map_lock);
		fprintf(stderr,
			"[gc570d WARN] dma_free_coherent: unknown buffer %p (handle 0x%llx)\n",
			cpu_addr, (unsigned long long)dma_handle);
		return;
	}
	if (size && m->aligned_size != aligned)
		fprintf(stderr,
			"[gc570d WARN] dma_free_coherent: size %zu != mapped %zu for %p\n",
			size, m->aligned_size, cpu_addr);

	if (g_dma_cached_blocks < GC570D_DMA_CACHE_BLOCKS &&
	    g_dma_cached_bytes + m->aligned_size <= GC570D_DMA_CACHE_BYTES) {
		m->in_use = false;
		g_dma_cached_blocks++;
		g_dma_cached_bytes += m->aligned_size;
	} else {
		gc570d_dma_unmap_locked(pdev, m);
	}
	pthread_mutex_unlock(&g_dma_map_lock);
}

int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{
	(void)dev;
	(void)mask;
	return 0;
}

void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{
	(void)dev;
	(void)gfp;
	return calloc(1, size ? size : 1);
}

int pcim_enable_device(struct pci_dev *pdev)
{
	uint16_t cmd = 0;
	uint16_t status = 0;
	if (pdev->vfio_device_fd >= 0) {
		/* Walk PCI capability list to transition from D3hot -> D0 after system sleep */
		if (pread(pdev->vfio_device_fd, &status, sizeof(status),
			  pdev->config_offset + PCI_STATUS) == sizeof(status) &&
		    (status & PCI_STATUS_CAP_LIST)) {
			uint8_t pos = 0;
			if (pread(pdev->vfio_device_fd, &pos, 1,
				  pdev->config_offset + PCI_CAPABILITY_LIST) == 1) {
				pos &= ~0x03U;
				for (int ttl = 0; ttl < 48 && pos >= 0x40; ttl++) {
					uint8_t cap_id = 0, next = 0;
					if (pread(pdev->vfio_device_fd, &cap_id, 1,
						  pdev->config_offset + pos) != 1 ||
					    pread(pdev->vfio_device_fd, &next, 1,
						  pdev->config_offset + pos + 1) != 1)
						break;
					if (cap_id == PCI_CAP_ID_PM) {
						uint16_t pmcsr = 0;
						if (pread(pdev->vfio_device_fd, &pmcsr, sizeof(pmcsr),
							  pdev->config_offset + pos + PCI_PM_CTRL) == sizeof(pmcsr) &&
						    (pmcsr & PCI_PM_CTRL_STATE_MASK) != 0) {
							dev_info(&pdev->dev,
								 "Waking PCI device from D%u to D0\n",
								 pmcsr & PCI_PM_CTRL_STATE_MASK);
							pmcsr = (pmcsr & ~PCI_PM_CTRL_STATE_MASK) | PCI_PM_CTRL_PME_STATUS;
							pwrite(pdev->vfio_device_fd, &pmcsr, sizeof(pmcsr),
							       pdev->config_offset + pos + PCI_PM_CTRL);
							usleep(20000);
						}
						break;
					}
					pos = next & ~0x03U;
				}
			}
		}

		if (pread(pdev->vfio_device_fd, &cmd, sizeof(cmd),
			  pdev->config_offset + PCI_COMMAND) == sizeof(cmd)) {
			cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
			cmd &= ~PCI_COMMAND_INTX_DISABLE;
			pwrite(pdev->vfio_device_fd, &cmd, sizeof(cmd),
			       pdev->config_offset + PCI_COMMAND);
		}
	}
	return 0;
}

void pci_disable_link_state(struct pci_dev *pdev, int state)
{
	(void)pdev;
	(void)state;
}

resource_size_t pci_resource_len(struct pci_dev *pdev, int bar)
{
	if (bar == 0)
		return pdev->bar0_size;
	return 0;
}

unsigned long pci_resource_flags(struct pci_dev *pdev, int bar)
{
	if (bar == 0)
		return pdev->resource[0].flags;
	return 0;
}

void __iomem *pcim_iomap_region(struct pci_dev *pdev, int bar, const char *name)
{
	(void)name;
	if (bar == 0 && pdev->bar0_vaddr)
		return pdev->bar0_vaddr;
	return ERR_PTR(-ENOMEM);
}

void pci_set_master(struct pci_dev *pdev)
{
	uint16_t cmd = 0;
	if (pdev && pdev->vfio_device_fd >= 0) {
		if (pread(pdev->vfio_device_fd, &cmd, sizeof(cmd),
			  pdev->config_offset + PCI_COMMAND) == sizeof(cmd)) {
			cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
			if (!pdev->msi_enabled)
				cmd &= ~PCI_COMMAND_INTX_DISABLE;
			pwrite(pdev->vfio_device_fd, &cmd, sizeof(cmd),
			       pdev->config_offset + PCI_COMMAND);
		}
	}
}

void pci_clear_master(struct pci_dev *pdev)
{
	/* Keep Bus Master enabled across VFIO sessions so concurrent DMA channels never drop */
	(void)pdev;
}

/* VFIO Interrupt Handling */
static irq_handler_t g_irq_handler = NULL;
static void *g_irq_dev_id = NULL;
static _Atomic bool g_irq_enabled = false;
static _Atomic bool g_irq_thread_stop = false;
static pthread_t g_irq_thread;
static pthread_mutex_t g_irq_sync_lock = PTHREAD_MUTEX_INITIALIZER;

static void vfio_unmask_intx(struct pci_dev *pdev)
{
	if (pdev->vfio_device_fd >= 0 &&
	    pdev->irq_index == VFIO_PCI_INTX_IRQ_INDEX) {
		struct vfio_irq_set irq_set = {
			.argsz = sizeof(irq_set),
			.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK,
			.index = VFIO_PCI_INTX_IRQ_INDEX,
			.start = 0,
			.count = 1,
		};
		ioctl(pdev->vfio_device_fd, VFIO_DEVICE_SET_IRQS, &irq_set);
	}
}

static void *vfio_irq_thread_fn(void *arg)
{
	struct pci_dev *pdev = arg;

	pthread_setname_np(pthread_self(), "gc570d-vfio-irq");
	while (!atomic_load(&g_irq_thread_stop)) {
		bool triggered = false;

		if (pdev->irq_event_valid && pdev->msi_enabled) {
			struct pollfd pfd = {
				.fd = pdev->irq_eventfd,
				.events = POLLIN,
			};
			int pr = poll(&pfd, 1, 100);

			if (atomic_load(&g_irq_thread_stop))
				break;
			if (pr > 0 && (pfd.revents & POLLIN)) {
				uint64_t cnt;

				while (read(pdev->irq_eventfd, &cnt,
					    sizeof(cnt)) == sizeof(cnt))
					;
				triggered = true;
			}
		} else {
			/*
			 * INTx is level-triggered, so its VFIO eventfd stays
			 * silent whenever the line is still asserted (and some
			 * hosts never deliver the event at all).  A device with
			 * no eventfd has no notification either.  Poll the MMIO
			 * status every 2 ms in both cases; the handler is a
			 * no-op when the status register is clear.
			 */
			usleep(2000);
			triggered = true;
		}

		if (!triggered)
			continue;

		if (atomic_load(&g_irq_enabled) && g_irq_handler && g_irq_dev_id) {
			pthread_mutex_lock(&g_irq_sync_lock);
			if (atomic_load(&g_irq_enabled))
				g_irq_handler(1, g_irq_dev_id);
			pthread_mutex_unlock(&g_irq_sync_lock);
		}
		if (pdev->irq_event_valid)
			vfio_unmask_intx(pdev);
	}
	return NULL;
}

int pci_alloc_irq_vectors(struct pci_dev *pdev, unsigned int min_vecs,
			  unsigned int max_vecs, unsigned int flags)
{
	(void)min_vecs;
	(void)max_vecs;
	(void)flags;

	pdev->irq_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (pdev->irq_eventfd < 0)
		return -errno;
	pdev->irq_event_valid = false;

	/*
	 * Try MSI first.  On the GC570D the VFIO INTx path (mask/unmask via
	 * VFIO_DEVICE_SET_IRQS) can stall the bridge I2C engine: the HDMI IN 2
	 * probe then times out at its first receiver register write and the
	 * whole device fails to initialize.  MSI delivers the same bridge,
	 * DMA and audio interrupts reliably, so only fall back to INTx when
	 * the device does not advertise MSI.
	 */
	uint32_t indices[2] = {
		VFIO_PCI_MSI_IRQ_INDEX,
		VFIO_PCI_INTX_IRQ_INDEX,
	};
	for (int i = 0; i < 2; i++) {
		struct vfio_irq_info irq_info = {
			.argsz = sizeof(irq_info),
			.index = indices[i],
		};
		if (ioctl(pdev->vfio_device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info) == 0 &&
		    irq_info.count >= 1) {
			uint8_t buf[sizeof(struct vfio_irq_set) + sizeof(int)];
			struct vfio_irq_set *irq_set = (struct vfio_irq_set *)buf;
			memset(buf, 0, sizeof(buf));
			irq_set->argsz = sizeof(buf);
			irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
			irq_set->index = indices[i];
			irq_set->start = 0;
			irq_set->count = 1;
			*(int *)&irq_set->data[0] = pdev->irq_eventfd;
			if (ioctl(pdev->vfio_device_fd, VFIO_DEVICE_SET_IRQS, irq_set) == 0) {
				pdev->irq_index = indices[i];
				pdev->msi_enabled = (indices[i] == VFIO_PCI_MSI_IRQ_INDEX);
				pdev->irq_event_valid = true;
				dev_info(&pdev->dev,
					 "VFIO interrupt configured using %s\n",
					 pdev->msi_enabled ? "MSI" : "INTx");
				if (!pdev->msi_enabled)
					vfio_unmask_intx(pdev);
				return 1;
			}
		}
	}

	dev_warn(&pdev->dev,
		 "VFIO_DEVICE_SET_IRQS unavailable; using 2ms low-latency MMIO IRQ polling\n");
	pdev->msi_enabled = false;
	pdev->irq_event_valid = false;
	return 1;
}

int pci_irq_vector(struct pci_dev *pdev, unsigned int nr)
{
	(void)pdev;
	(void)nr;
	return 1;
}

bool pci_dev_msi_enabled(struct pci_dev *pdev)
{
	return pdev && pdev->msi_enabled;
}

void pci_free_irq_vectors(struct pci_dev *pdev)
{
	if (pdev->vfio_device_fd >= 0 && pdev->irq_eventfd >= 0) {
		struct vfio_irq_set irq_set = {
			.argsz = sizeof(irq_set),
			.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
			.index = pdev->irq_index,
			.start = 0,
			.count = 0,
		};
		ioctl(pdev->vfio_device_fd, VFIO_DEVICE_SET_IRQS, &irq_set);
	}
	if (pdev->irq_eventfd >= 0) {
		close(pdev->irq_eventfd);
		pdev->irq_eventfd = -1;
	}
	pdev->irq_event_valid = false;
}

int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
		const char *name, void *dev)
{
	(void)irq;
	(void)flags;
	(void)name;
	g_irq_handler = handler;
	g_irq_dev_id = dev;
	atomic_store(&g_irq_enabled, true);
	atomic_store(&g_irq_thread_stop, false);
	pthread_create(&g_irq_thread, NULL, vfio_irq_thread_fn, &g_pdev);
	return 0;
}

void free_irq(unsigned int irq, void *dev_id)
{
	(void)irq;
	(void)dev_id;
	atomic_store(&g_irq_thread_stop, true);
	if (g_pdev.irq_eventfd >= 0) {
		uint64_t val = 1;
		if (write(g_pdev.irq_eventfd, &val, sizeof(val)) < 0) {
			/* ignore */
		}
	}
	pthread_join(g_irq_thread, NULL);
	g_irq_handler = NULL;
	g_irq_dev_id = NULL;
}

void enable_irq(unsigned int irq)
{
	(void)irq;
	atomic_store(&g_irq_enabled, true);
	vfio_unmask_intx(&g_pdev);
}

void disable_irq(unsigned int irq)
{
	(void)irq;
	pthread_mutex_lock(&g_irq_sync_lock);
	atomic_store(&g_irq_enabled, false);
	pthread_mutex_unlock(&g_irq_sync_lock);
}

void synchronize_irq(unsigned int irq)
{
	(void)irq;
	pthread_mutex_lock(&g_irq_sync_lock);
	pthread_mutex_unlock(&g_irq_sync_lock);
}

struct platform_device *platform_device_register_resndata(
	struct device *parent, const char *name, int id,
	const void *res, unsigned int num, const void *data, size_t size)
{
	struct platform_device *pdev = calloc(1, sizeof(*pdev));
	(void)id;
	(void)res;
	(void)num;
	(void)data;
	(void)size;
	if (!pdev)
		return ERR_PTR(-ENOMEM);
	pdev->name = name;
	pdev->dev.name = name;
	pdev->dev.pdev = parent ? parent->pdev : &g_pdev;
	return pdev;
}

void platform_device_unregister(struct platform_device *pdev)
{
	free(pdev);
}

/* =========================================================================
 * 3. Userspace Diagnostics Registry & LED Controller State
 * ========================================================================= */

struct gc570d_diag_entry {
	char name[64];
	umode_t mode;
	void *data;
	const struct file_operations *fops;
};

#define GC570D_MAX_DIAG_ENTRIES 128
static struct gc570d_diag_entry g_diag_entries[GC570D_MAX_DIAG_ENTRIES];
static size_t g_diag_count = 0;
static struct dentry g_root_dentry = { .name = "gc570d" };
static const struct device_attribute *g_led_effect_attr = NULL;
static struct device g_led_dev = { .name = "gc570d:rgb:status" };

struct dentry *debugfs_create_dir(const char *name, struct dentry *parent)
{
	(void)name;
	(void)parent;
	return &g_root_dentry;
}

struct dentry *debugfs_create_file(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops)
{
	(void)parent;
	if (g_diag_count < GC570D_MAX_DIAG_ENTRIES && name && fops) {
		struct gc570d_diag_entry *e = &g_diag_entries[g_diag_count++];
		strscpy(e->name, name, sizeof(e->name));
		e->mode = mode;
		e->data = data;
		e->fops = fops;
	}
	return &g_root_dentry;
}

void debugfs_remove_recursive(struct dentry *dentry)
{
	(void)dentry;
	g_diag_count = 0;
}

int gc570d_user_diag_action(const char *name, const char *value)
{
	if (!name || !*name || !value)
		return -EINVAL;

	for (size_t i = 0; i < g_diag_count; i++) {
		struct file f;
		loff_t pos = 0;
		ssize_t r;

		if (strcmp(g_diag_entries[i].name, name) != 0 ||
		    !g_diag_entries[i].fops->write)
			continue;
		memset(&f, 0, sizeof(f));
		f.private_data = g_diag_entries[i].data;
		r = g_diag_entries[i].fops->write(&f, value, strlen(value),
						  &pos);
		return (int)r;
	}

	return -ENOENT;
}

int led_classdev_multicolor_register(struct device *parent, struct led_classdev_mc *mcled_cdev)
{
	(void)parent;
	g_led_dev.driver_data = &mcled_cdev->led_cdev;
	mcled_cdev->led_cdev.dev = &g_led_dev;
	return 0;
}

void led_classdev_multicolor_unregister(struct led_classdev_mc *mcled_cdev)
{
	(void)mcled_cdev;
	g_led_effect_attr = NULL;
}

int device_create_file(struct device *dev, const struct device_attribute *attr)
{
	(void)dev;
	g_led_effect_attr = attr;
	return 0;
}

void device_remove_file(struct device *dev, const struct device_attribute *attr)
{
	(void)dev;
	(void)attr;
	g_led_effect_attr = NULL;
}

/* =========================================================================
 * 4. Userspace Video (VB2) & Audio (ALSA PCM) -> PipeWire Media Publisher
 * ========================================================================= */

#define GC570D_USER_VIDEO_BUFFERS 4
#define GC570D_PW_CLIENTS_PER_PORT 2
#define GC570D_AUDIO_RING_CAPACITY (32 * 1024)

struct gc570d_user_video_stream;

#ifdef HAVE_LIBPIPEWIRE
struct gc570d_pw_video_endpoint {
	struct gc570d_user_video_stream *parent;
	int port_index;
	int ep_index;
	struct pw_stream *pw_stream;
	struct spa_hook stream_listener;
	struct spa_source *timer;
	struct spa_video_info_raw negotiated_format;
	bool format_valid;
	uint32_t pw_seq;
};
#endif

struct gc570d_user_video_stream {
	int port_index; /* 0 = HDMI 1, 1 = HDMI 2 */
	struct video_device *vdev;
	struct vb2_queue *vq;
	struct gc570d_buffer buffers[GC570D_USER_VIDEO_BUFFERS];
	void *shared_dma_vaddr;
	dma_addr_t shared_dma_iova;
	pthread_mutex_t frame_lock;
	uint8_t *latest_yuy2;
	size_t latest_size;
	uint32_t latest_width;
	uint32_t latest_height;
	uint64_t latest_seq;
	uint64_t latest_ts_ns;
	uint64_t last_pushed_seq;
	uint64_t last_trigger_ns;
#ifdef HAVE_LIBPIPEWIRE
	struct gc570d_pw_video_endpoint eps[GC570D_PW_CLIENTS_PER_PORT];
#endif
};

struct gc570d_user_audio_stream {
	int channel_index; /* 0 = HDMI 1, 1 = HDMI 2 */
	struct snd_card *card;
	struct snd_pcm *pcm;
	pthread_mutex_t ring_lock;
	uint8_t ring[GC570D_AUDIO_RING_CAPACITY];
	size_t head;
	size_t tail;
	size_t fill;
	uint64_t periods_published;
#ifdef HAVE_LIBPIPEWIRE
	struct pw_stream *pw_stream;
	struct spa_hook stream_listener;
	uint32_t negotiated_format;
	bool format_valid;
	bool prebuffering;
#endif
};

static struct gc570d_user_video_stream g_vstreams[2];
static struct gc570d_user_audio_stream g_astreams[2];

/*
 * The PipeWire process thread reads the video frame under frame_lock, so the
 * locks must survive a hardware reinit.  Initialize them exactly once instead
 * of calling pthread_mutex_init() on a possibly-held mutex.
 */
static pthread_once_t g_vstream_locks_once = PTHREAD_ONCE_INIT;

static void gc570d_vstream_locks_init(void)
{
	for (int i = 0; i < 2; i++)
		pthread_mutex_init(&g_vstreams[i].frame_lock, NULL);
}

#ifdef HAVE_LIBPIPEWIRE
static struct pw_thread_loop *g_pw_loop = NULL;
static struct pw_context *g_pw_context = NULL;
static struct pw_core *g_pw_core = NULL;
#endif

/* Fast clamped BT.709 YUV -> RGB helper */
static inline uint8_t clamp_u8(int v)
{
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static inline void yuv_to_rgb_bt709(int y, int u, int v,
				    uint8_t *r, uint8_t *g, uint8_t *b)
{
	int c = y - 16;
	int d = u - 128;
	int e = v - 128;
	if (c < 0)
		c = 0;
	*r = clamp_u8((298 * c + 459 * e + 128) >> 8);
	*g = clamp_u8((298 * c - 55 * d - 136 * e + 128) >> 8);
	*b = clamp_u8((298 * c + 541 * d + 128) >> 8);
}

/*
 * Convert 1920x1080 YUY2 source into negotiated PipeWire destination format
 * (supports YUY2, NV12, BGRx, BGRA, RGBA, RGBx at 1920x1080 or scaled 1280x720/640x360).
 */
#ifdef HAVE_LIBPIPEWIRE
static size_t gc570d_convert_frame(const uint8_t *src_yuy2,
				   uint32_t src_w, uint32_t src_h,
				   uint8_t *dst, size_t dst_max,
				   uint32_t dst_fmt,
				   uint32_t dst_w, uint32_t dst_h,
				   uint32_t *stride_out)
{
	if (!src_yuy2 || !dst || !dst_w || !dst_h)
		return 0;

	if (dst_fmt == SPA_VIDEO_FORMAT_YUY2) {
		size_t needed = (size_t)dst_w * dst_h * 2;
		if (dst_max < needed)
			return 0;
		*stride_out = dst_w * 2;
		if (dst_w == src_w && dst_h == src_h) {
			memcpy(dst, src_yuy2, needed);
			return needed;
		}
		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src_yuy2 + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 2;
			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				memcpy(drow + x * 2, srow + sx * 2, 4);
			}
		}
		return needed;
	}

	if (dst_fmt == SPA_VIDEO_FORMAT_NV12 || dst_fmt == SPA_VIDEO_FORMAT_I420) {
		size_t y_plane = (size_t)dst_w * dst_h;
		size_t uv_plane = y_plane / 2;
		size_t needed = y_plane + uv_plane;
		if (dst_max < needed)
			return 0;
		*stride_out = dst_w;
		uint8_t *y_dst = dst;
		uint8_t *u_dst = dst + y_plane;
		uint8_t *v_dst = dst + y_plane + y_plane / 4;

		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src_yuy2 + (size_t)sy * src_w * 2;
			uint8_t *yd = y_dst + (size_t)y * dst_w;
			bool even_y = (y & 1) == 0;

			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;
				yd[x]     = p[0];
				yd[x + 1] = p[2];
				if (even_y) {
					if (dst_fmt == SPA_VIDEO_FORMAT_NV12) {
						uint8_t *uvd = u_dst + (size_t)(y / 2) * dst_w + x;
						uvd[0] = p[1];
						uvd[1] = p[3];
					} else {
						u_dst[(y / 2) * (dst_w / 2) + (x / 2)] = p[1];
						v_dst[(y / 2) * (dst_w / 2) + (x / 2)] = p[3];
					}
				}
			}
		}
		return needed;
	}

	if (dst_fmt == SPA_VIDEO_FORMAT_BGRx ||
	    dst_fmt == SPA_VIDEO_FORMAT_BGRA ||
	    dst_fmt == SPA_VIDEO_FORMAT_RGBA ||
	    dst_fmt == SPA_VIDEO_FORMAT_RGBx) {
		size_t needed = (size_t)dst_w * dst_h * 4;
		bool is_bgr = (dst_fmt == SPA_VIDEO_FORMAT_BGRx ||
			       dst_fmt == SPA_VIDEO_FORMAT_BGRA);
		if (dst_max < needed)
			return 0;
		*stride_out = dst_w * 4;

		for (uint32_t y = 0; y < dst_h; y++) {
			uint32_t sy = (y * src_h) / dst_h;
			const uint8_t *srow = src_yuy2 + (size_t)sy * src_w * 2;
			uint8_t *drow = dst + (size_t)y * dst_w * 4;

			for (uint32_t x = 0; x < dst_w; x += 2) {
				uint32_t sx = ((x * src_w) / dst_w) & ~1U;
				const uint8_t *p = srow + sx * 2;
				uint8_t r0, g0, b0, r1, g1, b1;
				yuv_to_rgb_bt709(p[0], p[1], p[3], &r0, &g0, &b0);
				yuv_to_rgb_bt709(p[2], p[1], p[3], &r1, &g1, &b1);
				if (is_bgr) {
					drow[x * 4 + 0] = b0;
					drow[x * 4 + 1] = g0;
					drow[x * 4 + 2] = r0;
					drow[x * 4 + 3] = 255;
					drow[(x + 1) * 4 + 0] = b1;
					drow[(x + 1) * 4 + 1] = g1;
					drow[(x + 1) * 4 + 2] = r1;
					drow[(x + 1) * 4 + 3] = 255;
				} else {
					drow[x * 4 + 0] = r0;
					drow[x * 4 + 1] = g0;
					drow[x * 4 + 2] = b0;
					drow[x * 4 + 3] = 255;
					drow[(x + 1) * 4 + 0] = r1;
					drow[(x + 1) * 4 + 1] = g1;
					drow[(x + 1) * 4 + 2] = b1;
					drow[(x + 1) * 4 + 3] = 255;
				}
			}
		}
		return needed;
	}

	return 0;
}

static void update_video_stream_timer(struct gc570d_pw_video_endpoint *ep, bool active)
{
	if (!g_pw_loop || !ep || !ep->timer)
		return;
	if (!active) {
		pw_loop_update_timer(pw_thread_loop_get_loop(g_pw_loop),
				     ep->timer, NULL, NULL, false);
		return;
	}
	uint64_t interval_ns = 16666667ULL; /* default 60 fps */
	if (ep->format_valid &&
	    ep->negotiated_format.framerate.num > 0 &&
	    ep->negotiated_format.framerate.denom > 0) {
		interval_ns = (1000000000ULL * (uint64_t)ep->negotiated_format.framerate.denom) /
			      (uint64_t)ep->negotiated_format.framerate.num;
		if (interval_ns < 4000000ULL)
			interval_ns = 16666667ULL;
	}
	struct timespec timeout = { .tv_sec = 0, .tv_nsec = 1 };
	struct timespec interval = {
		.tv_sec  = (time_t)(interval_ns / 1000000000ULL),
		.tv_nsec = (long)(interval_ns % 1000000000ULL),
	};
	pw_loop_update_timer(pw_thread_loop_get_loop(g_pw_loop),
			     ep->timer, &timeout, &interval, false);
}

static void on_video_timer(void *data, uint64_t expirations)
{
	struct gc570d_pw_video_endpoint *ep = data;
	(void)expirations;
	if (!ep || !ep->pw_stream)
		return;
	if (pw_stream_get_state(ep->pw_stream, NULL) == PW_STREAM_STATE_STREAMING &&
	    pw_stream_is_driving(ep->pw_stream)) {
		pw_stream_trigger_process(ep->pw_stream);
	}
}

static void on_video_process(void *data)
{
	struct gc570d_pw_video_endpoint *ep = data;
	struct gc570d_user_video_stream *vs = ep ? ep->parent : NULL;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *dst;
	uint32_t stride = 0;
	size_t written = 0;

	if (!ep || !vs || !ep->pw_stream || !ep->format_valid)
		return;

	b = pw_stream_dequeue_buffer(ep->pw_stream);
	if (!b)
		return;

	buf = b->buffer;
	dst = buf->datas[0].data;
	if (!dst) {
		pw_stream_queue_buffer(ep->pw_stream, b);
		return;
	}

	uint32_t w = ep->negotiated_format.size.width ?
		     ep->negotiated_format.size.width : 1920;
	uint32_t h = ep->negotiated_format.size.height ?
		     ep->negotiated_format.size.height : 1080;
	uint32_t fmt = ep->negotiated_format.format ?
		       ep->negotiated_format.format : SPA_VIDEO_FORMAT_YUY2;

	pthread_mutex_lock(&vs->frame_lock);
	uint32_t src_w = vs->latest_width ? vs->latest_width : 1920;
	uint32_t src_h = vs->latest_height ? vs->latest_height : 1080;

	if (vs->latest_yuy2 &&
	    vs->latest_size >= (size_t)src_w * src_h * 2) {
		written = gc570d_convert_frame(vs->latest_yuy2, src_w, src_h,
					       dst, buf->datas[0].maxsize,
					       fmt, w, h, &stride);
		vs->last_pushed_seq = vs->latest_seq;
	}
	pthread_mutex_unlock(&vs->frame_lock);

	if (!written) {
		stride = (fmt == SPA_VIDEO_FORMAT_YUY2) ? (w * 2) :
			 (fmt == SPA_VIDEO_FORMAT_NV12 || fmt == SPA_VIDEO_FORMAT_I420) ? w : (w * 4);
		written = (fmt == SPA_VIDEO_FORMAT_NV12 || fmt == SPA_VIDEO_FORMAT_I420) ?
			  ((size_t)w * h * 3 / 2) : ((size_t)stride * h);
		if (written > buf->datas[0].maxsize)
			written = buf->datas[0].maxsize;
		memset(dst, 0, written);
	}

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size   = written;
	buf->datas[0].chunk->flags  = 0;

	struct spa_meta_header *mh = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*mh));
	if (mh) {
		uint64_t now_ns = pw_stream_get_nsec(ep->pw_stream);
		mh->pts = (int64_t)(now_ns ? now_ns : ktime_get_ns());
		mh->flags = 0;
		mh->seq = ep->pw_seq++;
		mh->dts_offset = 0;
	}

	struct spa_meta *md = spa_buffer_find_meta(buf, SPA_META_VideoDamage);
	if (md) {
		struct spa_meta_region *r = spa_meta_first(md);
		if (spa_meta_check(r, md)) {
			r->region.position = SPA_POINT(0, 0);
			r->region.size = SPA_RECTANGLE(w, h);
			r++;
		}
		if (spa_meta_check(r, md))
			r->region = SPA_REGION(0, 0, 0, 0);
	}

	pw_stream_queue_buffer(ep->pw_stream, b);
}

static void on_video_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct gc570d_pw_video_endpoint *ep = data;

	if (!ep || !param || id != SPA_PARAM_Format)
		return;

	if (spa_format_video_raw_parse(param, &ep->negotiated_format) < 0)
		return;

	ep->format_valid = true;
	uint32_t w = ep->negotiated_format.size.width ? ep->negotiated_format.size.width : 1920;
	uint32_t h = ep->negotiated_format.size.height ? ep->negotiated_format.size.height : 1080;
	uint32_t fmt = ep->negotiated_format.format ? ep->negotiated_format.format : SPA_VIDEO_FORMAT_YUY2;
	uint32_t stride = (fmt == SPA_VIDEO_FORMAT_YUY2) ? (w * 2) :
			  (fmt == SPA_VIDEO_FORMAT_NV12 || fmt == SPA_VIDEO_FORMAT_I420) ? w : (w * 4);
	size_t frame_bytes = (fmt == SPA_VIDEO_FORMAT_NV12 || fmt == SPA_VIDEO_FORMAT_I420) ?
			     ((size_t)w * h * 3 / 2) : ((size_t)stride * h);

	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[3];

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int((int32_t)frame_bytes),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int((int32_t)stride),
		SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));

	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	params[2] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
		SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
			sizeof(struct spa_meta_region) * 16,
			sizeof(struct spa_meta_region) * 1,
			sizeof(struct spa_meta_region) * 16));

	pw_stream_update_params(ep->pw_stream, params, 3);
	if (pw_stream_get_state(ep->pw_stream, NULL) == PW_STREAM_STATE_STREAMING)
		update_video_stream_timer(ep, true);

	fprintf(stderr,
		"[gc570d INFO] PipeWire HDMI %d Video (ep=%d) negotiated: %ux%u @ %u/%u fps (format=%u, bytes=%zu)\n",
		ep->port_index + 1, ep->ep_index + 1, w, h,
		ep->negotiated_format.framerate.num,
		ep->negotiated_format.framerate.denom ? ep->negotiated_format.framerate.denom : 1,
		fmt, frame_bytes);
}

static void on_video_state_changed(void *data, enum pw_stream_state old,
				   enum pw_stream_state state, const char *error)
{
	struct gc570d_pw_video_endpoint *ep = data;
	(void)old;
	fprintf(stderr, "[gc570d INFO] PipeWire HDMI %d Video (ep=%d) stream state: %s%s%s\n",
		ep->port_index + 1, ep->ep_index + 1, pw_stream_state_as_string(state),
		error ? " (" : "", error ? error : "");
	update_video_stream_timer(ep, state == PW_STREAM_STATE_STREAMING);
}

static const struct pw_stream_events g_video_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_video_state_changed,
	.param_changed = on_video_param_changed,
	.process = on_video_process,
};

static void on_audio_state_changed(void *data, enum pw_stream_state old,
				   enum pw_stream_state state, const char *error)
{
	struct gc570d_user_audio_stream *as = data;
	(void)old;
	fprintf(stderr, "[gc570d INFO] PipeWire HDMI %d Audio stream state: %s%s%s\n",
		as->channel_index + 1, pw_stream_state_as_string(state),
		error ? " (" : "", error ? error : "");
	if (state == PW_STREAM_STATE_STREAMING) {
		pthread_mutex_lock(&as->ring_lock);
		/* Keep a 3-period (30ms) jitter cushion so 1024-sample PipeWire quanta never underrun */
		size_t cushion = 3 * GC570D_AUDIO_PERIOD_BYTES;
		if (as->fill > cushion) {
			size_t excess = as->fill - cushion;
			as->tail = (as->tail + excess) % GC570D_AUDIO_RING_CAPACITY;
			as->fill = cushion;
		} else {
			as->prebuffering = true;
		}
		pthread_mutex_unlock(&as->ring_lock);
	}
}

static void on_audio_process(void *data)
{
	struct gc570d_user_audio_stream *as = data;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *dst;
	uint32_t n_frames;
	size_t n_bytes;

	if (!as || !as->pw_stream)
		return;

	b = pw_stream_dequeue_buffer(as->pw_stream);
	if (!b)
		return;

	buf = b->buffer;
	dst = buf->datas[0].data;
	if (!dst) {
		pw_stream_queue_buffer(as->pw_stream, b);
		return;
	}

	bool is_f32 = (as->negotiated_format == SPA_AUDIO_FORMAT_F32 ||
		       as->negotiated_format == SPA_AUDIO_FORMAT_F32_LE);
	uint32_t bytes_per_frame = is_f32 ? 8 : 4;

	n_frames = b->requested ? (uint32_t)b->requested : 480;
	if (n_frames * bytes_per_frame > buf->datas[0].maxsize)
		n_frames = buf->datas[0].maxsize / bytes_per_frame;
	n_bytes = (size_t)n_frames * bytes_per_frame;

	size_t s16_bytes_needed = (size_t)n_frames * 4;
	uint8_t s16_temp[8192 * 4];
	if (s16_bytes_needed > sizeof(s16_temp)) {
		s16_bytes_needed = sizeof(s16_temp);
		n_frames = 8192;
		n_bytes = (size_t)n_frames * bytes_per_frame;
	}

	pthread_mutex_lock(&as->ring_lock);
	size_t min_start_fill = s16_bytes_needed + 2 * GC570D_AUDIO_PERIOD_BYTES;
	if (as->prebuffering) {
		if (as->fill >= min_start_fill)
			as->prebuffering = false;
	} else if (as->fill < s16_bytes_needed) {
		/* Underrun occurred: re-establish 2-period (20ms) jitter cushion instead of crackling */
		as->prebuffering = true;
	}

	size_t to_copy = 0;
	if (!as->prebuffering && as->fill >= s16_bytes_needed) {
		to_copy = s16_bytes_needed;
		size_t first = GC570D_AUDIO_RING_CAPACITY - as->tail;
		if (first > to_copy)
			first = to_copy;
		memcpy(s16_temp, as->ring + as->tail, first);
		if (to_copy > first)
			memcpy(s16_temp + first, as->ring, to_copy - first);
		as->tail = (as->tail + to_copy) % GC570D_AUDIO_RING_CAPACITY;
		as->fill -= to_copy;
	}
	pthread_mutex_unlock(&as->ring_lock);

	if (to_copy < s16_bytes_needed)
		memset(s16_temp + to_copy, 0, s16_bytes_needed - to_copy);

	if (is_f32) {
		const int16_t *src16 = (const int16_t *)s16_temp;
		float *dst32 = (float *)dst;
		size_t total_samples = n_frames * 2;
		for (size_t i = 0; i < total_samples; i++) {
			dst32[i] = (float)src16[i] / 32768.0f;
		}
	} else {
		memcpy(dst, s16_temp, n_bytes);
	}

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = bytes_per_frame;
	buf->datas[0].chunk->size   = n_bytes;
	buf->datas[0].chunk->flags  = 0;

	struct spa_meta_header *mh = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*mh));
	if (mh) {
		mh->pts = (int64_t)ktime_get_ns();
		mh->flags = 0;
		mh->seq = as->periods_published;
		mh->dts_offset = 0;
	}

	pw_stream_queue_buffer(as->pw_stream, b);
}

static void on_audio_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct gc570d_user_audio_stream *as = data;
	if (!param || id != SPA_PARAM_Format)
		return;

	struct spa_audio_info_raw ainfo;
	if (spa_format_audio_raw_parse(param, &ainfo) < 0)
		return;

	as->negotiated_format = ainfo.format;
	as->format_valid = true;

	uint32_t bytes_per_sample = (ainfo.format == SPA_AUDIO_FORMAT_F32 ||
				     ainfo.format == SPA_AUDIO_FORMAT_F32_LE) ? 4 : 2;
	uint32_t frame_bytes = bytes_per_sample * 2;
	/* Allocate buffers large enough for any PipeWire quantum up to 8192 frames */
	uint32_t period_bytes = 8192 * frame_bytes;

	uint8_t params_buffer[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int((int32_t)period_bytes),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int((int32_t)frame_bytes),
		SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));

	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	pw_stream_update_params(as->pw_stream, params, 2);

	fprintf(stderr,
		"[gc570d INFO] PipeWire HDMI %d Audio stream format negotiated: fmt=%u (%s), rate=%u, channels=%u\n",
		as->channel_index + 1, ainfo.format,
		(ainfo.format == SPA_AUDIO_FORMAT_F32 || ainfo.format == SPA_AUDIO_FORMAT_F32_LE) ? "F32" : "S16_LE",
		ainfo.rate, ainfo.channels);
}

static const struct pw_stream_events g_audio_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_audio_state_changed,
	.param_changed = on_audio_param_changed,
	.process = on_audio_process,
};

static const struct spa_pod *build_video_format_pod_single(struct spa_pod_builder *b,
							   uint32_t fmt,
							   uint32_t width, uint32_t height)
{
	struct spa_pod_frame f[2];

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b,
		SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(fmt),
		SPA_FORMAT_VIDEO_size,   SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
		0);
	spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_framerate, 0);
	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
	spa_pod_builder_fraction(b, 60, 1);
	spa_pod_builder_fraction(b, 60, 1);
	spa_pod_builder_fraction(b, 50, 1);
	spa_pod_builder_fraction(b, 30, 1);
	spa_pod_builder_pop(b, &f[1]);
	return spa_pod_builder_pop(b, &f[0]);
}

static const struct spa_pod *build_video_format_pod_webrtc(struct spa_pod_builder *b,
							   uint32_t width, uint32_t height)
{
	struct spa_pod_frame f[2];

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b,
		SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		0);
	spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_format, 0);
	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
	spa_pod_builder_id(b, SPA_VIDEO_FORMAT_YUY2);
	spa_pod_builder_id(b, SPA_VIDEO_FORMAT_YUY2);
	spa_pod_builder_id(b, SPA_VIDEO_FORMAT_I420);
	spa_pod_builder_id(b, SPA_VIDEO_FORMAT_NV12);
	spa_pod_builder_id(b, SPA_VIDEO_FORMAT_BGRx);
	spa_pod_builder_id(b, SPA_VIDEO_FORMAT_BGRA);
	spa_pod_builder_id(b, SPA_VIDEO_FORMAT_RGBA);
	spa_pod_builder_id(b, SPA_VIDEO_FORMAT_RGBx);
	spa_pod_builder_pop(b, &f[1]);

	spa_pod_builder_add(b,
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
		SPA_FORMAT_VIDEO_framerate,
		SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(60, 1),
			&SPA_FRACTION(15, 1),
			&SPA_FRACTION(60, 1)),
		0);
	return spa_pod_builder_pop(b, &f[0]);
}

static int gc570d_pipewire_init(void)
{
	pw_init(NULL, NULL);
	g_pw_loop = pw_thread_loop_new("gc570d-pw", NULL);
	if (!g_pw_loop)
		return -ENOMEM;

	g_pw_context = pw_context_new(pw_thread_loop_get_loop(g_pw_loop), NULL, 0);
	if (!g_pw_context)
		return -ENOMEM;

	if (pw_thread_loop_start(g_pw_loop) < 0)
		return -EIO;

	pw_thread_loop_lock(g_pw_loop);
	g_pw_core = pw_context_connect(g_pw_context, NULL, 0);
	if (!g_pw_core) {
		pw_thread_loop_unlock(g_pw_loop);
		fprintf(stderr,
			"[gc570d WARN] Could not connect to PipeWire daemon (is XDG_RUNTIME_DIR set?)\n");
		return -ECONNREFUSED;
	}

	/* Create Video/Source Camera streams (2 endpoints per HDMI port so OBS and Discord can stream simultaneously) */
	for (int i = 0; i < 2; i++) {
		struct gc570d_user_video_stream *vs = &g_vstreams[i];
		for (int ep_idx = 0; ep_idx < GC570D_PW_CLIENTS_PER_PORT; ep_idx++) {
			struct gc570d_pw_video_endpoint *ep = &vs->eps[ep_idx];
			ep->parent = vs;
			ep->port_index = i;
			ep->ep_index = ep_idx;

			char node_name[64];
			char node_desc[64];
			char node_nick[64];
			if (ep_idx == 0) {
				snprintf(node_name, sizeof(node_name), "gc570d_hdmi%d_video", i + 1);
				snprintf(node_desc, sizeof(node_desc),
					 i == 0 ? "GC570D HDMI 1 (Passthrough)" : "GC570D HDMI 2 (Capture)");
				snprintf(node_nick, sizeof(node_nick), "GC570D HDMI %d", i + 1);
			} else {
				snprintf(node_name, sizeof(node_name), "gc570d_hdmi%d_video_discord", i + 1);
				snprintf(node_desc, sizeof(node_desc),
					 i == 0 ? "GC570D HDMI 1 (Discord / App 2)" : "GC570D HDMI 2 (Discord / App 2)");
				snprintf(node_nick, sizeof(node_nick), "GC570D HDMI %d (Stream 2)", i + 1);
			}

			ep->timer = pw_loop_add_timer(pw_thread_loop_get_loop(g_pw_loop),
						      on_video_timer, ep);

			struct pw_properties *props = pw_properties_new(
				PW_KEY_MEDIA_TYPE, "Video",
				PW_KEY_MEDIA_CATEGORY, "Source",
				PW_KEY_MEDIA_ROLE, "Camera",
				PW_KEY_MEDIA_CLASS, "Video/Source",
				PW_KEY_NODE_NAME, node_name,
				PW_KEY_NODE_DESCRIPTION, node_desc,
				PW_KEY_NODE_NICK, node_nick,
				PW_KEY_DEVICE_API, "vfio-pci",
				"node.autoconnect", "false",
				NULL);

			ep->pw_stream = pw_stream_new(g_pw_core, node_desc, props);
			if (ep->pw_stream) {
				pw_stream_add_listener(ep->pw_stream, &ep->stream_listener,
						       &g_video_stream_events, ep);
				uint8_t buf[8192];
				struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
				static const uint32_t fmts[5] = {
					SPA_VIDEO_FORMAT_YUY2,
					SPA_VIDEO_FORMAT_I420,
					SPA_VIDEO_FORMAT_BGRx,
					SPA_VIDEO_FORMAT_RGBA,
					SPA_VIDEO_FORMAT_NV12,
				};
				static const uint32_t dims[3][2] = {
					{ 1920, 1080 },
					{ 1280, 720 },
					{ 640, 360 },
				};
				const struct spa_pod *params[18];
				uint32_t n_params = 0;
				/* Choice+range pods first (for Discord / WebRTC) + per-format pods (for OBS) */
				for (int d_idx = 0; d_idx < 3; d_idx++) {
					params[n_params++] = build_video_format_pod_webrtc(
						&b, dims[d_idx][0], dims[d_idx][1]);
				}
				for (int f_idx = 0; f_idx < 5; f_idx++) {
					for (int d_idx = 0; d_idx < 3; d_idx++) {
						params[n_params++] = build_video_format_pod_single(
							&b, fmts[f_idx],
							dims[d_idx][0], dims[d_idx][1]);
					}
				}

				pw_stream_connect(ep->pw_stream,
						  PW_DIRECTION_OUTPUT,
						  PW_ID_ANY,
						  PW_STREAM_FLAG_MAP_BUFFERS |
						  PW_STREAM_FLAG_DRIVER,
						  params, n_params);
			}
		}
	}

	/* Create 2 Audio/Source Capture streams (HDMI 1 Audio and HDMI 2 Audio) */
	for (int i = 0; i < 2; i++) {
		struct gc570d_user_audio_stream *as = &g_astreams[i];
		const char *node_name = (i == 0) ? "gc570d_hdmi1_audio" : "gc570d_hdmi2_audio";
		const char *node_desc = (i == 0) ? "GC570D HDMI 1 Audio" : "GC570D HDMI 2 Audio";
		const char *node_nick = (i == 0) ? "GC570D HDMI 1 Audio" : "GC570D HDMI 2 Audio";

		struct pw_properties *props = pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Source",
			PW_KEY_MEDIA_ROLE, "Capture",
			PW_KEY_MEDIA_CLASS, "Audio/Source",
			PW_KEY_NODE_NAME, node_name,
			PW_KEY_NODE_DESCRIPTION, node_desc,
			PW_KEY_NODE_NICK, node_nick,
			"audio.rate", "48000",
			"audio.channels", "2",
			"audio.position", "FL,FR",
			"device.class", "sound",
			"node.autoconnect", "false",
			NULL);

		as->pw_stream = pw_stream_new(g_pw_core, node_desc, props);
		if (as->pw_stream) {
			pw_stream_add_listener(as->pw_stream, &as->stream_listener,
					       &g_audio_stream_events, as);
			uint8_t buf[512];
			struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
			const struct spa_pod *params[1];
			struct spa_pod_frame f[2];
			spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
			spa_pod_builder_add(&b,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				0);
			spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_format, 0);
			spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Enum, 0);
			spa_pod_builder_id(&b, SPA_AUDIO_FORMAT_S16_LE);
			spa_pod_builder_id(&b, SPA_AUDIO_FORMAT_S16_LE);
			spa_pod_builder_id(&b, SPA_AUDIO_FORMAT_F32);
			spa_pod_builder_id(&b, SPA_AUDIO_FORMAT_F32_LE);
			spa_pod_builder_pop(&b, &f[1]);

			spa_pod_builder_add(&b,
				SPA_FORMAT_AUDIO_rate, SPA_POD_Int(48000),
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2),
				0);
			params[0] = spa_pod_builder_pop(&b, &f[0]);

			pw_stream_connect(as->pw_stream,
					  PW_DIRECTION_OUTPUT,
					  PW_ID_ANY,
					  PW_STREAM_FLAG_MAP_BUFFERS |
					  PW_STREAM_FLAG_RT_PROCESS,
					  params, 1);
		}
	}

	pw_thread_loop_unlock(g_pw_loop);
	fprintf(stderr,
		"[gc570d INFO] Published 4 native PipeWire nodes: HDMI 1/2 Video (YUY2/NV12/BGRx/RGBA) + HDMI 1/2 Audio (48kHz S16_LE)\n");
	return 0;
}

static void gc570d_pipewire_shutdown(void)
{
	if (!g_pw_loop)
		return;
	pw_thread_loop_lock(g_pw_loop);
	for (int i = 0; i < 2; i++) {
		for (int ep_idx = 0; ep_idx < GC570D_PW_CLIENTS_PER_PORT; ep_idx++) {
			if (g_vstreams[i].eps[ep_idx].pw_stream) {
				pw_stream_destroy(g_vstreams[i].eps[ep_idx].pw_stream);
				g_vstreams[i].eps[ep_idx].pw_stream = NULL;
			}
		}
		if (g_astreams[i].pw_stream) {
			pw_stream_destroy(g_astreams[i].pw_stream);
			g_astreams[i].pw_stream = NULL;
		}
	}
	if (g_pw_core) {
		pw_core_disconnect(g_pw_core);
		g_pw_core = NULL;
	}
	pw_thread_loop_unlock(g_pw_loop);
	pw_thread_loop_stop(g_pw_loop);
	if (g_pw_context) {
		pw_context_destroy(g_pw_context);
		g_pw_context = NULL;
	}
	pw_thread_loop_destroy(g_pw_loop);
	g_pw_loop = NULL;
	pw_deinit();
}
#endif /* HAVE_LIBPIPEWIRE */

/* VB2 & Video Device Callbacks */
int vb2_queue_init(struct vb2_queue *vq)
{
	vq->streaming = false;
	return 0;
}

void vb2_queue_error(struct vb2_queue *vq)
{
	(void)vq;
}

void vb2_buffer_done(struct vb2_buffer *vb, enum vb2_buffer_state state)
{
	struct vb2_queue *vq = vb ? vb->vb2_queue : NULL;
	if (!vq)
		return;

	int port = vq->port_index;
	if (port >= 0 && port < 2 && state == VB2_BUF_STATE_DONE &&
	    vb->vaddr && vb->payload > 0 && !atomic_load(&g_daemon_quit)) {
		struct gc570d_user_video_stream *vs = &g_vstreams[port];
		pthread_mutex_lock(&vs->frame_lock);
		if (vs->latest_yuy2 && vb->payload <= vs->latest_size) {
			u32 frame_w = GC570D_WIDTH;
			u32 frame_h = GC570D_HEIGHT;

			if (port == 0 && g_gc) {
				frame_w = READ_ONCE(g_gc->video0_width);
				frame_h = READ_ONCE(g_gc->video0_height);
			}
			if ((size_t)frame_w * frame_h * 2 == vb->payload) {
				memcpy(vs->latest_yuy2, vb->vaddr, vb->payload);
				vs->latest_width = frame_w;
				vs->latest_height = frame_h;
			}
			vs->latest_seq++;
			vs->latest_ts_ns = vb->timestamp;
			if (g_framestore &&
			    vb->payload <= everlive_framestore_frame_stride(g_framestore) &&
			    (size_t)frame_w * frame_h * 2 == vb->payload) {
				uint32_t no_signal = g_gc ?
					(port == 0 ? READ_ONCE(g_gc->video0_no_signal) :
						     READ_ONCE(g_gc->video_no_signal)) : 0;

				everlive_framestore_publish(g_framestore,
							    (unsigned int)port,
							    vb->vaddr, vb->payload,
							    frame_w, frame_h,
							    no_signal != 0, true);
			}
		}
		pthread_mutex_unlock(&vs->frame_lock);


		/* Re-queue buffer back to hardware capture thread */
		if (vq->streaming && vq->ops && vq->ops->buf_queue)
			vq->ops->buf_queue(vb);
	}
}

int video_register_device(struct video_device *vdev, int type, int nr)
{
	(void)type;
	(void)nr;
	int port = (strstr(vdev->name, "hdmi1") != NULL) ? 0 : 1;
	vdev->num = port;
	if (vdev->queue)
		vdev->queue->port_index = port;

	struct gc570d_user_video_stream *vs = &g_vstreams[port];
	vs->port_index = port;
	vs->vdev = vdev;
	vs->vq = vdev->queue;
	pthread_once(&g_vstream_locks_once, gc570d_vstream_locks_init);
	pthread_mutex_lock(&vs->frame_lock);
	free(vs->latest_yuy2);
	vs->latest_size = GC570D_DMA_FRAME_SIZE;
	vs->latest_width = GC570D_WIDTH;
	vs->latest_height = GC570D_HEIGHT;
	vs->latest_yuy2 = calloc(1, vs->latest_size);
	if (vs->latest_yuy2) {
		for (size_t i = 0; i < vs->latest_size; i += 4) {
			vs->latest_yuy2[i + 0] = 16;
			vs->latest_yuy2[i + 1] = 128;
			vs->latest_yuy2[i + 2] = 16;
			vs->latest_yuy2[i + 3] = 128;
		}
	}
	pthread_mutex_unlock(&vs->frame_lock);
	return 0;
}

void video_unregister_device(struct video_device *vdev)
{
	if (!vdev || !vdev->queue)
		return;
	int port = vdev->queue->port_index;
	if (port < 0 || port >= 2)
		return;
	struct gc570d_user_video_stream *vs = &g_vstreams[port];
	if (vs->vq && vs->vq->streaming) {
		vs->vq->streaming = false;
		if (vs->vq->ops && vs->vq->ops->stop_streaming)
			vs->vq->ops->stop_streaming(vs->vq);
	}
	if (vs->shared_dma_vaddr) {
		dma_free_coherent(vs->vq->dev, GC570D_DMA_FRAME_SIZE,
				  vs->shared_dma_vaddr, vs->shared_dma_iova);
		vs->shared_dma_vaddr = NULL;
		vs->shared_dma_iova = 0;
	}
	for (int i = 0; i < GC570D_USER_VIDEO_BUFFERS; i++) {
		struct vb2_buffer *vb = &vs->buffers[i].vb.vb2_buf;
		if (port == 0 && vb->vaddr)
			free(vb->vaddr);
		vb->vaddr = NULL;
		vb->dma_addr = 0;
	}
	/*
	 * Keep the PipeWire reader from touching the old buffer after it is
	 * released, and never re-initialize a mutex that may be held by the
	 * PipeWire thread during a reinit.
	 */
	pthread_mutex_lock(&vs->frame_lock);
	free(vs->latest_yuy2);
	vs->latest_yuy2 = NULL;
	vs->latest_size = 0;
	vs->latest_width = 0;
	vs->latest_height = 0;
	pthread_mutex_unlock(&vs->frame_lock);
}

static int gc570d_start_user_video_streams(void)
{
	int first_err = 0;

	for (int port = 0; port < 2; port++) {
		struct gc570d_user_video_stream *vs = &g_vstreams[port];
		if (!vs->vq || !vs->vq->ops)
			continue;
		if (port == 1) {
			vs->shared_dma_vaddr = dma_alloc_coherent(
				vs->vq->dev, GC570D_DMA_FRAME_SIZE,
				&vs->shared_dma_iova, GFP_KERNEL);
			if (!vs->shared_dma_vaddr) {
				first_err = -ENOMEM;
				continue;
			}
		}
		for (int i = 0; i < GC570D_USER_VIDEO_BUFFERS; i++) {
			struct vb2_buffer *vb = &vs->buffers[i].vb.vb2_buf;
			vb->vb2_queue = vs->vq;
			vb->index = i;
			vb->plane_size = GC570D_DMA_FRAME_SIZE;
			vb->payload = GC570D_DMA_FRAME_SIZE;
			if (port == 0) {
				vb->vaddr = calloc(1, GC570D_DMA_FRAME_SIZE);
				vb->dma_addr = 0;
			} else {
				vb->vaddr = vs->shared_dma_vaddr;
				vb->dma_addr = vs->shared_dma_iova;
			}
			if (!vb->vaddr) {
				first_err = -ENOMEM;
				break;
			}
			INIT_LIST_HEAD(&vs->buffers[i].list);
			vs->vq->ops->buf_queue(vb);
		}
		vs->vq->streaming = true;
		int ret = vs->vq->ops->start_streaming(vs->vq, GC570D_USER_VIDEO_BUFFERS);
		if (ret) {
			vs->vq->streaming = false;
			if (!first_err)
				first_err = ret;
		}
	}
	return first_err;
}

/* ALSA PCM Callbacks */
static int g_next_card_num = 0;
static pthread_once_t g_astream_locks_once = PTHREAD_ONCE_INIT;

static void gc570d_astream_locks_init(void)
{
	for (int i = 0; i < 2; i++)
		pthread_mutex_init(&g_astreams[i].ring_lock, NULL);
}

int snd_card_new(struct device *parent, int idx, const char *xid, void *module,
		 int extra_size, struct snd_card **card_ret)
{
	struct snd_card *card = calloc(1, sizeof(*card));
	(void)parent;
	(void)idx;
	(void)module;
	(void)extra_size;
	if (!card)
		return -ENOMEM;
	card->number = g_next_card_num++;
	if (xid)
		strscpy(card->id, xid, sizeof(card->id));
	*card_ret = card;
	return 0;
}

int snd_pcm_new(struct snd_card *card, const char *id, int device,
		int playback_count, int capture_count, struct snd_pcm **rpcm)
{
	struct snd_pcm *pcm = calloc(1, sizeof(*pcm));
	(void)device;
	(void)playback_count;
	(void)capture_count;
	if (!pcm)
		return -ENOMEM;
	pcm->card = card;
	card->pcm = pcm;
	if (id)
		strscpy(pcm->id, id, sizeof(pcm->id));
	pcm->runtime.dma_area = calloc(1, GC570D_AUDIO_PCM_BUFFER_MAX);
	pcm->runtime.buffer_size = GC570D_AUDIO_PCM_BUFFER_MAX / 4;
	pcm->runtime.frame_bits = 32;
	pcm->substream.pcm = pcm;
	pcm->substream.runtime = &pcm->runtime;
	pcm->substream.channel_index = card->number;
	*rpcm = pcm;
	return 0;
}

void snd_pcm_set_ops(struct snd_pcm *pcm, int direction, const struct snd_pcm_ops *ops)
{
	(void)direction;
	if (pcm)
		pcm->ops = ops;
}

int snd_pcm_set_managed_buffer_all(struct snd_pcm *pcm, int type, struct device *data,
				   size_t size, size_t max)
{
	(void)pcm;
	(void)type;
	(void)data;
	(void)size;
	(void)max;
	return 0;
}

int snd_jack_new(struct snd_card *card, const char *id, int type,
		 struct snd_jack **jack, bool initial_kctl, bool phantom_jack)
{
	struct snd_jack *j = calloc(1, sizeof(*j));
	(void)card;
	(void)type;
	(void)initial_kctl;
	(void)phantom_jack;
	if (!j)
		return -ENOMEM;
	if (id)
		strscpy(j->name, id, sizeof(j->name));
	*jack = j;
	return 0;
}

void snd_jack_report(struct snd_jack *jack, int status)
{
	if (jack)
		jack->status = status;
}

static struct snd_kcontrol g_dummy_kctl;

struct snd_kcontrol *snd_ctl_new1(const struct snd_kcontrol_new *ncontrol, void *private_data)
{
	(void)ncontrol;
	(void)private_data;
	return &g_dummy_kctl;
}

int snd_ctl_add(struct snd_card *card, struct snd_kcontrol *kcontrol)
{
	(void)card;
	(void)kcontrol;
	return 0;
}

int snd_card_register(struct snd_card *card)
{
	if (!card || !card->pcm)
		return -EINVAL;
	int ch = (strstr(card->id, "HDMI1") != NULL) ? 0 : 1;
	card->pcm->substream.channel_index = ch;
	struct gc570d_user_audio_stream *as = &g_astreams[ch];
	pthread_once(&g_astream_locks_once, gc570d_astream_locks_init);
	pthread_mutex_lock(&as->ring_lock);
	as->channel_index = ch;
	as->card = card;
	as->pcm = card->pcm;
	as->head = 0;
	as->tail = 0;
	as->fill = 0;
	as->periods_published = 0;
#ifdef HAVE_LIBPIPEWIRE
	as->prebuffering = true;
#endif
	pthread_mutex_unlock(&as->ring_lock);
	return 0;
}

void snd_pcm_period_elapsed(struct snd_pcm_substream *substream)
{
	if (!substream || !substream->runtime || !substream->runtime->dma_area)
		return;
	struct gc570d_dev *gc = snd_pcm_substream_chip(substream);
	if (!gc)
		return;
	int ch = substream->channel_index;
	if (ch < 0 || ch >= 2)
		return;

	size_t buffer_bytes = frames_to_bytes(substream->runtime, substream->runtime->buffer_size);
	size_t pos = (ch == 0) ? READ_ONCE(gc->audio0_pcm_pos) : READ_ONCE(gc->audio_pcm_pos);
	size_t start = (pos + buffer_bytes - GC570D_AUDIO_PERIOD_BYTES) % buffer_bytes;
	uint8_t period[GC570D_AUDIO_PERIOD_BYTES];
	size_t first = min_t(size_t, GC570D_AUDIO_PERIOD_BYTES, buffer_bytes - start);
	memcpy(period, substream->runtime->dma_area + start, first);
	if (first < GC570D_AUDIO_PERIOD_BYTES)
		memcpy(period + first, substream->runtime->dma_area,
		       GC570D_AUDIO_PERIOD_BYTES - first);

	struct gc570d_user_audio_stream *as = &g_astreams[ch];
	pthread_mutex_lock(&as->ring_lock);
	/* Allow up to 8 periods (80ms = 3840 frames) before trimming so 1024/2048-frame
	 * PipeWire quanta never underrun, while keeping latency low (~30-50ms).
	 */
	if (as->fill > 8 * GC570D_AUDIO_PERIOD_BYTES) {
		size_t target = 4 * GC570D_AUDIO_PERIOD_BYTES;
		size_t excess = as->fill - target;
		as->tail = (as->tail + excess) % GC570D_AUDIO_RING_CAPACITY;
		as->fill = target;
	}
	if (as->fill + GC570D_AUDIO_PERIOD_BYTES > GC570D_AUDIO_RING_CAPACITY) {
		size_t drop = (as->fill + GC570D_AUDIO_PERIOD_BYTES) - GC570D_AUDIO_RING_CAPACITY;
		as->tail = (as->tail + drop) % GC570D_AUDIO_RING_CAPACITY;
		as->fill -= drop;
	}
	size_t wfirst = GC570D_AUDIO_RING_CAPACITY - as->head;
	if (wfirst > GC570D_AUDIO_PERIOD_BYTES)
		wfirst = GC570D_AUDIO_PERIOD_BYTES;
	memcpy(as->ring + as->head, period, wfirst);
	if (GC570D_AUDIO_PERIOD_BYTES > wfirst)
		memcpy(as->ring, period + wfirst, GC570D_AUDIO_PERIOD_BYTES - wfirst);
	as->head = (as->head + GC570D_AUDIO_PERIOD_BYTES) % GC570D_AUDIO_RING_CAPACITY;
	as->fill += GC570D_AUDIO_PERIOD_BYTES;
	as->periods_published++;
	pthread_mutex_unlock(&as->ring_lock);

	gc570d_everlive_push_audio(ch, period, GC570D_AUDIO_PERIOD_BYTES);
}

void snd_pcm_stop_xrun(struct snd_pcm_substream *substream)
{
	(void)substream;
}

static int gc570d_start_user_audio_streams(void)
{
	for (int ch = 0; ch < 2; ch++) {
		struct gc570d_user_audio_stream *as = &g_astreams[ch];
		if (!as->pcm || !as->pcm->ops)
			continue;
		struct snd_pcm_substream *sub = &as->pcm->substream;
		if (as->pcm->ops->open)
			as->pcm->ops->open(sub);
		if (as->pcm->ops->prepare) {
			int ret = as->pcm->ops->prepare(sub);
			if (ret) {
				fprintf(stderr, "[gc570d WARN] Audio channel %d prepare failed: %d\n", ch, ret);
				continue;
			}
		}
		if (as->pcm->ops->trigger)
			as->pcm->ops->trigger(sub, SNDRV_PCM_TRIGGER_START);
	}
	return 0;
}

void snd_pcm_suspend_all(struct snd_pcm *pcm)
{
	if (!pcm || !pcm->ops)
		return;
	struct snd_pcm_substream *sub = &pcm->substream;
	if (pcm->ops->trigger)
		pcm->ops->trigger(sub, SNDRV_PCM_TRIGGER_STOP);
	if (pcm->ops->hw_free)
		pcm->ops->hw_free(sub);
	if (pcm->ops->close)
		pcm->ops->close(sub);
}

void snd_card_disconnect_sync(struct snd_card *card)
{
	(void)card;
}

void snd_card_free(struct snd_card *card)
{
	if (!card)
		return;
	for (int i = 0; i < 2; i++) {
		if (g_astreams[i].card == card) {
			g_astreams[i].card = NULL;
			g_astreams[i].pcm = NULL;
		}
	}
	if (card->pcm) {
		free(card->pcm->runtime.dma_area);
		free(card->pcm);
	}
	free(card);
}

/*
 * Snapshot for the JSON-RPC status.get handler.  Monotonic counters are read
 * without the stream locks: they only ever advance, and a torn sample would
 * at worst under-report a frame count for one poll interval.
 */
void gc570d_user_pipeline_stats(struct gc570d_jsonrpc_stats *out)
{
	if (!out)
		return;

	memset(out, 0, sizeof(*out));
	for (int i = 0; i < 2; i++) {
		out->video_seq[i] = g_vstreams[i].latest_seq;
		out->audio_periods[i] = g_astreams[i].periods_published;
		out->v4l2_configured[i] = gc570d_everlive_v4l2(i)[0] != '\0';
	}
	if (g_gc) {
		out->irq_dma0 = (uint64_t)atomic64_read(&g_gc->irq_dma0);
		out->irq_dma1 = (uint64_t)atomic64_read(&g_gc->irq_dma1);
	}
}

/* =========================================================================
 * 5. UNIX Control & Diagnostic Socket (/run/user/$UID/gc570d/control.sock)
 * ========================================================================= */

static int g_ctl_listen_fd = -1;
static char g_ctl_sock_path[280];
static char g_ctl_dir_path[256];

static void gc570d_get_runtime_dir(char *buf, size_t size)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	if (xdg && xdg[0])
		snprintf(buf, size, "%s/gc570d", xdg);
	else
		snprintf(buf, size, "/tmp/gc570d-%u", (unsigned int)geteuid());
}

static void gc570d_handle_ctl_client(int client_fd)
{
	char cmd[256] = { 0 };
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	struct pollfd pfd = {
		.fd = client_fd,
		.events = POLLIN,
	};
	ssize_t n;

	/*
	 * Bound every operation on the client socket: a client that connects
	 * and never speaks, or never drains its response, must not stall the
	 * daemon's single-threaded control loop.
	 */
	setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (poll(&pfd, 1, 250) <= 0 || !(pfd.revents & POLLIN))
		return;
	n = read(client_fd, cmd, sizeof(cmd) - 1);
	if (n <= 0)
		return;
	while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r'))
		cmd[--n] = '\0';

	FILE *fp = fdopen(dup(client_fd), "w");
	if (!fp)
		return;

	if (strcmp(cmd, "status") == 0) {
		fprintf(fp, "=== OpenGC570D Userspace Driver (VFIO-PCI + PipeWire) ===\n");
		fprintf(fp, "pci_device:        %s (%s)\n",
			g_pdev.bdf, g_pdev.msi_enabled ? "MSI" : "INTx");
		fprintf(fp, "bar0_size:         0x%zx\n", g_pdev.bar0_size);
		if (g_gc) {
			fprintf(fp, "hdmi1_auto_ready:  %s (phase=%u)\n",
				READ_ONCE(g_gc->hdmi1_auto_ready) ? "yes" : "no",
				READ_ONCE(g_gc->hdmi1_auto_phase));
			fprintf(fp, "hdmi1_video:       streaming=%s no_signal=%s frames=%llu (%ux%u)\n",
				READ_ONCE(g_gc->video0_streaming) ? "yes" : "no",
				READ_ONCE(g_gc->video0_no_signal) ? "yes" : "no",
				(unsigned long long)g_vstreams[0].latest_seq,
				g_gc->video0_width, g_gc->video0_height);
			fprintf(fp, "hdmi2_video:       streaming=%s no_signal=%s frames=%llu (1920x1080)\n",
				READ_ONCE(g_gc->video_streaming) ? "yes" : "no",
				READ_ONCE(g_gc->video_no_signal) ? "yes" : "no",
				(unsigned long long)g_vstreams[1].latest_seq);
			fprintf(fp, "hdmi1_audio:       running=%s recovering=%s periods=%llu\n",
				READ_ONCE(g_gc->audio0_running) ? "yes" : "no",
				READ_ONCE(g_gc->audio0_recovering) ? "yes" : "no",
				(unsigned long long)g_astreams[0].periods_published);
			fprintf(fp, "hdmi2_audio:       running=%s recovering=%s periods=%llu\n",
				READ_ONCE(g_gc->audio_running) ? "yes" : "no",
				READ_ONCE(g_gc->audio_recovering) ? "yes" : "no",
				(unsigned long long)g_astreams[1].periods_published);
			fprintf(fp, "irq_totals:        total=%lld dma0=%lld dma1=%lld audio0=%lld audio1=%lld\n",
				(long long)atomic64_read(&g_gc->irq_total),
				(long long)atomic64_read(&g_gc->irq_dma0),
				(long long)atomic64_read(&g_gc->irq_dma1),
				(long long)atomic64_read(&g_gc->irq_audio0),
				(long long)atomic64_read(&g_gc->irq_audio));
		}
	} else if (strcmp(cmd, "list") == 0) {
		for (size_t i = 0; i < g_diag_count; i++) {
			fprintf(fp, "%-42s (%s%s)\n",
				g_diag_entries[i].name,
				g_diag_entries[i].fops->show_fn || g_diag_entries[i].fops->read ? "R" : "",
				g_diag_entries[i].fops->write ? "W" : "");
		}
	} else if (strncmp(cmd, "show ", 5) == 0) {
		const char *target = cmd + 5;
		bool found = false;
		for (size_t i = 0; i < g_diag_count; i++) {
			if (strcmp(g_diag_entries[i].name, target) == 0 &&
			    g_diag_entries[i].fops->show_fn) {
				struct seq_file s = {
					.fp = fp,
					.private = g_diag_entries[i].data,
				};
				g_diag_entries[i].fops->show_fn(&s, NULL);
				found = true;
				break;
			}
		}
		if (!found)
			fprintf(fp, "Unknown or write-only diagnostic node: %s\n", target);
	} else if (strncmp(cmd, "action ", 7) == 0) {
		char name[64] = { 0 };
		char val[64] = "1";
		int r;

		sscanf(cmd + 7, "%63s %63s", name, val);
		r = gc570d_user_diag_action(name, val);
		if (r == -ENOENT)
			fprintf(fp, "Unknown or read-only action node: %s\n", name);
		else
			fprintf(fp, "action %s(%s) -> %d\n", name, val, r);
	} else if (strncmp(cmd, "led ", 4) == 0) {
		const char *mode = cmd + 4;
		if (strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0) {
			for (size_t i = 0; i < g_diag_count; i++) {
				if (strcmp(g_diag_entries[i].name, "led_basic") == 0 &&
				    g_diag_entries[i].fops->write) {
					struct file f = { .private_data = g_diag_entries[i].data };
					loff_t pos = 0;
					g_diag_entries[i].fops->write(&f, "0", 1, &pos);
					fprintf(fp, "LED turned off\n");
					break;
				}
			}
		} else if (g_led_effect_attr && g_led_effect_attr->store) {
			ssize_t r = g_led_effect_attr->store(&g_led_dev,
				(struct device_attribute *)g_led_effect_attr,
				mode, strlen(mode));
			fprintf(fp, "LED effect '%s' -> %zd\n", mode, r);
		} else {
			fprintf(fp, "LED attribute not registered\n");
		}
	} else if (strncmp(cmd, "input ", 6) == 0) {
		const char *spec = cmd + 6;
		bool second = (spec[0] == '2' || strcmp(spec, "hdmi2") == 0 ||
			       strcmp(spec, "HDMI2") == 0);

		gc570d_everlive_set_port(second ? 1 : 0);
		fprintf(fp, "Frontend input -> HDMI IN %d\n", second ? 2 : 1);
	} else if (strncmp(cmd, "osd ", 4) == 0) {
		bool enabled = !(cmd[4] == '0' || strncmp(cmd + 4, "off", 3) == 0);

		gc570d_everlive_set_osd(enabled);
		fprintf(fp, "OSD %s\n", enabled ? "enabled" : "disabled");
	} else if (strncmp(cmd, "osd_pos ", 8) == 0) {
		const char *pos = cmd + 8;

		gc570d_everlive_set_osd_pos(pos);
		fprintf(fp, "OSD position -> %s\n", pos);
	} else if (strncmp(cmd, "pip ", 4) == 0) {
		bool enabled = !(cmd[4] == '0' || strncmp(cmd + 4, "off", 3) == 0);

		gc570d_everlive_set_pip(enabled);
		fprintf(fp, "Picture-in-picture %s\n",
			enabled ? "enabled" : "disabled");
	} else if (strncmp(cmd, "aspect ", 7) == 0) {
		const char *aspect = cmd + 7;

		gc570d_everlive_set_aspect(aspect);
		fprintf(fp, "Aspect ratio -> %s\n", aspect);
	} else if (strncmp(cmd, "scale ", 6) == 0) {
		const char *scale = cmd + 6;

		gc570d_everlive_set_scale(scale);
		fprintf(fp, "Scaling mode -> %s\n", scale);
	} else if (strncmp(cmd, "volume ", 7) == 0) {
		double v = strtod(cmd + 7, NULL);
		int pct = (v <= 1.5 && strchr(cmd + 7, '.') != NULL) ?
			  (int)(v * 100.0 + 0.5) : (int)(v + 0.5);

		gc570d_everlive_set_volume(pct);
		fprintf(fp, "Volume -> %d%%\n", pct);
	} else if (strncmp(cmd, "mute ", 5) == 0) {
		bool muted = !(cmd[5] == '0' || strncmp(cmd + 5, "off", 3) == 0);

		gc570d_everlive_set_mute(muted);
		fprintf(fp, "Audio %s\n", muted ? "muted" : "unmuted");
	} else if (strcmp(cmd, "reinit") == 0 || strcmp(cmd, "restart") == 0) {
		fprintf(fp, "Triggering hardware re-initialization...\n");
		g_reinit_requested = true;
	} else {
		fprintf(fp, "Commands: status | reinit | input <1|2> | osd <0|1> | osd_pos <tr|br|bl|tl> | pip <0|1> | aspect <auto|16:9|4:3|stretch> | scale <fit|integer> | volume <0..150> | mute <0|1> | list | show <node> | action <node> [1|0] | led <colorful|breathing|static|off>\n");
	}

	fclose(fp);
}

static int gc570d_ctl_server_init(void)
{
	struct sockaddr_un addr;

	gc570d_get_runtime_dir(g_ctl_dir_path, sizeof(g_ctl_dir_path));
	mkdir(g_ctl_dir_path, 0700);
	snprintf(g_ctl_sock_path, sizeof(g_ctl_sock_path), "%s/control.sock", g_ctl_dir_path);
	unlink(g_ctl_sock_path);

	g_ctl_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (g_ctl_listen_fd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strscpy(addr.sun_path, g_ctl_sock_path, sizeof(addr.sun_path));
	if (bind(g_ctl_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(g_ctl_listen_fd);
		g_ctl_listen_fd = -1;
		return -errno;
	}
	if (listen(g_ctl_listen_fd, 8) < 0) {
		int err = -errno;

		close(g_ctl_listen_fd);
		g_ctl_listen_fd = -1;
		return err;
	}
	/* Only the owning user may issue diagnostic or LED commands. */
	chmod(g_ctl_sock_path, 0600);
	return 0;
}

static void gc570d_ctl_server_shutdown(void)
{
	if (g_ctl_listen_fd >= 0) {
		close(g_ctl_listen_fd);
		g_ctl_listen_fd = -1;
	}
	if (g_ctl_sock_path[0])
		unlink(g_ctl_sock_path);
}

/* =========================================================================
 * 6. VFIO-PCI Setup & Main Daemon Loop
 * ========================================================================= */

/*
 * Card discovery lives in libeverlive-core.  1461:0054 is shared with the
 * GC573/GC555, so only a descriptor positively identified as "gc570d"
 * (subsystem 1461:5700) is accepted.  A missing subsystem file fails safe.
 */
static int find_gc570d_pci_bdf(char *bdf_out, size_t bdf_size)
{
	struct everlive_device cards[8];
	int count;

	count = everlive_detect_installed_cards(cards, ARRAY_SIZE(cards), NULL);
	for (int i = 0; i < count; i++) {
		if (strcmp(cards[i].desc->model_slug, "gc570d") != 0)
			continue;
		strscpy(bdf_out, cards[i].pci_bdf, bdf_size);
		return 0;
	}

	return -ENODEV;
}

static int gc570d_vfio_open(struct pci_dev *pdev, const char *requested_bdf)
{
	char iommu_link[512], resolved[512], group_dev_path[128];
	int group_id = -1;

	memset(pdev, 0, sizeof(*pdev));
	pdev->vfio_container_fd = -1;
	pdev->vfio_group_fd = -1;
	pdev->vfio_device_fd = -1;
	pdev->irq_eventfd = -1;
	pdev->irq_event_valid = false;
	pdev->dev.pdev = pdev;

	if (requested_bdf && requested_bdf[0]) {
		strscpy(pdev->bdf, requested_bdf, sizeof(pdev->bdf));
	} else {
		int r = find_gc570d_pci_bdf(pdev->bdf, sizeof(pdev->bdf));
		if (r < 0) {
			fprintf(stderr,
				"[gc570d ERR ] Could not find AVerMedia GC570D (PCI 1461:0054) in /sys/bus/pci/devices\n");
			return r;
		}
	}
	pdev->dev.name = pdev->bdf;

	snprintf(iommu_link, sizeof(iommu_link),
		 "/sys/bus/pci/devices/%s/iommu_group", pdev->bdf);
	ssize_t len = readlink(iommu_link, resolved, sizeof(resolved) - 1);
	if (len < 0) {
		fprintf(stderr,
			"[gc570d ERR ] PCI device %s has no iommu_group link (%s). Ensure IOMMU is enabled in BIOS/kernel.\n",
			pdev->bdf, strerror(errno));
		return -ENODEV;
	}
	resolved[len] = '\0';
	const char *slash = strrchr(resolved, '/');
	group_id = slash ? atoi(slash + 1) : -1;
	if (group_id < 0)
		return -EINVAL;

	pdev->vfio_container_fd = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (pdev->vfio_container_fd < 0) {
		fprintf(stderr,
			"[gc570d ERR ] Cannot open /dev/vfio/vfio: %s (run ./install.sh to load vfio-pci and set permissions)\n",
			strerror(errno));
		return -errno;
	}

	if (ioctl(pdev->vfio_container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
		fprintf(stderr, "[gc570d ERR ] Unsupported VFIO_API_VERSION\n");
		return -EINVAL;
	}

	snprintf(group_dev_path, sizeof(group_dev_path), "/dev/vfio/%d", group_id);
	pdev->vfio_group_fd = open(group_dev_path, O_RDWR | O_CLOEXEC);
	if (pdev->vfio_group_fd < 0) {
		fprintf(stderr,
			"[gc570d ERR ] Cannot open %s for PCI %s: %s\n"
			"              Ensure %s is bound to vfio-pci and permissions are applied via 99-gc570d-vfio.rules.\n",
			group_dev_path, pdev->bdf, strerror(errno), pdev->bdf);
		return -errno;
	}

	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	if (ioctl(pdev->vfio_group_fd, VFIO_GROUP_GET_STATUS, &group_status) < 0 ||
	    !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		fprintf(stderr,
			"[gc570d ERR ] VFIO group %d is not viable (all devices in the group must be bound to vfio-pci)\n",
			group_id);
		return -EBUSY;
	}

	if (ioctl(pdev->vfio_group_fd, VFIO_GROUP_SET_CONTAINER, &pdev->vfio_container_fd) < 0) {
		fprintf(stderr, "[gc570d ERR ] VFIO_GROUP_SET_CONTAINER failed: %s\n", strerror(errno));
		return -errno;
	}

	if (ioctl(pdev->vfio_container_fd, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) < 0 &&
	    ioctl(pdev->vfio_container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
		fprintf(stderr, "[gc570d ERR ] VFIO_SET_IOMMU (Type1v2/Type1) failed: %s\n", strerror(errno));
		return -errno;
	}

	pdev->vfio_device_fd = ioctl(pdev->vfio_group_fd, VFIO_GROUP_GET_DEVICE_FD, pdev->bdf);
	if (pdev->vfio_device_fd < 0) {
		fprintf(stderr, "[gc570d ERR ] VFIO_GROUP_GET_DEVICE_FD(%s) failed: %s\n",
			pdev->bdf, strerror(errno));
		return -errno;
	}

	struct vfio_region_info cfg_reg = {
		.argsz = sizeof(cfg_reg),
		.index = VFIO_PCI_CONFIG_REGION_INDEX,
	};
	if (ioctl(pdev->vfio_device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg_reg) == 0)
		pdev->config_offset = cfg_reg.offset;

	struct vfio_region_info bar0_reg = {
		.argsz = sizeof(bar0_reg),
		.index = VFIO_PCI_BAR0_REGION_INDEX,
	};
	if (ioctl(pdev->vfio_device_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_reg) < 0) {
		fprintf(stderr, "[gc570d ERR ] VFIO_DEVICE_GET_REGION_INFO(BAR0) failed: %s\n",
			strerror(errno));
		return -errno;
	}

	pdev->bar0_size = bar0_reg.size;
	pdev->resource[0].start = 0;
	pdev->resource[0].end = bar0_reg.size ? (bar0_reg.size - 1) : 0;
	pdev->resource[0].flags = IORESOURCE_MEM;

	pdev->bar0_vaddr = mmap(NULL, bar0_reg.size, PROT_READ | PROT_WRITE,
				MAP_SHARED, pdev->vfio_device_fd, bar0_reg.offset);
	if (pdev->bar0_vaddr == MAP_FAILED) {
		pdev->bar0_vaddr = NULL;
		fprintf(stderr, "[gc570d ERR ] mmap(BAR0, size=0x%llx) failed: %s\n",
			(unsigned long long)bar0_reg.size, strerror(errno));
		return -errno;
	}

	dev_info(&pdev->dev,
		 "VFIO-PCI initialized: group=%d BAR0 mapped at %p (0x%zx bytes) with IOMMU Type1 DMA\n",
		 group_id, pdev->bar0_vaddr, pdev->bar0_size);
	return 0;
}

static void gc570d_vfio_close(struct pci_dev *pdev)
{
	int leaked = 0;
	int i;

	/* Release every cached (unused) DMA mapping while the container fd is
	 * still open.  Live mappings are reported below: freeing them here would
	 * hide real driver bugs, and the container teardown unmaps any stragglers
	 * anyway.
	 */
	gc570d_dma_cache_drain(pdev);
	pthread_mutex_lock(&g_dma_map_lock);
	for (i = 0; i < GC570D_MAX_DMA_MAPPINGS; i++) {
		if (g_dma_map_table[i].in_use)
			leaked++;
	}
	pthread_mutex_unlock(&g_dma_map_lock);
	if (leaked)
		dev_warn(&pdev->dev,
			 "%d DMA mapping(s) still live at VFIO teardown\n",
			 leaked);

	if (pdev->bar0_vaddr && pdev->bar0_size) {
		munmap(pdev->bar0_vaddr, pdev->bar0_size);
		pdev->bar0_vaddr = NULL;
	}
	if (pdev->vfio_device_fd >= 0) {
		close(pdev->vfio_device_fd);
		pdev->vfio_device_fd = -1;
	}
	if (pdev->vfio_group_fd >= 0) {
		close(pdev->vfio_group_fd);
		pdev->vfio_group_fd = -1;
	}
	if (pdev->vfio_container_fd >= 0) {
		close(pdev->vfio_container_fd);
		pdev->vfio_container_fd = -1;
	}
}

/*
 * Capture-pipeline health watchdog.
 *
 * The sleep/PCIe-reset watchdog below only notices a lost link when the
 * interrupt registers read as all-zero/all-ones.  A stalled DMA engine or a
 * dead VFIO eventfd can leave those registers looking healthy while no frames
 * ever complete.  HDMI IN 1's DMA0 ring is continuous, so irq_dma0 is a clean
 * heartbeat independent of VB2 consumer behaviour.  HDMI IN 2 submits DMA per
 * buffer, so it is only treated as active while VB2 has buffers queued.
 *
 * Audio is deliberately not a trigger: silence from the console is normal.
 * Its cursors are logged only as corroborating diagnostics.
 */
#define GC570D_HEALTH_GRACE_NS  (15ULL * NSEC_PER_SEC)
#define GC570D_HEALTH_STALL_NS  (5ULL * NSEC_PER_SEC)
#define GC570D_HEALTH_RETRY_NS  (60ULL * NSEC_PER_SEC)
#define GC570D_HEALTH_DECAY_NS  (10ULL * 60ULL * NSEC_PER_SEC)
#define GC570D_HEALTH_MAX_TRIES 3

struct gc570d_health_input {
	u64 last_irq;
	u64 stall_since_ns;
};

struct gc570d_health {
	struct gc570d_health_input hdmi1;
	struct gc570d_health_input hdmi2;
	u64 last_attempt_ns;
	u64 arm_at_ns;
	unsigned int attempts;
};

static void gc570d_health_arm(struct gc570d_dev *gc, struct gc570d_health *health,
			      u64 now_ns)
{
	health->hdmi1.last_irq = 0;
	health->hdmi1.stall_since_ns = 0;
	health->hdmi2.last_irq = 0;
	health->hdmi2.stall_since_ns = 0;
	health->arm_at_ns = now_ns + GC570D_HEALTH_GRACE_NS;
	if (!gc)
		return;
	health->hdmi1.last_irq = (u64)atomic64_read(&gc->irq_dma0);
	health->hdmi2.last_irq = (u64)atomic64_read(&gc->irq_dma1);
}

static bool gc570d_health_hdmi2_has_buffers(struct gc570d_dev *gc)
{
	unsigned long flags;
	bool queued;

	spin_lock_irqsave(&gc->video_qlock, flags);
	queued = !list_empty(&gc->video_buffers);
	spin_unlock_irqrestore(&gc->video_qlock, flags);
	return queued;
}

/* Caller must hold the input's state.  Returns true once it has stalled. */
static bool gc570d_health_input_stalled(struct gc570d_health_input *in,
					u64 counter, bool active, u64 now_ns)
{
	if (!active) {
		in->last_irq = counter;
		in->stall_since_ns = 0;
		return false;
	}

	if (counter != in->last_irq) {
		in->last_irq = counter;
		in->stall_since_ns = 0;
		return false;
	}

	if (!in->stall_since_ns) {
		in->stall_since_ns = now_ns;
		return false;
	}

	return now_ns - in->stall_since_ns >= GC570D_HEALTH_STALL_NS;
}

/* Returns true when the caller should request a hardware reinit. */
static bool gc570d_health_tick(struct gc570d_dev *gc,
			       struct gc570d_health *health, u64 now_ns)
{
	bool hdmi1_active, hdmi2_active;
	bool hdmi1_stalled, hdmi2_stalled;
	u64 dma0, dma1;

	if (!gc || READ_ONCE(gc->removing) || READ_ONCE(gc->suspended))
		return false;
	if (now_ns < health->arm_at_ns)
		return false;

	dma0 = (u64)atomic64_read(&gc->irq_dma0);
	dma1 = (u64)atomic64_read(&gc->irq_dma1);

	hdmi1_active = READ_ONCE(gc->video0_streaming) &&
		       READ_ONCE(gc->video0_dma_active) &&
		       !READ_ONCE(gc->video0_no_signal);
	hdmi2_active = READ_ONCE(gc->video_streaming) &&
		       READ_ONCE(gc->video_dma_active) &&
		       !READ_ONCE(gc->video_no_signal) &&
		       gc570d_health_hdmi2_has_buffers(gc);

	hdmi1_stalled = gc570d_health_input_stalled(&health->hdmi1, dma0,
						    hdmi1_active, now_ns);
	hdmi2_stalled = gc570d_health_input_stalled(&health->hdmi2, dma1,
						    hdmi2_active, now_ns);
	if (!hdmi1_stalled && !hdmi2_stalled) {
		/* Let a long healthy run clear the recovery budget again. */
		if (health->attempts && health->last_attempt_ns &&
		    now_ns - health->last_attempt_ns >= GC570D_HEALTH_DECAY_NS)
			health->attempts = 0;
		return false;
	}

	if (health->attempts >= GC570D_HEALTH_MAX_TRIES) {
		if (health->attempts == GC570D_HEALTH_MAX_TRIES) {
			dev_err(&gc->pdev->dev,
				"capture pipeline still stalled after %u automatic recoveries; restart the daemon\n",
				health->attempts);
			health->attempts++;
		}
		return false;
	}
	if (health->last_attempt_ns &&
	    now_ns - health->last_attempt_ns < GC570D_HEALTH_RETRY_NS)
		return false;

	dev_warn(&gc->pdev->dev,
		 "capture pipeline stalled on HDMI IN %s%s for %llus (irq_dma0=%llu irq_dma1=%llu audio_cursor=%u/%u); re-initializing\n",
		 hdmi1_stalled ? "1" : "",
		 hdmi2_stalled ? (hdmi1_stalled ? "+2" : "2") : "",
		 (unsigned long long)((now_ns -
			(hdmi1_stalled ? health->hdmi1.stall_since_ns
				       : health->hdmi2.stall_since_ns)) /
			NSEC_PER_SEC),
		 (unsigned long long)dma0, (unsigned long long)dma1,
		 READ_ONCE(gc->audio0_pcm_cursor),
		 READ_ONCE(gc->audio_pcm_cursor));

	health->hdmi1.stall_since_ns = 0;
	health->hdmi2.stall_since_ns = 0;
	health->last_attempt_ns = now_ns;
	health->attempts++;
	return true;
}

static void on_signal(int sig)
{
	(void)sig;
	atomic_store(&g_daemon_quit, true);
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [PCI_ADDRESS] [OPTIONS]\n", prog);
	printf("\n");
	printf("AVerMedia Live Gamer DUO (GC570D) userspace VFIO engine.\n");
	printf("\n");
	printf("  --pci BDF          PCI address (default: auto-detect 1461:5700)\n");
	printf("  --copy-edid        Copy the downstream EDID (default: internal 4K60 bridge)\n");
	printf("  --no-auto-hdmi1    Disable automatic HDMI IN 1 receiver setup\n");
	printf("\n");
	printf("Presentation:\n");
	printf("  --sdl              Open the EverLive SDL2 window\n");
	printf("  --headless         No SDL window; capture, V4L2 and audio only\n");
	printf("  -s, --studio       Alias for --headless (clean OBS/Studio mode)\n");
	printf("  -f, --fullscreen   Start fullscreen (F/F11 toggles)\n");
	printf("  --vsync            Pace presentation to the display refresh rate\n");
	printf("  --no-vsync         Disable VSync\n");
	printf("  --port N           Initial input: 1/2 (default: 1)\n");
	printf("  --pip              Show the other HDMI input as picture-in-picture\n");
	printf("  --osd              Show the on-screen overlay (default)\n");
	printf("  --no-osd           Hide the overlay (same as --streamer-mode)\n");
	printf("  --streamer-mode    Clean feed: no OSD\n");
	printf("  --osd-pos POS      tr|br|bl|tl (default: tr)\n");
	printf("  --aspect MODE      auto|16:9|4:3|stretch (default: auto)\n");
	printf("  --scale MODE       fit|integer (default: fit)\n");
	printf("  -v, --volume VOL   Initial SDL2 audio volume (0.0-1.5 or 0-150)\n");
	printf("  --title TEXT       Window title (default: EverLive DUO)\n");
	printf("\n");
	printf("Virtual cameras:\n");
	printf("  --v4l2 DEV         Publish selected input to DEV (e.g. /dev/video10)\n");
	printf("  --v4l2-2 DEV       Publish HDMI 2 to DEV (e.g. /dev/video11)\n");
	printf("  --loopback [DEV]   Alias for --v4l2 (default: /dev/video10)\n");
	printf("\n");
	printf("Hotkeys: 1/2 input, P PiP, A aspect, L scale, M mute, Up/Down vol, Tab/O OSD, F/F11 fullscreen, Esc/Q quit\n");
}

int main(int argc, char **argv)
{
	const char *requested_bdf = NULL;
	struct gc570d_everlive_options live = {
		.enable_sdl = false,
		.headless = true,
		.pip = false,
		.osd = true,
		.fullscreen = false,
		.vsync = false,
		.studio = false,
		.muted = false,
		.port = 0,
		.volume_pct = 100,
		.aspect = "auto",
		.scale = "fit",
		.osd_pos = "tr",
		.v4l2 = { NULL, NULL },
		.title = "EverLive DUO",
	};
	bool want_sdl = false;
	bool want_headless = false;
	bool want_studio = false;
	int ret;

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--pci") == 0 && i + 1 < argc) {
			requested_bdf = argv[++i];
		} else if (strncmp(arg, "--pci=", 6) == 0) {
			requested_bdf = arg + 6;
		} else if (strcmp(arg, "--copy-edid") == 0) {
			copy_edid = true;
		} else if (strcmp(arg, "--no-auto-hdmi1") == 0) {
			auto_hdmi1 = false;
		} else if (strcmp(arg, "--sdl") == 0) {
			want_sdl = true;
		} else if (strcmp(arg, "--headless") == 0) {
			want_headless = true;
		} else if (strcmp(arg, "-s") == 0 ||
			   strcmp(arg, "--studio") == 0) {
			want_studio = true;
			live.osd = false;
		} else if (strcmp(arg, "--streamer-mode") == 0 ||
			   strcmp(arg, "--clean") == 0) {
			/* Clean gaming feed: keep the SDL window, hide the OSD. */
			live.osd = false;
		} else if (strcmp(arg, "--no-osd") == 0) {
			live.osd = false;
		} else if (strcmp(arg, "--osd") == 0) {
			live.osd = true;
		} else if (strcmp(arg, "--osd-pos") == 0 && i + 1 < argc) {
			live.osd_pos = argv[++i];
		} else if (strncmp(arg, "--osd-pos=", 10) == 0) {
			live.osd_pos = arg + 10;
		} else if (strcmp(arg, "-f") == 0 ||
			   strcmp(arg, "--fullscreen") == 0) {
			live.fullscreen = true;
		} else if (strcmp(arg, "--vsync") == 0) {
			live.vsync = true;
		} else if (strcmp(arg, "--no-vsync") == 0) {
			live.vsync = false;
		} else if (strcmp(arg, "--pip") == 0) {
			live.pip = true;
		} else if (strcmp(arg, "--port") == 0 && i + 1 < argc) {
			const char *spec = argv[++i];

			if (spec[0] == '2' || strcmp(spec, "hdmi2") == 0)
				live.port = 1;
			else
				live.port = 0;
		} else if (strncmp(arg, "--port=", 7) == 0) {
			const char *spec = arg + 7;

			live.port = (spec[0] == '2' || strcmp(spec, "hdmi2") == 0);
		} else if (strcmp(arg, "--aspect") == 0 && i + 1 < argc) {
			live.aspect = argv[++i];
		} else if (strncmp(arg, "--aspect=", 9) == 0) {
			live.aspect = arg + 9;
		} else if (strcmp(arg, "--scale") == 0 && i + 1 < argc) {
			live.scale = argv[++i];
		} else if (strncmp(arg, "--scale=", 8) == 0) {
			live.scale = arg + 8;
		} else if (strcmp(arg, "--integer-scale") == 0) {
			live.scale = "integer";
		} else if ((strcmp(arg, "--volume") == 0 ||
			    strcmp(arg, "-v") == 0) && i + 1 < argc) {
			const char *vstr = argv[++i];
			double v = strtod(vstr, NULL);

			live.volume_pct = (v <= 1.5 && strchr(vstr, '.') != NULL) ?
					  (int)(v * 100.0 + 0.5) : (int)(v + 0.5);
		} else if (strncmp(arg, "--volume=", 9) == 0) {
			const char *vstr = arg + 9;
			double v = strtod(vstr, NULL);

			live.volume_pct = (v <= 1.5 && strchr(vstr, '.') != NULL) ?
					  (int)(v * 100.0 + 0.5) : (int)(v + 0.5);
		} else if (strcmp(arg, "--title") == 0 && i + 1 < argc) {
			live.title = argv[++i];
		} else if (strcmp(arg, "--v4l2") == 0 && i + 1 < argc &&
			   argv[i + 1][0] != '-') {
			live.v4l2[0] = argv[++i];
		} else if (strncmp(arg, "--v4l2=", 7) == 0) {
			live.v4l2[0] = arg + 7;
		} else if ((strcmp(arg, "--v4l2-2") == 0 ||
			    strcmp(arg, "--loopback-2") == 0) &&
			   i + 1 < argc && argv[i + 1][0] != '-') {
			live.v4l2[1] = argv[++i];
		} else if (strncmp(arg, "--v4l2-2=", 9) == 0) {
			live.v4l2[1] = arg + 9;
		} else if (strcmp(arg, "--loopback") == 0) {
			if (i + 1 < argc && strncmp(argv[i + 1], "/dev/", 5) == 0)
				live.v4l2[0] = argv[++i];
			else
				live.v4l2[0] = "/dev/video10";
		} else if (strncmp(arg, "--loopback=", 11) == 0) {
			live.v4l2[0] = arg + 11;
		} else if (strcmp(arg, "--help") == 0 ||
			   strcmp(arg, "-h") == 0) {
			print_usage(argv[0]);
			return 0;
		} else if (arg[0] != '-') {
			requested_bdf = arg;
		} else {
			fprintf(stderr, "[gc570d WARN] Unknown option '%s' ignored\n",
				arg);
		}
	}

	/* Explicit --sdl wins; --headless/--studio keep the window closed. */
	live.enable_sdl = want_sdl && !want_headless && !want_studio;
	live.headless = !live.enable_sdl;
	live.studio = want_studio;
	if (live.headless)
		live.vsync = false;

	/* Raise RLIMIT_MEMLOCK so VFIO_IOMMU_MAP_DMA can pin DMA buffers */
	struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };
	if (setrlimit(RLIMIT_MEMLOCK, &rlim) < 0) {
		if (getrlimit(RLIMIT_MEMLOCK, &rlim) == 0 && rlim.rlim_cur < rlim.rlim_max) {
			rlim.rlim_cur = rlim.rlim_max;
			(void)setrlimit(RLIMIT_MEMLOCK, &rlim);
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	/* Load optional runtime configuration */
	gc570d_load_config(NULL);

	ret = gc570d_vfio_open(&g_pdev, requested_bdf);
	if (ret < 0)
		return 1;

	struct pci_driver *drv = gc570d_get_pci_driver();
	bool probed = false;

	ret = drv->probe(&g_pdev, drv->id_table);
	if (ret < 0) {
		fprintf(stderr, "[gc570d ERR ] Driver probe failed: %d\n", ret);
		gc570d_vfio_close(&g_pdev);
		return 1;
	}
	g_gc = pci_get_drvdata(&g_pdev);
	probed = true;

	/* Initialize POSIX shared-memory frame store for libgc570d-v4l2.so */
	{
		struct everlive_framestore_config fs_cfg = {
			.name = "/gc570d_v4l2_shm",
			.pci_bdf = g_pdev.bdf,
			.port_count = 2,
			.slot_count = 3,
			.frame_stride = GC570D_WIDTH * GC570D_HEIGHT * 2,
		};

		g_framestore = everlive_framestore_create(&fs_cfg);
		if (!g_framestore) {
			fprintf(stderr,
				"[gc570d WARN] Virtual-camera frame store unavailable; continuing without it\n");
		} else {
			everlive_framestore_set_geometry(g_framestore, 0,
							 gc570d_cfg.width,
							 gc570d_cfg.height);
			everlive_framestore_set_geometry(g_framestore, 1,
							 GC570D_WIDTH,
							 GC570D_HEIGHT);
		}
	}

#ifdef HAVE_LIBPIPEWIRE
	gc570d_pipewire_init();
#endif

	ret = gc570d_start_user_video_streams();
	if (ret < 0)
		fprintf(stderr, "[gc570d WARN] Failed to start userspace video streams: %d\n", ret);

	ret = gc570d_start_user_audio_streams();
	if (ret < 0)
		fprintf(stderr, "[gc570d WARN] Failed to start userspace audio streams: %d\n", ret);

	gc570d_ctl_server_init();
	fprintf(stderr,
		"[gc570d INFO] Userspace daemon running (control socket: %s)\n",
		g_ctl_sock_path);

	ret = gc570d_everlive_start(g_framestore, g_gc, &live);
	if (ret < 0)
		fprintf(stderr,
			"[gc570d WARN] Failed to start EverLive frontend: %d\n",
			ret);

	gc570d_jsonrpc_set_stats_fn(gc570d_user_pipeline_stats);
	gc570d_jsonrpc_set_dev(g_gc);
	if (gc570d_jsonrpc_init(EVERLIVE_VERSION) < 0) {
		fprintf(stderr,
			"[gc570d WARN] JSON-RPC control socket unavailable; continuing without remote control\n");
	} else {
		fprintf(stderr,
			"[gc570d INFO] JSON-RPC control socket: %s\n",
			gc570d_jsonrpc_socket_path());
	}

	struct timespec ts_mono_prev = { 0 }, ts_boot_prev = { 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts_mono_prev);
	clock_gettime(CLOCK_BOOTTIME, &ts_boot_prev);
	uint64_t last_watchdog_ns = (uint64_t)ts_mono_prev.tv_sec * 1000000000ULL + ts_mono_prev.tv_nsec;
	struct gc570d_health health = { 0 };

	gc570d_health_arm(g_gc, &health,
			  (u64)ts_mono_prev.tv_sec * NSEC_PER_SEC +
			  (u64)ts_mono_prev.tv_nsec);

	while (!atomic_load(&g_daemon_quit) &&
	       !gc570d_everlive_quit_requested()) {
		struct pollfd pfd[1 + GC570D_JSONRPC_MAX_CLIENTS + 1];
		int nfds = 0;
		int rpc_base;
		int pr;

		if (g_ctl_listen_fd >= 0) {
			pfd[nfds].fd = g_ctl_listen_fd;
			pfd[nfds].events = POLLIN;
			nfds++;
		}
		rpc_base = nfds;
		nfds += gc570d_jsonrpc_fill_pollfds(&pfd[nfds],
				(int)(ARRAY_SIZE(pfd) - (size_t)nfds));

		pr = poll(pfd, (nfds_t)nfds, 250);

		if (pr > 0) {
			bool rpc_ready = false;

			for (int i = rpc_base; i < nfds; i++) {
				if (pfd[i].revents) {
					rpc_ready = true;
					break;
				}
			}
			if (rpc_ready)
				gc570d_jsonrpc_service();

			if (g_ctl_listen_fd >= 0 &&
			    (pfd[0].revents & POLLIN)) {
				int cfd = accept4(g_ctl_listen_fd, NULL, NULL,
						  SOCK_CLOEXEC);

				if (cfd >= 0) {
					gc570d_handle_ctl_client(cfd);
					close(cfd);
				}
			}
		}

		/* Check every ~1s for system sleep/resume or PCIe hardware power reset */
		struct timespec ts_mono_now = { 0 }, ts_boot_now = { 0 };
		clock_gettime(CLOCK_MONOTONIC, &ts_mono_now);
		clock_gettime(CLOCK_BOOTTIME, &ts_boot_now);
		uint64_t mono_now_ns = (uint64_t)ts_mono_now.tv_sec * 1000000000ULL + ts_mono_now.tv_nsec;
		if (mono_now_ns - last_watchdog_ns >= 1000000000ULL) {
			uint64_t mono_prev_ns = (uint64_t)ts_mono_prev.tv_sec * 1000000000ULL + ts_mono_prev.tv_nsec;
			uint64_t boot_prev_ns = (uint64_t)ts_boot_prev.tv_sec * 1000000000ULL + ts_boot_prev.tv_nsec;
			uint64_t boot_now_ns  = (uint64_t)ts_boot_now.tv_sec * 1000000000ULL + ts_boot_now.tv_nsec;
			uint64_t mono_delta   = mono_now_ns - mono_prev_ns;
			uint64_t boot_delta   = boot_now_ns - boot_prev_ns;

			ts_mono_prev = ts_mono_now;
			ts_boot_prev = ts_boot_now;
			last_watchdog_ns = mono_now_ns;

			if (boot_delta > mono_delta + 2000000000ULL) {
				fprintf(stderr,
					"[gc570d INFO] System resume from sleep detected (sleep=%llums); re-initializing hardware...\n",
					(unsigned long long)((boot_delta - mono_delta) / 1000000ULL));
				g_reinit_requested = true;
			} else if (g_gc && g_gc->bar0) {
				u32 irq_mode = readl(g_gc->bar0 + GC570D_REG_IRQ_MODE);
				u32 irq_mask = readl(g_gc->bar0 + GC570D_REG_IRQ_MASK);
				if (irq_mode == 0 || irq_mode == 0xffffffffU ||
				    irq_mask == 0 || irq_mask == 0xffffffffU) {
					fprintf(stderr,
						"[gc570d INFO] PCIe hardware reset detected after sleep (irq_mode=0x%x irq_mask=0x%x); re-initializing...\n",
						irq_mode, irq_mask);
					g_reinit_requested = true;
				}
			}

			/* A stalled DMA/IRQ path can leave the registers above
			 * looking sane, so also watch the per-input heartbeats.
			 */
			if (!g_reinit_requested &&
			    gc570d_health_tick(g_gc, &health, mono_now_ns))
				g_reinit_requested = true;
		}

		if (g_reinit_requested && !atomic_load(&g_daemon_quit)) {
			fprintf(stderr, "[gc570d INFO] Re-initializing GC570D hardware pipeline in place...\n");
			gc570d_everlive_set_dev(NULL);
			gc570d_jsonrpc_set_dev(NULL);
			drv->remove(&g_pdev);
			probed = false;
			g_gc = NULL;
			g_next_card_num = 0;
			g_reinit_requested = false;
			usleep(200000);
			if (drv->probe(&g_pdev, drv->id_table) == 0) {
				g_gc = pci_get_drvdata(&g_pdev);
				probed = true;
				gc570d_everlive_set_dev(g_gc);
				gc570d_jsonrpc_set_dev(g_gc);
				gc570d_start_user_video_streams();
				gc570d_start_user_audio_streams();
				clock_gettime(CLOCK_MONOTONIC, &ts_mono_prev);
				clock_gettime(CLOCK_BOOTTIME, &ts_boot_prev);
				last_watchdog_ns = (uint64_t)ts_mono_prev.tv_sec * 1000000000ULL + ts_mono_prev.tv_nsec;
				gc570d_health_arm(g_gc, &health,
						  (u64)ts_mono_prev.tv_sec * NSEC_PER_SEC +
						  (u64)ts_mono_prev.tv_nsec);
			} else {
				fprintf(stderr, "[gc570d ERR ] Hardware re-initialization failed; shutting down\n");
				break;
			}
		}
	}

	fprintf(stderr, "[gc570d INFO] Shutting down userspace driver...\n");
	gc570d_everlive_stop();
	gc570d_jsonrpc_shutdown();
	gc570d_ctl_server_shutdown();
#ifdef HAVE_LIBPIPEWIRE
	gc570d_pipewire_shutdown();
#endif
	if (probed)
		drv->remove(&g_pdev);
	gc570d_vfio_close(&g_pdev);
	everlive_framestore_destroy(g_framestore);
	g_framestore = NULL;
	return 0;
}

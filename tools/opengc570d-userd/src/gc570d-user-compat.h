// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace compatibility layer for running the OpenGC570D driver in userspace
 * via stock kernel vfio-pci (BAR0 MMIO + IOMMU Type1v2 DMA + eventfd IRQs) and
 * publishing video/audio directly to PipeWire (libpipewire-0.3).
 *
 * Designed to satisfy Epic Games Easy Anti-Cheat (EAC) Linux v2 requirements:
 * - No out-of-tree or unsigned kernel modules (uses stock signed vfio-pci)
 * - Secure Boot + Measured Boot + TPM 2.0 compatible
 * - Kernel Lockdown Integrity Mode compatible (no /dev/mem or raw sysfs BAR mmap)
 * - Hardware IOMMU enabled with passthrough disabled (VFIO_IOMMU_MAP_DMA)
 */
#ifndef GC570D_USER_COMPAT_H
#define GC570D_USER_COMPAT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <endian.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* Basic kernel integer types */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef long long  s64;

#include <linux/types.h>
#define array_size(a, b) ((size_t)(a) * (size_t)(b))

typedef uint64_t dma_addr_t;
typedef uint64_t resource_size_t;
typedef off_t    loff_t;
typedef uint32_t gfp_t;
typedef mode_t   umode_t;
typedef int64_t  ktime_t;

#define __iomem
#define __user
#define __maybe_unused __attribute__((unused))
#define THIS_MODULE ((void *)0)
#define GFP_KERNEL 0

#ifndef U32_MAX
#define U32_MAX ((u32)~0U)
#endif

#define SZ_512K 0x00080000ULL

#define BIT(nr) (1UL << (nr))
#define BIT_ULL(nr) (1ULL << (nr))

/* Linux <linux/bitops.h> helpers used by the receiver audio decoder. */
static inline int fls(unsigned int word)
{
	return word ? (int)(sizeof(unsigned int) * 8 - __builtin_clz(word)) : 0;
}

static inline int __fls(unsigned int word)
{
	return fls(word) - 1;
}

#define GENMASK(h, l) \
	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (sizeof(unsigned long) * 8 - 1 - (h))))
#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define min(x, y) ({ \
	typeof(x) _min1 = (x); \
	typeof(y) _min2 = (y); \
	(void) (&_min1 == &_min2); \
	_min1 < _min2 ? _min1 : _min2; })

#define max(x, y) ({ \
	typeof(x) _max1 = (x); \
	typeof(y) _max2 = (y); \
	(void) (&_max1 == &_max2); \
	_max1 > _max2 ? _max1 : _max2; })

#define min_t(type, x, y) ({ \
	type __min1 = (x); \
	type __min2 = (y); \
	__min1 < __min2 ? __min1 : __min2; })

#define max_t(type, x, y) ({ \
	type __max1 = (x); \
	type __max2 = (y); \
	__max1 > __max2 ? __max1 : __max2; })

#define clamp_t(type, val, lo, hi) min_t(type, max_t(type, val, lo), hi)

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(x, divisor) ({ \
	typeof(x) __x = x; \
	typeof(divisor) __d = divisor; \
	(((typeof(x))-1) > 0 || ((typeof(divisor))-1) > 0 || (__x) > 0) ? \
		(((__x) + ((__d) / 2)) / (__d)) : \
		(((__x) - ((__d) / 2)) / (__d)); \
})

static inline u64 div_u64(u64 dividend, u32 divisor)
{
	return divisor ? (dividend / divisor) : 0;
}

static inline u32 lower_32_bits(u64 n)
{
	return (u32)(n & 0xffffffffULL);
}

static inline u32 upper_32_bits(u64 n)
{
	return (u32)(n >> 32);
}

static inline __le32 cpu_to_le32(u32 v)
{
	return htole32(v);
}

static inline u32 le32_to_cpu(__le32 v)
{
	return le32toh(v);
}

#define READ_ONCE(x) (__atomic_load_n(&(x), __ATOMIC_ACQUIRE))
#define WRITE_ONCE(x, val) (__atomic_store_n(&(x), (val), __ATOMIC_RELEASE))
#define xchg(ptr, val) (__atomic_exchange_n((ptr), (val), __ATOMIC_SEQ_CST))

#define wmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define rmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define dma_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define dma_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)

#ifndef container_of
#define container_of(ptr, type, member) ({ \
	const typeof(((type *)0)->member) *__mptr = (ptr); \
	(type *)((char *)__mptr - offsetof(type, member)); })
#endif

/* Error pointers */
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) ((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline bool IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
static inline bool IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR(ptr); }

#define WARN_ON(condition) ({ \
	int __ret_warn_on = !!(condition); \
	if (__ret_warn_on) \
		fprintf(stderr, "[gc570d WARN] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
	__ret_warn_on; \
})

/* String helpers */
static inline ssize_t strscpy(char *dest, const char *src, size_t count)
{
	size_t len;
	if (!count)
		return -E2BIG;
	len = strnlen(src, count - 1);
	memcpy(dest, src, len);
	dest[len] = '\0';
	return src[len] ? -E2BIG : (ssize_t)len;
}

static inline bool sysfs_streq(const char *s1, const char *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	if (*s1 == *s2)
		return true;
	if (!*s1 && *s2 == '\n' && !s2[1])
		return true;
	if (*s1 == '\n' && !s1[1] && !*s2)
		return true;
	return false;
}

static inline int sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	int n;
	va_start(args, fmt);
	n = vsnprintf(buf, 4096, fmt, args);
	va_end(args);
	return n;
}

static inline int kstrtobool_from_user(const char *s, size_t count, bool *res)
{
	if (!s || !count)
		return -EINVAL;
	while (count > 0 && (*s == ' ' || *s == '\t')) {
		s++;
		count--;
	}
	if (!count)
		return -EINVAL;
	if (s[0] == '1' || s[0] == 'y' || s[0] == 'Y' ||
	    (count >= 2 && (s[0] == 'o' || s[0] == 'O') && (s[1] == 'n' || s[1] == 'N')) ||
	    (count >= 4 && strncasecmp(s, "true", 4) == 0)) {
		*res = true;
		return 0;
	}
	if (s[0] == '0' || s[0] == 'n' || s[0] == 'N' ||
	    (count >= 3 && (s[0] == 'o' || s[0] == 'O') && (s[1] == 'f' || s[1] == 'F')) ||
	    (count >= 5 && strncasecmp(s, "false", 5) == 0)) {
		*res = false;
		return 0;
	}
	return -EINVAL;
}

static inline ssize_t simple_read_from_buffer(void *to, size_t count,
					      loff_t *ppos, const void *from,
					      size_t available)
{
	loff_t pos = *ppos;
	if (pos < 0)
		return -EINVAL;
	if ((size_t)pos >= available || !count || !from)
		return 0;
	if (count > available - (size_t)pos)
		count = available - (size_t)pos;
	memcpy(to, (const char *)from + pos, count);
	*ppos = pos + count;
	return count;
}

/* Time and sleep helpers */
#define NSEC_PER_USEC 1000ULL
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC  1000000000ULL

static inline u64 ktime_get_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * NSEC_PER_SEC + (u64)ts.tv_nsec;
}

static inline ktime_t ktime_get(void)
{
	return (ktime_t)ktime_get_ns();
}

static inline s64 ktime_us_delta(const ktime_t later, const ktime_t earlier)
{
	return (later - earlier) / 1000LL;
}

static inline unsigned long msecs_to_jiffies(const unsigned int m)
{
	return (unsigned long)m;
}

#define jiffies ((unsigned long)(ktime_get_ns() / NSEC_PER_MSEC))
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)

static inline void usleep_range(unsigned long min_us, unsigned long max_us)
{
	(void)max_us;
	struct timespec req;
	req.tv_sec = min_us / 1000000UL;
	req.tv_nsec = (min_us % 1000000UL) * 1000UL;
	nanosleep(&req, NULL);
}

static inline void msleep(unsigned int msecs)
{
	usleep_range((unsigned long)msecs * 1000UL, (unsigned long)msecs * 1000UL);
}

static inline unsigned long msleep_interruptible(unsigned int msecs)
{
	msleep(msecs);
	return 0;
}

/* MMIO BAR0 register access */
static inline u32 readl(const volatile void __iomem *addr)
{
	u32 val = *(const volatile u32 *)addr;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return le32toh(val);
}

static inline void writel(u32 val, volatile void __iomem *addr)
{
	__atomic_thread_fence(__ATOMIC_RELEASE);
	*(volatile u32 *)addr = htole32(val);
}

#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before_read, args...) \
({ \
	u64 __timeout_ns = (u64)(timeout_us) * 1000ULL; \
	u64 __start_ns = ktime_get_ns(); \
	if (sleep_before_read && (sleep_us)) \
		usleep_range((sleep_us), (sleep_us)); \
	for (;;) { \
		(val) = op(args); \
		if (cond) \
			break; \
		if ((timeout_us) && (ktime_get_ns() - __start_ns) >= __timeout_ns) { \
			(val) = op(args); \
			break; \
		} \
		if (sleep_us) \
			usleep_range((sleep_us), (sleep_us)); \
	} \
	(cond) ? 0 : -ETIMEDOUT; \
})

#define readl_poll_timeout(addr, val, cond, delay_us, timeout_us) \
	read_poll_timeout(readl, val, cond, delay_us, timeout_us, false, addr)

/* Memory allocation */
static inline void *kzalloc(size_t size, gfp_t flags)
{
	(void)flags;
	return calloc(1, size ? size : 1);
}

static inline void *kvzalloc(size_t size, gfp_t flags)
{
	return kzalloc(size, flags);
}

static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
	(void)flags;
	return calloc(n ? n : 1, size ? size : 1);
}

static inline void *vmalloc(unsigned long size)
{
	return calloc(1, size ? size : 1);
}

static inline void kfree(const void *ptr)
{
	free((void *)ptr);
}

static inline void kvfree(const void *ptr)
{
	free((void *)ptr);
}

static inline void vfree(const void *ptr)
{
	free((void *)ptr);
}

/* Atomics */
typedef struct {
	_Atomic int counter;
} atomic_t;

typedef struct {
	_Atomic int64_t counter;
} atomic64_t;

static inline int atomic_read(const atomic_t *v)
{
	return atomic_load_explicit(&((atomic_t *)v)->counter, memory_order_acquire);
}

static inline void atomic_set(atomic_t *v, int i)
{
	atomic_store_explicit(&v->counter, i, memory_order_release);
}

static inline void atomic_or(int i, atomic_t *v)
{
	atomic_fetch_or_explicit(&v->counter, i, memory_order_acq_rel);
}

static inline void atomic_andnot(int i, atomic_t *v)
{
	atomic_fetch_and_explicit(&v->counter, ~i, memory_order_acq_rel);
}

static inline long long atomic64_read(const atomic64_t *v)
{
	return atomic_load_explicit(&((atomic64_t *)v)->counter, memory_order_acquire);
}

static inline void atomic64_set(atomic64_t *v, int64_t i)
{
	atomic_store_explicit(&v->counter, i, memory_order_release);
}

static inline void atomic64_inc(atomic64_t *v)
{
	atomic_fetch_add_explicit(&v->counter, 1, memory_order_acq_rel);
}

static inline void atomic64_add(int64_t i, atomic64_t *v)
{
	atomic_fetch_add_explicit(&v->counter, i, memory_order_acq_rel);
}

/* Mutexes and Spinlocks */
struct mutex {
	pthread_mutex_t lock;
};

static inline void mutex_init(struct mutex *m)
{
	pthread_mutex_init(&m->lock, NULL);
}

static inline void mutex_lock(struct mutex *m)
{
	pthread_mutex_lock(&m->lock);
}

static inline int mutex_trylock(struct mutex *m)
{
	return pthread_mutex_trylock(&m->lock) == 0 ? 1 : 0;
}

static inline void mutex_unlock(struct mutex *m)
{
	pthread_mutex_unlock(&m->lock);
}

typedef struct {
	pthread_mutex_t lock;
} spinlock_t;

static inline void spin_lock_init(spinlock_t *s)
{
	pthread_mutex_init(&s->lock, NULL);
}

#define spin_lock_irqsave(lock_ptr, flags) \
	do { (flags) = 0; pthread_mutex_lock(&(lock_ptr)->lock); } while (0)

#define spin_unlock_irqrestore(lock_ptr, flags) \
	do { (void)(flags); pthread_mutex_unlock(&(lock_ptr)->lock); } while (0)

/* Doubly-linked list */
struct list_head {
	struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *new_entry,
			      struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new_entry;
	new_entry->next = next;
	new_entry->prev = prev;
	WRITE_ONCE(prev->next, new_entry);
}

static inline void list_add_tail(struct list_head *new_entry, struct list_head *head)
{
	__list_add(new_entry, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
	struct list_head *prev = entry->prev;
	struct list_head *next = entry->next;
	next->prev = prev;
	WRITE_ONCE(prev->next, next);
	entry->next = entry;
	entry->prev = entry;
}

static inline int list_empty(const struct list_head *head)
{
	return READ_ONCE(head->next) == head;
}

static inline int list_empty_careful(const struct list_head *head)
{
	struct list_head *next = READ_ONCE(head->next);
	return (next == head) && (next == READ_ONCE(head->prev));
}

#define list_first_entry(ptr, type, member) \
	container_of((ptr)->next, type, member)

/* Waitqueues and Completions */
typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
} wait_queue_head_t;

void init_waitqueue_head(wait_queue_head_t *wq);
void wake_up_interruptible(wait_queue_head_t *wq);
void gc570d_wq_wait_ms(wait_queue_head_t *wq, unsigned long timeout_ms);

#define wait_event_interruptible(wq, condition) \
({ \
	int __ret = 0; \
	pthread_mutex_lock(&(wq).lock); \
	while (!(condition)) { \
		struct timespec __ts; \
		u64 __t_ns = ktime_get_ns() + 2ULL * NSEC_PER_MSEC; \
		__ts.tv_sec = __t_ns / NSEC_PER_SEC; \
		__ts.tv_nsec = __t_ns % NSEC_PER_SEC; \
		pthread_cond_timedwait(&(wq).cond, &(wq).lock, &__ts); \
	} \
	pthread_mutex_unlock(&(wq).lock); \
	__ret; \
})

#define wait_event_interruptible_timeout(wq, condition, timeout_ms) \
({ \
	long __rem = (long)(timeout_ms); \
	u64 __deadline = ktime_get_ns() + (u64)(timeout_ms) * NSEC_PER_MSEC; \
	pthread_mutex_lock(&(wq).lock); \
	while (!(condition)) { \
		u64 __now = ktime_get_ns(); \
		if (__now >= __deadline) { \
			__rem = (condition) ? 1 : 0; \
			break; \
		} \
		u64 __slice_ns = __deadline - __now; \
		if (__slice_ns > 2ULL * NSEC_PER_MSEC) \
			__slice_ns = 2ULL * NSEC_PER_MSEC; \
		u64 __wake_ns = __now + __slice_ns; \
		struct timespec __ts = { \
			.tv_sec = (time_t)(__wake_ns / NSEC_PER_SEC), \
			.tv_nsec = (long)(__wake_ns % NSEC_PER_SEC), \
		}; \
		pthread_cond_timedwait(&(wq).cond, &(wq).lock, &__ts); \
	} \
	if ((condition) && __rem <= 0) \
		__rem = 1; \
	pthread_mutex_unlock(&(wq).lock); \
	__rem; \
})

struct completion {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	unsigned int done;
};

void init_completion(struct completion *x);
void reinit_completion(struct completion *x);
void complete(struct completion *x);
bool try_wait_for_completion(struct completion *x);
unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout_ms);

/* Kernel threads */
struct task_struct {
	pthread_t thread;
	int (*threadfn)(void *data);
	void *data;
	char name[32];
	_Atomic bool should_stop;
	_Atomic bool started;
	_Atomic int refcount;
};

struct task_struct *gc570d_kthread_create(int (*threadfn)(void *data), void *data,
					  const char *fmt, ...);
struct task_struct *get_task_struct(struct task_struct *k);
void put_task_struct(struct task_struct *k);
int wake_up_process(struct task_struct *k);
int kthread_stop(struct task_struct *k);
bool kthread_should_stop(void);

#define kthread_create(threadfn, data, namefmt, arg...) \
	gc570d_kthread_create(threadfn, data, namefmt, ##arg)

#define kthread_run(threadfn, data, namefmt, ...) \
({ \
	struct task_struct *__k = gc570d_kthread_create(threadfn, data, namefmt, ##__VA_ARGS__); \
	if (!IS_ERR(__k)) \
		wake_up_process(__k); \
	__k; \
})

/* Workqueues */
struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	work_func_t func;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t thread;
	bool initialized;
	bool pending;
	bool running;
	bool shutting_down;
	u64 due_ns;
};

struct delayed_work {
	struct work_struct work;
};

#define system_unbound_wq ((void *)0)

static inline struct delayed_work *to_delayed_work(struct work_struct *work)
{
	return container_of(work, struct delayed_work, work);
}

void gc570d_init_work(struct work_struct *work, work_func_t func);
bool gc570d_schedule_work_delay(struct work_struct *work, unsigned long delay_ms);
bool gc570d_cancel_work_sync(struct work_struct *work);

#define INIT_WORK(w, f) gc570d_init_work((w), (f))
#define INIT_DELAYED_WORK(dw, f) gc570d_init_work(&(dw)->work, (f))
#define schedule_work(w) gc570d_schedule_work_delay((w), 0)
#define schedule_delayed_work(dw, delay) gc570d_schedule_work_delay(&(dw)->work, (delay))
#define mod_delayed_work(wq, dw, delay) gc570d_schedule_work_delay(&(dw)->work, (delay))
#define cancel_work_sync(w) gc570d_cancel_work_sync((w))
#define cancel_delayed_work_sync(dw) gc570d_cancel_work_sync(&(dw)->work)

/* Device, PCI, and IRQ model */
struct device {
	const char *name;
	void *driver_data;
	struct pci_dev *pdev;
};

struct resource {
	resource_size_t start;
	resource_size_t end;
	unsigned long flags;
};

#define IORESOURCE_MEM 0x00000200UL
#define PCIE_LINK_STATE_L0S   BIT(0)
#define PCIE_LINK_STATE_L1    BIT(1)
#define PCIE_LINK_STATE_CLKPM BIT(2)
#define PCI_IRQ_INTX          BIT(0)
#define IRQF_SHARED           0x00000080

struct pci_device_id {
	u32 vendor, device, subvendor, subdevice;
};

#define PCI_DEVICE_SUB(vend, dev, subvend, subdev) \
	.vendor = (vend), .device = (dev), .subvendor = (subvend), .subdevice = (subdev)

struct pci_dev {
	struct device dev;
	char bdf[32];
	struct resource resource[6];
	void *bar0_vaddr;
	size_t bar0_size;
	int vfio_container_fd;
	int vfio_group_fd;
	int vfio_device_fd;
	uint64_t config_offset;
	int irq_eventfd;
	uint32_t irq_index;
	bool msi_enabled;
};

struct platform_device {
	struct device dev;
	const char *name;
};

#define PLATFORM_DEVID_NONE (-1)

typedef int irqreturn_t;
#define IRQ_NONE    0
#define IRQ_HANDLED 1
typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);

static inline const char *pci_name(const struct pci_dev *pdev)
{
	return pdev && pdev->bdf[0] ? pdev->bdf : "0000:00:00.0";
}

static inline void *dev_get_drvdata(const struct device *dev)
{
	return dev->driver_data;
}

static inline void dev_set_drvdata(struct device *dev, void *data)
{
	if (dev)
		dev->driver_data = data;
}

static inline void pci_set_drvdata(struct pci_dev *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

static inline struct pci_dev *to_pci_dev(struct device *dev)
{
	return dev ? dev->pdev : NULL;
}

static inline void *pci_get_drvdata(struct pci_dev *pdev)
{
	return dev_get_drvdata(&pdev->dev);
}

void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp);
int pcim_enable_device(struct pci_dev *pdev);
void pci_disable_link_state(struct pci_dev *pdev, int state);
resource_size_t pci_resource_len(struct pci_dev *pdev, int bar);
unsigned long pci_resource_flags(struct pci_dev *pdev, int bar);
void __iomem *pcim_iomap_region(struct pci_dev *pdev, int bar, const char *name);
int pci_alloc_irq_vectors(struct pci_dev *pdev, unsigned int min_vecs,
			  unsigned int max_vecs, unsigned int flags);
int pci_irq_vector(struct pci_dev *pdev, unsigned int nr);
bool pci_dev_msi_enabled(struct pci_dev *pdev);
void pci_free_irq_vectors(struct pci_dev *pdev);
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
		const char *name, void *dev);
void free_irq(unsigned int irq, void *dev_id);
void enable_irq(unsigned int irq);
void disable_irq(unsigned int irq);
void synchronize_irq(unsigned int irq);
void pci_set_master(struct pci_dev *pdev);
void pci_clear_master(struct pci_dev *pdev);
int dma_set_mask_and_coherent(struct device *dev, u64 mask);
void *dma_alloc_coherent(struct device *dev, size_t size,
			 dma_addr_t *dma_handle, gfp_t gfp);
void dma_free_coherent(struct device *dev, size_t size,
		       void *cpu_addr, dma_addr_t dma_handle);
struct platform_device *platform_device_register_resndata(
	struct device *parent, const char *name, int id,
	const void *res, unsigned int num, const void *data, size_t size);
void platform_device_unregister(struct platform_device *pdev);

/* Logging macros */
void gc570d_log_msg(const char *level, const struct device *dev, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#define dev_info(dev, fmt, ...) gc570d_log_msg("INFO", (dev), fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) gc570d_log_msg("WARN", (dev), fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)  gc570d_log_msg("ERR ", (dev), fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)  do { if (0) gc570d_log_msg("DBG ", (dev), fmt, ##__VA_ARGS__); } while (0)
#define dev_warn_ratelimited(dev, fmt, ...) gc570d_log_msg("WARN", (dev), fmt, ##__VA_ARGS__)
#define dev_dbg_ratelimited(dev, fmt, ...)  do { if (0) gc570d_log_msg("DBG ", (dev), fmt, ##__VA_ARGS__); } while (0)

static inline int dev_err_probe(const struct device *dev, int err, const char *fmt, ...)
{
	va_list args;
	fprintf(stderr, "[gc570d ERR ] ");
	if (dev && dev->name)
		fprintf(stderr, "%s: ", dev->name);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	return err;
}

/* File operations & Debugfs shim */
struct inode {
	void *i_private;
};

struct file {
	void *private_data;
};

struct seq_file {
	FILE *fp;
	void *private;
};

struct file_operations {
	void *owner;
	int (*open)(struct inode *, struct file *);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	loff_t (*llseek)(struct file *, loff_t, int);
	int (*release)(struct inode *, struct file *);
	int (*show_fn)(struct seq_file *, void *);
};

struct dentry {
	char name[64];
};

static inline int single_open(struct file *file, int (*show)(struct seq_file *, void *), void *data)
{
	file->private_data = data;
	(void)show;
	return 0;
}

static inline int simple_open(struct inode *inode, struct file *file)
{
	if (inode)
		file->private_data = inode->i_private;
	return 0;
}

#define seq_read NULL
#define seq_lseek NULL
#define single_release NULL
#define noop_llseek NULL
#define default_llseek NULL

#define seq_printf(s, fmt, ...) fprintf((s)->fp, fmt, ##__VA_ARGS__)
#define seq_puts(s, str) fputs((str), (s)->fp)

struct dentry *debugfs_create_dir(const char *name, struct dentry *parent);
struct dentry *debugfs_create_file(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops);
void debugfs_remove_recursive(struct dentry *dentry);

/* Multicolor LED shim */
enum led_brightness {
	LED_OFF = 0,
	LED_FULL = 255,
};

#define LED_COLOR_ID_RED   1
#define LED_COLOR_ID_GREEN 2
#define LED_COLOR_ID_BLUE  3
#define LED_COLOR_ID_RGB   8

struct device_attribute {
	const char *name;
	umode_t mode;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
};

#define DEVICE_ATTR(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = { \
		.name = #_name, .mode = (_mode), .show = (_show), .store = (_store) \
	}

struct led_classdev {
	const char *name;
	enum led_brightness brightness;
	enum led_brightness max_brightness;
	int color;
	int (*brightness_set_blocking)(struct led_classdev *led_cdev,
				       enum led_brightness brightness);
	struct device *dev;
};

struct mc_subled {
	unsigned int color_index;
	unsigned int brightness;
	unsigned int intensity;
	unsigned int channel;
};

struct led_classdev_mc {
	struct led_classdev led_cdev;
	unsigned int num_colors;
	struct mc_subled *subled_info;
};

static inline struct led_classdev_mc *lcdev_to_mccdev(struct led_classdev *led_cdev)
{
	return container_of(led_cdev, struct led_classdev_mc, led_cdev);
}

static inline int led_mc_calc_color_components(struct led_classdev_mc *mcled_cdev,
					       enum led_brightness brightness)
{
	unsigned int i;
	if (!mcled_cdev || !mcled_cdev->led_cdev.max_brightness)
		return -EINVAL;
	mcled_cdev->led_cdev.brightness = brightness;
	for (i = 0; i < mcled_cdev->num_colors; i++) {
		mcled_cdev->subled_info[i].brightness =
			brightness * mcled_cdev->subled_info[i].intensity /
			mcled_cdev->led_cdev.max_brightness;
	}
	return 0;
}

int led_classdev_multicolor_register(struct device *parent, struct led_classdev_mc *mcled_cdev);
void led_classdev_multicolor_unregister(struct led_classdev_mc *mcled_cdev);
int device_create_file(struct device *dev, const struct device_attribute *attr);
void device_remove_file(struct device *dev, const struct device_attribute *attr);

/*
 * V4L2 and VB2 shim.
 *
 * Files that talk to a real V4L2 device (rather than emulating a kernel
 * driver) define GC570D_USE_SYSTEM_V4L2 and include <linux/videodev2.h>
 * first.  In that case the public V4L2 types below must come from the
 * system header, otherwise the ioctl numbers and structure sizes would
 * not match the kernel.
 */
#ifndef GC570D_USE_SYSTEM_V4L2
#define V4L2_PIX_FMT_YUYV 0x56595559U
#define V4L2_FIELD_NONE 1
#define V4L2_COLORSPACE_REC709 3
#define V4L2_YCBCR_ENC_709 2
#define V4L2_QUANTIZATION_LIM_RANGE 2
#define V4L2_XFER_FUNC_709 2
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_CAP_VIDEO_CAPTURE 0x00000001
#define V4L2_CAP_STREAMING     0x04000000
#define V4L2_CAP_READWRITE     0x01000000
#define V4L2_CAP_TIMEPERFRAME  0x1000
#define V4L2_INPUT_TYPE_CAMERA 2
#define V4L2_IN_ST_NO_SIGNAL   0x00000002
#define V4L2_FRMSIZE_TYPE_DISCRETE 1
#define V4L2_FRMIVAL_TYPE_DISCRETE 1
#define V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC 0x00002000
#define VFL_TYPE_VIDEO 0
#endif /* !GC570D_USE_SYSTEM_V4L2 */

#define VB2_MMAP   BIT(0)
#define VB2_DMABUF BIT(2)
#define VB2_READ   BIT(3)

enum vb2_buffer_state {
	VB2_BUF_STATE_DEQUEUED = 0,
	VB2_BUF_STATE_QUEUED,
	VB2_BUF_STATE_ACTIVE,
	VB2_BUF_STATE_DONE,
	VB2_BUF_STATE_ERROR,
};

struct vb2_queue;

struct vb2_buffer {
	struct vb2_queue *vb2_queue;
	u64 timestamp;
	void *vaddr;
	dma_addr_t dma_addr;
	size_t plane_size;
	size_t payload;
	unsigned int index;
};

struct vb2_v4l2_buffer {
	struct vb2_buffer vb2_buf;
	u32 sequence;
	u32 field;
};

static inline struct vb2_v4l2_buffer *to_vb2_v4l2_buffer(struct vb2_buffer *vb)
{
	return container_of(vb, struct vb2_v4l2_buffer, vb2_buf);
}

struct vb2_ops {
	int (*queue_setup)(struct vb2_queue *q, unsigned int *num_buffers,
			   unsigned int *num_planes, unsigned int sizes[],
			   struct device *alloc_devs[]);
	int (*buf_prepare)(struct vb2_buffer *vb);
	void (*buf_queue)(struct vb2_buffer *vb);
	int (*start_streaming)(struct vb2_queue *q, unsigned int count);
	void (*stop_streaming)(struct vb2_queue *q);
};

struct vb2_queue {
	unsigned int type;
	unsigned int io_modes;
	void *drv_priv;
	size_t buf_struct_size;
	const struct vb2_ops *ops;
	const void *mem_ops;
	unsigned int timestamp_flags;
	struct mutex *lock;
	struct device *dev;
	unsigned int min_queued_buffers;
	bool streaming;
	int port_index;
};

extern const int vb2_dma_contig_memops;

static inline void *vb2_get_drv_priv(struct vb2_queue *vq)
{
	return vq->drv_priv;
}

static inline void *vb2_plane_vaddr(struct vb2_buffer *vb, unsigned int plane_no)
{
	(void)plane_no;
	return vb ? vb->vaddr : NULL;
}

static inline dma_addr_t vb2_dma_contig_plane_dma_addr(struct vb2_buffer *vb,
						       unsigned int plane_no)
{
	(void)plane_no;
	return vb ? vb->dma_addr : 0;
}

static inline unsigned long vb2_plane_size(struct vb2_buffer *vb, unsigned int plane_no)
{
	(void)plane_no;
	return vb ? vb->plane_size : 0;
}

static inline void vb2_set_plane_payload(struct vb2_buffer *vb,
					 unsigned int plane_no, unsigned long size)
{
	(void)plane_no;
	if (vb)
		vb->payload = size;
}

static inline bool vb2_is_busy(struct vb2_queue *vq)
{
	return vq && vq->streaming;
}

void vb2_buffer_done(struct vb2_buffer *vb, enum vb2_buffer_state state);
void vb2_queue_error(struct vb2_queue *vq);
int vb2_queue_init(struct vb2_queue *vq);

#ifndef GC570D_USE_SYSTEM_V4L2
struct v4l2_pix_format {
	u32 width;
	u32 height;
	u32 pixelformat;
	u32 field;
	u32 bytesperline;
	u32 sizeimage;
	u32 colorspace;
	u32 ycbcr_enc;
	u32 quantization;
	u32 xfer_func;
};

struct v4l2_format {
	u32 type;
	union {
		struct v4l2_pix_format pix;
	} fmt;
};

struct v4l2_capability {
	char driver[16];
	char card[32];
	char bus_info[32];
};

struct v4l2_fmtdesc {
	u32 index;
	u32 pixelformat;
};

struct v4l2_frmsizeenum {
	u32 index;
	u32 pixel_format;
	u32 type;
	struct {
		u32 width;
		u32 height;
	} discrete;
};

struct v4l2_fract {
	u32 numerator;
	u32 denominator;
};

struct v4l2_frmivalenum {
	u32 index;
	u32 pixel_format;
	u32 width;
	u32 height;
	u32 type;
	struct v4l2_fract discrete;
};

struct v4l2_input {
	u32 index;
	char name[32];
	u32 type;
	u32 status;
};

struct v4l2_captureparm {
	u32 capability;
	struct v4l2_fract timeperframe;
	u32 readbuffers;
};

struct v4l2_streamparm {
	u32 type;
	union {
		struct v4l2_captureparm capture;
	} parm;
};
#endif /* !GC570D_USE_SYSTEM_V4L2 */

struct v4l2_ioctl_ops {
	int (*vidioc_querycap)(struct file *file, void *fh, struct v4l2_capability *cap);
	int (*vidioc_enum_fmt_vid_cap)(struct file *file, void *fh, struct v4l2_fmtdesc *f);
	int (*vidioc_g_fmt_vid_cap)(struct file *file, void *fh, struct v4l2_format *f);
	int (*vidioc_try_fmt_vid_cap)(struct file *file, void *fh, struct v4l2_format *f);
	int (*vidioc_s_fmt_vid_cap)(struct file *file, void *fh, struct v4l2_format *f);
	int (*vidioc_enum_framesizes)(struct file *file, void *fh, struct v4l2_frmsizeenum *fsize);
	int (*vidioc_enum_frameintervals)(struct file *file, void *fh, struct v4l2_frmivalenum *fival);
	int (*vidioc_enum_input)(struct file *file, void *fh, struct v4l2_input *inp);
	int (*vidioc_g_input)(struct file *file, void *fh, unsigned int *i);
	int (*vidioc_s_input)(struct file *file, void *fh, unsigned int i);
	int (*vidioc_g_parm)(struct file *file, void *fh, struct v4l2_streamparm *a);
	int (*vidioc_s_parm)(struct file *file, void *fh, struct v4l2_streamparm *a);
	void *vidioc_reqbufs;
	void *vidioc_create_bufs;
	void *vidioc_prepare_buf;
	void *vidioc_querybuf;
	void *vidioc_qbuf;
	void *vidioc_dqbuf;
	void *vidioc_expbuf;
	void *vidioc_streamon;
	void *vidioc_streamoff;
};

struct v4l2_file_operations {
	void *owner;
	void *open;
	void *release;
	void *read;
	void *poll;
	void *mmap;
	void *unlocked_ioctl;
};

#define v4l2_fh_open NULL
#define vb2_fop_release NULL
#define vb2_fop_read NULL
#define vb2_fop_poll NULL
#define vb2_fop_mmap NULL
#define video_ioctl2 NULL
#define vb2_ioctl_reqbufs NULL
#define vb2_ioctl_create_bufs NULL
#define vb2_ioctl_prepare_buf NULL
#define vb2_ioctl_querybuf NULL
#define vb2_ioctl_qbuf NULL
#define vb2_ioctl_dqbuf NULL
#define vb2_ioctl_expbuf NULL
#define vb2_ioctl_streamon NULL
#define vb2_ioctl_streamoff NULL
#define video_device_release_empty NULL

struct v4l2_device {
	int dummy;
};

struct video_device {
	char name[32];
	struct v4l2_device *v4l2_dev;
	struct device *dev_parent;
	const struct v4l2_file_operations *fops;
	const struct v4l2_ioctl_ops *ioctl_ops;
	struct vb2_queue *queue;
	struct mutex *lock;
	void *release;
	u32 device_caps;
	void *drv_priv;
	int num;
};

static inline void video_set_drvdata(struct video_device *vdev, void *data)
{
	if (vdev)
		vdev->drv_priv = data;
}

static inline void *video_drvdata(struct file *file)
{
	return file ? file->private_data : NULL;
}

static inline int v4l2_device_register(struct device *dev, struct v4l2_device *v4l2_dev)
{
	(void)dev;
	(void)v4l2_dev;
	return 0;
}

static inline void v4l2_device_unregister(struct v4l2_device *v4l2_dev)
{
	(void)v4l2_dev;
}

int video_register_device(struct video_device *vdev, int type, int nr);
void video_unregister_device(struct video_device *vdev);

/* ALSA PCM & Control shim */
typedef unsigned long snd_pcm_uframes_t;

#define SNDRV_PCM_INFO_INTERLEAVED    0x00000001
#define SNDRV_PCM_INFO_BLOCK_TRANSFER 0x00010000
#define SNDRV_PCM_INFO_MMAP           0x00000002
#define SNDRV_PCM_INFO_MMAP_VALID     0x00000004
#define SNDRV_PCM_FMTBIT_S16_LE       (1ULL << 2)
#define SNDRV_PCM_RATE_48000          (1U << 7)
#define SNDRV_PCM_HW_PARAM_PERIODS    10
#define SNDRV_PCM_STREAM_CAPTURE      1
#define SNDRV_DMA_TYPE_VMALLOC        7

#define SNDRV_PCM_TRIGGER_STOP          0
#define SNDRV_PCM_TRIGGER_START         1
#define SNDRV_PCM_TRIGGER_PAUSE_PUSH    3
#define SNDRV_PCM_TRIGGER_PAUSE_RELEASE 4
#define SNDRV_PCM_TRIGGER_SUSPEND       5
#define SNDRV_PCM_TRIGGER_RESUME        6

#define SNDRV_CTL_ELEM_IFACE_MIXER    2
#define SNDRV_CTL_ELEM_TYPE_INTEGER   2
#define SNDRV_CTL_POWER_D0            0x0000
#define SNDRV_CTL_POWER_D3hot         0x0300

#define SND_JACK_LINEIN               0x0004

struct snd_pcm_hardware {
	unsigned int info;
	u64 formats;
	unsigned int rates;
	unsigned int rate_min;
	unsigned int rate_max;
	unsigned int channels_min;
	unsigned int channels_max;
	size_t buffer_bytes_max;
	size_t period_bytes_min;
	size_t period_bytes_max;
	unsigned int periods_min;
	unsigned int periods_max;
};

struct snd_pcm_runtime {
	struct snd_pcm_hardware hw;
	unsigned char *dma_area;
	snd_pcm_uframes_t buffer_size;
	unsigned int frame_bits;
};

struct snd_pcm;

struct snd_pcm_substream {
	struct snd_pcm *pcm;
	void *private_data;
	struct snd_pcm_runtime *runtime;
	int channel_index;
};

struct snd_pcm_ops {
	int (*open)(struct snd_pcm_substream *substream);
	int (*close)(struct snd_pcm_substream *substream);
	void *ioctl;
	int (*hw_free)(struct snd_pcm_substream *substream);
	int (*prepare)(struct snd_pcm_substream *substream);
	int (*trigger)(struct snd_pcm_substream *substream, int cmd);
	snd_pcm_uframes_t (*pointer)(struct snd_pcm_substream *substream);
};

#define snd_pcm_lib_ioctl NULL

struct snd_card {
	int number;
	char id[32];
	char driver[16];
	char shortname[32];
	char longname[80];
	struct snd_pcm *pcm;
};

struct snd_pcm {
	struct snd_card *card;
	char id[64];
	char name[80];
	void *private_data;
	const struct snd_pcm_ops *ops;
	struct snd_pcm_substream substream;
	struct snd_pcm_runtime runtime;
};

struct snd_jack {
	char name[64];
	int status;
};

struct snd_kcontrol {
	int dummy;
};

struct snd_ctl_elem_value {
	union {
		struct {
			long value[2];
		} integer;
	} value;
};

struct snd_ctl_elem_info {
	int type;
	unsigned int count;
	union {
		struct {
			long min;
			long max;
			long step;
		} integer;
	} value;
};

struct snd_kcontrol_new {
	int iface;
	const char *name;
	void *info;
	int (*get)(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol);
	int (*put)(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol);
};

#define snd_ctl_boolean_stereo_info NULL

#define snd_pcm_substream_chip(substream) ((substream)->pcm->private_data)
#define snd_devm_card_new snd_card_new

static inline size_t frames_to_bytes(struct snd_pcm_runtime *runtime, snd_pcm_uframes_t size)
{
	(void)runtime;
	return (size_t)size * 4;
}

static inline snd_pcm_uframes_t bytes_to_frames(struct snd_pcm_runtime *runtime, size_t size)
{
	(void)runtime;
	return (snd_pcm_uframes_t)(size / 4);
}

static inline int snd_pcm_hw_constraint_integer(struct snd_pcm_runtime *runtime, int var)
{
	(void)runtime;
	(void)var;
	return 0;
}

void snd_pcm_period_elapsed(struct snd_pcm_substream *substream);
void snd_pcm_stop_xrun(struct snd_pcm_substream *substream);
int snd_card_new(struct device *parent, int idx, const char *xid, void *module,
		 int extra_size, struct snd_card **card_ret);
int snd_pcm_new(struct snd_card *card, const char *id, int device,
		int playback_count, int capture_count, struct snd_pcm **rpcm);
void snd_pcm_set_ops(struct snd_pcm *pcm, int direction, const struct snd_pcm_ops *ops);
int snd_pcm_set_managed_buffer_all(struct snd_pcm *pcm, int type, struct device *data,
				   size_t size, size_t max);
int snd_jack_new(struct snd_card *card, const char *id, int type,
		 struct snd_jack **jack, bool initial_kctl, bool phantom_jack);
void snd_jack_report(struct snd_jack *jack, int status);
struct snd_kcontrol *snd_ctl_new1(const struct snd_kcontrol_new *ncontrol, void *private_data);
int snd_ctl_add(struct snd_card *card, struct snd_kcontrol *kcontrol);
int snd_card_register(struct snd_card *card);
void snd_pcm_suspend_all(struct snd_pcm *pcm);
void snd_card_disconnect_sync(struct snd_card *card);
void snd_card_free(struct snd_card *card);
static inline void snd_power_change_state(struct snd_card *card, unsigned int state)
{
	(void)card;
	(void)state;
}

/* Module & PCI driver macros */
struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table;
	int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	struct {
		const void *pm;
	} driver;
};

#define module_param(name, type, perm)
#define MODULE_PARM_DESC(name, desc)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_AUTHOR(a)
#define MODULE_DESCRIPTION(d)
#define MODULE_SOFTDEP(s)
#define MODULE_LICENSE(l)
#define SIMPLE_DEV_PM_OPS(name, suspend_fn, resume_fn) \
	const int name = (int)(sizeof(&(suspend_fn)) + sizeof(&(resume_fn)))
#define DEFINE_SIMPLE_DEV_PM_OPS(name, suspend_fn, resume_fn) \
	const int name = (int)(sizeof(&(suspend_fn)) + sizeof(&(resume_fn)))
#define pm_sleep_ptr(ptr) (ptr)
#define module_pci_driver(drv) \
	struct pci_driver *gc570d_get_pci_driver(void) { return &(drv); }

struct pci_driver *gc570d_get_pci_driver(void);

#endif /* GC570D_USER_COMPAT_H */

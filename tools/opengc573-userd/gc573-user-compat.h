/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573-user-compat.h — Userspace compatibility definitions for OpenGC573 VFIO driver
 *
 * Implements Linux kernel-style MMIO accessors, timing primitives, logging,
 * and data types for running hardware control code in userspace via vfio-pci.
 */

#ifndef GC573_USER_COMPAT_H
#define GC573_USER_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <endian.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <sys/eventfd.h>
#include <poll.h>

#ifndef BIT
#define BIT(nr)			(1UL << (nr))
#endif

#ifndef GENMASK
#define GENMASK(h, l) \
	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (__SIZEOF_LONG__ * 8 - 1 - (h))))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr)		(sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef min
#define min(a, b)		(((a) < (b)) ? (a) : (b))
#endif

#ifndef max
#define max(a, b)		(((a) > (b)) ? (a) : (b))
#endif

#ifndef clamp
#define clamp(val, lo, hi)	min(max(val, lo), hi)
#endif

/* Kernel type aliases */
typedef uint8_t			u8;
typedef uint16_t		u16;
typedef uint32_t		u32;
typedef uint64_t		u64;
typedef int8_t			s8;
typedef int16_t			s16;
typedef int32_t			s32;
typedef int64_t			s64;
typedef uint64_t		dma_addr_t;
typedef size_t			resource_size_t;

/* Sleep / Delay wrappers */
static inline void msleep(unsigned int msecs)
{
	usleep(msecs * 1000);
}

static inline void usleep_range(unsigned long min, unsigned long max)
{
	(void)max;
	usleep(min);
}

static inline void udelay(unsigned long usecs)
{
	usleep(usecs);
}

/* Logging macros */
#define pr_info(fmt, ...)	printf("[GC573-USER] " fmt "\n", ##__VA_ARGS__)
#define pr_warn(fmt, ...)	printf("[GC573-USER][WARN] " fmt "\n", ##__VA_ARGS__)
#define pr_err(fmt, ...)	fprintf(stderr, "[GC573-USER][ERROR] " fmt "\n", ##__VA_ARGS__)
#define pr_debug(fmt, ...)	do { } while (0)

#define dev_info(dev, fmt, ...)		pr_info(fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)		pr_warn(fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)		pr_err(fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)		pr_debug(fmt, ##__VA_ARGS__)

/* MMIO read/write accessors */
static inline uint32_t readl(const volatile void *addr)
{
	return *(const volatile uint32_t *)addr;
}

static inline void writel(uint32_t val, volatile void *addr)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint8_t readb(const volatile void *addr)
{
	return *(const volatile uint8_t *)addr;
}

static inline void writeb(uint8_t val, volatile void *addr)
{
	*(volatile uint8_t *)addr = val;
}

static inline uint16_t readw(const volatile void *addr)
{
	return *(const volatile uint16_t *)addr;
}

static inline void writew(uint16_t val, volatile void *addr)
{
	*(volatile uint16_t *)addr = val;
}

/* Mutex wrapper */
typedef pthread_mutex_t mutex_t;
#define mutex_init(m)		pthread_mutex_init(m, NULL)
#define mutex_destroy(m)	pthread_mutex_destroy(m)
#define mutex_lock(m)		pthread_mutex_lock(m)
#define mutex_unlock(m)		pthread_mutex_unlock(m)

#endif /* GC573_USER_COMPAT_H */

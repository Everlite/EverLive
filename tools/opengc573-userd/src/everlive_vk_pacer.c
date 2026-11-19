/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * everlive_vk_pacer.c — Lightweight Vulkan VK_LAYER_EVERLIVE_pacer layer
 *
 * Paces vkQueuePresentKHR downstream of frame-generation layers (such as
 * lsfg-vk) when displaying 60/90/120/180 FPS output onto higher-refresh FIFO
 * monitors (e.g. 240 Hz), preventing multi-frame burst-then-pause judder.
 *
 * Uses only fixed Khronos Vulkan 1.0 C-ABI loader/layer structs so building
 * requires no external vulkan-headers package.
 */

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_SUCCESS					0
#define VK_ERROR_INITIALIZATION_FAILED			(-3)
#define VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO	47
#define VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO	48
#define VK_LAYER_LINK_INFO				0
#define LAYER_NEGOTIATE_INTERFACE_STRUCT		1

#define PACER_MAX_ENTRIES	32

#if defined(__GNUC__) || defined(__clang__)
#define PACER_EXPORT __attribute__((visibility("default")))
#else
#define PACER_EXPORT
#endif

typedef int32_t VkResult;
typedef uint32_t VkStructureType;
typedef uint32_t VkLayerFunction;
typedef void (*PFN_vkVoidFunction)(void);

typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void *instance,
							  const char *pName);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(void *device,
							const char *pName);
typedef PFN_vkVoidFunction (*PFN_GetPhysicalDeviceProcAddr)(void *instance,
							      const char *pName);

typedef struct VkNegotiateLayerInterface_ {
	VkStructureType sType;
	void *pNext;
	uint32_t loaderLayerInterfaceVersion;
	PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;
	PFN_GetPhysicalDeviceProcAddr pfnGetPhysicalDeviceProcAddr;
} VkNegotiateLayerInterface;

typedef struct VkBaseStruct_ {
	VkStructureType sType;
	const void *pNext;
} VkBaseStruct;

typedef struct VkLayerInstanceLink_ {
	struct VkLayerInstanceLink_ *pNext;
	PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
	PFN_GetPhysicalDeviceProcAddr pfnNextGetPhysicalDeviceProcAddr;
} VkLayerInstanceLink;

typedef struct VkLayerInstanceCreateInfo_ {
	VkStructureType sType;
	const void *pNext;
	VkLayerFunction function;
	union {
		VkLayerInstanceLink *pLayerInfo;
		void *pfnSetInstanceLoaderData;
	} u;
} VkLayerInstanceCreateInfo;

typedef struct VkLayerDeviceLink_ {
	struct VkLayerDeviceLink_ *pNext;
	PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
} VkLayerDeviceLink;

typedef struct VkLayerDeviceCreateInfo_ {
	VkStructureType sType;
	const void *pNext;
	VkLayerFunction function;
	union {
		VkLayerDeviceLink *pLayerInfo;
		void *pfnSetDeviceLoaderData;
	} u;
} VkLayerDeviceCreateInfo;

typedef struct VkInstanceCreateInfo_ {
	VkStructureType sType;
	const void *pNext;
} VkInstanceCreateInfo;

typedef struct VkDeviceCreateInfo_ {
	VkStructureType sType;
	const void *pNext;
} VkDeviceCreateInfo;

typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo *pCreateInfo,
					 const void *pAllocator,
					 void **pInstance);
typedef void (*PFN_vkDestroyInstance)(void *instance, const void *pAllocator);
typedef VkResult (*PFN_vkCreateDevice)(void *physicalDevice,
				       const VkDeviceCreateInfo *pCreateInfo,
				       const void *pAllocator,
				       void **pDevice);
typedef void (*PFN_vkDestroyDevice)(void *device, const void *pAllocator);
typedef VkResult (*PFN_vkQueuePresentKHR)(void *queue, const void *pPresentInfo);

struct pacer_inst_entry {
	void *key;
	void *instance;
	PFN_vkGetInstanceProcAddr next_gipa;
	PFN_vkDestroyInstance next_destroy_instance;
};

struct pacer_dev_entry {
	void *key;
	PFN_vkGetDeviceProcAddr next_gdpa;
	PFN_vkDestroyDevice next_destroy_device;
	PFN_vkQueuePresentKHR next_queue_present;
};

static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pacer_inst_entry g_instances[PACER_MAX_ENTRIES];
static struct pacer_dev_entry g_devices[PACER_MAX_ENTRIES];

static pthread_mutex_t g_pacer_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pacer_period_ns;
static uint64_t g_next_present_ns;
static uint64_t g_present_count;
static int g_debug_cached = -1;

static bool pacer_debug_enabled(void)
{
	int cached = __atomic_load_n(&g_debug_cached, __ATOMIC_RELAXED);

	if (cached < 0) {
		const char *env = getenv("EVERLIVE_VK_PACER_DEBUG");

		cached = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
		__atomic_store_n(&g_debug_cached, cached, __ATOMIC_RELAXED);
	}
	return cached != 0;
}

static uint64_t pacer_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *dispatch_key(void *handle)
{
	return handle ? *(void **)handle : NULL;
}

PACER_EXPORT void everlive_vk_pacer_set_hz(double target_hz)
{
	/*
	 * Apply a 2% headroom factor so FIFO swapchains comfortably catch every
	 * target VBlank (e.g. ~8.17 ms -> every 2nd VBlank on 240 Hz = 120 FPS)
	 * without starving capture or drifting late.
	 */
	uint64_t period_ns = (isfinite(target_hz) && target_hz > 10.0 &&
			      target_hz < 235.0)
				     ? (uint64_t)(1000000000.0 / (target_hz * 1.02))
				     : 0ULL;

	__atomic_store_n(&g_pacer_period_ns, period_ns, __ATOMIC_RELAXED);
	pthread_mutex_lock(&g_pacer_lock);
	g_next_present_ns = 0;
	pthread_mutex_unlock(&g_pacer_lock);

	if (pacer_debug_enabled()) {
		fprintf(stderr,
			"[EVERLIVE-VK-PACER] Target %.2f Hz -> period %.2f ms\n",
			target_hz, (double)period_ns / 1000000.0);
	}
}

PACER_EXPORT uint64_t everlive_vk_pacer_get_period_ns(void)
{
	return __atomic_load_n(&g_pacer_period_ns, __ATOMIC_RELAXED);
}

PACER_EXPORT uint64_t everlive_vk_pacer_present_count(void)
{
	return __atomic_load_n(&g_present_count, __ATOMIC_RELAXED);
}

static void pacer_apply_env_hz(void)
{
	const char *env = getenv("EVERLIVE_VK_PACER_HZ");

	if (!env || !*env)
		return;

	char *end = NULL;
	double hz = strtod(env, &end);

	if (end != env && isfinite(hz))
		everlive_vk_pacer_set_hz(hz);
}

__attribute__((constructor))
static void pacer_ctor(void)
{
	pacer_apply_env_hz();
	if (pacer_debug_enabled())
		fprintf(stderr, "[EVERLIVE-VK-PACER] Layer library loaded\n");
}

static VkLayerInstanceCreateInfo *find_instance_link_info(
	const VkInstanceCreateInfo *pCreateInfo)
{
	const VkBaseStruct *cur = pCreateInfo ? (const VkBaseStruct *)pCreateInfo->pNext : NULL;

	while (cur) {
		if (cur->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
			VkLayerInstanceCreateInfo *info = (VkLayerInstanceCreateInfo *)(uintptr_t)cur;

			if (info->function == VK_LAYER_LINK_INFO)
				return info;
		}
		cur = (const VkBaseStruct *)cur->pNext;
	}
	return NULL;
}

static VkLayerDeviceCreateInfo *find_device_link_info(
	const VkDeviceCreateInfo *pCreateInfo)
{
	const VkBaseStruct *cur = pCreateInfo ? (const VkBaseStruct *)pCreateInfo->pNext : NULL;

	while (cur) {
		if (cur->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
			VkLayerDeviceCreateInfo *info = (VkLayerDeviceCreateInfo *)(uintptr_t)cur;

			if (info->function == VK_LAYER_LINK_INFO)
				return info;
		}
		cur = (const VkBaseStruct *)cur->pNext;
	}
	return NULL;
}

static struct pacer_inst_entry *find_instance_by_key_locked(void *key)
{
	if (!key)
		return NULL;
	for (size_t i = 0; i < PACER_MAX_ENTRIES; i++) {
		if (g_instances[i].key == key)
			return &g_instances[i];
	}
	return NULL;
}

static struct pacer_dev_entry *find_device_by_key_locked(void *key)
{
	if (!key)
		return NULL;
	for (size_t i = 0; i < PACER_MAX_ENTRIES; i++) {
		if (g_devices[i].key == key)
			return &g_devices[i];
	}
	return NULL;
}

static PFN_vkGetInstanceProcAddr lookup_next_gipa(void *instance)
{
	void *key = dispatch_key(instance);
	PFN_vkGetInstanceProcAddr fn = NULL;

	pthread_mutex_lock(&g_table_lock);
	struct pacer_inst_entry *e = find_instance_by_key_locked(key);

	if (e)
		fn = e->next_gipa;
	pthread_mutex_unlock(&g_table_lock);
	return fn;
}

static PFN_vkGetDeviceProcAddr lookup_next_gdpa(void *device)
{
	void *key = dispatch_key(device);
	PFN_vkGetDeviceProcAddr fn = NULL;

	pthread_mutex_lock(&g_table_lock);
	struct pacer_dev_entry *e = find_device_by_key_locked(key);

	if (e)
		fn = e->next_gdpa;
	pthread_mutex_unlock(&g_table_lock);
	return fn;
}

static VkResult everlive_QueuePresentKHR(void *queue, const void *pPresentInfo)
{
	void *key = dispatch_key(queue);
	PFN_vkQueuePresentKHR next_present = NULL;

	pthread_mutex_lock(&g_table_lock);
	struct pacer_dev_entry *e = find_device_by_key_locked(key);

	if (e)
		next_present = e->next_queue_present;
	pthread_mutex_unlock(&g_table_lock);

	if (!next_present)
		return VK_ERROR_INITIALIZATION_FAILED;

	__atomic_add_fetch(&g_present_count, 1ULL, __ATOMIC_RELAXED);

	uint64_t period_ns = __atomic_load_n(&g_pacer_period_ns, __ATOMIC_RELAXED);

	if (period_ns > 0) {
		uint64_t sleep_until = 0;

		pthread_mutex_lock(&g_pacer_lock);
		uint64_t now = pacer_now_ns();

		if (g_next_present_ns == 0 || now >= g_next_present_ns + period_ns) {
			g_next_present_ns = now + period_ns;
		} else if (now < g_next_present_ns) {
			sleep_until = g_next_present_ns;
			g_next_present_ns = sleep_until + period_ns;
		} else {
			g_next_present_ns = now + period_ns;
		}
		pthread_mutex_unlock(&g_pacer_lock);

		if (sleep_until > 0) {
			struct timespec ts;

			ts.tv_sec = (time_t)(sleep_until / 1000000000ULL);
			ts.tv_nsec = (long)(sleep_until % 1000000000ULL);
			while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
					       &ts, NULL) == EINTR)
				;
		}
	}

	return next_present(queue, pPresentInfo);
}

static VkResult everlive_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
					const void *pAllocator,
					void **pInstance)
{
	VkLayerInstanceCreateInfo *chain = find_instance_link_info(pCreateInfo);

	if (!chain || !chain->u.pLayerInfo)
		return VK_ERROR_INITIALIZATION_FAILED;

	PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;

	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

	if (!next_gipa)
		return VK_ERROR_INITIALIZATION_FAILED;

	PFN_vkCreateInstance next_create =
		(PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
	if (!next_create)
		return VK_ERROR_INITIALIZATION_FAILED;

	VkResult res = next_create(pCreateInfo, pAllocator, pInstance);

	if (res != VK_SUCCESS || !pInstance || !*pInstance)
		return res;

	void *key = dispatch_key(*pInstance);

	pthread_mutex_lock(&g_table_lock);
	struct pacer_inst_entry *slot = find_instance_by_key_locked(key);

	if (!slot) {
		for (size_t i = 0; i < PACER_MAX_ENTRIES; i++) {
			if (g_instances[i].key == NULL) {
				slot = &g_instances[i];
				break;
			}
		}
	}
	if (slot) {
		slot->key = key;
		slot->instance = *pInstance;
		slot->next_gipa = next_gipa;
		slot->next_destroy_instance =
			(PFN_vkDestroyInstance)next_gipa(*pInstance, "vkDestroyInstance");
	}
	pthread_mutex_unlock(&g_table_lock);

	if (pacer_debug_enabled()) {
		fprintf(stderr,
			"[EVERLIVE-VK-PACER] vkCreateInstance hooked (instance=%p)\n",
			*pInstance);
	}
	return res;
}

static void everlive_DestroyInstance(void *instance, const void *pAllocator)
{
	void *key = dispatch_key(instance);
	PFN_vkDestroyInstance next_destroy = NULL;

	pthread_mutex_lock(&g_table_lock);
	struct pacer_inst_entry *e = find_instance_by_key_locked(key);

	if (e) {
		next_destroy = e->next_destroy_instance;
		memset(e, 0, sizeof(*e));
	}
	pthread_mutex_unlock(&g_table_lock);

	if (next_destroy)
		next_destroy(instance, pAllocator);
}

static VkResult everlive_CreateDevice(void *physicalDevice,
				      const VkDeviceCreateInfo *pCreateInfo,
				      const void *pAllocator,
				      void **pDevice)
{
	VkLayerDeviceCreateInfo *chain = find_device_link_info(pCreateInfo);

	if (!chain || !chain->u.pLayerInfo)
		return VK_ERROR_INITIALIZATION_FAILED;

	PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;

	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

	if (!next_gipa || !next_gdpa)
		return VK_ERROR_INITIALIZATION_FAILED;

	PFN_vkCreateDevice next_create =
		(PFN_vkCreateDevice)next_gipa(NULL, "vkCreateDevice");

	if (!next_create) {
		void *phys_key = dispatch_key(physicalDevice);

		pthread_mutex_lock(&g_table_lock);
		struct pacer_inst_entry *ie = find_instance_by_key_locked(phys_key);

		if (!ie && g_instances[0].key != NULL)
			ie = &g_instances[0];
		if (ie)
			next_create = (PFN_vkCreateDevice)next_gipa(
				ie->instance, "vkCreateDevice");
		pthread_mutex_unlock(&g_table_lock);
	}
	if (!next_create)
		return VK_ERROR_INITIALIZATION_FAILED;

	VkResult res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);

	if (res != VK_SUCCESS || !pDevice || !*pDevice)
		return res;

	void *key = dispatch_key(*pDevice);
	PFN_vkQueuePresentKHR next_present =
		(PFN_vkQueuePresentKHR)next_gdpa(*pDevice, "vkQueuePresentKHR");
	PFN_vkDestroyDevice next_destroy =
		(PFN_vkDestroyDevice)next_gdpa(*pDevice, "vkDestroyDevice");

	pthread_mutex_lock(&g_table_lock);
	struct pacer_dev_entry *slot = find_device_by_key_locked(key);

	if (!slot) {
		for (size_t i = 0; i < PACER_MAX_ENTRIES; i++) {
			if (g_devices[i].key == NULL) {
				slot = &g_devices[i];
				break;
			}
		}
	}
	if (slot) {
		slot->key = key;
		slot->next_gdpa = next_gdpa;
		slot->next_destroy_device = next_destroy;
		slot->next_queue_present = next_present;
	}
	pthread_mutex_unlock(&g_table_lock);

	pacer_apply_env_hz();
	if (pacer_debug_enabled()) {
		fprintf(stderr,
			"[EVERLIVE-VK-PACER] vkCreateDevice hooked (device=%p, present=%p, period_ns=%llu)\n",
			*pDevice, (void *)next_present,
			(unsigned long long)everlive_vk_pacer_get_period_ns());
	}
	return res;
}

static void everlive_DestroyDevice(void *device, const void *pAllocator)
{
	void *key = dispatch_key(device);
	PFN_vkDestroyDevice next_destroy = NULL;

	pthread_mutex_lock(&g_table_lock);
	struct pacer_dev_entry *e = find_device_by_key_locked(key);

	if (e) {
		next_destroy = e->next_destroy_device;
		memset(e, 0, sizeof(*e));
	}
	pthread_mutex_unlock(&g_table_lock);

	pthread_mutex_lock(&g_pacer_lock);
	g_next_present_ns = 0;
	pthread_mutex_unlock(&g_pacer_lock);

	if (next_destroy)
		next_destroy(device, pAllocator);
}

PACER_EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(void *device, const char *pName)
{
	if (!pName)
		return NULL;
	if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
	if (strcmp(pName, "vkDestroyDevice") == 0)
		return (PFN_vkVoidFunction)everlive_DestroyDevice;
	if (strcmp(pName, "vkQueuePresentKHR") == 0)
		return (PFN_vkVoidFunction)everlive_QueuePresentKHR;

	PFN_vkGetDeviceProcAddr next_gdpa = lookup_next_gdpa(device);

	return next_gdpa ? next_gdpa(device, pName) : NULL;
}

PACER_EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(void *instance, const char *pName)
{
	if (!pName)
		return NULL;
	if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
		return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
	if (strcmp(pName, "vkCreateInstance") == 0)
		return (PFN_vkVoidFunction)everlive_CreateInstance;
	if (strcmp(pName, "vkDestroyInstance") == 0)
		return (PFN_vkVoidFunction)everlive_DestroyInstance;
	if (strcmp(pName, "vkCreateDevice") == 0)
		return (PFN_vkVoidFunction)everlive_CreateDevice;
	if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
	if (strcmp(pName, "vkDestroyDevice") == 0)
		return (PFN_vkVoidFunction)everlive_DestroyDevice;
	if (strcmp(pName, "vkQueuePresentKHR") == 0)
		return (PFN_vkVoidFunction)everlive_QueuePresentKHR;

	PFN_vkGetInstanceProcAddr next_gipa = lookup_next_gipa(instance);

	return next_gipa ? next_gipa(instance, pName) : NULL;
}

PACER_EXPORT VkResult vkNegotiateLoaderLayerInterfaceVersion(
	VkNegotiateLayerInterface *pVersionStruct)
{
	if (!pVersionStruct ||
	    pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
		return VK_ERROR_INITIALIZATION_FAILED;

	if (pVersionStruct->loaderLayerInterfaceVersion > 2)
		pVersionStruct->loaderLayerInterfaceVersion = 2;

	pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
	pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}

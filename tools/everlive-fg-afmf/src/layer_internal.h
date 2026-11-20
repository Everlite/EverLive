#pragma once

/* VK_LAYER_AFMF: what the loader plumbing's translation units share. Nothing here is visible
 * outside src/layer*.c. */

#include "layer.h"

#include "config.h"
#include "framegen.h"
#include "log.h"
#include "swapchain.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vk_layer.h>

#define AFMF_EXPORT __attribute__((visibility("default")))

/* Handle logging casts VkSwapchainKHR to void*, which only holds on 64-bit builds. A 32-bit layer
 * (for 32-bit DXVK titles) is a separate build target, not this one. */
_Static_assert(sizeof(void *) == 8, "64-bit builds only");

struct afmf_instance {
    void *key;
    VkInstance handle;
    uint32_t api_version; /* VkApplicationInfo::apiVersion, 1.0 when absent */
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties;
    PFN_vkGetPhysicalDeviceProperties get_properties;
    PFN_vkGetPhysicalDeviceProperties2 get_properties2; /* NULL on a 1.0 instance */
    PFN_vkGetPhysicalDeviceFeatures get_features;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions;
    struct afmf_instance *next;
};

/* Every dispatchable handle begins with the loader's dispatch pointer. It is shared by an instance
 * and its physical devices, and by a device and its queues, which is what makes it a usable key. */
static inline void *dispatch_key(const void *handle)
{
    void *key;
    memcpy(&key, handle, sizeof key);
    return key;
}

/* layer_registry.c: the instances and devices that passed through the layer. Readers on every
 * submit and present from every thread of the application; writers only at creation and
 * destruction. */
struct afmf_instance *afmf_instance_find(void *key);
struct afmf_instance *afmf_instance_take(void *key);
void afmf_instance_register(struct afmf_instance *inst);
struct afmf_device *afmf_device_find(void *key);
struct afmf_device *afmf_device_take(void *key);
void afmf_device_register(struct afmf_device *dev);
void afmf_device_free(struct afmf_device *dev);

/* layer_device_queues.c */
uint32_t afmf_choose_async_family(const struct afmf_device *dev, const VkDeviceCreateInfo *info,
                                  uint32_t *index);
uint32_t afmf_choose_shared_family(const struct afmf_device *dev, const VkDeviceCreateInfo *info,
                                   uint32_t *index);
bool afmf_device_extension_available(struct afmf_instance *inst, VkPhysicalDevice pd,
                                     const char *name);
const char **afmf_extensions_with(const VkDeviceCreateInfo *info, const char *name, uint32_t *count);
VkDeviceQueueCreateInfo *afmf_queues_with_extra(const VkDeviceCreateInfo *info, uint32_t family,
                                                uint32_t *count, float **priorities);
void afmf_device_record_queue(struct afmf_device *dev, VkQueue queue, uint32_t family);

/* layer_device_probe.c */
VkResult afmf_device_probe(struct afmf_device *dev, struct afmf_instance *inst,
                           VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
                           VkPhysicalDeviceProperties *properties);

/* layer_device_fns.c */
bool afmf_load_device_fns(struct afmf_device *dev, PFN_vkGetDeviceProcAddr next_gdpa, VkDevice *out);

/* The hooks, one file per level; the tables in layer.c hand them out. */
VKAPI_ATTR VkResult VKAPI_CALL afmf_CreateInstance(const VkInstanceCreateInfo *info,
                                                   const VkAllocationCallbacks *alloc, VkInstance *out);
VKAPI_ATTR void VKAPI_CALL afmf_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc);
VKAPI_ATTR VkResult VKAPI_CALL afmf_CreateDevice(VkPhysicalDevice physical_device,
                                                 const VkDeviceCreateInfo *info,
                                                 const VkAllocationCallbacks *alloc, VkDevice *out);
VKAPI_ATTR void VKAPI_CALL afmf_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc);
VKAPI_ATTR void VKAPI_CALL afmf_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
                                               VkQueue *queue);
VKAPI_ATTR void VKAPI_CALL afmf_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info,
                                                VkQueue *queue);
VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueSubmit(VkQueue queue, uint32_t count,
                                                const VkSubmitInfo *submits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueSubmit2(VkQueue queue, uint32_t count,
                                                 const VkSubmitInfo2 *submits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueSubmit2KHR(VkQueue queue, uint32_t count,
                                                    const VkSubmitInfo2 *submits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueBindSparse(VkQueue queue, uint32_t count,
                                                    const VkBindSparseInfo *binds, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL afmf_DeviceWaitIdle(VkDevice device);
VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueWaitIdle(VkQueue queue);

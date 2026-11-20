/* Which queue the layer works from, and the bookkeeping of the queues the application gets. */

#include "layer_internal.h"

/* The queue family the layer's own queue should come from: compute without graphics first (the
 * async compute engines on RDNA), then any compute family, provided it has a queue the
 * application did not ask for. Returns UINT32_MAX when there is none; *index is the queue index
 * to request within the family. */
uint32_t afmf_choose_async_family(const struct afmf_device *dev, const VkDeviceCreateInfo *info,
                                  uint32_t *index)
{
    for (int compute_only = 1; compute_only >= 0; compute_only--) {
        for (uint32_t f = 0; f < dev->queue_family_count; f++) {
            VkQueueFlags flags = dev->queue_families[f].queueFlags;
            if (!(flags & VK_QUEUE_COMPUTE_BIT) || (compute_only && (flags & VK_QUEUE_GRAPHICS_BIT)))
                continue;
            /* The family's capacity counts every request; the index of the layer's queue counts
             * only the unprotected ones, since vkGetDeviceQueue indexes those alone. */
            uint32_t requested = 0, unprotected = 0;
            for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
                const VkDeviceQueueCreateInfo *q = &info->pQueueCreateInfos[i];
                if (q->queueFamilyIndex != f)
                    continue;
                requested += q->queueCount;
                if (!(q->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT))
                    unprotected += q->queueCount;
            }
            if (requested < dev->queue_families[f].queueCount) {
                *index = unprotected;
                return f;
            }
        }
    }
    return UINT32_MAX;
}

/* When no family has a spare queue: the application's last queue in a compute family, compute
 * without graphics first. Returns UINT32_MAX when the application created no compute queue. */
uint32_t afmf_choose_shared_family(const struct afmf_device *dev, const VkDeviceCreateInfo *info,
                                   uint32_t *index)
{
    for (int compute_only = 1; compute_only >= 0; compute_only--) {
        for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
            const VkDeviceQueueCreateInfo *q = &info->pQueueCreateInfos[i];
            VkQueueFlags flags = dev->queue_families[q->queueFamilyIndex].queueFlags;
            if (!(flags & VK_QUEUE_COMPUTE_BIT) || (compute_only && (flags & VK_QUEUE_GRAPHICS_BIT)))
                continue;
            if (q->queueCount == 0 || (q->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT))
                continue;
            *index = q->queueCount - 1;
            return q->queueFamilyIndex;
        }
    }
    return UINT32_MAX;
}

bool afmf_device_extension_available(struct afmf_instance *inst, VkPhysicalDevice pd,
                                     const char *name)
{
    uint32_t count = 0;
    if (inst->enumerate_device_extensions(pd, NULL, &count, NULL) != VK_SUCCESS || count == 0)
        return false;
    VkExtensionProperties *props = calloc(count, sizeof *props);
    if (props == NULL)
        return false;
    bool found = false;
    if (inst->enumerate_device_extensions(pd, NULL, &count, props) == VK_SUCCESS)
        for (uint32_t i = 0; i < count && !found; i++)
            found = strcmp(props[i].extensionName, name) == 0;
    free(props);
    return found;
}

/* The application's extension list plus `name` (unchanged when already there); caller frees. */
const char **afmf_extensions_with(const VkDeviceCreateInfo *info, const char *name, uint32_t *count)
{
    const char **list = calloc(info->enabledExtensionCount + 1, sizeof *list);
    if (list == NULL)
        return NULL;
    bool present = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        list[i] = info->ppEnabledExtensionNames[i];
        present = present || strcmp(list[i], name) == 0;
    }
    *count = info->enabledExtensionCount;
    if (!present)
        list[(*count)++] = name;
    return list;
}

/* Copies the application's queue requests plus one queue in `family`. The arrays are the caller's
 * to free after vkCreateDevice returned; NULL on allocation failure. */
VkDeviceQueueCreateInfo *afmf_queues_with_extra(const VkDeviceCreateInfo *info, uint32_t family,
                                                uint32_t *count, float **priorities)
{
    static const float one = 1.0f;
    VkDeviceQueueCreateInfo *queues = calloc(info->queueCreateInfoCount + 1, sizeof *queues);
    if (queues == NULL)
        return NULL;
    memcpy(queues, info->pQueueCreateInfos, info->queueCreateInfoCount * sizeof *queues);
    *count = info->queueCreateInfoCount;
    *priorities = NULL;

    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
        /* The layer's queue joins the family's unprotected entry; a protected one would make
         * it protected too. A family may carry one entry of each kind, so when only the
         * protected one exists the layer adds an unprotected entry of its own below. */
        if (queues[i].queueFamilyIndex != family ||
            (queues[i].flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT))
            continue;
        /* The family is already requested: one more queue, its priorities array extended. */
        uint32_t n = queues[i].queueCount + 1;
        *priorities = calloc(n, sizeof **priorities);
        if (*priorities == NULL) {
            free(queues);
            return NULL;
        }
        memcpy(*priorities, queues[i].pQueuePriorities, queues[i].queueCount * sizeof **priorities);
        (*priorities)[n - 1] = one;
        queues[i].queueCount = n;
        queues[i].pQueuePriorities = *priorities;
        return queues;
    }
    queues[*count] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family,
        .queueCount = 1,
        .pQueuePriorities = &one,
    };
    (*count)++;
    return queues;
}

bool afmf_device_queue_family(struct afmf_device *dev, VkQueue queue, uint32_t *family)
{
    bool found = false;
    pthread_mutex_lock(&dev->lock);
    for (uint32_t i = 0; i < dev->queue_count && !found; i++) {
        if (dev->queues[i].handle == queue) {
            *family = dev->queues[i].family;
            found = true;
        }
    }
    pthread_mutex_unlock(&dev->lock);
    return found;
}

void afmf_device_record_queue(struct afmf_device *dev, VkQueue queue, uint32_t family)
{
    if (queue == VK_NULL_HANDLE)
        return;
    pthread_mutex_lock(&dev->lock);
    bool known = false;
    for (uint32_t i = 0; i < dev->queue_count && !known; i++)
        known = dev->queues[i].handle == queue;
    if (!known) {
        if (dev->queue_count < dev->queue_capacity) {
            dev->queues[dev->queue_count].handle = queue;
            dev->queues[dev->queue_count].family = family;
            dev->queue_count++;
        } else {
            AFMF_WARN("more queues handed out than VkDeviceCreateInfo declared; ignoring %p",
                      (void *)queue);
        }
    }
    pthread_mutex_unlock(&dev->lock);
}

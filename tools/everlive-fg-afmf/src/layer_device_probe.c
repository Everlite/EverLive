/* Everything about the physical device the creation needs: the queue families decide whether
 * the layer can ask for a compute queue of its own. */

#include "layer_internal.h"

VkResult afmf_device_probe(struct afmf_device *dev, struct afmf_instance *inst,
                           VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
                           VkPhysicalDeviceProperties *properties)
{
    inst->get_properties(physical_device, properties);
    dev->limits = properties->limits;
    dev->api_version = properties->apiVersion < inst->api_version ? properties->apiVersion
                                                                  : inst->api_version;
    dev->subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    if (inst->get_properties2 != NULL && dev->api_version >= VK_API_VERSION_1_1) {
        VkPhysicalDeviceProperties2 properties2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &dev->subgroup,
        };
        inst->get_properties2(physical_device, &properties2);
        dev->subgroup.pNext = NULL;
    }
    inst->get_memory_properties(physical_device, &dev->memory_properties);
    inst->get_queue_family_properties(physical_device, &dev->queue_family_count, NULL);
    dev->queue_families = calloc(dev->queue_family_count, sizeof *dev->queue_families);
    dev->app_families = calloc(info->queueCreateInfoCount, sizeof *dev->app_families);
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++)
        dev->queue_capacity += info->pQueueCreateInfos[i].queueCount;
    dev->queues = calloc(dev->queue_capacity, sizeof *dev->queues);
    if (dev->queue_families == NULL || dev->queues == NULL || dev->app_families == NULL)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    inst->get_queue_family_properties(physical_device, &dev->queue_family_count,
                                      dev->queue_families);
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
        uint32_t family = info->pQueueCreateInfos[i].queueFamilyIndex;
        bool known = false;
        for (uint32_t k = 0; k < dev->app_family_count && !known; k++)
            known = dev->app_families[k] == family;
        if (!known)
            dev->app_families[dev->app_family_count++] = family;
    }

    dev->storage_write_without_format =
        info->pEnabledFeatures != NULL && info->pEnabledFeatures->shaderStorageImageWriteWithoutFormat;
    dev->shader_int16 = info->pEnabledFeatures != NULL && info->pEnabledFeatures->shaderInt16;
    for (const VkBaseInStructure *s = info->pNext; s != NULL; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            const VkPhysicalDeviceFeatures2 *f2 = (const VkPhysicalDeviceFeatures2 *)s;
            dev->storage_write_without_format = f2->features.shaderStorageImageWriteWithoutFormat;
            dev->shader_int16 = f2->features.shaderInt16;
        }
    return VK_SUCCESS;
}

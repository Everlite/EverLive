/* The device chain: vkCreateDevice with the layer's queue and shaderInt16 added to the
 * application's request, and the retries that give the application its device whatever the
 * layer's additions did. */

#include "layer_internal.h"

static VkLayerDeviceCreateInfo *device_loader_info(const VkDeviceCreateInfo *info,
                                                   VkLayerFunction function)
{
    for (const VkBaseInStructure *p = info->pNext; p != NULL; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            continue;
        const VkLayerDeviceCreateInfo *link = (const VkLayerDeviceCreateInfo *)p;
        if (link->function == function)
            return (VkLayerDeviceCreateInfo *)(uintptr_t)link;
    }
    return NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL afmf_CreateDevice(VkPhysicalDevice physical_device,
                                                 const VkDeviceCreateInfo *info,
                                                 const VkAllocationCallbacks *alloc, VkDevice *out)
{
    struct afmf_instance *inst = afmf_instance_find(dispatch_key(physical_device));
    VkLayerDeviceCreateInfo *link = device_loader_info(info, VK_LAYER_LINK_INFO);
    VkLayerDeviceCreateInfo *loader_data = device_loader_info(info, VK_LOADER_DATA_CALLBACK);
    if (inst == NULL || link == NULL || link->u.pLayerInfo == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(inst->handle, "vkCreateDevice");
    if (next_create == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    struct afmf_device *dev = calloc(1, sizeof *dev);
    if (dev == NULL)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dev->physical_device = physical_device;
    dev->gdpa = next_gdpa;
    dev->set_loader_data = loader_data != NULL ? loader_data->u.pfnSetDeviceLoaderData : NULL;
    dev->lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    dev->async_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    dev->ifns.get_surface_capabilities = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)next_gipa(
        inst->handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    dev->ifns.get_surface_support = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)next_gipa(
        inst->handle, "vkGetPhysicalDeviceSurfaceSupportKHR");
    dev->ifns.get_surface_present_modes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)next_gipa(
        inst->handle, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    dev->ifns.get_surface_capabilities2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)next_gipa(
        inst->handle, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    dev->ifns.get_format_properties = (PFN_vkGetPhysicalDeviceFormatProperties)next_gipa(
        inst->handle, "vkGetPhysicalDeviceFormatProperties");

    VkPhysicalDeviceProperties properties;
    VkResult res = afmf_device_probe(dev, inst, physical_device, info, &properties);
    if (res != VK_SUCCESS) {
        afmf_device_free(dev);
        return res;
    }

    VkDeviceCreateInfo patched = *info;

    /* shaderInt16 for the block search's packed SAD, when the device offers it and the
     * application did not ask: added to its feature struct (a copy), or one of the layer's own
     * when it enabled nothing. A VkPhysicalDeviceFeatures2 deeper in the chain than its head
     * cannot be replaced without copying its predecessor; the search then runs the scalar SAD. */
    const VkPhysicalDeviceFeatures2 *app_features2 = NULL;
    for (const VkBaseInStructure *s = info->pNext; s != NULL; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            app_features2 = (const VkPhysicalDeviceFeatures2 *)s;
    VkPhysicalDeviceFeatures supported;
    VkPhysicalDeviceFeatures features = {0};
    VkPhysicalDeviceFeatures2 features2;
    inst->get_features(physical_device, &supported);
    bool app_int16 = dev->shader_int16; /* what the application asked for, before any patch */
    if (!afmf_config_get()->sad_int16)
        dev->shader_int16 = false; /* the feature may stay on; the search does not use it */
    else if (!dev->shader_int16 && supported.shaderInt16) {
        if (app_features2 == NULL) {
            if (info->pEnabledFeatures != NULL)
                features = *info->pEnabledFeatures;
            features.shaderInt16 = VK_TRUE;
            patched.pEnabledFeatures = &features;
            dev->shader_int16 = true;
        } else if ((const void *)app_features2 == info->pNext) {
            features2 = *app_features2;
            features2.features.shaderInt16 = VK_TRUE;
            patched.pNext = &features2;
            dev->shader_int16 = true;
        } else {
            AFMF_DEBUG("shaderInt16 not enabled by the application and its feature chain cannot be"
                       " patched; the block search uses the scalar SAD");
        }
    }

    /* Ask for the layer's queue alongside the application's. */
    VkDeviceQueueCreateInfo *queues = NULL;
    float *priorities = NULL;
    uint32_t async_index = 0;
    uint32_t async_family = dev->set_loader_data != NULL && afmf_config_get()->async
                                ? afmf_choose_async_family(dev, info, &async_index)
                                : UINT32_MAX;
    if (async_family != UINT32_MAX) {
        queues = afmf_queues_with_extra(info, async_family, &patched.queueCreateInfoCount, &priorities);
        if (queues != NULL)
            patched.pQueueCreateInfos = queues;
        else
            async_family = UINT32_MAX;
    }

    /* When the layer's queue is an entry of its own, ask for high global priority so its work is
     * not starved by the application's, as the driver-level implementation does. amdgpu only
     * grants it to processes with CAP_SYS_NICE (or the DRM master), so the plain request is
     * kept as the fallback. */
    const char **extensions = NULL;
    VkDeviceQueueGlobalPriorityCreateInfoKHR high = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR,
        .globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR,
    };
    bool own_entry = async_family != UINT32_MAX &&
                     patched.queueCreateInfoCount == info->queueCreateInfoCount + 1;
    bool try_high = own_entry && afmf_device_extension_available(inst, physical_device,
                                                                 VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME);
    if (try_high) {
        extensions = afmf_extensions_with(info, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME,
                                          &patched.enabledExtensionCount);
        try_high = extensions != NULL;
    }
    if (try_high) {
        patched.ppEnabledExtensionNames = extensions;
        high.pNext = queues[patched.queueCreateInfoCount - 1].pNext;
        queues[patched.queueCreateInfoCount - 1].pNext = &high;
    }

    /* Every layer below advances the chain link as it goes, so a retry has to put it back to
     * where this layer left it. */
    VkLayerDeviceLink *next_link = link->u.pLayerInfo->pNext;
    link->u.pLayerInfo = next_link;
    res = next_create(physical_device, &patched, alloc, out);
    if (res != VK_SUCCESS && try_high) {
        /* Not permitted (or not liked): the same device without the priority request. */
        link->u.pLayerInfo = next_link;
        queues[patched.queueCreateInfoCount - 1].pNext = high.pNext;
        patched.ppEnabledExtensionNames = info->ppEnabledExtensionNames;
        patched.enabledExtensionCount = info->enabledExtensionCount;
        try_high = false;
        res = next_create(physical_device, &patched, alloc, out);
    }
    if (res != VK_SUCCESS && (async_family != UINT32_MAX || patched.pEnabledFeatures != info->pEnabledFeatures ||
                              patched.pNext != info->pNext)) {
        /* Whatever the layer added (its queue, shaderInt16) may be what failed: the
         * application's request as it came, so an implicit layer never fails a device that
         * would have been created without it. The layer then shares a queue and, without
         * shaderInt16, runs the scalar SAD. */
        link->u.pLayerInfo = next_link;
        async_family = UINT32_MAX;
        dev->shader_int16 = afmf_config_get()->sad_int16 && app_int16;
        res = next_create(physical_device, info, alloc, out);
    }
    dev->async_high_priority = try_high;
    free(extensions);
    free(queues);
    free(priorities);
    if (res != VK_SUCCESS) {
        afmf_device_free(dev);
        return res;
    }

    if (!afmf_load_device_fns(dev, next_gdpa, out)) {
        PFN_vkDestroyDevice next_destroy = (PFN_vkDestroyDevice)next_gdpa(*out, "vkDestroyDevice");
        if (next_destroy != NULL)
            next_destroy(*out, alloc);
        afmf_device_free(dev);
        *out = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    dev->key = dispatch_key(*out);
    dev->handle = *out;

    if (async_family == UINT32_MAX && dev->set_loader_data != NULL && afmf_config_get()->async) {
        async_family = afmf_choose_shared_family(dev, info, &async_index);
        dev->async_shared = async_family != UINT32_MAX;
    }
    if (async_family != UINT32_MAX) {
        /* Obtained below the loader's trampoline: stamp the dispatch pointer, as for command
         * buffers, or the next layer cannot route calls made with it. (A shared queue gets the
         * same stamp again when the application fetches it; the loader's value is identical.) */
        dev->fns.get_device_queue(*out, async_family, async_index, &dev->async_queue);
        if (dev->async_queue != VK_NULL_HANDLE &&
            dev->set_loader_data(*out, dev->async_queue) != VK_SUCCESS)
            dev->async_queue = VK_NULL_HANDLE;
        dev->async_family = async_family;
        dev->async_shared = dev->async_shared && dev->async_queue != VK_NULL_HANDLE;
    }
    afmf_device_register(dev);

    /* The shaders compile while the application sets itself up, not on its first present. */
    if (dev->fns.queue_present != NULL)
        afmf_framegen_pipelines_prepare(dev);

    if (dev->async_queue != VK_NULL_HANDLE && !dev->async_shared)
        AFMF_INFO("device %p created, VK_KHR_swapchain %s, layer queue on family %u (%s priority)",
                  (void *)*out, dev->fns.queue_present != NULL ? "enabled" : "not enabled",
                  dev->async_family, dev->async_high_priority ? "high" : "normal");
    else if (dev->async_queue != VK_NULL_HANDLE)
        AFMF_INFO("device %p created, VK_KHR_swapchain %s, no spare compute queue: sharing the "
                  "application's queue %u of family %u",
                  (void *)*out, dev->fns.queue_present != NULL ? "enabled" : "not enabled",
                  async_index, dev->async_family);
    else
        AFMF_INFO("device %p created, VK_KHR_swapchain %s, no compute queue to use: working on "
                  "the presenting queue",
                  (void *)*out, dev->fns.queue_present != NULL ? "enabled" : "not enabled");
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++)
        AFMF_DEBUG("device %p: application asked for %u queue(s) in family %u", (void *)*out,
                   info->pQueueCreateInfos[i].queueCount,
                   info->pQueueCreateInfos[i].queueFamilyIndex);
    AFMF_DEBUG("device %p: %s, subgroups of %u lanes (operations 0x%x in stages 0x%x)",
               (void *)*out, properties.deviceName, dev->subgroup.subgroupSize,
               dev->subgroup.supportedOperations, dev->subgroup.supportedStages);
    return VK_SUCCESS;
}

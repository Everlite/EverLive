/* The instance chain: creation and destruction, and the loader's chain links. */

#include "layer_internal.h"

/* The loader hands each layer the chain link inside pNext precisely so the layer advances it
 * (LoaderLayerInterface.md, "Layer Dispatch Initialization"). The const on pNext is the API's
 * promise to the driver, not to the loader's own structure, hence the cast through uintptr_t. */
static VkLayerInstanceCreateInfo *instance_link_info(const VkInstanceCreateInfo *info)
{
    for (const VkBaseInStructure *p = info->pNext; p != NULL; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)
            continue;
        const VkLayerInstanceCreateInfo *link = (const VkLayerInstanceCreateInfo *)p;
        if (link->function == VK_LAYER_LINK_INFO)
            return (VkLayerInstanceCreateInfo *)(uintptr_t)link;
    }
    return NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL afmf_CreateInstance(const VkInstanceCreateInfo *info,
                                                   const VkAllocationCallbacks *alloc, VkInstance *out)
{
    if (afmf_config_get()->invalid)
        AFMF_WARN("an AFMF_* variable is set to an invalid value; using its default");

    VkLayerInstanceCreateInfo *link = instance_link_info(info);
    if (link == NULL || link->u.pLayerInfo == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    if (next_create == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult res = next_create(info, alloc, out);
    if (res != VK_SUCCESS)
        return res;

    struct afmf_instance *inst = calloc(1, sizeof *inst);
    PFN_vkDestroyInstance next_destroy = (PFN_vkDestroyInstance)next_gipa(*out, "vkDestroyInstance");
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory =
        (PFN_vkGetPhysicalDeviceMemoryProperties)next_gipa(*out,
                                                           "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_families =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)next_gipa(
            *out, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties)next_gipa(*out, "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions =
        (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(*out,
                                                            "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceFeatures get_features =
        (PFN_vkGetPhysicalDeviceFeatures)next_gipa(*out, "vkGetPhysicalDeviceFeatures");
    if (inst == NULL || next_destroy == NULL || get_memory == NULL || get_families == NULL ||
        get_properties == NULL || enumerate_extensions == NULL || get_features == NULL) {
        if (next_destroy != NULL)
            next_destroy(*out, alloc);
        free(inst);
        *out = VK_NULL_HANDLE;
        return inst == NULL ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
    }

    inst->key = dispatch_key(*out);
    inst->handle = *out;
    inst->api_version = info->pApplicationInfo != NULL && info->pApplicationInfo->apiVersion != 0
                            ? info->pApplicationInfo->apiVersion
                            : VK_API_VERSION_1_0;
    inst->gipa = next_gipa;
    inst->destroy_instance = next_destroy;
    inst->get_memory_properties = get_memory;
    inst->get_queue_family_properties = get_families;
    inst->get_properties = get_properties;
    inst->get_properties2 = inst->api_version >= VK_API_VERSION_1_1
                                ? (PFN_vkGetPhysicalDeviceProperties2)next_gipa(
                                      *out, "vkGetPhysicalDeviceProperties2")
                                : NULL;
    inst->get_features = get_features;
    inst->enumerate_device_extensions = enumerate_extensions;
    afmf_instance_register(inst);

    /* Which display sockets the process can see: a Wine process without WAYLAND_DISPLAY falls
     * back to X11 whatever PROTON_ENABLE_WAYLAND says, and the surface line later shows it. */
    AFMF_INFO("layer active on instance %p (WAYLAND_DISPLAY %s, DISPLAY %s)", (void *)*out,
              getenv("WAYLAND_DISPLAY") != NULL ? "set" : "unset",
              getenv("DISPLAY") != NULL ? "set" : "unset");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL afmf_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    if (instance == VK_NULL_HANDLE)
        return;

    struct afmf_instance *inst = afmf_instance_take(dispatch_key(instance));
    if (inst == NULL) {
        AFMF_ERR("vkDestroyInstance on instance %p the layer never created", (void *)instance);
        return;
    }
    inst->destroy_instance(instance, alloc);
    free(inst);
}

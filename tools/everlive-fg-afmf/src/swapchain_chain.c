/* The swapchain create chain: what the surface offers, and the copy of the application's chain
 * that lets the swapchain be created in MAILBOX. */

#include "swapchain_internal.h"

bool afmf_sc_surface_offers(const struct afmf_device *dev, VkSurfaceKHR surface, VkPresentModeKHR mode)
{
    VkPresentModeKHR modes[16];
    uint32_t n = 16;
    if (dev->ifns.get_surface_present_modes == NULL)
        return false;
    VkResult res = dev->ifns.get_surface_present_modes(dev->physical_device, surface, &n, modes);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;
    for (uint32_t i = 0; i < n; i++)
        if (modes[i] == mode)
            return true;
    return false;
}

/* Size of a structure that may hang off VkSwapchainCreateInfoKHR, 0 for one the layer does not
 * know (the chain is then left alone). */
static size_t chain_node_size(VkStructureType type)
{
    switch ((int)type) {
    case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT:
        return sizeof(VkSwapchainPresentModesCreateInfoEXT);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT:
        return sizeof(VkSwapchainPresentScalingCreateInfoEXT);
    case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO:
        return sizeof(VkImageFormatListCreateInfo);
    case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT:
        return sizeof(VkImageCompressionControlEXT);
    case VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR:
        return sizeof(VkDeviceGroupSwapchainCreateInfoKHR);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT:
        return sizeof(VkSwapchainCounterCreateInfoEXT);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_DISPLAY_NATIVE_HDR_CREATE_INFO_AMD:
        return sizeof(VkSwapchainDisplayNativeHdrCreateInfoAMD);
#ifdef VK_NV_low_latency2
    case VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV:
        return sizeof(VkSwapchainLatencyCreateInfoNV);
#endif
#ifdef VK_NV_present_barrier
    case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_BARRIER_CREATE_INFO_NV:
        return sizeof(VkSwapchainPresentBarrierCreateInfoNV);
#endif
    default:
        return 0;
    }
}

/* Whether the swapchain may be created in MAILBOX with this chain. The list of allowed per-present
 * modes (VK_EXT_swapchain_maintenance1) must only hold modes compatible with the swapchain's, so
 * when the application gave one the whole chain is copied into `storage` (the chain is const,
 * and a node cannot be replaced without copying what precedes it) with the list cut down to what
 * the surface declares compatible with MAILBOX, MAILBOX included; `modes` receives that list. `*out`
 * is the chain to use. False means a structure the layer cannot copy, or no way to ask the surface:
 * the application's mode then stays. */
bool afmf_sc_chain_allows_mailbox(const struct afmf_device *dev, VkSurfaceKHR surface,
                                  const void *chain, uint64_t *storage, VkPresentModeKHR *modes,
                                  uint32_t *mode_count, const void **out)
{
    const VkSwapchainPresentModesCreateInfoEXT *list = NULL;
    for (const VkBaseInStructure *s = chain; s != NULL; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT)
            list = (const VkSwapchainPresentModesCreateInfoEXT *)s;
    *out = chain;
    *mode_count = 0;
    if (list == NULL)
        return true;

    /* What may share a swapchain with MAILBOX on this surface. */
    VkPresentModeKHR compatible[16];
    VkSurfacePresentModeCompatibilityEXT compatibility = {
        .sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT,
        .presentModeCount = 16,
        .pPresentModes = compatible,
    };
    VkSurfaceCapabilities2KHR caps2 = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
                                       .pNext = &compatibility};
    VkSurfacePresentModeEXT mailbox = {.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT,
                                       .presentMode = VK_PRESENT_MODE_MAILBOX_KHR};
    VkPhysicalDeviceSurfaceInfo2KHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        .pNext = &mailbox,
        .surface = surface,
    };
    if (dev->ifns.get_surface_capabilities2 == NULL ||
        dev->ifns.get_surface_capabilities2(dev->physical_device, &surface_info, &caps2) != VK_SUCCESS)
        return false;
    uint32_t n = 0;
    for (uint32_t i = 0; i < list->presentModeCount; i++) {
        bool ok = false;
        for (uint32_t k = 0; k < compatibility.presentModeCount && !ok; k++)
            ok = compatible[k] == list->pPresentModes[i];
        if (!ok || list->pPresentModes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            continue;
        if (n == AFMF_MAX_PRESENT_MODES - 1)
            return false; /* more modes than the layer can carry: the application's mode stays */
        modes[n++] = list->pPresentModes[i];
    }
    modes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    *mode_count = n;

    size_t used = 0;
    VkBaseOutStructure *prev = NULL;
    for (const VkBaseInStructure *s = chain; s != NULL; s = s->pNext) {
        size_t size = chain_node_size(s->sType);
        used = (used + 15u) & ~(size_t)15u;
        if (size == 0 || used + size > AFMF_CHAIN_BYTES)
            return false;
        size_t index = used / sizeof *storage;
        VkBaseOutStructure *copy = (VkBaseOutStructure *)(storage + index);
        memcpy(copy, s, size);
        copy->pNext = NULL;
        if (copy->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT) {
            VkSwapchainPresentModesCreateInfoEXT *l = (VkSwapchainPresentModesCreateInfoEXT *)copy;
            l->presentModeCount = n;
            l->pPresentModes = modes;
        }
        if (prev != NULL)
            prev->pNext = copy;
        else
            *out = copy;
        prev = copy;
        used += size;
    }
    return true;
}

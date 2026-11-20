/* The swapchain's creation and destruction: what the application asked for, patched with the
 * extra images, the usage and the present mode generation needs. */

#include "swapchain_internal.h"

/* Why this swapchain cannot host generated frames, or NULL when it can. */
static const char *generation_blocker(const struct afmf_device *dev,
                                      const VkSwapchainCreateInfoKHR *info,
                                      VkSurfaceCapabilitiesKHR *caps)
{
    const VkImageUsageFlags copy_usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (afmf_config_get()->passive)
        return "this process is Gamescope itself (AFMF_GAMESCOPE=1); the game inside gets the layer";
    if (dev->ifns.get_surface_capabilities == NULL ||
        dev->ifns.get_surface_capabilities(dev->physical_device, info->surface, caps) != VK_SUCCESS)
        return "surface capabilities unavailable";
    if (info->imageArrayLayers != 1)
        return "layered swapchain";
    if (info->presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
        info->presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR)
        return "shared present mode";
    if ((caps->supportedUsageFlags & copy_usage) != copy_usage)
        return "surface images cannot be copied";
    if (caps->maxImageCount != 0 &&
        info->minImageCount + afmf_config_get()->extra_images > caps->maxImageCount)
        return "no room for the extra images";
    return NULL;
}

/* The families the images must be usable from without ownership transfers when the layer works
 * and presents from a queue of its own: the application's plus ours. True when the layer's
 * queue can present to this surface. */
static bool share_with_layer_queue(const struct afmf_device *dev, const VkSwapchainCreateInfoKHR *info,
                                   VkSwapchainCreateInfoKHR *patched, uint32_t *families)
{
    VkBool32 can_present = VK_FALSE;
    if (dev->async_queue == VK_NULL_HANDLE || dev->ifns.get_surface_support == NULL ||
        dev->ifns.get_surface_support(dev->physical_device, dev->async_family, info->surface,
                                      &can_present) != VK_SUCCESS ||
        !can_present)
        return false;
    uint32_t n = 0;
    const uint32_t *base = info->imageSharingMode == VK_SHARING_MODE_CONCURRENT
                               ? info->pQueueFamilyIndices
                               : dev->app_families;
    uint32_t base_count = info->imageSharingMode == VK_SHARING_MODE_CONCURRENT
                              ? info->queueFamilyIndexCount
                              : dev->app_family_count;
    for (uint32_t i = 0; i < base_count && n < AFMF_MAX_FAMILIES; i++)
        if (base[i] != dev->async_family)
            families[n++] = base[i];
    if (n < AFMF_MAX_FAMILIES)
        families[n++] = dev->async_family;
    if (n >= 2) {
        patched->imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        patched->queueFamilyIndexCount = n;
        patched->pQueueFamilyIndices = families;
    }
    return true; /* n == 1 means the application already lives on our family */
}

VkResult afmf_swapchain_create(struct afmf_device *dev, const VkSwapchainCreateInfoKHR *info,
                               const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    VkSurfaceCapabilitiesKHR caps;
    const char *blocker = generation_blocker(dev, info, &caps);

    VkSwapchainCreateInfoKHR patched = *info;
    uint32_t families[AFMF_MAX_FAMILIES];
    _Alignas(16) uint64_t chain_storage[AFMF_CHAIN_BYTES / sizeof(uint64_t)];
    VkPresentModeKHR chain_modes[AFMF_MAX_PRESENT_MODES];
    uint32_t chain_mode_count = 0;
    bool async = false;
    bool fifo_to_mailbox = false;
    bool storage_usage = false;
    bool sampled_usage = false;
    if (blocker == NULL) {
        /* Direct ingest: the layer samples the game's frame straight into its colour ring and
         * luma (one read instead of a copy and a luma pass); needs SAMPLED usage on the images. */
        if (afmf_config_get()->direct_ingest) {
            sampled_usage = (caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 &&
                            afmf_framegen_can_ingest_direct(dev, info->imageFormat);
            if (sampled_usage)
                patched.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            else
                AFMF_INFO("direct ingest unavailable for format %d on this surface; copying",
                          (int)info->imageFormat);
        }
        /* Direct output: the interpolator writes the swapchain image, so it needs STORAGE usage;
         * only where the surface and the format take it (sRGB formats do not). */
        if (afmf_config_get()->direct_output) {
            storage_usage = (caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) != 0 &&
                            afmf_framegen_can_write_direct(dev, info->imageFormat);
            if (storage_usage)
                patched.imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
            else
                AFMF_INFO("direct output unavailable for format %d on this surface; copying",
                          (int)info->imageFormat);
        }
        /* Every present takes a refresh slot in FIFO and the layer doubles the presents: a game
         * above half the refresh rate loses real frames (120 at 165 Hz -> 82). MAILBOX shows the
         * latest frame and drops the excess instead, which is what the doubling needs. */
        if (afmf_config_get()->present_mode == AFMF_PRESENT_AUTO &&
            (info->presentMode == VK_PRESENT_MODE_FIFO_KHR ||
             info->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) &&
            afmf_sc_surface_offers(dev, info->surface, VK_PRESENT_MODE_MAILBOX_KHR)) {
            if (afmf_sc_chain_allows_mailbox(dev, info->surface, info->pNext, chain_storage,
                                             chain_modes, &chain_mode_count, &patched.pNext)) {
                patched.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                fifo_to_mailbox = true;
            } else {
                AFMF_INFO("swapchain create chain carries a structure the layer cannot copy: "
                          "present mode %d kept",
                          (int)info->presentMode);
            }
        }
        patched.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        /* Generated frames are presented from images of the same swapchain; the presentation
         * engine keeps a few queued, so one extra is not enough to find one free at present time. */
        patched.minImageCount += afmf_config_get()->extra_images;
        async = share_with_layer_queue(dev, info, &patched, families);
    }

    VkResult res = dev->fns.create_swapchain(dev->handle, &patched, alloc, out);
    if (res != VK_SUCCESS)
        return res;

    struct afmf_swapchain *sc = calloc(1, sizeof *sc);
    if (sc == NULL) {
        dev->fns.destroy_swapchain(dev->handle, *out, alloc);
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    sc->handle = *out;
    sc->dev = dev;
    sc->format = info->imageFormat;
    sc->extent = info->imageExtent;
    sc->present_mode = patched.presentMode;
    sc->fifo_to_mailbox = fifo_to_mailbox;
    sc->storage_usage = storage_usage;
    sc->sampled_usage = sampled_usage;
    memcpy(sc->allowed_modes, chain_modes, chain_mode_count * sizeof *chain_modes);
    sc->allowed_mode_count = chain_mode_count;
    sc->min_image_count = info->minImageCount;
    sc->async = async;
    sc->deferred_result = VK_SUCCESS;
    pthread_mutex_init(&sc->wsi_lock, NULL);
    pthread_mutex_init(&sc->wsi_turn, NULL);
    pthread_mutex_init(&sc->job_lock, NULL);
    pthread_mutex_init(&sc->work_lock, NULL);
    pthread_cond_init(&sc->work_cond, NULL);
    pthread_cond_init(&sc->work_drain_cond, NULL);
    pthread_condattr_t monotonic;
    pthread_condattr_init(&monotonic);
    pthread_condattr_setclock(&monotonic, CLOCK_MONOTONIC);
    pthread_cond_init(&sc->job_cond, &monotonic);
    pthread_cond_init(&sc->drain_cond, NULL);
    pthread_condattr_destroy(&monotonic);

    if (blocker == NULL) {
        sc->gen_enabled = true;
        VkResult gen = afmf_sc_gen_init(dev, sc);
        if (gen != VK_SUCCESS) {
            AFMF_WARN("swapchain %p: generation resources failed (VkResult %d); pass-through",
                      (void *)sc->handle, (int)gen);
            afmf_sc_gen_teardown(dev, sc);
        }
    } else {
        AFMF_INFO("swapchain %p: pass-through (%s)", (void *)sc->handle, blocker);
    }

    pthread_mutex_lock(&dev->lock);
    sc->next = dev->swapchains;
    dev->swapchains = sc;
    pthread_mutex_unlock(&dev->lock);

    AFMF_INFO("swapchain %p created: %ux%u, format %d, present mode %d (app asked %d), %u images "
              "(app asked %u), generation %s%s",
              (void *)sc->handle, sc->extent.width, sc->extent.height, (int)sc->format,
              (int)sc->present_mode, (int)info->presentMode, sc->image_count, sc->min_image_count,
              sc->gen_enabled ? "on" : "off", sc->async ? " (layer queue)" : "");
    return VK_SUCCESS;
}


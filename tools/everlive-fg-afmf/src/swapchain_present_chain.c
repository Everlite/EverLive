/* What the application's present chain carries, and its acquires and image queries, which
 * share the swapchain with the presentation thread. */

#include "swapchain_internal.h"

/* Copies what the application's chain carries that can outlive the call. False means something
 * the layer cannot carry (a present fence, display timing, regions of a kind it does not know):
 * the present then happens inline with the original chain. */
bool afmf_sc_job_from_chain(const struct afmf_swapchain *sc, const VkPresentInfoKHR *info,
                            struct afmf_present_job *job)
{
    for (const VkBaseInStructure *s = info->pNext; s != NULL; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR: {
            const VkPresentIdKHR *id = (const VkPresentIdKHR *)s;
            if (id->pPresentIds != NULL && id->pPresentIds[0] != 0) {
                job->have_present_id = true;
                job->present_id = id->pPresentIds[0];
            }
            break;
        }
#ifdef VK_KHR_present_id2
        case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR: {
            /* DXVK 2.7+ under a Mesa that offers present_id2; without carrying it every present
             * went inline, without pacing. */
            const VkPresentId2KHR *id = (const VkPresentId2KHR *)s;
            if (id->pPresentIds != NULL && id->pPresentIds[0] != 0) {
                job->have_present_id = true;
                job->present_id_v2 = true;
                job->present_id = id->pPresentIds[0];
            }
            break;
        }
#endif
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT: {
            const VkSwapchainPresentModeInfoEXT *m = (const VkSwapchainPresentModeInfoEXT *)s;
            if (m->swapchainCount < 1 || m->pPresentModes == NULL)
                break; /* nothing to carry */
            job->have_present_mode = true;
            job->present_mode = m->pPresentModes[0];
            /* The swapchain was moved to MAILBOX at creation: a switch back to FIFO would bring
             * the halved rate back, and a mode outside the cut-down list is invalid. */
            if (sc->fifo_to_mailbox) {
                bool allowed = false;
                for (uint32_t k = 0; k < sc->allowed_mode_count && !allowed; k++)
                    allowed = sc->allowed_modes[k] == job->present_mode;
                if (!allowed || job->present_mode == VK_PRESENT_MODE_FIFO_KHR ||
                    job->present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
                    job->present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT: {
            /* The application waits on it to reuse the present's resources: it goes with the
             * real frame, so the wait covers the layer's use of the image too. */
            const VkSwapchainPresentFenceInfoEXT *f = (const VkSwapchainPresentFenceInfoEXT *)s;
            if (f->pFences != NULL)
                job->present_fence = f->pFences[0];
            break;
        }
#ifdef VK_EXT_present_timing
        case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT: {
            /* The application's timing request (target time, stages to query) belongs to its
             * frame: it goes on the real present, whose past-presentation timing it will read
             * back. The generated frame carries none. */
            const VkPresentTimingsInfoEXT *t = (const VkPresentTimingsInfoEXT *)s;
            if (t->pTimingInfos != NULL && t->swapchainCount >= 1) {
                job->timing = t->pTimingInfos[0];
                job->timing.pNext = NULL;
                job->have_timing = true;
            }
            break;
        }
#endif
        case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR:
            break; /* a hint about what changed; the generated frame changes everything anyway */
        default: {
            static bool reported;
            if (!reported) {
                reported = true;
                AFMF_WARN("present chain carries sType %d, which the presentation thread cannot "
                          "carry: presenting inline, without pacing",
                          (int)s->sType);
            }
            return false;
        }
        }
    }
    return true;
}

/* The application's vkAcquireNextImage(2)KHR: under the swapchain lock in slices, so the
 * presentation thread never waits more than one slice and a long acquire never holds it off.
 * Everything but the timeout is the application's. */
VkResult afmf_swapchain_acquire(struct afmf_device *dev, const VkAcquireNextImageInfoKHR *info,
                                bool v2, uint32_t *index)
{
    pthread_mutex_lock(&dev->lock);
    struct afmf_swapchain *sc = find_locked(dev, info->swapchain);
    pthread_mutex_unlock(&dev->lock);

    VkAcquireNextImageInfoKHR sliced = *info;
    uint64_t left = info->timeout;
    for (;;) {
        bool ours = sc != NULL && sc->gen_enabled;
        sliced.timeout = ours && left > AFMF_ACQUIRE_SLICE_NS ? AFMF_ACQUIRE_SLICE_NS : left;
        if (ours)
            wsi_take(sc);
        VkResult res = v2 ? dev->fns.acquire_next_image2(dev->handle, &sliced, index)
                          : dev->fns.acquire_next_image(dev->handle, sliced.swapchain, sliced.timeout,
                                                        sliced.semaphore, sliced.fence, index);
        if (ours)
            wsi_give(sc);
        if (!ours || (res != VK_TIMEOUT && res != VK_NOT_READY))
            return res;
        if (left != UINT64_MAX)
            left -= sliced.timeout;
        if (left == 0)
            return res;
    }
}

VkResult afmf_swapchain_get_images(struct afmf_device *dev, VkSwapchainKHR swapchain,
                                   uint32_t *count, VkImage *images)
{
    pthread_mutex_lock(&dev->lock);
    struct afmf_swapchain *sc = find_locked(dev, swapchain);
    pthread_mutex_unlock(&dev->lock);

    bool ours = sc != NULL && sc->gen_enabled;
    if (ours)
        wsi_take(sc);
    VkResult res = dev->fns.get_swapchain_images(dev->handle, swapchain, count, images);
    if (ours)
        wsi_give(sc);
    return res;
}

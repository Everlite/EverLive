/* The swapchain's end: its report, its resources, and what the device's destruction leaves. */

#include "swapchain_internal.h"

static void report_and_free(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    AFMF_INFO("swapchain %p destroyed after %" PRIu64 " presents: %" PRIu64 " generated, %" PRIu64
              " skipped (%" PRIu64 " no free image, %" PRIu64 " no history, %" PRIu64
              " held back by the governor)",
              (void *)sc->handle, sc->present_count, sc->generated,
              sc->skipped_no_image + sc->skipped_no_history + sc->reduced_frames,
              sc->skipped_no_image, sc->skipped_no_history, sc->reduced_frames);
    afmf_sc_gen_teardown(dev, sc);
    pthread_cond_destroy(&sc->work_drain_cond);
    pthread_cond_destroy(&sc->work_cond);
    pthread_mutex_destroy(&sc->work_lock);
    pthread_cond_destroy(&sc->drain_cond);
    pthread_cond_destroy(&sc->job_cond);
    pthread_mutex_destroy(&sc->job_lock);
    pthread_mutex_destroy(&sc->wsi_lock);
    pthread_mutex_destroy(&sc->wsi_turn);
    free(sc);
}

/* Caller holds dev->lock. Returns the unlinked entry, or NULL when the handle was never tracked. */
static struct afmf_swapchain *unlink_locked(struct afmf_device *dev, VkSwapchainKHR handle)
{
    for (struct afmf_swapchain **link = &dev->swapchains; *link != NULL; link = &(*link)->next) {
        if ((*link)->handle == handle) {
            struct afmf_swapchain *sc = *link;
            *link = sc->next;
            return sc;
        }
    }
    return NULL;
}

void afmf_swapchain_destroy(struct afmf_device *dev, VkSwapchainKHR swapchain,
                            const VkAllocationCallbacks *alloc)
{
    if (swapchain != VK_NULL_HANDLE) {
        pthread_mutex_lock(&dev->lock);
        struct afmf_swapchain *sc = unlink_locked(dev, swapchain);
        pthread_mutex_unlock(&dev->lock);

        if (sc != NULL)
            report_and_free(dev, sc);
        else
            AFMF_WARN("destroying swapchain %p the layer never saw", (void *)swapchain);
    }
    dev->fns.destroy_swapchain(dev->handle, swapchain, alloc);
}

void afmf_swapchain_drain_all(struct afmf_device *dev)
{
    pthread_mutex_lock(&dev->lock);
    for (struct afmf_swapchain *sc = dev->swapchains; sc != NULL; sc = sc->next)
        afmf_sc_presenter_drain(sc); /* the thread never needs dev->lock to finish a job */
    pthread_mutex_unlock(&dev->lock);
}

void afmf_swapchain_forget_all(struct afmf_device *dev)
{
    pthread_mutex_lock(&dev->lock);
    struct afmf_swapchain *sc = dev->swapchains;
    dev->swapchains = NULL;
    pthread_mutex_unlock(&dev->lock);

    while (sc != NULL) {
        struct afmf_swapchain *next = sc->next;
        AFMF_WARN("swapchain %p still alive at device destruction", (void *)sc->handle);
        report_and_free(dev, sc);
        sc = next;
    }
}

/* The presentation thread: the generated frame at once, the real one after the pacing hold,
 * then the next spare. Under Proton each vkQueuePresentKHR measured 170-400 us of host time on
 * the application's thread; both presents run here instead. */

#include "swapchain_internal.h"

static void timespec_add_ns(struct timespec *t, uint64_t ns)
{
    t->tv_nsec += (long)(ns % 1000000000ull);
    t->tv_sec += (time_t)(ns / 1000000000ull) + t->tv_nsec / 1000000000L;
    t->tv_nsec %= 1000000000L;
}

/* One present from the layer's queue, with the parts of the application's chain that were kept.
 * The present id only goes on the real frame: ids must increase, and the companion has none. */
static VkResult present_one(struct afmf_device *dev, struct afmf_swapchain *sc, uint32_t image,
                            VkSemaphore wait, const struct afmf_present_job *job, bool real)
{
    VkPresentIdKHR id = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
        .pPresentIds = &job->present_id,
    };
#ifdef VK_KHR_present_id2
    VkPresentId2KHR id2 = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .swapchainCount = 1,
        .pPresentIds = &job->present_id,
    };
#endif
    VkSwapchainPresentModeInfoEXT mode = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .swapchainCount = 1,
        .pPresentModes = &job->present_mode,
    };
    VkSwapchainPresentFenceInfoEXT fence = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .swapchainCount = 1,
        .pFences = &job->present_fence,
    };
#ifdef VK_EXT_present_timing
    VkPresentTimingsInfoEXT timings = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
        .swapchainCount = 1,
        .pTimingInfos = &job->timing,
    };
#endif
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1u : 0u,
        .pWaitSemaphores = &wait,
        .swapchainCount = 1,
        .pSwapchains = &sc->handle,
        .pImageIndices = &image,
    };
    const void **tail = &present.pNext;
    if (job->have_present_mode) {
        *tail = &mode;
        tail = &mode.pNext;
    }
    if (real && job->have_present_id) {
#ifdef VK_KHR_present_id2
        if (job->present_id_v2) {
            *tail = &id2;
            tail = &id2.pNext;
        } else
#endif
        {
            *tail = &id;
            tail = &id.pNext;
        }
    }
    if (real && job->present_fence != VK_NULL_HANDLE) {
        *tail = &fence;
        tail = &fence.pNext;
    }
#ifdef VK_EXT_present_timing
    if (real && job->have_timing)
        *tail = &timings;
#endif

    struct timespec t0, t1;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    wsi_take(sc);
    pthread_mutex_lock(&dev->async_lock);
    VkResult res = dev->fns.queue_present(dev->async_queue, &present);
    pthread_mutex_unlock(&dev->async_lock);
    wsi_give(sc);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Thread-side counters live under job_lock (never dev->lock: vkDeviceWaitIdle drains the
     * thread while holding dev->lock). */
    pthread_mutex_lock(&sc->job_lock);
    if (real)
        sc->present_real_ms += elapsed_ms(&t0, &t1);
    else
        sc->present_ms += elapsed_ms(&t0, &t1);
    pthread_mutex_unlock(&sc->job_lock);
    return res;
}

void *afmf_sc_presenter_main(void *arg)
{
    struct afmf_swapchain *sc = arg;
    struct afmf_device *dev = sc->dev;

    pthread_mutex_lock(&sc->job_lock);
    for (;;) {
        while (sc->job_count == 0 && !sc->presenter_stop)
            pthread_cond_wait(&sc->job_cond, &sc->job_lock);
        if (sc->job_count == 0)
            break; /* stopping, and drained */
        struct afmf_present_job job = sc->jobs[sc->job_head];
        bool draining = sc->presenter_stop;

        VkResult first = VK_SUCCESS;
        if (job.generate && job.companion_count > 0) {
            /* Hold back companion frames according to pacing interval */
            if (job.hold_ns > 0 && !draining) {
                (void)dev->fns.wait_for_fences(dev->handle, 1, &job.done, VK_TRUE, 2 * job.hold_ns);
                struct timespec ready;
                (void)clock_gettime(CLOCK_MONOTONIC, &ready);
                double delay_ms = elapsed_ms(&job.arrival, &ready);
                if (delay_ms < 0.0)
                    delay_ms = 0.0;
                pthread_mutex_lock(&sc->job_lock);
                sc->gpu_delay_ms += delay_ms;
                sc->last_delay_ms = delay_ms;
                pthread_mutex_unlock(&sc->job_lock);
            }

            for (uint32_t c = 0; c < job.companion_count; c++) {
                uint32_t comp_img = job.companion_images[c];
                pthread_mutex_unlock(&sc->job_lock);
                first = present_one(dev, sc, comp_img, sc->sem_generated[comp_img],
                                    &job, false);

                if (job.hold_ns > 0 && !draining) {
                    struct timespec until;
                    (void)clock_gettime(CLOCK_MONOTONIC, &until);
                    timespec_add_ns(&until, job.hold_ns);
                    pthread_mutex_lock(&sc->job_lock);
                    sc->hold_ms += (double)job.hold_ns / 1e6;
                    while (!sc->presenter_stop && sc->job_count < 2 &&
                           pthread_cond_timedwait(&sc->job_cond, &sc->job_lock, &until) != ETIMEDOUT)
                        ;
                } else {
                    pthread_mutex_lock(&sc->job_lock);
                }
            }
        }
        pthread_mutex_unlock(&sc->job_lock);
        VkResult second = present_one(dev, sc, job.real_image,
                                      job.real_unwaited ? VK_NULL_HANDLE : sc->sem_real[job.real_image],
                                      &job, true);

        /* Line up the next companion's image while the application renders. */
        struct timespec r0, r1;
        (void)clock_gettime(CLOCK_MONOTONIC, &r0);
        wsi_take(sc);
        afmf_sc_spare_refill(dev, sc, 0);
        wsi_give(sc);
        (void)clock_gettime(CLOCK_MONOTONIC, &r1);

        pthread_mutex_lock(&sc->job_lock);
        sc->refill_ms += elapsed_ms(&r0, &r1);
        /* The worst outcome reaches the application with its next present call. Errors beat
         * SUBOPTIMAL, SUBOPTIMAL beats SUCCESS. */
        VkResult worst = first < 0 ? first : second < 0 ? second : first != VK_SUCCESS ? first : second;
        if (sc->deferred_result >= 0 && (worst < 0 || sc->deferred_result == VK_SUCCESS))
            sc->deferred_result = worst;
        sc->job_head = (sc->job_head + 1) % AFMF_MAX_JOBS;
        sc->job_count--;
        sc->in_flight--;
        /* Every waiter re-checks its own condition: the drain wants zero, the hook wants a
         * frame in flight fewer than the images. */
        pthread_cond_broadcast(&sc->drain_cond);
    }
    pthread_mutex_unlock(&sc->job_lock);
    return NULL;
}

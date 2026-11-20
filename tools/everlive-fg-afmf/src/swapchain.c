/* Frame generation at present time: the hook the application's vkQueuePresentKHR lands in. The
 * pieces live in the swapchain_*.c files next to it; this file is the present itself, threaded
 * (the hook only consumes the application's semaphores and queues the frame) or inline. */

#include "swapchain_internal.h"

/* The application's present, with the generated frame in front of it when one could be made. */
VkResult afmf_sc_present_generated(struct afmf_device *dev, struct afmf_swapchain *sc, VkQueue queue,
                                   uint32_t family, const VkPresentInfoKHR *info, uint64_t ordinal)
{
    const struct afmf_device_fns *f = &dev->fns;
    uint32_t i = info->pImageIndices[0];

    /* With a queue of its own the layer records for that family and presents from it; the
     * application's queue is never waited on. */
    VkQueue work_queue = sc->async ? dev->async_queue : queue;
    if (sc->async)
        family = dev->async_family;

    if (!afmf_sc_ensure_pool(dev, sc, family) || i >= sc->image_count ||
        info->waitSemaphoreCount > AFMF_MAX_APP_WAITS) {
        afmf_sc_gen_disable(sc, "cannot generate on this present path");
        return afmf_device_queue_present(dev, queue, info);
    }

    struct afmf_present_job job = {.real_image = i, .ordinal = ordinal};
    bool threaded = afmf_sc_presenter_start(sc) && afmf_sc_job_from_chain(sc, info, &job);
    if (!threaded)
        afmf_sc_presenter_drain(sc); /* a chain the threads cannot carry: inline, but in order */

    struct timespec t_start, t_present, t_companion, t_real, t_end;
    (void)clock_gettime(CLOCK_MONOTONIC, &t_start);
    job.arrival = t_start;
    uint32_t slot_index = sc->slot_index;
    sc->slot_index = (sc->slot_index + 1) % sc->image_count;

    if (threaded) {
        /* A slot comes round again after image_count frames, and the presentation thread waits
         * on its fence for the pacing: that frame has to be done before the work thread resets
         * it, so at most image_count frames are in flight between here and their presents. */
        pthread_mutex_lock(&sc->job_lock);
        while (sc->in_flight >= sc->image_count && !sc->presenter_stop)
            pthread_cond_wait(&sc->drain_cond, &sc->job_lock);
        sc->in_flight++;
        pthread_mutex_unlock(&sc->job_lock);

        /* The application's semaphores are consumed before the call returns, as it expects:
         * one submission with nothing to run turns them into the slot's semaphore, which the
         * work thread's submission waits on. */
        VkPipelineStageFlags stages[AFMF_MAX_APP_WAITS];
        for (uint32_t k = 0; k < info->waitSemaphoreCount; k++)
            stages[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo adapter = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = info->waitSemaphoreCount,
            .pWaitSemaphores = info->pWaitSemaphores,
            .pWaitDstStageMask = stages,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &sc->sem_app[slot_index],
        };
        pthread_mutex_lock(&dev->async_lock);
        VkResult res = f->queue_submit(work_queue, 1, &adapter, VK_NULL_HANDLE);
        pthread_mutex_unlock(&dev->async_lock);
        if (res != VK_SUCCESS) {
            /* Nothing consumed: the application's own present still works. */
            pthread_mutex_lock(&sc->job_lock);
            sc->in_flight--;
            pthread_cond_broadcast(&sc->drain_cond);
            pthread_mutex_unlock(&sc->job_lock);
            sc->slot_index = slot_index;
            afmf_sc_gen_disable(sc, "layer submission failed");
            return afmf_device_queue_present(dev, queue, info);
        }

        struct afmf_frame_job work = {.slot = slot_index, .present = job};
        pthread_mutex_lock(&sc->work_lock);
        while (sc->work_count == AFMF_MAX_JOBS)
            pthread_cond_wait(&sc->work_drain_cond, &sc->work_lock);
        sc->work[(sc->work_head + sc->work_count) % AFMF_MAX_JOBS] = work;
        sc->work_count++;
        pthread_cond_broadcast(&sc->work_cond);
        pthread_mutex_unlock(&sc->work_lock);

        /* The results of earlier presents come back here; the application also learns
         * OUT_OF_DATE from its acquire. */
        pthread_mutex_lock(&sc->job_lock);
        res = sc->deferred_result;
        sc->deferred_result = VK_SUCCESS;
        bool failed = sc->worker_failed;
        pthread_mutex_unlock(&sc->job_lock);
        if (info->pResults != NULL)
            info->pResults[0] = res;
        if (failed)
            afmf_sc_gen_disable(sc, "layer submission failed"); /* after this frame's presents */

        (void)clock_gettime(CLOCK_MONOTONIC, &t_end);
        sc->hook_ms += elapsed_ms(&t_start, &t_end);
        return res;
    }

    /* Inline: everything here, in the application's thread, under the swapchain lock (and the
     * layer's queue lock) from the submission to the last present. */
    VkResult res = afmf_sc_frame_generate(dev, sc, work_queue, slot_index, info->pWaitSemaphores,
                                          info->waitSemaphoreCount, false, &job);
    if (res != VK_SUCCESS) {
        /* The application's semaphores were not consumed, so its own present still works. */
        sc->slot_index = slot_index;
        afmf_sc_gen_disable(sc, "layer submission failed");
        return afmf_device_queue_present(dev, queue, info);
    }
    bool generate = job.generate;

    (void)clock_gettime(CLOCK_MONOTONIC, &t_present);
    if (generate && job.companion_count > 0) {
        for (uint32_t c = 0; c < job.companion_count; c++) {
            uint32_t j = job.companion_images[c];
            VkPresentInfoKHR companion = {
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &sc->sem_generated[j],
                .swapchainCount = 1,
                .pSwapchains = &sc->handle,
                .pImageIndices = &j,
            };
            VkResult presented = f->queue_present(work_queue, &companion);
            if (presented < 0)
                AFMF_DEBUG("swapchain %p: generated present %u returned %d", (void *)sc->handle,
                           c, (int)presented);
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_companion);

    /* The real frame keeps the application's pNext chain and pResults; only the wait moves to the
     * semaphore the layer signals once it has finished reading the image. */
    VkPresentInfoKHR real = *info;
    real.waitSemaphoreCount = 1;
    real.pWaitSemaphores = &sc->sem_real[i];
    /* The per-present mode gets the same rewrite as on the threaded path when it heads the
     * chain (the only place it can be replaced without copying what precedes it); deeper in a
     * chain the layer cannot carry, it goes out as the application wrote it. */
    VkSwapchainPresentModeInfoEXT mode_copy;
    const VkBaseInStructure *head = info->pNext;
    if (sc->fifo_to_mailbox && head != NULL &&
        head->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
        const VkSwapchainPresentModeInfoEXT *m = (const VkSwapchainPresentModeInfoEXT *)head;
        if (m->swapchainCount >= 1 && m->pPresentModes != NULL) {
            static const VkPresentModeKHR mailbox = VK_PRESENT_MODE_MAILBOX_KHR;
            bool allowed = false;
            for (uint32_t k = 0; k < sc->allowed_mode_count && !allowed; k++)
                allowed = sc->allowed_modes[k] == m->pPresentModes[0];
            if (!allowed || m->pPresentModes[0] == VK_PRESENT_MODE_FIFO_KHR ||
                m->pPresentModes[0] == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
                mode_copy = *m;
                mode_copy.swapchainCount = 1;
                mode_copy.pPresentModes = &mailbox;
                real.pNext = &mode_copy;
            }
        }
    }
    res = f->queue_present(work_queue, &real);
    if (sc->async)
        pthread_mutex_unlock(&dev->async_lock);
    (void)clock_gettime(CLOCK_MONOTONIC, &t_real);

    /* Try to line up the next companion's image now: by the next present, a frame later, the
     * presentation engine has had time to release one. */
    afmf_sc_spare_refill(dev, sc, 0);
    wsi_give(sc);

    (void)clock_gettime(CLOCK_MONOTONIC, &t_end);
    sc->hook_ms += elapsed_ms(&t_start, &t_end);
    pthread_mutex_lock(&sc->job_lock);
    sc->present_ms += elapsed_ms(&t_present, &t_companion);
    sc->present_real_ms += elapsed_ms(&t_companion, &t_real);
    sc->refill_ms += elapsed_ms(&t_real, &t_end);
    pthread_mutex_unlock(&sc->job_lock);
    return res;
}

VkResult afmf_swapchain_present(struct afmf_device *dev, VkQueue queue, const VkPresentInfoKHR *info)
{
    pthread_mutex_lock(&dev->lock);
    struct afmf_swapchain *sc = info->swapchainCount == 1 ? find_locked(dev, info->pSwapchains[0])
                                                          : NULL;
    pthread_mutex_unlock(&dev->lock);
    if (sc == NULL) {
        for (uint32_t k = 0; k < info->swapchainCount; k++) {
            pthread_mutex_lock(&dev->lock);
            struct afmf_swapchain *each = find_locked(dev, info->pSwapchains[k]);
            pthread_mutex_unlock(&dev->lock);
            if (each != NULL)
                afmf_sc_update_cadence(dev, each);
        }
        return afmf_device_queue_present(dev, queue, info);
    }

    uint64_t ordinal = afmf_sc_update_cadence(dev, sc);
    uint32_t family;
    if (!sc->gen_enabled || !afmf_device_queue_family(dev, queue, &family))
        return afmf_device_queue_present(dev, queue, info);
    return afmf_sc_present_generated(dev, sc, queue, family, info, ordinal);
}

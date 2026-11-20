/* The work thread: the slot's fence, the governor, the spare, recording and the submission run
 * here, so the hook only consumes the application's semaphores and queues. */

#include "swapchain_internal.h"

/* The frame's work behind the hook: the slot's fence, the governor, the spare, recording and
 * the submission, which waits on `waits` (the application's semaphores inline; from the work
 * thread the slot's adapter semaphore, signalled once they are). Fills the job's companion and
 * pacing; the slot is pending on success. Inline (`threaded` false) the swapchain lock and the
 * layer's queue lock stay held on success for the presents that follow, in the thread's order. */
VkResult afmf_sc_frame_generate(struct afmf_device *dev, struct afmf_swapchain *sc, VkQueue queue,
                                uint32_t slot_index, const VkSemaphore *waits, uint32_t wait_count,
                                bool threaded, struct afmf_present_job *job)
{
    const struct afmf_device_fns *f = &dev->fns;
    struct afmf_slot *slot = &sc->slots[slot_index];
    uint32_t i = job->real_image;
    struct timespec t_start, t_fence, t_acquire, t_record, t_submit;
    (void)clock_gettime(CLOCK_MONOTONIC, &t_start);

    if (slot->pending) {
        (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        (void)f->reset_fences(dev->handle, 1, &slot->fence);
        slot->pending = false;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_fence);

    /* The images the generated frames go into: up to (mult - 1) spares */
    bool generate = false;
    uint32_t companion_images[4];
    uint32_t companion_count = 0;
    pthread_mutex_lock(&sc->job_lock);
    bool allowed = afmf_sc_governor_allows(sc, sc->last_delay_ms, job->ordinal);
    bool have_history = sc->have_history;
    if (!have_history)
        sc->skipped_no_history++;
    pthread_mutex_unlock(&sc->job_lock);
    if (have_history && allowed) {
        uint32_t mult = afmf_config_get()->multiplier;
        uint32_t needed = mult > 1 ? mult - 1 : 1;
        if (needed > 3) needed = 3;
        for (uint32_t k = 0; k < needed; k++) {
            uint32_t img = 0;
            if (afmf_sc_spare_take(dev, sc, &img)) {
                companion_images[companion_count++] = img;
            } else {
                break;
            }
        }
        generate = companion_count > 0;
        if (!generate) {
            pthread_mutex_lock(&sc->job_lock);
            sc->skipped_no_image++;
            pthread_mutex_unlock(&sc->job_lock);
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_acquire);

    VkResult res = afmf_sc_record_frame(dev, sc, slot->cmd, slot_index, i,
                                        companion_images, companion_count);
    (void)clock_gettime(CLOCK_MONOTONIC, &t_record);
    if (res == VK_SUCCESS) {
        VkPipelineStageFlags stages[AFMF_MAX_APP_WAITS];
        for (uint32_t k = 0; k < wait_count; k++)
            stages[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSemaphore signals[5];
        uint32_t sig_count = 0;
        signals[sig_count++] = sc->sem_real[i];
        for (uint32_t k = 0; k < companion_count; k++) {
            signals[sig_count++] = sc->sem_generated[companion_images[k]];
        }
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = wait_count,
            .pWaitSemaphores = waits,
            .pWaitDstStageMask = stages,
            .commandBufferCount = 1,
            .pCommandBuffers = &slot->cmd,
            .signalSemaphoreCount = sig_count,
            .pSignalSemaphores = signals,
        };
        if (!threaded)
            wsi_take(sc);
        if (sc->async)
            pthread_mutex_lock(&dev->async_lock);
        res = f->queue_submit(queue, 1, &submit, slot->fence);
        if (sc->async && (res != VK_SUCCESS || threaded))
            pthread_mutex_unlock(&dev->async_lock);
        if (!threaded && res != VK_SUCCESS)
            wsi_give(sc);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_submit);

    pthread_mutex_lock(&sc->job_lock);
    sc->fence_ms += elapsed_ms(&t_start, &t_fence);
    sc->acquire_ms += elapsed_ms(&t_fence, &t_acquire);
    sc->record_ms += elapsed_ms(&t_acquire, &t_record);
    sc->submit_ms += elapsed_ms(&t_record, &t_submit);
    if (res == VK_SUCCESS) {
        sc->have_history = true;
        if (generate)
            sc->generated += companion_count;
    }
    pthread_mutex_unlock(&sc->job_lock);
    if (res != VK_SUCCESS)
        return res; /* the images acquired for the generated frame, if any, stay with the engine */

    slot->pending = true;
    job->generate = generate;
    job->companion_count = companion_count;
    for (uint32_t k = 0; k < companion_count; k++) {
        job->companion_images[k] = companion_images[k];
    }
    job->companion_image = companion_count > 0 ? companion_images[0] : 0;
    job->done = slot->fence;

    /* Debug dumps block on the submission; only while AFMF_DUMP_DIR asks for frames. */
    if (sc->fg != NULL)
        afmf_framegen_dump_arm(sc->fg);
    if (generate && sc->fg != NULL && afmf_framegen_dump_recorded(sc->fg)) {
        (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        afmf_framegen_dump_write(dev, sc->fg);
        (void)clock_gettime(CLOCK_MONOTONIC, &job->arrival); /* the pacing and the governor start after the stall */
    }

    if (generate && threaded && afmf_config_get()->pacing) {
        pthread_mutex_lock(&sc->job_lock);
        uint32_t mult = afmf_config_get()->multiplier;
        if (mult < 2) mult = 2;
        double step_ms = sc->frame_ms_ema / (double)mult;
        pthread_mutex_unlock(&sc->job_lock);
        uint64_t hold = (uint64_t)(step_ms * 1e6);
        uint64_t min_ns = AFMF_PACING_MIN_NS / 2;
        job->hold_ns = hold < min_ns ? min_ns
                       : hold > AFMF_PACING_MAX_NS ? AFMF_PACING_MAX_NS
                                                   : hold;
    }
    return VK_SUCCESS;
}

void *afmf_sc_worker_main(void *arg)
{
    struct afmf_swapchain *sc = arg;
    struct afmf_device *dev = sc->dev;

    pthread_mutex_lock(&sc->work_lock);
    for (;;) {
        while (sc->work_count == 0 && !sc->worker_stop)
            pthread_cond_wait(&sc->work_cond, &sc->work_lock);
        if (sc->work_count == 0)
            break; /* stopping, and drained */
        struct afmf_frame_job work = sc->work[sc->work_head];
        sc->worker_busy = true;
        pthread_mutex_unlock(&sc->work_lock);

        VkResult res = afmf_sc_frame_generate(dev, sc, dev->async_queue, work.slot,
                                              &sc->sem_app[work.slot], 1, true, &work.present);
        if (res != VK_SUCCESS) {
            /* The hook already consumed the application's semaphores into the slot's: the real
             * frame's present still needs sem_real, or it waits forever. One submission with
             * nothing to run passes the signal on; if that fails too, the present goes out
             * without its wait rather than never. Generation stops at the hook's next call. */
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo pass = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &sc->sem_app[work.slot],
                .pWaitDstStageMask = &stage,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &sc->sem_real[work.present.real_image],
            };
            pthread_mutex_lock(&dev->async_lock);
            VkResult passed = dev->fns.queue_submit(dev->async_queue, 1, &pass, VK_NULL_HANDLE);
            pthread_mutex_unlock(&dev->async_lock);
            work.present.generate = false;
            work.present.real_unwaited = passed != VK_SUCCESS;
            pthread_mutex_lock(&sc->job_lock);
            if (sc->deferred_result >= 0)
                sc->deferred_result = res;
            sc->worker_failed = true;
            pthread_mutex_unlock(&sc->job_lock);
        }

        /* Hand both presents to the presentation thread. */
        pthread_mutex_lock(&sc->job_lock);
        while (sc->job_count == AFMF_MAX_JOBS)
            pthread_cond_wait(&sc->drain_cond, &sc->job_lock);
        sc->jobs[(sc->job_head + sc->job_count) % AFMF_MAX_JOBS] = work.present;
        sc->job_count++;
        pthread_cond_broadcast(&sc->job_cond);
        pthread_mutex_unlock(&sc->job_lock);

        pthread_mutex_lock(&sc->work_lock);
        sc->work_head = (sc->work_head + 1) % AFMF_MAX_JOBS;
        sc->work_count--;
        sc->worker_busy = false;
        pthread_cond_broadcast(&sc->work_drain_cond);
    }
    pthread_mutex_unlock(&sc->work_lock);
    return NULL;
}

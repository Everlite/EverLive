/* The cadence of the application's presents, the governor that reads it, and the spare image
 * the next companion goes into. */

#include "swapchain_internal.h"

/* Returns this present's ordinal, which the work thread's governor paces on. */
uint64_t afmf_sc_update_cadence(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    struct timespec now;
    bool have_now = clock_gettime(CLOCK_MONOTONIC, &now) == 0;

    pthread_mutex_lock(&dev->lock);
    /* Cadence is measured between consecutive present calls on the CPU side: enough to size the
     * pacing work, not a measurement of when the frame reached the display. */
    if (have_now) {
        if (sc->present_count > 0) {
            double dt = elapsed_ms(&sc->last_present, &now);
            sc->frame_time_ms_accum += dt;
            sc->frame_time_samples++;
            /* Smoothed for pacing and the governor: quick enough to follow a scene change,
             * steady enough not to jitter the hold with every frame. Ignores loading pauses,
             * and a single hitch moves it by at most a factor of two: a 100 ms frame in a
             * 6 ms cadence must not turn the next holds into 20 ms ones and trip the fps floor. */
            if (dt < 250.0) {
                pthread_mutex_lock(&sc->job_lock); /* the work thread reads it for the governor and the pacing */
                if (sc->frame_ms_ema > 0.0 && dt > 2.0 * sc->frame_ms_ema)
                    dt = 2.0 * sc->frame_ms_ema;
                sc->frame_ms_ema = sc->frame_ms_ema == 0.0 ? dt : 0.9 * sc->frame_ms_ema + 0.1 * dt;
                pthread_mutex_unlock(&sc->job_lock);
            }
        }
        sc->last_present = now;
    }
    sc->present_count++;
    if (sc->frame_time_samples == AFMF_STATS_INTERVAL) {
        double n = (double)sc->frame_time_samples;
        double frame_ms = sc->frame_time_ms_accum / n;
        /* Where the application's thread waits (the hook: with AFMF_PROFILE this is the number
         * that says whether the layer costs the game host time), then the layer's two threads. */
#define STATS_LINE                                                                                \
    "swapchain %p: %" PRIu64 " presents, %" PRIu64 " generated, %" PRIu64 " no free image; "    \
    "%.2f ms between presents (%.0f real fps); in the hook %.0f us per present; work thread: "  \
    "slot fence %.0f, acquire %.0f, record %.0f, submit %.0f; presentation thread: present "     \
    "generated %.0f, present real %.0f, refill %.0f, gpu done +%.2f ms, pacing hold %.2f ms; "  \
    "governor step %u"
#define STATS_ARGS                                                                                \
    (void *)sc->handle, sc->present_count, sc->generated, sc->skipped_no_image, frame_ms,       \
        1e3 / frame_ms, 1e3 * sc->hook_ms / n, 1e3 * sc->fence_ms / n, 1e3 * sc->acquire_ms / n, \
        1e3 * sc->record_ms / n, 1e3 * sc->submit_ms / n, 1e3 * sc->present_ms / n,             \
        1e3 * sc->present_real_ms / n, 1e3 * sc->refill_ms / n, sc->gpu_delay_ms / n,           \
        sc->hold_ms / n, sc->governor_step
        pthread_mutex_lock(&sc->job_lock); /* the threads' counters */
        if (afmf_config_get()->profile)
            AFMF_INFO(STATS_LINE, STATS_ARGS);
        else
            AFMF_DEBUG(STATS_LINE, STATS_ARGS);
#undef STATS_LINE
#undef STATS_ARGS
        sc->present_ms = sc->present_real_ms = sc->refill_ms = sc->gpu_delay_ms = sc->hold_ms = 0.0;
        sc->fence_ms = sc->acquire_ms = sc->record_ms = sc->submit_ms = 0.0;
        pthread_mutex_unlock(&sc->job_lock);
        sc->frame_time_ms_accum = 0.0;
        sc->frame_time_samples = 0;
        sc->hook_ms = 0.0;
    }
    uint64_t ordinal = sc->present_count;
    pthread_mutex_unlock(&dev->lock);
    return ordinal;
}

/* Whether this present gets a companion, after the governor has looked at how late the last
 * generated frame was ready and at the real frame rate. Called with job_lock held. */
bool afmf_sc_governor_allows(struct afmf_swapchain *sc, double last_delay_ms, uint64_t ordinal)
{
    const struct afmf_config *cfg = afmf_config_get();
    double frame_ms = sc->frame_ms_ema;
    if (cfg->min_fps > 0 && frame_ms > 0.0) {
        /* Two thresholds, or a game sitting on the floor turns generation on and off with every
         * frame: doubling lightens the GPU, the rate rises, the floor clears, the companions
         * come back and it drops again. Generation returns once the rate is a tenth above the
         * floor, and the governor's streaks restart: what they counted is from before it. */
        double floor_ms = 1e3 / cfg->min_fps;
        if (!sc->under_floor && frame_ms > floor_ms)
            sc->under_floor = true;
        else if (sc->under_floor && frame_ms < 0.9 * floor_ms) {
            sc->under_floor = false;
            sc->late_streak = sc->early_streak = 0;
        }
        if (sc->under_floor) {
            sc->reduced_frames++;
            return false;
        }
    }
    if (!cfg->governor || frame_ms <= 0.0)
        return true;

    double ratio = last_delay_ms / frame_ms;
    uint32_t before = sc->governor_step;
    if (ratio > AFMF_GOVERNOR_LATE) {
        sc->early_streak = 0;
        if (++sc->late_streak >= AFMF_GOVERNOR_LATE_FRAMES && sc->governor_step < AFMF_GOVERNOR_STEPS) {
            sc->governor_step++;
            sc->late_streak = 0;
        }
    } else if (ratio < AFMF_GOVERNOR_EARLY) {
        sc->late_streak = 0;
        if (++sc->early_streak >= AFMF_GOVERNOR_EARLY_FRAMES && sc->governor_step > 0) {
            sc->governor_step--;
            sc->early_streak = 0;
        }
    }
    if (sc->governor_step != before) {
        if (sc->fg != NULL)
            afmf_framegen_set_levels(sc->fg, sc->governor_step >= 1 ? 5u
                                                                    : afmf_framegen_max_levels(sc->fg));
        AFMF_INFO("swapchain %p: governor step %u (generated frame ready %.0f%% into the frame): %s",
                  (void *)sc->handle, sc->governor_step, 100.0 * ratio,
                  sc->governor_step == 0   ? "every frame, full search"
                  : sc->governor_step == 1 ? "every frame, five search levels"
                  : sc->governor_step == 2 ? "one companion in two"
                                           : "one companion in three");
    }
    /* Step 2 and 3 thin the companions out; the reduced frames still enter the history. The
     * frame's own ordinal decides, not the present counter, which the application's thread has
     * moved on by the frames still queued. */
    uint64_t every = sc->governor_step >= 2 ? sc->governor_step : 1;
    if (ordinal % every != 0) {
        sc->reduced_frames++;
        return false;
    }
    return true;
}

/* Acquires the spare when there is none, waiting at most `timeout` (0 from the presentation
 * thread: it must not hold the swapchain lock). Caller holds the lock. */
void afmf_sc_spare_refill(struct afmf_device *dev, struct afmf_swapchain *sc, uint64_t timeout)
{
    if (sc->spare_valid)
        return;
    VkResult res = dev->fns.acquire_next_image(dev->handle, sc->handle, timeout, VK_NULL_HANDLE,
                                               sc->spare_fence, &sc->spare_image);
    sc->spare_valid = res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR;
}

/* Hands out the spare for this frame's companion once the presentation engine has released it;
 * false leaves it for the next frame (or means there was none to be had). Only the acquire and
 * the spare's state need the swapchain lock: the fence wait runs without it, so the presentation
 * thread keeps presenting meanwhile. The thread only refills once `spare_valid` drops, so the
 * fence is reset before that, and never raced. */
bool afmf_sc_spare_take(struct afmf_device *dev, struct afmf_swapchain *sc, uint32_t *image)
{
    /* AFMF_ACQUIRE_TIMEOUT_US bounds the whole wait for a released image: the acquire, in
     * slices so the presentation thread keeps its turn (its presents are what free images),
     * then the release fence. */
    uint64_t left = afmf_config_get()->acquire_timeout_ns;
    bool valid;
    uint32_t spare;
    for (;;) {
        uint64_t slice = left > AFMF_ACQUIRE_SLICE_NS ? AFMF_ACQUIRE_SLICE_NS : left;
        wsi_take(sc);
        afmf_sc_spare_refill(dev, sc, slice);
        valid = sc->spare_valid;
        spare = sc->spare_image;
        wsi_give(sc);
        left -= slice;
        if (valid || left == 0)
            break;
    }
    if (!valid)
        return false;
    /* The release fence gets at least a slice even when the acquire spent the whole budget:
     * the image is there, and a companion is worth a millisecond of the work thread. */
    uint64_t fence_wait = left > AFMF_ACQUIRE_SLICE_NS ? left : AFMF_ACQUIRE_SLICE_NS;
    if (dev->fns.wait_for_fences(dev->handle, 1, &sc->spare_fence, VK_TRUE, fence_wait) != VK_SUCCESS)
        return false;
    (void)dev->fns.reset_fences(dev->handle, 1, &sc->spare_fence);
    wsi_take(sc);
    sc->spare_valid = false;
    wsi_give(sc);
    *image = spare;
    return true;
}

#pragma once

/* The swapchain's generation state, shared between its translation units; nothing here is
 * visible outside src/swapchain_*.c. The small helpers every path takes are inline. */

#include "swapchain.h"

#include "config.h"
#include "framegen.h"
#include "log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Presents between two cadence reports at debug level. */
#define AFMF_STATS_INTERVAL 300u
/* Queue families a CONCURRENT swapchain can be shared between (the application's plus ours). */
#define AFMF_MAX_FAMILIES 8u
/* Wait semaphores an application may attach to one present before the layer gives up on it. */
#define AFMF_MAX_APP_WAITS 8u
/* Presents queued for the presentation thread; bounded by the images in flight in practice. */
#define AFMF_MAX_JOBS 16u
/* The application's acquire is run in slices of this length under the swapchain lock, so the
 * presentation thread is never held off for longer than one slice. */
#define AFMF_ACQUIRE_SLICE_NS 1000000ull
/* Pacing delay bounds for the real frame: half the frame time, clamped. */
#define AFMF_PACING_MIN_NS 500000ull
#define AFMF_PACING_MAX_NS 40000000ull /* half a frame down to 12.5 fps; AFMF_MIN_FPS=0 allows it */
/* Governor: the generated frame ready later than this fraction of the frame time, this many
 * frames in a row, steps generation down; ready before the lower fraction for this many frames
 * steps it back up. Steps: 0 everything, 1 five search levels, 2 one companion in two, 3 in three.
 * Late means a frame and a half: the fence includes the game's own queue depth, a GPU at 100 %
 * is the normal state of a game, and a companion ready within the frame still lands before the
 * next real one. Half a second of lateness steps down; a quarter of a second within three
 * quarters of a frame steps back up, so a step down costs fps for as short a time as it can. */
#define AFMF_GOVERNOR_LATE 1.5
#define AFMF_GOVERNOR_EARLY 0.75
#define AFMF_GOVERNOR_LATE_FRAMES 30u
#define AFMF_GOVERNOR_EARLY_FRAMES 15u
#define AFMF_GOVERNOR_STEPS 3u
/* Bytes and present modes a copied create chain may need. */
#define AFMF_CHAIN_BYTES 1024u
#define AFMF_MAX_PRESENT_MODES 8u

/* One frame handed to the presentation thread: the generated image first, the real one after
 * the pacing delay, each with what the application's pNext chain carried that survives being
 * copied (present id, per-present mode). */
struct afmf_present_job {
    uint32_t real_image;
    uint32_t companion_image;
    uint32_t companion_count;
    uint32_t companion_images[4];
    bool generate;
    uint64_t ordinal;         /* this present's number, for the governor's one-in-n cadence */
    struct timespec arrival;
    uint64_t hold_ns;         /* hold interval between frames */
    VkFence done;             /* the layer's submission for this frame: signalled once the generated frame is ready */
    bool have_present_id;
    uint64_t present_id;
    bool present_id_v2;   /* VK_KHR_present_id2 carried the id (same shape, its own sType) */
    bool have_present_mode;
    VkPresentModeKHR present_mode;
    VkFence present_fence; /* VK_EXT_swapchain_maintenance1: signalled with the real frame */
    bool real_unwaited;    /* the layer's submission failed twice: present the real frame without its semaphore */
#ifdef VK_EXT_present_timing
    /* Newer headers only (distribution packages build against older ones): without the
     * extension in the header, a timing request makes that present inline, like any unknown
     * structure. */
    bool have_timing;      /* VK_EXT_present_timing: the application's timing request, real frame */
    VkPresentTimingInfoEXT timing;
#endif
};

/* One frame handed by the hook to the work thread: the slot it took, and the present job the
 * thread completes (companion, pacing) and hands on to the presentation thread. */
struct afmf_frame_job {
    uint32_t slot;
    struct afmf_present_job present;
};

/* One in-flight layer submission: its command buffer and the fence that says the slot can be
 * reused. */
struct afmf_slot {
    VkCommandBuffer cmd;
    VkFence fence;
    bool pending;
};

struct afmf_swapchain {
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR present_mode; /* the swapchain's, after the layer's rewrite */
    bool fifo_to_mailbox;          /* the application asked for FIFO; per-present modes are rewritten too */
    /* The per-present modes the swapchain allows (VK_EXT_swapchain_maintenance1) after the
     * rewrite: the application's list cut down to what is compatible with MAILBOX. Empty when
     * the application gave no list. */
    VkPresentModeKHR allowed_modes[8];
    uint32_t allowed_mode_count;
    bool storage_usage; /* the images carry STORAGE usage for direct output (AFMF_DIRECT_OUTPUT) */
    bool sampled_usage; /* the images carry SAMPLED usage for the direct ingest (AFMF_DIRECT_INGEST) */
    uint32_t min_image_count;

    /* Frame generation state; everything below `gen_enabled` is unused when it is false. */
    bool gen_enabled;
    bool async;                 /* work and presents go to dev->async_queue */
    uint32_t image_count;
    VkImage *images;
    VkSemaphore *sem_generated; /* per image: signals "the generated frame in image k is ready" */
    VkSemaphore *sem_real;      /* per image: signals "the layer is done reading image k" */
    /* Per slot: the application's wait semaphores, consumed by the hook's empty submission
     * before the present call returns (a binary semaphore must be waited before it is signalled
     * again, and the application signals its own on its next frame), and turned into this one,
     * which the work thread's submission waits on. */
    VkSemaphore *sem_app;
    struct afmf_slot *slots;    /* one per image, used round-robin */
    uint32_t slot_index;
    VkCommandPool pool;
    uint32_t pool_family;
    struct afmf_framegen *fg;   /* optical flow + interpolation; NULL means repeat frames */
    VkImage history;            /* the previous real frame, only used when fg is NULL */
    VkDeviceMemory history_memory;
    VkImageLayout history_layout;
    bool have_history;
    /* The image the next companion goes into. Acquired without waiting, ideally at the end of the
     * previous present, so the application's thread never waits for the presentation engine: on a
     * FIFO desktop a free image comes back one refresh period after a present, and waiting for it
     * in the hook cost the application that whole period (measured: 5.1-5.9 ms at 165 Hz). */
    bool spare_valid;
    uint32_t spare_image;
    VkFence spare_fence; /* signalled once the presentation engine has released spare_image */

    uint64_t present_count;
    uint64_t generated;
    uint64_t skipped_no_image;
    uint64_t skipped_no_history;
    struct timespec last_present;
    double frame_time_ms_accum;
    uint32_t frame_time_samples;
    /* Host time the application's present thread spends inside the layer, per interval: the
     * whole hook and the three places it can block (slot fence, companion acquire, presents). */
    double hook_ms, fence_ms, acquire_ms, record_ms, submit_ms;
    double present_ms, present_real_ms, refill_ms, hold_ms;
    double gpu_delay_ms; /* generated frame ready this long after the present call (GPU contention) */
    double last_delay_ms; /* the latest such delay, what the governor reads */
    /* Governor (hook side, under dev->lock with the cadence): current step and the streaks that
     * move it. */
    uint32_t governor_step;
    uint32_t late_streak, early_streak;
    bool under_floor; /* the real frame rate is below AFMF_MIN_FPS: no companions until it clears it by a margin */
    uint64_t reduced_frames; /* presents that went out without a companion because of the step */
    double frame_ms_ema; /* smoothed time between the application's presents, for pacing */

    /* Presentation thread. Under Proton each vkQueuePresentKHR measured 170-400 us of host time
     * on the application's thread; both presents (and the spare refill) run here instead, and
     * the real frame is held back half a frame so the generated one lands in between. Only used
     * when the layer presents from its own queue (`async`): from the application's queue its
     * thread would race the application's submits. */
    struct afmf_device *dev;
    pthread_t presenter;
    bool presenter_running;
    bool presenter_stop;
    pthread_mutex_t job_lock;
    pthread_cond_t job_cond;   /* new job, or stop */
    pthread_cond_t drain_cond; /* the queue emptied */
    struct afmf_present_job jobs[AFMF_MAX_JOBS];
    uint32_t job_head, job_count;
    VkResult deferred_result; /* worst result of presents done so far, returned by the next present call */
    uint32_t in_flight;       /* frames between the hook and the end of their presents; at most image_count */

    /* Work thread: the slot's fence, the governor, the spare, recording and the submission run
     * here, so the hook only consumes the application's semaphores and queues. Its state is
     * under work_lock; what it shares with the hook and the presentation thread (counters,
     * the governor, the cadence) is under job_lock. It never takes dev->lock: vkDeviceWaitIdle
     * drains it while holding that. */
    pthread_t worker;
    bool worker_running;
    bool worker_stop;
    bool worker_busy;         /* between taking a frame off the queue and handing its presents on */
    bool worker_failed;       /* a submission failed: the hook disables generation on its next call */
    pthread_mutex_t work_lock;
    pthread_cond_t work_cond;       /* new frame, or stop */
    pthread_cond_t work_drain_cond; /* a frame finished */
    struct afmf_frame_job work[AFMF_MAX_JOBS];
    uint32_t work_head, work_count;
    /* vkAcquireNextImageKHR and vkQueuePresentKHR both need external synchronisation on the
     * swapchain: the application's acquires, the layer's spare acquires and the presentation
     * thread's presents all take this. */
    pthread_mutex_t wsi_lock;
    /* Taken before wsi_lock by everyone (wsi_take/wsi_give): a glibc mutex is not fair, and the
     * application's acquire loop re-took wsi_lock the instant it released it, so the
     * presentation thread never got a turn, presented nothing, and no image ever came free. */
    pthread_mutex_t wsi_turn;

    struct afmf_swapchain *next;
};

static inline void wsi_take(struct afmf_swapchain *sc)
{
    pthread_mutex_lock(&sc->wsi_turn);
    pthread_mutex_lock(&sc->wsi_lock);
    pthread_mutex_unlock(&sc->wsi_turn);
}

static inline void wsi_give(struct afmf_swapchain *sc)
{
    pthread_mutex_unlock(&sc->wsi_lock);
}

static inline double elapsed_ms(const struct timespec *from, const struct timespec *to)
{
    return (double)(to->tv_sec - from->tv_sec) * 1e3 + (double)(to->tv_nsec - from->tv_nsec) / 1e6;
}

/* Caller holds dev->lock. */
static inline struct afmf_swapchain *find_locked(const struct afmf_device *dev, VkSwapchainKHR handle)
{
    for (struct afmf_swapchain *sc = dev->swapchains; sc != NULL; sc = sc->next)
        if (sc->handle == handle)
            return sc;
    return NULL;
}

#include "swapchain_parts.h"

/* The two threads' lifetime: started on first use, drained and joined at teardown, the work
 * thread first since it feeds the other. */

#include "swapchain_internal.h"

/* Waits until every queued frame has been submitted and handed on. */
static void worker_drain(struct afmf_swapchain *sc)
{
    if (!sc->worker_running)
        return;
    pthread_mutex_lock(&sc->work_lock);
    while (sc->work_count > 0 || sc->worker_busy)
        pthread_cond_wait(&sc->work_drain_cond, &sc->work_lock);
    pthread_mutex_unlock(&sc->work_lock);
}

/* Drains and joins; safe to call more than once and without a running thread. */
static void worker_stop(struct afmf_swapchain *sc)
{
    if (!sc->worker_running)
        return;
    pthread_mutex_lock(&sc->work_lock);
    sc->worker_stop = true;
    pthread_cond_broadcast(&sc->work_cond);
    pthread_mutex_unlock(&sc->work_lock);
    (void)pthread_join(sc->worker, NULL);
    sc->worker_running = false;
}

/* Starts both threads on first use; false leaves everything on the application's thread. */
bool afmf_sc_presenter_start(struct afmf_swapchain *sc)
{
    if (sc->presenter_running && sc->worker_running)
        return true;
    if (!sc->async || sc->presenter_stop || sc->worker_stop)
        return false;
    if (!sc->presenter_running) {
        if (pthread_create(&sc->presenter, NULL, afmf_sc_presenter_main, sc) != 0) {
            AFMF_WARN("swapchain %p: no presentation thread; presenting inline", (void *)sc->handle);
            sc->presenter_stop = true; /* do not retry every frame */
            return false;
        }
        sc->presenter_running = true;
    }
    if (pthread_create(&sc->worker, NULL, afmf_sc_worker_main, sc) != 0) {
        AFMF_WARN("swapchain %p: no work thread; presenting inline", (void *)sc->handle);
        sc->worker_stop = true;
        return false;
    }
    sc->worker_running = true;
    return true;
}

/* Waits until every queued frame was submitted and every queued present went out: needed
 * before presenting inline behind them. */
void afmf_sc_presenter_drain(struct afmf_swapchain *sc)
{
    worker_drain(sc);
    if (!sc->presenter_running)
        return;
    pthread_mutex_lock(&sc->job_lock);
    while (sc->job_count > 0)
        pthread_cond_wait(&sc->drain_cond, &sc->job_lock);
    pthread_mutex_unlock(&sc->job_lock);
}

/* Drains and joins both threads, the work thread first (it feeds the other); safe to call more
 * than once and without running threads. */
void afmf_sc_presenter_stop(struct afmf_swapchain *sc)
{
    worker_stop(sc);
    if (!sc->presenter_running)
        return;
    pthread_mutex_lock(&sc->job_lock);
    sc->presenter_stop = true;
    pthread_cond_broadcast(&sc->job_cond);
    pthread_mutex_unlock(&sc->job_lock);
    (void)pthread_join(sc->presenter, NULL);
    sc->presenter_running = false;
}

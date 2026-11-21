/* Frame generation: GPU timestamps per stage (AFMF_PROFILE=1). One query range per slot; a
 * slot's results are read back when the slot is reused, i.e. after its fence, so it never
 * blocks. */

#include "framegen_internal.h"

void afmf_framegen_profiler_create(struct afmf_device *dev, struct afmf_framegen *fg)
{
    if (!afmf_config_get()->profile)
        return;
    if (!dev->limits.timestampComputeAndGraphics || dev->limits.timestampPeriod <= 0.0f) {
        AFMF_WARN("profiling requested but the device has no timestamps on compute queues");
        return;
    }
    VkQueryPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = AFMF_PROFILE_QUERIES * fg->slots,
    };
    fg->query_count = calloc(fg->slots, sizeof *fg->query_count);
    fg->query_stage = calloc((size_t)fg->slots * AFMF_PROFILE_QUERIES, sizeof *fg->query_stage);
    if (fg->query_count == NULL || fg->query_stage == NULL ||
        dev->fns.create_query_pool(dev->handle, &pool, NULL, &fg->queries) != VK_SUCCESS) {
        AFMF_WARN("profiling unavailable: query pool creation failed");
        fg->queries = VK_NULL_HANDLE;
    }
}

/* Reads the timestamps a slot wrote the last time it ran and folds them into the stage totals.
 * Called once the slot's fence has been waited on, so the results are complete. */
void afmf_framegen_profiler_collect(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t slot)
{
    uint32_t count = fg->query_count[slot];
    fg->query_count[slot] = 0; /* consumed: never folded in twice */
    if (count < 2)
        return;
    uint64_t ticks[AFMF_PROFILE_QUERIES];
    VkResult res = dev->fns.get_query_pool_results(dev->handle, fg->queries,
                                                   slot * AFMF_PROFILE_QUERIES, count, sizeof ticks,
                                                   ticks, sizeof ticks[0], VK_QUERY_RESULT_64_BIT);
    if (res != VK_SUCCESS)
        return;
    const uint8_t *stages = fg->query_stage + (size_t)slot * AFMF_PROFILE_QUERIES;
    for (uint32_t i = 1; i < count; i++)
        fg->stage_ns[stages[i]] += (double)(ticks[i] - ticks[i - 1]) * (double)dev->limits.timestampPeriod;
    fg->profiled_frames++;
}

void afmf_framegen_profiler_report(struct afmf_framegen *fg)
{
    if (fg->profiled_frames == 0)
        return;
    double total = 0.0;
    for (uint32_t s = 0; s < STAGE_COUNT; s++)
        total += fg->stage_ns[s];
    AFMF_INFO("GPU time per frame over %u frames at %ux%u: %.0f us total", fg->profiled_frames,
              fg->extent.width, fg->extent.height, total / fg->profiled_frames / 1e3);
    for (uint32_t s = 0; s < STAGE_COUNT; s++) {
        AFMF_INFO("  %-24s %7.0f us  %5.1f%%", stage_names[s],
                  fg->stage_ns[s] / fg->profiled_frames / 1e3,
                  total > 0.0 ? 100.0 * fg->stage_ns[s] / total : 0.0);
        fg->stage_ns[s] = 0.0;
    }
    fg->profiled_frames = 0;
}

/* Writes the timestamp that closes `stage`; a no-op without profiling. */
void afmf_framegen_profiler_mark(struct afmf_device *dev, struct afmf_framegen *fg,
                                 VkCommandBuffer cmd, uint32_t slot, enum stage stage)
{
    if (fg->queries == VK_NULL_HANDLE)
        return;
    uint32_t *count = &fg->query_count[slot];
    if (*count >= AFMF_PROFILE_QUERIES)
        return;
    fg->query_stage[(size_t)slot * AFMF_PROFILE_QUERIES + *count] = (uint8_t)stage;
    dev->fns.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fg->queries,
                                 slot * AFMF_PROFILE_QUERIES + *count);
    (*count)++;
}

void afmf_framegen_profiler_begin(struct afmf_device *dev, struct afmf_framegen *fg,
                                  VkCommandBuffer cmd, uint32_t slot)
{
    if (fg->queries == VK_NULL_HANDLE)
        return;
    afmf_framegen_profiler_collect(dev, fg, slot);
    if (fg->profiled_frames >= AFMF_PROFILE_INTERVAL)
        afmf_framegen_profiler_report(fg);
    dev->fns.cmd_reset_query_pool(cmd, fg->queries, slot * AFMF_PROFILE_QUERIES,
                                  AFMF_PROFILE_QUERIES);
    fg->query_count[slot] = 0;
    afmf_framegen_profiler_mark(dev, fg, cmd, slot, STAGE_INGEST); /* the opening timestamp; stage unused */
}

#pragma once

/* What the swapchain's translation units record for one another. Included from
 * swapchain_internal.h, after struct afmf_swapchain. */

/* swapchain_resources.c */
VkResult afmf_sc_gen_init(struct afmf_device *dev, struct afmf_swapchain *sc);
bool afmf_sc_ensure_pool(struct afmf_device *dev, struct afmf_swapchain *sc, uint32_t family);
void afmf_sc_gen_teardown(struct afmf_device *dev, struct afmf_swapchain *sc);
void afmf_sc_gen_disable(struct afmf_swapchain *sc, const char *why);

/* swapchain_chain.c */
bool afmf_sc_surface_offers(const struct afmf_device *dev, VkSurfaceKHR surface, VkPresentModeKHR mode);
bool afmf_sc_chain_allows_mailbox(const struct afmf_device *dev, VkSurfaceKHR surface,
                                  const void *chain, uint64_t *storage, VkPresentModeKHR *modes,
                                  uint32_t *mode_count, const void **out);

/* swapchain_record.c */
VkResult afmf_sc_record_frame(struct afmf_device *dev, struct afmf_swapchain *sc, VkCommandBuffer cmd,
                              uint32_t slot, uint32_t i, const uint32_t *companion_images, uint32_t companion_count);

/* swapchain_cadence.c */
uint64_t afmf_sc_update_cadence(struct afmf_device *dev, struct afmf_swapchain *sc);
bool afmf_sc_governor_allows(struct afmf_swapchain *sc, double last_delay_ms, uint64_t ordinal);
void afmf_sc_spare_refill(struct afmf_device *dev, struct afmf_swapchain *sc, uint64_t timeout);
bool afmf_sc_spare_take(struct afmf_device *dev, struct afmf_swapchain *sc, uint32_t *image);

/* swapchain_presenter.c */
void *afmf_sc_presenter_main(void *arg);

/* swapchain_worker.c */
VkResult afmf_sc_frame_generate(struct afmf_device *dev, struct afmf_swapchain *sc, VkQueue queue,
                                uint32_t slot_index, const VkSemaphore *waits, uint32_t wait_count,
                                bool threaded, struct afmf_present_job *job);
void *afmf_sc_worker_main(void *arg);

/* swapchain_threads.c */
bool afmf_sc_presenter_start(struct afmf_swapchain *sc);
void afmf_sc_presenter_drain(struct afmf_swapchain *sc);
void afmf_sc_presenter_stop(struct afmf_swapchain *sc);

/* swapchain_present_chain.c */
bool afmf_sc_job_from_chain(const struct afmf_swapchain *sc, const VkPresentInfoKHR *info,
                            struct afmf_present_job *job);

/* swapchain.c */
VkResult afmf_sc_present_generated(struct afmf_device *dev, struct afmf_swapchain *sc, VkQueue queue,
                                   uint32_t family, const VkPresentInfoKHR *info, uint64_t ordinal);

#pragma once

#include "layer.h"

/* Frame generation at present time.
 *
 * The swapchain is created with extra images and transfer usage. On every present of image i
 * (frame N+1) the layer takes the spare image j it acquired a frame earlier, records on its own
 * queue the optical flow between the previous frame and i and the interpolated frame into j, and
 * hands both presents to a presentation thread: j at once, i half a frame later (pacing), so the
 * application's thread returns after the submit. Without a queue of its own the presents happen
 * inline on the application's thread.
 *
 * When no image is free, or there is no history yet, only the real frame is presented and the
 * event is counted. Swapchains whose surface cannot support this fall back to pass-through. */

VkResult afmf_swapchain_create(struct afmf_device *dev, const VkSwapchainCreateInfoKHR *info,
                               const VkAllocationCallbacks *alloc, VkSwapchainKHR *out);

void afmf_swapchain_destroy(struct afmf_device *dev, VkSwapchainKHR swapchain,
                            const VkAllocationCallbacks *alloc);

VkResult afmf_swapchain_present(struct afmf_device *dev, VkQueue queue, const VkPresentInfoKHR *info);

/* vkAcquireNextImageKHR (v2 false: only swapchain/timeout/semaphore/fence of `info` are used) and
 * vkAcquireNextImage2KHR, serialised with the presentation thread. */
VkResult afmf_swapchain_acquire(struct afmf_device *dev, const VkAcquireNextImageInfoKHR *info,
                                bool v2, uint32_t *index);

/* vkGetSwapchainImagesKHR, serialised with the presentation thread: the application reads the
 * swapchain while the thread may be presenting from it. */
VkResult afmf_swapchain_get_images(struct afmf_device *dev, VkSwapchainKHR swapchain,
                                   uint32_t *count, VkImage *images);

/* Waits until every presentation thread of the device has nothing queued: vkDeviceWaitIdle
 * needs every queue idle and externally synchronised, ours included. */
void afmf_swapchain_drain_all(struct afmf_device *dev);

/* Frees whatever bookkeeping the application left behind when the device goes away. */
void afmf_swapchain_forget_all(struct afmf_device *dev);

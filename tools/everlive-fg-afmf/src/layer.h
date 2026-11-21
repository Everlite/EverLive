#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

struct afmf_swapchain;

/* Next-layer entry points the layer calls. The swapchain ones are NULL when the application did
 * not enable VK_KHR_swapchain on the device; the rest are core 1.0 and always present. */
struct afmf_device_fns {
    PFN_vkDestroyDevice destroy_device;
    PFN_vkGetDeviceQueue get_device_queue;
    PFN_vkGetDeviceQueue2 get_device_queue2;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_image_memory;
    PFN_vkCreateSemaphore create_semaphore;
    PFN_vkDestroySemaphore destroy_semaphore;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkWaitForFences wait_for_fences;
    PFN_vkResetFences reset_fences;
    PFN_vkCreateCommandPool create_command_pool;
    PFN_vkDestroyCommandPool destroy_command_pool;
    PFN_vkAllocateCommandBuffers allocate_command_buffers;
    PFN_vkBeginCommandBuffer begin_command_buffer;
    PFN_vkEndCommandBuffer end_command_buffer;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier;
    PFN_vkCmdCopyImage cmd_copy_image;
    PFN_vkQueueSubmit queue_submit;
    PFN_vkQueueSubmit2 queue_submit2;
    PFN_vkQueueSubmit2KHR queue_submit2_khr;
    PFN_vkQueueBindSparse queue_bind_sparse;
    PFN_vkQueueWaitIdle queue_wait_idle;
    PFN_vkDeviceWaitIdle device_wait_idle;

    PFN_vkCreateShaderModule create_shader_module;
    PFN_vkDestroyShaderModule destroy_shader_module;
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout;
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout;
    PFN_vkCreatePipelineLayout create_pipeline_layout;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout;
    PFN_vkCreateComputePipelines create_compute_pipelines;
    PFN_vkDestroyPipeline destroy_pipeline;
    PFN_vkCreateDescriptorPool create_descriptor_pool;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets;
    PFN_vkUpdateDescriptorSets update_descriptor_sets;
    PFN_vkCreateImageView create_image_view;
    PFN_vkDestroyImageView destroy_image_view;
    PFN_vkCreateSampler create_sampler;
    PFN_vkDestroySampler destroy_sampler;
    PFN_vkCreateBuffer create_buffer;
    PFN_vkDestroyBuffer destroy_buffer;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements;
    PFN_vkBindBufferMemory bind_buffer_memory;
    PFN_vkMapMemory map_memory;
    PFN_vkUnmapMemory unmap_memory;
    PFN_vkCmdBindPipeline cmd_bind_pipeline;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets;
    PFN_vkCmdDispatch cmd_dispatch;
    PFN_vkCmdPushConstants cmd_push_constants;
    PFN_vkCmdClearColorImage cmd_clear_color_image;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer;
    PFN_vkCmdExecuteCommands cmd_execute_commands;
    PFN_vkCreateQueryPool create_query_pool;
    PFN_vkDestroyQueryPool destroy_query_pool;
    PFN_vkCmdResetQueryPool cmd_reset_query_pool;
    PFN_vkCmdWriteTimestamp cmd_write_timestamp;
    PFN_vkGetQueryPoolResults get_query_pool_results;

    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images;
    PFN_vkAcquireNextImageKHR acquire_next_image;
    PFN_vkAcquireNextImage2KHR acquire_next_image2;
    PFN_vkQueuePresentKHR queue_present;
};

/* Instance-level entry points resolved once per device for its physical device. */
struct afmf_instance_fns {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_surface_capabilities;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR get_surface_support;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR get_surface_present_modes;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR get_surface_capabilities2; /* NULL without VK_KHR_get_surface_capabilities2 */
    PFN_vkGetPhysicalDeviceFormatProperties get_format_properties;
};

struct afmf_framegen_pipelines;

struct afmf_queue {
    VkQueue handle;
    uint32_t family;
};

/* One entry per VkDevice that passed through the layer. `key` is the loader's dispatch pointer,
 * shared by the device and every VkQueue obtained from it. */
struct afmf_device {
    void *key;
    VkDevice handle;
    VkPhysicalDevice physical_device;
    uint32_t api_version; /* what the application asked for, capped by the device */
    PFN_vkGetDeviceProcAddr gdpa;
    /* Dispatchable objects the layer creates below the loader's trampolines (command buffers) must
     * be handed to this so the loader stamps its dispatch pointer on them. */
    PFN_vkSetDeviceLoaderData set_loader_data;
    struct afmf_device_fns fns;
    struct afmf_instance_fns ifns;

    VkPhysicalDeviceMemoryProperties memory_properties;
    VkPhysicalDeviceLimits limits;
    /* Vulkan 1.1 subgroup properties (zero when the instance is 1.0 and cannot ask): the flow
     * shaders need basic, arithmetic and quad operations in compute; the size is logged. */
    VkPhysicalDeviceSubgroupProperties subgroup;
    VkQueueFamilyProperties *queue_families;
    uint32_t queue_family_count;

    /* The layer's own compute queue, requested on top of the application's at vkCreateDevice so
     * frame generation overlaps the application's rendering instead of queueing behind it.
     * VK_NULL_HANDLE when no family had a spare queue; the layer then works on the presenting
     * queue. `app_families` lists every family the application created queues in, for CONCURRENT
     * swapchains shared with `async_family`. */
    VkQueue async_queue;
    uint32_t async_family;
    bool async_high_priority; /* VK_KHR_global_priority HIGH was granted for it */
    /* The application enabled shaderStorageImageWriteWithoutFormat (DXVK and vkd3d-proton do):
     * the layer may then store into a B8G8R8A8 swapchain image directly. */
    bool storage_write_without_format;
    /* shaderInt16 is on (the application's, or added by the layer when the device offers it):
     * the block search runs its SAD on packed 16-bit pairs. */
    bool shader_int16;
    /* The application took every queue of every compute family (vkd3d-proton asks for all four
     * of RADV's), so `async_queue` is the last one it created: its own queue-level calls on it
     * are routed through `async_lock` too (afmf_Queue* in layer.c), since a VkQueue is
     * externally synchronised. Applications rarely touch their last compute queue. */
    bool async_shared;
    pthread_mutex_t async_lock; /* the application may present from several threads */
    uint32_t *app_families;
    uint32_t app_family_count;
    struct afmf_framegen_pipelines *framegen_pipelines; /* compiled at device creation on a thread, freed with the device */
    pthread_t pipelines_thread;
    bool pipelines_thread_running;
    VkResult pipelines_result;

    pthread_mutex_t lock; /* guards `queues`, `swapchains` and the per-swapchain counters */
    struct afmf_queue *queues; /* every queue handed out, sized from VkDeviceCreateInfo */
    uint32_t queue_count;
    uint32_t queue_capacity;
    struct afmf_swapchain *swapchains;

    struct afmf_device *next;
};

/* The queue family a queue was created from; false when the layer never saw the queue. */
bool afmf_device_queue_family(struct afmf_device *dev, VkQueue queue, uint32_t *family);

/* The application's own present, passed down; serialised with the layer when the queue is the
 * shared one. */
VkResult afmf_device_queue_present(struct afmf_device *dev, VkQueue queue,
                                   const VkPresentInfoKHR *info);

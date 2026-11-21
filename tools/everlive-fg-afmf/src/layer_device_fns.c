/* The next layer's device entry points, resolved once per device. */

#include "layer_internal.h"

#define LOAD_DEVICE_FN(field, name) dev->fns.field = (PFN_##name)next_gdpa(*out, #name)

/* Returns false when a core function is missing, which no conformant chain does. */
bool afmf_load_device_fns(struct afmf_device *dev, PFN_vkGetDeviceProcAddr next_gdpa, VkDevice *out)
{
    LOAD_DEVICE_FN(destroy_device, vkDestroyDevice);
    LOAD_DEVICE_FN(get_device_queue, vkGetDeviceQueue);
    LOAD_DEVICE_FN(get_device_queue2, vkGetDeviceQueue2);
    LOAD_DEVICE_FN(create_image, vkCreateImage);
    LOAD_DEVICE_FN(destroy_image, vkDestroyImage);
    LOAD_DEVICE_FN(get_image_memory_requirements, vkGetImageMemoryRequirements);
    LOAD_DEVICE_FN(allocate_memory, vkAllocateMemory);
    LOAD_DEVICE_FN(free_memory, vkFreeMemory);
    LOAD_DEVICE_FN(bind_image_memory, vkBindImageMemory);
    LOAD_DEVICE_FN(create_semaphore, vkCreateSemaphore);
    LOAD_DEVICE_FN(destroy_semaphore, vkDestroySemaphore);
    LOAD_DEVICE_FN(create_fence, vkCreateFence);
    LOAD_DEVICE_FN(destroy_fence, vkDestroyFence);
    LOAD_DEVICE_FN(wait_for_fences, vkWaitForFences);
    LOAD_DEVICE_FN(reset_fences, vkResetFences);
    LOAD_DEVICE_FN(create_command_pool, vkCreateCommandPool);
    LOAD_DEVICE_FN(destroy_command_pool, vkDestroyCommandPool);
    LOAD_DEVICE_FN(allocate_command_buffers, vkAllocateCommandBuffers);
    LOAD_DEVICE_FN(begin_command_buffer, vkBeginCommandBuffer);
    LOAD_DEVICE_FN(end_command_buffer, vkEndCommandBuffer);
    LOAD_DEVICE_FN(cmd_pipeline_barrier, vkCmdPipelineBarrier);
    LOAD_DEVICE_FN(cmd_copy_image, vkCmdCopyImage);
    LOAD_DEVICE_FN(queue_submit, vkQueueSubmit);
    LOAD_DEVICE_FN(queue_submit2, vkQueueSubmit2);
    LOAD_DEVICE_FN(queue_submit2_khr, vkQueueSubmit2KHR);
    LOAD_DEVICE_FN(queue_bind_sparse, vkQueueBindSparse);
    LOAD_DEVICE_FN(queue_wait_idle, vkQueueWaitIdle);
    LOAD_DEVICE_FN(device_wait_idle, vkDeviceWaitIdle);
    LOAD_DEVICE_FN(create_shader_module, vkCreateShaderModule);
    LOAD_DEVICE_FN(destroy_shader_module, vkDestroyShaderModule);
    LOAD_DEVICE_FN(create_descriptor_set_layout, vkCreateDescriptorSetLayout);
    LOAD_DEVICE_FN(destroy_descriptor_set_layout, vkDestroyDescriptorSetLayout);
    LOAD_DEVICE_FN(create_pipeline_layout, vkCreatePipelineLayout);
    LOAD_DEVICE_FN(destroy_pipeline_layout, vkDestroyPipelineLayout);
    LOAD_DEVICE_FN(create_compute_pipelines, vkCreateComputePipelines);
    LOAD_DEVICE_FN(destroy_pipeline, vkDestroyPipeline);
    LOAD_DEVICE_FN(create_descriptor_pool, vkCreateDescriptorPool);
    LOAD_DEVICE_FN(destroy_descriptor_pool, vkDestroyDescriptorPool);
    LOAD_DEVICE_FN(allocate_descriptor_sets, vkAllocateDescriptorSets);
    LOAD_DEVICE_FN(update_descriptor_sets, vkUpdateDescriptorSets);
    LOAD_DEVICE_FN(create_image_view, vkCreateImageView);
    LOAD_DEVICE_FN(destroy_image_view, vkDestroyImageView);
    LOAD_DEVICE_FN(create_sampler, vkCreateSampler);
    LOAD_DEVICE_FN(destroy_sampler, vkDestroySampler);
    LOAD_DEVICE_FN(create_buffer, vkCreateBuffer);
    LOAD_DEVICE_FN(destroy_buffer, vkDestroyBuffer);
    LOAD_DEVICE_FN(get_buffer_memory_requirements, vkGetBufferMemoryRequirements);
    LOAD_DEVICE_FN(bind_buffer_memory, vkBindBufferMemory);
    LOAD_DEVICE_FN(map_memory, vkMapMemory);
    LOAD_DEVICE_FN(unmap_memory, vkUnmapMemory);
    LOAD_DEVICE_FN(cmd_bind_pipeline, vkCmdBindPipeline);
    LOAD_DEVICE_FN(cmd_bind_descriptor_sets, vkCmdBindDescriptorSets);
    LOAD_DEVICE_FN(cmd_dispatch, vkCmdDispatch);
    LOAD_DEVICE_FN(cmd_push_constants, vkCmdPushConstants);
    LOAD_DEVICE_FN(cmd_clear_color_image, vkCmdClearColorImage);
    LOAD_DEVICE_FN(cmd_copy_image_to_buffer, vkCmdCopyImageToBuffer);
    LOAD_DEVICE_FN(cmd_execute_commands, vkCmdExecuteCommands);
    LOAD_DEVICE_FN(create_query_pool, vkCreateQueryPool);
    LOAD_DEVICE_FN(destroy_query_pool, vkDestroyQueryPool);
    LOAD_DEVICE_FN(cmd_reset_query_pool, vkCmdResetQueryPool);
    LOAD_DEVICE_FN(cmd_write_timestamp, vkCmdWriteTimestamp);
    LOAD_DEVICE_FN(get_query_pool_results, vkGetQueryPoolResults);
    /* NULL when VK_KHR_swapchain is not enabled: the hooks are then never handed out (see GDPA). */
    LOAD_DEVICE_FN(create_swapchain, vkCreateSwapchainKHR);
    LOAD_DEVICE_FN(destroy_swapchain, vkDestroySwapchainKHR);
    LOAD_DEVICE_FN(get_swapchain_images, vkGetSwapchainImagesKHR);
    LOAD_DEVICE_FN(acquire_next_image, vkAcquireNextImageKHR);
    LOAD_DEVICE_FN(acquire_next_image2, vkAcquireNextImage2KHR);
    LOAD_DEVICE_FN(queue_present, vkQueuePresentKHR);

    const struct afmf_device_fns *f = &dev->fns;
    return f->destroy_device && f->get_device_queue && f->create_image && f->destroy_image &&
           f->get_image_memory_requirements && f->allocate_memory && f->free_memory &&
           f->bind_image_memory && f->create_semaphore && f->destroy_semaphore &&
           f->create_fence && f->destroy_fence && f->wait_for_fences && f->reset_fences &&
           f->create_command_pool && f->destroy_command_pool && f->allocate_command_buffers &&
           f->begin_command_buffer && f->end_command_buffer && f->cmd_pipeline_barrier &&
           f->cmd_copy_image && f->queue_submit && f->queue_wait_idle && f->device_wait_idle &&
           f->create_shader_module &&
           f->destroy_shader_module && f->create_descriptor_set_layout &&
           f->destroy_descriptor_set_layout && f->create_pipeline_layout &&
           f->destroy_pipeline_layout && f->create_compute_pipelines && f->destroy_pipeline &&
           f->create_descriptor_pool && f->destroy_descriptor_pool &&
           f->allocate_descriptor_sets && f->update_descriptor_sets && f->create_image_view &&
           f->destroy_image_view && f->create_sampler && f->destroy_sampler && f->create_buffer &&
           f->destroy_buffer && f->get_buffer_memory_requirements && f->bind_buffer_memory &&
           f->map_memory && f->unmap_memory && f->cmd_bind_pipeline &&
           f->cmd_bind_descriptor_sets && f->cmd_dispatch && f->cmd_push_constants &&
           f->cmd_clear_color_image && f->cmd_copy_image_to_buffer && f->cmd_execute_commands &&
           f->create_query_pool &&
           f->destroy_query_pool && f->cmd_reset_query_pool && f->cmd_write_timestamp &&
           f->get_query_pool_results;
}

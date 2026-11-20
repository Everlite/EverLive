#pragma once

/* Frame generation: a small batcher for vkUpdateDescriptorSets, one set at a time. Used only
 * by framegen_descriptors.c. */

struct set_writer {
    struct afmf_device *dev;
    VkDescriptorSet set;
    VkWriteDescriptorSet writes[16];
    VkDescriptorImageInfo images[16];
    VkDescriptorBufferInfo buffers[16];
    uint32_t count;
};

static inline void writer_begin(struct set_writer *w, struct afmf_device *dev, VkDescriptorSet set)
{
    memset(w, 0, sizeof *w);
    w->dev = dev;
    w->set = set;
}

static inline void writer_image(struct set_writer *w, uint32_t binding, VkDescriptorType type,
                                VkImageView view, VkSampler sampler)
{
    uint32_t i = w->count++;
    w->images[i] = (VkDescriptorImageInfo){
        .sampler = sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    w->writes[i] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = w->set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = &w->images[i],
    };
}

static inline void writer_ubo(struct set_writer *w, uint32_t binding, VkBuffer buffer)
{
    uint32_t i = w->count++;
    w->buffers[i] = (VkDescriptorBufferInfo){.buffer = buffer, .offset = 0, .range = AFMF_CB_SIZE};
    w->writes[i] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = w->set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = UBO,
        .pBufferInfo = &w->buffers[i],
    };
}

static inline void writer_end(struct set_writer *w)
{
    w->dev->fns.update_descriptor_sets(w->dev->handle, w->count, w->writes, 0, NULL);
}

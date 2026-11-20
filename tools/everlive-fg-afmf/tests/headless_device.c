/* The device, its queue and command buffer, and the teardown of everything. */

#include "headless.h"

#define MAX_FAMILIES 16u
#define MAX_QUEUES 16u

/* AFMF_TEST_PRESENT_ID=1|2: present with VK_KHR_present_id / VK_KHR_present_id2 ids, which the
 * layer must carry on the real frame instead of falling back to inline presents. Skipped when the
 * driver or the headless surface does not offer the extension. Returns the extension's name. */
static const char *present_id_extension(struct ctx *ctx)
{
    const char *ext = ctx->present_id == 2 ? "VK_KHR_present_id2" : VK_KHR_PRESENT_ID_EXTENSION_NAME;
    bool found = device_extension_available(ctx->physical_device, ext);
#ifndef VK_KHR_present_id2
    if (ctx->present_id == 2)
        found = false; /* headers too old to build the structures */
#endif
#ifdef VK_KHR_present_id2
    if (found && ctx->present_id == 2) { /* also a per-surface capability */
        VkSurfaceCapabilitiesPresentId2KHR id2_caps = {
            .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
        VkSurfaceCapabilities2KHR caps2 = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
                                           .pNext = &id2_caps};
        VkPhysicalDeviceSurfaceInfo2KHR surface_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .surface = ctx->surface};
        PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR get_caps2 =
            (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)vkGetInstanceProcAddr(
                ctx->instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
        found = get_caps2 != NULL &&
                get_caps2(ctx->physical_device, &surface_info, &caps2) == VK_SUCCESS &&
                id2_caps.presentId2Supported == VK_TRUE;
    }
#endif
    if (!found) {
        (void)fprintf(stderr, "skipped: %s unavailable on this surface\n", ext);
        destroy(ctx); /* LeakSanitizer turns a skip with live objects into a failure */
        exit(EXIT_SKIP);
    }
    return ext;
}

/* AFMF_TEST_ALL_QUEUES=1: take every queue of every compute-capable family, as vkd3d-proton does,
 * so the layer has to share one of ours instead of getting its own. Returns the entry count. */
static uint32_t queue_requests(struct ctx *ctx, VkDeviceQueueCreateInfo *queues, const float *priorities)
{
    queues[0] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->queue_family,
        .queueCount = 1,
        .pQueuePriorities = priorities,
    };
    uint32_t count = 1;
    if (!env_is_1("AFMF_TEST_ALL_QUEUES"))
        return count;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &families, NULL);
    VkQueueFamilyProperties props[MAX_FAMILIES];
    if (families > MAX_FAMILIES)
        families = MAX_FAMILIES;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &families, props);
    for (uint32_t f = 0; f < families; f++) {
        if (!(props[f].queueFlags & VK_QUEUE_COMPUTE_BIT))
            continue;
        uint32_t n = props[f].queueCount < MAX_QUEUES ? props[f].queueCount : MAX_QUEUES;
        if (f == ctx->queue_family) {
            queues[0].queueCount = n;
            continue;
        }
        queues[count++] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = f,
            .queueCount = n,
            .pQueuePriorities = priorities,
        };
    }
    return count;
}

bool create_device_and_swapchain(struct ctx *ctx)
{
    static const float priorities[MAX_QUEUES] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    const char *extensions[3] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, NULL, NULL};
    uint32_t extension_count = 1;
    /* AFMF_TEST_PRESENT_MODES=1: the swapchain carries VK_EXT_swapchain_maintenance1's list of
     * allowed per-present modes (FIFO only) and every present asks for FIFO; the layer's MAILBOX
     * rewrite must extend the list and rewrite the per-present mode, or the presents are invalid. */
    ctx->present_modes = wanted_present_modes();
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
        .swapchainMaintenance1 = VK_TRUE};
    if (ctx->present_modes) {
        if (!device_extension_available(ctx->physical_device, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
            (void)fprintf(stderr, "skipped: " VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME " unavailable\n");
            destroy(ctx); /* LeakSanitizer turns a skip with live objects into a failure */
            exit(EXIT_SKIP);
        }
        extensions[extension_count++] = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
    }
    ctx->present_id = wanted_present_id();
    VkPhysicalDevicePresentIdFeaturesKHR id_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .presentId = VK_TRUE};
#ifdef VK_KHR_present_id2
    VkPhysicalDevicePresentId2FeaturesKHR id2_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, .presentId2 = VK_TRUE};
#endif
    void *device_next = NULL;
    if (ctx->present_id != 0) {
        extensions[extension_count++] = present_id_extension(ctx);
#ifdef VK_KHR_present_id2
        device_next = ctx->present_id == 2 ? (void *)&id2_features : (void *)&id_features;
#else
        device_next = &id_features;
#endif
    }
    if (ctx->present_modes) {
        maintenance1_features.pNext = device_next;
        device_next = &maintenance1_features;
    }
    VkDeviceQueueCreateInfo queues[MAX_FAMILIES];
    uint32_t queue_info_count = queue_requests(ctx, queues, priorities);
    VkDeviceCreateInfo device = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = device_next,
        .queueCreateInfoCount = queue_info_count,
        .pQueueCreateInfos = queues,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = extensions,
    };
    CHECK(vkCreateDevice(ctx->physical_device, &device, NULL, &ctx->device));
    vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);

    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx->queue_family,
    };
    CHECK(vkCreateCommandPool(ctx->device, &pool, NULL, &ctx->command_pool));
    VkCommandBufferAllocateInfo alloc_cmd = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CHECK(vkAllocateCommandBuffers(ctx->device, &alloc_cmd, &ctx->command_buffer));

    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CHECK(vkCreateSemaphore(ctx->device, &semaphore, NULL, &ctx->acquired));
    CHECK(vkCreateSemaphore(ctx->device, &semaphore, NULL, &ctx->ready));

    return create_swapchain(ctx, 0, 0);
}

void destroy(struct ctx *ctx)
{
    if (ctx->device != VK_NULL_HANDLE) {
        if (wanted_no_idle()) {
            /* The layer's submissions still wait on `ready`: the swapchain goes first, and
             * with it every frame the layer had in flight. */
            if (ctx->swapchain != VK_NULL_HANDLE)
                vkDestroySwapchainKHR(ctx->device, ctx->swapchain, NULL);
            ctx->swapchain = VK_NULL_HANDLE;
        }
        (void)vkDeviceWaitIdle(ctx->device);
        if (ctx->staging_mapped != NULL)
            vkUnmapMemory(ctx->device, ctx->staging_memory);
        if (ctx->staging != VK_NULL_HANDLE)
            vkDestroyBuffer(ctx->device, ctx->staging, NULL);
        if (ctx->staging_memory != VK_NULL_HANDLE)
            vkFreeMemory(ctx->device, ctx->staging_memory, NULL);
        if (ctx->ready != VK_NULL_HANDLE)
            vkDestroySemaphore(ctx->device, ctx->ready, NULL);
        if (ctx->acquired != VK_NULL_HANDLE)
            vkDestroySemaphore(ctx->device, ctx->acquired, NULL);
        if (ctx->command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
        if (ctx->swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(ctx->device, ctx->swapchain, NULL);
        vkDestroyDevice(ctx->device, NULL);
    }
    if (ctx->instance != VK_NULL_HANDLE) {
        if (ctx->surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (ctx->messenger != VK_NULL_HANDLE) {
            PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
                (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                    ctx->instance, "vkDestroyDebugUtilsMessengerEXT");
            if (destroy_messenger != NULL)
                destroy_messenger(ctx->instance, ctx->messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
    }
}

/* Everything create_swapchain made, in reverse. The device and its queue stay. */
void destroy_swapchain(struct ctx *ctx)
{
    (void)vkDeviceWaitIdle(ctx->device);
    if (ctx->staging_mapped != NULL)
        vkUnmapMemory(ctx->device, ctx->staging_memory);
    ctx->staging_mapped = NULL;
    if (ctx->staging != VK_NULL_HANDLE)
        vkDestroyBuffer(ctx->device, ctx->staging, NULL);
    ctx->staging = VK_NULL_HANDLE;
    if (ctx->staging_memory != VK_NULL_HANDLE)
        vkFreeMemory(ctx->device, ctx->staging_memory, NULL);
    ctx->staging_memory = VK_NULL_HANDLE;
    if (ctx->swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(ctx->device, ctx->swapchain, NULL);
    ctx->swapchain = VK_NULL_HANDLE;
}

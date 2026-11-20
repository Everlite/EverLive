/* VK_LAYER_AFMF: loader plumbing.
 *
 * From a build tree the layer is found through VK_ADD_IMPLICIT_LAYER_PATH=<build>/layer when it is
 * enabled implicitly (AFMF_ENABLE=1), or through VK_ADD_LAYER_PATH when an application enables it
 * explicitly by name; the loader searches the two kinds of manifest separately.
 *
 * Implements the loader/layer interface version 2 (vk_layer.h, LoaderLayerInterface.md): version
 * negotiation, the instance and device chains, and the dispatch tables keyed by the loader's
 * dispatch pointer. The behaviour lives in swapchain.c; this file routes calls there: the
 * swapchain hooks, the surface hooks, the hook tables and the two proc-address entry points. The
 * instance, device and queue hooks live in the layer_*.c files next to it. */

#include "layer_internal.h"

/* ---- swapchain hooks ----------------------------------------------------------------------- */

static VKAPI_ATTR VkResult VKAPI_CALL afmf_CreateSwapchainKHR(VkDevice device,
                                                              const VkSwapchainCreateInfoKHR *info,
                                                              const VkAllocationCallbacks *alloc,
                                                              VkSwapchainKHR *out)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(device));
    if (dev == NULL || dev->fns.create_swapchain == NULL)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return afmf_swapchain_create(dev, info, alloc, out);
}

static VKAPI_ATTR void VKAPI_CALL afmf_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                           const VkAllocationCallbacks *alloc)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(device));
    if (dev == NULL || dev->fns.destroy_swapchain == NULL) {
        AFMF_ERR("vkDestroySwapchainKHR on device %p without VK_KHR_swapchain", (void *)device);
        return;
    }
    afmf_swapchain_destroy(dev, swapchain, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL afmf_GetSwapchainImagesKHR(VkDevice device,
                                                                 VkSwapchainKHR swapchain,
                                                                 uint32_t *count, VkImage *images)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(device));
    if (dev == NULL || dev->fns.get_swapchain_images == NULL)
        return VK_ERROR_DEVICE_LOST;
    return afmf_swapchain_get_images(dev, swapchain, count, images);
}

static VKAPI_ATTR VkResult VKAPI_CALL afmf_AcquireNextImageKHR(VkDevice device,
                                                               VkSwapchainKHR swapchain,
                                                               uint64_t timeout,
                                                               VkSemaphore semaphore, VkFence fence,
                                                               uint32_t *index)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(device));
    if (dev == NULL || dev->fns.acquire_next_image == NULL)
        return VK_ERROR_DEVICE_LOST;
    VkAcquireNextImageInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
        .swapchain = swapchain,
        .timeout = timeout,
        .semaphore = semaphore,
        .fence = fence,
    };
    return afmf_swapchain_acquire(dev, &info, false, index);
}

static VKAPI_ATTR VkResult VKAPI_CALL afmf_AcquireNextImage2KHR(VkDevice device,
                                                                const VkAcquireNextImageInfoKHR *info,
                                                                uint32_t *index)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(device));
    if (dev == NULL || dev->fns.acquire_next_image2 == NULL)
        return VK_ERROR_DEVICE_LOST;
    return afmf_swapchain_acquire(dev, info, true, index);
}

static VKAPI_ATTR VkResult VKAPI_CALL afmf_QueuePresentKHR(VkQueue queue,
                                                           const VkPresentInfoKHR *info)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(queue));
    if (dev == NULL || dev->fns.queue_present == NULL) {
        AFMF_ERR("vkQueuePresentKHR on queue %p of an unknown device", (void *)queue);
        return VK_ERROR_DEVICE_LOST;
    }
    return afmf_swapchain_present(dev, queue, info);
}

/* ---- surface creation, logged ------------------------------------------------------------- */

/* Every vkCreate*SurfaceKHR has the shape (instance, create-info, allocator, out) and the
 * create-info is passed through untouched, so one signature covers every platform without its
 * headers. */
typedef VkResult(VKAPI_PTR *PFN_afmf_create_surface)(VkInstance, const void *,
                                                     const VkAllocationCallbacks *, VkSurfaceKHR *);

static VkResult create_surface_logged(VkInstance instance, const char *fn_name,
                                      const char *platform, const void *info,
                                      const VkAllocationCallbacks *alloc, VkSurfaceKHR *out)
{
    struct afmf_instance *inst = afmf_instance_find(dispatch_key(instance));
    if (inst == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_afmf_create_surface next = (PFN_afmf_create_surface)inst->gipa(instance, fn_name);
    if (next == NULL)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult res = next(instance, info, alloc, out);
    if (res == VK_SUCCESS)
        AFMF_INFO("surface %p created: %s", (void *)*out, platform);
    return res;
}

#define SURFACE_HOOK(function, platform)                                                          \
    static VKAPI_ATTR VkResult VKAPI_CALL afmf_##function(                                       \
        VkInstance instance, const void *info, const VkAllocationCallbacks *alloc,               \
        VkSurfaceKHR *out)                                                                        \
    {                                                                                             \
        return create_surface_logged(instance, "vk" #function, platform, info, alloc, out);      \
    }

SURFACE_HOOK(CreateWaylandSurfaceKHR, "Wayland")
SURFACE_HOOK(CreateXcbSurfaceKHR, "X11 (xcb)")
SURFACE_HOOK(CreateXlibSurfaceKHR, "X11 (xlib)")
SURFACE_HOOK(CreateHeadlessSurfaceEXT, "headless")
SURFACE_HOOK(CreateDisplayPlaneSurfaceKHR, "direct display")

/* ---- proc address routing ------------------------------------------------------------------ */

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL afmf_GetInstanceProcAddr(VkInstance instance,
                                                                          const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL afmf_GetDeviceProcAddr(VkDevice device,
                                                                        const char *name);

struct hook {
    const char *name;
    PFN_vkVoidFunction fn;
};

#define HOOK(function) {"vk" #function, (PFN_vkVoidFunction)afmf_##function}

static const struct hook instance_hooks[] = {
    HOOK(GetInstanceProcAddr), HOOK(CreateInstance), HOOK(DestroyInstance), HOOK(CreateDevice),
};

/* Only handed out when the next layer/driver has them (the platform extension is enabled). */
static const struct hook surface_hooks[] = {
    HOOK(CreateWaylandSurfaceKHR),   HOOK(CreateXcbSurfaceKHR),          HOOK(CreateXlibSurfaceKHR),
    HOOK(CreateHeadlessSurfaceEXT),  HOOK(CreateDisplayPlaneSurfaceKHR),
};

static const struct hook device_hooks[] = {
    HOOK(GetDeviceProcAddr), HOOK(DestroyDevice), HOOK(GetDeviceQueue), HOOK(GetDeviceQueue2),
    HOOK(QueueSubmit),       HOOK(QueueSubmit2),  HOOK(QueueSubmit2KHR), HOOK(QueueBindSparse),
    HOOK(QueueWaitIdle),     HOOK(DeviceWaitIdle),
};

/* Only handed out when the next layer/driver has them, i.e. when VK_KHR_swapchain is enabled. */
static const struct hook swapchain_hooks[] = {
    HOOK(CreateSwapchainKHR),   HOOK(DestroySwapchainKHR),   HOOK(QueuePresentKHR),
    HOOK(AcquireNextImageKHR),  HOOK(AcquireNextImage2KHR),  HOOK(GetSwapchainImagesKHR),
};

#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

static PFN_vkVoidFunction lookup(const struct hook *hooks, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(hooks[i].name, name) == 0)
            return hooks[i].fn;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL afmf_GetInstanceProcAddr(VkInstance instance,
                                                                          const char *name)
{
    PFN_vkVoidFunction fn = lookup(instance_hooks, ARRAY_LEN(instance_hooks), name);
    if (fn != NULL)
        return fn;

    /* A device-level or surface hook only where the chain below has the command, as the device
     * route does: a hook handed out for a command the driver lacks has nothing to forward to. */
    if (instance == VK_NULL_HANDLE)
        return NULL;
    struct afmf_instance *inst = afmf_instance_find(dispatch_key(instance));
    if (inst == NULL)
        return NULL;
    fn = lookup(device_hooks, ARRAY_LEN(device_hooks), name);
    if (fn == NULL)
        fn = lookup(swapchain_hooks, ARRAY_LEN(swapchain_hooks), name);
    if (fn == NULL)
        fn = lookup(surface_hooks, ARRAY_LEN(surface_hooks), name);
    if (fn != NULL)
        return inst->gipa(instance, name) != NULL ? fn : NULL;
    return inst->gipa(instance, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL afmf_GetDeviceProcAddr(VkDevice device,
                                                                        const char *name)
{
    if (device == VK_NULL_HANDLE)
        return NULL;
    struct afmf_device *dev = afmf_device_find(dispatch_key(device));
    if (dev == NULL)
        return NULL;

    PFN_vkVoidFunction fn = lookup(device_hooks, ARRAY_LEN(device_hooks), name);
    if (fn != NULL)
        return dev->gdpa(device, name) != NULL ? fn : NULL;

    fn = lookup(swapchain_hooks, ARRAY_LEN(swapchain_hooks), name);
    if (fn != NULL)
        return dev->gdpa(device, name) != NULL ? fn : NULL;

    return dev->gdpa(device, name);
}

/* ---- loader entry point -------------------------------------------------------------------- */

AFMF_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *negotiate)
{
    if (negotiate == NULL || negotiate->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (negotiate->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

    negotiate->loaderLayerInterfaceVersion = 2;
    negotiate->pfnGetInstanceProcAddr = afmf_GetInstanceProcAddr;
    negotiate->pfnGetDeviceProcAddr = afmf_GetDeviceProcAddr;
    negotiate->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}

/* The instance: the layer under test by name, validation below it when installed, the headless
 * surface, and the physical device that can present to it. */

#include "headless.h"

bool device_extension_available(VkPhysicalDevice device, const char *name)
{
    uint32_t n = 0;
    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &n, NULL) == VK_SUCCESS && n > 0) {
        VkExtensionProperties *props = calloc(n, sizeof *props);
        if (props != NULL && vkEnumerateDeviceExtensionProperties(device, NULL, &n, props) == VK_SUCCESS)
            for (uint32_t k = 0; k < n && !found; k++)
                found = strcmp(props[k].extensionName, name) == 0;
        free(props);
    }
    return found;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL on_debug_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    /* Only the validation layer's verdicts count. GENERAL messages are the loader talking, e.g.
     * about some other installed implicit layer it had to skip, which is not this layer's bug. */
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) &&
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) {
        struct ctx *ctx = user;
        ctx->validation_errors++;
        (void)fprintf(stderr, "validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

bool instance_layer_available(const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS || count == 0)
        return false;
    VkLayerProperties *props = calloc(count, sizeof *props);
    if (props == NULL)
        return false;
    bool found = false;
    if (vkEnumerateInstanceLayerProperties(&count, props) == VK_SUCCESS)
        for (uint32_t i = 0; i < count && !found; i++)
            found = strcmp(props[i].layerName, name) == 0;
    free(props);
    return found;
}

bool instance_extension_available(const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS || count == 0)
        return false;
    VkExtensionProperties *props = calloc(count, sizeof *props);
    if (props == NULL)
        return false;
    bool found = false;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, props) == VK_SUCCESS)
        for (uint32_t i = 0; i < count && !found; i++)
            found = strcmp(props[i].extensionName, name) == 0;
    free(props);
    return found;
}

bool create_instance(struct ctx *ctx, bool with_validation)
{
    /* Index 0 is closest to the application: the layer under test first, validation below it. */
    const char *layers[2] = {LAYER_NAME, VALIDATION_LAYER_NAME};
    const char *extensions[5] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                 VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    uint32_t extension_count = 2;
    if (with_validation)
        extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (wanted_present_id() == 2 || wanted_present_modes()) /* present_id2 and surface_maintenance1 depend on it */
        extensions[extension_count++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
    if (wanted_present_modes()) {
        if (!instance_extension_available(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) {
            (void)fprintf(stderr, "skipped: " VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME " unavailable\n");
            destroy(ctx); /* LeakSanitizer turns a skip with live objects into a failure */
            exit(EXIT_SKIP);
        }
        extensions[extension_count++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "afmf_headless",
        .apiVersion = VK_API_VERSION_1_1,
    };
    /* AFMF_TEST_GPUAV=1 asks the validation layer to instrument the shaders themselves, which
     * is the only way to see a read or a write of theirs leave its buffer. It is asked for here
     * rather than through the layer's own settings, which differ between its versions, and it
     * makes the run slow enough that only its own test uses it. */
    VkValidationFeatureEnableEXT gpu_assisted[] = {VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT};
    VkValidationFeaturesEXT features = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = 1,
        .pEnabledValidationFeatures = gpu_assisted,
    };
    bool with_gpuav = with_validation && env_is_1("AFMF_TEST_GPUAV");
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = with_gpuav ? &features : NULL,
        .pApplicationInfo = &app,
        .enabledLayerCount = with_validation ? 2u : 1u,
        .ppEnabledLayerNames = layers,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = extensions,
    };
    CHECK(vkCreateInstance(&info, NULL, &ctx->instance));

    if (with_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(ctx->instance, "vkCreateDebugUtilsMessengerEXT");
        if (create == NULL)
            return false;
        VkDebugUtilsMessengerCreateInfoEXT messenger = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
            .pfnUserCallback = on_debug_message,
            .pUserData = ctx,
        };
        CHECK(create(ctx->instance, &messenger, NULL, &ctx->messenger));
    }

    PFN_vkCreateHeadlessSurfaceEXT create_surface = (PFN_vkCreateHeadlessSurfaceEXT)
        vkGetInstanceProcAddr(ctx->instance, "vkCreateHeadlessSurfaceEXT");
    if (create_surface == NULL)
        return false;
    VkHeadlessSurfaceCreateInfoEXT surface = {
        .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
    };
    CHECK(create_surface(ctx->instance, &surface, NULL, &ctx->surface));
    return true;
}

/* Picks the first physical device with a graphics queue family that can present to the surface. */
bool pick_physical_device(struct ctx *ctx)
{
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(ctx->instance, &count, NULL));
    if (count == 0)
        return false;
    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (devices == NULL)
        return false;
    bool found = false;
    if (vkEnumeratePhysicalDevices(ctx->instance, &count, devices) == VK_SUCCESS) {
        for (uint32_t d = 0; d < count && !found; d++) {
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, NULL);
            VkQueueFamilyProperties *props = calloc(families, sizeof *props);
            if (props == NULL)
                break;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, props);
            for (uint32_t f = 0; f < families && !found; f++) {
                VkBool32 present = VK_FALSE;
                if ((props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    vkGetPhysicalDeviceSurfaceSupportKHR(devices[d], f, ctx->surface, &present) ==
                        VK_SUCCESS &&
                    present) {
                    ctx->physical_device = devices[d];
                    ctx->queue_family = f;
                    found = true;
                }
            }
            free(props);
        }
    }
    free(devices);
    if (found) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(ctx->physical_device, &props);
        (void)fprintf(stderr, "using %s\n", props.deviceName);
    }
    return found;
}

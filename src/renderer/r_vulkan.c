/*
 * QUICKEN Renderer - Vulkan Instance, Device, Swapchain
 */

#include "r_types.h"
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global renderer state */
r_state_t g_r;

/* ---- Debug Callback ---- */

#ifdef QUICKEN_DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user_data)
{
    QK_UNUSED(type);
    QK_UNUSED(user_data);

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    }
    return VK_FALSE;
}
#endif

/* ---- Instance ---- */

qk_result_t r_vulkan_create_instance(void)
{
    /* Check Vulkan version */
    u32 api_version = 0;
    vkEnumerateInstanceVersion(&api_version);
    if (api_version < VK_API_VERSION_1_2) {
        fprintf(stderr, "Vulkan 1.2 required, got %u.%u\n",
                VK_VERSION_MAJOR(api_version), VK_VERSION_MINOR(api_version));
        return QK_ERROR_VULKAN_INIT;
    }

    VkApplicationInfo app_info = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "QUICKEN",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName        = "QUICKEN Engine",
        .engineVersion      = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion         = VK_API_VERSION_1_2
    };

    /* Get SDL-required instance extensions */
    u32 sdl_ext_count = 0;
    const char * const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);

    /* Build extension list */
    const char *extensions[16];
    u32 ext_count = 0;

    for (u32 i = 0; i < sdl_ext_count && ext_count < 14; i++) {
        extensions[ext_count++] = sdl_exts[i];
    }

#ifdef QUICKEN_DEBUG
    extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
#endif

    /* Layers */
    u32 layer_count = 0;
    const char *layers[2];

#ifdef QUICKEN_DEBUG
    layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
#endif

    VkInstanceCreateInfo create_info = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &app_info,
        .enabledExtensionCount   = ext_count,
        .ppEnabledExtensionNames = extensions,
        .enabledLayerCount       = layer_count,
        .ppEnabledLayerNames     = layers
    };

    VkResult result = vkCreateInstance(&create_info, NULL, &g_r.instance.handle);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", result);
        return QK_ERROR_VULKAN_INIT;
    }

    /* Debug messenger */
#ifdef QUICKEN_DEBUG
    {
        VkDebugUtilsMessengerCreateInfoEXT dbg_info = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_callback
        };

        PFN_vkCreateDebugUtilsMessengerEXT func =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                g_r.instance.handle, "vkCreateDebugUtilsMessengerEXT");
        if (func) {
            func(g_r.instance.handle, &dbg_info, NULL, &g_r.instance.debug_messenger);
        }
    }
#endif

    return QK_SUCCESS;
}

/* ---- Physical Device Selection ---- */

static i32 score_physical_device(VkPhysicalDevice device, VkSurfaceKHR surface,
                                 r_queue_families_t *out_families)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    /* Require Vulkan 1.2 */
    if (props.apiVersion < VK_API_VERSION_1_2) return -1;

    /* Check for swapchain extension */
    u32 ext_count = 0;
    vkEnumerateDeviceExtensionProperties(device, NULL, &ext_count, NULL);
    VkExtensionProperties *exts = malloc(ext_count * sizeof(VkExtensionProperties));
    if (!exts) return -1;
    vkEnumerateDeviceExtensionProperties(device, NULL, &ext_count, exts);

    bool has_swapchain = false;
    for (u32 i = 0; i < ext_count; i++) {
        if (strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            has_swapchain = true;
            break;
        }
    }
    free(exts);
    if (!has_swapchain) return -1;

    /* Find queue families */
    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, NULL);
    VkQueueFamilyProperties *families = malloc(family_count * sizeof(VkQueueFamilyProperties));
    if (!families) return -1;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families);

    i32 graphics_family = -1;
    i32 transfer_family = -1;

    for (u32 i = 0; i < family_count; i++) {
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);

        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
            graphics_family = (i32)i;
        }

        /* Prefer a dedicated transfer queue */
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            transfer_family = (i32)i;
        }
    }

    free(families);

    if (graphics_family < 0) return -1;

    out_families->graphics = (u32)graphics_family;
    if (transfer_family >= 0) {
        out_families->transfer = (u32)transfer_family;
        out_families->has_dedicated_transfer = true;
    } else {
        out_families->transfer = (u32)graphics_family;
        out_families->has_dedicated_transfer = false;
    }

    /* Score */
    i32 score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    }
    score += (i32)(props.limits.maxImageDimension2D / 1024);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(device, &mem_props);
    for (u32 i = 0; i < mem_props.memoryHeapCount; i++) {
        if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            score += (i32)(mem_props.memoryHeaps[i].size / (1024 * 1024 * 256));
        }
    }

    return score;
}

qk_result_t r_vulkan_pick_physical_device(void)
{
    u32 device_count = 0;
    vkEnumeratePhysicalDevices(g_r.instance.handle, &device_count, NULL);
    if (device_count == 0) {
        fprintf(stderr, "No Vulkan devices found\n");
        return QK_ERROR_NO_SUITABLE_GPU;
    }

    VkPhysicalDevice *devices = malloc(device_count * sizeof(VkPhysicalDevice));
    if (!devices) return QK_ERROR_OUT_OF_MEMORY;
    vkEnumeratePhysicalDevices(g_r.instance.handle, &device_count, devices);

    i32 best_score = -1;
    VkPhysicalDevice best_device = VK_NULL_HANDLE;
    r_queue_families_t best_families = {0};

    for (u32 i = 0; i < device_count; i++) {
        r_queue_families_t families = {0};
        i32 score = score_physical_device(devices[i], g_r.swapchain.surface, &families);
        if (score > best_score) {
            best_score = score;
            best_device = devices[i];
            best_families = families;
        }
    }

    free(devices);

    if (best_device == VK_NULL_HANDLE) {
        fprintf(stderr, "No suitable GPU found\n");
        return QK_ERROR_NO_SUITABLE_GPU;
    }

    g_r.device.physical = best_device;
    g_r.device.families = best_families;
    vkGetPhysicalDeviceProperties(best_device, &g_r.device.properties);
    vkGetPhysicalDeviceMemoryProperties(best_device, &g_r.device.mem_properties);

    fprintf(stderr, "[Renderer] GPU: %s\n", g_r.device.properties.deviceName);

    return QK_SUCCESS;
}

/* ---- Logical Device ---- */

qk_result_t r_vulkan_create_device(void)
{
    float priority_high = 1.0f;
    float priority_low  = 0.5f;

    VkDeviceQueueCreateInfo queue_infos[2];
    u32 queue_info_count = 0;

    /* Graphics queue */
    queue_infos[queue_info_count++] = (VkDeviceQueueCreateInfo){
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_r.device.families.graphics,
        .queueCount       = 1,
        .pQueuePriorities = &priority_high
    };

    /* Transfer queue (only add if different family) */
    if (g_r.device.families.has_dedicated_transfer) {
        queue_infos[queue_info_count++] = (VkDeviceQueueCreateInfo){
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = g_r.device.families.transfer,
            .queueCount       = 1,
            .pQueuePriorities = &priority_low
        };
    }

    const char *device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkPhysicalDeviceFeatures device_features = {0};

    VkDeviceCreateInfo create_info = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = queue_info_count,
        .pQueueCreateInfos       = queue_infos,
        .enabledExtensionCount   = 1,
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures        = &device_features
    };

    VkResult result = vkCreateDevice(g_r.device.physical, &create_info, NULL, &g_r.device.handle);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed: %d\n", result);
        return QK_ERROR_VULKAN_INIT;
    }

    vkGetDeviceQueue(g_r.device.handle, g_r.device.families.graphics, 0, &g_r.device.graphics_queue);
    if (g_r.device.families.has_dedicated_transfer) {
        vkGetDeviceQueue(g_r.device.handle, g_r.device.families.transfer, 0, &g_r.device.transfer_queue);
    } else {
        g_r.device.transfer_queue = g_r.device.graphics_queue;
    }

    return QK_SUCCESS;
}

/* ---- Surface ---- */

qk_result_t r_vulkan_create_surface(void *sdl_window)
{
    if (!SDL_Vulkan_CreateSurface(sdl_window, g_r.instance.handle, NULL, &g_r.swapchain.surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return QK_ERROR_VULKAN_INIT;
    }
    return QK_SUCCESS;
}

/* ---- Swapchain ---- */

static VkSurfaceFormatKHR choose_surface_format(VkSurfaceFormatKHR *formats, u32 count)
{
    for (u32 i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return formats[i];
        }
    }
    for (u32 i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            return formats[i];
        }
    }
    return formats[0];
}

static VkPresentModeKHR choose_present_mode(VkPresentModeKHR *modes, u32 count, bool vsync)
{
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR;

    /* For competitive FPS: IMMEDIATE has lowest latency (allows tearing),
       MAILBOX is next best (no tearing, no vsync lock). FIFO is last resort. */
    bool has_immediate = false;
    bool has_mailbox = false;
    for (u32 i = 0; i < count; i++) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) has_immediate = true;
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)   has_mailbox = true;
    }

    if (has_immediate) {
        fprintf(stderr, "[Renderer] Present mode: IMMEDIATE (lowest latency)\n");
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (has_mailbox) {
        fprintf(stderr, "[Renderer] Present mode: MAILBOX\n");
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    fprintf(stderr, "[Renderer] Present mode: FIFO (vsync fallback)\n");
    return VK_PRESENT_MODE_FIFO_KHR;
}

qk_result_t r_vulkan_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_r.device.physical, g_r.swapchain.surface, &caps);

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_r.device.physical, g_r.swapchain.surface, &format_count, NULL);
    VkSurfaceFormatKHR *formats = malloc(format_count * sizeof(VkSurfaceFormatKHR));
    if (!formats) return QK_ERROR_OUT_OF_MEMORY;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_r.device.physical, g_r.swapchain.surface, &format_count, formats);

    u32 mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_r.device.physical, g_r.swapchain.surface, &mode_count, NULL);
    VkPresentModeKHR *modes = malloc(mode_count * sizeof(VkPresentModeKHR));
    if (!modes) { free(formats); return QK_ERROR_OUT_OF_MEMORY; }
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_r.device.physical, g_r.swapchain.surface, &mode_count, modes);

    VkSurfaceFormatKHR surface_format = choose_surface_format(formats, format_count);
    VkPresentModeKHR present_mode = choose_present_mode(modes, mode_count, g_r.config.vsync);

    free(formats);
    free(modes);

    /* Extent */
    VkExtent2D extent;
    if (caps.currentExtent.width != 0xFFFFFFFF) {
        extent = caps.currentExtent;
    } else {
        extent.width = g_r.config.window_width;
        extent.height = g_r.config.window_height;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }

    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }
    if (image_count > R_MAX_SWAPCHAIN_IMAGES) {
        image_count = R_MAX_SWAPCHAIN_IMAGES;
    }

    VkSwapchainCreateInfoKHR create_info = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = g_r.swapchain.surface,
        .minImageCount    = image_count,
        .imageFormat      = surface_format.format,
        .imageColorSpace  = surface_format.colorSpace,
        .imageExtent      = extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = present_mode,
        .clipped          = VK_TRUE,
        .oldSwapchain     = VK_NULL_HANDLE
    };

    VkResult result = vkCreateSwapchainKHR(g_r.device.handle, &create_info, NULL, &g_r.swapchain.handle);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateSwapchainKHR failed: %d\n", result);
        return QK_ERROR_SWAPCHAIN;
    }

    g_r.swapchain.format = surface_format.format;
    g_r.swapchain.extent = extent;

    /* Get swapchain images */
    vkGetSwapchainImagesKHR(g_r.device.handle, g_r.swapchain.handle, &g_r.swapchain.image_count, NULL);
    if (g_r.swapchain.image_count > R_MAX_SWAPCHAIN_IMAGES) {
        g_r.swapchain.image_count = R_MAX_SWAPCHAIN_IMAGES;
    }
    vkGetSwapchainImagesKHR(g_r.device.handle, g_r.swapchain.handle, &g_r.swapchain.image_count, g_r.swapchain.images);

    /* Create image views */
    for (u32 i = 0; i < g_r.swapchain.image_count; i++) {
        VkImageViewCreateInfo view_info = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = g_r.swapchain.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = g_r.swapchain.format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1
            }
        };

        result = vkCreateImageView(g_r.device.handle, &view_info, NULL, &g_r.swapchain.views[i]);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkCreateImageView failed for swapchain image %u\n", i);
            return QK_ERROR_SWAPCHAIN;
        }
    }

    return QK_SUCCESS;
}

void r_vulkan_destroy_swapchain(void)
{
    for (u32 i = 0; i < g_r.swapchain.image_count; i++) {
        if (g_r.swapchain.views[i]) {
            vkDestroyImageView(g_r.device.handle, g_r.swapchain.views[i], NULL);
            g_r.swapchain.views[i] = VK_NULL_HANDLE;
        }
    }
    if (g_r.swapchain.handle) {
        vkDestroySwapchainKHR(g_r.device.handle, g_r.swapchain.handle, NULL);
        g_r.swapchain.handle = VK_NULL_HANDLE;
    }
}

qk_result_t r_vulkan_recreate_swapchain(void)
{
    vkDeviceWaitIdle(g_r.device.handle);

    /* Destroy old compose framebuffers */
    for (u32 i = 0; i < R_MAX_SWAPCHAIN_IMAGES; i++) {
        if (g_r.compose_framebuffers[i]) {
            vkDestroyFramebuffer(g_r.device.handle, g_r.compose_framebuffers[i], NULL);
            g_r.compose_framebuffers[i] = VK_NULL_HANDLE;
        }
    }

    r_vulkan_destroy_swapchain();

    qk_result_t res = r_vulkan_create_swapchain();
    if (res != QK_SUCCESS) return res;

    /* Recreate compose framebuffers */
    for (u32 i = 0; i < g_r.swapchain.image_count; i++) {
        VkFramebufferCreateInfo fb_info = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = g_r.compose_render_pass,
            .attachmentCount = 1,
            .pAttachments    = &g_r.swapchain.views[i],
            .width           = g_r.swapchain.extent.width,
            .height          = g_r.swapchain.extent.height,
            .layers          = 1
        };
        VkResult vr = vkCreateFramebuffer(g_r.device.handle, &fb_info, NULL, &g_r.compose_framebuffers[i]);
        if (vr != VK_SUCCESS) return QK_ERROR_SWAPCHAIN;
    }

    g_r.swapchain_needs_recreate = false;
    return QK_SUCCESS;
}

/* ---- Render Targets ---- */

static VkFormat find_depth_format(void)
{
    VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT };
    for (u32 i = 0; i < 2; i++) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(g_r.device.physical, candidates[i], &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return candidates[i];
        }
    }
    return VK_FORMAT_D32_SFLOAT;
}

static qk_result_t create_image(u32 width, u32 height, VkFormat format,
                                VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                VkImage *out_image, VkDeviceMemory *out_memory,
                                VkImageView *out_view)
{
    VkImageCreateInfo img_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { width, height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkResult vr = vkCreateImage(g_r.device.handle, &img_info, NULL, out_image);
    if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(g_r.device.handle, *out_image, &mem_req);

    /* Try pool suballocation for device-local images */
    r_memory_pool_t *pool = &g_r.pools[R_MEMORY_POOL_DEVICE_LOCAL];
    if (pool->memory && (mem_req.memoryTypeBits & (1u << pool->memory_type))) {
        VkDeviceSize offset;
        if (r_memory_pool_alloc(R_MEMORY_POOL_DEVICE_LOCAL, mem_req.size,
                                 mem_req.alignment, &offset) == QK_SUCCESS) {
            vkBindImageMemory(g_r.device.handle, *out_image, pool->memory, offset);
            *out_memory = VK_NULL_HANDLE; /* Pool-owned */
            goto create_view;
        }
    }

    {
        u32 mem_type;
        if (!r_memory_find_type((u32)mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem_type)) {
            vkDestroyImage(g_r.device.handle, *out_image, NULL);
            return QK_ERROR_OUT_OF_MEMORY;
        }

        VkMemoryAllocateInfo alloc_info = {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = mem_req.size,
            .memoryTypeIndex = mem_type
        };
        vr = vkAllocateMemory(g_r.device.handle, &alloc_info, NULL, out_memory);
        if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

        vkBindImageMemory(g_r.device.handle, *out_image, *out_memory, 0);
    }

create_view:
    ;

    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = *out_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange = {
            .aspectMask     = aspect,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1
        }
    };
    vr = vkCreateImageView(g_r.device.handle, &view_info, NULL, out_view);
    if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

    return QK_SUCCESS;
}

static qk_result_t create_render_pass_world(VkRenderPass *out_pass, VkFormat depth_format)
{
    VkAttachmentDescription attachments[2] = {
        /* Color */
        {
            .format         = VK_FORMAT_R8G8B8A8_SRGB,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        },
        /* Depth */
        {
            .format         = depth_format,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        }
    };

    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkAttachmentReference depth_ref = {
        .attachment = 1,
        .layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &color_ref,
        .pDepthStencilAttachment = &depth_ref
    };

    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
    };

    VkRenderPassCreateInfo rp_info = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments    = attachments,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dep
    };

    VkResult vr = vkCreateRenderPass(g_r.device.handle, &rp_info, NULL, out_pass);
    return (vr == VK_SUCCESS) ? QK_SUCCESS : QK_ERROR_PIPELINE;
}

static qk_result_t create_render_pass_ui(VkRenderPass *out_pass)
{
    VkAttachmentDescription attachment = {
        .format         = VK_FORMAT_R8G8B8A8_SRGB,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_ref
    };

    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    };

    VkRenderPassCreateInfo rp_info = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &attachment,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dep
    };

    VkResult vr = vkCreateRenderPass(g_r.device.handle, &rp_info, NULL, out_pass);
    return (vr == VK_SUCCESS) ? QK_SUCCESS : QK_ERROR_PIPELINE;
}

static qk_result_t create_render_pass_compose(VkRenderPass *out_pass)
{
    VkAttachmentDescription attachment = {
        .format         = g_r.swapchain.format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };

    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_ref
    };

    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    };

    VkRenderPassCreateInfo rp_info = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &attachment,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dep
    };

    VkResult vr = vkCreateRenderPass(g_r.device.handle, &rp_info, NULL, out_pass);
    return (vr == VK_SUCCESS) ? QK_SUCCESS : QK_ERROR_PIPELINE;
}

qk_result_t r_create_render_targets(void)
{
    u32 rw = g_r.config.render_width;
    u32 rh = g_r.config.render_height;

    VkFormat depth_format = find_depth_format();

    /* World render pass */
    qk_result_t res = create_render_pass_world(&g_r.world_target.render_pass, depth_format);
    if (res != QK_SUCCESS) return res;

    /* World color */
    res = create_image(rw, rh, VK_FORMAT_R8G8B8A8_SRGB,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT,
                       &g_r.world_target.color_image, &g_r.world_target.color_memory,
                       &g_r.world_target.color_view);
    if (res != QK_SUCCESS) return res;

    /* World depth */
    res = create_image(rw, rh, depth_format,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT,
                       &g_r.world_target.depth_image, &g_r.world_target.depth_memory,
                       &g_r.world_target.depth_view);
    if (res != QK_SUCCESS) return res;

    g_r.world_target.extent = (VkExtent2D){ rw, rh };

    /* World framebuffer */
    VkImageView world_attachments[2] = { g_r.world_target.color_view, g_r.world_target.depth_view };
    VkFramebufferCreateInfo fb_info = {
        .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass      = g_r.world_target.render_pass,
        .attachmentCount = 2,
        .pAttachments    = world_attachments,
        .width           = rw,
        .height          = rh,
        .layers          = 1
    };
    VkResult vr = vkCreateFramebuffer(g_r.device.handle, &fb_info, NULL, &g_r.world_target.framebuffer);
    if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

    /* UI is drawn directly in the world render pass (no separate target needed) */

    /* Compose render pass */
    res = create_render_pass_compose(&g_r.compose_render_pass);
    if (res != QK_SUCCESS) return res;

    /* Compose framebuffers (one per swapchain image) */
    for (u32 i = 0; i < g_r.swapchain.image_count; i++) {
        VkFramebufferCreateInfo cfb_info = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = g_r.compose_render_pass,
            .attachmentCount = 1,
            .pAttachments    = &g_r.swapchain.views[i],
            .width           = g_r.swapchain.extent.width,
            .height          = g_r.swapchain.extent.height,
            .layers          = 1
        };
        vr = vkCreateFramebuffer(g_r.device.handle, &cfb_info, NULL, &g_r.compose_framebuffers[i]);
        if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;
    }

    return QK_SUCCESS;
}

void r_destroy_render_targets(void)
{
    VkDevice dev = g_r.device.handle;

    /* Compose framebuffers */
    for (u32 i = 0; i < R_MAX_SWAPCHAIN_IMAGES; i++) {
        if (g_r.compose_framebuffers[i]) {
            vkDestroyFramebuffer(dev, g_r.compose_framebuffers[i], NULL);
            g_r.compose_framebuffers[i] = VK_NULL_HANDLE;
        }
    }
    if (g_r.compose_render_pass) {
        vkDestroyRenderPass(dev, g_r.compose_render_pass, NULL);
        g_r.compose_render_pass = VK_NULL_HANDLE;
    }

    /* World target */
    if (g_r.world_target.framebuffer) vkDestroyFramebuffer(dev, g_r.world_target.framebuffer, NULL);
    if (g_r.world_target.color_view) vkDestroyImageView(dev, g_r.world_target.color_view, NULL);
    if (g_r.world_target.color_image) vkDestroyImage(dev, g_r.world_target.color_image, NULL);
    if (g_r.world_target.color_memory) vkFreeMemory(dev, g_r.world_target.color_memory, NULL);
    if (g_r.world_target.depth_view) vkDestroyImageView(dev, g_r.world_target.depth_view, NULL);
    if (g_r.world_target.depth_image) vkDestroyImage(dev, g_r.world_target.depth_image, NULL);
    if (g_r.world_target.depth_memory) vkFreeMemory(dev, g_r.world_target.depth_memory, NULL);
    if (g_r.world_target.render_pass) vkDestroyRenderPass(dev, g_r.world_target.render_pass, NULL);
    memset(&g_r.world_target, 0, sizeof(g_r.world_target));

    /* UI target */
    if (g_r.ui_target.framebuffer) vkDestroyFramebuffer(dev, g_r.ui_target.framebuffer, NULL);
    if (g_r.ui_target.color_view) vkDestroyImageView(dev, g_r.ui_target.color_view, NULL);
    if (g_r.ui_target.color_image) vkDestroyImage(dev, g_r.ui_target.color_image, NULL);
    if (g_r.ui_target.color_memory) vkFreeMemory(dev, g_r.ui_target.color_memory, NULL);
    if (g_r.ui_target.render_pass) vkDestroyRenderPass(dev, g_r.ui_target.render_pass, NULL);
    memset(&g_r.ui_target, 0, sizeof(g_r.ui_target));
}

/* ---- Descriptor Setup ---- */

qk_result_t r_descriptors_init(void)
{
    /* View set layout (set 0 for world pipeline) */
    {
        VkDescriptorSetLayoutBinding binding = {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
        };
        VkDescriptorSetLayoutCreateInfo info = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings    = &binding
        };
        vkCreateDescriptorSetLayout(g_r.device.handle, &info, NULL, &g_r.view_set_layout);
    }

    /* Texture set layout (set 1 for world, set 0 for UI) */
    {
        VkDescriptorSetLayoutBinding binding = {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
        };
        VkDescriptorSetLayoutCreateInfo info = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings    = &binding
        };
        vkCreateDescriptorSetLayout(g_r.device.handle, &info, NULL, &g_r.texture_set_layout);
    }

    /* Compose set layout */
    {
        VkDescriptorSetLayoutBinding bindings[2] = {
            {
                .binding         = 0,
                .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
            },
            {
                .binding         = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
            }
        };
        VkDescriptorSetLayoutCreateInfo info = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2,
            .pBindings    = bindings
        };
        vkCreateDescriptorSetLayout(g_r.device.handle, &info, NULL, &g_r.compose_set_layout);
    }

    /* Descriptor pool */
    {
        VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         16 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, R_MAX_TEXTURES + 16 }
        };
        VkDescriptorPoolCreateInfo pool_info = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets       = R_MAX_TEXTURES + 32,
            .poolSizeCount = 2,
            .pPoolSizes    = sizes
        };
        vkCreateDescriptorPool(g_r.device.handle, &pool_info, NULL, &g_r.descriptor_pool);
    }

    /* Compose sampler */
    {
        VkSamplerCreateInfo sampler_info = {
            .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter    = VK_FILTER_LINEAR,
            .minFilter    = VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .maxLod       = 1.0f
        };
        vkCreateSampler(g_r.device.handle, &sampler_info, NULL, &g_r.compose_sampler);
    }

    /* Allocate compose descriptor set */
    {
        VkDescriptorSetAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = g_r.descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &g_r.compose_set_layout
        };
        vkAllocateDescriptorSets(g_r.device.handle, &alloc_info, &g_r.compose_descriptor_set);
    }

    /* Allocate per-frame view descriptor sets */
    for (u32 i = 0; i < R_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSetAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = g_r.descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &g_r.view_set_layout
        };
        vkAllocateDescriptorSets(g_r.device.handle, &alloc_info, &g_r.frames[i].view_descriptor_set);
    }

    return QK_SUCCESS;
}

void r_descriptors_shutdown(void)
{
    VkDevice dev = g_r.device.handle;
    if (g_r.compose_sampler) vkDestroySampler(dev, g_r.compose_sampler, NULL);
    if (g_r.descriptor_pool) vkDestroyDescriptorPool(dev, g_r.descriptor_pool, NULL);
    if (g_r.view_set_layout) vkDestroyDescriptorSetLayout(dev, g_r.view_set_layout, NULL);
    if (g_r.texture_set_layout) vkDestroyDescriptorSetLayout(dev, g_r.texture_set_layout, NULL);
    if (g_r.compose_set_layout) vkDestroyDescriptorSetLayout(dev, g_r.compose_set_layout, NULL);
}

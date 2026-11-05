// Android bring-up: an SDL window cleared through a Vulkan swapchain, the colour cycling over time or set by the
// gamepad's face buttons. Proves the APK, SDL3, Vulkan and controller path before the game runs here.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

#define VK_CHECK(x)                                     \
    do {                                                \
        VkResult r_ = (x);                              \
        if (r_ != VK_SUCCESS) {                         \
            SDL_Log("vulkan: %s failed: %d", #x, r_);   \
            return false;                               \
        }                                               \
    } while (0)

struct Vk {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
};

bool CreateInstance(Vk& vk) {
    Uint32 n = 0;
    const char* const* ext = SDL_Vulkan_GetInstanceExtensions(&n);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SCDA";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = ext;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &vk.instance));
    return true;
}

// the first GPU with a queue family that draws and presents to the surface
bool CreateDevice(Vk& vk) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(vk.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(vk.instance, &n, gpus.data());
    for (VkPhysicalDevice gpu : gpus) {
        uint32_t m = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &m, nullptr);
        std::vector<VkQueueFamilyProperties> families(m);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &m, families.data());
        for (uint32_t i = 0; i < m; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, vk.surface, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                vk.gpu = gpu;
                vk.family = i;
                break;
            }
        }
        if (vk.gpu) break;
    }
    if (!vk.gpu) {
        SDL_Log("vulkan: no GPU presents to the window");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.gpu, &props);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(vk.gpu, &features);
    SDL_Log("vulkan: %s, API %u.%u, BC textures %s", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion), features.textureCompressionBC ? "yes" : "no");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vk.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = &ext;
    VK_CHECK(vkCreateDevice(vk.gpu, &ci, nullptr, &vk.device));
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vk.family;
    VK_CHECK(vkCreateCommandPool(vk.device, &pci, nullptr, &vk.pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = vk.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &ai, &vk.cmd));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(vk.device, &sci, nullptr, &vk.acquired));
    VK_CHECK(vkCreateSemaphore(vk.device, &sci, nullptr, &vk.rendered));
    return true;
}

bool CreateSwapchain(Vk& vk, SDL_Window* window) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps));
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &n, formats.data());
    if (formats.empty()) return false;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent = {uint32_t(w), uint32_t(h)};
    }
    // Android surfaces often offer only INHERIT
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    uint32_t count = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = vk.surface;
    ci.minImageCount = count;
    ci.imageFormat = formats[0].format;
    ci.imageColorSpace = formats[0].colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // the panel's own orientation spares the compositor a rotation pass; a renderer then rotates its final blit

    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = vk.swapchain;
    VkSwapchainKHR swapchain;
    VK_CHECK(vkCreateSwapchainKHR(vk.device, &ci, nullptr, &swapchain));
    if (vk.swapchain) vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);
    vk.swapchain = swapchain;

    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &n, nullptr);
    vk.images.resize(n);
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &n, vk.images.data());
    SDL_Log("vulkan: swapchain %ux%u, %u images, format %d, transform %d", extent.width, extent.height, n,
            formats[0].format, caps.currentTransform);
    return true;
}

bool CreateSurface(Vk& vk, SDL_Window* window) {
    if (!SDL_Vulkan_CreateSurface(window, vk.instance, nullptr, &vk.surface)) {
        SDL_Log("vulkan: no surface: %s", SDL_GetError());
        return false;
    }
    return true;
}

// the Android window goes away in the background: the swapchain and surface with it
void DestroySurface(Vk& vk) {
    vkDeviceWaitIdle(vk.device);
    vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);
    vk.swapchain = VK_NULL_HANDLE;
    vk.images.clear();
    vkDestroySurfaceKHR(vk.instance, vk.surface, nullptr);
    vk.surface = VK_NULL_HANDLE;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// false: the swapchain is out of date
bool Frame(Vk& vk, const float rgb[3]) {
    uint32_t i;
    VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.acquired, VK_NULL_HANDLE, &i);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(vk.cmd, 0);
    vkBeginCommandBuffer(vk.cmd, &bi);
    Barrier(vk.cmd, vk.images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue color{{rgb[0], rgb[1], rgb[2], 1.0f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(vk.cmd, vk.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    Barrier(vk.cmd, vk.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(vk.cmd);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &vk.acquired;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &vk.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &vk.rendered;
    vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &vk.swapchain;
    pi.pImageIndices = &i;
    r = vkQueuePresentKHR(vk.queue, &pi);
    // shortcut: one frame in flight, fine for a clear, the game's backend needs frames in flight
    vkQueueWaitIdle(vk.queue);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

}  // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("SCDA", 0, 0, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
    Vk vk;
    if (!window || !CreateInstance(vk) || !CreateSurface(vk, window) || !CreateDevice(vk) ||
        !CreateSwapchain(vk, window)) {
        SDL_Log("bring-up failed: %s", SDL_GetError());
        return 1;
    }

    float fixed[3] = {};
    bool use_fixed = false, background = false;
    for (bool running = true; running;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                background = true;
                if (vk.surface) DestroySurface(vk);
                break;
            case SDL_EVENT_DID_ENTER_FOREGROUND: background = false; break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) SDL_Log("gamepad: %s", SDL_GetGamepadName(pad));
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                static const float kColors[][3] = {{0, 0.8f, 0}, {0.8f, 0, 0}, {0, 0, 0.8f}, {0.8f, 0.8f, 0}};
                SDL_Log("gamepad: button %d", e.gbutton.button);
                use_fixed = e.gbutton.button <= SDL_GAMEPAD_BUTTON_NORTH;  // A B X Y, anything else cycles again
                if (use_fixed) std::copy_n(kColors[e.gbutton.button], 3, fixed);
                break;
            }
            }
        }
        // no window: in the background, or SDL has not handed over the new one yet after DID_ENTER_FOREGROUND
        if (!vk.surface && (background || !CreateSurface(vk, window) || !CreateSwapchain(vk, window))) {
            if (vk.surface) DestroySurface(vk);
            SDL_Delay(50);
            continue;
        }
        float t = SDL_GetTicks() / 1000.0f;
        float cycle[3] = {0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t + 2.1f), 0.5f + 0.5f * std::sin(t + 4.2f)};
        if (!Frame(vk, use_fixed ? fixed : cycle)) {
            vkDeviceWaitIdle(vk.device);
            // the window can go before WILL_ENTER_BACKGROUND arrives: the loop above waits for the new one
            if (!CreateSwapchain(vk, window)) DestroySurface(vk);
        }
    }
    vkDeviceWaitIdle(vk.device);
    SDL_Quit();
    return 0;
}

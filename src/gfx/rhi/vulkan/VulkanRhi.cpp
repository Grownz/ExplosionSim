#include "gfx/rhi/vulkan/VulkanRhi.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ex::rhi::vulkan {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LogWarn("Vulkan: %s", data && data->pMessage ? data->pMessage : "(null)");
    }
    return VK_FALSE;
}

bool HasInstanceLayer(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(layers.begin(), layers.end(), [name](const auto& layer) {
        return std::strcmp(layer.layerName, name) == 0;
    });
}

uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t typeBits,
                        VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return std::numeric_limits<uint32_t>::max();
}

bool CreateImage(VkPhysicalDevice physical, VkDevice device, uint32_t width,
                 uint32_t height, VkFormat format, VkImageUsageFlags usage,
                 VkImage& image, VkDeviceMemory& memory) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    const uint32_t type = FindMemoryType(physical, requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }
    return vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS;
}

VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format,
                            VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return view;
}

} // namespace

Device::~Device() {
    Shutdown();
}

bool Device::Initialize(const DeviceDesc& desc) {
    desc_ = desc;
    offscreen_ = desc.offscreen;
    width_ = std::max(1, desc.width);
    height_ = std::max(1, desc.height);

    if (!CreateInstance()) {
        return false;
    }
    if (!offscreen_ && !CreateSurface()) {
        return false;
    }
    if (!PickPhysicalDevice()) {
        return false;
    }
    if (!CreateDevice()) {
        return false;
    }
    if (!CreateCommandResources()) {
        return false;
    }

    if (offscreen_) {
        if (!CreateRenderPass() || !CreateOffscreenTarget()) {
            return false;
        }
    } else {
        if (!CreateSwapchain() || !CreateRenderPass() || !CreateFramebuffers()) {
            return false;
        }
    }

    initialized_ = true;
    LogInfo("Vulkan device: %s (%s, %dx%d)", adapterName_.c_str(),
            offscreen_ ? "offscreen" : "swapchain", width_, height_);
    return true;
}

void Device::Shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
    }
    if (device_) {
        DestroySwapchain();
        DestroyOffscreenTarget();
        if (renderPass_) {
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }
        if (imageAvailable_) {
            vkDestroySemaphore(device_, imageAvailable_, nullptr);
            imageAvailable_ = VK_NULL_HANDLE;
        }
        if (renderFinished_) {
            vkDestroySemaphore(device_, renderFinished_, nullptr);
            renderFinished_ = VK_NULL_HANDLE;
        }
        if (fence_) {
            vkDestroyFence(device_, fence_, nullptr);
            fence_ = VK_NULL_HANDLE;
        }
        if (commandPool_) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (debugMessenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) {
            destroy(instance_, debugMessenger_, nullptr);
        }
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
}

bool Device::CreateInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ExplosionSim";
    app.applicationVersion = VK_MAKE_VERSION(0, 10, 5);
    app.pEngineName = "ExplosionSim";
    app.engineVersion = VK_MAKE_VERSION(0, 10, 5);
    app.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> extensions;
    if (desc_.surface.instanceExtensions) {
        for (uint32_t i = 0; i < desc_.surface.instanceExtensionCount; ++i) {
            extensions.push_back(desc_.surface.instanceExtensions[i]);
        }
    }

    std::vector<const char*> layers;
    if (desc_.debug) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (HasInstanceLayer(kValidationLayer)) {
            layers.push_back(kValidationLayer);
        } else {
            LogWarn("Vulkan validation layer is not available.");
        }
    }

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = uint32_t(layers.size());
    info.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) {
        LogError("Vulkan instance creation failed.");
        return false;
    }

    if (desc_.debug) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT messenger{};
            messenger.sType =
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messenger.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messenger.pfnUserCallback = DebugCallback;
            create(instance_, &messenger, nullptr, &debugMessenger_);
        }
    }
    return true;
}

bool Device::CreateSurface() {
    if (!desc_.surface.create) {
        LogError("Vulkan surface provider is missing.");
        return false;
    }
    uint64_t surface = 0;
    if (!desc_.surface.create(desc_.surface.user, instance_, &surface) ||
        surface == 0) {
        LogError("Vulkan surface creation failed.");
        return false;
    }
    surface_ = reinterpret_cast<VkSurfaceKHR>(surface);
    return true;
}

bool Device::PickPhysicalDevice() {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        LogError("No Vulkan physical device found.");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                                                 families.data());

        bool graphics = false;
        bool present = offscreen_;
        uint32_t graphicsFamily = 0;
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphics = true;
                graphicsFamily = i;
                if (!offscreen_) {
                    VkBool32 supported = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_,
                                                        &supported);
                    present = supported == VK_TRUE;
                }
                if (graphics && present) {
                    break;
                }
            }
        }
        if (!graphics || !present) {
            continue;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                        ? 1000
                        : (properties.deviceType ==
                                   VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                               ? 500
                               : 100);
        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = candidate;
            queueFamily_ = graphicsFamily;
            adapterName_ = properties.deviceName;

            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(candidate, &memory);
            dedicatedVideoMemory_ = 0;
            for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
                if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    dedicatedVideoMemory_ += memory.memoryHeaps[i].size;
                }
            }
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        LogError("No suitable Vulkan device with graphics/present support.");
        return false;
    }
    return true;
}

bool Device::CreateDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    std::vector<const char*> extensions;
    if (!offscreen_) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateDevice(physicalDevice_, &info, nullptr, &device_) != VK_SUCCESS) {
        LogError("Vulkan logical device creation failed.");
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return true;
}

bool Device::CreateCommandResources() {
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &pool, nullptr, &commandPool_) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = commandPool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocate, &commandBuffer_) != VK_SUCCESS) {
        return false;
    }

    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device_, &fence, nullptr, &fence_) != VK_SUCCESS) {
        return false;
    }

    if (!offscreen_) {
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphore, nullptr, &imageAvailable_) !=
                VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphore, nullptr, &renderFinished_) !=
                VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

VkFormat Device::ChooseSurfaceFormat() const {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &count,
                                         formats.data());
    for (const auto& format : formats) {
        if ((format.format == VK_FORMAT_B8G8R8A8_UNORM ||
             format.format == VK_FORMAT_B8G8R8A8_SRGB) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format.format;
        }
    }
    return formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats.front().format;
}

VkExtent2D Device::ChooseExtent(const VkSurfaceCapabilitiesKHR& caps) const {
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return caps.currentExtent;
    }
    VkExtent2D extent{uint32_t(width_), uint32_t(height_)};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width,
                              caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height,
                               caps.maxImageExtent.height);
    return extent;
}

bool Device::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_,
                                                  &caps) != VK_SUCCESS) {
        return false;
    }
    swapchainFormat_ = ChooseSurfaceFormat();
    const VkExtent2D extent = ChooseExtent(caps);
    width_ = int(extent.width);
    height_ = int(extent.height);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = swapchainFormat_;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) {
        LogError("Vulkan swapchain creation failed.");
        return false;
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchainImages_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchainImages_.data());

    swapchainViews_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        swapchainViews_[i] = CreateImageView(device_, swapchainImages_[i],
                                             swapchainFormat_,
                                             VK_IMAGE_ASPECT_COLOR_BIT);
        if (swapchainViews_[i] == VK_NULL_HANDLE) {
            return false;
        }
    }
    return true;
}

bool Device::CreateRenderPass() {
    VkAttachmentDescription attachments[2]{};
    attachments[0].format =
        offscreen_ ? VK_FORMAT_B8G8R8A8_UNORM : swapchainFormat_;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = offscreen_
                                     ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                     : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &info, nullptr, &renderPass_) != VK_SUCCESS) {
        LogError("Vulkan render pass creation failed.");
        return false;
    }
    return true;
}

bool Device::CreateFramebuffers() {
    if (!depthImage_) {
        if (!CreateImage(physicalDevice_, device_, uint32_t(width_),
                         uint32_t(height_), VK_FORMAT_D32_SFLOAT,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depthImage_,
                         depthMemory_)) {
            return false;
        }
        depthView_ = CreateImageView(device_, depthImage_, VK_FORMAT_D32_SFLOAT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT);
        if (depthView_ == VK_NULL_HANDLE) {
            return false;
        }
    }

    framebuffers_.resize(swapchainViews_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < swapchainViews_.size(); ++i) {
        VkImageView views[2] = {swapchainViews_[i], depthView_};
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 2;
        info.pAttachments = views;
        info.width = uint32_t(width_);
        info.height = uint32_t(height_);
        info.layers = 1;
        if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]) !=
            VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool Device::CreateOffscreenTarget() {
    if (!CreateImage(physicalDevice_, device_, uint32_t(width_), uint32_t(height_),
                     VK_FORMAT_B8G8R8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     colorImage_, colorMemory_)) {
        return false;
    }
    colorView_ = CreateImageView(device_, colorImage_, VK_FORMAT_B8G8R8A8_UNORM,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
    if (colorView_ == VK_NULL_HANDLE) {
        return false;
    }

    if (!CreateImage(physicalDevice_, device_, uint32_t(width_), uint32_t(height_),
                     VK_FORMAT_D32_SFLOAT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depthImage_,
                     depthMemory_)) {
        return false;
    }
    depthView_ = CreateImageView(device_, depthImage_, VK_FORMAT_D32_SFLOAT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT);
    if (depthView_ == VK_NULL_HANDLE) {
        return false;
    }

    VkImageView views[2] = {colorView_, depthView_};
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = renderPass_;
    info.attachmentCount = 2;
    info.pAttachments = views;
    info.width = uint32_t(width_);
    info.height = uint32_t(height_);
    info.layers = 1;
    if (vkCreateFramebuffer(device_, &info, nullptr, &offscreenFramebuffer_) !=
        VK_SUCCESS) {
        return false;
    }
    return true;
}

void Device::DestroySwapchain() {
    for (VkFramebuffer framebuffer : framebuffers_) {
        if (framebuffer) {
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }
    }
    framebuffers_.clear();
    for (VkImageView view : swapchainViews_) {
        if (view) {
            vkDestroyImageView(device_, view, nullptr);
        }
    }
    swapchainViews_.clear();
    swapchainImages_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    if (depthView_) {
        vkDestroyImageView(device_, depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
    }
    if (depthImage_) {
        vkDestroyImage(device_, depthImage_, nullptr);
        depthImage_ = VK_NULL_HANDLE;
    }
    if (depthMemory_) {
        vkFreeMemory(device_, depthMemory_, nullptr);
        depthMemory_ = VK_NULL_HANDLE;
    }
}

void Device::DestroyOffscreenTarget() {
    if (offscreenFramebuffer_) {
        vkDestroyFramebuffer(device_, offscreenFramebuffer_, nullptr);
        offscreenFramebuffer_ = VK_NULL_HANDLE;
    }
    if (depthView_) {
        vkDestroyImageView(device_, depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
    }
    if (depthImage_) {
        vkDestroyImage(device_, depthImage_, nullptr);
        depthImage_ = VK_NULL_HANDLE;
    }
    if (depthMemory_) {
        vkFreeMemory(device_, depthMemory_, nullptr);
        depthMemory_ = VK_NULL_HANDLE;
    }
    if (colorView_) {
        vkDestroyImageView(device_, colorView_, nullptr);
        colorView_ = VK_NULL_HANDLE;
    }
    if (colorImage_) {
        vkDestroyImage(device_, colorImage_, nullptr);
        colorImage_ = VK_NULL_HANDLE;
    }
    if (colorMemory_) {
        vkFreeMemory(device_, colorMemory_, nullptr);
        colorMemory_ = VK_NULL_HANDLE;
    }
}

bool Device::WaitForFence() {
    if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return false;
    }
    vkResetFences(device_, 1, &fence_);
    return true;
}

bool Device::BeginCommandBuffer() {
    vkResetCommandBuffer(commandBuffer_, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(commandBuffer_, &begin) == VK_SUCCESS;
}

void Device::BeginFrame(float r, float g, float b, float a) {
    if (!initialized_ || frameActive_) {
        return;
    }

    // Wait for the previous submission before acquiring the next image: this
    // guarantees the imageAvailable semaphore has been consumed.
    if (!WaitForFence()) {
        return;
    }

    if (!offscreen_) {
        const VkResult result = vkAcquireNextImageKHR(
            device_, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE,
            &imageIndex_);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain();
            return;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return;
        }
    }

    if (!BeginCommandBuffer()) {
        return;
    }

    VkClearValue clears[2]{};
    clears[0].color = {{r, g, b, a}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = renderPass_;
    info.framebuffer =
        offscreen_ ? offscreenFramebuffer_ : framebuffers_[imageIndex_];
    info.renderArea.extent = {uint32_t(width_), uint32_t(height_)};
    info.clearValueCount = 2;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(commandBuffer_, &info, VK_SUBPASS_CONTENTS_INLINE);
    frameActive_ = true;
}

void Device::EndFrame(bool vsync) {
    (void)vsync;
    if (!frameActive_) {
        return;
    }
    vkCmdEndRenderPass(commandBuffer_);
    vkEndCommandBuffer(commandBuffer_);
    frameActive_ = false;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (!offscreen_) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailable_;
        submit.pWaitDstStageMask = &waitStage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished_;
    }

    if (vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS) {
        return;
    }

    if (offscreen_) {
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        return;
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex_;
    const VkResult result = vkQueuePresentKHR(queue_, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain();
    }
}

bool Device::ReadBack(std::vector<uint8_t>& bgra, int& outWidth, int& outHeight) {
    if (!offscreen_ || !initialized_) {
        return false;
    }
    WaitIdle();

    const VkDeviceSize size = VkDeviceSize(width_) * VkDeviceSize(height_) * 4;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    const uint32_t type = FindMemoryType(
        physicalDevice_, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == std::numeric_limits<uint32_t>::max()) {
        vkDestroyBuffer(device_, buffer, nullptr);
        return false;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &allocate, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        return false;
    }

    if (!WaitForFence() || !BeginCommandBuffer()) {
        vkDestroyBuffer(device_, buffer, nullptr);
        vkFreeMemory(device_, memory, nullptr);
        return false;
    }

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {uint32_t(width_), uint32_t(height_), 1};
    vkCmdCopyImageToBuffer(commandBuffer_, colorImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                           &region);
    vkEndCommandBuffer(commandBuffer_);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    if (vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS ||
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        vkFreeMemory(device_, memory, nullptr);
        return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device_, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        vkFreeMemory(device_, memory, nullptr);
        return false;
    }
    bgra.resize(size_t(size));
    std::memcpy(bgra.data(), mapped, size_t(size));
    vkUnmapMemory(device_, memory);

    vkDestroyBuffer(device_, buffer, nullptr);
    vkFreeMemory(device_, memory, nullptr);

    outWidth = width_;
    outHeight = height_;
    return true;
}

bool Device::RecreateSwapchain() {
    if (offscreen_) {
        return false;
    }
    WaitIdle();
    DestroySwapchain();
    if (renderPass_) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    return CreateSwapchain() && CreateRenderPass() && CreateFramebuffers();
}

bool Device::Resize(int width, int height) {
    if (!initialized_) {
        return false;
    }
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    if (offscreen_) {
        WaitIdle();
        DestroyOffscreenTarget();
        return CreateOffscreenTarget();
    }
    return RecreateSwapchain();
}

void Device::WaitIdle() {
    if (device_) {
        vkDeviceWaitIdle(device_);
    }
}

std::unique_ptr<Buffer> Device::CreateBuffer(const BufferDesc&) {
    return nullptr;
}

std::unique_ptr<Texture> Device::CreateTexture(const TextureDesc&) {
    return nullptr;
}

std::unique_ptr<Shader> Device::CreateShader(const ShaderDesc&) {
    return nullptr;
}

std::unique_ptr<Sampler> Device::CreateSampler(const SamplerDesc&) {
    return nullptr;
}

std::unique_ptr<Pipeline> Device::CreatePipeline(const PipelineDesc&) {
    return nullptr;
}

void Device::BindPipeline(Pipeline*) {}

} // namespace ex::rhi::vulkan

namespace ex::rhi {

std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc) {
    auto device = std::make_unique<vulkan::Device>();
    if (!device->Initialize(desc)) {
        return nullptr;
    }
    return device;
}

} // namespace ex::rhi

#pragma once

#include "gfx/rhi/Rhi.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ex::rhi::vulkan {

// Vulkan implementation of the RHI. M2 covers the device, swapchain/offscreen
// target, clear, present and readback. Buffers, textures, shaders and pipelines
// are added in M3.
class Device final : public rhi::Device {
public:
    Device() = default;
    ~Device() override;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool Initialize(const DeviceDesc& desc);
    void Shutdown();

    Backend GetBackend() const override { return Backend::Vulkan; }
    std::unique_ptr<Buffer> CreateBuffer(const BufferDesc& desc) override;
    std::unique_ptr<Texture> CreateTexture(const TextureDesc& desc) override;
    std::unique_ptr<Shader> CreateShader(const ShaderDesc& desc) override;
    std::unique_ptr<Sampler> CreateSampler(const SamplerDesc& desc) override;
    std::unique_ptr<Pipeline> CreatePipeline(const PipelineDesc& desc) override;
    std::unique_ptr<DescriptorSet> CreateDescriptorSet(Pipeline* pipeline) override;
    void BindPipeline(Pipeline* pipeline) override;
    void BindDescriptorSet(DescriptorSet* set, uint32_t index = 0) override;
    bool Resize(int width, int height) override;
    void WaitIdle() override;
    void* NativeHandle() const override { return device_; }

    // Clears the current frame target. EndFrame presents it (windowed) or
    // leaves it ready for ReadBack() (offscreen).
    void BeginFrame(float r, float g, float b, float a);
    void EndFrame(bool vsync);

    // Temporary immediate-mode draw path used by the M3 bring-up tests. The
    // command-list abstraction replaces these calls later.
    void BindVertexBuffer(Buffer* buffer, uint64_t offset = 0);
    void Draw(uint32_t vertexCount, uint32_t firstVertex = 0);

    // Offscreen only: copies the last rendered BGRA8 image into `bgra`.
    bool ReadBack(std::vector<uint8_t>& bgra, int& outWidth, int& outHeight);

    int Width() const { return width_; }
    int Height() const { return height_; }
    bool Offscreen() const { return offscreen_; }
    const std::string& AdapterName() const { return adapterName_; }
    uint64_t DedicatedVideoMemory() const { return dedicatedVideoMemory_; }

private:
    bool CreateInstance();
    bool CreateSurface();
    bool PickPhysicalDevice();
    bool CreateDevice();
    bool CreateCommandResources();
    bool CreateSwapchain();
    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateOffscreenTarget();
    void DestroySwapchain();
    void DestroyOffscreenTarget();
    bool RecreateSwapchain();

    bool BeginCommandBuffer();
    bool WaitForFence();
    bool ExecuteImmediate(const std::function<void(VkCommandBuffer)>& record);

    VkFormat ChooseSurfaceFormat() const;
    VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<VkFramebuffer> framebuffers_;
    uint32_t imageIndex_ = 0;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkImage colorImage_ = VK_NULL_HANDLE;
    VkDeviceMemory colorMemory_ = VK_NULL_HANDLE;
    VkImageView colorView_ = VK_NULL_HANDLE;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkFramebuffer offscreenFramebuffer_ = VK_NULL_HANDLE;

    VkPipeline boundPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout boundPipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet boundDescriptorSet_ = VK_NULL_HANDLE;
    VkBuffer boundVertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceSize boundVertexOffset_ = 0;

    DeviceDesc desc_{};
    std::string adapterName_ = "unknown";
    uint64_t dedicatedVideoMemory_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool offscreen_ = false;
    bool initialized_ = false;
    bool frameActive_ = false;
};

} // namespace ex::rhi::vulkan

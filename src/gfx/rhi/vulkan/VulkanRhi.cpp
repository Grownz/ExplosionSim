#include "gfx/rhi/vulkan/VulkanRhi.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace ex::rhi::vulkan {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* message =
        data && data->pMessage ? data->pMessage : "(null)";
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LogError("Vulkan: %s", message);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LogWarn("Vulkan: %s", message);
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

VkFormat ToVkFormat(Format format) {
    switch (format) {
    case Format::B8G8R8A8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::R8G8B8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::R8G8B8A8_UNORM_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::R16G16B16A16_FLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::R32G32B32A32_FLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::R32G32_FLOAT:
        return VK_FORMAT_R32G32_SFLOAT;
    case Format::R32_FLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case Format::R32_TYPELESS:
        return VK_FORMAT_D32_SFLOAT;
    case Format::D32_FLOAT:
        return VK_FORMAT_D32_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

VkFormat ToVkVertexFormat(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float:
        return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::Float2:
        return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::UInt:
        return VK_FORMAT_R32_UINT;
    case VertexFormat::UInt2:
        return VK_FORMAT_R32G32_UINT;
    case VertexFormat::UInt3:
        return VK_FORMAT_R32G32B32_UINT;
    case VertexFormat::UInt4:
        return VK_FORMAT_R32G32B32A32_UINT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

VkBufferUsageFlags ToVkBufferUsage(BufferUsage usage) {
    VkBufferUsageFlags flags = 0;
    if (HasUsage(usage, BufferUsage::Vertex)) {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (HasUsage(usage, BufferUsage::Index)) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (HasUsage(usage, BufferUsage::Uniform)) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (HasUsage(usage, BufferUsage::Storage) ||
        HasUsage(usage, BufferUsage::ShaderRead)) {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (HasUsage(usage, BufferUsage::Indirect)) {
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (HasUsage(usage, BufferUsage::Staging)) {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    return flags;
}

VkImageUsageFlags ToVkTextureUsage(TextureUsage usage) {
    VkImageUsageFlags flags = 0;
    if (HasUsage(usage, TextureUsage::Sampled)) {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (HasUsage(usage, TextureUsage::Storage)) {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (HasUsage(usage, TextureUsage::RenderTarget)) {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (HasUsage(usage, TextureUsage::DepthStencil)) {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (HasUsage(usage, TextureUsage::Staging)) {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return flags;
}

VkCompareOp ToVkCompare(CompareOp op) {
    switch (op) {
    case CompareOp::Never:
        return VK_COMPARE_OP_NEVER;
    case CompareOp::Less:
        return VK_COMPARE_OP_LESS;
    case CompareOp::Equal:
        return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessEqual:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater:
        return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual:
        return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterEqual:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always:
        return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS_OR_EQUAL;
}

VkCullModeFlags ToVkCull(CullMode mode) {
    switch (mode) {
    case CullMode::None:
        return VK_CULL_MODE_NONE;
    case CullMode::Front:
        return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back:
        return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

VkPrimitiveTopology ToVkTopology(PrimitiveTopology topology) {
    switch (topology) {
    case PrimitiveTopology::TriangleList:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::PointList:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkDescriptorType ToVkDescriptorType(DescriptorType type) {
    switch (type) {
    case DescriptorType::UniformBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case DescriptorType::StorageBuffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorType::SampledImage:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case DescriptorType::Sampler:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    case DescriptorType::StorageImage:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

VkShaderStageFlags ToVkStage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Pixel:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute:
        return VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return VK_SHADER_STAGE_FRAGMENT_BIT;
}

VkShaderStageFlags ToVkStage(const DescriptorBinding& binding) {
    if (binding.stageMask == 0) {
        return ToVkStage(binding.stage);
    }
    VkShaderStageFlags flags = 0;
    if (binding.stageMask & 1u) {
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (binding.stageMask & 2u) {
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (binding.stageMask & 4u) {
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return flags;
}

class BufferImpl final : public rhi::Buffer {
public:
    BufferImpl(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
               VkDeviceSize size, bool hostVisible)
        : device_(device), buffer_(buffer), memory_(memory), size_(size),
          hostVisible_(hostVisible) {}
    ~BufferImpl() override {
        Unmap();
        if (buffer_) {
            vkDestroyBuffer(device_, buffer_, nullptr);
        }
        if (memory_) {
            vkFreeMemory(device_, memory_, nullptr);
        }
    }

    uint64_t Size() const override { return size_; }

    void* Map() override {
        if (!hostVisible_) {
            return nullptr;
        }
        if (mapped_) {
            return mapped_;
        }
        if (vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_) !=
            VK_SUCCESS) {
            mapped_ = nullptr;
        }
        return mapped_;
    }

    void Unmap() override {
        if (mapped_) {
            vkUnmapMemory(device_, memory_);
            mapped_ = nullptr;
        }
    }

    void* NativeHandle() const override {
        return reinterpret_cast<void*>(uintptr_t(buffer_));
    }
    void* NativeShaderResourceView() const override { return nullptr; }

    VkBuffer Handle() const { return buffer_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    bool hostVisible_ = false;
    void* mapped_ = nullptr;
};

class TextureImpl final : public rhi::Texture {
public:
    TextureImpl(Device* owner, VkDevice device, VkImage image,
                VkDeviceMemory memory, VkImageView view, VkFormat format,
                const TextureDesc& desc)
        : owner_(owner), device_(device), image_(image), memory_(memory),
          view_(view), format_(format), desc_(desc) {}
    ~TextureImpl() override {
        if (view_) {
            vkDestroyImageView(device_, view_, nullptr);
        }
        if (image_) {
            vkDestroyImage(device_, image_, nullptr);
        }
        if (memory_) {
            vkFreeMemory(device_, memory_, nullptr);
        }
    }

    uint32_t Width() const override { return desc_.width; }
    uint32_t Height() const override { return desc_.height; }
    uint32_t Depth() const override { return desc_.depth; }
    Format GetFormat() const override { return desc_.format; }
    void* NativeHandle() const override {
        return reinterpret_cast<void*>(uintptr_t(image_));
    }
    void* NativeShaderResourceView() const override {
        return reinterpret_cast<void*>(uintptr_t(view_));
    }
    void* NativeRenderTargetView() const override {
        return reinterpret_cast<void*>(uintptr_t(view_));
    }
    void* NativeDepthStencilView() const override {
        return reinterpret_cast<void*>(uintptr_t(view_));
    }

    bool Update(const void* data) override;

    VkImage Handle() const { return image_; }
    VkImageView View() const { return view_; }
    const TextureDesc& Desc() const { return desc_; }

    // Layout the image is expected to be in for the next descriptor write. The
    // render pass helpers do not update it; textures used through
    // TransitionTexture track their own layout here so sampled-image
    // descriptors match the actual layout.
    VkImageLayout Layout() const { return layout_; }
    void SetLayout(VkImageLayout layout) { layout_ = layout; }

private:
    Device* owner_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    TextureDesc desc_{};
};

class SamplerImpl final : public rhi::Sampler {
public:
    SamplerImpl(VkDevice device, VkSampler sampler)
        : device_(device), sampler_(sampler) {}
    ~SamplerImpl() override {
        if (sampler_) {
            vkDestroySampler(device_, sampler_, nullptr);
        }
    }
    void* NativeHandle() const override {
        return reinterpret_cast<void*>(uintptr_t(sampler_));
    }
    VkSampler Handle() const { return sampler_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

class ShaderImpl final : public rhi::Shader {
public:
    ShaderImpl(VkDevice device, VkShaderModule module, ShaderStage stage,
               const char* entryPoint)
        : device_(device), module_(module), stage_(stage),
          entryPoint_(entryPoint ? entryPoint : "main") {}
    ~ShaderImpl() override {
        if (module_) {
            vkDestroyShaderModule(device_, module_, nullptr);
        }
    }
    ShaderStage Stage() const override { return stage_; }
    void* NativeHandle() const override {
        return reinterpret_cast<void*>(uintptr_t(module_));
    }
    VkShaderModule Module() const { return module_; }
    const char* EntryPoint() const { return entryPoint_.c_str(); }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
    ShaderStage stage_ = ShaderStage::Vertex;
    std::string entryPoint_;
};

class PipelineImpl final : public rhi::Pipeline {
public:
    PipelineImpl(VkDevice device, VkPipeline pipeline, VkPipelineLayout layout,
                 VkDescriptorSetLayout descriptorLayout,
                 std::vector<DescriptorBinding> bindings)
        : device_(device), pipeline_(pipeline), layout_(layout),
          descriptorLayout_(descriptorLayout), bindings_(std::move(bindings)) {}
    ~PipelineImpl() override {
        if (pipeline_) {
            vkDestroyPipeline(device_, pipeline_, nullptr);
        }
        if (layout_) {
            vkDestroyPipelineLayout(device_, layout_, nullptr);
        }
        if (descriptorLayout_) {
            vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
        }
    }
    void* NativeHandle() const override {
        return reinterpret_cast<void*>(uintptr_t(pipeline_));
    }

    VkPipeline Pipeline() const { return pipeline_; }
    VkPipelineLayout Layout() const { return layout_; }
    VkDescriptorSetLayout DescriptorLayout() const { return descriptorLayout_; }
    const std::vector<DescriptorBinding>& Bindings() const { return bindings_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    std::vector<DescriptorBinding> bindings_;
};

class DescriptorSetImpl final : public rhi::DescriptorSet {
public:
    DescriptorSetImpl(VkDevice device, VkDescriptorPool pool,
                      VkDescriptorSet set,
                      std::vector<DescriptorBinding> bindings)
        : device_(device), pool_(pool), set_(set),
          bindings_(std::move(bindings)) {}
    ~DescriptorSetImpl() override {
        if (pool_) {
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        }
    }

    void SetBuffer(uint32_t binding, rhi::Buffer* buffer, uint64_t offset,
                   uint64_t range) override {
        const DescriptorBinding* info = Find(binding);
        auto* impl = dynamic_cast<BufferImpl*>(buffer);
        if (!info || !impl) {
            return;
        }
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = impl->Handle();
        bufferInfo.offset = offset;
        bufferInfo.range = range ? range : VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = info->type == rhi::DescriptorType::StorageBuffer
                                   ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                   : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    void SetImage(uint32_t binding, rhi::Texture* texture) override {
        const DescriptorBinding* info = Find(binding);
        auto* impl = dynamic_cast<TextureImpl*>(texture);
        if (!info || !impl) {
            return;
        }
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = impl->View();
        // A sampled image must match the layout the texture is actually in.
        // Textures managed through TransitionTexture report their tracked
        // layout; everything else keeps the render-pass convention.
        const bool storage =
            info->type == rhi::DescriptorType::StorageImage;
        imageInfo.imageLayout =
            storage ? VK_IMAGE_LAYOUT_GENERAL
                    : (impl->Layout() != VK_IMAGE_LAYOUT_UNDEFINED
                           ? impl->Layout()
                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = info->type == rhi::DescriptorType::StorageImage
                                   ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                   : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    void SetSampler(uint32_t binding, rhi::Sampler* sampler) override {
        auto* impl = dynamic_cast<SamplerImpl*>(sampler);
        if (!impl || !Find(binding)) {
            return;
        }
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = impl->Handle();

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    VkDescriptorSet Handle() const { return set_; }

private:
    const DescriptorBinding* Find(uint32_t binding) const {
        for (const DescriptorBinding& info : bindings_) {
            if (info.binding == binding) {
                return &info;
            }
        }
        return nullptr;
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    std::vector<DescriptorBinding> bindings_;
};

class QueryPoolImpl final : public rhi::QueryPool {
public:
    QueryPoolImpl(VkDevice device, VkQueryPool pool, uint32_t count,
                  float periodNs, uint32_t validBits)
        : device_(device), pool_(pool), count_(count), periodNs_(periodNs),
          validBits_(validBits) {}
    ~QueryPoolImpl() override {
        if (pool_) {
            vkDestroyQueryPool(device_, pool_, nullptr);
        }
    }

    uint32_t Count() const override { return count_; }

    void Reset() override {
        if (pool_) {
            vkResetQueryPool(device_, pool_, 0, count_);
        }
    }

    bool Resolve(uint64_t* values, uint32_t count) override {
        if (!pool_ || !values || count == 0 || count > count_) {
            return false;
        }
        std::vector<uint64_t> raw(count);
        const VkResult result = vkGetQueryPoolResults(
            device_, pool_, 0, count, count * sizeof(uint64_t), raw.data(),
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (result != VK_SUCCESS) {
            return false;
        }
        const uint64_t mask = validBits_ >= 64
                                  ? ~uint64_t(0)
                                  : ((uint64_t(1) << validBits_) - 1);
        for (uint32_t i = 0; i < count; ++i) {
            values[i] = uint64_t(double(raw[i] & mask) * double(periodNs_));
        }
        return true;
    }

    VkQueryPool Handle() const { return pool_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    uint32_t count_ = 0;
    float periodNs_ = 1.0f;
    uint32_t validBits_ = 64;
};

bool CreateHostVisibleBuffer(VkPhysicalDevice physical, VkDevice device,
                             VkDeviceSize size, VkBufferUsageFlags usage,
                             VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const uint32_t type = FindMemoryType(
        physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    return vkAllocateMemory(device, &allocate, nullptr, &memory) == VK_SUCCESS &&
           vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS;
}

bool TextureImpl::Update(const void* data) {
    return owner_ ? owner_->UploadTexture(this, data) : false;
}

} // namespace

bool Device::UploadTexture(rhi::Texture* texture, const void* data) {
    auto* impl = dynamic_cast<TextureImpl*>(texture);
    if (!impl || !data) {
        return false;
    }
    const TextureDesc& desc = impl->Desc();
    if (desc.width == 0 || desc.height == 0) {
        return false;
    }
    const uint32_t bytesPerPixel = FormatBytesPerPixel(desc.format);
    if (bytesPerPixel == 0) {
        return false;
    }

    const bool is3D = desc.depth > 1;
    const VkDeviceSize rowBytes = VkDeviceSize(desc.width) * bytesPerPixel;
    const VkDeviceSize sliceBytes = rowBytes * desc.height;
    const VkDeviceSize totalBytes = sliceBytes * (is3D ? desc.depth : 1);

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!CreateHostVisibleBuffer(physicalDevice_, device_, totalBytes,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging,
                                 stagingMemory)) {
        return false;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device_, stagingMemory, 0, totalBytes, 0, &mapped) !=
        VK_SUCCESS) {
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMemory, nullptr);
        return false;
    }
    std::memcpy(mapped, data, size_t(totalBytes));
    vkUnmapMemory(device_, stagingMemory);

    const VkImageAspectFlags aspect =
        HasUsage(desc.usage, TextureUsage::DepthStencil)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageLayout newLayout =
        HasUsage(desc.usage, TextureUsage::Sampled)
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_GENERAL;
    const VkImageLayout oldLayout = impl->Layout();

    const bool copied = ExecuteImmediate([&](VkCommandBuffer command) {
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = oldLayout;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = impl->Handle();
        toTransfer.subresourceRange = {aspect, 0, desc.mipLevels, 0, 1};
        toTransfer.srcAccessMask =
            oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? 0
                : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toTransfer);

        const uint32_t slices = is3D ? desc.depth : 1;
        for (uint32_t slice = 0; slice < slices; ++slice) {
            VkBufferImageCopy region{};
            region.bufferOffset = sliceBytes * VkDeviceSize(slice);
            region.imageSubresource = {aspect, 0, 0, 1};
            region.imageOffset = {0, 0, int32_t(is3D ? slice : 0)};
            region.imageExtent = {desc.width, desc.height, 1};
            vkCmdCopyBufferToImage(command, staging, impl->Handle(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
        }

        VkImageMemoryBarrier toFinal{};
        toFinal.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toFinal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toFinal.newLayout = newLayout;
        toFinal.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toFinal.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toFinal.image = impl->Handle();
        toFinal.subresourceRange = {aspect, 0, desc.mipLevels, 0, 1};
        toFinal.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toFinal.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toFinal);
    });

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
    if (copied) {
        impl->SetLayout(newLayout);
    }
    return copied;
}

// Vulkan command list for the migrated renderer. It records into the device's
// single in-flight command buffer; the frame lifecycle still belongs to the
// device until the swapchain abstraction lands.
class CommandListImpl final : public rhi::CommandList {
public:
    // Immediate-mode bindings (the SceneRenderer path) are turned into one
    // descriptor set per recorded draw state.
    enum class BindingKind { Buffer, Image, Sampler };
    struct PendingBinding {
        BindingKind kind = BindingKind::Buffer;
        uint32_t binding = 0;
        rhi::Buffer* buffer = nullptr;
        uint64_t offset = 0;
        uint64_t range = 0;
        rhi::Texture* texture = nullptr;
        rhi::Sampler* sampler = nullptr;
    };
    struct PipelineBindings {
        std::vector<PendingBinding> pending;
        std::unique_ptr<rhi::DescriptorSet> current;
        std::vector<std::unique_ptr<rhi::DescriptorSet>> retired;
        std::vector<std::unique_ptr<rhi::DescriptorSet>> freeSets;
        bool inUse = false;
    };

    explicit CommandListImpl(Device* device) : device_(device) {}

    void Begin() override {}
    void End() override {}

    void BeginRenderPass(rhi::Texture* color, rhi::Texture* depth,
                         bool clearColor, bool clearDepth,
                         const float clearColorValue[4]) override {
        // A null colour target is a depth-only pass (shadow maps). The device
        // selects the matching render pass and framebuffer.
        if (!device_ || !depth) {
            return;
        }
        const float r = clearColorValue ? clearColorValue[0] : 0.0f;
        const float g = clearColorValue ? clearColorValue[1] : 0.0f;
        const float b = clearColorValue ? clearColorValue[2] : 0.0f;
        const float a = clearColorValue ? clearColorValue[3] : 1.0f;
        device_->BeginFrameTexture(color, depth, clearColor || clearDepth, r, g,
                                   b, a);
        // The previous pass was submitted before this one started, so its
        // descriptor sets are idle and can be reused.
        if (device_->frameActive_) {
            RecycleSets();
        }
    }

    void EndRenderPass() override {
        if (device_) {
            device_->EndFrame(false);
        }
    }

    void SetViewport(uint32_t width, uint32_t height) override {
        if (!device_ || !device_->frameActive_ || width == 0 || height == 0) {
            return;
        }
        VkViewport viewport{0.0f, float(height), float(width), -float(height),
                            0.0f, 1.0f};
        vkCmdSetViewport(device_->commandBuffer_, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {width, height}};
        vkCmdSetScissor(device_->commandBuffer_, 0, 1, &scissor);
    }

    void ClearDepth(rhi::Texture* depth, float value) override {
        // Depth-only clears outside a render pass run through the depth-only
        // render pass. It clears to 1.0, which is the value the shadow passes
        // use for a fully lit map.
        if (!device_ || !depth || device_->frameActive_) {
            return;
        }
        if (value != 1.0f) {
            LogWarn("Vulkan ClearDepth only supports the 1.0 default; got %g.",
                    value);
        }
        device_->BeginFrameTexture(nullptr, depth, true, 0.0f, 0.0f, 0.0f,
                                   1.0f);
        if (device_->frameActive_) {
            device_->EndFrame(false);
        }
    }

    void BindPipeline(rhi::Pipeline* pipeline) override {
        if (!device_) {
            return;
        }
        device_->BindPipeline(pipeline);
        currentSet_ = nullptr;
        if (!pipeline) {
            currentPipeline_ = nullptr;
            currentSet_ = nullptr;
            explicitSetBound_ = false;
            return;
        }
        currentPipeline_ = pipeline;
        auto& state = bindings_[pipeline];
        if (!state.current) {
            BindNewSet(pipeline, state);
        } else {
            currentSet_ = state.current.get();
            device_->BindDescriptorSet(currentSet_);
        }
        explicitSetBound_ = false;
    }
    void BindDescriptorSet(rhi::DescriptorSet* set, uint32_t index) override {
        if (device_) {
            device_->BindDescriptorSet(set, index);
            // An explicitly supplied set replaces the implicit one for the
            // draws that follow; it must not be recycled while in flight.
            explicitSetBound_ = true;
        }
    }
    void BindVertexBuffer(rhi::Buffer* buffer, uint32_t stride,
                          uint64_t offset) override {
        if (device_) {
            device_->BindVertexBuffer(buffer, stride, offset);
        }
    }
    void BindIndexBuffer(rhi::Buffer* buffer, uint64_t offset) override {
        if (device_) {
            device_->BindIndexBuffer(buffer, offset);
        }
    }

    void BindConstantBuffer(rhi::Buffer* buffer, rhi::ShaderStage,
                            uint32_t slot) override {
        if (!device_ || !currentPipeline_ || !buffer) {
            return;
        }
        PendingBinding value{};
        value.kind = BindingKind::Buffer;
        value.binding = slot;
        value.buffer = buffer;
        value.range = buffer->Size();
        SetBinding(currentPipeline_, value);
    }
    void BindShaderResource(rhi::Buffer* buffer, rhi::ShaderStage,
                            uint32_t slot) override {
        if (!device_ || !currentPipeline_ || !buffer) {
            return;
        }
        PendingBinding value{};
        value.kind = BindingKind::Buffer;
        value.binding = 8 + slot;
        value.buffer = buffer;
        value.range = buffer->Size();
        SetBinding(currentPipeline_, value);
    }
    void BindShaderResource(rhi::Texture* texture, rhi::ShaderStage,
                            uint32_t slot) override {
        if (!device_ || !currentPipeline_ || !texture) {
            return;
        }
        PendingBinding value{};
        value.kind = BindingKind::Image;
        value.binding = 8 + slot;
        value.texture = texture;
        SetBinding(currentPipeline_, value);
    }
    void BindSampler(rhi::Sampler* sampler, rhi::ShaderStage,
                     uint32_t slot) override {
        if (!device_ || !currentPipeline_ || !sampler) {
            return;
        }
        PendingBinding value{};
        value.kind = BindingKind::Sampler;
        value.binding = 16 + slot;
        value.sampler = sampler;
        SetBinding(currentPipeline_, value);
    }

    // Immediate-mode bindings are snapshotted per draw: Vulkan evaluates a
    // descriptor set at submit time, so a single mutable set per pipeline
    // would make every draw see the last binding written. Each binding change
    // that happens after a draw starts a new set, so recorded draws keep the
    // state they were recorded with (the D3D11 behaviour the RHI models).
    void SetBinding(rhi::Pipeline* pipeline, const PendingBinding& value) {
        auto& state = bindings_[pipeline];
        bool replaced = false;
        for (PendingBinding& entry : state.pending) {
            if (entry.binding == value.binding && entry.kind == value.kind) {
                entry = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            state.pending.push_back(value);
        }

        if (!state.current || state.inUse) {
            BindNewSet(pipeline, state);
            return;
        }
        ApplyBinding(state.current.get(), value);
    }

    void BindNewSet(rhi::Pipeline* pipeline, PipelineBindings& state) {
        if (state.current) {
            state.retired.push_back(std::move(state.current));
        }
        std::unique_ptr<rhi::DescriptorSet> set;
        if (!state.freeSets.empty()) {
            set = std::move(state.freeSets.back());
            state.freeSets.pop_back();
        } else {
            set = device_->CreateDescriptorSet(pipeline);
        }
        if (!set) {
            return;
        }
        for (const PendingBinding& entry : state.pending) {
            ApplyBinding(set.get(), entry);
        }
        state.current = std::move(set);
        state.inUse = false;
        currentSet_ = state.current.get();
        device_->BindDescriptorSet(currentSet_);
        explicitSetBound_ = false;
    }

    static void ApplyBinding(rhi::DescriptorSet* set,
                             const PendingBinding& value) {
        if (!set) {
            return;
        }
        switch (value.kind) {
        case BindingKind::Buffer:
            set->SetBuffer(value.binding, value.buffer, value.offset,
                           value.range);
            break;
        case BindingKind::Image:
            set->SetImage(value.binding, value.texture);
            break;
        case BindingKind::Sampler:
            set->SetSampler(value.binding, value.sampler);
            break;
        }
    }

    void MarkDrawUsesBindings() {
        if (!currentPipeline_ || explicitSetBound_) {
            return;
        }
        auto found = bindings_.find(currentPipeline_);
        if (found != bindings_.end()) {
            found->second.inUse = true;
        }
    }

    // Called at the start of every pass. Passes are submitted and waited for
    // one at a time, so every set referenced by earlier passes is idle again
    // and can be reused.
    void RecycleSets() {
        for (auto& entry : bindings_) {
            PipelineBindings& state = entry.second;
            for (std::unique_ptr<rhi::DescriptorSet>& set : state.retired) {
                state.freeSets.push_back(std::move(set));
            }
            state.retired.clear();
            if (state.current) {
                state.freeSets.push_back(std::move(state.current));
            }
            state.inUse = false;
        }
        currentPipeline_ = nullptr;
        currentSet_ = nullptr;
        explicitSetBound_ = false;
    }

    void Draw(uint32_t vertexCount, uint32_t firstVertex) override {
        if (device_) {
            MarkDrawUsesBindings();
            device_->Draw(vertexCount, firstVertex);
        }
    }
    void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                       uint32_t firstVertex, uint32_t firstInstance) override {
        if (device_) {
            MarkDrawUsesBindings();
            device_->DrawInstanced(vertexCount, instanceCount, firstVertex,
                                   firstInstance);
        }
    }
    void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                              uint32_t firstIndex, int32_t baseVertex,
                              uint32_t firstInstance) override {
        if (device_) {
            MarkDrawUsesBindings();
            device_->DrawIndexedInstanced(indexCount, instanceCount, firstIndex,
                                          baseVertex, firstInstance);
        }
    }

    void DrawInstancedIndirect(rhi::Buffer* arguments,
                               uint64_t offset) override {
        if (!device_ || !device_->frameActive_ || !device_->boundPipeline_ ||
            !arguments) {
            return;
        }
        auto* impl = dynamic_cast<BufferImpl*>(arguments);
        if (!impl) {
            return;
        }
        vkCmdBindPipeline(device_->commandBuffer_,
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          device_->boundPipeline_);
        if (device_->boundDescriptorSet_) {
            vkCmdBindDescriptorSets(device_->commandBuffer_,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    device_->boundPipelineLayout_, 0, 1,
                                    &device_->boundDescriptorSet_, 0, nullptr);
        }
        vkCmdDrawIndirect(device_->commandBuffer_, impl->Handle(), offset, 1,
                          0);
    }

    void BeginCompute() override {
        if (!device_ || device_->frameActive_ || computeActive_) {
            return;
        }
        if (!device_->WaitForFence() || !device_->BeginCommandBuffer()) {
            return;
        }
        device_->boundPipeline_ = VK_NULL_HANDLE;
        device_->boundPipelineLayout_ = VK_NULL_HANDLE;
        device_->boundDescriptorSet_ = VK_NULL_HANDLE;
        computeActive_ = true;
        RecycleSets();
    }

    void EndCompute() override {
        if (!device_ || !computeActive_) {
            return;
        }
        vkEndCommandBuffer(device_->commandBuffer_);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &device_->commandBuffer_;
        vkResetFences(device_->device_, 1, &device_->fence_);
        if (vkQueueSubmit(device_->queue_, 1, &submit, device_->fence_) !=
            VK_SUCCESS) {
            computeActive_ = false;
            return;
        }
        vkWaitForFences(device_->device_, 1, &device_->fence_, VK_TRUE, UINT64_MAX);
        computeActive_ = false;
    }

    void Dispatch(uint32_t x, uint32_t y, uint32_t z) override {
        if (!device_ || !computeActive_ || !device_->boundPipeline_) {
            return;
        }
        vkCmdBindPipeline(device_->commandBuffer_,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          device_->boundPipeline_);
        if (device_->boundDescriptorSet_) {
            vkCmdBindDescriptorSets(device_->commandBuffer_,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    device_->boundPipelineLayout_, 0, 1,
                                    &device_->boundDescriptorSet_, 0, nullptr);
        }
        vkCmdDispatch(device_->commandBuffer_, x, y, z);
    }

    void ComputeBarrier() override {
        if (!device_ || !computeActive_) {
            return;
        }
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(device_->commandBuffer_,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
    }

    void TransitionTexture(rhi::Texture* texture,
                           rhi::TextureState state) override {
        if (!device_ || (!computeActive_ && !device_->frameActive_) ||
            !texture) {
            return;
        }
        auto* impl = dynamic_cast<TextureImpl*>(texture);
        if (!impl) {
            return;
        }
        const bool isDepth =
            texture->GetFormat() == rhi::Format::D32_FLOAT ||
            texture->GetFormat() == rhi::Format::R32_TYPELESS;

        VkImageLayout newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        switch (state) {
        case rhi::TextureState::General:
            newLayout = VK_IMAGE_LAYOUT_GENERAL;
            break;
        case rhi::TextureState::ShaderRead:
            newLayout = isDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            break;
        case rhi::TextureState::DepthRead:
            newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            break;
        case rhi::TextureState::DepthAttachment:
            newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            break;
        }

        const VkImageLayout oldLayout = impl->Layout();
        if (oldLayout == newLayout) {
            return;
        }

        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccess = 0;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        VkAccessFlags dstAccess = VK_ACCESS_SHADER_READ_BIT;

        if (oldLayout == VK_IMAGE_LAYOUT_GENERAL) {
            srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            srcAccess = VK_ACCESS_SHADER_WRITE_BIT |
                        VK_ACCESS_SHADER_READ_BIT;
        } else if (oldLayout ==
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            srcAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        } else if (oldLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            srcAccess = VK_ACCESS_SHADER_READ_BIT;
        }

        if (newLayout == VK_IMAGE_LAYOUT_GENERAL) {
            dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            dstAccess = VK_ACCESS_SHADER_WRITE_BIT |
                        VK_ACCESS_SHADER_READ_BIT;
        } else if (newLayout ==
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dstAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = impl->Handle();
        barrier.subresourceRange.aspectMask =
            isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(device_->commandBuffer_, srcStage, dstStage, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        impl->SetLayout(newLayout);
    }

    void WriteTimestamp(rhi::QueryPool* pool, uint32_t index) override {
        if (!device_ || (!computeActive_ && !device_->frameActive_) || !pool) {
            return;
        }
        auto* impl = dynamic_cast<QueryPoolImpl*>(pool);
        if (!impl || index >= impl->Count()) {
            return;
        }
        vkCmdWriteTimestamp(device_->commandBuffer_,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            impl->Handle(), index);
    }

    void* NativeHandle() const override {
        return device_ ? device_->commandBuffer_ : VK_NULL_HANDLE;
    }

private:
    Device* device_ = nullptr;
    bool computeActive_ = false;
    std::unordered_map<rhi::Pipeline*, PipelineBindings> bindings_;
    rhi::Pipeline* currentPipeline_ = nullptr;
    rhi::DescriptorSet* currentSet_ = nullptr;
    // True while an explicitly supplied descriptor set is bound, so the
    // implicit set does not get recycled underneath it.
    bool explicitSetBound_ = false;
};

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
    if (!CreateTextureRenderPass()) {
        return false;
    }
    if (!CreateTextureLoadRenderPass()) {
        return false;
    }
    if (!CreateTextureDepthOnlyRenderPass(false) ||
        !CreateTextureDepthOnlyRenderPass(true)) {
        return false;
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
        DestroyTargetResources();
        if (textureRenderPass_) {
            vkDestroyRenderPass(device_, textureRenderPass_, nullptr);
            textureRenderPass_ = VK_NULL_HANDLE;
        }
        if (textureLoadRenderPass_) {
            vkDestroyRenderPass(device_, textureLoadRenderPass_, nullptr);
            textureLoadRenderPass_ = VK_NULL_HANDLE;
        }
        if (textureDepthOnlyRenderPass_) {
            vkDestroyRenderPass(device_, textureDepthOnlyRenderPass_, nullptr);
            textureDepthOnlyRenderPass_ = VK_NULL_HANDLE;
        }
        if (textureDepthOnlyLoadRenderPass_) {
            vkDestroyRenderPass(device_, textureDepthOnlyLoadRenderPass_, nullptr);
            textureDepthOnlyLoadRenderPass_ = VK_NULL_HANDLE;
        }
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
        if (pipelineCache_) {
            vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
            pipelineCache_ = VK_NULL_HANDLE;
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

    const VkResult result = vkCreateInstance(&info, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        LogError("Vulkan instance creation failed (%d).", int(result));
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
            timestampPeriod_ = properties.limits.timestampPeriod;
            timestampValidBits_ = families[graphicsFamily].timestampValidBits;

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

    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &supportedFeatures);
    samplerAnisotropy_ = supportedFeatures.samplerAnisotropy == VK_TRUE;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    const bool supports12 = properties.apiVersion >= VK_API_VERSION_1_2;

    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    bool hostQueryReset = false;
    if (supports12) {
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &supported12;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &features2);
        hostQueryReset = supported12.hostQueryReset == VK_TRUE;
    }
    if (!hostQueryReset) {
        // vkResetQueryPool needs the hostQueryReset feature; without it the
        // solver benchmark falls back to wall-clock timing.
        timestampValidBits_ = 0;
    }

    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.samplerAnisotropy =
        samplerAnisotropy_ ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled12.hostQueryReset = hostQueryReset ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceFeatures2 enabledFeatures2{};
    enabledFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabledFeatures2.features = enabledFeatures;
    enabledFeatures2.pNext = supports12 ? &enabled12 : nullptr;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.pNext = supports12 ? &enabledFeatures2 : nullptr;
    info.pEnabledFeatures = supports12 ? nullptr : &enabledFeatures;

    if (vkCreateDevice(physicalDevice_, &info, nullptr, &device_) != VK_SUCCESS) {
        LogError("Vulkan logical device creation failed.");
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_) !=
        VK_SUCCESS) {
        pipelineCache_ = VK_NULL_HANDLE;
    }
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

bool Device::CreateTextureRenderPass() {
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

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

    if (vkCreateRenderPass(device_, &info, nullptr, &textureRenderPass_) !=
        VK_SUCCESS) {
        LogError("Vulkan texture render pass creation failed.");
        return false;
    }
    return true;
}

bool Device::CreateTextureLoadRenderPass() {
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

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

    if (vkCreateRenderPass(device_, &info, nullptr, &textureLoadRenderPass_) !=
        VK_SUCCESS) {
        LogError("Vulkan texture load render pass creation failed.");
        return false;
    }
    return true;
}

bool Device::CreateTextureDepthOnlyRenderPass(bool load) {
    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp =
        load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = load ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                               : VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{
        0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
        load ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT)
             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependency.srcAccessMask = load ? VK_ACCESS_SHADER_READ_BIT : 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &depth;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;

    VkRenderPass* target =
        load ? &textureDepthOnlyLoadRenderPass_ : &textureDepthOnlyRenderPass_;
    if (vkCreateRenderPass(device_, &info, nullptr, target) != VK_SUCCESS) {
        LogError("Vulkan depth-only render pass creation failed.");
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

void Device::DestroyTargetResources() {
    if (targetFramebuffer_) {
        vkDestroyFramebuffer(device_, targetFramebuffer_, nullptr);
        targetFramebuffer_ = VK_NULL_HANDLE;
    }
    if (targetDepthView_) {
        vkDestroyImageView(device_, targetDepthView_, nullptr);
        targetDepthView_ = VK_NULL_HANDLE;
    }
    if (targetDepthImage_) {
        vkDestroyImage(device_, targetDepthImage_, nullptr);
        targetDepthImage_ = VK_NULL_HANDLE;
    }
    if (targetDepthMemory_) {
        vkFreeMemory(device_, targetDepthMemory_, nullptr);
        targetDepthMemory_ = VK_NULL_HANDLE;
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
    return vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
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
    // Vulkan's NDC Y points down while D3D's points up. A negative viewport
    // height flips the image so both backends share the same screen-space
    // convention.
    VkViewport viewport{0.0f, float(height_), float(width_), -float(height_),
                        0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {uint32_t(width_), uint32_t(height_)}};
    vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);
    boundPipeline_ = VK_NULL_HANDLE;
    boundPipelineLayout_ = VK_NULL_HANDLE;
    boundDescriptorSet_ = VK_NULL_HANDLE;
    boundVertexBuffer_ = VK_NULL_HANDLE;
    boundIndexBuffer_ = VK_NULL_HANDLE;
    frameActive_ = true;
}

void Device::BeginFrameTexture(Texture* color, float r, float g, float b,
                               float a) {
    if (!initialized_ || frameActive_ || !textureRenderPass_) {
        return;
    }
    auto* texture = dynamic_cast<TextureImpl*>(color);
    if (!texture || !texture->View()) {
        return;
    }
    if (!WaitForFence()) {
        return;
    }

    targetWidth_ = texture->Width();
    targetHeight_ = texture->Height();
    if (!CreateImage(physicalDevice_, device_, targetWidth_, targetHeight_,
                     VK_FORMAT_D32_SFLOAT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     targetDepthImage_, targetDepthMemory_)) {
        return;
    }
    targetDepthView_ = CreateImageView(device_, targetDepthImage_,
                                       VK_FORMAT_D32_SFLOAT,
                                       VK_IMAGE_ASPECT_DEPTH_BIT);
    if (targetDepthView_ == VK_NULL_HANDLE) {
        DestroyTargetResources();
        return;
    }

    VkImageView views[2] = {texture->View(), targetDepthView_};
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = textureRenderPass_;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = views;
    framebufferInfo.width = targetWidth_;
    framebufferInfo.height = targetHeight_;
    framebufferInfo.layers = 1;
    if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr,
                            &targetFramebuffer_) != VK_SUCCESS) {
        DestroyTargetResources();
        return;
    }

    if (!BeginCommandBuffer()) {
        DestroyTargetResources();
        return;
    }

    VkClearValue clears[2]{};
    clears[0].color = {{r, g, b, a}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = textureRenderPass_;
    info.framebuffer = targetFramebuffer_;
    info.renderArea.extent = {targetWidth_, targetHeight_};
    info.clearValueCount = 2;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(commandBuffer_, &info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0.0f, float(targetHeight_), float(targetWidth_),
                        -float(targetHeight_), 0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {targetWidth_, targetHeight_}};
    vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);

    boundPipeline_ = VK_NULL_HANDLE;
    boundPipelineLayout_ = VK_NULL_HANDLE;
    boundDescriptorSet_ = VK_NULL_HANDLE;
    boundVertexBuffer_ = VK_NULL_HANDLE;
    boundIndexBuffer_ = VK_NULL_HANDLE;
    renderingToTexture_ = true;
    frameActive_ = true;
}

void Device::BeginFrameTexture(Texture* color, Texture* depth, bool clear,
                               float r, float g, float b, float a) {
    if (!initialized_ || frameActive_ || !textureRenderPass_ ||
        !textureLoadRenderPass_) {
        return;
    }
    auto* colorTexture = dynamic_cast<TextureImpl*>(color);
    auto* depthTexture = dynamic_cast<TextureImpl*>(depth);
    if (!depthTexture || !depthTexture->View()) {
        return;
    }
    if (!colorTexture) {
        if (!textureDepthOnlyRenderPass_ || !textureDepthOnlyLoadRenderPass_) {
            return;
        }
        if (!WaitForFence()) {
            return;
        }
        targetWidth_ = depthTexture->Width();
        targetHeight_ = depthTexture->Height();

        const VkRenderPass pass =
            clear ? textureDepthOnlyRenderPass_ : textureDepthOnlyLoadRenderPass_;
        VkImageView view = depthTexture->View();
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = pass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &view;
        framebufferInfo.width = targetWidth_;
        framebufferInfo.height = targetHeight_;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr,
                                &targetFramebuffer_) != VK_SUCCESS) {
            return;
        }
        if (!BeginCommandBuffer()) {
            DestroyTargetResources();
            return;
        }

        VkClearValue clearValue{};
        clearValue.depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = pass;
        info.framebuffer = targetFramebuffer_;
        info.renderArea.extent = {targetWidth_, targetHeight_};
        info.clearValueCount = 1;
        info.pClearValues = &clearValue;
        vkCmdBeginRenderPass(commandBuffer_, &info, VK_SUBPASS_CONTENTS_INLINE);
        depthTexture->SetLayout(
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        VkViewport viewport{0.0f, float(targetHeight_), float(targetWidth_),
                            -float(targetHeight_), 0.0f, 1.0f};
        vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {targetWidth_, targetHeight_}};
        vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);

        boundPipeline_ = VK_NULL_HANDLE;
        boundPipelineLayout_ = VK_NULL_HANDLE;
        boundDescriptorSet_ = VK_NULL_HANDLE;
        boundVertexBuffer_ = VK_NULL_HANDLE;
        boundIndexBuffer_ = VK_NULL_HANDLE;
        renderingToTexture_ = true;
        depthOnlyFrame_ = true;
        frameDepthOnlyTexture_ = depth;
        frameActive_ = true;
        return;
    }
    if (!colorTexture->View()) {
        return;
    }
    if (!WaitForFence()) {
        return;
    }

    targetWidth_ = colorTexture->Width();
    targetHeight_ = colorTexture->Height();

    VkImageView views[2] = {colorTexture->View(), depthTexture->View()};
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass =
        clear ? textureRenderPass_ : textureLoadRenderPass_;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = views;
    framebufferInfo.width = targetWidth_;
    framebufferInfo.height = targetHeight_;
    framebufferInfo.layers = 1;
    if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr,
                            &targetFramebuffer_) != VK_SUCCESS) {
        return;
    }

    if (!BeginCommandBuffer()) {
        DestroyTargetResources();
        return;
    }

    VkClearValue clears[2]{};
    clears[0].color = {{r, g, b, a}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = clear ? textureRenderPass_ : textureLoadRenderPass_;
    info.framebuffer = targetFramebuffer_;
    info.renderArea.extent = {targetWidth_, targetHeight_};
    info.clearValueCount = 2;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(commandBuffer_, &info, VK_SUBPASS_CONTENTS_INLINE);
    depthTexture->SetLayout(
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkViewport viewport{0.0f, float(targetHeight_), float(targetWidth_),
                        -float(targetHeight_), 0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {targetWidth_, targetHeight_}};
    vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);

    boundPipeline_ = VK_NULL_HANDLE;
    boundPipelineLayout_ = VK_NULL_HANDLE;
    boundDescriptorSet_ = VK_NULL_HANDLE;
    boundVertexBuffer_ = VK_NULL_HANDLE;
    boundIndexBuffer_ = VK_NULL_HANDLE;
    renderingToTexture_ = true;
    frameActive_ = true;
}

void Device::EndFrame(bool vsync) {
    (void)vsync;
    if (!frameActive_) {
        return;
    }
    vkCmdEndRenderPass(commandBuffer_);
    if (depthOnlyFrame_) {
        if (auto* texture = dynamic_cast<TextureImpl*>(frameDepthOnlyTexture_)) {
            texture->SetLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        depthOnlyFrame_ = false;
        frameDepthOnlyTexture_ = nullptr;
    }
    vkEndCommandBuffer(commandBuffer_);
    frameActive_ = false;
    const bool toTexture = renderingToTexture_;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (!offscreen_ && !toTexture) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailable_;
        submit.pWaitDstStageMask = &waitStage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished_;
    }

    if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS ||
        vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS) {
        return;
    }

    if (offscreen_ || toTexture) {
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        if (toTexture) {
            DestroyTargetResources();
            renderingToTexture_ = false;
        }
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
    if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS ||
        vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS ||
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

std::unique_ptr<rhi::CommandList> Device::CreateCommandList() {
    return std::make_unique<CommandListImpl>(this);
}

std::unique_ptr<rhi::QueryPool> Device::CreateQueryPool(uint32_t count) {
    if (!device_ || count == 0 || timestampValidBits_ == 0) {
        return nullptr;
    }
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (vkCreateQueryPool(device_, &info, nullptr, &pool) != VK_SUCCESS) {
        return nullptr;
    }
    return std::make_unique<QueryPoolImpl>(device_, pool, count,
                                           timestampPeriod_,
                                           timestampValidBits_);
}

bool Device::ExecuteImmediate(
    const std::function<void(VkCommandBuffer)>& record) {
    if (!WaitForFence() || !BeginCommandBuffer()) {
        return false;
    }
    record(commandBuffer_);
    vkEndCommandBuffer(commandBuffer_);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS ||
        vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS ||
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return false;
    }
    return true;
}

std::unique_ptr<Buffer> Device::CreateBuffer(const BufferDesc& desc) {
    if (!device_ || desc.size == 0) {
        return nullptr;
    }

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = desc.size;
    info.usage = ToVkBufferUsage(desc.usage);
    if (info.usage == 0) {
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    info.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device_, &info, nullptr, &buffer) != VK_SUCCESS) {
        return nullptr;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);

    const bool hostVisible = HasUsage(desc.usage, BufferUsage::Dynamic) ||
                             HasUsage(desc.usage, BufferUsage::Staging);
    const uint32_t type = FindMemoryType(
        physicalDevice_, requirements.memoryTypeBits,
        hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == std::numeric_limits<uint32_t>::max()) {
        vkDestroyBuffer(device_, buffer, nullptr);
        return nullptr;
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &allocate, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        if (memory) {
            vkFreeMemory(device_, memory, nullptr);
        }
        return nullptr;
    }

    auto result = std::make_unique<BufferImpl>(device_, buffer, memory, desc.size,
                                               hostVisible);
    if (!desc.initialData) {
        return result;
    }

    if (hostVisible) {
        void* mapped = result->Map();
        if (!mapped) {
            return nullptr;
        }
        std::memcpy(mapped, desc.initialData, size_t(desc.size));
        result->Unmap();
        return result;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!CreateHostVisibleBuffer(physicalDevice_, device_, desc.size,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging,
                                 stagingMemory)) {
        return nullptr;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device_, stagingMemory, 0, desc.size, 0, &mapped) !=
        VK_SUCCESS) {
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMemory, nullptr);
        return nullptr;
    }
    std::memcpy(mapped, desc.initialData, size_t(desc.size));
    vkUnmapMemory(device_, stagingMemory);

    const bool copied = ExecuteImmediate([&](VkCommandBuffer command) {
        VkBufferCopy region{0, 0, desc.size};
        vkCmdCopyBuffer(command, staging, buffer, 1, &region);
    });
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
    return copied ? std::move(result) : nullptr;
}

std::unique_ptr<Texture> Device::CreateTexture(const TextureDesc& desc) {
    if (!device_ || desc.width == 0 || desc.height == 0) {
        return nullptr;
    }
    VkFormat format = ToVkFormat(desc.format);
    if (format == VK_FORMAT_UNDEFINED) {
        return nullptr;
    }
    // D3D11 uses a typeless R32 format for a depth texture that is also
    // sampled. Vulkan has no typeless formats, so pick the depth format.
    if (desc.format == Format::R32_TYPELESS &&
        HasUsage(desc.usage, TextureUsage::DepthStencil)) {
        format = VK_FORMAT_D32_SFLOAT;
    }

    // Tightly packed initialData is the common case for generated 3D volumes
    // (the solver mask and the volume bake). Treat it as one subresource so it
    // takes the same staging upload path as an explicit mip chain; otherwise
    // the texture would be created with undefined contents.
    std::vector<TextureSubresource> initialSubresources;
    const TextureSubresource* subresources = desc.subresources;
    uint32_t subresourceCount = desc.subresourceCount;
    if ((!subresources || subresourceCount == 0) && desc.initialData) {
        const uint32_t bytesPerPixel = FormatBytesPerPixel(desc.format);
        if (bytesPerPixel == 0) {
            return nullptr;
        }
        TextureSubresource packed{};
        packed.data = desc.initialData;
        packed.width = desc.width;
        packed.height = desc.height;
        packed.rowPitch = desc.width * bytesPerPixel;
        packed.slicePitch = packed.rowPitch * desc.height;
        initialSubresources.push_back(packed);
        subresources = initialSubresources.data();
        subresourceCount = 1;
    }

    const bool is3D = desc.depth > 1;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.extent = {desc.width, desc.height, is3D ? desc.depth : 1};
    info.mipLevels = desc.mipLevels;
    info.arrayLayers = 1;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = ToVkTextureUsage(desc.usage);
    if (info.usage == 0) {
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (subresources && subresourceCount > 0) {
        info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device_, &info, nullptr, &image) != VK_SUCCESS) {
        return nullptr;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image, &requirements);
    const uint32_t type = FindMemoryType(physicalDevice_, requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == std::numeric_limits<uint32_t>::max()) {
        vkDestroyImage(device_, image, nullptr);
        return nullptr;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &allocate, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, image, memory, 0) != VK_SUCCESS) {
        vkDestroyImage(device_, image, nullptr);
        if (memory) {
            vkFreeMemory(device_, memory, nullptr);
        }
        return nullptr;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = is3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask =
        HasUsage(desc.usage, TextureUsage::DepthStencil)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device_, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        vkDestroyImage(device_, image, nullptr);
        vkFreeMemory(device_, memory, nullptr);
        return nullptr;
    }

    auto result = std::make_unique<TextureImpl>(this, device_, image, memory,
                                                view, format, desc);
    if (subresourceCount == 0) {
        return result;
    }

    VkDeviceSize stagingSize = 0;
    std::vector<VkDeviceSize> offsets(subresourceCount);
    std::vector<VkDeviceSize> sliceSizes(subresourceCount);
    for (uint32_t i = 0; i < subresourceCount; ++i) {
        offsets[i] = stagingSize;
        const VkDeviceSize rowBytes =
            VkDeviceSize(subresources[i].rowPitch) * subresources[i].height;
        sliceSizes[i] = subresources[i].slicePitch
                            ? subresources[i].slicePitch
                            : rowBytes;
        stagingSize += sliceSizes[i] * (is3D ? desc.depth : 1);
    }
    if (stagingSize == 0) {
        return result;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!CreateHostVisibleBuffer(physicalDevice_, device_, stagingSize,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging,
                                 stagingMemory)) {
        return nullptr;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device_, stagingMemory, 0, stagingSize, 0, &mapped) !=
        VK_SUCCESS) {
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMemory, nullptr);
        return nullptr;
    }
    for (uint32_t i = 0; i < subresourceCount; ++i) {
        const uint32_t slices = is3D ? desc.depth : 1;
        const size_t rowBytes =
            size_t(subresources[i].rowPitch) * subresources[i].height;
        for (uint32_t slice = 0; slice < slices; ++slice) {
            std::memcpy(static_cast<uint8_t*>(mapped) + offsets[i] +
                            size_t(sliceSizes[i]) * slice,
                        static_cast<const uint8_t*>(subresources[i].data) +
                            size_t(sliceSizes[i]) * slice,
                        rowBytes);
        }
    }
    vkUnmapMemory(device_, stagingMemory);

    const VkImageAspectFlags aspect =
        HasUsage(desc.usage, TextureUsage::DepthStencil)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageLayout finalLayout =
        HasUsage(desc.usage, TextureUsage::Sampled)
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : (HasUsage(desc.usage, TextureUsage::RenderTarget)
                   ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                   : VK_IMAGE_LAYOUT_GENERAL);

    const bool copied = ExecuteImmediate([&](VkCommandBuffer command) {
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange = {aspect, 0, desc.mipLevels, 0, 1};
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toTransfer);

        for (uint32_t i = 0; i < subresourceCount; ++i) {
            const uint32_t slices = is3D ? desc.depth : 1;
            for (uint32_t slice = 0; slice < slices; ++slice) {
                VkBufferImageCopy region{};
                region.bufferOffset =
                    offsets[i] + sliceSizes[i] * VkDeviceSize(slice);
                region.imageSubresource = {aspect, i, 0, 1};
                region.imageOffset = {0, 0, int32_t(slice)};
                region.imageExtent = {subresources[i].width,
                                      subresources[i].height, 1};
                vkCmdCopyBufferToImage(command, staging, image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);
            }
        }

        VkImageMemoryBarrier toFinal{};
        toFinal.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toFinal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toFinal.newLayout = finalLayout;
        toFinal.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toFinal.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toFinal.image = image;
        toFinal.subresourceRange = {aspect, 0, desc.mipLevels, 0, 1};
        toFinal.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toFinal.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toFinal);
    });
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
    if (!copied) {
        return nullptr;
    }
    result->SetLayout(finalLayout);
    return result;
}

std::unique_ptr<Shader> Device::CreateShader(const ShaderDesc& desc) {
    if (!device_ || !desc.data || desc.size == 0 || (desc.size % 4) != 0) {
        return nullptr;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = desc.size;
    info.pCode = static_cast<const uint32_t*>(desc.data);
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
        return nullptr;
    }
    return std::make_unique<ShaderImpl>(device_, module, desc.stage, desc.name);
}

std::unique_ptr<Sampler> Device::CreateSampler(const SamplerDesc& desc) {
    if (!device_) {
        return nullptr;
    }
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = desc.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = info.magFilter;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    const VkSamplerAddressMode address =
        desc.wrap ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                  : (desc.clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    info.addressModeU = address;
    info.addressModeV = address;
    info.addressModeW = address;
    info.borderColor = desc.comparison
                           ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                           : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.compareEnable = desc.comparison ? VK_TRUE : VK_FALSE;
    info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    info.maxLod = desc.comparison ? 0.0f : VK_LOD_CLAMP_NONE;
    if (!desc.comparison && desc.maxAnisotropy > 1 && samplerAnisotropy_) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = float(desc.maxAnisotropy);
    }

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS) {
        return nullptr;
    }
    return std::make_unique<SamplerImpl>(device_, sampler);
}

std::unique_ptr<Pipeline> Device::CreatePipeline(const PipelineDesc& desc) {
    if (!device_) {
        return nullptr;
    }
    if (desc.computeShader) {
        auto* computeShader = dynamic_cast<ShaderImpl*>(desc.computeShader);
        if (!computeShader ||
            desc.computeShader->Stage() != ShaderStage::Compute) {
            return nullptr;
        }
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        std::vector<DescriptorBinding> bindings;
        layoutBindings.reserve(desc.descriptorBindingCount);
        bindings.reserve(desc.descriptorBindingCount);
        for (uint32_t i = 0; i < desc.descriptorBindingCount; ++i) {
            const DescriptorBinding& descriptorBinding =
                desc.descriptorBindings[i];
            bindings.push_back(descriptorBinding);
            VkDescriptorSetLayoutBinding converted{};
            converted.binding = descriptorBinding.binding;
            converted.descriptorType =
                ToVkDescriptorType(descriptorBinding.type);
            converted.descriptorCount =
                descriptorBinding.count == 0 ? 1 : descriptorBinding.count;
            converted.stageFlags = ToVkStage(descriptorBinding);
            layoutBindings.push_back(converted);
        }
        VkDescriptorSetLayoutCreateInfo descriptorInfo{};
        descriptorInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorInfo.bindingCount = uint32_t(layoutBindings.size());
        descriptorInfo.pBindings = layoutBindings.data();
        VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(device_, &descriptorInfo, nullptr,
                                        &descriptorLayout) != VK_SUCCESS) {
            return nullptr;
        }
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorLayout;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout) !=
            VK_SUCCESS) {
            vkDestroyDescriptorSetLayout(device_, descriptorLayout, nullptr);
            return nullptr;
        }
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = computeShader->Module();
        stage.pName = computeShader->EntryPoint();
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage = stage;
        info.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device_, pipelineCache_, 1, &info,
                                     nullptr, &pipeline) != VK_SUCCESS) {
            vkDestroyPipelineLayout(device_, layout, nullptr);
            vkDestroyDescriptorSetLayout(device_, descriptorLayout, nullptr);
            return nullptr;
        }
        return std::make_unique<PipelineImpl>(device_, pipeline, layout,
                                              descriptorLayout,
                                              std::move(bindings));
    }
    if (!renderPass_ || !desc.vertexShader) {
        return nullptr;
    }
    auto* vertexShader = dynamic_cast<ShaderImpl*>(desc.vertexShader);
    auto* pixelShader = dynamic_cast<ShaderImpl*>(desc.pixelShader);
    const bool depthOnly = desc.depthOnly || !desc.pixelShader;
    if (!vertexShader || (!depthOnly && !pixelShader)) {
        return nullptr;
    }
    if (depthOnly && !textureDepthOnlyRenderPass_) {
        return nullptr;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader->Module();
    stages[0].pName = vertexShader->EntryPoint();
    uint32_t stageCount = 1;
    if (!depthOnly) {
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = pixelShader->Module();
        stages[1].pName = pixelShader->EntryPoint();
        stageCount = 2;
    }

    uint32_t stride = desc.vertexStride;
    if (stride == 0) {
        for (uint32_t i = 0; i < desc.vertexAttributeCount; ++i) {
            const VertexAttribute& attribute = desc.vertexAttributes[i];
            uint32_t size = 4;
            switch (attribute.format) {
            case VertexFormat::Float2:
            case VertexFormat::UInt2:
                size = 8;
                break;
            case VertexFormat::Float3:
            case VertexFormat::UInt3:
                size = 12;
                break;
            case VertexFormat::Float4:
            case VertexFormat::UInt4:
                size = 16;
                break;
            default:
                break;
            }
            stride = std::max(stride, attribute.offset + size);
        }
    }

    VkVertexInputBindingDescription binding{0, stride,
                                            VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(desc.vertexAttributeCount);
    for (uint32_t i = 0; i < desc.vertexAttributeCount; ++i) {
        const VertexAttribute& attribute = desc.vertexAttributes[i];
        VkVertexInputAttributeDescription converted{};
        converted.location = i;
        converted.binding = attribute.bufferSlot;
        converted.format = ToVkVertexFormat(attribute.format);
        converted.offset = attribute.offset;
        if (converted.format == VK_FORMAT_UNDEFINED) {
            return nullptr;
        }
        attributes.push_back(converted);
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = stride > 0 ? 1 : 0;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = ToVkTopology(desc.topology);
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = ToVkCull(desc.cullMode);
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    raster.depthClampEnable = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = ToVkCompare(desc.depthCompare);
    depth.depthBoundsTestEnable = VK_FALSE;
    depth.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor =
        desc.additiveBlend || desc.premultipliedBlend
            ? VK_BLEND_FACTOR_ONE
            : VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor =
        desc.additiveBlend ? VK_BLEND_FACTOR_ONE
                           : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = depthOnly ? 0u : 1u;
    blend.pAttachments = depthOnly ? nullptr : &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
    std::vector<DescriptorBinding> bindings;
    layoutBindings.reserve(desc.descriptorBindingCount);
    bindings.reserve(desc.descriptorBindingCount);
    for (uint32_t i = 0; i < desc.descriptorBindingCount; ++i) {
        const DescriptorBinding& descriptorBinding = desc.descriptorBindings[i];
        bindings.push_back(descriptorBinding);
        VkDescriptorSetLayoutBinding converted{};
        converted.binding = descriptorBinding.binding;
        converted.descriptorType = ToVkDescriptorType(descriptorBinding.type);
        converted.descriptorCount =
            descriptorBinding.count == 0 ? 1 : descriptorBinding.count;
        converted.stageFlags = ToVkStage(descriptorBinding);
        layoutBindings.push_back(converted);
    }

    VkDescriptorSetLayoutCreateInfo descriptorInfo{};
    descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorInfo.bindingCount = uint32_t(layoutBindings.size());
    descriptorInfo.pBindings = layoutBindings.data();
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device_, &descriptorInfo, nullptr,
                                    &descriptorLayout) != VK_SUCCESS) {
        return nullptr;
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout) !=
        VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(device_, descriptorLayout, nullptr);
        return nullptr;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = stageCount;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depth;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass =
        depthOnly ? textureDepthOnlyRenderPass_ : renderPass_;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pipelineResult =
        vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo,
                                  nullptr, &pipeline);
    if (pipelineResult != VK_SUCCESS) {
        LogError("Vulkan graphics pipeline creation failed (%d).",
                 int(pipelineResult));
        vkDestroyPipelineLayout(device_, layout, nullptr);
        vkDestroyDescriptorSetLayout(device_, descriptorLayout, nullptr);
        return nullptr;
    }
    return std::make_unique<PipelineImpl>(device_, pipeline, layout,
                                          descriptorLayout, std::move(bindings));
}

std::unique_ptr<DescriptorSet> Device::CreateDescriptorSet(Pipeline* pipeline) {
    auto* impl = dynamic_cast<PipelineImpl*>(pipeline);
    if (!impl) {
        return nullptr;
    }

    std::vector<VkDescriptorPoolSize> poolSizes;
    poolSizes.reserve(impl->Bindings().size());
    for (const DescriptorBinding& binding : impl->Bindings()) {
        VkDescriptorPoolSize size{};
        size.type = ToVkDescriptorType(binding.type);
        size.descriptorCount = binding.count == 0 ? 1 : binding.count;
        poolSizes.push_back(size);
    }

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = uint32_t(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return nullptr;
    }

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    const VkDescriptorSetLayout layout = impl->DescriptorLayout();
    allocate.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &allocate, &set) != VK_SUCCESS) {
        vkDestroyDescriptorPool(device_, pool, nullptr);
        return nullptr;
    }
    return std::make_unique<DescriptorSetImpl>(device_, pool, set,
                                               impl->Bindings());
}

void Device::BindDescriptorSet(DescriptorSet* set, uint32_t index) {
    (void)index;
    auto* impl = dynamic_cast<DescriptorSetImpl*>(set);
    boundDescriptorSet_ = impl ? impl->Handle() : VK_NULL_HANDLE;
}

void Device::BindPipeline(Pipeline* pipeline) {
    auto* impl = dynamic_cast<PipelineImpl*>(pipeline);
    boundPipeline_ = impl ? impl->Pipeline() : VK_NULL_HANDLE;
    boundPipelineLayout_ = impl ? impl->Layout() : VK_NULL_HANDLE;
}

void Device::BindVertexBuffer(Buffer* buffer, uint32_t stride,
                              uint64_t offset) {
    (void)stride;
    auto* impl = dynamic_cast<BufferImpl*>(buffer);
    boundVertexBuffer_ = impl ? impl->Handle() : VK_NULL_HANDLE;
    boundVertexOffset_ = offset;
}

void Device::BindIndexBuffer(Buffer* buffer, uint64_t offset) {
    auto* impl = dynamic_cast<BufferImpl*>(buffer);
    boundIndexBuffer_ = impl ? impl->Handle() : VK_NULL_HANDLE;
    boundIndexOffset_ = offset;
}

void Device::Draw(uint32_t vertexCount, uint32_t firstVertex) {
    if (!frameActive_ || !boundPipeline_) {
        return;
    }
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      boundPipeline_);
    if (boundDescriptorSet_) {
        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                boundPipelineLayout_, 0, 1, &boundDescriptorSet_,
                                0, nullptr);
    }
    if (boundVertexBuffer_) {
        vkCmdBindVertexBuffers(commandBuffer_, 0, 1, &boundVertexBuffer_,
                               &boundVertexOffset_);
    }
    vkCmdDraw(commandBuffer_, vertexCount, 1, firstVertex, 0);
}

void Device::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                           uint32_t firstVertex, uint32_t firstInstance) {
    if (!frameActive_ || !boundPipeline_) {
        return;
    }
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      boundPipeline_);
    if (boundDescriptorSet_) {
        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                boundPipelineLayout_, 0, 1, &boundDescriptorSet_,
                                0, nullptr);
    }
    if (boundVertexBuffer_) {
        vkCmdBindVertexBuffers(commandBuffer_, 0, 1, &boundVertexBuffer_,
                               &boundVertexOffset_);
    }
    vkCmdDraw(commandBuffer_, vertexCount, instanceCount, firstVertex,
              firstInstance);
}

void Device::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                                  uint32_t firstIndex, int32_t baseVertex,
                                  uint32_t firstInstance) {
    if (!frameActive_ || !boundPipeline_) {
        return;
    }
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      boundPipeline_);
    if (boundDescriptorSet_) {
        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                boundPipelineLayout_, 0, 1, &boundDescriptorSet_,
                                0, nullptr);
    }
    if (boundVertexBuffer_) {
        vkCmdBindVertexBuffers(commandBuffer_, 0, 1, &boundVertexBuffer_,
                               &boundVertexOffset_);
    }
    if (boundIndexBuffer_) {
        vkCmdBindIndexBuffer(commandBuffer_, boundIndexBuffer_,
                             boundIndexOffset_, VK_INDEX_TYPE_UINT32);
    }
    vkCmdDrawIndexed(commandBuffer_, indexCount, instanceCount, firstIndex,
                     baseVertex, firstInstance);
}

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

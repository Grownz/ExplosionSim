#include "gfx/GpuVolumeBaker.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace ex::gfx {

bool GpuVolumeBaker::Initialize(ex::rhi::Device* device,
                                const void* shaderData, size_t shaderSize) {
    if (!device || !shaderData || shaderSize == 0) {
        return false;
    }
    device_ = device;
    commandList_ = device->CreateCommandList();
    if (!commandList_) {
        return false;
    }

    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Compute;
    shaderDesc.data = shaderData;
    shaderDesc.size = shaderSize;
    shaderDesc.name = "CSBake";
    shader_ = device->CreateShader(shaderDesc);

    const ex::rhi::DescriptorBinding bindings[] = {
        {0, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Compute, 4},
        {21, ex::rhi::DescriptorType::StorageImage, 1,
         ex::rhi::ShaderStage::Compute, 4},
        {22, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Compute, 4},
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.computeShader = shader_.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 3;
    pipeline_ = device->CreatePipeline(pipelineDesc);
    if (!pipeline_) {
        LogWarn("GPU volume bake pipeline unavailable; using the CPU bake.");
        return false;
    }
    descriptorSet_ = device->CreateDescriptorSet(pipeline_.get());

    ex::rhi::BufferDesc constantDesc{};
    constantDesc.size = sizeof(Constants);
    constantDesc.usage = ex::rhi::BufferUsage::Uniform |
                         ex::rhi::BufferUsage::Dynamic;
    constants_ = device->CreateBuffer(constantDesc);
    if (!descriptorSet_ || !constants_) {
        return false;
    }

    // Placeholder mask until the first bake supplies one; the pipeline always
    // needs a bound sampled image.
    const float zero = 0.0f;
    return CreateSolidMask(&zero, 1);
}

bool GpuVolumeBaker::CreateSolidMask(const void* data, int resolution) {
    if (!device_ || !commandList_) {
        return false;
    }
    device_->WaitIdle();
    ex::rhi::TextureDesc desc{};
    desc.format = ex::rhi::Format::R32_FLOAT;
    desc.width = uint32_t(std::max(resolution, 1));
    desc.height = uint32_t(std::max(resolution, 1));
    desc.depth = uint32_t(std::max(resolution, 1));
    desc.usage = ex::rhi::TextureUsage::Sampled;
    desc.initialData = data;
    auto texture = device_->CreateTexture(desc);
    if (!texture) {
        return false;
    }
    // The image is created with initial data, but the validation layers still
    // expect an explicit transition to the layout the descriptor declares.
    commandList_->BeginCompute();
    commandList_->TransitionTexture(texture.get(),
                                    ex::rhi::TextureState::ShaderRead);
    commandList_->EndCompute();
    solidMask_ = std::move(texture);
    if (descriptorSet_) {
        descriptorSet_->SetImage(22, solidMask_.get());
    }
    return true;
}

void GpuVolumeBaker::Shutdown() {
    texture_.reset();
    solidMask_.reset();
    descriptorSet_.reset();
    constants_.reset();
    pipeline_.reset();
    shader_.reset();
    commandList_.reset();
    device_ = nullptr;
}

bool GpuVolumeBaker::Bake(const ex::sim::VolumeBakeRequest& request) {
    if (!device_ || !commandList_ || !pipeline_ || !descriptorSet_ ||
        !constants_ || !request.valid()) {
        return false;
    }
    const int resolution = request.resolution;
    if (!texture_ || resolution_ != resolution) {
        device_->WaitIdle();
        ex::rhi::TextureDesc desc{};
        desc.format = ex::rhi::Format::R16G16B16A16_FLOAT;
        desc.width = uint32_t(resolution);
        desc.height = uint32_t(resolution);
        desc.depth = uint32_t(resolution);
        desc.usage = ex::rhi::TextureUsage::Storage |
                     ex::rhi::TextureUsage::Sampled;
        texture_ = device_->CreateTexture(desc);
        if (!texture_) {
            return false;
        }
        resolution_ = resolution;
        descriptorSet_->SetImage(21, texture_.get());
    }

    // Upload the solid mask only when its contents actually changed; the CPU
    // baker caches it the same way and bumps the version on every rebuild.
    const uint64_t wantedMaskVersion =
        request.hasSolidMask() ? request.solidMaskVersion : 0;
    if (solidMaskVersion_ != wantedMaskVersion && solidMask_) {
        if (request.hasSolidMask()) {
            if (!CreateSolidMask(request.solidMask->data(),
                                 request.solidMaskResolution)) {
                return false;
            }
        } else {
            const float zero = 0.0f;
            if (!CreateSolidMask(&zero, 1)) {
                return false;
            }
        }
        solidMaskVersion_ = wantedMaskVersion;
    }

    Constants constants{};
    const ex::sim::VolumeFieldParams& p = request.params;
    constants.boundsMin =
        DirectX::XMFLOAT4(request.bounds.min.x, request.bounds.min.y,
                          request.bounds.min.z, float(resolution));
    const DirectX::XMFLOAT3 size(
        request.bounds.max.x - request.bounds.min.x,
        request.bounds.max.y - request.bounds.min.y,
        request.bounds.max.z - request.bounds.min.z);
    constants.boundsSize =
        DirectX::XMFLOAT4(size.x, size.y, size.z, request.voxelSize);
    constants.fireball =
        DirectX::XMFLOAT4(p.fireballX, p.fireballY, p.fireballZ,
                          p.fireballRadius);
    constants.fireball2 =
        DirectX::XMFLOAT4(p.fireballTemperature,
                          p.fireballVisible ? 1.0f : 0.0f, p.time,
                          p.ambientTemperature);
    constants.cloud =
        DirectX::XMFLOAT4(p.cloudX, p.cloudY, p.cloudZ, p.cloudRadius);
    constants.cloud2 =
        DirectX::XMFLOAT4(p.cloudTemperature, p.stemRadius, p.cloudStrength,
                          p.dustRingRadius);
    constants.shock =
        DirectX::XMFLOAT4(p.shockRadius, p.shockMach, p.burstY,
                          p.dustIntensity);
    constants.maskParams =
        DirectX::XMFLOAT4(request.hasSolidMask() ? 1.0f : 0.0f, 0.0f, 0.0f,
                          0.0f);
    constants.maskMin = DirectX::XMFLOAT4(request.solidMaskBounds.min.x,
                                          request.solidMaskBounds.min.y,
                                          request.solidMaskBounds.min.z, 0.0f);
    constants.maskMax = DirectX::XMFLOAT4(request.solidMaskBounds.max.x,
                                          request.solidMaskBounds.max.y,
                                          request.solidMaskBounds.max.z, 0.0f);
    void* mapped = constants_->Map();
    if (!mapped) {
        return false;
    }
    std::memcpy(mapped, &constants, sizeof(constants));
    constants_->Unmap();
    descriptorSet_->SetBuffer(0, constants_.get(), 0, sizeof(constants));

    commandList_->BeginCompute();
    commandList_->TransitionTexture(texture_.get(),
                                    ex::rhi::TextureState::General);
    commandList_->BindPipeline(pipeline_.get());
    commandList_->BindDescriptorSet(descriptorSet_.get());
    const uint32_t groups = uint32_t((resolution + 3) / 4);
    commandList_->Dispatch(groups, groups, groups);
    commandList_->TransitionTexture(texture_.get(),
                                    ex::rhi::TextureState::ShaderRead);
    commandList_->EndCompute();

    bounds_ = request.bounds;
    extinction_ = request.extinctionScale;
    ++version_;
    return true;
}

} // namespace ex::gfx

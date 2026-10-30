#include "gfx/GpuVolumeBaker.h"

#include "core/Log.h"

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
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.computeShader = shader_.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 2;
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
    return descriptorSet_ && constants_;
}

void GpuVolumeBaker::Shutdown() {
    texture_.reset();
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

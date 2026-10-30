#include "gfx/GpuParticleSystem.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace ex::gfx {

bool GpuParticleSystem::Initialize(ex::rhi::Device* device, int capacity,
                                   const ShaderSource& apply,
                                   const ShaderSource& reset,
                                   const ShaderSource& integrate,
                                   const ShaderSource& finalize) {
    if (!device || capacity <= 0) {
        return false;
    }
    device_ = device;
    capacity_ = capacity;
    commandList_ = device->CreateCommandList();
    if (!commandList_) {
        return false;
    }

    ex::rhi::BufferDesc constantDesc{};
    constantDesc.size = 3 * sizeof(DirectX::XMFLOAT4);
    constantDesc.usage = ex::rhi::BufferUsage::Uniform |
                         ex::rhi::BufferUsage::Dynamic;
    constants_ = device->CreateBuffer(constantDesc);

    auto makeBuffer = [&](uint64_t size, ex::rhi::BufferUsage usage,
                          uint32_t stride) {
        ex::rhi::BufferDesc desc{};
        desc.size = size;
        desc.usage = usage;
        desc.structureStride = stride;
        return device->CreateBuffer(desc);
    };
    state_ = makeBuffer(uint64_t(capacity) * sizeof(ex::sim::ParticleGpuState),
                        ex::rhi::BufferUsage::Storage |
                            ex::rhi::BufferUsage::ShaderRead,
                        sizeof(ex::sim::ParticleGpuState));
    // Start from a known-zero state so a missed reset cannot produce garbage
    // particles from uninitialized device memory.
    {
        std::vector<uint8_t> zeroState(
            size_t(capacity) * sizeof(ex::sim::ParticleGpuState), 0);
        ex::rhi::BufferDesc desc{};
        desc.size = zeroState.size();
        desc.usage = ex::rhi::BufferUsage::Storage |
                     ex::rhi::BufferUsage::ShaderRead;
        desc.structureStride = sizeof(ex::sim::ParticleGpuState);
        desc.initialData = zeroState.data();
        state_ = device->CreateBuffer(desc);
    }
    spawn_ = makeBuffer(uint64_t(capacity) * sizeof(ex::sim::ParticleSpawn),
                        ex::rhi::BufferUsage::Storage |
                            ex::rhi::BufferUsage::ShaderRead |
                            ex::rhi::BufferUsage::Dynamic,
                        sizeof(ex::sim::ParticleSpawn));
    additive_ = makeBuffer(uint64_t(capacity) * sizeof(ex::sim::ParticleInstance),
                           ex::rhi::BufferUsage::Storage |
                               ex::rhi::BufferUsage::ShaderRead,
                           sizeof(ex::sim::ParticleInstance));
    alpha_ = makeBuffer(uint64_t(capacity) * sizeof(ex::sim::ParticleInstance),
                        ex::rhi::BufferUsage::Storage |
                            ex::rhi::BufferUsage::ShaderRead,
                        sizeof(ex::sim::ParticleInstance));
    fire_ = makeBuffer(uint64_t(capacity) * sizeof(ex::sim::ParticleInstance),
                       ex::rhi::BufferUsage::Storage |
                           ex::rhi::BufferUsage::ShaderRead,
                       sizeof(ex::sim::ParticleInstance));
    counters_ = makeBuffer(4 * sizeof(uint32_t),
                           ex::rhi::BufferUsage::Storage |
                               ex::rhi::BufferUsage::Dynamic,
                           sizeof(uint32_t));
    indirect_ = makeBuffer(12 * sizeof(uint32_t),
                           ex::rhi::BufferUsage::Storage |
                               ex::rhi::BufferUsage::Indirect,
                           sizeof(uint32_t));

    if (!constants_ || !state_ || !spawn_ || !additive_ || !alpha_ || !fire_ ||
        !counters_ || !indirect_) {
        return false;
    }

    if (!CreatePipeline(device, apply, "CSApplySpawns", applyShader_, applyPipeline_, applySet_) ||
        !CreatePipeline(device, reset, "CSReset", resetShader_, resetPipeline_, resetSet_) ||
        !CreatePipeline(device, integrate, "CSIntegrate", integrateShader_,
                        integratePipeline_, integrateSet_) ||
        !CreatePipeline(device, finalize, "CSFinalize", finalizeShader_,
                        finalizePipeline_, finalizeSet_)) {
        return false;
    }

    auto bind = [&](ex::rhi::DescriptorSet* set) {
        if (!set) {
            return;
        }
        set->SetBuffer(0, constants_.get(), 0, constants_->Size());
        set->SetBuffer(8, spawn_.get(), 0, spawn_->Size());
        set->SetBuffer(21, state_.get(), 0, state_->Size());
        set->SetBuffer(22, additive_.get(), 0, additive_->Size());
        set->SetBuffer(23, alpha_.get(), 0, alpha_->Size());
        set->SetBuffer(24, fire_.get(), 0, fire_->Size());
        set->SetBuffer(25, counters_.get(), 0, counters_->Size());
        set->SetBuffer(26, indirect_.get(), 0, indirect_->Size());
    };
    bind(applySet_.get());
    bind(resetSet_.get());
    bind(integrateSet_.get());
    bind(finalizeSet_.get());
    return true;
}

void GpuParticleSystem::Shutdown() {
    finalizeSet_.reset();
    integrateSet_.reset();
    resetSet_.reset();
    applySet_.reset();
    finalizePipeline_.reset();
    integratePipeline_.reset();
    resetPipeline_.reset();
    applyPipeline_.reset();
    finalizeShader_.reset();
    integrateShader_.reset();
    resetShader_.reset();
    applyShader_.reset();
    indirect_.reset();
    counters_.reset();
    fire_.reset();
    alpha_.reset();
    additive_.reset();
    spawn_.reset();
    state_.reset();
    constants_.reset();
    commandList_.reset();
    device_ = nullptr;
}

bool GpuParticleSystem::CreatePipeline(
    ex::rhi::Device* device, const ShaderSource& source,
    const char* entryPoint, std::unique_ptr<ex::rhi::Shader>& shader,
    std::unique_ptr<ex::rhi::Pipeline>& pipeline,
    std::unique_ptr<ex::rhi::DescriptorSet>& set) {
    if (!source.data || source.size == 0) {
        return false;
    }
    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Compute;
    shaderDesc.data = source.data;
    shaderDesc.size = source.size;
    shaderDesc.name = entryPoint;
    shader = device->CreateShader(shaderDesc);
    if (!shader) {
        return false;
    }
    const ex::rhi::DescriptorBinding bindings[] = {
        {0, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Compute, 4},
        {8, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Compute, 4},
        {21, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Compute, 4},
        {22, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Compute, 4},
        {23, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Compute, 4},
        {24, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Compute, 4},
        {25, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Compute, 4},
        {26, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Compute, 4},
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.computeShader = shader.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 8;
    pipeline = device->CreatePipeline(pipelineDesc);
    if (!pipeline) {
        return false;
    }
    set = device->CreateDescriptorSet(pipeline.get());
    return set != nullptr;
}

bool GpuParticleSystem::Update(ex::sim::ParticleSystem& particles) {
    if (!device_ || !commandList_ || !constants_ || !state_ || !spawn_ ||
        !counters_ || !indirect_) {
        return false;
    }
    const bool reset = particles.ConsumeGpuReset();
    const float dt = particles.ConsumeGpuDt();
    if (!reset && dt <= 0.0f) {
        return true;
    }

    // Host writes must not race the previous frame's compute/render work.
    device_->WaitIdle();
    const std::vector<ex::sim::ParticleSpawn>& spawns =
        particles.PrepareGpuSpawns();
    if (spawns.size() > size_t(capacity_)) {
        LogWarn("GPU particles: %zu spawns exceed the %d capacity.",
                spawns.size(), capacity_);
    }
    const uint32_t spawnCount =
        uint32_t(std::min(spawns.size(), size_t(capacity_)));
    if (spawnCount > 0) {
        void* mapped = spawn_->Map();
        if (!mapped) {
            return false;
        }
        std::memcpy(mapped, spawns.data(),
                    size_t(spawnCount) * sizeof(ex::sim::ParticleSpawn));
        spawn_->Unmap();
    }

    if (void* mapped = counters_->Map()) {
        std::memset(mapped, 0, 4 * sizeof(uint32_t));
        counters_->Unmap();
    }
    const ex::sim::ParticleGpuParams& gp = particles.GpuParams();
    struct Constants {
        DirectX::XMFLOAT4 windTime;
        DirectX::XMFLOAT4 params;
        DirectX::XMFLOAT4 counts;
    } constants{};
    constants.windTime = DirectX::XMFLOAT4(gp.wind.x, gp.wind.y, gp.wind.z,
                                           gp.time);
    constants.params = DirectX::XMFLOAT4(gp.ambientTemperature, gp.gravity, dt,
                                        float(capacity_));
    constants.counts = DirectX::XMFLOAT4(float(spawnCount), 0.0f, 0.0f, 0.0f);
    if (void* mapped = constants_->Map()) {
        std::memcpy(mapped, &constants, sizeof(constants));
        constants_->Unmap();
    } else {
        return false;
    }

    commandList_->BeginCompute();
    if (reset) {
        commandList_->BindPipeline(resetPipeline_.get());
        commandList_->BindDescriptorSet(resetSet_.get());
        commandList_->Dispatch((capacity_ + 63) / 64, 1, 1);
        commandList_->ComputeBarrier();
    }
    if (spawnCount > 0) {
        commandList_->BindPipeline(applyPipeline_.get());
        commandList_->BindDescriptorSet(applySet_.get());
        commandList_->Dispatch((spawnCount + 63) / 64, 1, 1);
        commandList_->ComputeBarrier();
    }
    commandList_->BindPipeline(integratePipeline_.get());
    commandList_->BindDescriptorSet(integrateSet_.get());
    commandList_->Dispatch((capacity_ + 63) / 64, 1, 1);
    commandList_->ComputeBarrier();
    commandList_->BindPipeline(finalizePipeline_.get());
    commandList_->BindDescriptorSet(finalizeSet_.get());
    commandList_->Dispatch(1, 1, 1);
    commandList_->EndCompute();
    return true;
}

} // namespace ex::gfx

#include "gfx/RhiEulerSolver.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace ex::gfx {

namespace {

constexpr float kCooling = 2.0e4f;
constexpr float kCfl = 0.20f;

using ex::rhi::DescriptorBinding;
using ex::rhi::DescriptorType;
using ex::rhi::ShaderStage;

const DescriptorBinding kInitBindings[] = {
    {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, 4},
    {21, DescriptorType::StorageImage, 1, ShaderStage::Compute, 4},
    {22, DescriptorType::StorageImage, 1, ShaderStage::Compute, 4},
};

const DescriptorBinding kSweepBindings[] = {
    {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, 4},
    {8, DescriptorType::SampledImage, 1, ShaderStage::Compute, 4},
    {9, DescriptorType::SampledImage, 1, ShaderStage::Compute, 4},
    {21, DescriptorType::StorageImage, 1, ShaderStage::Compute, 4},
    {22, DescriptorType::StorageImage, 1, ShaderStage::Compute, 4},
};

const DescriptorBinding kReduceBindings[] = {
    {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, 4},
    {8, DescriptorType::SampledImage, 1, ShaderStage::Compute, 4},
    {9, DescriptorType::SampledImage, 1, ShaderStage::Compute, 4},
    {23, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, 4},
};

const DescriptorBinding kRenderBindings[] = {
    {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, 4},
    {8, DescriptorType::SampledImage, 1, ShaderStage::Compute, 4},
    {9, DescriptorType::SampledImage, 1, ShaderStage::Compute, 4},
    {24, DescriptorType::StorageImage, 1, ShaderStage::Compute, 4},
};

const DescriptorBinding kLightBindings[] = {
    {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, 4},
    {10, DescriptorType::SampledImage, 1, ShaderStage::Compute, 4},
    {16, DescriptorType::Sampler, 1, ShaderStage::Compute, 4},
    {25, DescriptorType::StorageImage, 1, ShaderStage::Compute, 4},
};

const DescriptorBinding kRescaleBindings[] = {
    {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, 4},
    {8, DescriptorType::SampledImage, 1, ShaderStage::Compute, 4},
    {9, DescriptorType::SampledImage, 1, ShaderStage::Compute, 4},
    {16, DescriptorType::Sampler, 1, ShaderStage::Compute, 4},
    {21, DescriptorType::StorageImage, 1, ShaderStage::Compute, 4},
    {22, DescriptorType::StorageImage, 1, ShaderStage::Compute, 4},
};

} // namespace

bool RhiEulerSolver::Initialize(ex::rhi::Device* device, const Shaders& shaders) {
    if (!device || device->GetBackend() != ex::rhi::Backend::Vulkan) {
        return false;
    }
    device_ = device;
    commandList_ = device->CreateCommandList();
    if (!commandList_) {
        return false;
    }

    ex::rhi::BufferDesc constantDesc{};
    constantDesc.size = sizeof(Constants);
    constantDesc.usage = ex::rhi::BufferUsage::Uniform |
                         ex::rhi::BufferUsage::Dynamic;
    constantsBuffer_ = device->CreateBuffer(constantDesc);

    ex::rhi::BufferDesc reduceDesc{};
    reduceDesc.size = kReduceChannels * sizeof(uint32_t);
    reduceDesc.usage = ex::rhi::BufferUsage::Storage |
                       ex::rhi::BufferUsage::Dynamic;
    reduceDesc.structureStride = sizeof(uint32_t);
    reduceBuffer_ = device->CreateBuffer(reduceDesc);

    ex::rhi::SamplerDesc samplerDesc{};
    samplerDesc.linear = true;
    samplerDesc.clamp = true;
    linearSampler_ = device->CreateSampler(samplerDesc);

    if (!constantsBuffer_ || !reduceBuffer_ || !linearSampler_) {
        LogError("High-fidelity solver: RHI resource creation failed.");
        return false;
    }

    struct PassDesc {
        const ShaderSource* source;
        const char* name;
        const DescriptorBinding* bindings;
        uint32_t bindingCount;
        std::unique_ptr<ex::rhi::Shader>* shader;
        std::unique_ptr<ex::rhi::Pipeline>* pipeline;
    };
    const PassDesc passes[] = {
        {&shaders.init, "CSInit", kInitBindings, 3, &initShader_, &initPipeline_},
        {&shaders.sweepX, "CSSweepX", kSweepBindings, 5, &sweepShaders_[0],
         &sweepPipelines_[0]},
        {&shaders.sweepY, "CSSweepY", kSweepBindings, 5, &sweepShaders_[1],
         &sweepPipelines_[1]},
        {&shaders.sweepZ, "CSSweepZ", kSweepBindings, 5, &sweepShaders_[2],
         &sweepPipelines_[2]},
        {&shaders.sweepXSource, "CSSweepXSource", kSweepBindings, 5,
         &sweepSourceShaders_[0], &sweepSourcePipelines_[0]},
        {&shaders.sweepYSource, "CSSweepYSource", kSweepBindings, 5,
         &sweepSourceShaders_[1], &sweepSourcePipelines_[1]},
        {&shaders.sweepZSource, "CSSweepZSource", kSweepBindings, 5,
         &sweepSourceShaders_[2], &sweepSourcePipelines_[2]},
        {&shaders.reduce, "CSReduce", kReduceBindings, 4, &reduceShader_,
         &reducePipeline_},
        {&shaders.render, "CSRender", kRenderBindings, 4, &renderShader_,
         &renderPipeline_},
        {&shaders.light, "CSLight", kLightBindings, 4, &lightShader_,
         &lightPipeline_},
        {&shaders.rescale, "CSRescale", kRescaleBindings, 6, &rescaleShader_,
         &rescalePipeline_},
    };
    for (const PassDesc& pass : passes) {
        if (!CreatePipeline(*pass.source, pass.name, pass.bindings,
                            pass.bindingCount, *pass.shader, *pass.pipeline)) {
            LogError("High-fidelity solver: %s pipeline creation failed.",
                     pass.name);
            return false;
        }
    }

    initialized_ = true;
    return true;
}

bool RhiEulerSolver::CreatePipeline(
    const ShaderSource& source, const char* name,
    const ex::rhi::DescriptorBinding* bindings, uint32_t bindingCount,
    std::unique_ptr<ex::rhi::Shader>& shader,
    std::unique_ptr<ex::rhi::Pipeline>& pipeline) {
    if (!source.data || source.size == 0) {
        return false;
    }
    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Compute;
    shaderDesc.data = source.data;
    shaderDesc.size = source.size;
    shaderDesc.name = name;
    shader = device_->CreateShader(shaderDesc);
    if (!shader) {
        return false;
    }
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.computeShader = shader.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = bindingCount;
    pipeline = device_->CreatePipeline(pipelineDesc);
    return pipeline != nullptr;
}

void RhiEulerSolver::Shutdown() {
    ReleaseVolumeResources();
    timingPool_.reset();
    timingLabels_.clear();
    reduceBuffer_.reset();
    linearSampler_.reset();
    constantsBuffer_.reset();
    rescalePipeline_.reset();
    lightPipeline_.reset();
    renderPipeline_.reset();
    reducePipeline_.reset();
    for (auto& pipeline : sweepSourcePipelines_) {
        pipeline.reset();
    }
    for (auto& pipeline : sweepPipelines_) {
        pipeline.reset();
    }
    initPipeline_.reset();
    rescaleShader_.reset();
    lightShader_.reset();
    renderShader_.reset();
    reduceShader_.reset();
    for (auto& shader : sweepSourceShaders_) {
        shader.reset();
    }
    for (auto& shader : sweepShaders_) {
        shader.reset();
    }
    initShader_.reset();
    commandList_.reset();
    device_ = nullptr;
    initialized_ = false;
}

bool RhiEulerSolver::CreateTextures(int resolution) {
    ReleaseVolumeResources();
    resolution_ = std::clamp(resolution, 32, 512);

    auto makeTexture = [&](ex::rhi::Format format, int size) {
        ex::rhi::TextureDesc desc{};
        desc.format = format;
        desc.width = uint32_t(size);
        desc.height = uint32_t(size);
        desc.depth = uint32_t(size);
        desc.usage = ex::rhi::TextureUsage::Storage |
                     ex::rhi::TextureUsage::Sampled;
        return device_->CreateTexture(desc);
    };

    for (int i = 0; i < 2; ++i) {
        stateA_[i] = makeTexture(ex::rhi::Format::R32G32B32A32_FLOAT,
                                 resolution_);
        stateB_[i] =
            makeTexture(ex::rhi::Format::R32G32_FLOAT, resolution_);
        if (!stateA_[i] || !stateB_[i]) {
            LogError("High-fidelity solver: state texture creation failed.");
            return false;
        }
    }
    renderVolume_ = makeTexture(ex::rhi::Format::R16G16B16A16_FLOAT, resolution_);
    lightVolume_ =
        makeTexture(ex::rhi::Format::R32_FLOAT, kLightVolumeResolution);
    if (!renderVolume_ || !lightVolume_) {
        LogError("High-fidelity solver: render/light volume creation failed.");
        return false;
    }

    // The state textures are read and written by different passes, so keep
    // them in GENERAL for their whole lifetime. This lets the sampled and
    // storage descriptors alias the same image without per-pass transitions.
    commandList_->BeginCompute();
    for (auto& texture : stateA_) {
        commandList_->TransitionTexture(texture.get(),
                                        ex::rhi::TextureState::General);
    }
    for (auto& texture : stateB_) {
        commandList_->TransitionTexture(texture.get(),
                                        ex::rhi::TextureState::General);
    }
    commandList_->TransitionTexture(renderVolume_.get(),
                                    ex::rhi::TextureState::General);
    commandList_->TransitionTexture(lightVolume_.get(),
                                    ex::rhi::TextureState::General);
    commandList_->EndCompute();

    stateIndex_ = 0;
    return CreateDescriptorSets();
}

bool RhiEulerSolver::CreateDescriptorSets() {
    auto bindCommon = [&](ex::rhi::DescriptorSet* set) {
        set->SetBuffer(0, constantsBuffer_.get(), 0, sizeof(Constants));
    };

    initSet_ = device_->CreateDescriptorSet(initPipeline_.get());
    if (!initSet_) {
        return false;
    }
    bindCommon(initSet_.get());
    initSet_->SetImage(21, stateA_[0].get());
    initSet_->SetImage(22, stateB_[0].get());

    for (int parity = 0; parity < 2; ++parity) {
        const int write = 1 - parity;
        for (int dir = 0; dir < 3; ++dir) {
            auto makeSweepSet = [&](ex::rhi::Pipeline* pipeline) {
                auto set = device_->CreateDescriptorSet(pipeline);
                if (!set) {
                    return SetPtr();
                }
                bindCommon(set.get());
                set->SetImage(8, stateA_[parity].get());
                set->SetImage(9, stateB_[parity].get());
                set->SetImage(21, stateA_[write].get());
                set->SetImage(22, stateB_[write].get());
                return set;
            };
            sweepSets_[dir][parity] = makeSweepSet(sweepPipelines_[dir].get());
            sweepSourceSets_[dir][parity] =
                makeSweepSet(sweepSourcePipelines_[dir].get());
            if (!sweepSets_[dir][parity] ||
                !sweepSourceSets_[dir][parity]) {
                return false;
            }
        }

        auto makeReadSet = [&](ex::rhi::Pipeline* pipeline) {
            auto set = device_->CreateDescriptorSet(pipeline);
            if (!set) {
                return SetPtr();
            }
            bindCommon(set.get());
            set->SetImage(8, stateA_[parity].get());
            set->SetImage(9, stateB_[parity].get());
            return set;
        };
        reduceSets_[parity] = makeReadSet(reducePipeline_.get());
        renderSets_[parity] = makeReadSet(renderPipeline_.get());
        if (!reduceSets_[parity] || !renderSets_[parity]) {
            return false;
        }
        reduceSets_[parity]->SetBuffer(23, reduceBuffer_.get(), 0,
                                       reduceBuffer_->Size());
        renderSets_[parity]->SetImage(24, renderVolume_.get());

        rescaleSets_[parity] =
            device_->CreateDescriptorSet(rescalePipeline_.get());
        if (!rescaleSets_[parity]) {
            return false;
        }
        bindCommon(rescaleSets_[parity].get());
        rescaleSets_[parity]->SetImage(8, stateA_[parity].get());
        rescaleSets_[parity]->SetImage(9, stateB_[parity].get());
        rescaleSets_[parity]->SetSampler(16, linearSampler_.get());
        rescaleSets_[parity]->SetImage(21, stateA_[write].get());
        rescaleSets_[parity]->SetImage(22, stateB_[write].get());
    }

    lightSet_ = device_->CreateDescriptorSet(lightPipeline_.get());
    if (!lightSet_) {
        return false;
    }
    bindCommon(lightSet_.get());
    lightSet_->SetImage(10, renderVolume_.get());
    lightSet_->SetSampler(16, linearSampler_.get());
    lightSet_->SetImage(25, lightVolume_.get());
    return true;
}

void RhiEulerSolver::ReleaseVolumeResources() {
    initSet_.reset();
    for (auto& row : sweepSets_) {
        for (auto& set : row) {
            set.reset();
        }
    }
    for (auto& row : sweepSourceSets_) {
        for (auto& set : row) {
            set.reset();
        }
    }
    for (auto& set : reduceSets_) {
        set.reset();
    }
    for (auto& set : renderSets_) {
        set.reset();
    }
    for (auto& set : rescaleSets_) {
        set.reset();
    }
    lightSet_.reset();
    lightVolume_.reset();
    renderVolume_.reset();
    for (auto& texture : stateB_) {
        texture.reset();
    }
    for (auto& texture : stateA_) {
        texture.reset();
    }
    resolution_ = 0;
    hasVolume_ = false;
}

void RhiEulerSolver::UpdateConstants(int direction, float dt) {
    constants_.grid = XMINT4(resolution_, resolution_, resolution_, direction);
    constants_.domainMin =
        XMFLOAT4(bounds_.min.x, bounds_.min.y, bounds_.min.z, dx_);
    constants_.domainMax = XMFLOAT4(bounds_.max.x, bounds_.max.y, bounds_.max.z, dt);
    constants_.ambient = XMFLOAT4(float(config_.ambientDensity), float(config_.ambientPressure),
                                  float(config_.ambientTemperature), float(config_.gamma));
    constants_.charge = XMFLOAT4(float(config_.chargeMassKg), float(config_.chargeEnergyJ),
                                 depositRadius_, float(config_.heightOfBurstM));
    constants_.physics = XMFLOAT4(float(config_.gravity), float(config_.gasConstant), kCooling,
                                  float(time_));
    constants_.extra = XMFLOAT4(dragRadius_, plumeCenterY_,
                                config_.hemispherical ? 1.0f : 0.0f, capThreshold_);
    constants_.oldDomainMin =
        XMFLOAT4(oldBounds_.min.x, oldBounds_.min.y, oldBounds_.min.z, oldDx_);
    constants_.oldDomainMax =
        XMFLOAT4(oldBounds_.max.x, oldBounds_.max.y, oldBounds_.max.z, 0.0f);
    constants_.sunDirection = XMFLOAT4(sunDirection_.x, sunDirection_.y, sunDirection_.z, 0.0f);
    constants_.lightParams = XMFLOAT4(extinctionScale_, 24.0f, 0.0f, 0.0f);
    constants_.capParams =
        XMFLOAT4(plumeRadius_, plumeCenterY_, plumeTop_, capThreshold_);
    constants_.envParams = XMFLOAT4(stableLayerY_, initialTop_, riseSpeed_, 0.0f);

    if (void* mapped = constantsBuffer_->Map()) {
        std::memcpy(mapped, &constants_, sizeof(constants_));
        constantsBuffer_->Unmap();
    }
}

void RhiEulerSolver::DispatchGrid() {
    const uint32_t groups = uint32_t((resolution_ + kThreadGroup - 1) / kThreadGroup);
    commandList_->Dispatch(groups, groups, groups);
}

void RhiEulerSolver::DispatchSweep(int direction, bool applySource) {
    ex::rhi::Pipeline* pipeline =
        applySource ? sweepSourcePipelines_[direction].get()
                    : sweepPipelines_[direction].get();
    ex::rhi::DescriptorSet* set =
        applySource ? sweepSourceSets_[direction][stateIndex_].get()
                    : sweepSets_[direction][stateIndex_].get();
    commandList_->BindPipeline(pipeline);
    commandList_->BindDescriptorSet(set);
    BeginPass(SolverPass(int(SolverPass::SweepX) + direction));
    DispatchGrid();
    EndPass();
    commandList_->ComputeBarrier();
    stateIndex_ = 1 - stateIndex_;
}

void RhiEulerSolver::DispatchReduce() {
    commandList_->BindPipeline(reducePipeline_.get());
    commandList_->BindDescriptorSet(reduceSets_[stateIndex_].get());
    BeginPass(SolverPass::Reduce);
    DispatchGrid();
    EndPass();
    commandList_->ComputeBarrier();
    reducePending_ = true;
}

void RhiEulerSolver::DispatchRender() {
    UpdateConstants(0, 0.0f);
    commandList_->BindPipeline(renderPipeline_.get());
    commandList_->BindDescriptorSet(renderSets_[stateIndex_].get());
    BeginPass(SolverPass::Render);
    DispatchGrid();
    EndPass();
    commandList_->ComputeBarrier();
}

void RhiEulerSolver::BeginPass(SolverPass pass) {
    if (!timingActive_ || !timingPool_ ||
        timingIndex_ + 1 >= timingQueryLimit_) {
        return;
    }
    timingLabels_.push_back(uint8_t(pass));
    commandList_->WriteTimestamp(timingPool_.get(), uint32_t(timingIndex_++));
    timingOpenPass_ = int(timingLabels_.size()) - 1;
}

void RhiEulerSolver::EndPass() {
    if (!timingActive_ || !timingPool_ || timingOpenPass_ < 0 ||
        timingIndex_ >= timingQueryLimit_) {
        return;
    }
    commandList_->WriteTimestamp(timingPool_.get(), uint32_t(timingIndex_++));
    timingOpenPass_ = -1;
}

bool RhiEulerSolver::BeginTiming(int maxPasses) {
    if (!initialized_ || maxPasses <= 0) {
        return false;
    }
    EndTiming();

    const int queryCount = maxPasses * 2;
    if (!timingPool_ || timingPool_->Count() < uint32_t(queryCount)) {
        timingPool_ = device_->CreateQueryPool(uint32_t(queryCount));
        if (!timingPool_) {
            LogWarn("Solver timing: GPU timestamp queries are unavailable.");
            return false;
        }
    }
    timingPool_->Reset();
    timingLabels_.clear();
    timingLabels_.reserve(size_t(maxPasses));
    timingQueryLimit_ = queryCount;
    timingIndex_ = 0;
    timingOpenPass_ = -1;
    timingActive_ = true;
    return true;
}

void RhiEulerSolver::EndTiming() {
    timingActive_ = false;
    timingOpenPass_ = -1;
}

SolverTiming RhiEulerSolver::ResolveTiming() {
    SolverTiming timing;
    EndTiming();
    if (!timingPool_ || timingIndex_ < 2 || timingLabels_.empty()) {
        return timing;
    }

    // The only blocking readback in the benchmark; it happens once after all
    // steps have been submitted.
    std::vector<uint64_t> stamps;
    stamps.resize(size_t(timingIndex_));
    if (!timingPool_->Resolve(stamps.data(), uint32_t(stamps.size()))) {
        LogWarn("Solver timing: timestamp readback failed.");
        return timing;
    }

    constexpr double kNsToMs = 1.0e-6;
    timing.valid = true;
    timing.totalMs =
        double(stamps[size_t(timingIndex_) - 1] - stamps[0]) * kNsToMs;
    for (size_t pass = 0; pass < timingLabels_.size(); ++pass) {
        const size_t index = pass * 2;
        if (index + 1 >= stamps.size()) {
            break;
        }
        const int label = timingLabels_[pass];
        timing.passMs[label] +=
            double(stamps[index + 1] - stamps[index]) * kNsToMs;
        ++timing.passCount[label];
    }
    return timing;
}

bool RhiEulerSolver::Reset(const EulerConfig& config) {
    if (!initialized_) {
        return false;
    }

    config_ = config;
    const int resolution = std::clamp(config.resolution, 32, 512);
    if (resolution_ != resolution) {
        if (!CreateTextures(resolution)) {
            return false;
        }
    }

    const double half = std::max(config.domainHalfExtent, 0.5);
    bounds_.min = XMFLOAT3(float(-half), 0.0f, float(-half));
    bounds_.max = XMFLOAT3(float(half), float(2.0 * half), float(half));
    dx_ = float(2.0 * half / double(resolution_));
    initialTop_ = bounds_.max.y;
    stableLayerY_ = 0.0f;
    plumeTop_ = 0.0f;
    dragRadius_ = 0.0f;
    riseSpeed_ = 0.0f;

    const double chargeRadius =
        std::cbrt(3.0 * std::max(config.chargeMassKg, 1e-6) /
                  (4.0 * 3.14159265358979 * 1630.0));
    depositRadius_ = float(std::max(chargeRadius, 3.5 * double(dx_)));
    extinctionScale_ = sim::VolumeExtinctionScale(config.fireballRadiusM);

    stateIndex_ = 0;
    time_ = 0.0;
    advance_ = 0.0;
    totalSteps_ = 0;
    maxSpeed_ = 4000.0f;
    lastDt_ = 0.0f;
    capThreshold_ = 0.0f;
    reducePending_ = false;
    hasVolume_ = false;
    oldBounds_ = bounds_;
    oldDx_ = dx_;
    initialDx_ = dx_;
    expansions_ = 0;

    UpdateConstants(0, 0.0f);
    commandList_->BeginCompute();
    commandList_->BindPipeline(initPipeline_.get());
    commandList_->BindDescriptorSet(initSet_.get());
    DispatchGrid();
    commandList_->ComputeBarrier();
    commandList_->EndCompute();

    UpdateRenderVolume();

    stats_ = SolverStats{};
    stats_.active = true;
    stats_.resolution = resolution_;
    stats_.time = 0.0;
    stats_.dt = 0.0f;
    stats_.maxSpeed = maxSpeed_;
    stats_.steps = 0;
    stats_.domainHalfExtent = float(half);
    stats_.domainTop = bounds_.max.y;
    stats_.voxelSize = dx_;
    stats_.accuracyFactor = 1.0f;
    stats_.expansions = 0;
    return true;
}

void RhiEulerSolver::Step(int steps) {
    if (!initialized_ || !hasVolume_ || steps <= 0) {
        return;
    }

    int remaining = steps;
    float lastSpeed = std::max(maxSpeed_, 1.0f);
    while (remaining > 0) {
        PollReduction();
        lastSpeed = std::max(maxSpeed_, 1.0f);
        const float dt = std::clamp(kCfl * dx_ / lastSpeed, 1e-7f, 1.0e-3f);
        const int batch = std::min(remaining, kMaxBatchSteps);

        UpdateConstants(0, dt);
        if (void* mapped = reduceBuffer_->Map()) {
            std::memset(mapped, 0, kReduceChannels * sizeof(uint32_t));
            reduceBuffer_->Unmap();
        }

        commandList_->BeginCompute();
        for (int step = 0; step < batch; ++step) {
            // Alternate the dimensional-splitting order per step so the
            // truncation error averages out instead of biasing the surge.
            const bool reverse = (totalSteps_ & 1) != 0;
            for (int i = 0; i < 3; ++i) {
                const int direction = reverse ? 2 - i : i;
                DispatchSweep(direction, i == 2);
            }
            ++totalSteps_;
        }
        if (!reducePending_) {
            DispatchReduce();
        }
        commandList_->EndCompute();

        // EndCompute waits for the submission fence, so the reduction is
        // already available and the CFL step of the next batch sees it.
        if (reducePending_) {
            ConsumeStaging();
            reducePending_ = false;
        }

        time_ += double(dt) * double(batch);
        lastDt_ = dt;
        advance_ += double(dt) * double(batch);
        remaining -= batch;
    }

    stats_.active = true;
    stats_.resolution = resolution_;
    stats_.time = time_;
    stats_.advance = advance_;
    stats_.dt = lastDt_;
    stats_.maxSpeed = lastSpeed;
    stats_.steps = totalSteps_;
}

void RhiEulerSolver::ConsumeStaging() {
    void* mapped = reduceBuffer_->Map();
    if (!mapped) {
        return;
    }
    uint32_t values[kReduceChannels] = {};
    std::memcpy(values, mapped, sizeof(values));
    reduceBuffer_->Unmap();

    const float value = *reinterpret_cast<const float*>(&values[0]);
    if (std::isfinite(value) && value > 1.0f) {
        maxSpeed_ = value;
    }

    if (values[1] > 0) {
        stats_.plumeTop = float(values[1]) / kBoundScale;
    }
    if (values[2] > 0) {
        stats_.plumeRadius = float(values[2]) / kBoundScale;
    }
    if (values[3] > 0) {
        stats_.plumeDomeFraction = float(values[3]) / kBoundScale;
    }
    if (values[4] > 0) {
        stats_.plumeCapRadius = float(values[4]) / kBoundScale;
    }
    if (stats_.plumeTop > 0.0f) {
        capThreshold_ = 0.70f * stats_.plumeTop;
    }
    if (values[1] > 0 || values[2] > 0) {
        stats_.fieldBoundsValid = true;
    }
}

bool RhiEulerSolver::PollReduction() {
    if (!reducePending_) {
        return false;
    }
    ConsumeStaging();
    reducePending_ = false;
    return true;
}

bool RhiEulerSolver::WaitForReduction() {
    return PollReduction();
}

void RhiEulerSolver::UpdateRenderVolume() {
    if (!renderVolume_) {
        return;
    }
    commandList_->BeginCompute();
    commandList_->TransitionTexture(renderVolume_.get(),
                                    ex::rhi::TextureState::General);
    DispatchRender();
    commandList_->EndCompute();
    hasVolume_ = true;
}

void RhiEulerSolver::TransitionToShaderRead() {
    if (!renderVolume_ || !lightVolume_) {
        return;
    }
    commandList_->BeginCompute();
    commandList_->TransitionTexture(renderVolume_.get(),
                                    ex::rhi::TextureState::ShaderRead);
    commandList_->TransitionTexture(lightVolume_.get(),
                                    ex::rhi::TextureState::ShaderRead);
    commandList_->EndCompute();
}

void RhiEulerSolver::UpdateLightVolume(const XMFLOAT3& sunDirection) {
    if (!initialized_ || !hasVolume_ || !lightVolume_ || !lightPipeline_) {
        return;
    }
    sunDirection_ = sunDirection;
    UpdateConstants(0, 0.0f);

    commandList_->BeginCompute();
    commandList_->TransitionTexture(lightVolume_.get(),
                                    ex::rhi::TextureState::General);
    commandList_->BindPipeline(lightPipeline_.get());
    commandList_->BindDescriptorSet(lightSet_.get());
    const uint32_t groups = (kLightVolumeResolution + 3) / 4;
    commandList_->Dispatch(groups, groups, groups);
    commandList_->ComputeBarrier();
    commandList_->EndCompute();
}

bool RhiEulerSolver::RescaleDomain(float factor) {
    if (!initialized_ || !hasVolume_ || factor <= 1.0f) {
        return false;
    }

    const sim::VolumeBounds previous = bounds_;
    const float previousDx = dx_;
    const float centerX = 0.5f * (bounds_.min.x + bounds_.max.x);
    const float centerZ = 0.5f * (bounds_.min.z + bounds_.max.z);
    const float half = 0.5f * (bounds_.max.x - bounds_.min.x);
    const float newHalf = half * factor;
    const float newTop = bounds_.max.y * factor;

    bounds_.min = XMFLOAT3(centerX - newHalf, 0.0f, centerZ - newHalf);
    bounds_.max = XMFLOAT3(centerX + newHalf, newTop, centerZ + newHalf);
    dx_ = 2.0f * newHalf / float(resolution_);

    oldBounds_ = previous;
    oldDx_ = previousDx;

    UpdateConstants(0, 0.0f);
    commandList_->BeginCompute();
    commandList_->BindPipeline(rescalePipeline_.get());
    commandList_->BindDescriptorSet(rescaleSets_[stateIndex_].get());
    DispatchGrid();
    commandList_->ComputeBarrier();
    commandList_->EndCompute();
    stateIndex_ = 1 - stateIndex_;

    oldBounds_ = bounds_;
    oldDx_ = dx_;
    ++expansions_;

    stats_.domainHalfExtent = newHalf;
    stats_.domainTop = newTop;
    stats_.voxelSize = dx_;
    stats_.accuracyFactor = dx_ / std::max(initialDx_, 1e-6f);
    stats_.expansions = expansions_;

    LogInfo("High-fidelity domain expanded: half %.2f m, top %.2f m, voxel %.1f mm, "
            "accuracy x%.2f",
            newHalf, newTop, dx_ * 1000.0f, stats_.accuracyFactor);
    return true;
}

double RhiEulerSolver::ConsumeAdvance() {
    const double advance = advance_;
    advance_ = 0.0;
    stats_.advance = 0.0;
    return advance;
}

} // namespace ex::gfx

#include "gfx/GpuEulerSolver.h"

#include "core/Log.h"
#include "gfx/Shader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

using namespace DirectX;

namespace ex::gfx {

namespace {

constexpr int kThreadGroupX = 4;
constexpr int kThreadGroupY = 4;
constexpr int kThreadGroupZ = 4;
constexpr float kCooling = 2.0e4f;
constexpr float kCfl = 0.20f;

bool CreateVolumeTexture(ID3D11Device* device, int resolution, DXGI_FORMAT format,
                         ID3D11Texture3D** texture, ID3D11ShaderResourceView** srv,
                         ID3D11UnorderedAccessView** uav) {
    D3D11_TEXTURE3D_DESC desc{};
    desc.Width = UINT(resolution);
    desc.Height = UINT(resolution);
    desc.Depth = UINT(resolution);
    desc.MipLevels = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    if (FAILED(device->CreateTexture3D(&desc, nullptr, texture))) {
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(*texture, nullptr, srv))) {
        return false;
    }
    if (FAILED(device->CreateUnorderedAccessView(*texture, nullptr, uav))) {
        return false;
    }
    return true;
}

// D3D11 returns S_FALSE from GetData while a query is still in flight. The
// benchmark resolves its timings once, after all work was submitted, so polling
// until the GPU catches up is fine here.
template <typename T>
bool ReadQueryBlocking(ID3D11DeviceContext* context, ID3D11Query* query, T& data) {
    for (int attempt = 0; attempt < 10000; ++attempt) {
        const HRESULT result = context->GetData(query, &data, sizeof(T), 0);
        if (result == S_OK) {
            return true;
        }
        if (result != S_FALSE) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

bool GpuEulerSolver::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                uint64_t dedicatedVideoMemoryBytes) {
    device_ = device;
    context_ = context;
    videoMemoryBytes_ = dedicatedVideoMemoryBytes;
    initialized_ = CreateComputeShaders() && CreateAuxiliaryResources();
    return initialized_;
}

bool GpuEulerSolver::CreateComputeShaders() {
    // The group size is compiled into every solver shader. Keeping it in one
    // place makes occupancy experiments a one-line change.
    const std::string groupX = std::to_string(kThreadGroupX);
    const std::string groupY = std::to_string(kThreadGroupY);
    const std::string groupZ = std::to_string(kThreadGroupZ);
    const D3D_SHADER_MACRO defines[] = {
        {"EULER_GROUP_X", groupX.c_str()},
        {"EULER_GROUP_Y", groupY.c_str()},
        {"EULER_GROUP_Z", groupZ.c_str()},
        {nullptr, nullptr},
    };

    auto compile = [&defines](const wchar_t* relativePath, const char* entry) {
        const std::wstring path = ResolveAssetPath(relativePath);
        if (path.empty()) {
            LogError("Compute shader not found: %ls", relativePath);
            return ComPtr<ID3DBlob>{};
        }
        std::string errors;
        return CompileShaderFile(path, entry, "cs_5_0", errors, defines);
    };

    auto initBlob = compile(L"assets\\shaders\\euler_init.hlsl", "CSInit");
    auto sweepXBlob = compile(L"assets\\shaders\\euler_sweep.hlsl", "CSSweepX");
    auto sweepYBlob = compile(L"assets\\shaders\\euler_sweep.hlsl", "CSSweepY");
    auto sweepZBlob = compile(L"assets\\shaders\\euler_sweep.hlsl", "CSSweepZ");
    auto sweepXSourceBlob =
        compile(L"assets\\shaders\\euler_sweep.hlsl", "CSSweepXSource");
    auto sweepYSourceBlob =
        compile(L"assets\\shaders\\euler_sweep.hlsl", "CSSweepYSource");
    auto sweepZSourceBlob =
        compile(L"assets\\shaders\\euler_sweep.hlsl", "CSSweepZSource");
    auto reduceBlob = compile(L"assets\\shaders\\euler_reduce.hlsl", "CSReduce");
    auto renderBlob = compile(L"assets\\shaders\\euler_render.hlsl", "CSRender");
    auto rescaleBlob = compile(L"assets\\shaders\\euler_rescale.hlsl", "CSRescale");
    auto lightBlob = compile(L"assets\\shaders\\euler_light.hlsl", "CSLight");

    if (!initBlob || !sweepXBlob || !sweepYBlob || !sweepZBlob ||
        !sweepXSourceBlob || !sweepYSourceBlob || !sweepZSourceBlob ||
        !reduceBlob || !renderBlob || !rescaleBlob || !lightBlob) {
        LogError("High-fidelity solver: shader compilation failed.");
        return false;
    }

    auto create = [this](ID3DBlob* blob, ComPtr<ID3D11ComputeShader>& shader) {
        return blob && SUCCEEDED(device_->CreateComputeShader(blob->GetBufferPointer(),
                                                             blob->GetBufferSize(), nullptr,
                                                             &shader));
    };

    if (!create(initBlob.Get(), initShader_) ||
        !create(sweepXBlob.Get(), sweepShaders_[0]) ||
        !create(sweepYBlob.Get(), sweepShaders_[1]) ||
        !create(sweepZBlob.Get(), sweepShaders_[2]) ||
        !create(sweepXSourceBlob.Get(), sweepSourceShaders_[0]) ||
        !create(sweepYSourceBlob.Get(), sweepSourceShaders_[1]) ||
        !create(sweepZSourceBlob.Get(), sweepSourceShaders_[2]) ||
        !create(reduceBlob.Get(), reduceShader_) ||
        !create(renderBlob.Get(), renderShader_) || !create(rescaleBlob.Get(), rescaleShader_)) {
        LogError("High-fidelity solver: failed to create compute shader objects.");
        return false;
    }
    if (!create(lightBlob.Get(), lightShader_)) {
        LogError("High-fidelity solver: failed to create the light shader object.");
        return false;
    }
    return true;
}

bool GpuEulerSolver::CreateAuxiliaryResources() {
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(Constants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (auto& buffer : constantBuffers_) {
        if (FAILED(device_->CreateBuffer(&cbDesc, nullptr, buffer.GetAddressOf()))) {
            LogError("High-fidelity solver: constant buffer creation failed.");
            return false;
        }
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = kReduceChannels * sizeof(uint32_t);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = sizeof(uint32_t);
    if (FAILED(device_->CreateBuffer(&bufferDesc, nullptr, &maxSpeedBuffer_))) {
        LogError("High-fidelity solver: max-speed buffer creation failed.");
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = kReduceChannels;
    if (FAILED(device_->CreateUnorderedAccessView(maxSpeedBuffer_.Get(), &uavDesc,
                                                  &maxSpeedUAV_))) {
        LogError("High-fidelity solver: max-speed UAV creation failed.");
        return false;
    }

    D3D11_BUFFER_DESC stagingDesc{};
    stagingDesc.ByteWidth = kReduceChannels * sizeof(uint32_t);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    stagingDesc.StructureByteStride = sizeof(uint32_t);
    if (FAILED(device_->CreateBuffer(&stagingDesc, nullptr, &maxSpeedStaging_))) {
        LogError("High-fidelity solver: max-speed staging buffer creation failed.");
        return false;
    }

    D3D11_QUERY_DESC queryDesc{};
    queryDesc.Query = D3D11_QUERY_EVENT;
    if (FAILED(device_->CreateQuery(&queryDesc, &reduceQuery_))) {
        LogError("High-fidelity solver: reduction query creation failed.");
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&samplerDesc, &linearSampler_))) {
        LogError("High-fidelity solver: sampler creation failed.");
        return false;
    }
    return true;
}

bool GpuEulerSolver::CreateTextures(int resolution) {
    ReleaseVolumeResources();
    resolution_ = std::clamp(resolution, 32, 512);

    // 4 state textures (two ping-pong sets: A RGBA32F, B RG32F) + render
    // volume (RGBA16F).
    const uint64_t voxels = uint64_t(resolution_) * resolution_ * resolution_;
    const uint64_t requiredBytes = voxels * 16ull * 2ull + voxels * 8ull * 2ull +
                                   voxels * 8ull;
    if (videoMemoryBytes_ > 0 && requiredBytes > videoMemoryBytes_ * 3ull / 4ull) {
        LogError("High-fidelity solver: %d^3 needs %.2f GB of VRAM, but only %.2f GB are "
                 "available. Reduce the solver grid.",
                 resolution_, double(requiredBytes) / (1024.0 * 1024.0 * 1024.0),
                 double(videoMemoryBytes_) / (1024.0 * 1024.0 * 1024.0));
        return false;
    }

    for (int i = 0; i < 2; ++i) {
        if (!CreateVolumeTexture(device_, resolution_, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                 &stateA_[i].texture, &stateA_[i].srv, &stateA_[i].uav) ||
            !CreateVolumeTexture(device_, resolution_, DXGI_FORMAT_R32G32_FLOAT,
                                 &stateB_[i].texture, &stateB_[i].srv, &stateB_[i].uav)) {
            LogError("High-fidelity solver: state texture creation failed.");
            return false;
        }
    }

    if (!CreateVolumeTexture(device_, resolution_, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             &renderVolume_.texture, &renderVolume_.srv, &renderVolume_.uav)) {
        LogError("High-fidelity solver: render volume creation failed.");
        return false;
    }

    if (!CreateVolumeTexture(device_, kLightVolumeResolution, DXGI_FORMAT_R32_FLOAT,
                             &lightVolume_.texture, &lightVolume_.srv, &lightVolume_.uav)) {
        LogError("High-fidelity solver: light volume creation failed.");
        return false;
    }

    stateIndex_ = 0;
    return true;
}

void GpuEulerSolver::ReleaseVolumeResources() {
    for (auto& set : stateA_) {
        set.Reset();
    }
    for (auto& set : stateB_) {
        set.Reset();
    }
    renderVolume_.Reset();
    lightVolume_.Reset();
    resolution_ = 0;
    hasVolume_ = false;
}

void GpuEulerSolver::Shutdown() {
    ReleaseVolumeResources();
    timingQueries_.clear();
    timingDisjoint_.Reset();
    maxSpeedStaging_.Reset();
    reduceQuery_.Reset();
    maxSpeedUAV_.Reset();
    maxSpeedBuffer_.Reset();
    for (auto& buffer : constantBuffers_) {
        buffer.Reset();
    }
    linearSampler_.Reset();
    rescaleShader_.Reset();
    renderShader_.Reset();
    reduceShader_.Reset();
    lightShader_.Reset();
    for (auto& shader : sweepShaders_) {
        shader.Reset();
    }
    for (auto& shader : sweepSourceShaders_) {
        shader.Reset();
    }
    initShader_.Reset();
    device_ = nullptr;
    context_ = nullptr;
    initialized_ = false;
}

void GpuEulerSolver::UpdateConstants(int direction, float dt) {
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
    constants_.extra = XMFLOAT4(plumeRadius_, plumeCenterY_,
                                config_.hemispherical ? 1.0f : 0.0f, capThreshold_);
    constants_.oldDomainMin =
        XMFLOAT4(oldBounds_.min.x, oldBounds_.min.y, oldBounds_.min.z, oldDx_);
    constants_.oldDomainMax =
        XMFLOAT4(oldBounds_.max.x, oldBounds_.max.y, oldBounds_.max.z, 0.0f);
    constants_.sunDirection = XMFLOAT4(sunDirection_.x, sunDirection_.y, sunDirection_.z, 0.0f);
    constants_.lightParams = XMFLOAT4(extinctionScale_, 24.0f, 0.0f, 0.0f);
    constants_.capParams =
        XMFLOAT4(plumeRadius_, plumeCenterY_, plumeTop_, capThreshold_);
    constants_.envParams = XMFLOAT4(stableLayerY_, initialTop_, 0.0f, 0.0f);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ID3D11Buffer* buffer = constantBuffers_[constantBufferIndex_].Get();
    constantBufferIndex_ = (constantBufferIndex_ + 1) % kConstantBufferCount;
    if (SUCCEEDED(context_->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &constants_, sizeof(constants_));
        context_->Unmap(buffer, 0);
    }

    context_->CSSetConstantBuffers(0, 1, &buffer);
}

void GpuEulerSolver::BindStateSRV(int index) {
    ID3D11ShaderResourceView* views[2] = {stateA_[index].srv.Get(), stateB_[index].srv.Get()};
    context_->CSSetShaderResources(0, 2, views);
}

void GpuEulerSolver::BindStateUAV(int index) {
    ID3D11UnorderedAccessView* views[2] = {stateA_[index].uav.Get(), stateB_[index].uav.Get()};
    context_->CSSetUnorderedAccessViews(0, 2, views, nullptr);
}

void GpuEulerSolver::UnbindStateUAVs() {
    ID3D11UnorderedAccessView* nullUAVs[2] = {nullptr, nullptr};
    context_->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
}

void GpuEulerSolver::UnbindAll() {
    ID3D11ShaderResourceView* nullViews[3] = {nullptr, nullptr, nullptr};
    context_->CSSetShaderResources(0, 3, nullViews);
    ID3D11UnorderedAccessView* nullUAVs[3] = {nullptr, nullptr, nullptr};
    context_->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
}

void GpuEulerSolver::DispatchGrid() {
    context_->Dispatch(UINT((resolution_ + kThreadGroupX - 1) / kThreadGroupX),
                       UINT((resolution_ + kThreadGroupY - 1) / kThreadGroupY),
                       UINT((resolution_ + kThreadGroupZ - 1) / kThreadGroupZ));
}

void GpuEulerSolver::DispatchSweep(int direction, bool applySource) {
    ID3D11ComputeShader* shader = applySource ? sweepSourceShaders_[direction].Get()
                                              : sweepShaders_[direction].Get();
    context_->CSSetShader(shader, nullptr, 0);
    BindStateSRV(stateIndex_);
    BindStateUAV(1 - stateIndex_);
    BeginPass(SolverPass(int(SolverPass::SweepX) + direction));
    DispatchGrid();
    EndPass();
    // Only the state UAVs need unbinding: the next pass reads the other
    // ping-pong set, and leaving them bound would overlap SRV and UAV slots.
    UnbindStateUAVs();
    stateIndex_ = 1 - stateIndex_;
}

void GpuEulerSolver::BeginPass(SolverPass pass) {
    if (!timingActive_ || timingIndex_ + 1 >= timingQueryLimit_) {
        return;
    }
    timingLabels_.push_back(uint8_t(pass));
    context_->End(timingQueries_[timingIndex_++].Get());
    timingOpenPass_ = int(timingLabels_.size()) - 1;
}

void GpuEulerSolver::EndPass() {
    if (!timingActive_ || timingOpenPass_ < 0) {
        return;
    }
    context_->End(timingQueries_[timingIndex_++].Get());
    timingOpenPass_ = -1;
}

bool GpuEulerSolver::BeginTiming(int maxPasses) {
    if (!initialized_ || !context_ || maxPasses <= 0) {
        return false;
    }
    EndTiming();

    const int queryCount = maxPasses * 2;
    if (int(timingQueries_.size()) < queryCount) {
        timingQueries_.clear();
        timingQueries_.resize(size_t(queryCount));
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& query : timingQueries_) {
            if (FAILED(device_->CreateQuery(&desc, &query))) {
                timingQueries_.clear();
                return false;
            }
        }
    }

    if (!timingDisjoint_) {
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(device_->CreateQuery(&desc, &timingDisjoint_))) {
            return false;
        }
    }

    timingLabels_.clear();
    timingLabels_.reserve(size_t(maxPasses));
    timingQueryLimit_ = queryCount;
    timingIndex_ = 0;
    timingOpenPass_ = -1;
    timingActive_ = true;
    context_->Begin(timingDisjoint_.Get());
    return true;
}

void GpuEulerSolver::EndTiming() {
    if (!timingActive_) {
        return;
    }
    context_->End(timingDisjoint_.Get());
    timingActive_ = false;
    timingOpenPass_ = -1;
}

SolverTiming GpuEulerSolver::ResolveTiming() {
    SolverTiming timing;
    EndTiming();
    if (timingIndex_ < 2 || timingLabels_.empty() || !timingDisjoint_) {
        return timing;
    }

    // This is the only blocking readback in the benchmark; it happens once after
    // all steps have been submitted.
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    if (!ReadQueryBlocking(context_, timingDisjoint_.Get(), disjoint) ||
        disjoint.Disjoint || disjoint.Frequency == 0) {
        LogWarn("Solver timing: disjoint readback failed (disjoint=%d, frequency=%llu, "
                "passes=%zu, index=%d).",
                int(disjoint.Disjoint), static_cast<unsigned long long>(disjoint.Frequency),
                timingLabels_.size(), timingIndex_);
        return timing;
    }

    std::vector<uint64_t> stamps;
    stamps.resize(size_t(timingIndex_));
    for (int i = 0; i < timingIndex_; ++i) {
        if (!ReadQueryBlocking(context_, timingQueries_[size_t(i)].Get(),
                               stamps[size_t(i)])) {
            LogWarn("Solver timing: timestamp %d readback failed.", i);
            return timing;
        }
    }

    const double toMs = 1000.0 / double(disjoint.Frequency);
    timing.valid = true;
    timing.totalMs = double(stamps[size_t(timingIndex_ - 1)] - stamps[0]) * toMs;
    for (size_t pass = 0; pass < timingLabels_.size(); ++pass) {
        const size_t index = pass * 2;
        if (index + 1 >= size_t(timingIndex_)) {
            break;
        }
        const int label = timingLabels_[pass];
        timing.passMs[label] += double(stamps[index + 1] - stamps[index]) * toMs;
        ++timing.passCount[label];
    }
    return timing;
}

void GpuEulerSolver::DispatchReduce() {
    const UINT zeros[kReduceChannels] = {};
    context_->ClearUnorderedAccessViewUint(maxSpeedUAV_.Get(), zeros);
    context_->CSSetShader(reduceShader_.Get(), nullptr, 0);
    BindStateSRV(stateIndex_);

    ID3D11UnorderedAccessView* uavs[1] = {maxSpeedUAV_.Get()};
    context_->CSSetUnorderedAccessViews(2, 1, uavs, nullptr);

    BeginPass(SolverPass::Reduce);
    DispatchGrid();
    EndPass();

    // The reduction buffer must not be bound as a UAV while it is copied to the
    // staging buffer.
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context_->CSSetUnorderedAccessViews(2, 1, &nullUAV, nullptr);

    context_->CopyResource(maxSpeedStaging_.Get(), maxSpeedBuffer_.Get());
    if (deterministic_) {
        ConsumeStaging();
        reducePending_ = false;
    } else {
        context_->End(reduceQuery_.Get());
        reducePending_ = true;
    }
}

void GpuEulerSolver::DispatchRender() {
    if (!renderVolume_.uav) {
        return;
    }

    context_->CSSetShader(renderShader_.Get(), nullptr, 0);
    // The render pass reads Physics.w (simulation time), which has advanced
    // since the step constants were written. Update once per frame.
    UpdateConstants(0, 0.0f);
    BindStateSRV(stateIndex_);

    ID3D11UnorderedAccessView* uavs[1] = {renderVolume_.uav.Get()};
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    BeginPass(SolverPass::Render);
    DispatchGrid();
    EndPass();
    UnbindAll();
}

bool GpuEulerSolver::PollReduction() {
    if (deterministic_ || !reducePending_ || !reduceQuery_) {
        return false;
    }

    // Interactive path: never block the CPU on the GPU. If the copy has not
    // completed yet, keep the previous maximum speed and try again after the
    // next sub-batch.
    BOOL complete = FALSE;
    const HRESULT queryResult = context_->GetData(reduceQuery_.Get(), &complete,
                                                  sizeof(complete),
                                                  D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (queryResult != S_OK || !complete) {
        return false;
    }

    ConsumeStaging();
    reducePending_ = false;
    return true;
}

bool GpuEulerSolver::WaitForReduction() {
    if (!reducePending_ || !reduceQuery_) {
        return false;
    }
    for (int attempt = 0; attempt < 10000; ++attempt) {
        if (PollReduction()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void GpuEulerSolver::ConsumeStaging() {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(maxSpeedStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }

    uint32_t values[kReduceChannels] = {};
    memcpy(values, mapped.pData, sizeof(values));
    context_->Unmap(maxSpeedStaging_.Get(), 0);

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
    // Measure the cap radius above the base surge next time. The threshold is
    // derived from the measured top, so it follows the rising plume.
    if (stats_.plumeTop > 0.0f) {
        capThreshold_ = 0.70f * stats_.plumeTop;
    }
    if (values[1] > 0 || values[2] > 0) {
        stats_.fieldBoundsValid = true;
    }
}

bool GpuEulerSolver::Reset(const EulerConfig& config) {
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
    stableLayerY_ = 0.95f * initialTop_;
    plumeTop_ = 0.0f;

    const double chargeRadius =
        std::cbrt(3.0 * std::max(config.chargeMassKg, 1e-6) / (4.0 * 3.14159265358979 * 1630.0));
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

    // Initial condition.
    context_->CSSetShader(initShader_.Get(), nullptr, 0);
    UpdateConstants(0, 0.0f);
    BindStateUAV(0);
    DispatchGrid();
    UnbindAll();

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

void GpuEulerSolver::Step(int steps) {
    if (!initialized_ || !hasVolume_ || steps <= 0) {
        return;
    }

    // Splitting the batch lets the solver react to a rising maximum signal
    // speed without a blocking readback. The reduction from the previous
    // sub-batch is consumed here when the GPU has already finished it.
    int remaining = steps;
    float lastSpeed = std::max(maxSpeed_, 1.0f);
    while (remaining > 0) {
        PollReduction();
        lastSpeed = std::max(maxSpeed_, 1.0f);
        const float dt = std::clamp(kCfl * dx_ / lastSpeed, 1e-7f, 1.0e-3f);
        const int batch = std::min(remaining, kMaxBatchSteps);

        // The sweep and source constants only change when dt changes, so one
        // update per sub-batch covers all three sweeps and the reduction.
        UpdateConstants(0, dt);

        for (int step = 0; step < batch; ++step) {
            // Alternate the dimensional-splitting order so the truncation error
            // averages out instead of biasing the base surge along the axes.
            const bool reverse = (totalSteps_ & 1) != 0;
            for (int i = 0; i < 3; ++i) {
                const int direction = reverse ? 2 - i : i;
                DispatchSweep(direction, i == 2);
            }

            time_ += double(dt);
            ++totalSteps_;
        }

        lastDt_ = dt;
        advance_ += double(dt) * double(batch);
        remaining -= batch;

        // At most one reduction may be in flight; otherwise the staging copy
        // would overwrite a result that has not been consumed yet.
        if (!reducePending_) {
            DispatchReduce();
        }
    }

    stats_.active = true;
    stats_.resolution = resolution_;
    stats_.time = time_;
    stats_.advance = advance_;
    stats_.dt = lastDt_;
    stats_.maxSpeed = lastSpeed;
    stats_.steps = totalSteps_;
}

void GpuEulerSolver::UpdateRenderVolume() {
    if (!renderVolume_.uav) {
        return;
    }
    DispatchRender();
    hasVolume_ = true;
}

void GpuEulerSolver::UpdateLightVolume(const XMFLOAT3& sunDirection) {
    if (!initialized_ || !hasVolume_ || !lightVolume_.uav || !lightShader_) {
        return;
    }
    sunDirection_ = sunDirection;
    context_->CSSetShader(lightShader_.Get(), nullptr, 0);
    UpdateConstants(0, 0.0f);

    ID3D11ShaderResourceView* render = renderVolume_.srv.Get();
    context_->CSSetShaderResources(2, 1, &render);
    ID3D11UnorderedAccessView* light = lightVolume_.uav.Get();
    context_->CSSetUnorderedAccessViews(2, 1, &light, nullptr);

    const int groups = (kLightVolumeResolution + 3) / 4;
    context_->Dispatch(groups, groups, groups);
    UnbindAll();
}

bool GpuEulerSolver::RescaleDomain(float factor) {
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

    // The remap reads the old state and writes the other ping-pong set. The
    // constants carry both the old and the new domain.
    oldBounds_ = previous;
    oldDx_ = previousDx;

    context_->CSSetShader(rescaleShader_.Get(), nullptr, 0);
    UpdateConstants(0, 0.0f);
    BindStateSRV(stateIndex_);
    BindStateUAV(1 - stateIndex_);
    ID3D11SamplerState* sampler = linearSampler_.Get();
    context_->CSSetSamplers(0, 1, &sampler);
    DispatchGrid();
    UnbindAll();
    ID3D11SamplerState* nullSampler = nullptr;
    context_->CSSetSamplers(0, 1, &nullSampler);
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

double GpuEulerSolver::ConsumeAdvance() {
    const double advance = advance_;
    advance_ = 0.0;
    stats_.advance = 0.0;
    return advance;
}

} // namespace ex::gfx

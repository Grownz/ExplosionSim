#include "gfx/GpuEulerSolver.h"

#include "core/Log.h"
#include "gfx/Shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace ex::gfx {

namespace {

constexpr int kThreadGroupSize = 8;
constexpr float kCooling = 2.0e4f;
constexpr float kCfl = 0.30f;

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
    auto compile = [](const wchar_t* relativePath, const char* entry) {
        const std::wstring path = ResolveAssetPath(relativePath);
        if (path.empty()) {
            LogError("Compute shader not found: %ls", relativePath);
            return ComPtr<ID3DBlob>{};
        }
        std::string errors;
        return CompileShaderFile(path, entry, "cs_5_0", errors);
    };

    auto initBlob = compile(L"assets\\shaders\\euler_init.hlsl", "CSInit");
    auto sweepBlob = compile(L"assets\\shaders\\euler_sweep.hlsl", "CSSweep");
    auto sourceBlob = compile(L"assets\\shaders\\euler_source.hlsl", "CSSource");
    auto reduceBlob = compile(L"assets\\shaders\\euler_reduce.hlsl", "CSReduce");
    auto renderBlob = compile(L"assets\\shaders\\euler_render.hlsl", "CSRender");

    if (!initBlob || !sweepBlob || !sourceBlob || !reduceBlob || !renderBlob) {
        LogError("High-fidelity solver: shader compilation failed.");
        return false;
    }

    auto create = [this](ID3DBlob* blob, ComPtr<ID3D11ComputeShader>& shader) {
        return blob && SUCCEEDED(device_->CreateComputeShader(blob->GetBufferPointer(),
                                                             blob->GetBufferSize(), nullptr,
                                                             &shader));
    };

    if (!create(initBlob.Get(), initShader_) || !create(sweepBlob.Get(), sweepShader_) ||
        !create(sourceBlob.Get(), sourceShader_) || !create(reduceBlob.Get(), reduceShader_) ||
        !create(renderBlob.Get(), renderShader_)) {
        LogError("High-fidelity solver: failed to create compute shader objects.");
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
    if (FAILED(device_->CreateBuffer(&cbDesc, nullptr, &constantBuffer_))) {
        LogError("High-fidelity solver: constant buffer creation failed.");
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(uint32_t);
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
    uavDesc.Buffer.NumElements = 1;
    if (FAILED(device_->CreateUnorderedAccessView(maxSpeedBuffer_.Get(), &uavDesc,
                                                  &maxSpeedUAV_))) {
        LogError("High-fidelity solver: max-speed UAV creation failed.");
        return false;
    }

    D3D11_BUFFER_DESC stagingDesc{};
    stagingDesc.ByteWidth = sizeof(uint32_t);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    stagingDesc.StructureByteStride = sizeof(uint32_t);
    if (FAILED(device_->CreateBuffer(&stagingDesc, nullptr, &maxSpeedStaging_))) {
        LogError("High-fidelity solver: max-speed staging buffer creation failed.");
        return false;
    }
    return true;
}

bool GpuEulerSolver::CreateTextures(int resolution) {
    ReleaseVolumeResources();
    resolution_ = std::clamp(resolution, 32, 512);

    // 4 state textures (two ping-pong sets, RGBA32F) + render volume (RGBA16F).
    const uint64_t voxels = uint64_t(resolution_) * resolution_ * resolution_;
    const uint64_t requiredBytes = voxels * 16ull * 4ull + voxels * 8ull;
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
            !CreateVolumeTexture(device_, resolution_, DXGI_FORMAT_R32G32B32A32_FLOAT,
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
    resolution_ = 0;
    hasVolume_ = false;
}

void GpuEulerSolver::Shutdown() {
    ReleaseVolumeResources();
    maxSpeedStaging_.Reset();
    maxSpeedUAV_.Reset();
    maxSpeedBuffer_.Reset();
    constantBuffer_.Reset();
    renderShader_.Reset();
    reduceShader_.Reset();
    sourceShader_.Reset();
    sweepShader_.Reset();
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
    constants_.extra = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &constants_, sizeof(constants_));
        context_->Unmap(constantBuffer_.Get(), 0);
    }

    ID3D11Buffer* buffers[1] = {constantBuffer_.Get()};
    context_->CSSetConstantBuffers(0, 1, buffers);
}

void GpuEulerSolver::BindStateSRV(int index) {
    ID3D11ShaderResourceView* views[2] = {stateA_[index].srv.Get(), stateB_[index].srv.Get()};
    context_->CSSetShaderResources(0, 2, views);
}

void GpuEulerSolver::BindStateUAV(int index) {
    ID3D11UnorderedAccessView* views[2] = {stateA_[index].uav.Get(), stateB_[index].uav.Get()};
    context_->CSSetUnorderedAccessViews(0, 2, views, nullptr);
}

void GpuEulerSolver::UnbindAll() {
    ID3D11ShaderResourceView* nullViews[3] = {nullptr, nullptr, nullptr};
    context_->CSSetShaderResources(0, 3, nullViews);
    ID3D11UnorderedAccessView* nullUAVs[3] = {nullptr, nullptr, nullptr};
    context_->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
}

void GpuEulerSolver::Dispatch(int groups) {
    context_->Dispatch(UINT(groups), UINT(groups), UINT(groups));
}

void GpuEulerSolver::DispatchSweep(int direction, float dt) {
    context_->CSSetShader(sweepShader_.Get(), nullptr, 0);
    UpdateConstants(direction, dt);
    BindStateSRV(stateIndex_);
    BindStateUAV(1 - stateIndex_);
    Dispatch((resolution_ + kThreadGroupSize - 1) / kThreadGroupSize);
    UnbindAll();
    stateIndex_ = 1 - stateIndex_;
}

void GpuEulerSolver::DispatchSource(float dt) {
    context_->CSSetShader(sourceShader_.Get(), nullptr, 0);
    UpdateConstants(0, dt);
    BindStateSRV(stateIndex_);
    BindStateUAV(1 - stateIndex_);
    Dispatch((resolution_ + kThreadGroupSize - 1) / kThreadGroupSize);
    UnbindAll();
    stateIndex_ = 1 - stateIndex_;
}

void GpuEulerSolver::DispatchReduce() {
    const UINT zeros[4] = {0, 0, 0, 0};
    context_->ClearUnorderedAccessViewUint(maxSpeedUAV_.Get(), zeros);

    context_->CSSetShader(reduceShader_.Get(), nullptr, 0);
    UpdateConstants(0, 0.0f);
    BindStateSRV(stateIndex_);

    ID3D11UnorderedAccessView* uavs[1] = {maxSpeedUAV_.Get()};
    context_->CSSetUnorderedAccessViews(2, 1, uavs, nullptr);

    Dispatch((resolution_ + kThreadGroupSize - 1) / kThreadGroupSize);
    UnbindAll();

    context_->CopyResource(maxSpeedStaging_.Get(), maxSpeedBuffer_.Get());
    stagingReady_ = true;
}

void GpuEulerSolver::DispatchRender() {
    if (!renderVolume_.uav) {
        return;
    }

    context_->CSSetShader(renderShader_.Get(), nullptr, 0);
    UpdateConstants(0, 0.0f);
    BindStateSRV(stateIndex_);

    ID3D11UnorderedAccessView* uavs[1] = {renderVolume_.uav.Get()};
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    Dispatch((resolution_ + kThreadGroupSize - 1) / kThreadGroupSize);
    UnbindAll();
}

float GpuEulerSolver::ReadMaxSpeed() {
    if (!stagingReady_) {
        return maxSpeed_;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(maxSpeedStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        uint32_t bits = 0;
        memcpy(&bits, mapped.pData, sizeof(bits));
        const float value = *reinterpret_cast<const float*>(&bits);
        if (std::isfinite(value) && value > 1.0f) {
            maxSpeed_ = value;
        }
        context_->Unmap(maxSpeedStaging_.Get(), 0);
    }
    stagingReady_ = false;
    return maxSpeed_;
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

    const double chargeRadius =
        std::cbrt(3.0 * std::max(config.chargeMassKg, 1e-6) / (4.0 * 3.14159265358979 * 1630.0));
    depositRadius_ = float(std::max(chargeRadius, 3.5 * double(dx_)));
    extinctionScale_ = float(0.9 / std::max(config.fireballRadiusM, 0.5));

    stateIndex_ = 0;
    time_ = 0.0;
    advance_ = 0.0;
    totalSteps_ = 0;
    maxSpeed_ = 4000.0f;
    lastDt_ = 0.0f;
    stagingReady_ = false;
    hasVolume_ = false;

    // Initial condition.
    context_->CSSetShader(initShader_.Get(), nullptr, 0);
    UpdateConstants(0, 0.0f);
    BindStateUAV(0);
    Dispatch((resolution_ + kThreadGroupSize - 1) / kThreadGroupSize);
    UnbindAll();

    UpdateRenderVolume();

    stats_ = SolverStats{};
    stats_.active = true;
    stats_.resolution = resolution_;
    stats_.time = 0.0;
    stats_.dt = 0.0f;
    stats_.maxSpeed = maxSpeed_;
    stats_.steps = 0;
    return true;
}

void GpuEulerSolver::Step(int steps) {
    if (!initialized_ || !hasVolume_ || steps <= 0) {
        return;
    }

    const float maxSpeed = std::max(ReadMaxSpeed(), 1.0f);
    const float dt = std::clamp(kCfl * dx_ / maxSpeed, 1e-7f, 5e-5f);

    for (int step = 0; step < steps; ++step) {
        for (int direction = 0; direction < 3; ++direction) {
            DispatchSweep(direction, dt);
        }
        DispatchSource(dt);

        time_ += double(dt);
        ++totalSteps_;
    }

    DispatchReduce();
    UpdateRenderVolume();

    lastDt_ = dt;
    advance_ += double(dt) * double(steps);

    stats_.active = true;
    stats_.resolution = resolution_;
    stats_.time = time_;
    stats_.advance = advance_;
    stats_.dt = lastDt_;
    stats_.maxSpeed = maxSpeed;
    stats_.steps = totalSteps_;
}

void GpuEulerSolver::UpdateRenderVolume() {
    if (!renderVolume_.uav) {
        return;
    }
    DispatchRender();
    hasVolume_ = true;
}

double GpuEulerSolver::ConsumeAdvance() {
    const double advance = advance_;
    advance_ = 0.0;
    stats_.advance = 0.0;
    return advance;
}

} // namespace ex::gfx

#pragma once

#include "gfx/rhi/Rhi.h"
#include "sim/VolumeBaker.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ex::gfx {

// RHI compute path for the analytic volume bake. It owns a storage 3D texture
// and dispatches volume_bake.hlsl; the renderer samples the result directly.
class GpuVolumeBaker {
public:
    bool Initialize(ex::rhi::Device* device, const void* shaderData,
                    size_t shaderSize);
    void Shutdown();

    bool Bake(const ex::sim::VolumeBakeRequest& request);

    ex::rhi::Texture* Texture() const { return texture_.get(); }
    const ex::sim::VolumeBounds& Bounds() const { return bounds_; }
    float Extinction() const { return extinction_; }
    uint64_t Version() const { return version_; }
    bool Valid() const { return texture_ != nullptr; }

private:
    bool CreateSolidMask(const void* data, int resolution);

    struct Constants {
        DirectX::XMFLOAT4 boundsMin;   // w = resolution
        DirectX::XMFLOAT4 boundsSize;  // w = voxel size
        DirectX::XMFLOAT4 fireball;    // w = radius
        DirectX::XMFLOAT4 fireball2;   // x temp, y visible, z time, w ambient
        DirectX::XMFLOAT4 cloud;       // w = radius
        DirectX::XMFLOAT4 cloud2;      // x temp, y stem, z strength, w dust ring
        DirectX::XMFLOAT4 shock;       // x radius, y mach, z burstY, w dust
        DirectX::XMFLOAT4 maskParams;  // x = solid mask enabled
        DirectX::XMFLOAT4 maskMin;     // xyz = world-space mask box minimum
        DirectX::XMFLOAT4 maskMax;     // xyz = world-space mask box maximum
    };

    ex::rhi::Device* device_ = nullptr;
    std::unique_ptr<ex::rhi::CommandList> commandList_;
    std::unique_ptr<ex::rhi::Shader> shader_;
    std::unique_ptr<ex::rhi::Pipeline> pipeline_;
    std::unique_ptr<ex::rhi::Buffer> constants_;
    std::unique_ptr<ex::rhi::DescriptorSet> descriptorSet_;
    std::unique_ptr<ex::rhi::Texture> texture_;
    std::unique_ptr<ex::rhi::Texture> solidMask_;
    uint64_t solidMaskVersion_ = 0;
    ex::sim::VolumeBounds bounds_{};
    float extinction_ = 0.1f;
    int resolution_ = 0;
    uint64_t version_ = 0;
};

} // namespace ex::gfx

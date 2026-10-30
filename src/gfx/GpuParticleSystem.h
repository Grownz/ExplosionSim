#pragma once

#include "gfx/rhi/Rhi.h"
#include "sim/ParticleSystem.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ex::gfx {

// Compute-driven particle path. The CPU particle system only produces spawns;
// this class integrates the population and builds the three render batches.
class GpuParticleSystem {
public:
    struct ShaderSource {
        const void* data = nullptr;
        size_t size = 0;
    };

    bool Initialize(ex::rhi::Device* device, int capacity,
                    const ShaderSource& apply, const ShaderSource& reset,
                    const ShaderSource& integrate, const ShaderSource& finalize);
    void Shutdown();
    bool Update(ex::sim::ParticleSystem& particles);

    ex::rhi::Buffer* AdditiveBuffer() const { return additive_.get(); }
    ex::rhi::Buffer* AlphaBuffer() const { return alpha_.get(); }
    ex::rhi::Buffer* FireBuffer() const { return fire_.get(); }
    ex::rhi::Buffer* IndirectBuffer() const { return indirect_.get(); }
    int Capacity() const { return capacity_; }
    bool Valid() const { return state_ && spawn_ && additive_ && alpha_ &&
                                 fire_ && counters_ && indirect_; }

private:
    bool CreatePipeline(ex::rhi::Device* device, const ShaderSource& source,
                        const char* entryPoint,
                        std::unique_ptr<ex::rhi::Shader>& shader,
                        std::unique_ptr<ex::rhi::Pipeline>& pipeline,
                        std::unique_ptr<ex::rhi::DescriptorSet>& set);

    ex::rhi::Device* device_ = nullptr;
    std::unique_ptr<ex::rhi::CommandList> commandList_;
    std::unique_ptr<ex::rhi::Buffer> constants_;
    std::unique_ptr<ex::rhi::Buffer> state_;
    std::unique_ptr<ex::rhi::Buffer> spawn_;
    std::unique_ptr<ex::rhi::Buffer> additive_;
    std::unique_ptr<ex::rhi::Buffer> alpha_;
    std::unique_ptr<ex::rhi::Buffer> fire_;
    std::unique_ptr<ex::rhi::Buffer> counters_;
    std::unique_ptr<ex::rhi::Buffer> indirect_;

    std::unique_ptr<ex::rhi::Shader> applyShader_;
    std::unique_ptr<ex::rhi::Shader> resetShader_;
    std::unique_ptr<ex::rhi::Shader> integrateShader_;
    std::unique_ptr<ex::rhi::Shader> finalizeShader_;
    std::unique_ptr<ex::rhi::Pipeline> applyPipeline_;
    std::unique_ptr<ex::rhi::Pipeline> resetPipeline_;
    std::unique_ptr<ex::rhi::Pipeline> integratePipeline_;
    std::unique_ptr<ex::rhi::Pipeline> finalizePipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> applySet_;
    std::unique_ptr<ex::rhi::DescriptorSet> resetSet_;
    std::unique_ptr<ex::rhi::DescriptorSet> integrateSet_;
    std::unique_ptr<ex::rhi::DescriptorSet> finalizeSet_;
    int capacity_ = 0;
};

} // namespace ex::gfx

#pragma once

#include "core/FrameProfile.h"
#include "gfx/rhi/Rhi.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ex::gfx {

// Per-pass GPU timestamps. The passes are submitted separately, so a
// timestamp pair recorded inside a render pass measures that submission.
//
// The query pools are double buffered: the window of frame N is resolved at
// the start of frame N+1, when the GPU has almost certainly finished. Waiting
// for the current frame instead would serialize the CPU with the GPU and make
// the measured frame time meaningless.
//
// The timer only allocates query pools when EXPLOSION_FRAME_PROFILE is set;
// otherwise every call is a cheap no-op.
class GpuPassTimer {
public:
    static constexpr uint32_t kMaxTimestamps = 32;

    bool Initialize(rhi::Device* device);
    void Shutdown();

    bool enabled() const { return initialized_; }

    // Resolves the previous frame and starts a new timing window.
    void BeginFrame();

    // Both must be called inside an active render pass.
    void BeginPass(rhi::CommandList* commandList, core::ProfilePhase phase);
    void EndPass(rhi::CommandList* commandList, core::ProfilePhase phase);

    // Flushes the last window, e.g. at the end of a benchmark run.
    void Flush();

private:
    struct Open {
        uint32_t begin = 0;
        core::ProfilePhase phase = core::ProfilePhase::Scene;
    };
    struct Sample {
        uint32_t begin = 0;
        uint32_t end = 0;
        core::ProfilePhase phase = core::ProfilePhase::Scene;
    };
    struct PoolState {
        std::unique_ptr<rhi::QueryPool> pool;
        std::vector<Open> open;
        std::vector<Sample> samples;
        uint32_t nextSlot = 0;
    };

    void Resolve(PoolState& state);

    PoolState pools_[2];
    int current_ = 0;
    bool initialized_ = false;
};

inline bool GpuPassTimer::Initialize(rhi::Device* device) {
    // Reading timestamps back synchronises the GPU, so GPU timing is a separate
    // opt-in on top of the CPU profile.
    if (!device || initialized_ ||
        !core::FrameProfiler::Get().enabled() ||
        !core::EnvironmentFlag("EXPLOSION_GPU_PROFILE")) {
        return false;
    }
    for (PoolState& state : pools_) {
        state.pool = device->CreateQueryPool(kMaxTimestamps);
        if (!state.pool) {
            Shutdown();
            return false;
        }
        state.open.reserve(8);
        state.samples.reserve(16);
    }
    initialized_ = true;
    return true;
}

inline void GpuPassTimer::Shutdown() {
    if (initialized_) {
        // Drain the windows before the pool and device disappear.
        Resolve(pools_[current_]);
        Resolve(pools_[1 - current_]);
    }
    for (PoolState& state : pools_) {
        state.pool.reset();
        state.open.clear();
        state.samples.clear();
        state.nextSlot = 0;
    }
    initialized_ = false;
}

inline void GpuPassTimer::Resolve(PoolState& state) {
    if (!state.pool || state.samples.empty()) {
        return;
    }
    uint64_t values[kMaxTimestamps]{};
    const uint32_t count = state.nextSlot;
    if (!state.pool->Resolve(values, count)) {
        state.samples.clear();
        return;
    }
    for (const Sample& sample : state.samples) {
        if (sample.end >= count || sample.begin >= count) {
            continue;
        }
        const double milliseconds =
            double(values[sample.end] - values[sample.begin]) * 1.0e-6;
        core::FrameProfiler::Get().AddGpu(sample.phase, milliseconds);
    }
    state.samples.clear();
}

inline void GpuPassTimer::BeginFrame() {
    if (!initialized_) {
        return;
    }
    PoolState& previous = pools_[1 - current_];
    Resolve(previous);

    current_ = 1 - current_;
    PoolState& state = pools_[current_];
    state.open.clear();
    state.samples.clear();
    state.nextSlot = 0;
    state.pool->Reset();
}

inline void GpuPassTimer::BeginPass(rhi::CommandList* commandList,
                                    core::ProfilePhase phase) {
    if (!initialized_ || !commandList) {
        return;
    }
    PoolState& state = pools_[current_];
    if (state.nextSlot + 2 > kMaxTimestamps) {
        return;
    }
    const uint32_t slot = state.nextSlot++;
    commandList->WriteTimestamp(state.pool.get(), slot);
    state.open.push_back({slot, phase});
}

inline void GpuPassTimer::EndPass(rhi::CommandList* commandList,
                                  core::ProfilePhase phase) {
    if (!initialized_ || !commandList) {
        return;
    }
    PoolState& state = pools_[current_];
    if (state.open.empty() || state.nextSlot >= kMaxTimestamps) {
        return;
    }
    Open entry{};
    bool matched = false;
    for (size_t i = state.open.size(); i > 0; --i) {
        if (state.open[i - 1].phase == phase) {
            entry = state.open[i - 1];
            state.open.erase(state.open.begin() + std::ptrdiff_t(i - 1));
            matched = true;
            break;
        }
    }
    if (!matched) {
        return;
    }
    const uint32_t slot = state.nextSlot++;
    commandList->WriteTimestamp(state.pool.get(), slot);
    state.samples.push_back({entry.begin, slot, phase});
}

inline void GpuPassTimer::Flush() {
    if (!initialized_) {
        return;
    }
    Resolve(pools_[current_]);
}

} // namespace ex::gfx

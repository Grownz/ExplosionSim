#pragma once

#include "core/Log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace ex::core {

// Reads a boolean environment flag. MSVC deprecates getenv, so the shared
// header uses the secure variant there.
inline bool EnvironmentFlag(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) {
        return false;
    }
    const bool enabled = value[0] != '\0';
    std::free(value);
    return enabled;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
#endif
}

// Per-frame phases shared by the Windows and Linux frame loops. Keeping the
// list here means both applications report the same breakdown in the same
// units, which is what makes the numbers comparable between the backends.
enum class ProfilePhase : int {
    Simulation,
    Physics,
    Obstacles,
    StaticShadow,
    CascadeShadow,
    ChunkInstances,
    Scene,
    Volume,
    Particles,
    Bloom,
    Composite,
    Count,
};

inline const char* ProfilePhaseName(ProfilePhase phase) {
    switch (phase) {
    case ProfilePhase::Simulation:
        return "simulation";
    case ProfilePhase::Physics:
        return "physics";
    case ProfilePhase::Obstacles:
        return "obstacles";
    case ProfilePhase::StaticShadow:
        return "static-shadow";
    case ProfilePhase::CascadeShadow:
        return "cascade-shadow";
    case ProfilePhase::ChunkInstances:
        return "chunk-instances";
    case ProfilePhase::Scene:
        return "scene";
    case ProfilePhase::Volume:
        return "volume";
    case ProfilePhase::Particles:
        return "particles";
    case ProfilePhase::Bloom:
        return "bloom";
    case ProfilePhase::Composite:
        return "composite";
    case ProfilePhase::Count:
        break;
    }
    return "?";
}

// Lightweight phase profiler. It is disabled unless EXPLOSION_FRAME_PROFILE is
// set, so the hot path only pays for a bool check. While enabled it logs one
// line per second with the average milliseconds spent in each phase, which is
// enough to tell a rendering spike from a simulation spike.
class FrameProfiler {
public:
    using Clock = std::chrono::steady_clock;

    static FrameProfiler& Get() {
        static FrameProfiler instance;
        return instance;
    }

    bool enabled() const { return enabled_; }

    // Drops the accumulated samples, e.g. after a benchmark pre-roll.
    void Reset() {
        frames_ = 0;
        windowStart_ = Clock::now();
        for (double& value : accumMs_) {
            value = 0.0;
        }
    }

    void Add(ProfilePhase phase, double milliseconds) {
        accumMs_[int(phase)] += milliseconds;
    }

    // Called once per rendered frame by the application loop.
    void Frame() {
        ++frames_;
        const Clock::time_point now = Clock::now();
        const std::chrono::duration<double> window = now - windowStart_;
        if (window.count() >= 1.0) {
            LogWindow(now);
        }
    }

    // Logs the samples collected so far, e.g. at the end of a benchmark.
    void Flush() {
        if (frames_ > 0) {
            LogWindow(Clock::now());
        }
    }

private:
    void LogWindow(Clock::time_point now) {
        const std::chrono::duration<double> window = now - windowStart_;
        const double seconds = std::max(window.count(), 1e-9);
        const double fps = double(frames_) / seconds;
        char line[512];
        int length = std::snprintf(line, sizeof(line),
                                   "Frame profile: %.2f ms/frame (%.1f fps, "
                                   "%d frames)",
                                   seconds * 1000.0 / double(frames_), fps,
                                   frames_);
        for (int i = 0; i < int(ProfilePhase::Count); ++i) {
            if (length <= 0 || size_t(length) >= sizeof(line)) {
                break;
            }
            length += std::snprintf(
                line + length, sizeof(line) - size_t(length), " %s=%.2f",
                ProfilePhaseName(ProfilePhase(i)),
                accumMs_[i] / double(frames_));
        }
        LogInfo("%s", line);

        frames_ = 0;
        windowStart_ = now;
        for (double& value : accumMs_) {
            value = 0.0;
        }
    }

    FrameProfiler()
        : enabled_(EnvironmentFlag("EXPLOSION_FRAME_PROFILE")),
          windowStart_(Clock::now()) {}

    bool enabled_ = false;
    int frames_ = 0;
    Clock::time_point windowStart_{};
    double accumMs_[int(ProfilePhase::Count)]{};
};

// RAII scope for one phase. Usage: `ProfileScope profile(ProfilePhase::Scene);`
class ProfileScope {
public:
    explicit ProfileScope(ProfilePhase phase)
        : profiler_(FrameProfiler::Get()), phase_(phase),
          active_(profiler_.enabled()) {
        if (active_) {
            start_ = Clock::now();
        }
    }
    ~ProfileScope() {
        if (active_) {
            const double milliseconds =
                std::chrono::duration<double, std::milli>(Clock::now() - start_)
                    .count();
            profiler_.Add(phase_, milliseconds);
        }
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    using Clock = FrameProfiler::Clock;

    FrameProfiler& profiler_;
    ProfilePhase phase_;
    bool active_ = false;
    Clock::time_point start_{};
};

} // namespace ex::core

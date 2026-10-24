#pragma once

#include <DirectXMath.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "sim/BlastModel.h"

namespace ex::core {
class ThreadPool;
}

namespace ex::sim {

struct VolumeBounds {
    DirectX::XMFLOAT3 min{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 max{1.0f, 1.0f, 1.0f};
};

// CPU-side volumetric field: RGB = (density, temperature/12kK, dust mask),
// A = reserved. Resolution is uniform in all three axes.
struct VolumeFrame {
    int resolution = 0;
    VolumeBounds bounds{};
    float extinctionScale = 0.1f;
    uint64_t version = 0;
    std::vector<float> rgba;

    bool empty() const { return resolution <= 0 || rgba.empty(); }
};

// Bakes the analytic blast field into a 3D texture on the thread pool.
class VolumeBaker {
public:
    void Initialize(core::ThreadPool* pool, int resolution);
    void Reset();
    void SetResolution(int resolution);
    int Resolution() const { return resolution_; }

    // Starts an asynchronous bake. Does nothing while another bake is running.
    void StartBake(const BlastModel& model);

    // Bakes synchronously (used for deterministic screenshots / benchmarks).
    void BakeNow(const BlastModel& model);

    // Adopts a finished bake. Returns true if a new frame became current.
    bool Poll();

    bool Busy() const { return inFlight_.load(std::memory_order_acquire); }
    bool HasData() const { return hasData_; }
    const VolumeFrame& Current() const { return buffers_[currentIndex_]; }
    uint64_t Version() const { return version_; }
    float LastBakeMilliseconds() const { return lastBakeMs_; }
    void WaitForIdle();

private:
    struct FieldParams {
        float time = 0.0f;
        float ambientTemperature = 288.15f;
        float fireballX = 0.0f;
        float fireballY = 0.0f;
        float fireballZ = 0.0f;
        float fireballRadius = 1.0f;
        float fireballTemperature = 288.15f;
        bool fireballVisible = false;
        float cloudX = 0.0f;
        float cloudY = 0.0f;
        float cloudZ = 0.0f;
        float cloudRadius = 1.0f;
        float cloudTemperature = 288.15f;
        float stemRadius = 1.0f;
        float cloudStrength = 0.0f;
        float dustRingRadius = 0.0f;
        float dustIntensity = 0.0f;
        float shockRadius = 0.0f;
        float shockMach = 1.0f;
        float burstY = 0.0f;
        float extinctionScale = 0.1f;
        float voxelSize = 1.0f;
    };

    struct BakeJob {
        FieldParams params;
        VolumeBounds bounds;
        float extinctionScale = 0.1f;
        int resolution = 0;
        std::vector<float>* target = nullptr;
    };

    void Dispatch(const BlastModel& model, bool synchronous);
    void BakeRange(int zBegin, int zEnd, const BakeJob& job);
    void AdoptFinished();

    core::ThreadPool* pool_ = nullptr;
    int resolution_ = 96;

    VolumeFrame buffers_[2];
    int currentIndex_ = 0;
    int writeIndex_ = 1;
    bool hasData_ = false;
    uint64_t version_ = 0;

    std::atomic<bool> inFlight_{false};
    std::atomic<bool> ready_{false};
    BakeJob job_;
    float lastBakeMs_ = 0.0f;
};

} // namespace ex::sim

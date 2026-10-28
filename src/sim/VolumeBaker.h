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

// Analytic field parameters shared by the CPU and GPU bakers. The layout is
// mirrored by the VolumeBakeConstants buffer in the renderer.
struct VolumeFieldParams {
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

// One bake request: everything needed to produce a volume frame, independent of
// where it is evaluated. The renderer uses it to dispatch the compute-shader
// bake; the CPU path uses the same struct.
struct VolumeBakeRequest {
    int resolution = 0;
    VolumeBounds bounds{};
    float extinctionScale = 0.1f;
    float voxelSize = 0.0f;
    bool split = false;
    VolumeFieldParams params{};

    bool valid() const { return resolution > 0; }
};

// CPU-side volumetric field in RGBA16F layout:
//   R = density, G = temperature / 12kK, B = dust mask, A = soot mask.
// Half precision halves the memory and upload bandwidth compared to RGBA32F,
// which is essential for the 256³-1024³ options.
struct VolumeFrame {
    int resolution = 0;
    VolumeBounds bounds{};
    float extinctionScale = 0.1f;
    uint64_t version = 0;
    int bytesPerVoxel = 8;
    bool halfFloat = true;
    bool split = false;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> rgba2; // second RG8 plane (dust/soot) when split

    bool empty() const {
        return resolution <= 0 || rgba.empty() || (split && rgba2.empty());
    }
};

// Bakes the analytic blast field into a 3D texture on the thread pool.
//
// A single CPU buffer is used: a new bake may only start after the previous
// frame has been uploaded to the GPU (MarkUploaded). This keeps the memory
// footprint at one frame, which is what makes 512³ and 1024³ possible at all.
class VolumeBaker {
public:
    void Initialize(core::ThreadPool* pool, int resolution);
    void Reset();
    void SetResolution(int resolution);
    int Resolution() const { return resolution_; }
    void SetAutoScale(bool enabled);
    bool AutoScale() const { return autoScale_; }

    void StartBake(const BlastModel& model);
    void BakeNow(const BlastModel& model);

    // Computes the bounds and field parameters for the current blast state
    // without evaluating the field. Used by the GPU bake path.
    VolumeBakeRequest Prepare(const BlastModel& model);

    // When enabled, StartBake/BakeNow only prepare a request; the renderer
    // evaluates it on the GPU. The CPU bake stays available as a fallback.
    void SetGpuBake(bool enabled);
    bool GpuBake() const { return gpuBake_; }
    // Consumes the pending GPU request (latest wins). Returns false if none.
    bool TakeGpuRequest(VolumeBakeRequest& out);

    bool Poll();
    bool HasNewFrame() const;
    bool CanStartBake() const;
    void MarkUploaded(uint64_t version);

    bool Busy() const { return inFlight_.load(std::memory_order_acquire); }
    bool HasData() const { return hasData_; }
    const VolumeFrame& Current() const { return frame_; }
    uint64_t Version() const { return frame_.version; }
    float LastBakeMilliseconds() const { return lastBakeMs_; }
    void WaitForIdle();

private:
    struct BakeJob {
        VolumeFieldParams params;
        VolumeBounds bounds;
        float extinctionScale = 0.1f;
        int resolution = 0;
        int bytesPerVoxel = 8;
        bool halfFloat = true;
        bool split = false;
        std::vector<uint8_t>* target = nullptr;
        std::vector<uint8_t>* target2 = nullptr;
    };

    void Dispatch(const BlastModel& model, bool synchronous);
    void BakeRange(int zBegin, int zEnd, const BakeJob& job);

    core::ThreadPool* pool_ = nullptr;
    int resolution_ = 128;

    VolumeFrame frame_;
    bool hasData_ = false;
    bool autoScale_ = true;
    double autoExtent_ = 0.0;
    double autoTop_ = 0.0;

    std::atomic<bool> inFlight_{false};
    std::atomic<bool> ready_{false};
    std::atomic<uint64_t> uploadedVersion_{0};
    BakeJob job_;
    bool gpuBake_ = false;
    bool gpuPending_ = false;
    VolumeBakeRequest gpuRequest_{};
    float lastBakeMs_ = 0.0f;
};

} // namespace ex::sim

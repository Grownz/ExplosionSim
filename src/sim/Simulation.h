#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "sim/BlastModel.h"
#include "sim/ParticleSystem.h"
#include "sim/VolumeBaker.h"
#include "scene/SceneTypes.h"

namespace ex::core {
class ThreadPool;
}

namespace ex::sim {

enum class SimulationMode : int {
    ReducedOrder = 0,
    HighFidelity = 1,
};

struct SimulationSettings {
    // Physics
    double chargeMassKg = 0.2;
    double heightOfBurstM = 0.4;
    double ambientTemperatureK = 288.15;
    double ambientPressurePa = 101325.0;
    double relativeHumidity = 0.45;
    double windSpeedMs = 1.0;
    double windDirectionDeg = 40.0;

    // Playback
    double fuseSeconds = 2.0;
    double timeScale = 0.06;
    bool loop = false;
    double loopDurationS = 0.55;

    // Simulation model
    SimulationMode mode = SimulationMode::ReducedOrder;
    int solverResolution = 128;
    int solverStepsPerFrame = 24;
    bool solverAutoScale = true;

    // Performance
    int particleBudget = 40000;
    int volumeResolution = 128;
    bool bakeVolume = true;
    bool autoScaleVolume = true;

    // Procedural environment
    scene::SceneSettings scene{};
};

class Simulation {
public:
    Simulation() = default;
    ~Simulation();
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    bool Initialize(const SimulationSettings& settings, core::ThreadPool& pool);
    void Shutdown();

    // Restarts the detonation with the current settings.
    void Reset();

    // Applies new settings. Physical changes restart the simulation.
    void ApplySettings(const SimulationSettings& settings);

    // Advances by `realDt` seconds of wall-clock time, scaled by timeScale.
    void Update(float realDt);

    // Advances the physics by an exact amount (deterministic; used for
    // screenshots and benchmarks).
    void StepFixed(double dtSim);

    void DetonateNow();
    bool IsArmed() const { return !detonated_; }
    double FuseRemaining() const { return fuseRemaining_; }
    uint64_t BlastSerial() const { return blastSerial_; }

    // Advances the model without the real-time clock. Used in high-fidelity
    // mode so the particles/camera follow the GPU solver's simulated time.
    void AdvanceExternal(double dtSim);

    const SimulationSettings& Settings() const { return settings_; }
    double Time() const { return blast_.State().time; }
    double RealTime() const { return realTime_; }

    const BlastModel& Blast() const { return blast_; }
    const ParticleSystem& Particles() const { return particles_; }
    VolumeBaker& Volume() { return volume_; }
    const VolumeBaker& Volume() const { return volume_; }
    const scene::Scene& Scene() const { return scene_; }

    // Waits until all pending volume work is finished and adopts the result.
    void FinishPendingVolumeWork();

    // Background scene generation. Disabled by default so headless runs and
    // tests keep the synchronous behaviour; the interactive app enables it.
    void SetAsyncSceneGeneration(bool enabled) { asyncScene_ = enabled; }
    bool SceneGenerating() const {
        return sceneGenerating_.load(std::memory_order_relaxed);
    }
    float SceneProgress() const {
        return sceneProgress_.load(std::memory_order_relaxed);
    }
    // Polls a finished background job and swaps the new scene into place.
    void PollSceneGeneration();
    // Stops a running job (used by Shutdown); adopts a finished scene first.
    void WaitForSceneGeneration();
    // True once after a background scene has been adopted.
    bool ConsumeSceneCompletion();

    double ObserverDistanceM() const { return observerDistance_; }
    void SetObserverDistanceM(double distance) { observerDistance_ = distance; }

private:
    void Detonate();
    void ScheduleVolumeBake(double dtReal);
    void UpdateScene(double dtSim);
    void StartSceneGeneration(const scene::SceneSettings& settings,
                              const Charge& charge, const Environment& environment);
    void LogSceneStats();

    SimulationSettings settings_{};
    BlastModel blast_{};
    ParticleSystem particles_{};
    VolumeBaker volume_{};
    scene::Scene scene_{};
    core::ThreadPool* pool_ = nullptr;

    std::unique_ptr<scene::Scene> pendingScene_;
    std::atomic<float> sceneProgress_{1.0f};
    std::atomic<bool> sceneGenerating_{false};
    std::atomic<bool> sceneReady_{false};
    bool asyncScene_ = false;
    bool sceneCompleted_ = false;
    std::jthread sceneThread_;

    double realTime_ = 0.0;
    double bakeAccumulator_ = 0.0;
    double observerDistance_ = 100.0;
    double fuseRemaining_ = 0.0;
    bool detonated_ = false;
    bool initialized_ = false;
    uint64_t blastSerial_ = 0;
};

} // namespace ex::sim

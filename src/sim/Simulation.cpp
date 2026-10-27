#include "sim/Simulation.h"

#include "core/Log.h"
#include "core/ThreadPool.h"

#include <algorithm>
#include <cmath>
#include <exception>

namespace ex::sim {

namespace {

bool PhysicsChanged(const SimulationSettings& a, const SimulationSettings& b) {
    return a.chargeMassKg != b.chargeMassKg || a.heightOfBurstM != b.heightOfBurstM ||
           a.ambientTemperatureK != b.ambientTemperatureK ||
           a.ambientPressurePa != b.ambientPressurePa ||
           a.relativeHumidity != b.relativeHumidity || a.windSpeedMs != b.windSpeedMs ||
           a.windDirectionDeg != b.windDirectionDeg;
}

bool SceneChanged(const SimulationSettings& a, const SimulationSettings& b) {
    return a.scene.enabled != b.scene.enabled || a.scene.seed != b.scene.seed ||
           a.scene.density != b.scene.density || a.scene.buildings != b.scene.buildings ||
           a.scene.vehicles != b.scene.vehicles ||
           a.scene.vegetation != b.scene.vegetation ||
           a.scene.quality != b.scene.quality ||
           a.scene.chunkScale != b.scene.chunkScale ||
           a.scene.groundReflection != b.scene.groundReflection ||
           a.scene.voronoiChunks != b.scene.voronoiChunks;
}

} // namespace

bool Simulation::Initialize(const SimulationSettings& settings, core::ThreadPool& pool,
                            bool holdFuse) {
    pool_ = &pool;
    settings_ = settings;
    particles_.Initialize(settings.particleBudget);
    volume_.Initialize(pool_, settings.volumeResolution);
    initialized_ = true;
    ResetInternal(holdFuse, true);
    return true;
}

void Simulation::Shutdown() {
    WaitForSceneGeneration();
    volume_.WaitForIdle();
    scene_.Clear();
    sceneValid_ = false;
    initialized_ = false;
}

Simulation::~Simulation() {
    WaitForSceneGeneration();
}

void Simulation::StartSceneGeneration(const scene::SceneSettings& settings,
                                      const Charge& charge,
                                      const Environment& environment) {
    // A superseded job checks cancellation per building; the join below waits
    // for at most the buildings already in flight on the pool.
    if (sceneThread_.joinable()) {
        sceneThread_.request_stop();
        sceneThread_.join();
    }
    pendingScene_.reset();
    // Reset the visible town immediately; the worker fills the replacement.
    scene_.Clear();
    sceneValid_ = false;
    sceneReady_.store(false, std::memory_order_relaxed);
    sceneCompleted_ = false;
    sceneProgress_.store(0.0f, std::memory_order_relaxed);
    sceneGenerating_.store(true, std::memory_order_release);

    sceneThread_ = std::jthread(
        [this, settings, charge, environment](std::stop_token token) {
            auto scene = std::make_unique<scene::Scene>();
            bool success = false;
            try {
                sim::BlastModel localBlast;
                localBlast.Configure(charge, environment);
                scene->Generate(settings, localBlast, [this, token](float value) {
                    sceneProgress_.store(value, std::memory_order_relaxed);
                    return !token.stop_requested();
                }, pool_);
                success = !token.stop_requested();
            } catch (const std::exception& error) {
                LogError("Scene generation failed: %s", error.what());
            } catch (...) {
                LogError("Scene generation failed.");
            }
            if (success) {
                pendingScene_ = std::move(scene);
                sceneReady_.store(true, std::memory_order_release);
            }
            sceneGenerating_.store(false, std::memory_order_release);
        });
}

void Simulation::PollSceneGeneration() {
    if (sceneReady_.exchange(false, std::memory_order_acquire)) {
        if (sceneThread_.joinable()) {
            sceneThread_.join();
        }
        if (pendingScene_) {
            scene_ = std::move(*pendingScene_);
            pendingScene_.reset();
            sceneValid_ = true;
            sceneCompleted_ = true;
            sceneProgress_.store(1.0f, std::memory_order_relaxed);
            LogSceneStats();
        } else {
            sceneValid_ = false;
        }
        sceneGenerating_.store(false, std::memory_order_release);
        return;
    }
    if (!sceneGenerating_.load(std::memory_order_acquire) && sceneThread_.joinable()) {
        // The job was cancelled or failed; release its resources.
        sceneThread_.join();
        pendingScene_.reset();
        sceneValid_ = false;
    }
}

void Simulation::WaitForSceneGeneration() {
    if (sceneThread_.joinable()) {
        sceneThread_.request_stop();
        sceneThread_.join();
    }
    if (sceneReady_.exchange(false, std::memory_order_acquire) && pendingScene_) {
        scene_ = std::move(*pendingScene_);
        sceneValid_ = true;
        LogSceneStats();
    } else {
        sceneValid_ = false;
    }
    pendingScene_.reset();
    sceneGenerating_.store(false, std::memory_order_release);
}

bool Simulation::ConsumeSceneCompletion() {
    const bool completed = sceneCompleted_;
    sceneCompleted_ = false;
    return completed;
}

void Simulation::LogSceneStats() {
    if (scene_.Settings().enabled && !scene_.Empty()) {
        const scene::SceneStats stats = scene_.Stats();
        LogInfo("Scene: %d buildings, %d vehicles, %d trees, %d chunks, %.0f m radius",
                stats.buildings, stats.vehicles, stats.trees, stats.chunks,
                scene_.Extent());
    }
}

void Simulation::Reset() {
    ResetInternal(false, !sceneValid_ && !sceneGenerating_);
}

void Simulation::Rearm() {
    ResetInternal(true, !sceneValid_ && !sceneGenerating_);
}

void Simulation::ResetInternal(bool holdFuse, bool regenerateScene) {
    if (!initialized_) {
        return;
    }

    ++blastSerial_;

    Environment environment{};
    environment.pressurePa = settings_.ambientPressurePa;
    environment.temperatureK = settings_.ambientTemperatureK;
    environment.relativeHumidity = settings_.relativeHumidity;
    environment.windSpeedMs = settings_.windSpeedMs;
    environment.windDirectionDeg = settings_.windDirectionDeg;

    Charge charge{};
    charge.tntMassKg = std::max(settings_.chargeMassKg, 0.01);
    charge.heightOfBurstM = std::max(settings_.heightOfBurstM, 0.0);

    blast_.Configure(charge, environment);

    if (!regenerateScene && !sceneValid_ && !sceneGenerating_) {
        // No usable town (first run, failed job): generate one.
        regenerateScene = true;
    }
    if (regenerateScene) {
        if (asyncScene_) {
            StartSceneGeneration(settings_.scene, charge, environment);
        } else {
            scene_.Generate(settings_.scene, blast_, {}, pool_);
            sceneValid_ = true;
            LogSceneStats();
        }
    } else {
        // The deterministic geometry is unchanged; only reset the runtime
        // state (damage, burning, bodies, joints, spawn queues).
        scene_.ResetState();
    }

    particles_.Reset();
    volume_.SetAutoScale(settings_.autoScaleVolume);
    volume_.SetResolution(settings_.volumeResolution);
    volume_.Reset();

    realTime_ = 0.0;
    bakeAccumulator_ = 0.0;
    detonated_ = false;
    fuseHeld_ = holdFuse;
    fuseRemaining_ = std::max(0.0, settings_.fuseSeconds);

    if (!fuseHeld_ && fuseRemaining_ <= 0.0) {
        Detonate();
    }
}

void Simulation::Detonate() {
    if (!initialized_ || detonated_) {
        return;
    }

    detonated_ = true;
    fuseHeld_ = false;
    fuseRemaining_ = 0.0;
    ++blastSerial_;

    blast_.Reset();
    particles_.Reset();
    particles_.Detonate(blast_, settings_.mode == SimulationMode::ReducedOrder);
    volume_.SetResolution(settings_.volumeResolution);
    volume_.Reset();

    if (settings_.bakeVolume && settings_.mode == SimulationMode::ReducedOrder) {
        volume_.StartBake(blast_);
    }
}

void Simulation::DetonateNow() {
    if (!detonated_) {
        Detonate();
    }
}

void Simulation::ApplySettings(const SimulationSettings& settings) {
    const bool physicsChanged = PhysicsChanged(settings_, settings);
    const bool sceneChanged = SceneChanged(settings_, settings);
    const bool budgetChanged = settings_.particleBudget != settings.particleBudget;
    const bool volumeChanged = settings_.volumeResolution != settings.volumeResolution;
    const bool fuseChanged = settings_.fuseSeconds != settings.fuseSeconds;
    const bool modeChanged = settings_.mode != settings.mode;
    const bool solverResolutionChanged =
        settings_.solverResolution != settings.solverResolution;
    const bool solverDomainChanged =
        settings_.solverHemisphere != settings.solverHemisphere;
    const bool autoScaleChanged = settings_.autoScaleVolume != settings.autoScaleVolume;
    // The scene geometry depends on the layout settings and on the charge /
    // environment (the radius and the stored blast fields). Everything else
    // only restarts the simulation and can reuse the generated town.
    const bool needsScene = sceneChanged || physicsChanged;
    const bool needsReset = physicsChanged || budgetChanged || fuseChanged ||
                            modeChanged || solverResolutionChanged ||
                            solverDomainChanged;

    settings_ = settings;

    if (autoScaleChanged) {
        volume_.SetAutoScale(settings_.autoScaleVolume);
        if (detonated_ && settings_.bakeVolume &&
            settings_.mode == SimulationMode::ReducedOrder) {
            volume_.StartBake(blast_);
        }
    }

    if (budgetChanged) {
        particles_.Initialize(settings_.particleBudget);
    }
    if (needsReset) {
        ResetInternal(false, needsScene);
        return;
    }

    if (volumeChanged) {
        volume_.SetResolution(settings_.volumeResolution);
        volume_.Reset();
        if (detonated_ && settings_.bakeVolume &&
            settings_.mode == SimulationMode::ReducedOrder) {
            volume_.StartBake(blast_);
        }
    }

    if (sceneChanged) {
        if (asyncScene_) {
            StartSceneGeneration(settings_.scene, blast_.GetCharge(),
                                 blast_.GetEnvironment());
        } else {
            scene_.Generate(settings_.scene, blast_, {}, pool_);
            sceneValid_ = true;
            LogSceneStats();
        }
    }
}

void Simulation::Update(float realDt) {
    if (!initialized_) {
        return;
    }

    PollSceneGeneration();

    realDt = std::max(realDt, 0.0f);
    realTime_ += realDt;

    if (!detonated_) {
        if (!fuseHeld_) {
            fuseRemaining_ -= double(realDt);
            if (fuseRemaining_ <= 0.0) {
                Detonate();
            }
        }
        return;
    }

    const double dtSim = std::clamp(double(realDt) * settings_.timeScale, 0.0, 0.05);
    if (dtSim > 0.0) {
        blast_.Advance(dtSim);
        UpdateScene(dtSim);
        particles_.Update(float(dtSim), blast_, *pool_,
                          settings_.mode == SimulationMode::ReducedOrder);
    }

    if (settings_.loop && blast_.State().time > settings_.loopDurationS) {
        Reset();
        return;
    }

    volume_.Poll();
    ScheduleVolumeBake(realDt);
}

void Simulation::StepFixed(double dtSim) {
    if (!initialized_ || dtSim <= 0.0) {
        return;
    }

    if (!detonated_) {
        if (!fuseHeld_) {
            fuseRemaining_ -= dtSim;
            if (fuseRemaining_ <= 0.0) {
                Detonate();
            }
        }
        return;
    }

    blast_.Advance(dtSim);
    UpdateScene(dtSim);
    particles_.Update(float(dtSim), blast_, *pool_,
                      settings_.mode == SimulationMode::ReducedOrder);
    volume_.Poll();
    realTime_ += dtSim;
}

void Simulation::AdvanceExternal(double dtSim, double realDt) {
    if (!initialized_ || !detonated_ || dtSim <= 0.0) {
        return;
    }

    blast_.Advance(dtSim);
    UpdateScene(dtSim);
    particles_.Update(float(dtSim), blast_, *pool_,
                      settings_.mode == SimulationMode::ReducedOrder);
    realTime_ += realDt >= 0.0 ? realDt : dtSim;
}

void Simulation::UpdateScene(double dtSim) {
    if (sceneGenerating_.load(std::memory_order_acquire)) {
        return;
    }
    scene_.Update(blast_, float(dtSim));

    const std::vector<scene::SceneDebrisSpawn> spawns = scene_.ConsumeDebrisSpawns();
    const std::vector<scene::SceneFireSpawn> fires = scene_.ConsumeFireSpawns();
    if (settings_.mode == SimulationMode::ReducedOrder) {
        for (const scene::SceneDebrisSpawn& spawn : spawns) {
            particles_.SpawnSceneDebris(spawn.position, spawn.count, spawn.scale,
                                        spawn.speed);
        }
    }
    // Scene fires are a cosmetic overlay on burning objects and are shown in
    // both modes; the high-fidelity solver only replaces the blast particles.
    for (const scene::SceneFireSpawn& fire : fires) {
        particles_.SpawnSceneFire(fire.position, fire.count, fire.scale);
    }
}

void Simulation::ScheduleVolumeBake(double dtReal) {
    if (!settings_.bakeVolume || !detonated_ ||
        settings_.mode != SimulationMode::ReducedOrder) {
        return;
    }

    bakeAccumulator_ += dtReal;
    double interval = 1.0 / 30.0;
    if (settings_.volumeResolution >= 1024) {
        interval = 5.0;
    } else if (settings_.volumeResolution >= 512) {
        interval = 2.0;
    } else if (settings_.volumeResolution >= 256) {
        interval = 0.5;
    }
    if (bakeAccumulator_ < interval) {
        return;
    }
    if (!volume_.CanStartBake()) {
        return;
    }
    bakeAccumulator_ = 0.0;
    volume_.StartBake(blast_);
}

void Simulation::FinishPendingVolumeWork() {
    volume_.WaitForIdle();
    volume_.Poll();
}

} // namespace ex::sim

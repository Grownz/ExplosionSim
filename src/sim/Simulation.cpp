#include "sim/Simulation.h"

#include "core/Log.h"
#include "core/ThreadPool.h"

#include <algorithm>
#include <chrono>
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
    particles_.SetGpuMode(settings.gpuParticles);
    particles_.Initialize(settings.particleBudget);
    volume_.Initialize(pool_, settings.volumeResolution);
    volume_.SetScene(&scene_);
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
    pendingPreview_.reset();
    // Keep the previous town visible until the early layout preview arrives.
    sceneValid_ = false;
    sceneReady_.store(false, std::memory_order_relaxed);
    previewReady_.store(false, std::memory_order_relaxed);
    sceneCompleted_ = false;
    scenePreviewCompleted_ = false;
    sceneProgress_.store(0.0f, std::memory_order_relaxed);
    sceneGenerating_.store(true, std::memory_order_release);

    const auto publishProgress = [this](float value) {
        float previous = sceneProgress_.load(std::memory_order_relaxed);
        while (value > previous &&
               !sceneProgress_.compare_exchange_weak(previous, value,
                                                     std::memory_order_relaxed)) {
        }
    };

    sceneThread_ = std::jthread(
        [this, settings, charge, environment, publishProgress](std::stop_token token) {
            sim::BlastModel localBlast;
            localBlast.Configure(charge, environment);

            // Early layout preview: the intact town is visible while the
            // Voronoi chunks are fractured below.
            auto preview = std::make_unique<scene::Scene>();
            const auto previewStart = std::chrono::steady_clock::now();
            bool previewSuccess = false;
            try {
                previewSuccess = preview->GenerateLayout(settings, localBlast);
            } catch (const std::exception& error) {
                LogError("Scene preview failed: %s", error.what());
            } catch (...) {
                LogError("Scene preview failed.");
            }
            if (previewSuccess && !token.stop_requested()) {
                const int previewObjects = int(preview->Objects().size());
                pendingPreview_ = std::move(preview);
                previewReady_.store(true, std::memory_order_release);
                publishProgress(0.05f);
                LogInfo("Scene layout ready in %.1f ms: %d objects",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - previewStart)
                            .count(),
                        previewObjects);
            }

            // Full scene with chunks.
            auto scene = std::make_unique<scene::Scene>();
            bool success = false;
            try {
                scene->Generate(settings, localBlast,
                                [this, token, publishProgress](float value) {
                                    publishProgress(value);
                                    return !token.stop_requested();
                                },
                                pool_);
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
    if (previewReady_.exchange(false, std::memory_order_acquire)) {
        if (pendingPreview_) {
            scene_ = std::move(*pendingPreview_);
            pendingPreview_.reset();
            scenePreviewCompleted_ = true;
        }
    }
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
        pendingPreview_.reset();
        sceneValid_ = false;
    }
}

void Simulation::WaitForSceneGeneration() {
    if (sceneThread_.joinable()) {
        sceneThread_.request_stop();
        sceneThread_.join();
    }
    if (previewReady_.exchange(false, std::memory_order_acquire) && pendingPreview_) {
        scene_ = std::move(*pendingPreview_);
    }
    pendingPreview_.reset();
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

bool Simulation::ConsumeScenePreview() {
    const bool completed = scenePreviewCompleted_;
    scenePreviewCompleted_ = false;
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
    volume_.SetGpuBake(settings_.gpuVolumeBake);
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
    const bool gpuParticlesChanged = settings_.gpuParticles != settings.gpuParticles;
    const bool volumeChanged = settings_.volumeResolution != settings.volumeResolution;
    const bool fuseChanged = settings_.fuseSeconds != settings.fuseSeconds;
    const bool modeChanged = settings_.mode != settings.mode;
    const bool solverResolutionChanged =
        settings_.solverResolution != settings.solverResolution;
    const bool solverDomainChanged =
        settings_.solverHemisphere != settings.solverHemisphere;
    const bool autoScaleChanged = settings_.autoScaleVolume != settings.autoScaleVolume;
    const bool gpuBakeChanged = settings_.gpuVolumeBake != settings.gpuVolumeBake;
    // The scene geometry depends on the layout settings and on the charge /
    // environment (the radius and the stored blast fields). Everything else
    // only restarts the simulation and can reuse the generated town.
    const bool needsScene = sceneChanged || physicsChanged;
    const bool needsReset = physicsChanged || budgetChanged || fuseChanged ||
                            modeChanged || solverResolutionChanged ||
                            solverDomainChanged || gpuParticlesChanged;

    settings_ = settings;

    if (autoScaleChanged) {
        volume_.SetAutoScale(settings_.autoScaleVolume);
        if (detonated_ && settings_.bakeVolume &&
            settings_.mode == SimulationMode::ReducedOrder) {
            volume_.StartBake(blast_);
        }
    }

    if (gpuBakeChanged) {
        volume_.SetGpuBake(settings_.gpuVolumeBake);
        if (detonated_ && settings_.bakeVolume &&
            settings_.mode == SimulationMode::ReducedOrder) {
            volume_.StartBake(blast_);
        }
    }

    if (budgetChanged) {
        particles_.Initialize(settings_.particleBudget);
    }
    if (gpuParticlesChanged) {
        particles_.SetGpuMode(settings_.gpuParticles);
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

std::vector<ObstacleBox> Simulation::CollectSolverObstacles(
    const VolumeBounds& bounds) const {
    std::vector<ObstacleBox> boxes = SceneObstacles(scene_);
    std::vector<ObstacleBox> bodies = PhysicsObstacles(scene_.Physics(), bounds);
    boxes.insert(boxes.end(), bodies.begin(), bodies.end());
    return boxes;
}

std::vector<FlowBody> Simulation::CollectFlowBodies(
    const VolumeBounds& bounds) const {
    std::vector<FlowBody> flow;
    const std::vector<scene::RigidBody>& rigid = scene_.Physics().Bodies();
    for (int i = 0; i < int(rigid.size()); ++i) {
        const scene::RigidBody& body = rigid[i];
        if (body.isStatic) {
            continue;
        }
        if (body.position.x < bounds.min.x || body.position.x > bounds.max.x ||
            body.position.y < bounds.min.y || body.position.y > bounds.max.y ||
            body.position.z < bounds.min.z || body.position.z > bounds.max.z) {
            continue;
        }
        FlowBody entry;
        entry.position = body.position;
        entry.velocity = body.velocity;
        entry.halfExtents = body.halfExtents;
        entry.orientation = body.orientation;
        entry.physicsIndex = i;
        flow.push_back(entry);
    }
    return flow;
}

void Simulation::ApplyBodyDrag(const std::vector<FlowBody>& bodies,
                               const std::vector<BodyFlowSample>& samples,
                               float dt) {
    if (settings_.solverBodyDrag) {
        ex::sim::ApplyBodyDrag(scene_.Physics(), bodies, samples, dt);
    }
}

void Simulation::UpdateScene(double dtSim) {
    if (sceneGenerating_.load(std::memory_order_acquire)) {
        return;
    }
    scene_.Update(blast_, float(dtSim));

    const std::vector<scene::SceneDebrisSpawn> spawns = scene_.ConsumeDebrisSpawns();
    const std::vector<scene::SceneFireSpawn> fires = scene_.ConsumeFireSpawns();
    const std::vector<scene::SceneEmberSpawn> embers = scene_.ConsumeEmberSpawns();
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
    for (const scene::SceneEmberSpawn& ember : embers) {
        particles_.SpawnSceneEmbers(ember.position, ember.direction, ember.count,
                                    ember.scale);
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

#include "sim/Simulation.h"

#include "core/Log.h"
#include "core/ThreadPool.h"

#include <algorithm>
#include <cmath>

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
           a.scene.voronoiChunks != b.scene.voronoiChunks;
}

} // namespace

bool Simulation::Initialize(const SimulationSettings& settings, core::ThreadPool& pool) {
    pool_ = &pool;
    settings_ = settings;
    particles_.Initialize(settings.particleBudget);
    volume_.Initialize(pool_, settings.volumeResolution);
    initialized_ = true;
    Reset();
    return true;
}

void Simulation::Shutdown() {
    volume_.WaitForIdle();
    scene_.Clear();
    initialized_ = false;
}

void Simulation::Reset() {
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
    scene_.Generate(settings_.scene, blast_);
    if (settings_.scene.enabled && !scene_.Empty()) {
        const scene::SceneStats stats = scene_.Stats();
        LogInfo("Scene: %d buildings, %d vehicles, %d trees, %d chunks, %.0f m extent",
                stats.buildings, stats.vehicles, stats.trees, stats.chunks,
                scene_.Extent());
    }
    particles_.Reset();
    volume_.SetAutoScale(settings_.autoScaleVolume);
    volume_.SetResolution(settings_.volumeResolution);
    volume_.Reset();

    realTime_ = 0.0;
    bakeAccumulator_ = 0.0;
    detonated_ = false;
    fuseRemaining_ = std::max(0.0, settings_.fuseSeconds);

    if (fuseRemaining_ <= 0.0) {
        Detonate();
    }
}

void Simulation::Detonate() {
    if (!initialized_ || detonated_) {
        return;
    }

    detonated_ = true;
    fuseRemaining_ = 0.0;
    ++blastSerial_;

    blast_.Reset();
    particles_.Reset();
    particles_.Detonate(blast_);
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
    const bool autoScaleChanged = settings_.autoScaleVolume != settings.autoScaleVolume;

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
    if (physicsChanged || budgetChanged || fuseChanged || modeChanged ||
        solverResolutionChanged) {
        Reset();
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
        scene_.Generate(settings_.scene, blast_);
    }
}

void Simulation::Update(float realDt) {
    if (!initialized_) {
        return;
    }

    realDt = std::clamp(realDt, 0.0f, 0.1f);
    realTime_ += realDt;

    if (!detonated_) {
        fuseRemaining_ -= double(realDt);
        if (fuseRemaining_ <= 0.0) {
            Detonate();
        }
        return;
    }

    const double dtSim = std::clamp(double(realDt) * settings_.timeScale, 0.0, 0.05);
    if (dtSim > 0.0) {
        blast_.Advance(dtSim);
        UpdateScene(dtSim);
        if (settings_.mode == SimulationMode::ReducedOrder) {
            particles_.Update(float(dtSim), blast_, *pool_);
        }
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
        fuseRemaining_ -= dtSim;
        if (fuseRemaining_ <= 0.0) {
            Detonate();
        }
        return;
    }

    blast_.Advance(dtSim);
    UpdateScene(dtSim);
    if (settings_.mode == SimulationMode::ReducedOrder) {
        particles_.Update(float(dtSim), blast_, *pool_);
    }
    volume_.Poll();
    realTime_ += dtSim;
}

void Simulation::AdvanceExternal(double dtSim) {
    if (!initialized_ || !detonated_ || dtSim <= 0.0) {
        return;
    }

    blast_.Advance(dtSim);
    UpdateScene(dtSim);
    if (settings_.mode == SimulationMode::ReducedOrder) {
        particles_.Update(float(dtSim), blast_, *pool_);
    }
    realTime_ += dtSim;
}

void Simulation::UpdateScene(double dtSim) {
    scene_.Update(blast_, float(dtSim));

    const std::vector<scene::SceneDebrisSpawn> spawns = scene_.ConsumeDebrisSpawns();
    const std::vector<scene::SceneFireSpawn> fires = scene_.ConsumeFireSpawns();
    if (settings_.mode != SimulationMode::ReducedOrder) {
        return;
    }
    for (const scene::SceneDebrisSpawn& spawn : spawns) {
        particles_.SpawnSceneDebris(spawn.position, spawn.count, spawn.scale,
                                    spawn.speed);
    }
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

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
    particles_.Reset();
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

void Simulation::RestartInternal() {
    Reset();
}

void Simulation::ApplySettings(const SimulationSettings& settings) {
    const bool physicsChanged = PhysicsChanged(settings_, settings);
    const bool budgetChanged = settings_.particleBudget != settings.particleBudget;
    const bool volumeChanged = settings_.volumeResolution != settings.volumeResolution;
    const bool fuseChanged = settings_.fuseSeconds != settings.fuseSeconds;
    const bool modeChanged = settings_.mode != settings.mode;
    const bool solverResolutionChanged =
        settings_.solverResolution != settings.solverResolution;

    settings_ = settings;

    if (budgetChanged) {
        particles_.Initialize(settings_.particleBudget);
    }
    if (volumeChanged) {
        volume_.SetResolution(settings_.volumeResolution);
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
    if (settings_.mode == SimulationMode::ReducedOrder) {
        particles_.Update(float(dtSim), blast_, *pool_);
    }
    realTime_ += dtSim;
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

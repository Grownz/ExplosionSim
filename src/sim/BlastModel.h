#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ex::sim {

// Heat of detonation of TNT in J/kg (used for the equivalent-energy scaling).
inline constexpr double kTntSpecificEnergyJkg = 4.184e6;
inline constexpr double kTntDensityKgM3 = 1630.0;

// Visible surface temperature of a young fireball; the optically thick shock
// front radiates close to this value while it expands.
inline constexpr double kFireballSurfaceTemperatureK = 3600.0;

// Shared extinction scale for the baked and solver-driven volumes. The optical
// depth is normalised by the fireball radius so the cloud stays visually
// opaque at every charge scale.
inline float VolumeExtinctionScale(double fireballRadiusM) {
    return float(0.9 / std::max(fireballRadiusM, 0.5));
}

struct Environment {
    double pressurePa = 101325.0;
    double temperatureK = 288.15;
    double relativeHumidity = 0.45;
    double windSpeedMs = 1.0;
    double windDirectionDeg = 40.0; // direction the wind blows towards
    double gravity = 9.80665;
    double gamma = 1.4;
    double gasConstant = 287.058; // dry air, J/(kg K)

    // Magnus-Tetens saturation vapour pressure over water.
    double SaturationVaporPressurePa() const;
    double VaporPressurePa() const { return relativeHumidity * SaturationVaporPressurePa(); }
    double SpecificHumidity() const;

    // Moist-air density and sound speed. Humidity lowers the density and
    // slightly raises the speed of sound.
    double AirDensity() const;
    double SoundSpeed() const;
    double VirtualTemperatureK() const;

    double SpecificHeatCp() const { return gamma * gasConstant / (gamma - 1.0); }
    double WindX() const;
    double WindZ() const;
};

struct Charge {
    double tntMassKg = 20000.0;
    double heightOfBurstM = 30.0;

    double EnergyJ() const { return tntMassKg * kTntSpecificEnergyJkg; }
    double RadiusM() const;
    double CbrtMass() const;
    double ScaledDistance(double distanceM) const { return distanceM / CbrtMass(); }
};

// Everything the renderer needs to know about the current blast.
struct BlastState {
    double time = 0.0;

    double shockRadius = 0.0;
    double shockSpeed = 0.0;
    double shockMach = 1.0;
    double shockOverpressurePa = 0.0;

    double fireballRadius = 0.0;
    double fireballTemperatureK = 0.0;
    double fireballCenterY = 0.0;
    bool fireballVisible = false;

    double cloudCenterY = 0.0;
    double cloudRadius = 0.0;
    double cloudTemperatureK = 288.15;
    double cloudRise = 0.0;
    double cloudRiseSpeed = 0.0;
    double cloudDriftX = 0.0;
    double cloudDriftZ = 0.0;
    double stemRadius = 0.0;

    double dustRingRadius = 0.0;
    double dustIntensity = 0.0;
};

// Reduced-order blast model. It combines engineering blast scaling (Brode) with
// a buoyant-thermal mushroom-cloud model and a radiating fireball. The model is
// designed for interactive visualisation with physically meaningful
// parameters; it is not a CFD solver. See docs/physics.md.
class BlastModel {
public:
    void Configure(const Charge& charge, const Environment& environment);
    void Reset();
    void Advance(double dt);

    const BlastState& State() const { return state_; }
    const Charge& GetCharge() const { return charge_; }
    const Environment& GetEnvironment() const { return environment_; }

    // Brode's incident overpressure fit. `scaledDistance` is Z = R / W^(1/3)
    // with R in metres and W in kg TNT.
    static double PeakOverpressurePa(double scaledDistance, double ambientPressurePa);

    double ScaledDistance(double distanceM) const { return charge_.ScaledDistance(distanceM); }
    double PeakOverpressureAtDistance(double distanceM) const;
    double ShockSpeedAtDistance(double distanceM) const;

    // Time at which the shock front reaches `distanceM` (slant range). Returns
    // -1 while the front has not arrived yet.
    double ArrivalTimeS(double distanceM) const;

    // Duration of the positive overpressure phase (empirical).
    double PositivePhaseDurationS(double distanceM) const;

    // Incident overpressure of the Friedlander waveform at a fixed observer.
    double OverpressureAtTime(double distanceM, double time) const;

    // Radiant exposure in J/m^2 at a given slant range.
    double ThermalFluenceJm2(double distanceM) const;

    double RadiusForOverpressure(double targetPa) const;
    double RadiusForThermalFluence(double targetJm2) const;

    double FireballMaxRadiusM() const { return fireballMaxRadius_; }
    double CloudTopHeightM() const;

// Conservative visual cloud top used by the readouts. The baked volume box
// uses a tighter 1.6 R fit and the camera a 2.4 R display margin.
    double ObserverSlantRange(double groundDistanceM) const;

private:
    double ShockSpeed(double radiusM) const;
    void UpdateFireball();
    void RecordHistory();

    Charge charge_;
    Environment environment_;
    BlastState state_;

    double fireballMaxRadius_ = 0.0;
    double fireballGrowthTau_ = 0.05;
    double fireballCoolTau_ = 1.0;
    double cloudTemperatureExcess_ = 0.0;

    std::vector<double> historyTime_;
    std::vector<double> historyRadius_;
};

} // namespace ex::sim

#include "sim/BlastModel.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ex::sim {

namespace {

constexpr double kBrodeBlendStart = 0.85; // near/far-field fit crossover
constexpr double kBrodeBlendEnd = 1.00;
constexpr double kThermalFraction = 0.05;        // TNT yield emitted thermally
constexpr double kAtmosphericTransmission = 0.85; // broadband, clear air

double ClampPositive(double value) {
    return value > 0.0 ? value : 0.0;
}

double SmoothStep01(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Empirical Friedlander decay coefficient. The positive-phase tail is longer
// (smaller b) far from the charge, so a fixed value over-damps distant
// waveforms.
double FriedlanderDecay(double scaledDistance) {
    const double z = std::max(scaledDistance, 0.05);
    return std::clamp(0.9 + 2.2 / (1.0 + 0.45 * z), 0.9, 3.0);
}

} // namespace

double Environment::SaturationVaporPressurePa() const {
    // Magnus-Tetens over liquid water (valid for -45..60 degC).
    const double celsius = temperatureK - 273.15;
    return 610.94 * std::exp(17.625 * celsius / (celsius + 243.04));
}

double Environment::SpecificHumidity() const {
    // Mass of water vapour per mass of moist air.
    const double vapour = VaporPressurePa();
    return 0.622 * vapour / std::max(pressurePa - 0.378 * vapour, 1.0);
}

double Environment::AirDensity() const {
    const double vapour = VaporPressurePa();
    const double dry = (pressurePa - vapour) / (gasConstant * temperatureK);
    const double wet = vapour / (461.5 * temperatureK); // R_v for water vapour
    return dry + wet;
}

double Environment::SoundSpeed() const {
    return std::sqrt(gamma * pressurePa / std::max(AirDensity(), 1e-6));
}

double Environment::VirtualTemperatureK() const {
    return pressurePa / std::max(AirDensity() * gasConstant, 1e-6);
}

double Environment::WindX() const {
    return windSpeedMs * std::cos(windDirectionDeg * M_PI / 180.0);
}

double Environment::WindZ() const {
    return windSpeedMs * std::sin(windDirectionDeg * M_PI / 180.0);
}

double Charge::RadiusM() const {
    const double volume = std::max(tntMassKg, 1e-6) / kTntDensityKgM3;
    return std::cbrt(3.0 * volume / (4.0 * M_PI));
}

double Charge::CbrtMass() const {
    return std::cbrt(std::max(tntMassKg, 1e-6));
}

void BlastModel::Configure(const Charge& charge, const Environment& environment) {
    charge_ = charge;
    environment_ = environment;

    const double kilotons = std::max(charge_.tntMassKg, 1e-6) / 1.0e6;

    // Fireball radius. Large charges follow the nuclear fireball scaling
    // R ~ W_kt^0.4; small charges are dominated by their own detonation
    // products and follow R ~ W_kg^0.25. Taking the larger of both branches
    // gives a continuous, plausible curve from grams to kilotons.
    const double nuclearRadius = 50.0 * std::pow(kilotons, 0.4);
    const double conventionalRadius = 0.9 * std::pow(charge_.tntMassKg, 0.25);
    fireballMaxRadius_ = std::max(nuclearRadius, conventionalRadius);

    // Sachs scaling: at lower ambient pressure the same yield produces a
    // larger fireball (and a larger blast wave at a given distance).
    fireballMaxRadius_ *= std::cbrt(101325.0 / std::max(environment_.pressurePa, 1.0));

    // Growth and cooling time scales, fitted to the observed behaviour of
    // nuclear and large conventional fireballs.
    fireballGrowthTau_ = 0.05 * std::pow(kilotons, 0.25) + 0.004;
    fireballCoolTau_ = std::max(1.2 * std::pow(kilotons, 0.15), 0.75);

    // Thermal energy that remains in the rising cloud (~12 % of the yield).
    const double cloudEnergy = 0.12 * charge_.EnergyJ();
    const double cloudMass = environment_.AirDensity() * (4.0 / 3.0) * M_PI *
                             fireballMaxRadius_ * fireballMaxRadius_ * fireballMaxRadius_;
    const double deltaT = cloudEnergy / std::max(cloudMass * environment_.SpecificHeatCp(), 1.0);
    cloudTemperatureExcess_ = std::min(deltaT, 2500.0);

    Reset();
}

void BlastModel::Reset() {
    state_ = BlastState{};
    state_.time = 0.0;
    state_.shockRadius = std::max(charge_.RadiusM(), 0.25);
    state_.shockSpeed = ShockSpeed(state_.shockRadius);
    state_.shockMach = state_.shockSpeed / environment_.SoundSpeed();
    state_.shockOverpressurePa = PeakOverpressureAtDistance(state_.shockRadius);

    // The fireball is initially identical to the explosive charge.
    state_.fireballRadius = std::max(state_.shockRadius * 0.9, 0.1);
    state_.fireballTemperatureK = kFireballSurfaceTemperatureK;
    state_.fireballCenterY = charge_.heightOfBurstM;
    state_.fireballVisible = true;

    state_.cloudCenterY = charge_.heightOfBurstM;
    state_.cloudRadius = std::max(state_.fireballRadius, fireballMaxRadius_);
    state_.cloudTemperatureK = environment_.temperatureK + cloudTemperatureExcess_;
    state_.stemRadius = std::max(0.5, state_.cloudRadius * 0.2);

    state_.dustRingRadius = 0.0;
    state_.dustIntensity = 0.0;

    historyTime_.clear();
    historyRadius_.clear();
    historyTime_.reserve(8192);
    historyRadius_.reserve(8192);
    RecordHistory();
}

void BlastModel::Advance(double dt) {
    if (dt <= 0.0) {
        return;
    }

    double remaining = dt;
    int guard = 0;
    constexpr int kMaxIterations = 20000;

    while (remaining > 1e-9 && guard++ < kMaxIterations) {
        const double radius = std::max(state_.shockRadius, 0.1);
        const double speed = ShockSpeed(radius);

        // The step is limited by the relative change of the shock radius so
        // that the integration of dR/dt = U(R) stays stable.
        const double cfl = 0.08 * radius / std::max(speed, 1.0);
        const double step = std::min({remaining, std::max(cfl, 2e-5), 0.005});
        const double midpointRadius = radius + 0.5 * speed * step;
        const double midpointSpeed = ShockSpeed(midpointRadius);

        state_.shockRadius = radius + midpointSpeed * step;
        state_.time += step;
        remaining -= step;

        UpdateFireball();

        // --- Mushroom cloud: buoyant thermal with entrainment drag ---------
        const double riseSpeed = state_.cloudRiseSpeed;
        const double cloudRadius = std::max(state_.cloudRadius, fireballMaxRadius_);
        const double excessTemperature =
            std::min(cloudTemperatureExcess_ * std::pow(fireballMaxRadius_ / cloudRadius, 3.0),
                     cloudTemperatureExcess_);
        const double buoyancy = environment_.gravity * excessTemperature / environment_.temperatureK;
        const double drag = 0.6 * riseSpeed * std::abs(riseSpeed) / (2.0 * cloudRadius);

        const double newSpeed = std::clamp(riseSpeed + (buoyancy - drag) * step, 0.0, 120.0);
        state_.cloudRiseSpeed = newSpeed;
        state_.cloudRise += newSpeed * step;
        state_.cloudRadius = cloudRadius + (0.45 * newSpeed + 0.10 * fireballMaxRadius_) * step;

        state_.cloudCenterY = charge_.heightOfBurstM + state_.cloudRise;
        state_.cloudTemperatureK =
            environment_.temperatureK + excessTemperature +
            (state_.fireballTemperatureK - environment_.temperatureK) * 0.14;
        state_.cloudDriftX += environment_.WindX() * step;
        state_.cloudDriftZ += environment_.WindZ() * step;
        state_.stemRadius = std::max(0.5, state_.cloudRadius * 0.22);

        // The fireball is dragged along by the rising cloud but lags behind.
        state_.fireballCenterY =
            charge_.heightOfBurstM + std::min(state_.cloudRise * 0.35, state_.fireballRadius * 2.0);

        // --- Ground effects ------------------------------------------------
        const double height = charge_.heightOfBurstM;
        state_.dustRingRadius = state_.shockRadius > height
                                    ? std::sqrt(std::max(0.0, state_.shockRadius * state_.shockRadius -
                                                                  height * height))
                                    : 0.0;
        const double dustBuildUp = std::clamp((state_.time - 0.02) / 0.35, 0.0, 1.0);
        const double dustDecay = std::exp(-state_.time / 9.0);
        state_.dustIntensity = dustBuildUp * dustDecay;

        RecordHistory();
    }

    state_.shockSpeed = ShockSpeed(state_.shockRadius);
    state_.shockMach = state_.shockSpeed / environment_.SoundSpeed();
    state_.shockOverpressurePa = PeakOverpressureAtDistance(state_.shockRadius);
}

void BlastModel::UpdateFireball() {
    const double t = state_.time;

    // Radius growth follows a Sedov-like t^(2/5) trend that saturates at the
    // empirically observed maximum radius.
    const double growth = 1.0 - std::exp(-t / fireballGrowthTau_);
    const double radius = fireballMaxRadius_ * std::pow(std::max(growth, 0.0), 0.4);
    state_.fireballRadius = std::max(radius, charge_.RadiusM() * 0.9);

    const double formationTime = 4.0 * fireballGrowthTau_;
    if (t <= formationTime) {
        state_.fireballTemperatureK = kFireballSurfaceTemperatureK;
    } else {
        const double decay = std::exp(-(t - formationTime) / fireballCoolTau_);
        state_.fireballTemperatureK =
            environment_.temperatureK + (kFireballSurfaceTemperatureK - environment_.temperatureK) * decay;
    }
    state_.fireballVisible = state_.fireballTemperatureK > environment_.temperatureK + 250.0;
}

void BlastModel::RecordHistory() {
    // Store the shock front trajectory so observer arrival times can be
    // interpolated later.
    if (!historyRadius_.empty() && state_.time - historyTime_.back() < 1e-6) {
        return;
    }
    historyTime_.push_back(state_.time);
    historyRadius_.push_back(state_.shockRadius);
}

double BlastModel::PeakOverpressurePa(double scaledDistance, double ambientPressurePa) {
    const double z = std::max(scaledDistance, 0.05);
    const double z2 = z * z;
    const double z3 = z2 * z;

    // Brode (1955): near-field and far-field fits, expressed in bar. The two
    // branches are blended over [0.85, 1.0] so the curve stays continuous and
    // monotonic; the inverse bisection relies on that.
    const double nearField = 6.7 / z3 + 1.0;
    const double farField = 0.975 / z + 1.455 / z2 + 5.85 / z3 - 0.019;
    const double blend =
        SmoothStep01((z - kBrodeBlendStart) / (kBrodeBlendEnd - kBrodeBlendStart));
    double bar = nearField + (farField - nearField) * blend;

    // Brode's far-field fit turns negative beyond Z ~ 55. Continue with an
    // acoustic 1/r tail so that the shock front keeps moving at the speed of
    // sound instead of pinning to ambient pressure.
    if (z > 50.0 || bar < 0.0) {
        bar = 0.055 / z;
    }

    const double pressure = ClampPositive(bar) * 1.0e5;
    return pressure * (ambientPressurePa / 101325.0);
}

double BlastModel::PeakOverpressureAtDistance(double distanceM) const {
    // Sachs scaling: the effective scaled distance shrinks with the cube root
    // of the ambient pressure ratio, so a charge at altitude has a stronger
    // blast at the same physical range.
    const double pressureRatio = environment_.pressurePa / 101325.0;
    const double z =
        charge_.ScaledDistance(std::max(distanceM, 0.01)) * std::cbrt(pressureRatio);
    return PeakOverpressurePa(z, environment_.pressurePa);
}

double BlastModel::ShockSpeed(double radiusM) const {
    const double overpressure = PeakOverpressureAtDistance(radiusM);
    const double gamma = environment_.gamma;
    // Rankine-Hugoniot relation between peak overpressure and shock Mach number.
    const double machSquared =
        1.0 + (gamma + 1.0) / (2.0 * gamma) * overpressure / environment_.pressurePa;
    const double mach = std::sqrt(std::max(1.0, machSquared));
    return std::max(environment_.SoundSpeed(), mach * environment_.SoundSpeed());
}

double BlastModel::ShockSpeedAtDistance(double distanceM) const {
    return ShockSpeed(distanceM);
}

double BlastModel::ArrivalTimeS(double distanceM) const {
    if (historyRadius_.empty()) {
        return 0.0;
    }
    if (distanceM <= historyRadius_.front()) {
        return historyTime_.front();
    }
    if (distanceM > historyRadius_.back()) {
        return -1.0;
    }

    const auto upper = std::lower_bound(historyRadius_.begin(), historyRadius_.end(), distanceM);
    const std::size_t index = std::size_t(upper - historyRadius_.begin());
    if (index == 0) {
        return historyTime_.front();
    }
    const double r0 = historyRadius_[index - 1];
    const double r1 = historyRadius_[index];
    const double t0 = historyTime_[index - 1];
    const double t1 = historyTime_[index];
    const double alpha = (distanceM - r0) / std::max(r1 - r0, 1e-9);
    return t0 + (t1 - t0) * alpha;
}

double BlastModel::PositivePhaseDurationS(double distanceM) const {
    // Approximated by the time the shock needs to travel roughly one blast
    // radius. This reproduces the correct order of magnitude over the range
    // relevant for visualisation (see docs/physics.md).
    const double speed = std::max(ShockSpeedAtDistance(distanceM), environment_.SoundSpeed());
    return std::clamp(1.1 * distanceM / speed, 1e-4, 5.0);
}

double BlastModel::EstimateArrivalTimeS(double distanceM) const {
    double radius = std::max(charge_.RadiusM(), 0.1);
    const double target = std::max(distanceM, radius);
    double time = 0.0;
    int guard = 0;

    while (radius < target && guard++ < 200000) {
        const double speed = std::max(ShockSpeed(radius), environment_.SoundSpeed());
        const double step = std::clamp(0.08 * radius / speed, 1e-6, 0.01);
        const double midpoint = radius + 0.5 * speed * step;
        const double midpointSpeed =
            std::max(ShockSpeed(midpoint), environment_.SoundSpeed());
        radius += midpointSpeed * step;
        time += step;
        if (time > 120.0) {
            break;
        }
    }
    return time;
}

double BlastModel::OverpressureImpulsePaS(double distanceM) const {
    const double peak = PeakOverpressureAtDistance(distanceM);
    const double duration = PositivePhaseDurationS(distanceM);
    const double pressureRatio = std::cbrt(environment_.pressurePa / 101325.0);
    const double effectiveZ = charge_.ScaledDistance(distanceM) * pressureRatio;
    const double b = FriedlanderDecay(effectiveZ);
    // Integral of (1 - x) * exp(-b x) over x in [0, 1].
    const double integral = (b - 1.0 + std::exp(-b)) / (b * b);
    return peak * duration * integral;
}

double BlastModel::OverpressureAtTime(double distanceM, double time) const {
    const double arrival = ArrivalTimeS(distanceM);
    if (arrival < 0.0 || time < arrival) {
        return 0.0;
    }
    const double duration = PositivePhaseDurationS(distanceM);
    const double local = time - arrival;
    if (local > duration) {
        return 0.0;
    }
    const double peak = PeakOverpressureAtDistance(distanceM);
    // Friedlander waveform: p(t) = p_s (1 - t/t+) exp(-b t/t+), with the decay
    // coefficient depending on the effective scaled distance.
    const double pressureRatio = std::cbrt(environment_.pressurePa / 101325.0);
    const double effectiveZ = charge_.ScaledDistance(distanceM) * pressureRatio;
    const double decay = FriedlanderDecay(effectiveZ);
    const double normalized = local / duration;
    return peak * (1.0 - normalized) * std::exp(-decay * normalized);
}

double BlastModel::ThermalFluenceJm2(double distanceM) const {
    // For TNT most of the yield is mechanical; only a fraction is emitted
    // thermally. The transmission is a constant clear-air broadband estimate.
    const double radius = std::max(distanceM, charge_.RadiusM());
    return kThermalFraction * charge_.EnergyJ() * kAtmosphericTransmission /
           (4.0 * M_PI * radius * radius);
}

double BlastModel::RadiusForOverpressure(double targetPa) const {
    if (targetPa <= 0.0) {
        return 0.0;
    }
    double low = std::max(charge_.RadiusM(), 0.1);
    double high = 1.0e5;
    if (PeakOverpressureAtDistance(low) < targetPa) {
        return low;
    }
    for (int i = 0; i < 90; ++i) {
        const double mid = std::sqrt(low * high);
        if (PeakOverpressureAtDistance(mid) > targetPa) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return 0.5 * (low + high);
}

double BlastModel::RadiusForThermalFluence(double targetJm2) const {
    if (targetJm2 <= 0.0) {
        return 0.0;
    }
    const double numerator =
        kThermalFraction * charge_.EnergyJ() * kAtmosphericTransmission;
    return std::sqrt(numerator / (4.0 * M_PI * targetJm2));
}

double BlastModel::CloudTopHeightM() const {
    return state_.cloudCenterY + 2.2 * state_.cloudRadius;
}

double BlastModel::ObserverSlantRange(double groundDistanceM) const {
    const double h = charge_.heightOfBurstM;
    return std::sqrt(groundDistanceM * groundDistanceM + h * h);
}

} // namespace ex::sim

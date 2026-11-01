#include "scene/FireModel.h"

#include "core/DetMath.h"

#include <algorithm>
#include <cmath>

namespace ex::scene {

namespace fire {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kStefanBoltzmann = 5.670374419e-8; // W / m^2 / K^4
// Flames of a developed compartment or crown fire: 1000-1300 K, soot-laden
// (emissivity 0.9). SFPE Handbook of Fire Protection Engineering, radiation
// chapter; the emissive power is about 106 kW/m^2.
constexpr double kFlameTemperatureK = 1200.0;
constexpr double kFlameEmissivity = 0.9;

// Thermally thick ignition: (pi / 4) * k * rho * c * (T_ig - T_0)^2
// (Drysdale, "An Introduction to Fire Dynamics", ch. 6).
constexpr double ThickGamma(double kRhoC, double deltaT) {
    return 0.25 * kPi * kRhoC * deltaT * deltaT;
}

// Building: softwood finishes and contents, k = 0.17 W/m/K, rho = 500 kg/m^3,
// c = 2.5 kJ/kg/K, piloted ignition at 623 K from 293 K; q_cr 12.5 kW/m^2
// (Babrauskas, "Ignition Handbook"). Fuel load 35 kg/m^2 of floor (wood
// equivalent, 18 MJ/kg) is the Eurocode 1 mean of offices (420 MJ/m^2) and
// dwellings (780 MJ/m^2). The mass flux 0.011 kg/m^2/s is the open-burning
// flux of wood over the envelope, giving burn times of about an hour.
constexpr Material kBuilding = {35.0f, 0.0f, 0.011f, 300.0f, 0.4f, 12500.0f, false,
                                ThickGamma(0.17 * 500.0 * 2500.0, 330.0)};
// Vehicle: plastics and paint, k = 0.2, rho = 1100, c = 1.5 kJ/kg/K, T_ig 623 K;
// 40 kg of combustible mass, a burn time of about five minutes and a growth of
// two minutes (passenger-car fire tests, NIST).
constexpr Material kVehicle = {0.0f, 40.0f, 0.0045f, 120.0f, 0.6f, 13000.0f, false,
                               ThickGamma(0.2 * 1100.0 * 1500.0, 330.0)};
// Tree: dry foliage, thermally thin, rho c delta = 500 kg/m^3 * 2 kJ/kg/K * 0.5 mm,
// ignition at 573 K from 293 K, q_cr 10 kW/m^2; 15 kg of fine fuel that burns
// off in tens of seconds (torching, Rothermel fuel models).
constexpr Material kTree = {0.0f, 15.0f, 0.004f, 15.0f, 0.7f, 10000.0f, true,
                            500.0 * 2000.0 * 0.0005 * 280.0};

// Quarter of the rectangle: configuration factor from an element at the
// rectangle's corner normal, sides a and b, distance c (Howell catalogue).
double CornerFactor(double a, double b, double c) {
    const double ra = std::sqrt(a * a + c * c);
    const double rb = std::sqrt(b * b + c * c);
    return (a / ra * core::det::Atan2(b / ra, 1.0) +
            b / rb * core::det::Atan2(a / rb, 1.0)) /
           (2.0 * kPi);
}

} // namespace

const Material& MaterialFor(SceneObjectType type) {
    switch (type) {
    case SceneObjectType::Vehicle:
        return kVehicle;
    case SceneObjectType::Tree:
        return kTree;
    default:
        return kBuilding;
    }
}

float EnvelopeAreaM2(const SceneObject& object) {
    const float x = object.size.x;
    const float y = object.size.y;
    const float z = object.size.z;
    return 2.0f * (x * y + z * y) + x * z;
}

float InitialFuelKg(const SceneObject& object) {
    const Material& material = MaterialFor(object.type);
    if (material.fuelKgPerFloorAreaM2 > 0.0f) {
        const float floors = std::max(1.0f, std::floor(object.size.y / 3.2f + 0.5f));
        return material.fuelKgPerFloorAreaM2 * object.size.x * object.size.z * floors;
    }
    return material.fuelKg;
}

double FlameEmissivePowerWm2() {
    const double t2 = kFlameTemperatureK * kFlameTemperatureK;
    return kFlameEmissivity * kStefanBoltzmann * t2 * t2;
}

float BlastIgnitionFluenceJm2(SceneObjectType type, double pulseS) {
    const Material& material = MaterialFor(type);
    const double pulse = std::max(pulseS, 0.01);
    const double surface = material.thin ? material.ignitionParam
                                         : std::sqrt(material.ignitionParam * pulse);
    return float(material.criticalFluxWm2 * pulse + surface);
}

double RectangleViewFactor(double width, double height, double distance) {
    const double c = std::max(distance, 1e-3);
    const double factor = 4.0 * CornerFactor(0.5 * width, 0.5 * height, c);
    return std::clamp(factor, 0.0, 1.0);
}

} // namespace fire

namespace {

constexpr float kSpreadCutoffM = 30.0f;
constexpr float kExtinguishFraction = 0.02f;
constexpr float kDecayFraction = 0.3f;

struct Placement {
    float x;
    float z;
};

Placement PlacementOf(const SceneObject& object, const PhysicsWorld& physics) {
    if (object.bodyIndex >= 0 && object.bodyIndex < int(physics.Bodies().size())) {
        const DirectX::XMFLOAT3& p = physics.Bodies()[object.bodyIndex].position;
        return {p.x, p.z};
    }
    return {object.position.x, object.position.z};
}

// Horizontal extent of the object's footprint along the unit direction (ux, uz)
// (half length) and perpendicular to it (full width). The yaw convention is the
// one of the body orientation: x axis (cos, -sin), z axis (sin, cos).
void FootprintExtent(const SceneObject& object, float ux, float uz, float& halfAlong,
                     float& widthAcross) {
    const double yaw = double(object.yaw) * 3.14159265358979323846 / 180.0;
    const float c = float(core::det::Cos(yaw));
    const float s = float(core::det::Sin(yaw));
    const float alongX = std::abs(ux * c - uz * s);
    const float alongZ = std::abs(ux * s + uz * c);
    halfAlong = 0.5f * (alongX * object.size.x + alongZ * object.size.z);
    const float vx = -uz;
    const float vz = ux;
    const float acrossX = std::abs(vx * c - vz * s);
    const float acrossZ = std::abs(vx * s + vz * c);
    widthAcross = acrossX * object.size.x + acrossZ * object.size.z;
}

} // namespace

void Scene::StartFire(SceneObject& object) {
    object.burning = true;
    object.burnedOut = false;
    object.burnElapsedS = 0.0f;
    object.fuelKg = fire::InitialFuelKg(object);
    object.fireIntensity = 0.0f;
}

void Scene::Ignite(size_t objectIndex) {
    if (objectIndex >= objects_.size()) {
        return;
    }
    SceneObject& object = objects_[objectIndex];
    if (object.burning || object.burnedOut) {
        return;
    }
    if (firePropagation_) {
        StartFire(object);
    } else {
        object.burning = true;
    }
}

int Scene::BurningCount() const {
    int count = 0;
    for (const SceneObject& object : objects_) {
        count += object.burning ? 1 : 0;
    }
    return count;
}

int Scene::BurnedOutCount() const {
    int count = 0;
    for (const SceneObject& object : objects_) {
        count += object.burnedOut ? 1 : 0;
    }
    return count;
}

void Scene::StepFire(float dt) {
    // Burn: t^2 growth to the developed rate, fuel consumption, linear decay
    // of the intensity below 30 % of the fuel, extinction at 2 %.
    for (SceneObject& object : objects_) {
        if (!object.burning) {
            continue;
        }
        const fire::Material& material = fire::MaterialFor(object.type);
        const float initial = std::max(fire::InitialFuelKg(object), 1e-3f);
        object.burnElapsedS += dt;
        const float growth = std::min(1.0f, object.burnElapsedS / material.growthS);
        const float fraction = object.fuelKg / initial;
        const float decay = fraction < kDecayFraction
                                ? std::max(fraction / kDecayFraction, 0.05f)
                                : 1.0f;
        object.fireIntensity = growth * growth * decay;
        const float rate =
            material.massFluxKgM2s * fire::EnvelopeAreaM2(object) * object.fireIntensity;
        object.fuelKg -= rate * dt;
        if (object.fuelKg <= kExtinguishFraction * initial) {
            object.fuelKg = 0.0f;
            object.burning = false;
            object.burnedOut = true;
            object.fireIntensity = 0.0f;
        }
    }

    // Spread: radiation from each burning object onto every unburnt object
    // within the cutoff. The heat-flux dose follows the ignition model of the
    // receiver material.
    const double emissive = fire::FlameEmissivePowerWm2();
    for (const SceneObject& source : objects_) {
        if (!source.burning || source.fireIntensity < 0.02f) {
            continue;
        }
        const fire::Material& sourceMaterial = fire::MaterialFor(source.type);
        const Placement from = PlacementOf(source, physics_);
        const double power = emissive * sourceMaterial.coverage * source.fireIntensity;
        const float sourceRadius = 0.5f * std::sqrt(source.size.x * source.size.x +
                                                    source.size.z * source.size.z);
        for (SceneObject& target : objects_) {
            if (target.burning || target.burnedOut || &target == &source) {
                continue;
            }
            const Placement to = PlacementOf(target, physics_);
            const float dx = to.x - from.x;
            const float dz = to.z - from.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            const float targetRadius = 0.5f * std::sqrt(target.size.x * target.size.x +
                                                        target.size.z * target.size.z);
            if (distance > kSpreadCutoffM + sourceRadius + targetRadius) {
                continue;
            }
            const float ux = distance > 1e-4f ? dx / distance : 1.0f;
            const float uz = distance > 1e-4f ? dz / distance : 0.0f;
            float sourceHalf = 0.0f;
            float sourceWidth = 0.0f;
            float targetHalf = 0.0f;
            float targetWidth = 0.0f;
            FootprintExtent(source, ux, uz, sourceHalf, sourceWidth);
            FootprintExtent(target, ux, uz, targetHalf, targetWidth);
            const double gap = std::max(double(distance - sourceHalf - targetHalf), 0.5);
            if (gap > kSpreadCutoffM) {
                continue;
            }
            const double factor =
                fire::RectangleViewFactor(sourceWidth, source.size.y, gap);
            const double flux = power * factor;
            const fire::Material& material = fire::MaterialFor(target.type);
            const double excess = flux - material.criticalFluxWm2;
            if (excess <= 0.0) {
                continue;
            }
            target.ignitionDose +=
                float(material.thin ? excess * dt : excess * excess * dt);
            if (double(target.ignitionDose) >= material.ignitionParam) {
                StartFire(target);
            }
        }
    }
}

} // namespace ex::scene

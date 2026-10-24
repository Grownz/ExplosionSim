#include "scene/SceneTypes.h"

#include "core/Random.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace ex::scene {

namespace {

constexpr DirectX::XMFLOAT4 kBuildingColors[4] = {
    {0.62f, 0.58f, 0.52f, 1.0f},
    {0.52f, 0.52f, 0.50f, 1.0f},
    {0.56f, 0.40f, 0.32f, 1.0f},
    {0.70f, 0.68f, 0.62f, 1.0f},
};

uint64_t HashObject(uint64_t hash, const SceneObject& object) {    auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    };
    mix(object.id);
    mix(uint64_t(object.type));
    mix(uint64_t(int(std::lround(object.position.x * 100.0f))));
    mix(uint64_t(int(std::lround(object.position.z * 100.0f))));
    mix(uint64_t(int(std::lround(object.yaw * 10.0f))));
    mix(uint64_t(int(std::lround(object.size.x * 100.0f))));
    mix(uint64_t(int(std::lround(object.size.y * 100.0f))));
    mix(uint64_t(int(std::lround(object.size.z * 100.0f))));
    mix(uint64_t(object.meshVariant));
    return hash;
}

struct DamageThresholds {
    float pressureLight;
    float pressureHeavy;
    float pressureDestroy;
    float impulseLight;
    float impulseHeavy;
    float impulseDestroy;
};

DamageThresholds ThresholdsFor(SceneObjectType type) {
    switch (type) {
    case SceneObjectType::Building:
        return {7000.0f, 20000.0f, 45000.0f, 150.0f, 500.0f, 1500.0f};
    case SceneObjectType::Vehicle:
        return {10000.0f, 25000.0f, 50000.0f, 200.0f, 600.0f, 1500.0f};
    case SceneObjectType::Tree:
        return {8000.0f, 18000.0f, 35000.0f, 150.0f, 400.0f, 900.0f};
    default:
        return {1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f};
    }
}

DamageState EvaluateDamage(float pressure, float impulse,
                           const DamageThresholds& thresholds) {
    if (pressure >= thresholds.pressureDestroy || impulse >= thresholds.impulseDestroy) {
        return DamageState::Destroyed;
    }
    if (pressure >= thresholds.pressureHeavy || impulse >= thresholds.impulseHeavy) {
        return DamageState::Heavy;
    }
    if (pressure >= thresholds.pressureLight || impulse >= thresholds.impulseLight) {
        return DamageState::Light;
    }
    return DamageState::Intact;
}

} // namespace

void Scene::Clear() {
    settings_ = SceneSettings{};
    objects_.clear();
    debrisSpawns_.clear();
    fireSpawns_.clear();
    physics_.Clear();
    hash_ = 0;
    extent_ = 0.0f;
    time_ = 0.0;
    physicsAccumulator_ = 0.0;
}

void Scene::Generate(const SceneSettings& settings, const sim::BlastModel& blast) {
    settings_ = settings;
    objects_.clear();
    debrisSpawns_.clear();
    fireSpawns_.clear();
    physics_.Clear();
    hash_ = 0;
    extent_ = 0.0f;
    time_ = 0.0;
    physicsAccumulator_ = 0.0;

    if (!settings_.enabled) {
        return;
    }

    const sim::Charge& charge = blast.GetCharge();
    const double fireball = std::max(blast.FireballMaxRadiusM(), 0.1);
    const double minRadius = std::max({fireball * 2.0, charge.RadiusM() * 2.0, 6.0});
    extent_ = float(std::clamp(10.0 * fireball + 20.0, 45.0, 160.0));

    const int budget =
        std::max(0, int(800.0f * std::clamp(settings_.density, 0.0f, 4.0f)));
    if (budget == 0) {
        return;
    }

    const int buildingBudget = settings_.buildings ? int(budget * 0.45f) : 0;
    const int vehicleBudget = settings_.vehicles ? int(budget * 0.20f) : 0;
    const int treeBudget = settings_.vegetation
                               ? std::max(0, budget - buildingBudget - vehicleBudget)
                               : 0;

    core::Rng rng(uint64_t(settings_.seed) * 0x9E3779B97F4A7C15ull + 0xC0FFEEull);

    const float block = 16.0f;
    const float road = 6.0f;
    const float pitch = block + road;
    const int cells = int(extent_ / pitch) + 1;
    const double burstHeight = charge.heightOfBurstM;

    auto addObject = [&](SceneObjectType type, float x, float z, float yaw,
                         DirectX::XMFLOAT3 size, int variant) {
        SceneObject object;
        object.id = uint32_t(objects_.size());
        object.type = type;
        object.position = DirectX::XMFLOAT3(x, 0.0f, z);
        object.yaw = yaw;
        object.size = size;
        object.meshVariant = variant;
        object.swayPhase = rng.Float01() * 6.2831853f;
        object.color = type == SceneObjectType::Building
                           ? kBuildingColors[variant & 3]
                           : DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

        if (type == SceneObjectType::Building) {
            const int quality = std::clamp(settings_.quality, 0, 2);
            const int nx = quality >= 2 ? 3 : (quality >= 1 ? 2 : 1);
            const int ny = quality >= 2 ? 4 : (quality >= 1 ? 3 : 2);
            const int nz = nx;
            const float sx = size.x / float(nx);
            const float sy = size.y / float(ny);
            const float sz = size.z / float(nz);
            for (int ix = 0; ix < nx; ++ix) {
                for (int iy = 0; iy < ny; ++iy) {
                    for (int iz = 0; iz < nz; ++iz) {
                        SceneChunk chunk;
                        chunk.center = DirectX::XMFLOAT3(
                            x - size.x * 0.5f + (float(ix) + 0.5f) * sx,
                            (float(iy) + 0.5f) * sy,
                            z - size.z * 0.5f + (float(iz) + 0.5f) * sz);
                        chunk.half = DirectX::XMFLOAT3(sx * 0.5f, sy * 0.5f, sz * 0.5f);
                        chunk.color = DirectX::XMFLOAT4(
                            object.color.x * 0.82f, object.color.y * 0.80f,
                            object.color.z * 0.78f, 1.0f);
                        object.chunks.push_back(chunk);
                    }
                }
            }
        }

        const double dx = double(x);
        const double dz = double(z);
        const double dy = double(size.y) * 0.5 - burstHeight;
        object.distanceToBurst = float(std::sqrt(dx * dx + dz * dz + dy * dy));
        if (object.distanceToBurst < minRadius) {
            return;
        }

        object.arrivalTime = float(blast.EstimateArrivalTimeS(object.distanceToBurst));
        object.peakOverpressurePa =
            float(blast.PeakOverpressureAtDistance(object.distanceToBurst));
        object.impulsePaS = float(blast.OverpressureImpulsePaS(object.distanceToBurst));
        object.thermalFluenceJm2 =
            float(blast.ThermalFluenceJm2(object.distanceToBurst));
        objects_.push_back(object);
    };

    int buildings = 0;
    for (int ix = -cells; ix <= cells && buildings < buildingBudget; ++ix) {
        for (int iz = -cells; iz <= cells && buildings < buildingBudget; ++iz) {
            const float cx = ix * pitch + block * 0.5f;
            const float cz = iz * pitch + block * 0.5f;
            if (std::abs(cx) > extent_ || std::abs(cz) > extent_) {
                continue;
            }
            if (rng.Float01() < 0.15f) {
                continue; // occasional empty lot
            }

            const float jitterX = rng.Range(-3.0f, 3.0f);
            const float jitterZ = rng.Range(-3.0f, 3.0f);
            const float width = rng.Range(6.0f, 12.0f);
            const float depth = rng.Range(6.0f, 12.0f);
            const float height = rng.Range(6.0f, 26.0f);
            const float yaw = rng.Float01() < 0.5f ? 0.0f : 90.0f;
            addObject(SceneObjectType::Building, cx + jitterX, cz + jitterZ, yaw,
                      DirectX::XMFLOAT3(width, height, depth),
                      int(rng.NextU32() % 4));
            ++buildings;
        }
    }

    int vehicles = 0;
    for (int ix = -cells; ix <= cells && vehicles < vehicleBudget; ++ix) {
        for (int iz = -cells; iz <= cells && vehicles < vehicleBudget; ++iz) {
            if (rng.Float01() < 0.55f) {
                continue;
            }
            const bool alongX = rng.Float01() < 0.5f;
            const float x = alongX ? ix * pitch + block + road * 0.5f
                                   : ix * pitch + rng.Range(0.0f, block);
            const float z = alongX ? iz * pitch + rng.Range(0.0f, block)
                                   : iz * pitch + block + road * 0.5f;
            if (std::abs(x) > extent_ || std::abs(z) > extent_) {
                continue;
            }
            const float yaw = alongX ? 90.0f : 0.0f;
            addObject(SceneObjectType::Vehicle, x, z, yaw,
                      DirectX::XMFLOAT3(rng.Range(1.7f, 2.2f), rng.Range(1.3f, 2.4f),
                                        rng.Range(3.8f, 6.5f)),
                      int(rng.NextU32() % 3));
            ++vehicles;
        }
    }

    int trees = 0;
    for (int ix = -cells; ix <= cells && trees < treeBudget; ++ix) {
        for (int iz = -cells; iz <= cells && trees < treeBudget; ++iz) {
            if (rng.Float01() < 0.45f) {
                continue;
            }
            const float x = ix * pitch + block + road * 0.5f +
                            rng.Range(-road * 0.4f, road * 0.4f);
            const float z = iz * pitch + block + road * 0.5f +
                            rng.Range(-road * 0.4f, road * 0.4f);
            if (std::abs(x) > extent_ || std::abs(z) > extent_) {
                continue;
            }
            const float height = rng.Range(5.0f, 12.0f);
            addObject(SceneObjectType::Tree, x, z, rng.Range(0.0f, 360.0f),
                      DirectX::XMFLOAT3(rng.Range(2.0f, 4.5f), height,
                                        rng.Range(0.25f, 0.45f)),
                      int(rng.NextU32() % 3));
            ++trees;
        }
    }

    hash_ = 1469598103934665603ull;
    for (const SceneObject& object : objects_) {
        hash_ = HashObject(hash_, object);
    }
}

void Scene::Update(const sim::BlastModel&, float dtSim) {
    if (dtSim > 0.0f) {
        time_ += double(dtSim);
    }

    for (SceneObject& object : objects_) {
        if (object.resolved) {
            continue;
        }
        if (time_ + 1e-4 < double(object.arrivalTime)) {
            continue;
        }

        object.resolved = true;
        const DamageThresholds thresholds = ThresholdsFor(object.type);
        object.damage = EvaluateDamage(object.peakOverpressurePa, object.impulsePaS,
                                       thresholds);

        switch (object.damage) {
        case DamageState::Light:
            object.damageAmount = 0.30f;
            break;
        case DamageState::Heavy:
            object.damageAmount = 0.65f;
            break;
        case DamageState::Destroyed:
            object.damageAmount = 1.00f;
            break;
        default:
            object.damageAmount = 0.0f;
            break;
        }

        if (object.damage == DamageState::Heavy ||
            object.damage == DamageState::Destroyed) {
            const bool destroyed = object.damage == DamageState::Destroyed;
            SceneDebrisSpawn spawn;
            spawn.position = DirectX::XMFLOAT3(object.position.x, object.size.y * 0.5f,
                                               object.position.z);
            spawn.count = object.type == SceneObjectType::Building
                              ? (destroyed ? 14 : 6)
                              : (destroyed ? 8 : 4);
            spawn.scale = (object.type == SceneObjectType::Building ? 0.55f : 0.30f) *
                          (destroyed ? 1.0f : 0.7f);
            spawn.speed = destroyed ? 22.0f : 12.0f;
            debrisSpawns_.push_back(spawn);
        }

        const float ignition = object.type == SceneObjectType::Tree
                                   ? 5000.0f
                                   : (object.type == SceneObjectType::Vehicle ? 15000.0f
                                                                             : 10000.0f);
        if (object.thermalFluenceJm2 >= ignition) {
            object.burning = true;
        }

        if (object.type == SceneObjectType::Building) {
            if ((object.damage == DamageState::Heavy ||
                 object.damage == DamageState::Destroyed) &&
                !object.collapsed) {
                FractureBuilding(object);
            }
        } else if ((object.damage == DamageState::Heavy ||
                    object.damage == DamageState::Destroyed) &&
                   object.bodyIndex < 0) {
            ActivateBody(object);
        }
    }

    for (SceneObject& object : objects_) {
        if (!object.burning) {
            continue;
        }
        object.burnTimer += dtSim;
        if (object.burnTimer >= 0.25f) {
            object.burnTimer -= 0.25f;
            SceneFireSpawn spawn;
            spawn.position = DirectX::XMFLOAT3(object.position.x, object.size.y * 0.5f,
                                               object.position.z);
            spawn.count = object.type == SceneObjectType::Tree ? 3 : 2;
            spawn.scale = object.type == SceneObjectType::Tree ? 0.8f : 0.5f;
            fireSpawns_.push_back(spawn);
        }
    }

    StepPhysics(dtSim);
}

void Scene::ActivateBody(SceneObject& object) {
    if (object.bodyIndex >= 0) {
        return;
    }

    RigidBody body;
    body.position = DirectX::XMFLOAT3(object.position.x, object.size.y * 0.5f,
                                      object.position.z);
    if (object.type == SceneObjectType::Tree) {
        body.halfExtents = DirectX::XMFLOAT3(object.size.x, object.size.y * 0.5f,
                                             object.size.x);
    } else {
        body.halfExtents = DirectX::XMFLOAT3(object.size.x * 0.5f,
                                             object.size.y * 0.5f,
                                             object.size.z * 0.5f);
    }
    body.boundingRadius = std::sqrt(body.halfExtents.x * body.halfExtents.x +
                                    body.halfExtents.y * body.halfExtents.y +
                                    body.halfExtents.z * body.halfExtents.z);
    body.mass = object.type == SceneObjectType::Vehicle ? 1500.0f : 600.0f;
    body.invMass = 1.0f / body.mass;

    const float halfYaw = object.yaw * 3.14159265f / 360.0f;
    body.orientation = DirectX::XMFLOAT4(0.0f, std::sin(halfYaw), 0.0f,
                                         std::cos(halfYaw));

    const float radialLength = std::sqrt(object.position.x * object.position.x +
                                         object.position.z * object.position.z);
    const float nx = radialLength > 1e-3f ? object.position.x / radialLength : 1.0f;
    const float nz = radialLength > 1e-3f ? object.position.z / radialLength : 0.0f;
    float speed = std::clamp(object.peakOverpressurePa / 5000.0f, 1.0f, 20.0f);
    if (object.damage == DamageState::Heavy) {
        speed *= 0.6f;
    }
    body.velocity = DirectX::XMFLOAT3(nx * speed, speed * 0.35f, nz * speed);
    const float sign = (object.id & 1u) ? 1.0f : -1.0f;
    body.angularVelocity = DirectX::XMFLOAT3(sign * 1.5f, sign * 0.7f, -sign * 1.1f);
    body.restitution = 0.25f;
    body.friction = 0.7f;

    object.bodyIndex = physics_.AddBody(body);
}

void Scene::FractureBuilding(SceneObject& object) {
    if (object.collapsed) {
        return;
    }
    object.collapsed = true;

    const float radialLength = std::sqrt(object.position.x * object.position.x +
                                         object.position.z * object.position.z);
    const float nx = radialLength > 1e-3f ? object.position.x / radialLength : 1.0f;
    const float nz = radialLength > 1e-3f ? object.position.z / radialLength : 0.0f;
    const float baseSpeed =
        std::clamp(object.peakOverpressurePa / 8000.0f, 1.0f, 12.0f);

    for (SceneChunk& chunk : object.chunks) {
        RigidBody body;
        body.position = chunk.center;
        body.halfExtents = chunk.half;
        body.boundingRadius = std::sqrt(chunk.half.x * chunk.half.x +
                                        chunk.half.y * chunk.half.y +
                                        chunk.half.z * chunk.half.z);
        const float volume = 8.0f * chunk.half.x * chunk.half.y * chunk.half.z;
        body.mass = std::clamp(volume * 350.0f, 80.0f, 15000.0f);
        body.invMass = 1.0f / body.mass;
        body.orientation = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        body.velocity = DirectX::XMFLOAT3(nx * baseSpeed * 0.8f, baseSpeed * 0.25f,
                                          nz * baseSpeed * 0.8f);
        body.angularVelocity = DirectX::XMFLOAT3(chunk.center.x * 0.08f,
                                                 chunk.center.z * 0.08f,
                                                 chunk.center.y * 0.05f);
        body.restitution = 0.15f;
        body.friction = 0.8f;
        chunk.bodyIndex = physics_.AddBody(body);
    }
}

void Scene::StepPhysics(float dtSim) {
    if (dtSim <= 0.0f) {
        return;
    }
    physicsAccumulator_ += double(dtSim);
    const double step = 1.0 / 120.0;
    int guard = 0;
    while (physicsAccumulator_ >= step && guard++ < 8) {
        physics_.Step(float(step));
        physicsAccumulator_ -= step;
    }
}

std::vector<SceneDebrisSpawn> Scene::ConsumeDebrisSpawns() {
    std::vector<SceneDebrisSpawn> result = std::move(debrisSpawns_);
    debrisSpawns_.clear();
    return result;
}

std::vector<SceneFireSpawn> Scene::ConsumeFireSpawns() {
    std::vector<SceneFireSpawn> result = std::move(fireSpawns_);
    fireSpawns_.clear();
    return result;
}

} // namespace ex::scene

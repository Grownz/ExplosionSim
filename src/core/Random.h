#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace ex::core {

// Deterministic xoshiro256** generator. Every particle owns an independent
// stream so simulation results do not depend on thread scheduling.
class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) { Seed(seed); }

    void Seed(uint64_t seed);

    uint32_t NextU32();
    uint64_t NextU64();

    float Float01() { return float(NextU32() >> 8) * (1.0f / 16777216.0f); }
    float Range(float minValue, float maxValue) { return minValue + (maxValue - minValue) * Float01(); }

    // Uniform direction on the unit sphere (Marsaglia's method).
    DirectX::XMFLOAT3 UnitVector();

    // Uniform point inside the unit ball.
    DirectX::XMFLOAT3 InsideUnitBall();

private:
    uint64_t state_[4]{};
};

} // namespace ex::core

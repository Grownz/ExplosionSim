#include "core/Random.h"

#include <cmath>

namespace ex::core {

namespace {

inline uint64_t Rotl(uint64_t value, int shift) {
    return (value << shift) | (value >> (64 - shift));
}

} // namespace

void Rng::Seed(uint64_t seed) {
    // splitmix64 expands a single seed into the xoshiro state.
    for (int i = 0; i < 4; ++i) {
        seed += 0x9E3779B97F4A7C15ull;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        state_[i] = z ^ (z >> 31);
    }
    if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0) {
        state_[0] = 1;
    }
}

uint64_t Rng::NextU64() {
    const uint64_t result = Rotl(state_[1] * 5, 7) * 9;
    const uint64_t t = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = Rotl(state_[3], 45);
    return result;
}

uint32_t Rng::NextU32() {
    return uint32_t(NextU64() >> 32);
}

DirectX::XMFLOAT3 Rng::UnitVector() {
    using namespace DirectX;
    for (;;) {
        const float x = Range(-1.0f, 1.0f);
        const float y = Range(-1.0f, 1.0f);
        const float z = Range(-1.0f, 1.0f);
        const float lengthSq = x * x + y * y + z * z;
        if (lengthSq > 1e-6f && lengthSq <= 1.0f) {
            const float inv = 1.0f / std::sqrt(lengthSq);
            return XMFLOAT3(x * inv, y * inv, z * inv);
        }
    }
}

DirectX::XMFLOAT3 Rng::InsideUnitBall() {
    using namespace DirectX;
    const XMFLOAT3 direction = UnitVector();
    const float radius = std::cbrt(Float01());
    return XMFLOAT3(direction.x * radius, direction.y * radius, direction.z * radius);
}

} // namespace ex::core

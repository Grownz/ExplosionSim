#include "euler_common.hlsli"

RWTexture3D<float4> RenderVolume : register(u0);

// Converts the conservative state into the render field used by the raymarcher:
//   R = extinction density, G = temperature / 12 000 K, B = dust, A = 1.
[numthreads(8, 8, 8)]
void CSRender(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= (uint3)Grid.xyz))
    {
        return;
    }

    float4 a = StateA.Load(int4(id, 0));
    float4 b = StateB.Load(int4(id, 0));

    float rho = max(a.x, 1e-6);
    float3 velocity = a.yzw / rho;
    float energy = b.x;
    float soot = saturate(b.y);

    float pressure = max((Ambient.w - 1.0) * (energy - 0.5 * rho * dot(velocity, velocity)), 1e-3);
    float temperature = AmbientTemperature(rho, pressure);

    float heat = saturate((temperature - Ambient.z) / 3000.0);
    float compression = saturate(rho / Ambient.x - 1.0);

    // Density gradient highlights the shock front without fogging the whole
    // post-shock region.
    int3 p = int3(id);
    float3 gradient = float3(LoadA(p + int3(1, 0, 0)).x - LoadA(p - int3(1, 0, 0)).x,
                             LoadA(p + int3(0, 1, 0)).x - LoadA(p - int3(0, 1, 0)).x,
                             LoadA(p + int3(0, 0, 1)).x - LoadA(p - int3(0, 0, 1)).x);
    float gradientMagnitude = length(gradient) / (2.0 * max(DomainMin.w, 1e-5));
    float shock = saturate(gradientMagnitude * 0.035);

    // Soot incandescence: the detonation products are optically thick and stay
    // hot through afterburning, which the adiabatic Euler solve does not
    // capture. Represent it as a temperature floor applied only to the product
    // mass, not to the ambient air.
    float sootDensity = max(b.y, 0.0);
    float products = smoothstep(0.004, 0.030, sootDensity);
    float fireFloor = 1200.0 + 1800.0 * products;
    float visibleTemperature =
        lerp(temperature, max(temperature, fireFloor), saturate(products * 4.0));

    float density = saturate(products * 0.90 + shock * 0.18);

    RenderVolume[id] = float4(density, saturate(visibleTemperature / 12000.0), 0.0, 1.0);
}

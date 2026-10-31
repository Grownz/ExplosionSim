#include "euler_common.hlsli"

EX_VK_BINDING(24) RWTexture3D<float4> RenderVolume : register(u0);

// Converts the conservative state into the render field used by the raymarcher:
//   R = extinction density, G = temperature / 12 000 K, B = dust, A = soot mass.
[numthreads(EULER_GROUP_X, EULER_GROUP_Y, EULER_GROUP_Z)]
void CSRender(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= (uint3)Grid.xyz))
    {
        return;
    }

    if (!IsActiveCell(int3(id)))
    {
        RenderVolume[id] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    if (IsSolidCell(int3(id)))
    {
        RenderVolume[id] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    float4 a = StateA.Load(int4(id, 0));
    float2 b = StateB.Load(int4(id, 0));

    float rho = max(a.x, 1e-6);
    float3 velocity = a.yzw / rho;
    float energy = b.x;

    float pressure = max((Ambient.w - 1.0) * (energy - 0.5 * rho * dot(velocity, velocity)), 1e-3);
    float temperature = AmbientTemperature(rho, pressure);

    // Isotropic 27-point smoothing (binomial weights 8/4/2/1) of the product
    // mass. The previous 7-point cross filter sampled only the axes, which left
    // a four-lobed pattern at the ground contact.
    int3 p = int3(id);
    float sootSum = 0.0;
    [unroll]
    for (int dz = -1; dz <= 1; ++dz)
    {
        [unroll]
        for (int dy = -1; dy <= 1; ++dy)
        {
            [unroll]
            for (int dx = -1; dx <= 1; ++dx)
            {
                const float weight = (dx == 0 ? 2.0 : 1.0) *
                                     (dy == 0 ? 2.0 : 1.0) *
                                     (dz == 0 ? 2.0 : 1.0);
                sootSum += weight * max(LoadB(p + int3(dx, dy, dz)).y, 0.0);
            }
        }
    }
    float sootDensity = max(sootSum / 64.0, 0.0);

    float products = smoothstep(0.004, 0.030, sootDensity);
    float smoke = smoothstep(0.0005, 0.008, sootDensity);

    // Soot incandescence: the detonation products are optically thick and stay
    // hot through afterburning, which the adiabatic Euler solve does not
    // capture. Represent it as a temperature floor applied only to the product
    // mass, decaying as the cloud cools.
    float age = max(Physics.w, 0.0);
    float incandescence = exp(-age / 1.15);
    float fireFloor = 650.0 + 2200.0 * products * incandescence;
    float visibleTemperature =
        lerp(temperature, max(temperature, fireFloor), saturate(products * 4.0));

    // Sub-grid radiative cooling: the adiabatic solve has no radiation loss, so
    // the products stay near the detonation temperature. Cool the *visual*
    // temperature with a ~1.6 s time constant so the cloud turns from white
    // through orange to dull red as it expands.
    visibleTemperature =
        Ambient.z + (visibleTemperature - Ambient.z) * exp(-age / 1.6);

    // The rising cap has expanded and mixed with cold air, while the stem and
    // the base surge are still hot. Fade the incandescence with height so the
    // cap reads as smoke and the base glows.
    float3 world = DomainMin.xyz + (float3(id) + 0.5) * DomainMin.w;
    float heightFraction =
        saturate((world.y - DomainMin.y) / max(DomainMax.y - DomainMin.y, 1.0));
    visibleTemperature =
        Ambient.z + (visibleTemperature - Ambient.z) * exp(-heightFraction * 2.2);

    // Only genuinely hot gas glows; the bulk of the products is smoke that is
    // lit by the sun and the remaining incandescent core.
    float glow = smoothstep(900.0, 2200.0, visibleTemperature);
    float emitTemperature = lerp(Ambient.z, visibleTemperature, glow);

    float density = saturate(smoke * 0.92);

    // The ground-reflected boundary layer spreads the products into a flat,
    // star-shaped disc on the coarse grid. Fade the lowest layer so it reads as
    // a thin dust skirt instead of a solid geometric shape.
    float groundFade = smoothstep(0.4 * DomainMin.w, 2.4 * DomainMin.w, world.y);
    density *= groundFade;

    RenderVolume[id] = float4(density, saturate(emitTemperature / 12000.0), 0.0,
                              saturate(sootDensity * 20.0));
}

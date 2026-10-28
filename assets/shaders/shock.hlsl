#include "common.hlsli"

Texture2D<float4> SceneColor : register(t0);
Texture2D<float> SceneDepth : register(t1);
SamplerState LinearSampler : register(s0);

// The blast front is a thin shell of compressed air. Its refractive index
// follows the Gladstone-Dale relation
//
//     n - 1 = K * rho,     K = 2.26e-4 m^3/kg (air, 589 nm)
//
// so the index jump across the front is K * rho0 * (rho2/rho1 - 1) with the
// Rankine-Hugoniot density ratio. Rays are deflected towards the density
// gradient; integrating that over the shell turns the sphere into a weak
// positive lens whose strongest bending sits at the limb, where the chord
// through the compressed layer is longest.
//
// The physical deflection is well below one pixel at the displayed distances,
// so the gain exaggerates it the same way high-speed schlieren footage is
// usually shown with an amplified density gradient.
static const float kGladstoneDale = 2.26e-4;
static const float kAmbientDensity = 1.225;
static const float kRefractionGain = 4000.0;

float4 PSShock(VSOut input) : SV_Target
{
    float4 original = SceneColor.SampleLevel(LinearSampler, input.uv, 0);

    const float radius = Misc3.w;
    const float mach = Blast.w;
    if (radius <= 0.5 || mach <= 1.01)
    {
        return original;
    }

    const float3 origin = CameraPosition.xyz;
    const float3 direction = RayDirection(input.uv);
    const float3 center = Blast.xyz;

    // Closest approach of the view ray to the burst centre.
    const float3 offset = center - origin;
    const float tca = dot(offset, direction);
    const float b2 = dot(offset, offset) - tca * tca;
    if (b2 >= radius * radius)
    {
        return original;
    }

    const float b = sqrt(max(b2, 0.0));
    const float shell = max(radius * 0.05, 0.75);
    const float halfChord = sqrt(max(radius * radius - b2, 0.0));
    const float tNear = tca - halfChord;
    // Once the front has passed the camera the visible shell is the far side,
    // so use the exit intersection instead of giving up.
    const float tHit = tNear > 0.0 ? tNear : tca + halfChord;
    if (tHit <= 0.0)
    {
        return original;
    }

    const float3 entry = origin + direction * tHit;
    if (entry.y < 0.0)
    {
        // The shell intersects the ground; the ground pass draws that part.
        return original;
    }

    // Do not distort geometry that is in front of the front.
    const float sceneDepth = SceneDepth.Load(int3(input.position.xy, 0));
    const float3 scenePosition = WorldFromUV(input.uv, sceneDepth);
    const float sceneDistance = length(scenePosition - origin);
    if (tHit > sceneDistance + 1.0)
    {
        return original;
    }

    // Rankine-Hugoniot density ratio across the front.
    const float gamma = 1.4;
    const float m2 = max(mach * mach, 1.0);
    const float densityRatio = (gamma + 1.0) * m2 / ((gamma - 1.0) * m2 + 2.0);
    const float deltaN = kGladstoneDale * kAmbientDensity * max(densityRatio - 1.0, 0.0);

    // The front fades into an acoustic wave as the Mach number drops.
    const float strength = saturate((mach - 1.0) / 0.6);

    // Longest chord through the compressed layer at the limb.
    const float limb = saturate((b - (radius - shell)) / shell);

    float magnitude = clamp(deltaN * kRefractionGain, 0.0, 0.028) * strength;
    magnitude *= 0.20 + 0.80 * limb;

    // Project the radial density gradient into screen space so the offset
    // follows the lens direction for any camera orientation. The linear part
    // of the view-projection is enough for a direction vector.
    const float3 radialWorld = normalize(entry - center);
    const float4 clipDirection = mul(ViewProj, float4(radialWorld, 0.0));
    float2 radial = float2(clipDirection.x, -clipDirection.y);
    const float radialLength = length(radial);
    radial = radialLength > 1e-6 ? radial / radialLength : float2(0.0, 0.0);

    // A positive lens bends the ray towards the axis, so the background is
    // sampled closer to the projected centre. The tiny per-channel spread is
    // the dispersion of the compressed air.
    const float2 shift = radial * magnitude;
    const float3 refracted = float3(
        SceneColor.SampleLevel(LinearSampler, input.uv - shift * 0.97, 0).r,
        SceneColor.SampleLevel(LinearSampler, input.uv - shift, 0).g,
        SceneColor.SampleLevel(LinearSampler, input.uv - shift * 1.04, 0).b);

    // A faint caustic rim keeps the front readable against the smooth sky,
    // where a pure distortion has almost no contrast to work with.
    const float rim = pow(limb, 4.0) * strength * saturate(deltaN * 5000.0);
    const float3 color = refracted * (1.0 - rim * 0.14) +
                         float3(0.55, 0.66, 0.82) * rim * 0.45;
    return float4(color, original.a);
}

#include "common.hlsli"

Texture3D<float4> VolumeTexture : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture3D<float4> NoiseTexture : register(t2);
Texture3D<float4> VolumeAux : register(t3);
SamplerState LinearSampler : register(s0);
SamplerState NoiseSampler : register(s1);

// Samples only the extinction channel. Used for the gradient-based shading.
float SampleDensity(float3 uvw)
{
    if (VolumeParams.y > 0.5)
    {
        return VolumeTexture.SampleLevel(LinearSampler, uvw, 0).r;
    }
    return VolumeTexture.SampleLevel(LinearSampler, uvw, 0).r;
}

// Front-to-back emission/absorption raymarching through the baked blast field.
// The result is premultiplied and blended with One / InvSrcAlpha.
float4 PSVolume(VSOut input) : SV_Target
{
    float3 rayOrigin = CameraPosition.xyz;
    float3 rayDirection = RayDirection(input.uv);

    float2 range = RayBoxRange(rayOrigin, rayDirection, VolumeMin.xyz, VolumeMax.xyz);
    range.x = max(range.x, 0.0);

    float sceneDepth = SceneDepth.Load(int3(input.position.xy, 0));
    float3 scenePosition = WorldFromUV(input.uv, sceneDepth);
    float sceneDistance = length(scenePosition - rayOrigin);
    range.y = min(range.y, sceneDistance);

    if (range.y <= range.x)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const int steps = (int)max(VolumeMax.w, 8.0);
    const float stepSize = (range.y - range.x) / float(steps);
    const float extinctionScale = VolumeMin.w;
    const float3 boxSize = VolumeMax.xyz - VolumeMin.xyz;

    float jitter = Hash12(input.position.xy + frac(Misc3.x * 61.0) * 17.0);
    float t = range.x + stepSize * jitter;

    float3 sunDirection = normalize(SunDirection.xyz);
    float phase = HenyeyGreenstein(dot(rayDirection, sunDirection), 0.35) * 4.0 * PI;
    float3 sunLight = SunColor.rgb * phase * 1.5 * VolumeParams2.x;
    float3 ambientLight = float3(0.20, 0.21, 0.24) * VolumeParams2.x;
    float3 windDrift = float3(0.0, -Misc3.x * 0.06, 0.0);

    float3 accum = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        float3 position = rayOrigin + rayDirection * t;

        // Domain warping with a tileable 3D noise texture. This breaks the
        // smooth analytic iso-surfaces into filaments and adds sub-voxel
        // detail without increasing the volume resolution.
        float fireballScale = max(Misc2.z, 0.25);
        float3 noiseCoordinate =
            position * (0.85 / fireballScale) + float3(0.0, -Misc3.x * 0.05, 0.0);
        float3 warp = NoiseTexture.SampleLevel(NoiseSampler, noiseCoordinate, 0).xyz - 0.5;
        // Fade the warp out for very small fireballs so the initial explosion
        // remains a smooth sphere rather than a noisy blob.
        float warpAmount = smoothstep(0.15, 0.50, fireballScale);
        float3 warpedPosition = position + warp * (0.22 * fireballScale * warpAmount);

        float3 uvw = (warpedPosition - VolumeMin.xyz) / max(boxSize, 1e-4);
        float4 sample;
        if (VolumeParams.y > 0.5)
        {
            // Split 1024³ volume: two RG8 planes.
            float4 low = VolumeTexture.SampleLevel(LinearSampler, uvw, 0);
            float4 high = VolumeAux.SampleLevel(LinearSampler, uvw, 0);
            sample = float4(low.rg, high.rg);
        }
        else
        {
            sample = VolumeTexture.SampleLevel(LinearSampler, uvw, 0);
        }
        float density = sample.r;

        if (density > 0.002)
        {
            float temperature = sample.g * 12000.0;
            float soot = sample.a;
            float dust = sample.b;

            // Fine detail from the high-frequency channel of the noise volume.
            float detailScale = 10.0 / fireballScale;
            float4 noiseDetail =
                NoiseTexture.SampleLevel(NoiseSampler, position * detailScale * 0.55 + 13.0, 0);

            float3 noisePosition = position * detailScale + windDrift;
            noisePosition.y *= 0.75;
            float structure = Fbm3(noisePosition);
            float fine = ValueNoise3(noisePosition * 3.0 + 11.0);
            density *= lerp(0.05, 2.40, structure) * (0.60 + 0.70 * fine);
            float detailAmount = VolumeParams.z;
            float structureAmount = VolumeParams.w;
            density *= lerp(1.0 - 0.95 * structureAmount, 1.0 + 1.40 * structureAmount,
                            structure);
            density *= lerp(1.0 - 0.25 * detailAmount, 1.0 + 0.35 * detailAmount,
                            noiseDetail.a);
            temperature *= lerp(1.0 - 0.45 * structureAmount, 1.0 + 0.30 * structureAmount,
                                structure);

            float heat = smoothstep(550.0, 1250.0, temperature);
            float3 blackbody = BlackbodyRGB(temperature);
            float coreBoost = 0.75 + 0.90 * smoothstep(2400.0, 4300.0, temperature);
            float3 emission = blackbody * density * heat * coreBoost * 1.10 * VolumeParams2.y;

            // Cold soot absorbs the fireball light; hot soot glows.
            float coldSoot = soot * saturate((2400.0 - temperature) / 1500.0);
            emission *= lerp(1.0, 0.10, coldSoot);

            float3 albedo = lerp(float3(0.05, 0.045, 0.04), float3(0.19, 0.16, 0.12), dust);
            float3 scattering = albedo * density * (sunLight + ambientLight);

            // Fireball light scattered by dust and smoke: warm near the blast,
            // dark farther out.
            float3 toFireball = Fireball.xyz - position;
            float fireDistanceSq = dot(toFireball, toFireball);
            float fireRadius = max(Misc2.z, 0.2);
            float fireLight = Fireball.w * 1.0 * fireRadius * fireRadius /
                              (fireDistanceSq + fireRadius * fireRadius * 1.2);
            scattering += albedo * density * fireLight * VolumeParams2.z * float3(1.0, 0.42, 0.12);

            // Directional sun shading from the density gradient. Without it the
            // single-scatter term lights the whole cloud uniformly and the
            // billows vanish.
            const float gradEps = 0.006;
            float gx = SampleDensity(uvw + float3(gradEps, 0.0, 0.0)) -
                       SampleDensity(uvw - float3(gradEps, 0.0, 0.0));
            float gy = SampleDensity(uvw + float3(0.0, gradEps, 0.0)) -
                       SampleDensity(uvw - float3(0.0, gradEps, 0.0));
            float gz = SampleDensity(uvw + float3(0.0, 0.0, gradEps)) -
                       SampleDensity(uvw - float3(0.0, 0.0, gradEps));
            float3 gradient = float3(gx, gy, gz) / (2.0 * gradEps);
            float gradientLength = length(gradient);
            float3 normal = gradientLength > 1e-4 ? -gradient / gradientLength
                                                  : float3(0.0, 1.0, 0.0);
            float sunShade = saturate(dot(normal, sunDirection));
            float skyShade = 0.5 + 0.5 * normal.y;
            float shade = 0.45 + 0.85 * sunShade + 0.30 * skyShade;
            scattering *= shade;
            emission *= lerp(1.0, shade, 0.5);

            accum += transmittance * (emission + scattering) * stepSize;
            transmittance *= exp(-density * extinctionScale * stepSize);

            if (transmittance < 0.02)
            {
                break;
            }
        }

        t += stepSize;
    }

    float alpha = 1.0 - transmittance;
    return float4(accum, alpha);
}

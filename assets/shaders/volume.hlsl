#include "common.hlsli"

Texture3D<float4> VolumeTexture : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture3D<float4> NoiseTexture : register(t2);
Texture3D<float4> VolumeAux : register(t3);
Texture3D<float> LightVolume : register(t4);
SamplerState LinearSampler : register(s0);
SamplerState NoiseSampler : register(s1);

// Samples only the extinction channel. Used for the gradient-based shading.
float SampleDensity(float3 uvw)
{
    // Both the single RGBA16F volume and the split 1024³ layout store the
    // extinction in the red channel of the main texture.
    return VolumeTexture.SampleLevel(LinearSampler, uvw, 0).r;
}

// View-independent sun transmittance precomputed by euler_light.hlsl.
float SampleLight(float3 position)
{
    const float3 uvw = (position - LightVolumeMin.xyz) /
                       max(LightVolumeMax.xyz - LightVolumeMin.xyz, 1e-4);
    return LightVolume.SampleLevel(LinearSampler, uvw, 0);
}

// Transmittance from a point inside the cloud towards the sun. Used as the
// fallback for the CPU-baked volume, which has no precomputed light volume.
float SunTransmittance(float3 position)
{
    const float3 sunDir = normalize(SunDirection.xyz);
    float2 range = RayBoxRange(position, sunDir, VolumeMin.xyz, VolumeMax.xyz);
    range.x = max(range.x, 0.0);
    if (range.y <= range.x)
    {
        return 1.0;
    }

    const int steps = 6;
    const float stepSize = (range.y - range.x) / float(steps);
    float t = range.x + stepSize * 0.5;
    float opticalDepth = 0.0;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        const float3 samplePosition = position + sunDir * t;
        const float3 uvw = (samplePosition - VolumeMin.xyz) /
                           max(VolumeMax.xyz - VolumeMin.xyz, 1e-4);
        opticalDepth += SampleDensity(uvw) * VolumeMin.w * stepSize;
        t += stepSize;
    }
    return exp(-opticalDepth);
}

// Front-to-back emission/absorption raymarching through the baked blast field.
// The result is premultiplied and blended with One / InvSrcAlpha.
float4 PSVolume(VSOut input) : SV_Target
{
    float3 rayOrigin = CameraPosition.xyz;
    float3 rayDirection = RayDirection(input.uv);

    const float voxel = max(VolumeParams.x, 1e-4);
    float2 range = RayBoxRange(rayOrigin, rayDirection, VolumeMin.xyz, VolumeMax.xyz);
    range.x = max(range.x, 0.0);

    float sceneDepth = SceneDepth.Load(int3(input.position.xy, 0));
    float3 scenePosition = WorldFromUV(input.uv, sceneDepth);
    float sceneDistance = length(scenePosition - rayOrigin);
    // A half-voxel bias avoids a hard cut where the cloud meets the ground.
    range.y = min(range.y, sceneDistance + voxel * 0.5);

    if (range.y <= range.x)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Fixed world-space step: the step size no longer grows with the ray length
    // at grazing angles, which used to make dense layers disappear.
    const int maxSteps = (int)max(VolumeMax.w, 8.0);
    const float distance = range.y - range.x;
    const int steps = min(maxSteps, max(1, int(ceil(distance / voxel))));
    const float stepSize = distance / float(steps);
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

    // The directional shading normal is computed once at the first dense sample
    // with an isotropic 27-point gradient. The shadow itself is sampled from the
    // precomputed light volume at every step, so it no longer depends on the
    // camera.
    bool shadingReady = false;
    float3 shadingNormal = float3(0.0, 1.0, 0.0);
    float firstCloudShadow = 1.0;

    int i = 0;
    [loop]
    while (i < steps)
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
            if (!shadingReady)
            {
                const float gradEps = 0.006;
                float3 gradient = float3(0.0, 0.0, 0.0);
                [unroll]
                for (int oz = -1; oz <= 1; ++oz)
                {
                    [unroll]
                    for (int oy = -1; oy <= 1; ++oy)
                    {
                        [unroll]
                        for (int ox = -1; ox <= 1; ++ox)
                        {
                            const float weight = (ox == 0 ? 2.0 : 1.0) *
                                                 (oy == 0 ? 2.0 : 1.0) *
                                                 (oz == 0 ? 2.0 : 1.0);
                            const float d =
                                SampleDensity(uvw + float3(ox, oy, oz) * gradEps);
                            gradient += float3(ox, oy, oz) * weight * d;
                        }
                    }
                }
                const float gradientLength = length(gradient);
                shadingNormal = gradientLength > 1e-4 ? -gradient / gradientLength
                                                      : float3(0.0, 1.0, 0.0);
                // The light volume provides the shadow in the solver path; the
                // CPU-baked volume still needs the marched fallback.
                if (VolumeParams2.w < 0.5)
                {
                    firstCloudShadow = SunTransmittance(position);
                }
                shadingReady = true;
            }
            float3 normal = shadingNormal;
            float sunShade = saturate(dot(normal, sunDirection));
            float skyShade = 0.5 + 0.5 * normal.y;
            float shade = 0.45 + 0.85 * sunShade + 0.30 * skyShade;

            float cloudShadow = firstCloudShadow;
            if (VolumeParams2.w > 0.5)
            {
                cloudShadow = SampleLight(position);
            }
            // Multiple-scattering approximation: shadowed smoke still receives
            // some diffuse light instead of turning into a black hole.
            float shadow = lerp(0.35, 1.0, pow(saturate(cloudShadow), 0.5));
            scattering *= shade * shadow;
            scattering += albedo * density * ambientLight * 0.6;
            emission *= lerp(1.0, shade, 0.5);

            // Fade the first few metres so flying through the cloud does not
            // smear a screen-filling red veil over the camera.
            const float nearFade = smoothstep(0.0, 3.0, t);
            accum += transmittance * (emission + scattering) * stepSize * nearFade;
            transmittance *= exp(-density * extinctionScale * stepSize);

            if (transmittance < 0.02)
            {
                break;
            }
        }

        // Conservative empty-space skipping: double the step while the sample is
        // empty. The render volume is smoothed, so dense layers span several
        // voxels and are not lost by the 2x skip.
        if (density <= 0.002)
        {
            t += stepSize * 2.0;
            i += 2;
        }
        else
        {
            t += stepSize;
            ++i;
        }
    }

    float alpha = 1.0 - transmittance;
    return float4(accum, alpha);
}

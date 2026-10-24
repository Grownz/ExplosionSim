#include "common.hlsli"

Texture3D<float4> VolumeTexture : register(t0);
Texture2D<float> SceneDepth : register(t1);
SamplerState LinearSampler : register(s0);

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
    float3 sunLight = SunColor.rgb * phase * 1.5;
    float3 ambientLight = float3(0.20, 0.21, 0.24);
    float3 windDrift = float3(0.0, -Misc3.x * 0.06, 0.0);

    float3 accum = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        float3 position = rayOrigin + rayDirection * t;
        float3 uvw = (position - VolumeMin.xyz) / max(boxSize, 1e-4);

        float4 sample = VolumeTexture.SampleLevel(LinearSampler, uvw, 0);
        float density = sample.r;

        if (density > 0.002)
        {
            float temperature = sample.g * 12000.0;
            float soot = sample.a;
            float dust = sample.b;

            // Billowing structure: modulate the density with a rising 3D noise
            // field. The frequency adapts to the fireball size and the vertical
            // stretch creates flame-like tongues.
            float detailScale = 10.0 / max(Misc2.z, 0.25);
            float3 noisePosition = position * detailScale + windDrift;
            noisePosition.y *= 0.75;
            float structure = Fbm3(noisePosition);
            float fine = ValueNoise3(noisePosition * 3.0 + 11.0);
            density *= lerp(0.05, 2.40, structure) * (0.60 + 0.70 * fine);
            temperature *= lerp(0.55, 1.30, structure);

            float heat = smoothstep(550.0, 1250.0, temperature);
            float3 blackbody = BlackbodyRGB(temperature);
            float coreBoost = 0.75 + 0.90 * smoothstep(2400.0, 4300.0, temperature);
            float3 emission = blackbody * density * heat * coreBoost * 1.10 * VolumeParams.x;

            // Cold soot absorbs the fireball light; hot soot glows.
            float coldSoot = soot * saturate((2400.0 - temperature) / 1500.0);
            emission *= lerp(1.0, 0.10, coldSoot);

            float3 albedo = lerp(float3(0.05, 0.045, 0.04), float3(0.22, 0.18, 0.13), dust);
            float3 scattering = albedo * density * (sunLight + ambientLight);

            // Fireball light scattered by dust and smoke: warm near the blast,
            // dark farther out.
            float3 toFireball = Fireball.xyz - position;
            float fireDistanceSq = dot(toFireball, toFireball);
            float fireRadius = max(Misc2.z, 0.2);
            float fireLight = Fireball.w * 1.8 * fireRadius * fireRadius /
                              (fireDistanceSq + fireRadius * fireRadius * 1.2);
            scattering += albedo * density * fireLight * float3(1.0, 0.42, 0.12);

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

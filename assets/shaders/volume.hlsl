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

            // Billowing structure: modulate the density with a rising 3D noise
            // field. The frequency adapts to the fireball size so small charges
            // show the same amount of detail as large ones.
            float detailScale = 2.0 / max(Misc2.z, 0.25);
            float structure = Fbm3(position * detailScale + windDrift);
            density *= lerp(0.30, 1.75, structure);
            temperature *= lerp(0.80, 1.12, structure);

            float heat = smoothstep(600.0, 1500.0, temperature);
            float3 blackbody = BlackbodyRGB(temperature);
            float coreBoost = 0.65 + 1.7 * smoothstep(2200.0, 3800.0, temperature);
            float3 emission = blackbody * density * heat * coreBoost * 0.55;

            float3 albedo = lerp(float3(0.26, 0.24, 0.22), float3(0.55, 0.47, 0.36), sample.b);
            float3 scattering = albedo * density * (sunLight + ambientLight);

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

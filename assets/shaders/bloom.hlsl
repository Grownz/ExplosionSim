#include "common.hlsli"

Texture2D<float4> SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

// 4-tap box downsample with an optional HDR threshold.
float4 PSDownsample(VSOut input) : SV_Target
{
    float2 texel = TexelSize.xy;
    float3 color = SourceTexture.SampleLevel(LinearSampler, input.uv + float2(-texel.x, -texel.y), 0).rgb;
    color += SourceTexture.SampleLevel(LinearSampler, input.uv + float2(texel.x, -texel.y), 0).rgb;
    color += SourceTexture.SampleLevel(LinearSampler, input.uv + float2(-texel.x, texel.y), 0).rgb;
    color += SourceTexture.SampleLevel(LinearSampler, input.uv + float2(texel.x, texel.y), 0).rgb;
    color *= 0.25;

    color = max(color - BloomParams.x, 0.0);
    return float4(color, 1.0);
}

// 9-tap tent upsample, accumulated additively into the next larger level.
float4 PSUpsample(VSOut input) : SV_Target
{
    float2 texel = TexelSize.xy;
    float3 color = float3(0.0, 0.0, 0.0);

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float weight = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            color += SourceTexture.SampleLevel(LinearSampler,
                                               input.uv + float2(x, y) * texel, 0).rgb * weight;
        }
    }

    return float4(color / 16.0 * BloomParams.y, 1.0);
}

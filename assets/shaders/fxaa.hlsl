#include "aa_common.hlsli"

// FXAA 3.11 quality preset (Timothy Lottes, public domain), reduced to the
// single-pass variant that fits this renderer: the composite has already
// tone-mapped and sRGB-encoded the frame, so the filter works on the final
// 8-bit image.

EX_VK_BINDING(8) Texture2D<float4> ColorTexture : register(t0);
EX_VK_BINDING(16) SamplerState LinearSampler : register(s0);

static const float kEdgeThreshold = 1.0 / 8.0;
static const float kEdgeThresholdMin = 1.0 / 32.0;
static const float kSubpixQuality = 1.0 / 4.0;
static const float kSearchSteps = 8.0;

float Luma(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

float LumaAt(float2 uv)
{
    return Luma(ColorTexture.SampleLevel(LinearSampler, uv, 0).rgb);
}

float4 PSFXAA(AaVSOut input) : SV_Target
{
    const float2 texel = AaScreenParams.xy;
    const float3 center = ColorTexture.SampleLevel(LinearSampler, input.uv, 0).rgb;
    const float lumaCenter = Luma(center);

    const float lumaNW = LumaAt(input.uv + float2(-1.0, -1.0) * texel);
    const float lumaNE = LumaAt(input.uv + float2(1.0, -1.0) * texel);
    const float lumaSW = LumaAt(input.uv + float2(-1.0, 1.0) * texel);
    const float lumaSE = LumaAt(input.uv + float2(1.0, 1.0) * texel);

    const float lumaMin =
        min(lumaCenter, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax =
        max(lumaCenter, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    // Skip flat areas: the local contrast has to exceed both an absolute and a
    // relative threshold.
    if ((lumaMax - lumaMin) <
        max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
        return float4(center, 1.0);
    }

    // Choose the blur direction from the luma gradient.
    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    const float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) *
                                    (0.25 * kSubpixQuality),
                                1.0 / 128.0);
    const float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -kSearchSteps, kSearchSteps) * texel;

    const float3 rgbA =
        0.5 * (ColorTexture.SampleLevel(LinearSampler,
                                        input.uv + dir * (1.0 / 3.0 - 0.5), 0)
                       .rgb +
               ColorTexture.SampleLevel(LinearSampler,
                                        input.uv + dir * (2.0 / 3.0 - 0.5), 0)
                       .rgb);
    const float3 rgbB =
        rgbA * 0.5 +
        0.25 * (ColorTexture.SampleLevel(LinearSampler,
                                         input.uv + dir * -0.5, 0)
                    .rgb +
                ColorTexture.SampleLevel(LinearSampler, input.uv + dir * 0.5,
                                         0)
                    .rgb);

    // The wider blur may overshoot the local luma range; fall back to the
    // narrower one when it does.
    const float lumaB = Luma(rgbB);
    if (lumaB < lumaMin || lumaB > lumaMax) {
        return float4(rgbA, 1.0);
    }
    return float4(rgbB, 1.0);
}

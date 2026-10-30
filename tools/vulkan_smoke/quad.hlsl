struct VSInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> tex : register(t0);
[[vk::binding(1, 0)]] SamplerState samp : register(s0);
[[vk::binding(2, 0)]] cbuffer Tint : register(b0) {
    float4 tint;
};

float4 PSMain(VSOutput input) : SV_Target {
    return tex.Sample(samp, input.uv) * tint;
}

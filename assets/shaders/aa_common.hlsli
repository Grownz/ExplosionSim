#ifndef EXPLOSION_AA_COMMON_HLSLI
#define EXPLOSION_AA_COMMON_HLSLI

// Shared by the anti-aliasing post-process shaders (FXAA and SMAA 1x).
// Keeping the constant buffer local to the AA passes means they do not depend
// on the frame or bloom layout, and each pass owns exactly the descriptors it
// declares.

#if defined(EXPLOSION_VULKAN)
#define EX_VK_BINDING(n) [[vk::binding(n, 0)]]
#else
#define EX_VK_BINDING(n)
#endif

// b1 is free in the AA passes: the frame constants live in b0 and the bloom
// constants in b1 only for the render passes that need them.
EX_VK_BINDING(1) cbuffer AaConstants : register(b1)
{
    // xy = 1 / target size, zw = target size in pixels.
    float4 AaScreenParams;
};

struct AaVSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

AaVSOut FullscreenVS(uint vertexID : SV_VertexID)
{
    AaVSOut output;
    const float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

#endif // EXPLOSION_AA_COMMON_HLSLI

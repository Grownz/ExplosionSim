#ifndef EXPLOSION_COMMON_HLSLI
#define EXPLOSION_COMMON_HLSLI

// Vulkan uses explicit descriptor bindings because D3D register spaces do not
// map one-to-one onto Vulkan descriptor sets. D3D11 compiles the macro away.
// Binding map: b0..b3 -> 0..3, t0..t7 -> 8..15, s0..s3 -> 16..19.
#if defined(EXPLOSION_VULKAN)
#define EX_VK_BINDING(n) [[vk::binding(n, 0)]]
#else
#define EX_VK_BINDING(n)
#endif

static const float PI = 3.14159265359;

// Shared frame constants. The layout must match FrameConstants in
// src/gfx/Renderer.cpp exactly.
EX_VK_BINDING(0) cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4 CameraPosition;   // xyz = position, w = simulation time
    float4 SunDirection;     // xyz = direction to the sun, w = exposure
    float4 SunColor;         // rgb = sun radiance, w = bloom strength
    float4 VolumeMin;        // xyz = box minimum, w = extinction scale
    float4 VolumeMax;        // xyz = box maximum, w = raymarch steps
    float4 ScreenParams;     // xy = size, zw = 1 / size
    float4 Misc;             // x = scorch radius, y = 5 psi radius, z = dust ring, w = ambient K
    float4 Misc2;            // x = cloud radius, y = cloud height, z = fireball radius, w = fireball K
    float4 Misc3;            // x = time, y = dt, z = frame index, w = shock radius
    float4 Wind;             // xyz = wind velocity, w = speed
    float4 CameraRight;      // xyz = camera right vector
    float4 CameraUp;         // xyz = camera up vector
    float4 GridParams;       // x = spacing, y = thickness, z = intensity, w = horizon fade
    float4 RangeParams;      // x = near, y = far, z = volume enabled, w = particle scale
    float4 Fireball;         // xyz = centre, w = emitted light intensity
    float4 VolumeParams;     // x = voxel size, y = split volume, z/w = detail/structure
    float4 VolumeParams2;    // x = scattering gain, y = emission gain, z = fireball light, w = use light volume
    float4 LightVolumeMin;   // xyz = solver-frame light volume minimum
    float4 LightVolumeMax;   // xyz = solver-frame light volume maximum
    float4 Blast;            // xyz = burst centre, w = shock Mach number
};

EX_VK_BINDING(1) cbuffer BloomConstants : register(b1)
{
    float4 TexelSize;        // xy = source texel, zw = target texel
    float4 BloomParams;      // x = threshold, y = intensity, z = exposure, w = bloom strength
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut FullscreenVS(uint vertexID : SV_VertexID)
{
    VSOut output;
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

float3 WorldFromUV(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(InvViewProj, float4(ndc, depth, 1.0));
    return world.xyz / world.w;
}

float3 RayDirection(float2 uv)
{
    return normalize(WorldFromUV(uv, 1.0) - CameraPosition.xyz);
}

float2 RayBoxRange(float3 origin, float3 direction, float3 boxMin, float3 boxMax)
{
    float3 safeDirection = direction + step(abs(direction), 1e-6) * 1e-6 * sign(direction + 1e-9);
    float3 invDirection = 1.0 / safeDirection;
    float3 t0 = (boxMin - origin) * invDirection;
    float3 t1 = (boxMax - origin) * invDirection;
    float3 tMin = min(t0, t1);
    float3 tMax = max(t0, t1);
    return float2(max(max(tMin.x, tMin.y), tMin.z), min(min(tMax.x, tMax.y), tMax.z));
}

float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float Hash13(float3 p3)
{
    p3 = frac(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return frac((p3.x + p3.y) * p3.z);
}

float ValueNoise3(float3 p)
{
    float3 cell = floor(p);
    float3 local = frac(p);
    local = local * local * (3.0 - 2.0 * local);

    float n000 = Hash13(cell + float3(0, 0, 0));
    float n100 = Hash13(cell + float3(1, 0, 0));
    float n010 = Hash13(cell + float3(0, 1, 0));
    float n110 = Hash13(cell + float3(1, 1, 0));
    float n001 = Hash13(cell + float3(0, 0, 1));
    float n101 = Hash13(cell + float3(1, 0, 1));
    float n011 = Hash13(cell + float3(0, 1, 1));
    float n111 = Hash13(cell + float3(1, 1, 1));

    float nx00 = lerp(n000, n100, local.x);
    float nx10 = lerp(n010, n110, local.x);
    float nx01 = lerp(n001, n101, local.x);
    float nx11 = lerp(n011, n111, local.x);
    float nxy0 = lerp(nx00, nx10, local.y);
    float nxy1 = lerp(nx01, nx11, local.y);
    return lerp(nxy0, nxy1, local.z);
}

float Fbm3(float3 p)
{
    float value = 0.0;
    float amplitude = 0.55;
    for (int i = 0; i < 4; ++i)
    {
        value += amplitude * ValueNoise3(p);
        p = p * 2.03 + float3(13.1, 7.7, 3.3);
        amplitude *= 0.5;
    }
    return value;
}

float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denominator = pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
    return (1.0 - g2) / (4.0 * PI * denominator);
}

// Planckian locus approximation (Kim et al. 2002), converted to linear sRGB
// and normalised so the brightest channel is 1.
float3 BlackbodyRGB(float kelvin)
{
    float t = clamp(kelvin, 1000.0, 40000.0);
    float x;
    if (t < 4000.0)
    {
        x = -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) + 0.8776956e3 / t + 0.179910;
    }
    else
    {
        x = -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) + 0.2226347e3 / t + 0.240390;
    }

    float y;
    if (t < 2222.0)
    {
        y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    }
    else if (t < 4000.0)
    {
        y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    }
    else
    {
        y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    }

    y = max(y, 1e-3);
    float3 xyz = float3(x / y, 1.0, (1.0 - x - y) / y);
    const float3x3 XYZ_TO_RGB = float3x3(
        3.2406, -1.5372, -0.4986,
        -0.9689, 1.8758, 0.0415,
        0.0557, -0.2040, 1.0570);

    float3 rgb = max(mul(XYZ_TO_RGB, xyz), 0.0);
    return rgb / max(max(rgb.r, max(rgb.g, rgb.b)), 1e-3);
}

float3 SkyColor(float3 direction)
{
    float height = max(direction.y, 0.0);
    float3 zenith = float3(0.020, 0.055, 0.135);
    float3 horizon = float3(0.52, 0.47, 0.42);
    float3 color = lerp(horizon, zenith, pow(height, 0.52));

    float sunDot = saturate(dot(direction, SunDirection.xyz));
    color += SunColor.rgb * pow(sunDot, 24.0) * 0.12;
    color += SunColor.rgb * smoothstep(0.9995, 0.99995, sunDot) * 0.9;

    // Thin, high altostratus deck.
    if (height > 0.02)
    {
        float2 cloudUV = direction.xz / max(height, 0.02) * 1.5 + Wind.xy * Misc3.x * 0.002;
        float clouds = smoothstep(0.55, 0.85, Fbm3(float3(cloudUV, 3.1)));
        clouds *= smoothstep(0.02, 0.25, height) * 0.55;
        color = lerp(color, float3(0.62, 0.58, 0.56), clouds);
    }

    // Ground haze near the horizon.
    color = lerp(color, float3(0.30, 0.24, 0.19), smoothstep(0.12, -0.02, direction.y) * 0.75);
    return color;
}

#endif // EXPLOSION_COMMON_HLSLI

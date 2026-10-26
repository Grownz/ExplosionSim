#include "euler_common.hlsli"

// Reduce output layout (one structured buffer, four uints):
//   [0] = max signal speed        (float bits)
//   [1] = max product height      (fixed point, 1/512 m)
//   [2] = max product radius      (fixed point, 1/512 m)
//   [3] = max normalized dome fill (fixed point, 1/512)
RWStructuredBuffer<uint> ReduceOutput : register(u2);

groupshared uint gMaxSpeed;
groupshared uint gMaxHeight;
groupshared uint gMaxRadius;
groupshared uint gMaxDome;

static const float kBoundScale = 512.0;
static const float kHeightThreshold = 0.004;
static const float kSmokeThreshold = 0.0005;
static const float kFireballThreshold = 0.02;
// The radial trigger must not react to the low, dilute base surge; above this
// fraction of the domain height every visible smoke cell counts. Below it only
// dense fireball products count, so the fireball's equator is still measured.
static const float kRadiusHeightFraction = 0.25;
// Ignore the ground layer that the render fades out (see euler_render.hlsl).
static const float kLowLayerFraction = 0.02;
static const float kGroundFadeVoxels = 2.4;

uint Quantize(float value)
{
    return (uint)clamp(value * kBoundScale, 0.0, 1.0e9);
}

// Reduces the maximum signal speed (|v| + c) plus the visible product bounds.
// The bounds feed the auto-scale trigger so it reacts to the plume the solver
// actually produced instead of the analytic estimate.
[numthreads(8, 8, 8)]
void CSReduce(uint3 id : SV_DispatchThreadID, uint3 groupThread : SV_GroupThreadID)
{
    uint localIndex = groupThread.x + groupThread.y * 8 + groupThread.z * 64;
    if (localIndex == 0)
    {
        gMaxSpeed = 0;
        gMaxHeight = 0;
        gMaxRadius = 0;
        gMaxDome = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    float speed = 0.0;
    uint height = 0;
    uint radius = 0;
    uint dome = 0;

    const float domainHeight = max(DomainMax.y - DomainMin.y, 1e-3);
    const float lowFloor = max(kGroundFadeVoxels * DomainMin.w, kLowLayerFraction * domainHeight);
    const float upperFloor = kRadiusHeightFraction * domainHeight;

    if (all(id < (uint3)Grid.xyz) && IsActiveCell(int3(id)))
    {
        float4 a = StateA.Load(int4(id, 0));
        float4 b = StateB.Load(int4(id, 0));
        float rho = max(a.x, 1e-6);
        float3 velocity = a.yzw / rho;
        float pressure =
            max((Ambient.w - 1.0) * (b.x - 0.5 * rho * dot(velocity, velocity)), 1e-3);
        float soundSpeed = sqrt(Ambient.w * pressure / rho);
        speed = length(velocity) + soundSpeed;

        const float3 world = DomainMin.xyz + (float3(id) + 0.5) * DomainMin.w;
        if (b.y > kHeightThreshold)
        {
            height = Quantize(world.y);
        }

        // Horizontal extent: the dilute smoke cap counts above the base surge;
        // below it only the dense fireball counts. Both are visible in the
        // render, so letting either reach the wall must grow the domain.
        if (b.y > kSmokeThreshold && world.y > lowFloor)
        {
            const bool aboveBaseSurge = world.y > upperFloor;
            if (aboveBaseSurge || b.y > kFireballThreshold)
            {
                radius = Quantize(max(abs(world.x), abs(world.z)));
                if (IsHemispherical())
                {
                    dome = Quantize(DomainFraction(world));
                }
            }
        }
    }

    InterlockedMax(gMaxSpeed, asuint(speed));
    InterlockedMax(gMaxHeight, height);
    InterlockedMax(gMaxRadius, radius);
    InterlockedMax(gMaxDome, dome);
    GroupMemoryBarrierWithGroupSync();

    if (localIndex == 0)
    {
        InterlockedMax(ReduceOutput[0], gMaxSpeed);
        InterlockedMax(ReduceOutput[1], gMaxHeight);
        InterlockedMax(ReduceOutput[2], gMaxRadius);
        InterlockedMax(ReduceOutput[3], gMaxDome);
    }
}

#include "euler_common.hlsli"

// Reduce output layout (one structured buffer, four uints):
//   [0] = max signal speed        (float bits)
//   [1] = max product height      (fixed point, 1/512 m)
//   [2] = max product radius      (fixed point, 1/512 m)
//   [3] = reserved
RWStructuredBuffer<uint> ReduceOutput : register(u2);

groupshared uint gMaxSpeed;
groupshared uint gMaxHeight;
groupshared uint gMaxRadius;

static const float kBoundScale = 512.0;
static const float kProductThreshold = 0.004;

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
    }
    GroupMemoryBarrierWithGroupSync();

    float speed = 0.0;
    uint height = 0;
    uint radius = 0;

    if (all(id < (uint3)Grid.xyz))
    {
        float4 a = StateA.Load(int4(id, 0));
        float4 b = StateB.Load(int4(id, 0));
        float rho = max(a.x, 1e-6);
        float3 velocity = a.yzw / rho;
        float pressure =
            max((Ambient.w - 1.0) * (b.x - 0.5 * rho * dot(velocity, velocity)), 1e-3);
        float soundSpeed = sqrt(Ambient.w * pressure / rho);
        speed = length(velocity) + soundSpeed;

        if (b.y > kProductThreshold)
        {
            float3 world = DomainMin.xyz + (float3(id) + 0.5) * DomainMin.w;
            height = Quantize(world.y);
            // Ignore the thin ground layer when measuring the radius: it is
            // faded out in the renderer and would otherwise dominate.
            if (world.y > 1.5 * DomainMin.w)
            {
                radius = Quantize(max(abs(world.x), abs(world.z)));
            }
        }
    }

    InterlockedMax(gMaxSpeed, asuint(speed));
    InterlockedMax(gMaxHeight, height);
    InterlockedMax(gMaxRadius, radius);
    GroupMemoryBarrierWithGroupSync();

    if (localIndex == 0)
    {
        InterlockedMax(ReduceOutput[0], gMaxSpeed);
        InterlockedMax(ReduceOutput[1], gMaxHeight);
        InterlockedMax(ReduceOutput[2], gMaxRadius);
    }
}

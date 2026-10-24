#include "euler_common.hlsli"

RWStructuredBuffer<uint> MaxSpeed : register(u2);

groupshared uint gMaxSpeed;

// Reduces the maximum signal speed (|v| + c) over the domain. The result is read
// back one frame later to choose the next CFL time step.
[numthreads(8, 8, 8)]
void CSReduce(uint3 id : SV_DispatchThreadID, uint3 groupThread : SV_GroupThreadID)
{
    uint localIndex = groupThread.x + groupThread.y * 8 + groupThread.z * 64;
    if (localIndex == 0)
    {
        gMaxSpeed = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    float speed = 0.0;
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
    }

    InterlockedMax(gMaxSpeed, asuint(speed));
    GroupMemoryBarrierWithGroupSync();

    if (localIndex == 0)
    {
        InterlockedMax(MaxSpeed[0], gMaxSpeed);
    }
}

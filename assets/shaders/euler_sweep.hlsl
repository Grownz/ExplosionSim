#include "euler_common.hlsli"

// One directional finite-volume sweep with MUSCL reconstruction and HLLC fluxes.
[numthreads(8, 8, 8)]
void CSSweep(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= (uint3)Grid.xyz))
    {
        return;
    }

    const int dir = Grid.w;
    const int3 offset = dir == 0 ? int3(1, 0, 0) : (dir == 1 ? int3(0, 1, 0) : int3(0, 0, 1));
    const int3 cell = int3(id);

    if (!IsActiveCell(cell))
    {
        OutA[id] = float4(Ambient.x, 0.0, 0.0, 0.0);
        OutB[id] = float4(Ambient.y / (Ambient.w - 1.0), 0.0, 0.0, 1.0);
        return;
    }

    Primitive wm2 = LoadPrimitive(cell - 2 * offset);
    Primitive wm1 = LoadPrimitive(cell - offset);
    Primitive w0 = LoadPrimitive(cell);
    Primitive wp1 = LoadPrimitive(cell + offset);
    Primitive wp2 = LoadPrimitive(cell + 2 * offset);

    // Faces i-1/2 and i+1/2.
    Primitive leftMinus = Reconstruct(wm2, wm1, w0, 1.0, wm1);
    Primitive rightMinus = Reconstruct(wm1, w0, wp1, -1.0, w0);
    Primitive leftPlus = Reconstruct(wm1, w0, wp1, 1.0, w0);
    Primitive rightPlus = Reconstruct(w0, wp1, wp2, -1.0, wp1);

    float4 fluxMinusA;
    float4 fluxPlusA;
    float2 fluxMinusB;
    float2 fluxPlusB;

    HllcFlux(leftMinus, rightMinus, dir, fluxMinusA, fluxMinusB);
    HllcFlux(leftPlus, rightPlus, dir, fluxPlusA, fluxPlusB);

    float4 a = StateA.Load(int4(cell, 0));
    float2 b = StateB.Load(int4(cell, 0)).xy;

    const float scale = DomainMax.w / DomainMin.w;
    a -= scale * (fluxPlusA - fluxMinusA);
    b -= scale * (fluxPlusB - fluxMinusB);

    Sanitise(a, b);

    OutA[id] = a;
    OutB[id] = float4(b, 0.0, 1.0);
}

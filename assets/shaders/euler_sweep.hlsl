#include "euler_common.hlsli"

// One directional finite-volume sweep with MUSCL reconstruction and HLLC fluxes.
// The Z sweep also applies the source terms, so a step is three dispatches
// instead of four and the state never round-trips through memory between the Z
// update and the source terms. X/Y and Z use separate entry points: the fused
// source code raises the temp-register count, and compiling it into the X/Y
// sweep would reduce occupancy for all three passes.
void SweepBody(uint3 id, bool applySource)
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
        OutB[id] = float2(Ambient.y / (Ambient.w - 1.0), 0.0);
        return;
    }

    // The centre cell is needed both as a primitive (reconstruction) and as the
    // conserved state (update), so load and decode it once.
    CellState center = LoadCell(cell);
    Primitive w0 = center.prim;
    Primitive wm2 = LoadPrimitive(cell - 2 * offset);
    Primitive wm1 = LoadPrimitive(cell - offset);
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

    float4 a = center.a;
    float2 b = center.b;

    const float scale = DomainMax.w / DomainMin.w;
    a -= scale * (fluxPlusA - fluxMinusA);
    b -= scale * (fluxPlusB - fluxMinusB);

    Sanitise(a, b);

    // The source terms run after the Z update, exactly as the former separate
    // pass did (X, Y, Z, Source).
    if (applySource)
    {
        ApplySource(a, b, cell);
    }

    OutA[id] = a;
    OutB[id] = b;
}

[numthreads(8, 8, 8)]
void CSSweep(uint3 id : SV_DispatchThreadID)
{
    SweepBody(id, false);
}

[numthreads(8, 8, 8)]
void CSSweepZ(uint3 id : SV_DispatchThreadID)
{
    SweepBody(id, true);
}

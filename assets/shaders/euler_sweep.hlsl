#include "euler_common.hlsli"

// One directional finite-volume sweep with MUSCL reconstruction and HLLC fluxes.
// The Z sweep also applies the source terms, so a step is three dispatches
// instead of four and the state never round-trips through memory between the Z
// update and the source terms.
//
// Each direction has its own entry point. `dir` is then a compile-time constant,
// which folds the direction selects in HLLC and keeps the X/Y shaders free of the
// fused source code (the source raises the temp-register count and would reduce
// occupancy for all three passes).
void SweepBody(uint3 id, int dir, bool applySource)
{
    if (any(id >= (uint3)Grid.xyz))
    {
        return;
    }

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

    // Faces i-1/2 and i+1/2. The middle stencil (wm1, w0, wp1) feeds both
    // faces, so its MUSCL slope is computed once instead of twice.
    Primitive leftMinus = Reconstruct(wm2, wm1, w0, 1.0, wm1);
    Primitive rightPlus = Reconstruct(w0, wp1, wp2, -1.0, wp1);
    Primitive midSlope = Slope(wm1, w0, wp1);
    Primitive rightMinus = ApplySlope(w0, midSlope, -1.0, w0);
    Primitive leftPlus = ApplySlope(w0, midSlope, 1.0, w0);

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

[numthreads(EULER_GROUP_X, EULER_GROUP_Y, EULER_GROUP_Z)]
void CSSweepX(uint3 id : SV_DispatchThreadID)
{
    SweepBody(id, 0, false);
}

[numthreads(EULER_GROUP_X, EULER_GROUP_Y, EULER_GROUP_Z)]
void CSSweepY(uint3 id : SV_DispatchThreadID)
{
    SweepBody(id, 1, false);
}

[numthreads(EULER_GROUP_X, EULER_GROUP_Y, EULER_GROUP_Z)]
void CSSweepZ(uint3 id : SV_DispatchThreadID)
{
    SweepBody(id, 2, true);
}

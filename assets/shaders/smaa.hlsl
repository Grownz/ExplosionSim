#include "aa_common.hlsli"

// SMAA 1x (Enhanced Subpixel Morphological Anti-Aliasing).
//
// Ported from the reference implementation by Jorge Jimenez et al.,
// https://github.com/iryoku/smaa, MIT licence. Only the SMAA 1x path is kept:
// luma edge detection, blending-weight calculation (including diagonal and
// corner handling) and neighborhood blending. The area and search lookup
// tables are the reference SMAA textures.

EX_VK_BINDING(8) Texture2D<float4> ColorTexture : register(t0);
EX_VK_BINDING(9) Texture2D<float4> SecondTexture : register(t1);
EX_VK_BINDING(10) Texture2D<float4> ThirdTexture : register(t2);
EX_VK_BINDING(16) SamplerState LinearSampler : register(s0);
EX_VK_BINDING(17) SamplerState PointSampler : register(s1);

#define SMAA_RT_METRICS AaScreenParams

#define SMAA_THRESHOLD 0.1
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0
#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_MAX_DISTANCE_DIAG 20
#define SMAA_AREATEX_PIXEL_SIZE (1.0 / float2(160.0, 560.0))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)
#define SMAA_SEARCHTEX_SIZE float2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0, 16.0)
#define SMAA_CORNER_ROUNDING_NORM (float(SMAA_CORNER_ROUNDING) / 100.0)
#define SMAA_AREATEX_SELECT(sample) sample.rg
#define SMAA_SEARCHTEX_SELECT(sample) sample.r

#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) \
    tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)

//-----------------------------------------------------------------------------
// Conditional move and diagonal decoding helpers.

void SMAAMovc(bool2 cond, inout float2 variable, float2 value)
{
    if (cond.x)
    {
        variable.x = value.x;
    }
    if (cond.y)
    {
        variable.y = value.y;
    }
}

void SMAAMovc(bool4 cond, inout float4 variable, float4 value)
{
    SMAAMovc(cond.xy, variable.xy, value.xy);
    SMAAMovc(cond.zw, variable.zw, value.zw);
}

// Decodes two binary values from a bilinear-filtered access.
float2 SMAADecodeDiagBilinearAccess(float2 e)
{
    e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);
    return round(e);
}

float4 SMAADecodeDiagBilinearAccess(float4 e)
{
    e.rb = e.rb * abs(5.0 * e.rb - 5.0 * 0.75);
    return round(e);
}

//-----------------------------------------------------------------------------
// Edge detection (first pass).

float2 PSAASmaaEdge(AaVSOut input) : SV_Target
{
    const float4 offsetTopLeft =
        float4(-1.0, 0.0, 0.0, -1.0) * SMAA_RT_METRICS.xyxy +
        input.uv.xyxy;
    const float4 offsetBottomRight =
        float4(1.0, 0.0, 0.0, 1.0) * SMAA_RT_METRICS.xyxy + input.uv.xyxy;
    const float4 offsetFar =
        float4(-2.0, 0.0, 0.0, -2.0) * SMAA_RT_METRICS.xyxy + input.uv.xyxy;

    const float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);

    const float3 weights = float3(0.2126, 0.7152, 0.0722);
    const float L = dot(SMAASamplePoint(ColorTexture, input.uv).rgb, weights);
    const float Lleft =
        dot(SMAASamplePoint(ColorTexture, offsetTopLeft.xy).rgb, weights);
    const float Ltop =
        dot(SMAASamplePoint(ColorTexture, offsetTopLeft.zw).rgb, weights);

    float4 delta;
    delta.xy = abs(L - float2(Lleft, Ltop));
    float2 edges = step(threshold, delta.xy);

    if (dot(edges, float2(1.0, 1.0)) == 0.0)
    {
        discard;
    }

    const float Lright =
        dot(SMAASamplePoint(ColorTexture, offsetBottomRight.xy).rgb, weights);
    const float Lbottom =
        dot(SMAASamplePoint(ColorTexture, offsetBottomRight.zw).rgb, weights);
    delta.zw = abs(L - float2(Lright, Lbottom));

    float2 maxDelta = max(delta.xy, delta.zw);

    const float Lleftleft =
        dot(SMAASamplePoint(ColorTexture, offsetFar.xy).rgb, weights);
    const float Ltoptop =
        dot(SMAASamplePoint(ColorTexture, offsetFar.zw).rgb, weights);
    delta.zw = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));

    maxDelta = max(maxDelta.xy, delta.zw);
    const float finalDelta = max(maxDelta.x, maxDelta.y);

    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    return edges;
}

//-----------------------------------------------------------------------------
// Diagonal search functions.

float2 SMAASearchDiag1(Texture2D<float4> edgesTex, float2 texcoord, float2 dir,
                       out float2 e)
{
    float4 coord = float4(texcoord, -1.0, 1.0);
    const float3 t = float3(SMAA_RT_METRICS.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)
    {
        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);
        e = SMAASampleLevelZero(edgesTex, coord.xy).rg;
        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

float2 SMAASearchDiag2(Texture2D<float4> edgesTex, float2 texcoord, float2 dir,
                       out float2 e)
{
    float4 coord = float4(texcoord, -1.0, 1.0);
    coord.x += 0.25 * SMAA_RT_METRICS.x;
    const float3 t = float3(SMAA_RT_METRICS.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)
    {
        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);
        e = SMAASampleLevelZero(edgesTex, coord.xy).rg;
        e = SMAADecodeDiagBilinearAccess(e);
        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

float2 SMAAAreaDiag(Texture2D<float4> areaTex, float2 dist, float2 e,
                    float offset)
{
    float2 texcoord =
        mad(float2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG),
            e, dist);
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    texcoord.x += 0.5;
    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return SMAA_AREATEX_SELECT(SMAASampleLevelZero(areaTex, texcoord));
}

float2 SMAACalculateDiagWeights(Texture2D<float4> edgesTex,
                                Texture2D<float4> areaTex, float2 texcoord,
                                float2 e, float4 subsampleIndices)
{
    float2 weights = float2(0.0, 0.0);

    float4 d;
    float2 end;
    if (e.r > 0.0)
    {
        d.xz = SMAASearchDiag1(edgesTex, texcoord, float2(-1.0, 1.0), end);
        d.x += float(end.y > 0.9);
    }
    else
    {
        d.xz = float2(0.0, 0.0);
    }
    d.yw = SMAASearchDiag1(edgesTex, texcoord, float2(1.0, -1.0), end);

    if (d.x + d.y > 2.0)
    {
        const float4 coords =
            mad(float4(-d.x + 0.25, d.x, d.y, -d.y - 0.25),
                SMAA_RT_METRICS.xyxy, texcoord.xyxy);
        float4 c;
        c.xy = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1, 0)).rg;
        c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2(1, 0)).rg;
        c.yxwz = SMAADecodeDiagBilinearAccess(c.xyzw);

        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);
        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));
        weights += SMAAAreaDiag(areaTex, d.xy, cc, subsampleIndices.z);
    }

    d.xz = SMAASearchDiag2(edgesTex, texcoord, float2(-1.0, -1.0), end);
    if (SMAASampleLevelZeroOffset(edgesTex, texcoord, int2(1, 0)).r > 0.0)
    {
        d.yw = SMAASearchDiag2(edgesTex, texcoord, float2(1.0, 1.0), end);
        d.y += float(end.y > 0.9);
    }
    else
    {
        d.yw = float2(0.0, 0.0);
    }

    if (d.x + d.y > 2.0)
    {
        const float4 coords =
            mad(float4(-d.x, -d.x, d.y, d.y), SMAA_RT_METRICS.xyxy, texcoord.xyxy);
        float4 c;
        c.x = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(-1, 0)).g;
        c.y = SMAASampleLevelZeroOffset(edgesTex, coords.xy, int2(0, -1)).r;
        c.zw = SMAASampleLevelZeroOffset(edgesTex, coords.zw, int2(1, 0)).gr;
        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);
        SMAAMovc(bool2(step(0.9, d.zw)), cc, float2(0.0, 0.0));
        weights += SMAAAreaDiag(areaTex, d.xy, cc, subsampleIndices.w).gr;
    }

    return weights;
}

//-----------------------------------------------------------------------------
// Horizontal/vertical search functions.

float SMAASearchLength(Texture2D<float4> searchTex, float2 e, float offset)
{
    const float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);
    const float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);
    const float2 adjustedScale = scale + float2(-1.0, 1.0);
    const float2 adjustedBias = bias + float2(0.5, -0.5);
    return SMAA_SEARCHTEX_SELECT(SMAASampleLevelZero(
        searchTex, mad(adjustedScale, e, adjustedBias) /
                       SMAA_SEARCHTEX_PACKED_SIZE));
}

float SMAASearchXLeft(Texture2D<float4> edgesTex, Texture2D<float4> searchTex,
                      float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    while (texcoord.x > end && e.g > 0.8281 && e.r == 0.0)
    {
        e = SMAASampleLevelZero(edgesTex, texcoord).rg;
        texcoord = mad(-float2(2.0, 0.0), SMAA_RT_METRICS.xy, texcoord);
    }
    const float offset =
        mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e, 0.0), 3.25);
    return mad(SMAA_RT_METRICS.x, offset, texcoord.x);
}

float SMAASearchXRight(Texture2D<float4> edgesTex, Texture2D<float4> searchTex,
                       float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    while (texcoord.x < end && e.g > 0.8281 && e.r == 0.0)
    {
        e = SMAASampleLevelZero(edgesTex, texcoord).rg;
        texcoord = mad(float2(2.0, 0.0), SMAA_RT_METRICS.xy, texcoord);
    }
    const float offset =
        mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e, 0.5), 3.25);
    return mad(-SMAA_RT_METRICS.x, offset, texcoord.x);
}

float SMAASearchYUp(Texture2D<float4> edgesTex, Texture2D<float4> searchTex,
                    float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    while (texcoord.y > end && e.r > 0.8281 && e.g == 0.0)
    {
        e = SMAASampleLevelZero(edgesTex, texcoord).rg;
        texcoord = mad(-float2(0.0, 2.0), SMAA_RT_METRICS.xy, texcoord);
    }
    const float offset =
        mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e.gr, 0.0), 3.25);
    return mad(SMAA_RT_METRICS.y, offset, texcoord.y);
}

float SMAASearchYDown(Texture2D<float4> edgesTex, Texture2D<float4> searchTex,
                      float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    while (texcoord.y < end && e.r > 0.8281 && e.g == 0.0)
    {
        e = SMAASampleLevelZero(edgesTex, texcoord).rg;
        texcoord = mad(float2(0.0, 2.0), SMAA_RT_METRICS.xy, texcoord);
    }
    const float offset =
        mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e.gr, 0.5), 3.25);
    return mad(-SMAA_RT_METRICS.y, offset, texcoord.y);
}

float2 SMAAArea(Texture2D<float4> areaTex, float2 dist, float e1, float e2,
                float offset)
{
    float2 texcoord =
        mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE),
            round(4.0 * float2(e1, e2)), dist);
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    texcoord.y = mad(SMAA_AREATEX_SUBTEX_SIZE, offset, texcoord.y);
    return SMAA_AREATEX_SELECT(SMAASampleLevelZero(areaTex, texcoord));
}

//-----------------------------------------------------------------------------
// Corner detection functions.

void SMAADetectHorizontalCornerPattern(Texture2D<float4> edgesTex,
                                       inout float2 weights, float4 texcoord,
                                       float2 d)
{
    const float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0, 1.0);
    factor.x -=
        rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0, 1)).r;
    factor.x -=
        rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, 1)).r;
    factor.y -=
        rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(0, -2)).r;
    factor.y -=
        rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, -2)).r;

    weights *= saturate(factor);
}

void SMAADetectVerticalCornerPattern(Texture2D<float4> edgesTex,
                                     inout float2 weights, float4 texcoord,
                                     float2 d)
{
    const float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0, 1.0);
    factor.x -=
        rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(1, 0)).g;
    factor.x -=
        rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(1, 1)).g;
    factor.y -=
        rounding.x * SMAASampleLevelZeroOffset(edgesTex, texcoord.xy, int2(-2, 0)).g;
    factor.y -=
        rounding.y * SMAASampleLevelZeroOffset(edgesTex, texcoord.zw, int2(-2, 1)).g;

    weights *= saturate(factor);
}

//-----------------------------------------------------------------------------
// Blending weight calculation (second pass).

float4 PSAASmaaWeight(AaVSOut input) : SV_Target
{

    const float2 texcoord = input.uv;
    const float2 pixcoord = texcoord * SMAA_RT_METRICS.zw;
    const float4 subsampleIndices = float4(0.0, 0.0, 0.0, 0.0);

    // Offsets used by the searches (see PSEUDO_GATHER4 in the reference).
    const float4 offset0 =
        mad(SMAA_RT_METRICS.xyxy, float4(-0.25, -0.125, 1.25, -0.125),
            texcoord.xyxy);
    const float4 offset1 =
        mad(SMAA_RT_METRICS.xyxy, float4(-0.125, -0.25, -0.125, 1.25),
            texcoord.xyxy);
    const float4 offset2 =
        mad(SMAA_RT_METRICS.xxyy,
            float4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS),
            float4(offset0.xz, offset1.yw));

    float4 weights = float4(0.0, 0.0, 0.0, 0.0);
    float2 e = SMAASample(ColorTexture, texcoord).rg;

    if (e.g > 0.0)
    {
        // Diagonals have both north and west edges, so searching for them in
        // one of the boundaries is enough.
        weights.rg = SMAACalculateDiagWeights(ColorTexture, SecondTexture, texcoord, e,
                                              subsampleIndices);

        // We give priority to diagonals, so if we find a diagonal we skip
        // horizontal/vertical processing.
        if (weights.r == -weights.g)
        {
            float2 d;

            // Find the distance to the left.
            float3 coords;
            coords.x = SMAASearchXLeft(ColorTexture, ThirdTexture, offset0.xy,
                                       offset2.x);
            coords.y = offset1.y;
            d.x = coords.x;

            // Fetch the left crossing edges, two at a time using bilinear
            // filtering. Sampling at -0.25 (CROSSING_OFFSET) disambiguates
            // which edges are active.
            const float e1 = SMAASampleLevelZero(ColorTexture, coords.xy).r;

            // Find the distance to the right.
            coords.z = SMAASearchXRight(ColorTexture, ThirdTexture, offset0.zw,
                                        offset2.y);
            d.y = coords.z;

            // Distances in pixel units.
            d = abs(round(mad(SMAA_RT_METRICS.zz, d, -pixcoord.xx)));

            const float2 sqrt_d = sqrt(d);

            // Fetch the right crossing edges.
            const float e2 =
                SMAASampleLevelZeroOffset(ColorTexture, coords.zy, int2(1, 0)).r;

            weights.rg = SMAAArea(SecondTexture, sqrt_d, e1, e2, subsampleIndices.y);

            coords.y = texcoord.y;
            SMAADetectHorizontalCornerPattern(ColorTexture, weights.rg,
                                              coords.xyzy, d);
        }
        else
        {
            e.r = 0.0;
        }
    }

    if (e.r > 0.0)
    {
        float2 d;

        // Find the distance to the top.
        float3 coords;
        coords.y = SMAASearchYUp(ColorTexture, ThirdTexture, offset1.xy, offset2.z);
        coords.x = offset0.x;
        d.x = coords.y;

        // Fetch the top crossing edges.
        const float e1 = SMAASampleLevelZero(ColorTexture, coords.xy).g;

        // Find the distance to the bottom.
        coords.z = SMAASearchYDown(ColorTexture, ThirdTexture, offset1.zw,
                                   offset2.w);
        d.y = coords.z;

        d = abs(round(mad(SMAA_RT_METRICS.ww, d, -pixcoord.yy)));

        const float2 sqrt_d = sqrt(d);

        // Fetch the bottom crossing edges.
        const float e2 =
            SMAASampleLevelZeroOffset(ColorTexture, coords.xz, int2(0, 1)).g;

        weights.ba = SMAAArea(SecondTexture, sqrt_d, e1, e2, subsampleIndices.x);

        coords.x = texcoord.x;
        SMAADetectVerticalCornerPattern(ColorTexture, weights.ba, coords.xyxz, d);
    }

    return weights;
}

//-----------------------------------------------------------------------------
// Neighborhood blending (third pass).

float4 PSAASmaaBlend(AaVSOut input) : SV_Target
{
    Texture2D<float4> blendTex = SecondTexture;

    const float2 texcoord = input.uv;
    const float4 offset =
        mad(SMAA_RT_METRICS.xyxy, float4(1.0, 0.0, 0.0, 1.0), texcoord.xyxy);

    // Fetch the blending weights for the current pixel.
    float4 a;
    a.x = SMAASample(blendTex, offset.xy).a;  // Right
    a.y = SMAASample(blendTex, offset.zw).g;  // Top
    a.wz = SMAASample(blendTex, texcoord).xz; // Bottom / Left

    // Is there any blending weight with a value greater than 0.0?
    if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) < 1e-5)
    {
        return SMAASampleLevelZero(ColorTexture, texcoord);
    }

    const bool h = max(a.x, a.z) > max(a.y, a.w); // max(horizontal) > max(vertical)

    // Calculate the blending offsets.
    float4 blendingOffset = float4(0.0, a.y, 0.0, a.w);
    float2 blendingWeight = a.yw;
    SMAAMovc(bool4(h, h, h, h), blendingOffset, float4(a.x, 0.0, a.z, 0.0));
    SMAAMovc(bool2(h, h), blendingWeight, a.xz);
    blendingWeight /= dot(blendingWeight, float2(1.0, 1.0));

    // Calculate the texture coordinates.
    const float4 blendingCoord =
        mad(blendingOffset, float4(SMAA_RT_METRICS.xy, -SMAA_RT_METRICS.xy),
            texcoord.xyxy);

    // Exploit bilinear filtering to mix the current pixel with the chosen
    // neighbour.
    float4 color =
        blendingWeight.x * SMAASampleLevelZero(ColorTexture, blendingCoord.xy);
    color +=
        blendingWeight.y * SMAASampleLevelZero(ColorTexture, blendingCoord.zw);
    return color;
}

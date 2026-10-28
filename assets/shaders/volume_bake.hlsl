// GPU port of the reduced-order analytic volume bake (VolumeBaker::BakeRange).
// Writes the same RGBA16F field directly into the volume texture, which removes
// the CPU voxel loop and the 16 MB upload per bake.
//
//   R = density, G = temperature / 12 kK, B = dust mask, A = soot mask.

cbuffer VolumeBakeConstants : register(b0)
{
    float4 BoundsMin;   // xyz = box minimum, w = resolution
    float4 BoundsSize;  // xyz = box size, w = voxel size
    float4 Fireball;    // xyz = centre, w = radius
    float4 Fireball2;   // x = temperature, y = visible, z = time, w = ambient temperature
    float4 Cloud;       // xyz = centre, w = radius
    float4 Cloud2;      // x = temperature, y = stem radius, z = strength, w = dust ring radius
    float4 Shock;       // x = radius, y = mach, z = burst height, w = dust intensity
};

RWTexture3D<float4> Output : register(u0);

static const float kTemperatureScale = 12000.0f;

uint HashCell(int x, int y, int z)
{
    uint hash = uint(x) * 0x8da6b343u ^ uint(y) * 0xd8163841u ^ uint(z) * 0xcb1ab31fu;
    hash ^= hash >> 13;
    hash *= 0x5bd1e995u;
    hash ^= hash >> 15;
    return hash;
}

float HashCellFloat(int x, int y, int z)
{
    return float(HashCell(x, y, z) & 0xFFFFFFu) / 16777215.0f;
}

// Trilinear value noise, matching the CPU baker exactly.
float ValueNoise(float x, float y, float z)
{
    const int xi = int(floor(x));
    const int yi = int(floor(y));
    const int zi = int(floor(z));
    const float fx = x - float(xi);
    const float fy = y - float(yi);
    const float fz = z - float(zi);
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float uz = fz * fz * (3.0f - 2.0f * fz);

    const float n000 = HashCellFloat(xi, yi, zi);
    const float n100 = HashCellFloat(xi + 1, yi, zi);
    const float n010 = HashCellFloat(xi, yi + 1, zi);
    const float n110 = HashCellFloat(xi + 1, yi + 1, zi);
    const float n001 = HashCellFloat(xi, yi, zi + 1);
    const float n101 = HashCellFloat(xi + 1, yi, zi + 1);
    const float n011 = HashCellFloat(xi, yi + 1, zi + 1);
    const float n111 = HashCellFloat(xi + 1, yi + 1, zi + 1);

    const float nx00 = n000 + (n100 - n000) * ux;
    const float nx10 = n010 + (n110 - n010) * ux;
    const float nx01 = n001 + (n101 - n001) * ux;
    const float nx11 = n011 + (n111 - n011) * ux;
    const float nxy0 = nx00 + (nx10 - nx00) * uy;
    const float nxy1 = nx01 + (nx11 - nx01) * uy;
    return nxy0 + (nxy1 - nxy0) * uz;
}

float Length2D(float x, float z)
{
    return sqrt(x * x + z * z);
}

[numthreads(4, 4, 4)]
void CSBake(uint3 id : SV_DispatchThreadID)
{
    const uint resolution = (uint)BoundsMin.w;
    if (any(id >= uint3(resolution, resolution, resolution)))
    {
        return;
    }

    const float invResolution = 1.0 / float(resolution);
    const float3 world = BoundsMin.xyz + (float3(id) + 0.5) * BoundsSize.xyz * invResolution;

    const float ambient = Fireball2.w;
    const float time = Fireball2.z;
    const float fireballRadius = max(Fireball.w, 0.25f);
    const float fireballRadiusSq = fireballRadius * fireballRadius;
    const float cloudRadius = max(Cloud.w, 0.5f);
    const float capMajor = cloudRadius * 0.62f;
    const float capMinor = max(cloudRadius * 0.52f, 0.75f);
    const float capX = Cloud.x;
    const float capY = Cloud.y;
    const float capZ = Cloud.z;
    const float cloudStrength = Cloud2.z;
    const float noiseScale = 6.0f;

    float density = 0.0f;
    float temperature = ambient;
    float dust = 0.0f;
    float soot = 0.0f;
    float fireballDistance = 0.0f;

    // --- Fireball ---------------------------------------------------------
    if (Fireball2.y > 0.5f)
    {
        const float dx = world.x - Fireball.x;
        const float dy = world.y - Fireball.y;
        const float dz = world.z - Fireball.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        fireballDistance = sqrt(distanceSq);

        if (distanceSq < fireballRadiusSq * 2.25f)
        {
            // Lumpy, filamentary surface. The lump amplitude fades in with the
            // fireball size so the initial explosion stays spherical.
            const float lumpFactor = smoothstep(0.15f, 0.60f, fireballRadius);
            const float lump =
                0.62f * ValueNoise((world.x + 17.0f) * noiseScale,
                                   (world.y - 9.0f) * noiseScale,
                                   (world.z + 31.0f) * noiseScale) +
                0.38f * ValueNoise((world.x - 3.0f) * noiseScale * 2.4f,
                                   (world.y + 11.0f) * noiseScale * 2.4f,
                                   (world.z - 19.0f) * noiseScale * 2.4f);
            const float effectiveRadius =
                fireballRadius * (1.0f + (lump - 0.5f) * 0.70f * lumpFactor);
            const float q = fireballDistance / max(effectiveRadius, 0.01f);
            const float falloff = 1.0f - smoothstep(0.25f, 1.20f, q);

            if (falloff > 0.001f)
            {
                const float detail = ValueNoise(
                    (world.x + 3.0f) * noiseScale * 2.2f,
                    (world.y - 13.0f) * noiseScale * 2.2f,
                    (world.z + 29.0f) * noiseScale * 2.2f);

                const float filament = ValueNoise(
                    (world.x + 41.0f) * noiseScale * 2.1f,
                    (world.y - 7.0f) * noiseScale * 2.1f,
                    (world.z + 13.0f) * noiseScale * 2.1f);
                const float flame = 0.60f + 0.85f * filament;
                density += 2.4f * pow(falloff, 1.15f) * flame *
                           (0.55f + 0.90f * detail);

                const float heatNoise = ValueNoise(
                    (world.x - 5.0f) * noiseScale * 1.7f,
                    (world.y + 23.0f) * noiseScale * 1.7f,
                    (world.z + 7.0f) * noiseScale * 1.7f);
                const float core =
                    saturate(1.0f - 0.9f * q) * (0.70f + 0.55f * heatNoise);
                const float hot = ambient +
                                  (Fireball2.x - ambient) * (0.28f + 1.00f * core) *
                                      (0.72f + 0.55f * detail);
                temperature = max(temperature, hot);

                const float sootRamp = saturate((time - 0.02f) / 0.25f);
                const float shell = smoothstep(0.52f, 0.98f, q);
                soot = max(soot, shell * sootRamp * (0.45f + 0.75f * detail));
            }
        }

        // --- Cooling smoke envelope ---------------------------------------
        const float smokeRamp = saturate((time - 0.035f) / 0.30f);
        if (smokeRamp > 0.01f)
        {
            const float radius = fireballRadius * (1.02f + 0.55f * smokeRamp);
            const float q = fireballDistance / max(radius, 0.05f);
            if (q < 2.0f)
            {
                const float ring = exp(-pow((q - 0.95f) / 0.42f, 2.0f));
                const float inner = exp(-pow(q / 0.95f, 2.0f));
                const float turbulence =
                    0.55f + 0.85f * ValueNoise(world.x * noiseScale * 0.8f + 5.0f,
                                               world.y * noiseScale * 0.8f - 3.0f,
                                               world.z * noiseScale * 0.8f + 9.0f);
                const float shell = (ring + 0.85f * inner) * turbulence;
                density += 2.20f * shell * smokeRamp;
                soot = max(soot, 0.95f * shell * smokeRamp);
            }
        }
    }

    // --- Shock front (compressed, heated air) -----------------------------
    if (Shock.x > 0.1f && Shock.y > 1.02f)
    {
        const float dx = world.x;
        const float dy = world.y - Shock.z;
        const float dz = world.z;
        const float distance = sqrt(dx * dx + dy * dy + dz * dz) - Shock.x;
        const float thickness = max(BoundsSize.w * 1.8f, Shock.x * 0.035f);

        if (abs(distance) < thickness * 2.5f)
        {
            const float q = distance / thickness;
            const float shell = exp(-q * q);
            density += 0.65f * shell;

            const float mach = max(Shock.y, 1.0f);
            const float machSq = mach * mach;
            const float temperatureRatio =
                ((2.8f * machSq - 0.4f) * (0.4f * machSq + 2.0f)) / (5.76f * machSq);
            const float shockTemperature =
                min(ambient * max(temperatureRatio, 1.0f), 6500.0f);
            temperature =
                max(temperature, ambient + (shockTemperature - ambient) * shell);
            dust = max(dust, 0.35f * shell);
        }
    }

    // --- Rising mushroom cap (torus) --------------------------------------
    if (cloudStrength > 0.01f)
    {
        const float dx = world.x - capX;
        const float dz = world.z - capZ;
        const float horiz = Length2D(dx, dz);
        const float ringOffset = horiz - capMajor;
        const float dy = (world.y - capY) * 1.35f;
        const float torusDistance =
            sqrt(ringOffset * ringOffset + dy * dy) / capMinor;

        if (torusDistance < 1.8f)
        {
            const float falloff = exp(-torusDistance * torusDistance * 1.25f);
            density += 1.1f * falloff * cloudStrength;
            const float hot = ambient + (Cloud2.x - ambient) * (0.35f + 0.65f * falloff);
            temperature = max(temperature, hot);
            dust = max(dust, 0.25f * falloff);
            soot = max(soot, 0.65f * falloff * cloudStrength);
        }

        // --- Stem ---------------------------------------------------------
        if (world.y > 0.0f && world.y < capY + capMinor)
        {
            const float axisX = capX * 0.65f;
            const float axisZ = capZ * 0.65f;
            const float wobbleX =
                sin(world.y * 0.035f + time * 0.6f) * Cloud2.y * 0.55f;
            const float wobbleZ =
                cos(world.y * 0.028f + time * 0.45f) * Cloud2.y * 0.55f;

            const float stemRadius =
                Cloud2.y * (0.85f + 0.55f * world.y / max(capY, 1.0f));
            const float radial =
                Length2D(world.x - axisX - wobbleX, world.z - axisZ - wobbleZ) /
                max(stemRadius, 0.5f);

            if (radial < 2.5f)
            {
                const float verticalMask =
                    smoothstep(0.0f, 1.0f, world.y / 1.5f) *
                    (1.0f - smoothstep(capY * 0.9f, capY + capMinor * 0.6f, world.y));
                const float falloff = exp(-radial * radial * 1.5f);
                density += 0.65f * falloff * verticalMask * cloudStrength;
                temperature = max(
                    temperature,
                    ambient + (Cloud2.x - ambient) * 0.55f * falloff * verticalMask);
                dust = max(dust, 0.55f * falloff * verticalMask);
                soot = max(soot, 0.70f * falloff * verticalMask);
            }
        }
    }

    // --- Base dust surge ---------------------------------------------------
    if (world.y < fireballRadius * 3.0f)
    {
        const float baseRamp = saturate((time - 0.015f) / 0.15f);
        if (baseRamp > 0.01f)
        {
            const float radial = Length2D(world.x, world.z);
            const float baseRadius = fireballRadius * 1.9f;
            const float ringOffset =
                (radial - baseRadius * 0.55f) / (baseRadius * 0.65f);
            const float ring = exp(-ringOffset * ringOffset);
            const float height = exp(-world.y / (fireballRadius * 0.80f + 0.25f));
            const float turbulence =
                0.55f + 0.80f * ValueNoise(world.x * 3.1f + 2.0f,
                                           world.y * 3.1f - 5.0f,
                                           world.z * 3.1f + 7.0f);
            density += 1.35f * ring * height * turbulence * baseRamp;
            dust = max(dust, 0.95f * ring * height * baseRamp);
        }
    }

    // --- Ground dust ring (only the part inside the box) -------------------
    if (Shock.w > 0.01f && Cloud2.w > 1.0f && world.y < 12.0f)
    {
        const float horiz = Length2D(world.x, world.z);
        const float width = max(Cloud2.w * 0.10f, 2.0f);
        const float ringOffset = (horiz - Cloud2.w) / width;
        if (abs(ringOffset) < 3.0f)
        {
            const float ring = exp(-ringOffset * ringOffset);
            const float heightFalloff = exp(-world.y / (Cloud2.w * 0.06f + 1.5f));
            density += 0.9f * ring * heightFalloff * Shock.w;
            dust = max(dust, 0.85f);
        }
    }

    if (density > 0.002f)
    {
        Output[id] = float4(density, saturate(temperature / kTemperatureScale),
                            saturate(dust), saturate(soot));
    }
    else
    {
        Output[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
}

// GPU particle path: applies the CPU spawns, integrates the whole particle
// population and builds the three instance batches in one compute pass. The
// integration and colour code mirror ParticleSystem::IntegrateRange and
// BuildBatches, so the visual result matches the CPU path.

cbuffer ParticleUpdateConstants : register(b0)
{
    float4 WindTime;   // xyz = wind, w = time
    float4 Params;     // x = ambient temperature, y = gravity, z = dt, w = capacity
    float4 Counts;     // x = spawn count
};

struct ParticleState
{
    float3 position;
    float size;
    float3 velocity;
    float life;
    float maxLife;
    float temperature;
    float opacity;
    float drag;
    float growth;
    float seed;
    uint kind;
    uint alive;
};

struct ParticleSpawn
{
    uint slot;
    uint pad0;
    uint pad1;
    uint pad2;
    ParticleState state;
};

struct Instance
{
    float3 position;
    float size;
    float3 color;
    float opacity;
    float rotation;
    float variation;
    float pad0;
    float pad1;
};

RWStructuredBuffer<ParticleState> StateBuffer : register(u0);
StructuredBuffer<ParticleSpawn> Spawns : register(t0);
RWStructuredBuffer<Instance> Additive : register(u1);
RWStructuredBuffer<Instance> Alpha : register(u2);
RWStructuredBuffer<Instance> Fire : register(u3);
RWStructuredBuffer<uint> Counters : register(u4);
RWBuffer<uint> Indirect : register(u5);

static const uint kDebris = 0;
static const uint kSpark = 1;
static const uint kFire = 2;
static const uint kSmoke = 3;
static const uint kDust = 4;

float HashNoise(float x, float y, float z)
{
    const float value = sin(x * 12.9898f + y * 78.233f + z * 37.719f) * 43758.5453f;
    return value - floor(value);
}

// Tanner-Helland approximation in linear RGB (matches the CPU particles).
float3 BlackbodyColor(float kelvin)
{
    const float t = clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    if (t <= 66.0f)
    {
        r = 1.0f;
        g = clamp((99.4708025861f * log(t) - 161.1195681661f) / 255.0f, 0.0f, 1.0f);
        b = t <= 19.0f ? 0.0f
                       : clamp((log(t - 10.0f) * 138.5177312231f - 305.0447927307f) /
                                   255.0f,
                               0.0f, 1.0f);
    }
    else
    {
        r = clamp(329.698727446f * pow(t - 60.0f, -0.1332047592f) / 255.0f, 0.0f, 1.0f);
        g = clamp(288.1221695283f * pow(t - 60.0f, -0.0755148492f) / 255.0f, 0.0f, 1.0f);
        b = 1.0f;
    }

    return float3(pow(r, 2.2f), pow(g, 2.2f), pow(b, 2.2f));
}

void AppendInstance(uint batch, Instance instance)
{
    uint index = 0;
    InterlockedAdd(Counters[batch], 1, index);
    if (batch == 0)
    {
        Additive[index] = instance;
    }
    else if (batch == 1)
    {
        Alpha[index] = instance;
    }
    else
    {
        Fire[index] = instance;
    }
}

[numthreads(64, 1, 1)]
void CSApplySpawns(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Counts.x)
    {
        return;
    }
    const ParticleSpawn spawn = Spawns[id.x];
    StateBuffer[spawn.slot] = spawn.state;
}

// Clears the whole state buffer. ClearUnorderedAccessViewUint only writes the
// first 16 bytes of a structured element, so the reset needs a real pass.
[numthreads(64, 1, 1)]
void CSReset(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Params.w)
    {
        return;
    }
    ParticleState empty = (ParticleState)0;
    StateBuffer[id.x] = empty;
}

[numthreads(64, 1, 1)]
void CSIntegrate(uint3 id : SV_DispatchThreadID)
{
    const uint capacity = (uint)Params.w;
    if (id.x >= capacity)
    {
        return;
    }

    ParticleState p = StateBuffer[id.x];
    if (p.alive == 0)
    {
        return;
    }

    const float dt = Params.z;
    const float time = WindTime.w;
    const float gravity = Params.y;
    const float ambient = Params.x;

    p.life += dt;
    const float lifeRatio = p.life / max(p.maxLife, 1e-3f);
    if (lifeRatio >= 1.0f)
    {
        p.alive = 0;
        StateBuffer[id.x] = p;
        return;
    }

    if (p.kind == kDebris)
    {
        const float speed = length(p.velocity);
        const float dragMagnitude = p.drag * speed;
        p.velocity.x -= p.velocity.x * dragMagnitude * dt;
        p.velocity.z -= p.velocity.z * dragMagnitude * dt;
        p.velocity.y -= (p.velocity.y * dragMagnitude + gravity) * dt;

        p.position += p.velocity * dt;
        if (p.position.y < p.size * 0.5f)
        {
            p.position.y = p.size * 0.5f;
            if (p.velocity.y < 0.0f)
            {
                p.velocity.y = -p.velocity.y * 0.22f;
                p.velocity.x *= 0.55f;
                p.velocity.z *= 0.55f;
            }
        }

        p.temperature = ambient + (p.temperature - ambient) * exp(-dt / 1.6f);
        p.opacity = 1.0f;
    }
    else if (p.kind == kSpark)
    {
        const float speed = length(p.velocity);
        const float dragMagnitude = p.drag * speed;
        p.velocity += p.velocity * (-dragMagnitude * dt);
        p.velocity.y -= gravity * dt;
        p.position += p.velocity * dt;

        p.temperature = ambient + (p.temperature - ambient) * exp(-dt / 0.35f);
        p.opacity = clamp((p.temperature - 800.0f) / 1200.0f, 0.0f, 1.0f) *
                    (1.0f - lifeRatio * lifeRatio);
    }
    else if (p.kind == kFire)
    {
        const float speed = length(p.velocity);
        const float dragMagnitude = p.drag * speed;
        p.velocity += p.velocity * (-dragMagnitude * dt);
        p.velocity.y += (8.0f - p.velocity.y * 0.4f) * dt;

        const float noiseX = HashNoise(p.position.x * 4.0f, p.position.y * 4.0f, time * 8.0f) - 0.5f;
        const float noiseZ = HashNoise(p.position.z * 4.0f, p.seed, time * 8.0f) - 0.5f;
        p.velocity.x += noiseX * dt * 30.0f;
        p.velocity.z += noiseZ * dt * 30.0f;

        p.position += p.velocity * dt;
        p.size += p.growth * dt;
        p.temperature = ambient + (p.temperature - ambient) * exp(-dt / 0.11f);
        p.opacity = pow(max(0.0f, 1.0f - lifeRatio), 1.4f);
    }
    else if (p.kind == kSmoke)
    {
        const float excess = (p.temperature - ambient) / ambient;
        const float buoyancy = gravity * clamp(excess, 0.0f, 4.0f) * 0.22f;

        const float relax = 1.0f - exp(-dt * 0.35f);
        p.velocity.x += (WindTime.x - p.velocity.x) * relax;
        p.velocity.z += (WindTime.z - p.velocity.z) * relax;
        p.velocity.y += (buoyancy - p.velocity.y * 0.05f) * dt;

        const float noiseX = HashNoise(p.position.x * 0.05f, p.position.y * 0.05f, time * 0.4f) - 0.5f;
        const float noiseZ = HashNoise(p.position.z * 0.05f, p.seed, time * 0.4f) - 0.5f;
        p.velocity.x += noiseX * dt * 9.0f;
        p.velocity.z += noiseZ * dt * 9.0f;

        p.position += p.velocity * dt;
        p.size += p.growth * dt;
        p.temperature = ambient + (p.temperature - ambient) * exp(-dt / 2.4f);

        const float fadeIn = clamp(p.life / 0.6f, 0.0f, 1.0f);
        const float fadeOut = 1.0f - clamp((lifeRatio - 0.55f) / 0.45f, 0.0f, 1.0f);
        p.opacity = 0.65f * fadeIn * fadeOut;
    }
    else if (p.kind == kDust)
    {
        const float excess = (p.temperature - ambient) / ambient;
        const float buoyancy = gravity * clamp(excess, 0.0f, 1.0f) * 0.08f;

        const float relax = 1.0f - exp(-dt * 0.8f);
        p.velocity.x += (WindTime.x - p.velocity.x) * relax;
        p.velocity.z += (WindTime.z - p.velocity.z) * relax;
        p.velocity.y += (buoyancy - gravity * 0.02f) * dt - p.velocity.y * 0.15f * dt;

        p.position += p.velocity * dt;
        if (p.position.y < 0.5f)
        {
            p.position.y = 0.5f;
            p.velocity.y = max(0.0f, p.velocity.y);
        }
        p.size += p.growth * dt;
        p.temperature = ambient + (p.temperature - ambient) * exp(-dt / 3.0f);

        const float fadeIn = clamp(p.life / 1.2f, 0.0f, 1.0f);
        const float fadeOut = 1.0f - clamp((lifeRatio - 0.5f) / 0.5f, 0.0f, 1.0f);
        p.opacity = 0.30f * fadeIn * fadeOut;
    }

    Instance instance;
    instance.position = p.position;
    instance.size = p.size;
    instance.opacity = p.opacity;
    instance.rotation = p.seed;
    instance.variation = p.seed * 0.731f;
    instance.color = float3(0.0f, 0.0f, 0.0f);
    instance.pad0 = 1.0f;
    instance.pad1 = 0.0f;

    if (p.kind == kDebris || p.kind == kSpark)
    {
        instance.color = BlackbodyColor(p.temperature);
        AppendInstance(0, instance);
    }
    else if (p.kind == kFire)
    {
        instance.color = BlackbodyColor(p.temperature);
        instance.opacity = p.opacity;
        instance.rotation = 0.0f;
        instance.pad0 = 0.25f;
        AppendInstance(2, instance);
    }
    else
    {
        const float warmth = clamp((p.temperature - ambient) / 900.0f, 0.0f, 1.0f);
        const float3 cool = p.kind == kSmoke ? float3(0.18f, 0.17f, 0.16f)
                                             : float3(0.55f, 0.46f, 0.34f);
        const float3 warm = float3(0.85f, 0.42f, 0.16f);
        instance.color = cool + (warm - cool) * warmth * 0.25f;
        AppendInstance(1, instance);
    }

    StateBuffer[id.x] = p;
}

// One thread turns the batch counters into three DrawInstancedIndirect args:
// { vertexCount = 6, instanceCount, startVertex = 0, startInstance = 0 }.
[numthreads(1, 1, 1)]
void CSFinalize(uint3 id : SV_DispatchThreadID)
{
    Counters[3] = Counters[0] + Counters[1] + Counters[2];
    Indirect[0] = 6;
    Indirect[1] = Counters[0];
    Indirect[2] = 0;
    Indirect[3] = 0;
    Indirect[4] = 6;
    Indirect[5] = Counters[1];
    Indirect[6] = 0;
    Indirect[7] = 0;
    Indirect[8] = 6;
    Indirect[9] = Counters[2];
    Indirect[10] = 0;
    Indirect[11] = 0;
}

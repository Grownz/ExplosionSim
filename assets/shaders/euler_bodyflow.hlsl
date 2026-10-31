// Samples the fluid state at each rigid-body centre for the 4C drag coupling.
// One thread per body; the CPU turns the sampled density and velocity into an
// impulse on the matching PhysicsWorld body.
//
// The shader declares only the fields it uses from the solver constants buffer
// (grid and domain minimum), so it can share the solver's constant buffer and
// state texture without binding the full pass set.

#if defined(EXPLOSION_VULKAN)
#define EX_VK_BINDING(n) [[vk::binding(n, 0)]]
#else
#define EX_VK_BINDING(n)
#endif

EX_VK_BINDING(0) cbuffer BodyFlowConstants : register(b0)
{
    int4 Grid;         // xyz = resolution
    float4 DomainMin;  // xyz = lower corner, w = cell size dx
};

EX_VK_BINDING(8) Texture3D<float4> StateA : register(t0);
EX_VK_BINDING(30) StructuredBuffer<float4> BodyPositions : register(t4);
EX_VK_BINDING(31) RWStructuredBuffer<float4> BodyFlow : register(u4);

[numthreads(64, 1, 1)]
void CSBodyFlow(uint3 id : SV_DispatchThreadID)
{
    uint count = 0;
    uint stride = 0;
    BodyPositions.GetDimensions(count, stride);
    if (id.x >= count)
    {
        return;
    }

    const float3 world = BodyPositions[id.x].xyz;
    const int3 cell = clamp(
        int3((world - DomainMin.xyz) / max(DomainMin.w, 1e-4f)),
        int3(0, 0, 0), Grid.xyz - 1);
    const float4 state = StateA.Load(int4(cell, 0));
    const float rho = max(state.x, 1e-6f);
    BodyFlow[id.x] = float4(rho, state.yzw / rho);
}

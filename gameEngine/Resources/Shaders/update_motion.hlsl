// Step F2: GPU motion — integrate velocity into GpuTransform (before ComposeWorld)
//
// linear:  v.y -= gravity * dt;  p += v * dt
// angular: r += w * dt  (pitch/yaw/roll radians)
// boundsCenter follows translation so frustum/occlusion stay roughly valid

struct GpuTransform
{
    float3 position; float pad0;
    float3 rotation; float pad1;
    float3 scale;    float pad2;
    float3 boundsCenter;  float pad3;
    float3 boundsExtents; float pad4;
    uint batchId;
    uint flags;
    uint pad5;
    uint pad6;
};

struct GpuMotion
{
    float3 linearVelocity;  float pad0;
    float3 angularVelocity; float pad1;
    float  gravity;
    uint   flags;
    uint   pad2;
    uint   pad3;
};

cbuffer cbMotion : register(b0)
{
    uint  gNumInstances;
    uint  gMaxInstances;
    float gDeltaTime;
    uint  gPad0;
};

RWStructuredBuffer<GpuTransform> gXforms : register(u0);
RWStructuredBuffer<GpuMotion>    gMotion : register(u1);

[numthreads(64, 1, 1)]
void CS_UpdateMotion(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= gNumInstances || i >= gMaxInstances)
        return;

    GpuMotion m = gMotion[i];
    if ((m.flags & 1u) == 0u)
        return;

    float dt = gDeltaTime;
    if (dt <= 0.0f || dt > 0.25f)
        dt = 1.0f / 60.0f;

    // Gravity (same convention as GravitySystem: -Y)
    if (m.gravity != 0.0f)
        m.linearVelocity.y -= m.gravity * dt;

    float3 dp = m.linearVelocity * dt;
    float3 dr = m.angularVelocity * dt;

    GpuTransform x = gXforms[i];
    x.position += dp;
    x.rotation += dr;
    // Keep world AABB center tracking translation (extents unchanged)
    x.boundsCenter += dp;

    gXforms[i] = x;
    gMotion[i] = m;
}

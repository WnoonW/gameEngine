// Pass1 CS: 컬링 + 인스턴스 압축
// Pass2 CSFinalize: 카운터로 DRAW_INDEXED 인자 1개 기록

struct DrawRequest
{
    float4x4 world;
    float boundsCenterX, boundsCenterY, boundsCenterZ, boundsPad0;
    float boundsExtentsX, boundsExtentsY, boundsExtentsZ, boundsPad1;
};

struct InstanceWorld
{
    float4x4 world;
};

struct IndirectCommand
{
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    int  BaseVertexLocation;
    uint StartInstanceLocation;
    uint pad0, pad1, pad2;
};

cbuffer cbBuild : register(b0)
{
    float4 gFrustumPlanes[6];
    uint   gNumRequests;
    uint   gEnableFrustumCull;
    uint   gIndexCount;
    uint   gStartIndexLocation;
    int    gBaseVertexLocation;
    uint   gMaxInstances;
    uint   gPad1;
    uint   gPad2;
};

StructuredBuffer<DrawRequest>         gRequests  : register(t0);
RWStructuredBuffer<InstanceWorld>     gInstances : register(u0);
RWStructuredBuffer<uint>              gCounter   : register(u1);
RWStructuredBuffer<IndirectCommand>   gDrawCmd   : register(u2);

bool IsAabbInsideOrIntersectFrustum(float3 center, float3 extents)
{
    if (extents.x <= 0.0f && extents.y <= 0.0f && extents.z <= 0.0f)
        return true;

    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float4 p = gFrustumPlanes[i];
        float3 n = p.xyz;
        float r = extents.x * abs(n.x) + extents.y * abs(n.y) + extents.z * abs(n.z);
        float s = dot(n, center) + p.w;
        if (s + r < 0.0f)
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void CS(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= gNumRequests)
        return;

    DrawRequest req = gRequests[i];

    if (gEnableFrustumCull != 0)
    {
        float3 center = float3(req.boundsCenterX, req.boundsCenterY, req.boundsCenterZ);
        float3 extents = float3(req.boundsExtentsX, req.boundsExtentsY, req.boundsExtentsZ);
        if (!IsAabbInsideOrIntersectFrustum(center, extents))
            return;
    }

    uint outIndex;
    InterlockedAdd(gCounter[0], 1, outIndex);
    if (outIndex >= gMaxInstances)
        return;

    InstanceWorld inst;
    inst.world = req.world;
    gInstances[outIndex] = inst;
}

[numthreads(1, 1, 1)]
void CSFinalize(uint3 dtid : SV_DispatchThreadID)
{
    uint n = gCounter[0];
    if (n > gMaxInstances)
        n = gMaxInstances;

    IndirectCommand cmd;
    cmd.IndexCountPerInstance = gIndexCount;
    cmd.InstanceCount = n;
    cmd.StartIndexLocation = gStartIndexLocation;
    cmd.BaseVertexLocation = gBaseVertexLocation;
    cmd.StartInstanceLocation = 0;
    cmd.pad0 = cmd.pad1 = cmd.pad2 = 0;
    gDrawCmd[0] = cmd;
}

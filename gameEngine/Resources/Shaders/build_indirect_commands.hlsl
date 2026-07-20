// GPU: DrawRequest -> IndirectCommand (선택적 프러스텀 컬링 + 압축)

struct DrawRequest
{
    uint objectCbvLow;
    uint objectCbvHigh;
    uint indexCount;
    uint startIndexLocation;
    int  baseVertexLocation;
    uint instanceCount;
    uint pad0;
    uint pad1;
    float boundsCenterX;
    float boundsCenterY;
    float boundsCenterZ;
    float boundsPad0;
    float boundsExtentsX;
    float boundsExtentsY;
    float boundsExtentsZ;
    float boundsPad1;
};

struct IndirectCommand
{
    uint2 objectCbv;
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    int  BaseVertexLocation;
    uint StartInstanceLocation;
    uint pad;
};

cbuffer cbBuild : register(b0)
{
    float4 gFrustumPlanes[6];
    uint   gNumRequests;
    uint   gEnableFrustumCull;
    uint   gCommandWriteBase;
    uint   gPad1;
};

StructuredBuffer<DrawRequest>       gRequests : register(t0);
RWStructuredBuffer<IndirectCommand> gCommands : register(u0);
RWStructuredBuffer<uint>            gCounter  : register(u1);

bool IsAabbInsideOrIntersectFrustum(float3 center, float3 extents)
{
    // extents == 0 → 컬링 정보 없음: 통과
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
    if (req.indexCount == 0 || req.instanceCount == 0)
        return;

    if (gEnableFrustumCull != 0)
    {
        float3 center = float3(req.boundsCenterX, req.boundsCenterY, req.boundsCenterZ);
        float3 extents = float3(req.boundsExtentsX, req.boundsExtentsY, req.boundsExtentsZ);
        if (!IsAabbInsideOrIntersectFrustum(center, extents))
            return;
    }

    uint outIndex;
    InterlockedAdd(gCounter[0], 1, outIndex);

    // 그룹 내 상대 인덱스 + 그룹 베이스
    uint writeIndex = gCommandWriteBase + outIndex;
    if (writeIndex >= 8192)
        return;

    IndirectCommand cmd;
    cmd.objectCbv = uint2(req.objectCbvLow, req.objectCbvHigh);
    cmd.IndexCountPerInstance = req.indexCount;
    cmd.InstanceCount = req.instanceCount;
    cmd.StartIndexLocation = req.startIndexLocation;
    cmd.BaseVertexLocation = req.baseVertexLocation;
    cmd.StartInstanceLocation = 0;
    cmd.pad = 0;

    gCommands[writeIndex] = cmd;
}

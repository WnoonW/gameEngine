// Path3 GPU-driven:
//   CS_CullCompact  — 전역 절두체 컬링 + 배치별 압축
//   CS_BuildCommands — 서브메시별 DRAW_INDEXED 인자 일괄 생성

struct GpuInstanceSource
{
    float4x4 world;
    float boundsCenterX, boundsCenterY, boundsCenterZ, boundsPad0;
    float boundsExtentsX, boundsExtentsY, boundsExtentsZ, boundsPad1;
    uint batchId;
    uint flags;
    uint pad2;
    uint pad3;
};

struct InstanceWorld
{
    float4x4 world;
};

struct GpuBatchDesc
{
    uint firstInstance;
    uint instanceCount;
    uint firstSubmesh;
    uint submeshCount;
};

struct GpuSubmeshDesc
{
    uint indexCount;
    uint startIndexLocation;
    int  baseVertexLocation;
    uint batchId;
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

cbuffer cbFrame : register(b0)
{
    float4 gFrustumPlanes[6];
    uint   gNumInstances;
    uint   gEnableFrustumCull;
    uint   gNumBatches;
    uint   gNumSubmeshDraws;
    uint   gMaxInstances;
    uint   gPad0;
    uint   gPad1;
    uint   gPad2;
};

StructuredBuffer<GpuInstanceSource> gSource       : register(t0);
StructuredBuffer<GpuBatchDesc>      gBatches      : register(t1);
StructuredBuffer<GpuSubmeshDesc>    gSubmeshes    : register(t2);

RWStructuredBuffer<InstanceWorld>   gCompact      : register(u0);
RWStructuredBuffer<uint>            gBatchCounters: register(u1);
RWStructuredBuffer<IndirectCommand> gDrawCmds     : register(u2);

bool IsAabbInsideOrIntersectFrustum(float3 center, float3 extents)
{
    // extents 전부 0 → 아직 bounds 미계산: 컬링 스킵(보이도록)
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

// ---------------------------------------------------------------------------
// 전역 1회: 컬링 + 배치 로컬 compact
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CS_CullCompact(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= gNumInstances)
        return;

    GpuInstanceSource src = gSource[i];
    if ((src.flags & 1u) == 0u)
        return;

    if (gEnableFrustumCull != 0)
    {
        float3 center = float3(src.boundsCenterX, src.boundsCenterY, src.boundsCenterZ);
        float3 extents = float3(src.boundsExtentsX, src.boundsExtentsY, src.boundsExtentsZ);
        if (!IsAabbInsideOrIntersectFrustum(center, extents))
            return;
    }

    uint batchId = src.batchId;
    if (batchId >= gNumBatches)
        return;

    GpuBatchDesc batch = gBatches[batchId];
    if (batch.instanceCount == 0)
        return;

    uint local;
    InterlockedAdd(gBatchCounters[batchId], 1, local);
    if (local >= batch.instanceCount)
        return;

    uint outIndex = batch.firstInstance + local;
    if (outIndex >= gMaxInstances)
        return;

    InstanceWorld inst;
    inst.world = src.world;
    gCompact[outIndex] = inst;
}

// ---------------------------------------------------------------------------
// 전역 1회: 서브메시 드로우 커맨드 생성
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void CS_BuildCommands(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= gNumSubmeshDraws)
        return;

    GpuSubmeshDesc sub = gSubmeshes[i];
    uint batchId = sub.batchId;
    uint n = 0;
    if (batchId < gNumBatches)
    {
        n = gBatchCounters[batchId];
        GpuBatchDesc batch = gBatches[batchId];
        if (n > batch.instanceCount)
            n = batch.instanceCount;
    }

    IndirectCommand cmd;
    cmd.IndexCountPerInstance = sub.indexCount;
    cmd.InstanceCount = n;
    cmd.StartIndexLocation = sub.startIndexLocation;
    cmd.BaseVertexLocation = sub.baseVertexLocation;
    cmd.StartInstanceLocation = 0;
    cmd.pad0 = 0;
    cmd.pad1 = 0;
    cmd.pad2 = 0;
    gDrawCmds[i] = cmd;
}



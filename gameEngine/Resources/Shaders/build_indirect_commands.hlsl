// Path3 GPU-driven:
//   CS_CullCompact  — frustum cull + batch compact
//   CS_BuildCommands — DRAW_INDEXED args (InstanceCount clamped)
//
// Hi-Z occlusion is CPU-gated (gEnableOcclusion). When off, no texture samples.

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
    uint   gEnableOcclusion;
    uint   gHizMipCount;
    uint   gHizValid;
    float4x4 gViewProj;
    float  gRtWidth;
    float  gRtHeight;
    float  gZNear;
    float  gZFar;
    uint4  gPad;
};

StructuredBuffer<GpuInstanceSource> gSource       : register(t0);
StructuredBuffer<GpuBatchDesc>      gBatches      : register(t1);
StructuredBuffer<GpuSubmeshDesc>    gSubmeshes    : register(t2);
// Optional Hi-Z (only sampled when gEnableOcclusion && gHizValid)
Texture2D                           gHiZ          : register(t3);
SamplerState                        gPointSampler : register(s0);

RWStructuredBuffer<InstanceWorld>   gCompact      : register(u0);
RWStructuredBuffer<uint>            gBatchCounters: register(u1);
RWStructuredBuffer<IndirectCommand> gDrawCmds     : register(u2);

bool IsAabbInsideOrIntersectFrustum(float3 center, float3 extents)
{
    // 단일 return 경로 — FXC X4000(미초기화 오탐) 방지
    bool inside = true;
    const bool hasExtents =
        (extents.x > 0.0f) || (extents.y > 0.0f) || (extents.z > 0.0f);

    if (hasExtents)
    {
        [unroll]
        for (int i = 0; i < 6; ++i)
        {
            float4 p = gFrustumPlanes[i];
            float3 n = p.xyz;
            float r = extents.x * abs(n.x) + extents.y * abs(n.y) + extents.z * abs(n.z);
            float s = dot(n, center) + p.w;
            if (s + r < 0.0f)
                inside = false;
        }
    }
    return inside;
}

// Conservative previous-frame depth test. Prefer false (visible) on doubt.
bool IsOccludedByHiZ(float3 center, float3 extents)
{
    bool occluded = false;

    [branch]
    if (gHizValid != 0 && gRtWidth >= 1.0f && gRtHeight >= 1.0f
        && (extents.x > 0.0f || extents.y > 0.0f || extents.z > 0.0f))
    {
        float3 c0 = center + float3(-extents.x, -extents.y, -extents.z);
        float3 c1 = center + float3(+extents.x, -extents.y, -extents.z);
        float3 c2 = center + float3(-extents.x, +extents.y, -extents.z);
        float3 c3 = center + float3(+extents.x, +extents.y, -extents.z);
        float3 c4 = center + float3(-extents.x, -extents.y, +extents.z);
        float3 c5 = center + float3(+extents.x, -extents.y, +extents.z);
        float3 c6 = center + float3(-extents.x, +extents.y, +extents.z);
        float3 c7 = center + float3(+extents.x, +extents.y, +extents.z);

        float minU = 1.0f, minV = 1.0f, maxU = 0.0f, maxV = 0.0f;
        float nearestZ = 1.0f;
        uint valid = 0;

        float3 corners[8] = { c0, c1, c2, c3, c4, c5, c6, c7 };
        [unroll]
        for (int i = 0; i < 8; ++i)
        {
            float4 clip = mul(float4(corners[i], 1.0f), gViewProj);
            if (clip.w > 1e-4f)
            {
                float invW = rcp(clip.w);
                float ndcX = clip.x * invW;
                float ndcY = clip.y * invW;
                float ndcZ = clip.z * invW;
                if (ndcZ >= 0.0f && ndcZ <= 1.0f)
                {
                    float u = ndcX * 0.5f + 0.5f;
                    float v = 0.5f - ndcY * 0.5f;
                    minU = min(minU, u);
                    maxU = max(maxU, u);
                    minV = min(minV, v);
                    maxV = max(maxV, v);
                    nearestZ = min(nearestZ, ndcZ);
                    valid++;
                }
            }
        }

        if (valid == 8)
        {
            minU = saturate(minU);
            maxU = saturate(maxU);
            minV = saturate(minV);
            maxV = saturate(maxV);

            float farthest = 0.0f;
            bool anySky = false;
            [unroll]
            for (int gy = 0; gy < 2; ++gy)
            {
                [unroll]
                for (int gx = 0; gx < 2; ++gx)
                {
                    float u = lerp(minU, maxU, (gx + 0.5f) * 0.5f);
                    float v = lerp(minV, maxV, (gy + 0.5f) * 0.5f);
                    float d = gHiZ.SampleLevel(gPointSampler, float2(u, v), 0).r;
                    farthest = max(farthest, d);
                    if (d <= 1e-5f || d >= 0.999f)
                        anySky = true;
                }
            }

            if (!anySky && farthest > 1e-5f && farthest < 0.999f)
            {
                const float bias = 0.01f;
                if (nearestZ > (farthest + bias))
                    occluded = true;
            }
        }
    }

    return occluded;
}

[numthreads(64, 1, 1)]
void CS_CullCompact(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= gNumInstances || i >= gMaxInstances)
        return;

    GpuInstanceSource src = gSource[i];
    if ((src.flags & 1u) == 0u)
        return;

    float3 center = float3(src.boundsCenterX, src.boundsCenterY, src.boundsCenterZ);
    float3 extents = float3(src.boundsExtentsX, src.boundsExtentsY, src.boundsExtentsZ);

    if (gEnableFrustumCull != 0)
    {
        if (!IsAabbInsideOrIntersectFrustum(center, extents))
            return;
    }

    // Only sample Hi-Z when fully enabled (uniform branch)
    [branch]
    if (gEnableOcclusion != 0 && gHizValid != 0)
    {
        if (IsOccludedByHiZ(center, extents))
            return;
    }

    uint batchId = src.batchId;
    if (batchId >= gNumBatches || batchId >= 256u)
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

[numthreads(64, 1, 1)]
void CS_BuildCommands(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= gNumSubmeshDraws)
        return;

    GpuSubmeshDesc sub = gSubmeshes[i];
    uint batchId = sub.batchId;
    uint n = 0;
    if (batchId < gNumBatches && batchId < 256u)
    {
        n = gBatchCounters[batchId];
        GpuBatchDesc batch = gBatches[batchId];
        if (n > batch.instanceCount)
            n = batch.instanceCount;
        if (n > gMaxInstances)
            n = gMaxInstances;
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

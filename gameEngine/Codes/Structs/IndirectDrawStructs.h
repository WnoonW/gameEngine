#pragma once
#include <cstdint>
#include <d3d12.h>
#include "RenderLimits.h"

// CPU/GPU 공유 — HLSL float4x4 = 64 bytes

struct InstanceWorld
{
    float worldMatrix[16]{};
};
static_assert(sizeof(InstanceWorld) == 64, "InstanceWorld");

#pragma pack(push, 4)
struct IndirectCommand
{
    uint32_t IndexCountPerInstance = 0;
    uint32_t InstanceCount = 0;
    uint32_t StartIndexLocation = 0;
    int32_t  BaseVertexLocation = 0;
    uint32_t StartInstanceLocation = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};
#pragma pack(pop)
static_assert(sizeof(IndirectCommand) == 32, "IndirectCommand");

struct GpuInstanceSource
{
    float worldMatrix[16]{};
    float boundsCenterX = 0;
    float boundsCenterY = 0;
    float boundsCenterZ = 0;
    float boundsPad0 = 0;
    float boundsExtentsX = 0;
    float boundsExtentsY = 0;
    float boundsExtentsZ = 0;
    float boundsPad1 = 0;
    uint32_t batchId = 0;
    uint32_t flags = 1;
    uint32_t pad2 = 0;
    uint32_t pad3 = 0;
};
static_assert(sizeof(GpuInstanceSource) == 112, "GpuInstanceSource");

// Step F1: CPU uploads TRS; CS_ComposeWorld fills GpuInstanceSource.world
// (column_major store of S*R*T == StoreWorldTransposed on CPU)
struct GpuTransform
{
    float position[3]{};
    float pad0 = 0;
    float rotation[3]{}; // pitch, yaw, roll (GetWorldMatrix와 동일)
    float pad1 = 0;
    float scale[3]{ 1, 1, 1 };
    float pad2 = 0;
    float boundsCenter[3]{};
    float pad3 = 0;
    float boundsExtents[3]{};
    float pad4 = 0;
    uint32_t batchId = 0;
    uint32_t flags = 1;
    uint32_t pad5 = 0;
    uint32_t pad6 = 0;
};
static_assert(sizeof(GpuTransform) == 96, "GpuTransform");

struct GpuBatchDesc
{
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t firstSubmesh = 0;
    uint32_t submeshCount = 0;
};
static_assert(sizeof(GpuBatchDesc) == 16, "GpuBatchDesc");

struct GpuSubmeshDesc
{
    uint32_t indexCount = 0;
    uint32_t startIndexLocation = 0;
    int32_t  baseVertexLocation = 0;
    uint32_t batchId = 0;
};
static_assert(sizeof(GpuSubmeshDesc) == 16, "GpuSubmeshDesc");

// Path3 frame CB — must match build_indirect_commands.hlsl cbFrame (256 bytes)
struct GpuDrivenFrameConstants
{
    float frustumPlanes[6][4]{};     // 96
    uint32_t numInstances = 0;
    uint32_t enableFrustumCull = 1;
    uint32_t numBatches = 0;
    uint32_t numSubmeshDraws = 0;    // +16 = 112
    uint32_t maxInstances = kMaxInstancesPerDraw;
    uint32_t enableOcclusion = 0;
    uint32_t hizMipCount = 0;
    uint32_t hizValid = 0;           // +16 = 128
    float viewProj[16]{};            // +64 = 192 (transposed for HLSL mul)
    float rtWidth = 1.f;
    float rtHeight = 1.f;
    float zNear = 0.1f;
    float zFar = 1000.f;            // +16 = 208
    uint32_t pad[12]{};              // +48 = 256
};
static_assert(sizeof(GpuDrivenFrameConstants) == 256, "GpuDrivenFrameConstants");

// Hi-Z pass CB
struct HiZBuildConstants
{
    uint32_t srcWidth = 1;
    uint32_t srcHeight = 1;
    uint32_t dstWidth = 1;
    uint32_t dstHeight = 1;
};
static_assert(sizeof(HiZBuildConstants) == 16, "HiZBuildConstants");

using IndirectDrawRequest = GpuInstanceSource;
using IndirectBuildConstants = GpuDrivenFrameConstants;

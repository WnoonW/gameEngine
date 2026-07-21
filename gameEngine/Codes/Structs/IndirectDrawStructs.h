#pragma once
#include <cstdint>
#include <d3d12.h>
#include "RenderLimits.h"

// CPU/GPU 공유 — HLSL float4x4 = 64 bytes

// ---------------------------------------------------------------------------
// Path2 / compact 결과: 인스턴스 월드만
// ---------------------------------------------------------------------------
struct InstanceWorld
{
    float worldMatrix[16]{};
};
static_assert(sizeof(InstanceWorld) == 64, "InstanceWorld");

// ExecuteIndirect: DRAW_INDEXED only (worlds in t1/t3 SRV)
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

// ---------------------------------------------------------------------------
// Path3 GPU-driven: persistent source + batch table
// ---------------------------------------------------------------------------

// 소스 인스턴스 1개 (GPU 상주 후보). HLSL GpuInstanceSource 와 동일 레이아웃.
struct GpuInstanceSource
{
    float worldMatrix[16]{}; // transposed for HLSL mul(pos, world)
    float boundsCenterX = 0;
    float boundsCenterY = 0;
    float boundsCenterZ = 0;
    float boundsPad0 = 0;
    float boundsExtentsX = 0;
    float boundsExtentsY = 0;
    float boundsExtentsZ = 0;
    float boundsPad1 = 0;
    uint32_t batchId = 0;
    uint32_t flags = 1; // bit0 = visible
    uint32_t pad2 = 0;
    uint32_t pad3 = 0;
};
static_assert(sizeof(GpuInstanceSource) == 112, "GpuInstanceSource");

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

// Path3 프레임 상수 (cbuffer b0). 256-byte 정렬은 업로드 시 적용.
struct GpuDrivenFrameConstants
{
    float frustumPlanes[6][4]{};
    uint32_t numInstances = 0;
    uint32_t enableFrustumCull = 1;
    uint32_t numBatches = 0;
    uint32_t numSubmeshDraws = 0;
    uint32_t maxInstances = kMaxInstancesPerDraw;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};
static_assert(sizeof(GpuDrivenFrameConstants) == 128, "GpuDrivenFrameConstants");

// 구 이름 호환 (일부 코드/문서)
using IndirectDrawRequest = GpuInstanceSource;
using IndirectBuildConstants = GpuDrivenFrameConstants;

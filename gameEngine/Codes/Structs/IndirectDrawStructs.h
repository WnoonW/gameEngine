#pragma once
#include <cstdint>
#include <d3d12.h>
#include "RenderLimits.h"

// CPU/GPU 공유 — HLSL float4x4 = 64 bytes

// 인스턴스 후보 1개 (월드 + AABB)
struct IndirectDrawRequest
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
};
static_assert(sizeof(IndirectDrawRequest) == 96, "IndirectDrawRequest");

struct InstanceWorld
{
    float worldMatrix[16]{};
};
static_assert(sizeof(InstanceWorld) == 64, "InstanceWorld");

// ExecuteIndirect: DRAW_INDEXED only (인스턴스 월드 별도 SRV)
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

struct IndirectBuildConstants
{
    float frustumPlanes[6][4]{};
    uint32_t numRequests = 0;
    uint32_t enableFrustumCull = 1;
    uint32_t indexCount = 0;
    uint32_t startIndexLocation = 0;
    int32_t  baseVertexLocation = 0;
    uint32_t maxInstances = kMaxInstancesPerDraw;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};
static_assert(sizeof(IndirectBuildConstants) == 128, "IndirectBuildConstants");

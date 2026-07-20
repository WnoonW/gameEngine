#pragma once
#include <cstdint>
#include <d3d12.h>

// CPU/GPU 공유 레이아웃 — HLSL과 바이트 단위 일치

// Compute 입력 (64 bytes)
struct IndirectDrawRequest
{
    uint32_t objectCbvLow = 0;
    uint32_t objectCbvHigh = 0;
    uint32_t indexCount = 0;
    uint32_t startIndexLocation = 0;
    int32_t  baseVertexLocation = 0;
    uint32_t instanceCount = 1;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    float boundsCenterX = 0;
    float boundsCenterY = 0;
    float boundsCenterZ = 0;
    float boundsPad0 = 0;
    float boundsExtentsX = 0;
    float boundsExtentsY = 0;
    float boundsExtentsZ = 0;
    float boundsPad1 = 0;
};
static_assert(sizeof(IndirectDrawRequest) == 64, "IndirectDrawRequest size must match HLSL");

// ExecuteIndirect 커맨드 + 16B 정렬 패딩 (32 bytes)
#pragma pack(push, 4)
struct IndirectCommand
{
    uint32_t objectCbvLow = 0;
    uint32_t objectCbvHigh = 0;
    uint32_t IndexCountPerInstance = 0;
    uint32_t InstanceCount = 0;
    uint32_t StartIndexLocation = 0;
    int32_t  BaseVertexLocation = 0;
    uint32_t StartInstanceLocation = 0;
    uint32_t pad = 0;
};
#pragma pack(pop)
static_assert(sizeof(IndirectCommand) == 32, "IndirectCommand size must match command signature stride");

// Compute b0 (112 bytes, CBV 정렬은 Upload 시 256 단위 슬롯 사용)
struct IndirectBuildConstants
{
    float frustumPlanes[6][4]{};
    uint32_t numRequests = 0;
    uint32_t enableFrustumCull = 0; // 기본 끔 (플레인 오류로 과다 컬링 방지)
    uint32_t commandWriteBase = 0;  // 출력 커맨드 버퍼 시작 인덱스
    uint32_t pad1 = 0;
};
static_assert(sizeof(IndirectBuildConstants) == 112, "IndirectBuildConstants size");

constexpr UINT kMaxIndirectDraws = 8192;
constexpr UINT kMaxIndirectGroups = 256;
constexpr UINT kIndirectFrameCount = 3;

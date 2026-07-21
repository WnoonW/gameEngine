#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <vector>
#include <unordered_map>
#include <cstdint>

#include "World.h"
#include "ComponentStruct.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"
#include "../Structs/IndirectDrawStructs.h"
#include "../Structs/RenderLimits.h"

using namespace ECS;
using Microsoft::WRL::ComPtr;

struct Material;
struct Mesh;

// 1 Basic  = 원본 per-object 드로우
// 2 Instanced = CPU 업로드 + DrawIndexedInstanced (안정 인스턴싱)
// 3 ComputeIndirect = GPU-driven (persistent source + cull/compact + EI)
enum class RenderPath
{
    Basic = 0,
    Instanced = 1,
    ComputeIndirect = 2,

    // 별칭 (기존 코드 호환)
    Direct = Basic,
    Indirect = Instanced,
};

class RenderSystem
{
public:
    void Initialize(ID3D12Device* device);
    void Shutdown();

    void SetRenderPath(RenderPath path);
    RenderPath GetRenderPath() const { return mRenderPath; }
    bool IsComputeIndirectReady() const { return mComputeIndirectReady; }

    // ComputeIndirect: GPU 절두체 컬링 (기본 ON)
    void SetGpuFrustumCullEnabled(bool enabled) { mGpuFrustumCull = enabled; }
    bool IsGpuFrustumCullEnabled() const { return mGpuFrustumCull; }

    // 스폰/삭제/머티리얼 변경 시 Path2 캐시 + Path3 구조 무효화
    void InvalidateDrawCache();

    void render(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

private:
    struct FrameGpuResources
    {
        // Path2: requestUpload = InstanceWorld[]
        // Path3: sourceUpload = GpuInstanceSource[], batch/submesh meta, frame CB
        ComPtr<ID3D12Resource> requestUpload;
        ComPtr<ID3D12Resource> batchUpload;
        ComPtr<ID3D12Resource> submeshUpload;
        ComPtr<ID3D12Resource> frameCBUpload;
        ComPtr<ID3D12Resource> instanceBuffer;  // compact InstanceWorld[] UAV
        ComPtr<ID3D12Resource> countBuffer;     // per-batch counters
        ComPtr<ID3D12Resource> drawCmdBuffer;   // IndirectCommand[]

        BYTE* requestMapped = nullptr;
        BYTE* batchMapped = nullptr;
        BYTE* submeshMapped = nullptr;
        BYTE* frameCBMapped = nullptr;

        D3D12_RESOURCE_STATES instanceState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES countState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES drawCmdState = D3D12_RESOURCE_STATE_COMMON;

        uint32_t uploadedSourceVersion = 0;
        uint32_t uploadedMetaVersion = 0;
    };

    // Path3: CPU 배치 (바인딩용 mesh/material 포함)
    struct GpuCpuBatch
    {
        Mesh* mesh = nullptr;
        Material* mainMaterial = nullptr;
        UINT firstInstance = 0;
        UINT instanceCount = 0;
        UINT firstSubmesh = 0;
        UINT submeshCount = 0;
        std::vector<UINT64> materialGpuPtrs;
    };

    void renderBasic(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    void renderInstanced(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    void renderComputeIndirect(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    void renderDirect(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix)
    {
        renderBasic(world, cmdList, currentFrameResource, descriptorAllocator, viewMatrix, projMatrix);
    }

    void renderIndirect(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix)
    {
        renderInstanced(world, cmdList, currentFrameResource, descriptorAllocator,
            currentFrameIndex, viewMatrix, projMatrix);
    }

    void EnsureGpuResources(ID3D12Device* device);
    void DestroyGpuResources();
    void ExtractFrustumPlanes(const DirectX::XMMATRIX& viewProj, float outPlanes[6][4]);
    static UINT FrameCBAlignedSize();

    // Path3 scene management
    void RebuildGpuDrivenScene(World& world);
    void PatchGpuDrivenTransforms(World& world, FrameResource* frameResource);
    void UploadGpuDrivenFrameData(FrameGpuResources& frame);

    // Path2 cache draw
    void DrawInstancedBatches(
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        FrameGpuResources& frame,
        const std::vector<Entity>& overrideEntities,
        World& world);

    struct CachedInstancedBatch
    {
        Mesh* mesh = nullptr;
        Material* mainMaterial = nullptr;
        std::vector<InstanceWorld> instances;
    };

    RenderPath mRenderPath = RenderPath::ComputeIndirect;
    bool mGpuFrustumCull = true; // Path3 기본 ON
    ID3D12Device* mDevice = nullptr;

    ComPtr<ID3D12PipelineState> mCullCompactPSO;
    ComPtr<ID3D12PipelineState> mBuildCommandsPSO;
    ComPtr<ID3D12Resource> mCounterZeroUpload;
    ComPtr<ID3D12Resource> mDummyInstanceBuffer;

    FrameGpuResources mFrames[kIndirectFrameCount]{};
    bool mComputeIndirectReady = false;

    // Path2 static cache
    bool mInstancedCacheValid = false;
    std::vector<CachedInstancedBatch> mCachedInstancedBatches;
    std::vector<Entity> mCachedOverrideEntities;

    // Path3 CPU mirror + versions
    bool mGpuStructureDirty = true;
    uint32_t mSourceContentVersion = 1;
    uint32_t mMetaVersion = 1;
    std::vector<GpuInstanceSource> mSourceCpu;
    std::vector<GpuBatchDesc> mBatchDescsCpu;
    std::vector<GpuSubmeshDesc> mSubmeshDescsCpu;
    std::vector<GpuCpuBatch> mGpuCpuBatches;
    std::vector<Entity> mGpuOverrideEntities;
    std::unordered_map<Entity, UINT> mEntityToGpuSlot;
};

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

// ---------------------------------------------------------------------------
// Render paths
//   Basic           = per-object DrawIndexed
//   Instanced       = CPU batch + DrawIndexedInstanced
//   ComputeIndirect = GPU cull/compact + ExecuteIndirect (default)
// ---------------------------------------------------------------------------
enum class RenderPath
{
    Basic = 0,
    Instanced = 1,
    ComputeIndirect = 2,
    Direct = Basic,
    Indirect = Instanced,
};

enum class GraphicsStyle
{
    Realistic = 0,
    Toon = 1,
};

// CPU stats for ImGui (filled during render)
struct GpuDrivenFrameStats
{
    RenderPath path = RenderPath::Instanced;
    bool autoPath = false;
    bool cullEnabled = false;
    bool occlusionEnabled = false;
    bool hizValid = false;
    bool didRebuild = false;
    bool didSourceUpload = false;
    bool didMetaUpload = false;
    bool usedDirtyList = false;
    bool skippedPatch = false;
    bool usedDefaultHeapCopy = false;
    bool didBuildHiZ = false;
    bool didComposeWorld = false;
    bool didGpuMotion = false;
    bool gpuMotionEnabled = false;
    bool lodEnabled = false;
    uint32_t lodCulled = 0;
    uint32_t lodLevelCounts[4]{};
    uint32_t lodSwitches = 0;
    uint32_t sourceCount = 0;
    uint32_t batchCount = 0;
    uint32_t submeshDraws = 0;
    uint32_t dirtyPatched = 0;
    uint32_t dirtyListIn = 0;
    uint32_t pendingDirty = 0;
    uint32_t hizMips = 0;
    uint32_t eiCalls = 0;
    uint32_t multiEiRuns = 0;
    uint32_t motionActive = 0;
    bool gpuCullReadbackValid = false;
    uint32_t gpuVisibleInstances = 0;
    uint32_t gpuSubmittedInstances = 0;
    uint32_t gpuCulledInstances = 0;
    float rebuildMs = 0.f;
    float patchMs = 0.f;
    float uploadMs = 0.f;
    float composeMs = 0.f;
    float motionMs = 0.f;
    float lodMs = 0.f;
    float hizMs = 0.f;
};

// =============================================================================
// RenderSystem
//
// Frame: FillPassCB -> RenderShadowMap -> SceneBegin -> render -> SceneEnd
//        -> BuildHiZ
//
// Code layout (3 tiers):
//   MAIN     RenderSystem.cpp + _GpuDriven/_Instanced/_Basic/_Shadow/_HiZ.cpp
//   SUB      RenderSystem_SubLogic.cpp   (path/shadow/LOD stages)
//   HELPERS  RenderSystem_Helpers.cpp + RenderDrawHelpers.h
// =============================================================================
class RenderSystem
{
public:
    // ----- Main frame stream -----
    void render(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    void RenderShadowMap(
        World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    void BuildHiZ(
        ID3D12GraphicsCommandList* cmdList,
        DescriptorAllocator* descriptorAllocator,
        ID3D12Resource* sceneDepth,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSrvCpu,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvGpu,
        UINT width, UINT height);

    // ----- Controls -----
    void SetRenderPath(RenderPath path);
    RenderPath GetRenderPath() const { return mRenderPath; }
    bool IsComputeIndirectReady() const { return mComputeIndirectReady; }

    void SetAutoRenderPathEnabled(bool enabled);
    bool IsAutoRenderPathEnabled() const { return mAutoRenderPath; }

    void SetGpuFrustumCullEnabled(bool enabled) { mGpuFrustumCull = enabled; }
    bool IsGpuFrustumCullEnabled() const { return mGpuFrustumCull; }
    void SetGpuOcclusionEnabled(bool enabled) { mGpuOcclusion = enabled; }
    bool IsGpuOcclusionEnabled() const { return mGpuOcclusion; }

    void SetGpuMotionEnabled(bool enabled);
    bool IsGpuMotionEnabled() const { return mGpuMotionEnabled; }
    bool ShouldSkipCpuGravity() const;
    void NotifyEntityMotionChanged(World& world, Entity e);
    void SetFrameDeltaTime(float dt) { mFrameDeltaTime = dt; }

    void SetLodEnabled(bool enabled);
    bool IsLodEnabled() const { return mLodEnabled; }
    void SetLodDistanceCullEnabled(bool enabled) { mLodDistanceCull = enabled; }
    bool IsLodDistanceCullEnabled() const { return mLodDistanceCull; }
    void SetLodBias(float bias) { mLodBias = (bias > 0.05f) ? bias : 0.05f; }
    float GetLodBias() const { return mLodBias; }
    void SetLodCullDistance(float d) { mLodCullDistance = (d > 1.f) ? d : 1.f; }
    float GetLodCullDistance() const { return mLodCullDistance; }

    void SetOutlinePassEnabled(bool enabled) { mOutlinePassEnabled = enabled; }
    bool IsOutlinePassEnabled() const { return mOutlinePassEnabled; }
    void SetShadowsEnabled(bool enabled) { mShadowsEnabled = enabled; }
    bool IsShadowsEnabled() const { return mShadowsEnabled; }

    // ----- Lifecycle / debug -----
    void Initialize(ID3D12Device* device);
    void Shutdown();
    void PrepareHiZForSceneSize(DescriptorAllocator* alloc, UINT width, UINT height);
    void InvalidateDrawCache();
    const GpuDrivenFrameStats& GetLastFrameStats() const { return mLastStats; }

private:
    // =========================================================================
    // Types
    // =========================================================================
    struct FrameGpuResources
    {
        ComPtr<ID3D12Resource> requestUpload;
        ComPtr<ID3D12Resource> transformUpload;
        ComPtr<ID3D12Resource> motionUpload;
        ComPtr<ID3D12Resource> batchUpload;
        ComPtr<ID3D12Resource> submeshUpload;
        ComPtr<ID3D12Resource> frameCBUpload;
        ComPtr<ID3D12Resource> transformDefault;
        ComPtr<ID3D12Resource> motionDefault;
        ComPtr<ID3D12Resource> sourceDefault;
        ComPtr<ID3D12Resource> batchDefault;
        ComPtr<ID3D12Resource> submeshDefault;
        ComPtr<ID3D12Resource> instanceBuffer;
        ComPtr<ID3D12Resource> countBuffer;
        ComPtr<ID3D12Resource> drawCmdBuffer;
        ComPtr<ID3D12Resource> countReadback;
        bool countReadbackPending = false;
        uint32_t countReadbackBatches = 0;
        uint32_t countReadbackSources = 0;
        BYTE* requestMapped = nullptr;
        BYTE* transformMapped = nullptr;
        BYTE* motionMapped = nullptr;
        BYTE* batchMapped = nullptr;
        BYTE* submeshMapped = nullptr;
        BYTE* frameCBMapped = nullptr;
        D3D12_RESOURCE_STATES instanceState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES countState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES drawCmdState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES transformDefaultState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES motionDefaultState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES sourceDefaultState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES batchDefaultState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES submeshDefaultState = D3D12_RESOURCE_STATE_COMMON;
        uint32_t uploadedTransformVersion = 0;
        uint32_t uploadedMotionVersion = 0;
        uint32_t composedTransformVersion = 0;
        uint32_t uploadedMetaVersion = 0;
    };

    struct GpuCpuBatch
    {
        Mesh* mesh = nullptr;
        Material* mainMaterial = nullptr;
        UINT firstInstance = 0;
        UINT instanceCount = 0;
        UINT firstSubmesh = 0;
        UINT submeshCount = 0;
        std::vector<UINT> materialIndices;
    };

    struct CachedInstancedBatch
    {
        Mesh* mesh = nullptr;
        Material* mainMaterial = nullptr;
        std::vector<InstanceWorld> instances;
        std::vector<UINT> materialIndices;
    };

    struct InstancedDrawRecord
    {
        Mesh* mesh = nullptr;
        UINT instanceCount = 0;
        UINT64 byteOffset = 0;
        Material* mainMaterial = nullptr;
    };

    // =========================================================================
    // MAIN — path orchestrators (one "story" function per path)
    // =========================================================================
    void renderComputeIndirect(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);
    void renderInstanced(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);
    void renderBasic(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    // =========================================================================
    // SUB-LOGIC — pipeline stages (RenderSystem_SubLogic.cpp)
    // =========================================================================
    // Frame prep
    void UpdateEntityLods(World& world, const DirectX::XMMATRIX& viewMatrix);
    void UpdateAutoRenderPath(World& world);

    // Path3
    void BeginGpuDrivenFrameStats();
    void UpdateGpuDrivenSceneData(World& world, FrameResource* currentFrameResource);
    void DrawMaterialOverrideEntities(
        World& world, ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource, ID3D12RootSignature* sceneRS);
    UINT PrepareGpuDrivenMotionAndCompose(
        ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame,
        int currentFrameIndex, World& world);
    void FillGpuDrivenCullConstants(
        FrameGpuResources& frame,
        const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
        UINT numInstances);
    bool DispatchGpuCullAndBuildCommands(
        ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame,
        DescriptorAllocator* descriptorAllocator, UINT numInstances);
    void ExecuteIndirectOpaquePass(
        ID3D12GraphicsCommandList* cmdList, FrameResource* currentFrameResource,
        FrameGpuResources& frame, DescriptorAllocator* descriptorAllocator,
        ID3D12RootSignature* sceneRS, ID3D12PipelineState* gfxPSO,
        ID3D12CommandSignature* cmdSig);
    void DrawGpuDrivenOutlinePass(
        ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame,
        DescriptorAllocator* descriptorAllocator, ID3D12RootSignature* sceneRS);
    void RebuildGpuDrivenScene(World& world);
    void PatchGpuDrivenTransforms(World& world, FrameResource* frameResource);
    bool PatchOneGpuEntity(World& world, FrameResource* frameResource, Entity e,
        bool& anyPatched, bool& motionPatched);
    void SyncGpuMotionFromWorld(World& world);
    void PullGpuTransformsFromWorld(World& world);
    bool UploadGpuDrivenFrameData(ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame);
    bool DispatchUpdateMotion(ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame, UINT numInstances);
    void DispatchComposeWorld(ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame, UINT numInstances);

    // Path2
    bool TryDrawCachedInstancedFrame(
        ID3D12GraphicsCommandList* cmdList, FrameResource* currentFrameResource,
        FrameGpuResources& frame, DescriptorAllocator* descriptorAllocator, World& world);
    void RebuildInstancedBatchCache(World& world, FrameResource* currentFrameResource);
    void DrawInstancedBatches(
        ID3D12GraphicsCommandList* cmdList, FrameResource* currentFrameResource,
        FrameGpuResources& frame, DescriptorAllocator* descriptorAllocator,
        const std::vector<Entity>& overrideEntities, World& world);
    void DrawInstancedOverrideEntities(
        ID3D12GraphicsCommandList* cmdList, FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator, ID3D12RootSignature* sceneRS,
        const std::vector<Entity>& overrideEntities, World& world);
    void UploadAndDrawInstancedOpaque(
        ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame,
        DescriptorAllocator* descriptorAllocator, ID3D12RootSignature* sceneRS,
        std::vector<InstancedDrawRecord>& outDrawRecs);
    void DrawInstancedOutlinePass(
        ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame,
        DescriptorAllocator* descriptorAllocator, ID3D12RootSignature* sceneRS,
        const std::vector<InstancedDrawRecord>& drawRecs);

    // Path1
    void DrawBasicOpaquePass(
        World& world, ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource, DescriptorAllocator* descriptorAllocator);
    void DrawBasicOutlinePass(
        World& world, ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource, DescriptorAllocator* descriptorAllocator,
        ID3D12RootSignature* sceneRS);

    // Shadow stages
    void BeginShadowDepthTarget(ID3D12GraphicsCommandList* cmdList);
    void DrawShadowCasters(
        World& world, ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource, ID3D12RootSignature* shadowRS);
    void EndShadowMapAsSrv(ID3D12GraphicsCommandList* cmdList);

    // =========================================================================
    // HELPERS — resources / bind / math (RenderSystem_Helpers.cpp)
    // =========================================================================
    void BindSceneFrameState(
        ID3D12GraphicsCommandList* cmdList, FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator, ID3D12RootSignature* sceneRS,
        bool bindPassAndShadow);
    void BindShadowMapSrv(ID3D12GraphicsCommandList* cmdList) const;
    void ExtractFrustumPlanes(const DirectX::XMMATRIX& viewProj, float outPlanes[6][4]);
    static UINT FrameCBAlignedSize();

    void EnsureGpuResources(ID3D12Device* device);
    void DestroyGpuResources();

    void EnsureShadowMap(DescriptorAllocator* alloc);
    void DestroyShadowMap(DescriptorAllocator* alloc);

    bool IsHiZSampleReady() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetHiZSampleSrvGpu() const;
    void EnsureHiZResources(DescriptorAllocator* alloc, UINT width, UINT height);
    void EnsureDummyHiZSrv(DescriptorAllocator* alloc);
    void DestroyHiZResources(DescriptorAllocator* alloc);

    void ResolveGpuCullReadback(FrameGpuResources& frame);
    void ScheduleGpuCullReadback(
        ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame,
        UINT numBatches, UINT numSources);

    // =========================================================================
    // Members
    // =========================================================================
    RenderPath mRenderPath = RenderPath::ComputeIndirect;
    bool mAutoRenderPath = true;
    bool mGpuFrustumCull = true;
    bool mGpuOcclusion = true;
    bool mGpuMotionEnabled = true;
    bool mLodEnabled = true;
    bool mLodDistanceCull = true;
    bool mOutlinePassEnabled = false;
    bool mShadowsEnabled = true;
    float mLodBias = 1.f;
    float mLodCullDistance = 250.f;
    float mFrameDeltaTime = 1.f / 60.f;

    GpuDrivenFrameStats mLastStats{};
    uint32_t mMotionActiveCount = 0;
    bool mLodStatEnabled = false;
    uint32_t mLodStatCulled = 0;
    uint32_t mLodStatSwitches = 0;
    uint32_t mLodStatLevels[4]{};
    float mLodStatMs = 0.f;

    bool mInstancedCacheValid = false;
    std::vector<CachedInstancedBatch> mCachedInstancedBatches;
    std::vector<Entity> mCachedOverrideEntities;
    uint32_t mPath2CachedDirtyGen = 0;

    bool mGpuStructureDirty = true;
    uint32_t mTransformContentVersion = 1;
    uint32_t mMotionContentVersion = 1;
    uint32_t mMetaVersion = 1;
    std::vector<GpuTransform> mTransformCpu;
    std::vector<GpuMotion> mMotionCpu;
    std::vector<GpuInstanceSource> mSourceCpu;
    std::vector<GpuBatchDesc> mBatchDescsCpu;
    std::vector<GpuSubmeshDesc> mSubmeshDescsCpu;
    std::vector<GpuCpuBatch> mGpuCpuBatches;
    std::vector<Entity> mGpuOverrideEntities;
    std::unordered_map<Entity, UINT> mEntityToGpuSlot;
    std::vector<Entity> mPendingTransformDirty;
    uint32_t mLastProcessedDirtyGen = 0;

    ID3D12Device* mDevice = nullptr;
    DescriptorAllocator* mSrvAlloc = nullptr;
    bool mComputeIndirectReady = false;
    FrameGpuResources mFrames[kIndirectFrameCount]{};

    ComPtr<ID3D12PipelineState> mCullCompactPSO;
    ComPtr<ID3D12PipelineState> mBuildCommandsPSO;
    ComPtr<ID3D12PipelineState> mComposeWorldPSO;
    ComPtr<ID3D12PipelineState> mUpdateMotionPSO;
    ComPtr<ID3D12PipelineState> mHiZCopyPSO;
    ComPtr<ID3D12PipelineState> mHiZDownsamplePSO;
    ComPtr<ID3D12Resource> mCounterZeroUpload;
    ComPtr<ID3D12Resource> mDummyInstanceBuffer;
    ComPtr<ID3D12Resource> mDummyHiZTexture;
    DescriptorAllocator::DescriptorHandle mDummyHiZSrv{};

    static constexpr UINT kShadowMapSize = 2048;
    Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mShadowDsvHeap;
    DescriptorAllocator::DescriptorHandle mShadowSrv{};
    D3D12_RESOURCE_STATES mShadowState = D3D12_RESOURCE_STATE_COMMON;

    static constexpr UINT kHiZRingSize = kIndirectFrameCount;
    struct HiZSlot
    {
        ComPtr<ID3D12Resource> texture;
        DescriptorAllocator::DescriptorHandle srv{};
        DescriptorAllocator::DescriptorHandle uav{};
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };
    HiZSlot mHiZRing[kHiZRingSize]{};
    UINT mHiZWriteSlot = 0;
    UINT mHiZBuildCount = 0;
    UINT mHiZWidth = 0;
    UINT mHiZHeight = 0;
    UINT mHiZMipCount = 1;
    XMMATRIX mLastViewProj = DirectX::XMMatrixIdentity();
    float mLastNear = 0.1f;
    float mLastFar = 1000.f;
    UINT mLastRtWidth = 1;
    UINT mLastRtHeight = 1;
};

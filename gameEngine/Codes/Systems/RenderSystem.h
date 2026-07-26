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

    Direct = Basic,
    Indirect = Instanced,
};

// Scene lighting / shading look (PassCB gGraphicsStyle)
enum class GraphicsStyle
{
    Realistic = 0, // smooth Lambert + soft Blinn
    Toon = 1,      // banded NdotL (+ optional inverted-hull outline)
};

// Step A: 프레임 단위 CPU 통계 (ImGui / 디버그)
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
    bool didComposeWorld = false; // Step F1
    bool didGpuMotion = false;    // Step F2
    bool gpuMotionEnabled = false;
    bool lodEnabled = false;      // Step H
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
    uint32_t eiCalls = 0;       // ExecuteIndirect 호출 수
    uint32_t multiEiRuns = 0;   // MaxCommandCount > 1 인 연속 머티리얼 런
    uint32_t motionActive = 0;  // slots with motion flags
    // Step I: GPU cull readback (1 ring-slot delayed ≈ 1–3 frames)
    bool gpuCullReadbackValid = false;
    uint32_t gpuVisibleInstances = 0;  // after frustum+occlusion compact
    uint32_t gpuSubmittedInstances = 0; // sources dispatched to cull that frame
    uint32_t gpuCulledInstances = 0;    // submitted - visible
    float rebuildMs = 0.f;
    float patchMs = 0.f;
    float uploadMs = 0.f;
    float composeMs = 0.f;
    float motionMs = 0.f;
    float lodMs = 0.f;
    float hizMs = 0.f;
};

class RenderSystem
{
public:
    void Initialize(ID3D12Device* device);
    void Shutdown();

    void SetRenderPath(RenderPath path);
    RenderPath GetRenderPath() const { return mRenderPath; }
    bool IsComputeIndirectReady() const { return mComputeIndirectReady; }

    // Step G: 오브젝트 수에 따라 Basic/Instanced/ComputeIndirect 자동 선택
    void SetAutoRenderPathEnabled(bool enabled);
    bool IsAutoRenderPathEnabled() const { return mAutoRenderPath; }

    void SetGpuFrustumCullEnabled(bool enabled) { mGpuFrustumCull = enabled; }
    bool IsGpuFrustumCullEnabled() const { return mGpuFrustumCull; }

    void SetGpuOcclusionEnabled(bool enabled) { mGpuOcclusion = enabled; }
    bool IsGpuOcclusionEnabled() const { return mGpuOcclusion; }

    // Step F2: GPU integrates velocity/gravity into TRS (Path3)
    void SetGpuMotionEnabled(bool enabled);
    bool IsGpuMotionEnabled() const { return mGpuMotionEnabled; }
    // True when Path3 will run GPU motion this frame (CPU gravity should skip)
    bool ShouldSkipCpuGravity() const;
    // Gravity add/remove/enable toggle — reseed motion slot without full rebuild
    void NotifyEntityMotionChanged(World& world, Entity e);

    void SetFrameDeltaTime(float dt) { mFrameDeltaTime = dt; }

    // Step H: distance LOD + far cull
    void SetLodEnabled(bool enabled);
    bool IsLodEnabled() const { return mLodEnabled; }
    void SetLodDistanceCullEnabled(bool enabled) { mLodDistanceCull = enabled; }
    bool IsLodDistanceCullEnabled() const { return mLodDistanceCull; }
    void SetLodBias(float bias) { mLodBias = (bias > 0.05f) ? bias : 0.05f; }
    float GetLodBias() const { return mLodBias; }
    void SetLodCullDistance(float d) { mLodCullDistance = (d > 1.f) ? d : 1.f; }
    float GetLodCullDistance() const { return mLodCullDistance; }

    // Toon inverted-hull second pass (ignored when style is Realistic)
    void SetOutlinePassEnabled(bool enabled) { mOutlinePassEnabled = enabled; }
    bool IsOutlinePassEnabled() const { return mOutlinePassEnabled; }

    void SetShadowsEnabled(bool enabled) { mShadowsEnabled = enabled; }
    bool IsShadowsEnabled() const { return mShadowsEnabled; }

    // Directional shadow map depth pass (before Scene RT is bound).
    void RenderShadowMap(
        World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    void InvalidateDrawCache();

    const GpuDrivenFrameStats& GetLastFrameStats() const { return mLastStats; }

    void BuildHiZ(
        ID3D12GraphicsCommandList* cmdList,
        DescriptorAllocator* descriptorAllocator,
        ID3D12Resource* sceneDepth,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSrvCpu,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvGpu,
        UINT width, UINT height);

    // Scene RT 리사이즈 직후(GPU idle / Flush 이후)에만 호출
    void PrepareHiZForSceneSize(DescriptorAllocator* alloc, UINT width, UINT height);

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
        // Staging (UPLOAD) — Path2 instances / Path3 transform TRS
        ComPtr<ID3D12Resource> requestUpload;
        ComPtr<ID3D12Resource> transformUpload; // Step F1 GpuTransform[]
        ComPtr<ID3D12Resource> motionUpload;    // Step F2 GpuMotion[]
        ComPtr<ID3D12Resource> batchUpload;
        ComPtr<ID3D12Resource> submeshUpload;
        ComPtr<ID3D12Resource> frameCBUpload;

        // Step C/F: GPU-resident DEFAULT
        ComPtr<ID3D12Resource> transformDefault; // TRS (UAV for F2, SRV for Compose)
        ComPtr<ID3D12Resource> motionDefault;    // Step F2 motion UAV
        ComPtr<ID3D12Resource> sourceDefault;    // ComposeWorld output / Cull input
        ComPtr<ID3D12Resource> batchDefault;
        ComPtr<ID3D12Resource> submeshDefault;

        ComPtr<ID3D12Resource> instanceBuffer;
        ComPtr<ID3D12Resource> countBuffer;
        ComPtr<ID3D12Resource> drawCmdBuffer;
        // Step I: GPU→CPU batch counter readback (READBACK heap)
        ComPtr<ID3D12Resource> countReadback;
        bool countReadbackPending = false; // copy scheduled last use of this slot
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
        // Step E: DescriptorAllocator heap index (bindless)
        std::vector<UINT> materialIndices;
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

    void EnsureGpuResources(ID3D12Device* device);
    void DestroyGpuResources();
    void ExtractFrustumPlanes(const DirectX::XMMATRIX& viewProj, float outPlanes[6][4]);
    static UINT FrameCBAlignedSize();

    void RebuildGpuDrivenScene(World& world);
    void PatchGpuDrivenTransforms(World& world, FrameResource* frameResource);
    // Compare ECS Gravity vs mMotionCpu; reseed + bump version on change
    void SyncGpuMotionFromWorld(World& world);
    // Before TRS upload: refresh all slots from live ECS (collision/editor sync)
    void PullGpuTransformsFromWorld(World& world);
    // Returns true if TRS was uploaded this call (skip GPU motion same frame — avoid +1dt drift)
    bool UploadGpuDrivenFrameData(ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame);
    // Returns true if any instance was integrated (forces recompose)
    bool DispatchUpdateMotion(ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame, UINT numInstances);
    void DispatchComposeWorld(ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame, UINT numInstances);

    void DrawInstancedBatches(
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        FrameGpuResources& frame,
        DescriptorAllocator* descriptorAllocator,
        const std::vector<Entity>& overrideEntities,
        World& world);

    bool PatchOneGpuEntity(World& world, FrameResource* frameResource, Entity e,
        bool& anyPatched, bool& motionPatched);

    struct CachedInstancedBatch
    {
        Mesh* mesh = nullptr;
        Material* mainMaterial = nullptr;
        std::vector<InstanceWorld> instances;
        // Parallel to mesh DrawArgs order used at cache build (bindless indices)
        std::vector<UINT> materialIndices;
    };

    void EnsureHiZResources(DescriptorAllocator* alloc, UINT width, UINT height);
    void EnsureDummyHiZSrv(DescriptorAllocator* alloc);
    void DestroyHiZResources(DescriptorAllocator* alloc);
    bool IsHiZSampleReady() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetHiZSampleSrvGpu() const;
    void UpdateAutoRenderPath(World& world);
    void UpdateEntityLods(World& world, const DirectX::XMMATRIX& viewMatrix);
    // Step I: map previous cull counters on this frame slot
    void ResolveGpuCullReadback(FrameGpuResources& frame);
    void ScheduleGpuCullReadback(
        ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame,
        UINT numBatches, UINT numSources);

    RenderPath mRenderPath = RenderPath::ComputeIndirect;
    bool mAutoRenderPath = true; // Step G 기본 ON
    bool mGpuFrustumCull = true;
    bool mGpuOcclusion = true;
    bool mGpuMotionEnabled = true; // Step F2 default ON for Path3
    bool mLodEnabled = true;       // Step H default ON
    bool mLodDistanceCull = true;
    bool mOutlinePassEnabled = false; // Toon inverted-hull second pass
    bool mShadowsEnabled = true;
    static constexpr UINT kShadowMapSize = 2048;
    Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mShadowDsvHeap;
    DescriptorAllocator::DescriptorHandle mShadowSrv{};
    D3D12_RESOURCE_STATES mShadowState = D3D12_RESOURCE_STATE_COMMON;

    void EnsureShadowMap(DescriptorAllocator* alloc);
    void DestroyShadowMap(DescriptorAllocator* alloc);
    void BindShadowMapSrv(ID3D12GraphicsCommandList* cmdList) const;
    float mLodBias = 1.f;
    float mLodCullDistance = 250.f; // global override if > 0 applied as max with component
    float mFrameDeltaTime = 1.f / 60.f;
    uint32_t mMotionActiveCount = 0;
    // preserved across path-local mLastStats = {}
    bool mLodStatEnabled = false;
    uint32_t mLodStatCulled = 0;
    uint32_t mLodStatSwitches = 0;
    uint32_t mLodStatLevels[4]{};
    float mLodStatMs = 0.f;
    ID3D12Device* mDevice = nullptr;
    DescriptorAllocator* mSrvAlloc = nullptr;

    ComPtr<ID3D12PipelineState> mCullCompactPSO;
    ComPtr<ID3D12PipelineState> mBuildCommandsPSO;
    ComPtr<ID3D12PipelineState> mComposeWorldPSO;
    ComPtr<ID3D12PipelineState> mUpdateMotionPSO;
    ComPtr<ID3D12PipelineState> mHiZCopyPSO;
    ComPtr<ID3D12PipelineState> mHiZDownsamplePSO;
    ComPtr<ID3D12Resource> mCounterZeroUpload;
    ComPtr<ID3D12Resource> mDummyInstanceBuffer;
    ComPtr<ID3D12Resource> mDummyHiZTexture; // 1x1 far depth
    DescriptorAllocator::DescriptorHandle mDummyHiZSrv{};

    // Hierarchical-Z: 프레임 지연(triple buffer) — 읽기/쓰기 충돌 방지
    // 증상: 시작 시 occlusion=true → TDR, 런타임 토글은 정상
    static constexpr UINT kHiZRingSize = kIndirectFrameCount; // 3
    struct HiZSlot
    {
        ComPtr<ID3D12Resource> texture;
        DescriptorAllocator::DescriptorHandle srv{};
        DescriptorAllocator::DescriptorHandle uav{};
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };
    HiZSlot mHiZRing[kHiZRingSize]{};
    UINT mHiZWriteSlot = 0;   // next BuildHiZ target
    UINT mHiZBuildCount = 0;  // total successful builds
    UINT mHiZWidth = 0;
    UINT mHiZHeight = 0;
    UINT mHiZMipCount = 1;
    XMMATRIX mLastViewProj = DirectX::XMMatrixIdentity();
    float mLastNear = 0.1f;
    float mLastFar = 1000.f;
    UINT mLastRtWidth = 1;
    UINT mLastRtHeight = 1;

    FrameGpuResources mFrames[kIndirectFrameCount]{};
    bool mComputeIndirectReady = false;

    bool mInstancedCacheValid = false;
    std::vector<CachedInstancedBatch> mCachedInstancedBatches;
    std::vector<Entity> mCachedOverrideEntities;
    uint32_t mPath2CachedDirtyGen = 0;

    bool mGpuStructureDirty = true;
    uint32_t mTransformContentVersion = 1; // TRS + world source content
    uint32_t mMotionContentVersion = 1;    // Step F2 motion seed
    uint32_t mMetaVersion = 1;
    std::vector<GpuTransform> mTransformCpu;      // Step F1 TRS mirror
    std::vector<GpuMotion> mMotionCpu;            // Step F2 motion seed
    std::vector<GpuInstanceSource> mSourceCpu;    // CPU fallback / seed world
    std::vector<GpuBatchDesc> mBatchDescsCpu;
    std::vector<GpuSubmeshDesc> mSubmeshDescsCpu;
    std::vector<GpuCpuBatch> mGpuCpuBatches;
    std::vector<Entity> mGpuOverrideEntities;
    std::unordered_map<Entity, UINT> mEntityToGpuSlot;

    // Step B: multi-frame dirtyFrames 유지용
    std::vector<Entity> mPendingTransformDirty;
    uint32_t mLastProcessedDirtyGen = 0;

    GpuDrivenFrameStats mLastStats{};
};

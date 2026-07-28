#include <DirectXMath.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <vector>
#include <unordered_set>
#include <cstring>
#include "RenderSystem.h"
#include "RenderDrawHelpers.h"
#include "TransformDirtyTracker.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "ShaderManager.h"
#include "constantStruct.h"
#include "d3dUtil.h"
#include "d3dx12.h"

using namespace DirectX;
using namespace RenderDrawHelpers;

// =============================================================================
// SUB-LOGIC — multi-step stages for paths / shadow / LOD
// Called only from MAIN orchestrators (not public API)
// =============================================================================

void RenderSystem::UpdateEntityLods(World& world, const XMMATRIX& viewMatrix)
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    mLastStats.lodEnabled = mLodEnabled;
    mLastStats.lodCulled = 0;
    mLastStats.lodSwitches = 0;
    mLastStats.lodLevelCounts[0] = mLastStats.lodLevelCounts[1] =
        mLastStats.lodLevelCounts[2] = mLastStats.lodLevelCounts[3] = 0;

    XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
    XMFLOAT3 eye{};
    XMStoreFloat3(&eye, invView.r[3]);

    const float bias = mLodBias;
    bool anyChange = false;

    world.ForEach<TransformComponent, RenderableComponent, LodComponent>(
        [&](Entity, TransformComponent& tf, RenderableComponent& rend, LodComponent& lod)
        {
            if (lod.levelCount <= 0 || !lod.levels[0])
            {
                if (rend.mesh)
                    lod.levels[0] = rend.mesh;
                lod.levelCount = lod.levels[0] ? 1 : 0;
            }
            if (lod.levelCount <= 0)
                return;

            if (!mLodEnabled)
            {
                if (lod.culled || lod.currentLevel != 0 || rend.mesh != lod.levels[0])
                {
                    lod.culled = false;
                    lod.currentLevel = 0;
                    rend.mesh = lod.levels[0];
                    anyChange = true;
                }
                ++mLastStats.lodLevelCounts[0];
                return;
            }

            const float dx = tf.position.x - eye.x;
            const float dy = tf.position.y - eye.y;
            const float dz = tf.position.z - eye.z;
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);

            int level = 0;
            if (lod.forcedLevel >= 0)
            {
                level = lod.forcedLevel;
                if (level >= lod.levelCount)
                    level = lod.levelCount - 1;
            }
            else
            {
                for (int i = 0; i < lod.levelCount - 1; ++i)
                {
                    if (dist > lod.thresholds[i] * bias)
                        level = i + 1;
                }
            }

            const float cullDist = (std::min)(lod.cullDistance, mLodCullDistance) * bias;
            const bool culled = mLodDistanceCull && (dist > cullDist);

            Mesh* want = lod.levels[level] ? lod.levels[level] : lod.levels[0];
            if (!want)
                want = rend.mesh;

            if (lod.currentLevel != level || lod.culled != culled || (!culled && rend.mesh != want))
            {
                lod.currentLevel = level;
                lod.culled = culled;
                if (!culled && want)
                    rend.mesh = want;
                else if (lod.levels[0])
                    rend.mesh = lod.levels[0]; // keep valid pointer while culled
                ++mLastStats.lodSwitches;
                anyChange = true;
            }

            if (culled)
                ++mLastStats.lodCulled;
            else if (level >= 0 && level < 4)
                ++mLastStats.lodLevelCounts[level];
        });

    if (anyChange)
    {
        mGpuStructureDirty = true;
        mInstancedCacheValid = false;
    }
    mLastStats.lodMs = ElapsedMs(t0);

    mLodStatEnabled = mLastStats.lodEnabled;
    mLodStatCulled = mLastStats.lodCulled;
    mLodStatSwitches = mLastStats.lodSwitches;
    mLodStatMs = mLastStats.lodMs;
    for (int i = 0; i < 4; ++i)
        mLodStatLevels[i] = mLastStats.lodLevelCounts[i];
}

void RenderSystem::UpdateAutoRenderPath(World& world)
{
    if (!mAutoRenderPath)
        return;

    size_t n = 0;
    world.ForEach<RenderableComponent>(
        [&](Entity, RenderableComponent&)
        {
            ++n;
        });

    // Step G thresholds (기능 ?��? + ?�???�에??Path3)
    //  < 32  : Instanced (?�버?�드 ?�음, ?�정)
    //  >= 32 : ComputeIndirect (가?�하�?
    RenderPath chosen = RenderPath::Instanced;
    if (n >= 32 && mComputeIndirectReady)
        chosen = RenderPath::ComputeIndirect;
    else if (n > 0 && n < 4)
        chosen = RenderPath::Instanced; // Basic?� ?�버그용?�로�??�동 ?�용

    if (chosen != mRenderPath)
    {
        if (chosen == RenderPath::ComputeIndirect)
            mGpuStructureDirty = true;
        mRenderPath = chosen;
        char buf[128];
        sprintf_s(buf, "[RenderSystem] Auto path -> %s (objects=%zu)\n",
            chosen == RenderPath::ComputeIndirect ? "ComputeIndirect" : "Instanced", n);
        OutputDebugStringA(buf);
    }
}

void RenderSystem::BeginGpuDrivenFrameStats()
{
    mLastStats = {};
    RestoreLodStats(mLastStats, mLodStatEnabled, mLodStatCulled, mLodStatSwitches,
        mLodStatLevels, mLodStatMs);
    mLastStats.path = RenderPath::ComputeIndirect;
    mLastStats.autoPath = mAutoRenderPath;
    mLastStats.cullEnabled = mGpuFrustumCull;
    mLastStats.occlusionEnabled = mGpuOcclusion;
    mLastStats.hizValid = IsHiZSampleReady();
    mLastStats.hizMips = mHiZMipCount;
    mLastStats.eiCalls = 0;
    mLastStats.multiEiRuns = 0;
}

void RenderSystem::UpdateGpuDrivenSceneData(World& world, FrameResource* currentFrameResource)
{
    if (mGpuStructureDirty)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        RebuildGpuDrivenScene(world);
        mLastStats.didRebuild = true;
        mLastStats.rebuildMs = ElapsedMs(t0);
        TransformDirtyTracker::ClearEntities();
        mPendingTransformDirty.clear();
        for (const auto& kv : mEntityToGpuSlot)
        {
            if (auto* tf = world.GetComponent<TransformComponent>(kv.first))
            {
                if (tf->dirtyFrames > 0)
                    mPendingTransformDirty.push_back(kv.first);
            }
        }
        mLastProcessedDirtyGen = TransformDirtyTracker::Generation();
    }
    PatchGpuDrivenTransforms(world, currentFrameResource);
    SyncGpuMotionFromWorld(world);

    mLastStats.sourceCount = static_cast<uint32_t>(mSourceCpu.size());
    mLastStats.batchCount = static_cast<uint32_t>(mBatchDescsCpu.size());
    mLastStats.submeshDraws = static_cast<uint32_t>(mSubmeshDescsCpu.size());
}

void RenderSystem::DrawMaterialOverrideEntities(
    World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    ID3D12RootSignature* sceneRS)
{
    if (!sceneRS || mGpuOverrideEntities.empty())
        return;

    cmdList->SetGraphicsRootSignature(sceneRS);
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }
    const PSOKey basicKey = MakeOpaquePsoKey("object_cb");
    if (ID3D12PipelineState* basicPso = GetScenePso(basicKey, sceneRS))
        cmdList->SetPipelineState(basicPso);
    if (mDummyInstanceBuffer)
        cmdList->SetGraphicsRootShaderResourceView(3, mDummyInstanceBuffer->GetGPUVirtualAddress());

    for (Entity e : mGpuOverrideEntities)
    {
        auto* tf = world.GetComponent<TransformComponent>(e);
        auto* rend = world.GetComponent<RenderableComponent>(e);
        if (tf && rend)
            DrawEntityWithResolvedMaterials(cmdList, currentFrameResource, e, *tf, *rend);
    }
}

UINT RenderSystem::PrepareGpuDrivenMotionAndCompose(
    ID3D12GraphicsCommandList* cmdList,
    FrameGpuResources& frame,
    int currentFrameIndex,
    World& world)
{
    // Before TRS upload: refresh CPU mirror for motion-active slots so seed stays in sync.
    {
        const int frameIdx = ((currentFrameIndex % (int)kIndirectFrameCount) + (int)kIndirectFrameCount)
            % (int)kIndirectFrameCount;
        FrameGpuResources& fr = mFrames[frameIdx];
        if (fr.uploadedTransformVersion != mTransformContentVersion && mMotionActiveCount > 0)
            PullGpuTransformsFromWorld(world);
    }

    const bool didTrsUpload = UploadGpuDrivenFrameData(cmdList, frame);

    const UINT numXforms = static_cast<UINT>((std::min)(
        (std::min)(mTransformCpu.size(), mSourceCpu.size()),
        static_cast<size_t>(kMaxInstancesPerDraw)));

    // Skip motion on the same frame as TRS upload to avoid +1dt drift vs CPU mirror.
    if (!didTrsUpload && DispatchUpdateMotion(cmdList, frame, numXforms))
        frame.composedTransformVersion = 0;
    else if (didTrsUpload)
        frame.composedTransformVersion = 0;

    DispatchComposeWorld(cmdList, frame, numXforms);
    return numXforms;
}

void RenderSystem::FillGpuDrivenCullConstants(
    FrameGpuResources& frame,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix,
    UINT numInstances)
{
    const XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
    mLastViewProj = viewProj;
    mLastRtWidth = (mHiZWidth > 0) ? mHiZWidth : 1;
    mLastRtHeight = (mHiZHeight > 0) ? mHiZHeight : 1;

    GpuDrivenFrameConstants cb{};
    ExtractFrustumPlanes(viewProj, cb.frustumPlanes);
    cb.numInstances = numInstances;
    cb.enableFrustumCull = mGpuFrustumCull ? 1u : 0u;
    cb.numBatches = static_cast<uint32_t>(mBatchDescsCpu.size());
    cb.numSubmeshDraws = static_cast<uint32_t>(mSubmeshDescsCpu.size());
    cb.maxInstances = kMaxInstancesPerDraw;
    const bool hizReady = IsHiZSampleReady();
    cb.enableOcclusion = (mGpuOcclusion && hizReady) ? 1u : 0u;
    cb.hizMipCount = mHiZMipCount;
    cb.hizValid = hizReady ? 1u : 0u;
    {
        XMFLOAT4X4 vpT;
        XMStoreFloat4x4(&vpT, XMMatrixTranspose(viewProj));
        std::memcpy(cb.viewProj, &vpT, sizeof(vpT));
    }
    cb.rtWidth = static_cast<float>((std::max)(1u, mLastRtWidth));
    cb.rtHeight = static_cast<float>((std::max)(1u, mLastRtHeight));
    cb.zNear = mLastNear;
    cb.zFar = mLastFar;
    std::memcpy(frame.frameCBMapped, &cb, sizeof(cb));
}

bool RenderSystem::DispatchGpuCullAndBuildCommands(
    ID3D12GraphicsCommandList* cmdList,
    FrameGpuResources& frame,
    DescriptorAllocator* descriptorAllocator,
    UINT numInstances)
{
    ID3D12RootSignature* buildRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::IndirectBuild);
    if (!buildRS || !mCullCompactPSO || !mBuildCommandsPSO)
        return false;

    // 1) Clear per-batch visible counters
    if (frame.countState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            frame.countBuffer.Get(), frame.countState, D3D12_RESOURCE_STATE_COPY_DEST));
        frame.countState = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    const UINT64 counterBytes = sizeof(UINT) * kMaxGpuBatches;
    cmdList->CopyBufferRegion(
        frame.countBuffer.Get(), 0,
        mCounterZeroUpload.Get(), 0,
        counterBytes);

    // 2) Transition UAVs for cull/build
    {
        D3D12_RESOURCE_BARRIER b[3];
        UINT nb = 0;
        auto toUav = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& st)
        {
            if (st != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                b[nb++] = CD3DX12_RESOURCE_BARRIER::Transition(
                    res, st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                st = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
        };
        toUav(frame.countBuffer.Get(), frame.countState);
        toUav(frame.instanceBuffer.Get(), frame.instanceState);
        toUav(frame.drawCmdBuffer.Get(), frame.drawCmdState);
        if (nb) cmdList->ResourceBarrier(nb, b);
    }

    // 3) Bind compute roots: frame CB, source/batch/submesh, Hi-Z, UAVs
    cmdList->SetComputeRootSignature(buildRS);
    cmdList->SetComputeRootConstantBufferView(0, frame.frameCBUpload->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(1, frame.sourceDefault->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(2, frame.batchDefault->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(3, frame.submeshDefault->GetGPUVirtualAddress());

    EnsureDummyHiZSrv(descriptorAllocator);
    D3D12_GPU_DESCRIPTOR_HANDLE hizGpu = GetHiZSampleSrvGpu();
    if (hizGpu.ptr == 0)
        hizGpu = mDummyHiZSrv.GPU;
    if (hizGpu.ptr == 0)
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            OutputDebugStringA("[RenderSystem] No valid HiZ SRV; skip GPU-driven frame.\n");
            sLogged = true;
        }
        return false;
    }
    cmdList->SetComputeRootDescriptorTable(4, hizGpu);
    cmdList->SetComputeRootUnorderedAccessView(5, frame.instanceBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(6, frame.countBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(7, frame.drawCmdBuffer->GetGPUVirtualAddress());

    // 4) Cull + compact visible instances into instanceBuffer
    cmdList->SetPipelineState(mCullCompactPSO.Get());
    if (numInstances == 0)
        return false;
    cmdList->Dispatch((numInstances + 63u) / 64u, 1, 1);

    {
        D3D12_RESOURCE_BARRIER uav[2];
        uav[0] = CD3DX12_RESOURCE_BARRIER::UAV(frame.instanceBuffer.Get());
        uav[1] = CD3DX12_RESOURCE_BARRIER::UAV(frame.countBuffer.Get());
        cmdList->ResourceBarrier(2, uav);
    }

    // 5) Build IndirectCommand buffer for ExecuteIndirect
    cmdList->SetPipelineState(mBuildCommandsPSO.Get());
    const UINT numSubs = static_cast<UINT>((std::min)(
        mSubmeshDescsCpu.size(), static_cast<size_t>(kMaxGpuSubmeshDraws)));
    if (numSubs == 0)
        return false;
    cmdList->Dispatch((numSubs + 63u) / 64u, 1, 1);

    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::Transition(
            frame.drawCmdBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            frame.instanceBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(2, b);
        frame.drawCmdState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        frame.instanceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    // 6) Schedule GPU->CPU counter readback (resolved next time this ring slot is used)
    ScheduleGpuCullReadback(
        cmdList, frame,
        static_cast<UINT>(mBatchDescsCpu.size()),
        numInstances);
    return true;
}

void RenderSystem::ExecuteIndirectOpaquePass(
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    FrameGpuResources& frame,
    DescriptorAllocator* descriptorAllocator,
    ID3D12RootSignature* sceneRS,
    ID3D12PipelineState* gfxPSO,
    ID3D12CommandSignature* cmdSig)
{
    cmdList->SetGraphicsRootSignature(sceneRS);
    cmdList->SetPipelineState(gfxPSO);
    cmdList->OMSetStencilRef(1);
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }
    BindShadowMapSrv(cmdList);

    UINT lastMat = UINT_MAX;
    for (const GpuCpuBatch& batch : mGpuCpuBatches)
    {
        if (!batch.mesh || batch.instanceCount == 0 || batch.submeshCount == 0)
            continue;

        const D3D12_GPU_VIRTUAL_ADDRESS instVA =
            frame.instanceBuffer->GetGPUVirtualAddress()
            + (UINT64)batch.firstInstance * sizeof(InstanceWorld);
        cmdList->SetGraphicsRootShaderResourceView(3, instVA);

        auto vbv = batch.mesh->VertexBufferView();
        auto ibv = batch.mesh->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Merge consecutive submeshes that share the same material into one EI call.
        UINT si = 0;
        while (si < batch.submeshCount)
        {
            if (batch.firstSubmesh + si >= mSubmeshDescsCpu.size()
                || si >= batch.materialIndices.size())
                break;

            const UINT mat = batch.materialIndices[si];
            const UINT runStart = si;
            while (si < batch.submeshCount
                && si < batch.materialIndices.size()
                && batch.materialIndices[si] == mat)
            {
                ++si;
            }
            const UINT runLen = si - runStart;
            if (runLen == 0)
                break;

            BindMaterialByIndex(cmdList, descriptorAllocator, mat, lastMat);

            const UINT cmdIndex = batch.firstSubmesh + runStart;
            const UINT64 argOffset = (UINT64)cmdIndex * sizeof(IndirectCommand);
            cmdList->ExecuteIndirect(
                cmdSig, runLen,
                frame.drawCmdBuffer.Get(), argOffset,
                nullptr, 0);

            ++mLastStats.eiCalls;
            if (runLen > 1)
                ++mLastStats.multiEiRuns;
        }
    }
}

void RenderSystem::DrawGpuDrivenOutlinePass(
    ID3D12GraphicsCommandList* cmdList,
    FrameGpuResources& frame,
    DescriptorAllocator* descriptorAllocator,
    ID3D12RootSignature* sceneRS)
{
    if (!mOutlinePassEnabled || !sceneRS)
        return;

    const PSOKey outlineKey = MakeOutlinePsoKey("object_instanced_outline");
    ID3D12PipelineState* outlinePso = GetScenePso(outlineKey, sceneRS);
    if (!outlinePso)
        return;

    cmdList->SetPipelineState(outlinePso);
    cmdList->OMSetStencilRef(1);
    UINT lastMat = UINT_MAX;

    for (const GpuCpuBatch& batch : mGpuCpuBatches)
    {
        if (!batch.mesh || batch.instanceCount == 0)
            continue;

        const D3D12_GPU_VIRTUAL_ADDRESS instVA =
            frame.instanceBuffer->GetGPUVirtualAddress()
            + (UINT64)batch.firstInstance * sizeof(InstanceWorld);
        cmdList->SetGraphicsRootShaderResourceView(3, instVA);

        if (!batch.materialIndices.empty())
        {
            BindMaterialByIndex(cmdList, descriptorAllocator,
                batch.materialIndices[0], lastMat);
        }
        DrawMeshOutlineIndexed(cmdList, batch.mesh, batch.instanceCount);
    }
}

void RenderSystem::RebuildGpuDrivenScene(World& world)
{
    struct Entry
    {
        Entity entity = INVALID_ENTITY;
        Mesh* mesh = nullptr;
        Material* mainMat = nullptr;
        TransformComponent* tf = nullptr;
        RenderableComponent* rend = nullptr;
        BoundsComponent* bounds = nullptr;
    };

    std::vector<Entry> entries;
    entries.reserve(1024);
    mGpuOverrideEntities.clear();

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (const auto* lod = world.GetComponent<LodComponent>(e); lod && lod->culled)
                return;
            if (HasEntitySubMaterialOverride(e))
            {
                mGpuOverrideEntities.push_back(e);
                return;
            }
            Entry en;
            en.entity = e;
            en.mesh = rend.mesh;
            en.mainMat = GetEntityMainMaterialOverride(e);
            en.tf = &tf;
            en.rend = &rend;
            en.bounds = world.GetComponent<BoundsComponent>(e);
            entries.push_back(en);
        });

    std::stable_sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b)
        {
            if (a.mesh != b.mesh) return a.mesh < b.mesh;
            return a.mainMat < b.mainMat;
        });

    mTransformCpu.clear();
    mMotionCpu.clear();
    mSourceCpu.clear();
    mBatchDescsCpu.clear();
    mSubmeshDescsCpu.clear();
    mGpuCpuBatches.clear();
    mEntityToGpuSlot.clear();
    mMotionActiveCount = 0;

    const size_t cap = (std::min)(entries.size(), static_cast<size_t>(kMaxInstancesPerDraw));
    mTransformCpu.reserve(cap);
    mMotionCpu.reserve(cap);
    mSourceCpu.reserve(cap);

    UINT slot = 0;
    size_t i = 0;
    while (i < entries.size() && slot < kMaxInstancesPerDraw
        && mGpuCpuBatches.size() < kMaxGpuBatches)
    {
        Mesh* mesh = entries[i].mesh;
        Material* mainMat = entries[i].mainMat;
        const size_t groupBegin = i;

        // 같�? (mesh, material) 그룹 범위 ?�정
        while (i < entries.size()
            && entries[i].mesh == mesh
            && entries[i].mainMat == mainMat)
        {
            ++i;
        }
        const size_t groupEnd = i;
        const UINT groupCount = static_cast<UINT>((std::min)(
            groupEnd - groupBegin,
            static_cast<size_t>(kMaxInstancesPerDraw - slot)));
        if (groupCount == 0 || !mesh)
            continue;

        // ?�브메시/머티리얼 먼�? ?�석 ???�효???�만 ?�스 커밋
        std::vector<GpuSubmeshDesc> localSubs;
        std::vector<UINT> localMatIndices;
        for (auto& pair : mesh->DrawArgs)
        {
            if (mSubmeshDescsCpu.size() + localSubs.size() >= kMaxGpuSubmeshDraws)
                break;
            const auto& sub = pair.second;
            if (sub.IndexCount == 0)
                continue;
            Material* material = ResolveBatchMaterial(mainMat, sub);
            if (!material)
                continue;
            GpuSubmeshDesc sd{};
            sd.indexCount = sub.IndexCount;
            sd.startIndexLocation = sub.StartIndexLocation;
            sd.baseVertexLocation = sub.BaseVertexLocation;
            sd.batchId = 0; // ?�래?�서 채�?
            localSubs.push_back(sd);
            localMatIndices.push_back(material->mTextureHandle.Index);
        }
        if (localSubs.empty())
            continue;

        const UINT batchId = static_cast<UINT>(mGpuCpuBatches.size());
        const UINT batchFirst = slot;

        for (UINT k = 0; k < groupCount; ++k)
        {
            const Entry& en = entries[groupBegin + k];
            GpuTransform xf{};
            GpuMotion mot{};
            GpuInstanceSource src{};
            FillTransformFromEntity(xf, *en.tf, *en.rend, en.bounds, batchId);
            FillMotionFromEntity(mot, world.GetComponent<GravityComponent>(en.entity));
            FillSourceFromEntity(src, *en.tf, *en.rend, en.bounds, batchId);
            mEntityToGpuSlot[en.entity] = slot;
            mTransformCpu.push_back(xf);
            mMotionCpu.push_back(mot);
            mSourceCpu.push_back(src);
            if (mot.flags & 1u)
                ++mMotionActiveCount;
            ++slot;
        }

        GpuCpuBatch cpuBatch{};
        cpuBatch.mesh = mesh;
        cpuBatch.mainMaterial = mainMat;
        cpuBatch.firstInstance = batchFirst;
        cpuBatch.instanceCount = groupCount;
        cpuBatch.firstSubmesh = static_cast<UINT>(mSubmeshDescsCpu.size());
        cpuBatch.submeshCount = static_cast<UINT>(localSubs.size());
        cpuBatch.materialIndices = std::move(localMatIndices);

        for (auto& sd : localSubs)
        {
            sd.batchId = batchId;
            mSubmeshDescsCpu.push_back(sd);
        }

        GpuBatchDesc bd{};
        bd.firstInstance = cpuBatch.firstInstance;
        bd.instanceCount = cpuBatch.instanceCount;
        bd.firstSubmesh = cpuBatch.firstSubmesh;
        bd.submeshCount = cpuBatch.submeshCount;
        mBatchDescsCpu.push_back(bd);
        mGpuCpuBatches.push_back(std::move(cpuBatch));
    }

    ++mTransformContentVersion;
    ++mMotionContentVersion;
    ++mMetaVersion;
    mGpuStructureDirty = false;
}

void RenderSystem::PatchGpuDrivenTransforms(World& world, FrameResource* frameResource)
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    mLastStats.skippedPatch = false;
    mLastStats.usedDirtyList = false;
    mLastStats.dirtyPatched = 0;
    mLastStats.dirtyListIn = 0;

    const uint32_t gen = TransformDirtyTracker::Generation();
    auto incoming = TransformDirtyTracker::TakeEntities();
    mLastStats.dirtyListIn = static_cast<uint32_t>(incoming.size());

    if (!incoming.empty())
    {
        mPendingTransformDirty.insert(
            mPendingTransformDirty.end(), incoming.begin(), incoming.end());
        mLastStats.usedDirtyList = true;
    }

    const bool genChanged = (gen != mLastProcessedDirtyGen);
    const bool hasPending = !mPendingTransformDirty.empty();

    if (!genChanged && !hasPending)
    {
        mLastStats.skippedPatch = true;
        mLastStats.patchMs = ElapsedMs(t0);
        mLastStats.pendingDirty = 0;
        return;
    }

    // MarkDirty() without entity ??generation only: full scan once
    if (genChanged && mPendingTransformDirty.empty())
    {
        world.ForEach<TransformComponent, RenderableComponent>(
            [&](Entity e, TransformComponent& tf, RenderableComponent&)
            {
                if (tf.dirtyFrames > 0)
                    mPendingTransformDirty.push_back(e);
            });
        mLastStats.usedDirtyList = false;
    }

    if (mPendingTransformDirty.size() > 1)
    {
        std::sort(mPendingTransformDirty.begin(), mPendingTransformDirty.end());
        mPendingTransformDirty.erase(
            std::unique(mPendingTransformDirty.begin(), mPendingTransformDirty.end()),
            mPendingTransformDirty.end());
    }

    bool anyPatched = false;
    bool motionPatched = false;
    std::vector<Entity> stillPending;
    stillPending.reserve(mPendingTransformDirty.size());
    uint32_t patched = 0;

    for (Entity e : mPendingTransformDirty)
    {
        bool touched = false;
        bool motTouched = false;
        const bool keep = PatchOneGpuEntity(world, frameResource, e, touched, motTouched);
        if (touched)
        {
            anyPatched = true;
            ++patched;
        }
        if (motTouched)
            motionPatched = true;
        if (keep)
            stillPending.push_back(e);
    }

    mPendingTransformDirty = std::move(stillPending);
    mLastProcessedDirtyGen = gen;
    mLastStats.dirtyPatched = patched;
    mLastStats.pendingDirty = static_cast<uint32_t>(mPendingTransformDirty.size());

    if (anyPatched)
        ++mTransformContentVersion;
    if (motionPatched)
        ++mMotionContentVersion;

    mLastStats.patchMs = ElapsedMs(t0);
}

bool RenderSystem::PatchOneGpuEntity(
    World& world, FrameResource* frameResource, Entity e,
    bool& anyPatched, bool& motionPatched)
{
    auto* tf = world.GetComponent<TransformComponent>(e);
    auto* rend = world.GetComponent<RenderableComponent>(e);
    if (!tf || !rend || !rend->mesh)
        return false;

    auto it = mEntityToGpuSlot.find(e);
    if (it == mEntityToGpuSlot.end())
        return false;

    const UINT slot = it->second;
    if (slot >= mTransformCpu.size() || slot >= mSourceCpu.size() || slot >= mMotionCpu.size())
        return false;

    if (tf->dirtyFrames <= 0)
    {
        const uint32_t want = rend->visible ? 1u : 0u;
        if ((mTransformCpu[slot].flags & 1u) != want)
        {
            mTransformCpu[slot].flags = want;
            mSourceCpu[slot].flags = want;
            anyPatched = true;
        }
        return false; // no longer pending
    }

    const bool gpuOwnsMotion = mGpuMotionEnabled && mUpdateMotionPSO
        && (mMotionCpu[slot].flags & 1u) != 0;
    // Gravity 미러 dirty: CPU/bounds�?갱신, GPU TRS·velocity ??��?��? ?�음
    const bool skipGpuTrs = gpuOwnsMotion && tf->suppressGpuUpload;

    BoundsComponent* bounds = world.GetComponent<BoundsComponent>(e);
    const UINT batchId = mTransformCpu[slot].batchId;
    GravityComponent* gravity = world.GetComponent<GravityComponent>(e);

    if (!skipGpuTrs)
    {
        FillTransformFromEntity(mTransformCpu[slot], *tf, *rend, bounds, batchId);
        FillSourceFromEntity(mSourceCpu[slot], *tf, *rend, bounds, batchId);
        // ?�디???�동·충돌 보정: motion??CPU ?�태�??�시??        FillMotionFromEntity(mMotionCpu[slot], gravity);
        motionPatched = true;
        anyPatched = true;
    }
    else
    {
        // visibility flag only on CPU mirror path
        mTransformCpu[slot].flags = rend->visible ? 1u : 0u;
        mSourceCpu[slot].flags = mTransformCpu[slot].flags;
    }

    if (frameResource && frameResource->ObjectCB
        && rend->objectCBIndex < kMaxSceneObjects)
    {
        ObjectConstants objConst{};
        XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf->GetWorldMatrix()));
        frameResource->ObjectCB->CopyData(static_cast<int>(rend->objectCBIndex), objConst);
    }

    --tf->dirtyFrames;
    if (tf->dirtyFrames <= 0)
        tf->suppressGpuUpload = false;
    return tf->dirtyFrames > 0;
}

void RenderSystem::SyncGpuMotionFromWorld(World& world)
{
    if (mMotionCpu.empty() || mEntityToGpuSlot.empty())
    {
        mMotionActiveCount = 0;
        return;
    }

    bool anyChange = false;
    uint32_t active = 0;

    for (const auto& kv : mEntityToGpuSlot)
    {
        const UINT slot = kv.second;
        if (slot >= mMotionCpu.size())
            continue;

        GpuMotion want{};
        FillMotionFromEntity(want, world.GetComponent<GravityComponent>(kv.first));
        GpuMotion& cur = mMotionCpu[slot];

        const bool wantOn = (want.flags & 1u) != 0;
        const bool curOn = (cur.flags & 1u) != 0;

        // Enable/disable/strength/? change ??reseed from ECS
        // Stay-on with same params: GPU owns integrated velocity (do not overwrite)
        const bool paramChanged = wantOn && curOn
            && (want.gravity != cur.gravity
                || want.angularVelocity[0] != cur.angularVelocity[0]
                || want.angularVelocity[1] != cur.angularVelocity[1]
                || want.angularVelocity[2] != cur.angularVelocity[2]);

        if (wantOn != curOn || paramChanged)
        {
            cur = want;
            anyChange = true;
        }

        if (cur.flags & 1u)
            ++active;
    }

    mMotionActiveCount = active;
    if (anyChange)
        ++mMotionContentVersion;
}

void RenderSystem::PullGpuTransformsFromWorld(World& world)
{
    for (const auto& kv : mEntityToGpuSlot)
    {
        const UINT slot = kv.second;
        if (slot >= mTransformCpu.size() || slot >= mSourceCpu.size())
            continue;
        auto* tf = world.GetComponent<TransformComponent>(kv.first);
        auto* rend = world.GetComponent<RenderableComponent>(kv.first);
        if (!tf || !rend)
            continue;
        BoundsComponent* bounds = world.GetComponent<BoundsComponent>(kv.first);
        const UINT batchId = mTransformCpu[slot].batchId;
        FillTransformFromEntity(mTransformCpu[slot], *tf, *rend, bounds, batchId);
        FillSourceFromEntity(mSourceCpu[slot], *tf, *rend, bounds, batchId);
    }
}

bool RenderSystem::UploadGpuDrivenFrameData(
    ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame)
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    mLastStats.didSourceUpload = false;
    mLastStats.didMetaUpload = false;
    mLastStats.usedDefaultHeapCopy = false;
    bool didTransformUpload = false;

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& st, D3D12_RESOURCE_STATES target)
    {
        if (st != target)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(res, st, target));
            st = target;
        }
    };

    // Step F1: TRS staging ??DEFAULT (compose / motion input)
    if (frame.uploadedTransformVersion != mTransformContentVersion && frame.transformMapped)
    {
        const UINT64 bytes = sizeof(GpuTransform) * mTransformCpu.size();
        if (!mTransformCpu.empty() && bytes > 0)
        {
            std::memcpy(frame.transformMapped, mTransformCpu.data(), static_cast<size_t>(bytes));
            transition(frame.transformDefault.Get(), frame.transformDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                frame.transformDefault.Get(), 0,
                frame.transformUpload.Get(), 0,
                bytes);
            transition(frame.transformDefault.Get(), frame.transformDefaultState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            mLastStats.usedDefaultHeapCopy = true;
            didTransformUpload = true;
        }
        frame.uploadedTransformVersion = mTransformContentVersion;
        mLastStats.didSourceUpload = true; // TRS upload (legacy stat name)
    }
    else
    {
        transition(frame.transformDefault.Get(), frame.transformDefaultState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Step F2: motion seed ??DEFAULT (only on version change; GPU owns velocity after)
    if (frame.uploadedMotionVersion != mMotionContentVersion && frame.motionMapped && frame.motionDefault)
    {
        const UINT64 bytes = sizeof(GpuMotion) * mMotionCpu.size();
        if (!mMotionCpu.empty() && bytes > 0)
        {
            std::memcpy(frame.motionMapped, mMotionCpu.data(), static_cast<size_t>(bytes));
            transition(frame.motionDefault.Get(), frame.motionDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                frame.motionDefault.Get(), 0,
                frame.motionUpload.Get(), 0,
                bytes);
            transition(frame.motionDefault.Get(), frame.motionDefaultState,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            mLastStats.usedDefaultHeapCopy = true;
        }
        frame.uploadedMotionVersion = mMotionContentVersion;
    }
    else if (frame.motionDefault)
    {
        transition(frame.motionDefault.Get(), frame.motionDefaultState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (frame.uploadedMetaVersion != mMetaVersion)
    {
        if (frame.batchMapped && !mBatchDescsCpu.empty())
        {
            const UINT64 bytes = sizeof(GpuBatchDesc) * mBatchDescsCpu.size();
            std::memcpy(frame.batchMapped, mBatchDescsCpu.data(), static_cast<size_t>(bytes));
            transition(frame.batchDefault.Get(), frame.batchDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                frame.batchDefault.Get(), 0,
                frame.batchUpload.Get(), 0,
                bytes);
            transition(frame.batchDefault.Get(), frame.batchDefaultState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            mLastStats.usedDefaultHeapCopy = true;
        }
        if (frame.submeshMapped && !mSubmeshDescsCpu.empty())
        {
            const UINT64 bytes = sizeof(GpuSubmeshDesc) * mSubmeshDescsCpu.size();
            std::memcpy(frame.submeshMapped, mSubmeshDescsCpu.data(), static_cast<size_t>(bytes));
            transition(frame.submeshDefault.Get(), frame.submeshDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                frame.submeshDefault.Get(), 0,
                frame.submeshUpload.Get(), 0,
                bytes);
            transition(frame.submeshDefault.Get(), frame.submeshDefaultState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            mLastStats.usedDefaultHeapCopy = true;
        }
        frame.uploadedMetaVersion = mMetaVersion;
        mLastStats.didMetaUpload = true;
    }
    else
    {
        transition(frame.batchDefault.Get(), frame.batchDefaultState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(frame.submeshDefault.Get(), frame.submeshDefaultState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    mLastStats.uploadMs = ElapsedMs(t0);
    return didTransformUpload;
}

bool RenderSystem::DispatchUpdateMotion(
    ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame, UINT numInstances)
{
    mLastStats.didGpuMotion = false;
    mLastStats.motionMs = 0.f;
    mLastStats.motionActive = mMotionActiveCount;
    mLastStats.gpuMotionEnabled = mGpuMotionEnabled;

    if (!mGpuMotionEnabled || !mUpdateMotionPSO || !frame.transformDefault || !frame.motionDefault)
        return false;
    if (numInstances == 0 || mMotionActiveCount == 0)
        return false;
    if (numInstances > mTransformCpu.size())
        numInstances = static_cast<UINT>(mTransformCpu.size());

    ID3D12RootSignature* motionRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::UpdateMotion);
    if (!motionRS)
        return false;

    const auto t0 = std::chrono::high_resolution_clock::now();

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& st, D3D12_RESOURCE_STATES target)
    {
        if (st != target)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(res, st, target));
            st = target;
        }
    };

    transition(frame.transformDefault.Get(), frame.transformDefaultState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(frame.motionDefault.Get(), frame.motionDefaultState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetComputeRootSignature(motionRS);
    cmdList->SetPipelineState(mUpdateMotionPSO.Get());

    const float dt = (mFrameDeltaTime > 0.f && mFrameDeltaTime < 0.25f)
        ? mFrameDeltaTime
        : (1.f / 60.f);
    // Root constants: uint, uint, float, uint ??pack float as bits
    UINT constants[4];
    constants[0] = numInstances;
    constants[1] = kMaxInstancesPerDraw;
    std::memcpy(&constants[2], &dt, sizeof(float));
    constants[3] = 0;
    cmdList->SetComputeRoot32BitConstants(0, 4, constants, 0);
    cmdList->SetComputeRootUnorderedAccessView(1, frame.transformDefault->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(2, frame.motionDefault->GetGPUVirtualAddress());

    cmdList->Dispatch((numInstances + 63u) / 64u, 1, 1);

    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::UAV(frame.transformDefault.Get());
        b[1] = CD3DX12_RESOURCE_BARRIER::UAV(frame.motionDefault.Get());
        cmdList->ResourceBarrier(2, b);
    }

    // Compose reads transforms as SRV
    transition(frame.transformDefault.Get(), frame.transformDefaultState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    mLastStats.didGpuMotion = true;
    mLastStats.motionMs = ElapsedMs(t0);
    return true;
}

void RenderSystem::DispatchComposeWorld(
    ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame, UINT numInstances)
{
    mLastStats.didComposeWorld = false;
    mLastStats.composeMs = 0.f;

    if (!frame.sourceDefault || numInstances == 0)
        return;
    if (numInstances > mTransformCpu.size())
        numInstances = static_cast<UINT>(mTransformCpu.size());
    if (numInstances == 0)
        return;

    // Already composed for this transform version on this frame slot
    if (frame.composedTransformVersion == mTransformContentVersion
        && frame.composedTransformVersion != 0)
    {
        // Cull expects source as SRV
        if (frame.sourceDefaultState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                frame.sourceDefault.Get(),
                frame.sourceDefaultState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            frame.sourceDefaultState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        return;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& st, D3D12_RESOURCE_STATES target)
    {
        if (st != target)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(res, st, target));
            st = target;
        }
    };

    ID3D12RootSignature* composeRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::ComposeWorld);

    // GPU path: TRS (transformDefault) ??CS_ComposeWorld ??sourceDefault
    if (mComposeWorldPSO && composeRS && frame.transformDefault)
    {
        transition(frame.transformDefault.Get(), frame.transformDefaultState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(frame.sourceDefault.Get(), frame.sourceDefaultState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmdList->SetComputeRootSignature(composeRS);
        cmdList->SetPipelineState(mComposeWorldPSO.Get());

        const UINT constants[4] = {
            numInstances,
            kMaxInstancesPerDraw,
            0u,
            0u
        };
        cmdList->SetComputeRoot32BitConstants(0, 4, constants, 0);
        cmdList->SetComputeRootShaderResourceView(
            1, frame.transformDefault->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(
            2, frame.sourceDefault->GetGPUVirtualAddress());

        cmdList->Dispatch((numInstances + 63u) / 64u, 1, 1);

        // UAV write ??SRV read (cull)
        {
            D3D12_RESOURCE_BARRIER b[2];
            b[0] = CD3DX12_RESOURCE_BARRIER::UAV(frame.sourceDefault.Get());
            b[1] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.sourceDefault.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(2, b);
            frame.sourceDefaultState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }

        frame.composedTransformVersion = mTransformContentVersion;
        mLastStats.didComposeWorld = true;
        mLastStats.composeMs = ElapsedMs(t0);
        return;
    }

    // Fallback: CPU GetWorldMatrix path (PSO missing)
    if (!frame.requestMapped || mSourceCpu.empty())
        return;
    if (numInstances > mSourceCpu.size())
        numInstances = static_cast<UINT>(mSourceCpu.size());

    const UINT64 bytes = sizeof(GpuInstanceSource) * numInstances;
    std::memcpy(frame.requestMapped, mSourceCpu.data(), static_cast<size_t>(bytes));

    transition(frame.sourceDefault.Get(), frame.sourceDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(
        frame.sourceDefault.Get(), 0,
        frame.requestUpload.Get(), 0,
        bytes);
    transition(frame.sourceDefault.Get(), frame.sourceDefaultState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    frame.composedTransformVersion = mTransformContentVersion;
    mLastStats.didComposeWorld = true;
    mLastStats.composeMs = ElapsedMs(t0);
}

bool RenderSystem::TryDrawCachedInstancedFrame(
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    FrameGpuResources& frame,
    DescriptorAllocator* descriptorAllocator,
    World& world)
{
    auto incoming = TransformDirtyTracker::TakeEntities();
    if (!incoming.empty())
    {
        mPendingTransformDirty.insert(
            mPendingTransformDirty.end(), incoming.begin(), incoming.end());
    }

    const uint32_t gen = TransformDirtyTracker::Generation();
    const bool staticScene =
        mInstancedCacheValid
        && mPendingTransformDirty.empty()
        && gen == mPath2CachedDirtyGen;

    if (!staticScene)
    {
        if (mInstancedCacheValid)
            mInstancedCacheValid = false;
        return false;
    }

    mLastStats = {};
    RestoreLodStats(mLastStats, mLodStatEnabled, mLodStatCulled, mLodStatSwitches,
        mLodStatLevels, mLodStatMs);
    mLastStats.path = RenderPath::Instanced;
    mLastStats.autoPath = mAutoRenderPath;
    mLastStats.skippedPatch = true;
    mLastStats.sourceCount = 0;
    for (const auto& b : mCachedInstancedBatches)
        mLastStats.sourceCount += static_cast<uint32_t>(b.instances.size());
    mLastStats.batchCount = static_cast<uint32_t>(mCachedInstancedBatches.size());

    DrawInstancedBatches(cmdList, currentFrameResource, frame,
        descriptorAllocator, mCachedOverrideEntities, world);
    return true;
}

void RenderSystem::RebuildInstancedBatchCache(World& world, FrameResource* currentFrameResource)
{
    std::map<std::pair<Mesh*, Material*>, std::vector<InstanceWorld>> batchMap;
    std::vector<Entity> overrideEntities;
    UINT totalObjects = 0;

    mPendingTransformDirty.clear();
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (const auto* lod = world.GetComponent<LodComponent>(e); lod && lod->culled)
                return;
            if (totalObjects >= kMaxInstancesPerDraw) return;

            if (HasEntitySubMaterialOverride(e))
            {
                overrideEntities.push_back(e);
                return;
            }

            if (tf.dirtyFrames > 0 && rend.objectCBIndex < kMaxSceneObjects)
            {
                ObjectConstants objConst{};
                XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf.GetWorldMatrix()));
                currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);
                --tf.dirtyFrames;
                // Multi-frame ObjectCB: keep pending so cache stays invalid until settled.
                if (tf.dirtyFrames > 0)
                    mPendingTransformDirty.push_back(e);
            }

            Material* mainMat = GetEntityMainMaterialOverride(e);
            InstanceWorld inst{};
            StoreWorldTransposed(tf.GetWorldMatrix(), inst.worldMatrix);
            batchMap[{rend.mesh, mainMat}].push_back(inst);
            ++totalObjects;
        });

    mCachedInstancedBatches.clear();
    mCachedInstancedBatches.reserve(batchMap.size());
    for (auto& kv : batchMap)
    {
        CachedInstancedBatch b;
        b.mesh = kv.first.first;
        b.mainMaterial = kv.first.second;
        b.instances = std::move(kv.second);
        mCachedInstancedBatches.push_back(std::move(b));
    }
    mCachedOverrideEntities = std::move(overrideEntities);
    mInstancedCacheValid = true;
    mPath2CachedDirtyGen = TransformDirtyTracker::Generation();
    TransformDirtyTracker::ClearEntities();
}

void RenderSystem::DrawInstancedBatches(
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    FrameGpuResources& frame,
    DescriptorAllocator* descriptorAllocator,
    const std::vector<Entity>& overrideEntities,
    World& world)
{
    ID3D12RootSignature* sceneRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    if (!sceneRS)
        return;

    BindSceneFrameState(cmdList, currentFrameResource, nullptr, sceneRS, true);
    cmdList->OMSetStencilRef(1);

    DrawInstancedOverrideEntities(
        cmdList, currentFrameResource, descriptorAllocator, sceneRS, overrideEntities, world);

    std::vector<InstancedDrawRecord> drawRecs;
    UploadAndDrawInstancedOpaque(cmdList, frame, descriptorAllocator, sceneRS, drawRecs);
    DrawInstancedOutlinePass(cmdList, frame, descriptorAllocator, sceneRS, drawRecs);
}

void RenderSystem::DrawInstancedOverrideEntities(
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* /*descriptorAllocator*/,
    ID3D12RootSignature* sceneRS,
    const std::vector<Entity>& overrideEntities,
    World& world)
{
    if (overrideEntities.empty() || !sceneRS)
        return;

    const PSOKey basicKey = MakeOpaquePsoKey("object_cb");
    if (ID3D12PipelineState* basicPso = GetScenePso(basicKey, sceneRS))
        cmdList->SetPipelineState(basicPso);
    if (mDummyInstanceBuffer)
        cmdList->SetGraphicsRootShaderResourceView(3, mDummyInstanceBuffer->GetGPUVirtualAddress());

    for (Entity e : overrideEntities)
    {
        auto* tf = world.GetComponent<TransformComponent>(e);
        auto* rend = world.GetComponent<RenderableComponent>(e);
        if (tf && rend)
            DrawEntityWithResolvedMaterials(cmdList, currentFrameResource, e, *tf, *rend);
    }
}

void RenderSystem::UploadAndDrawInstancedOpaque(
    ID3D12GraphicsCommandList* cmdList,
    FrameGpuResources& frame,
    DescriptorAllocator* descriptorAllocator,
    ID3D12RootSignature* sceneRS,
    std::vector<InstancedDrawRecord>& outDrawRecs)
{
    if (mCachedInstancedBatches.empty())
        return;

    const PSOKey gfxKey = MakeOpaquePsoKey("object_instanced");
    ID3D12PipelineState* gfxPSO = GetScenePso(gfxKey, sceneRS);
    if (!gfxPSO)
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            OutputDebugStringA(
                "[RenderSystem] Instanced PSO failed; drawing overrides only.\n");
            sLogged = true;
        }
        return;
    }

    cmdList->SetPipelineState(gfxPSO);
    cmdList->OMSetStencilRef(1);

    BYTE* upload = frame.requestMapped;
    UINT uploadCursor = 0;
    UINT lastMat = UINT_MAX;
    outDrawRecs.reserve(mCachedInstancedBatches.size());

    for (const CachedInstancedBatch& batch : mCachedInstancedBatches)
    {
        Mesh* mesh = batch.mesh;
        const auto& instances = batch.instances;
        if (!mesh || instances.empty())
            continue;

        const UINT n = static_cast<UINT>((std::min)(
            instances.size(),
            static_cast<size_t>(kMaxInstancesPerDraw - uploadCursor)));
        if (n == 0)
            break;

        const UINT64 byteOffset = (UINT64)uploadCursor * sizeof(InstanceWorld);
        std::memcpy(upload + byteOffset, instances.data(), sizeof(InstanceWorld) * n);

        const D3D12_GPU_VIRTUAL_ADDRESS instVA =
            frame.requestUpload->GetGPUVirtualAddress() + byteOffset;
        cmdList->SetGraphicsRootShaderResourceView(3, instVA);

        auto vbv = mesh->VertexBufferView();
        auto ibv = mesh->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (auto& pair : mesh->DrawArgs)
        {
            const auto& sub = pair.second;
            if (sub.IndexCount == 0)
                continue;

            Material* material = ResolveBatchMaterial(batch.mainMaterial, sub);
            if (!material || !material->HasValidTexture())
                continue;

            BindMaterialByIndex(cmdList, descriptorAllocator,
                material->mTextureHandle.Index, lastMat);
            cmdList->DrawIndexedInstanced(
                sub.IndexCount, n,
                sub.StartIndexLocation,
                sub.BaseVertexLocation, 0);
        }

        outDrawRecs.push_back(InstancedDrawRecord{ mesh, n, byteOffset, batch.mainMaterial });
        uploadCursor += n;
    }
}

void RenderSystem::DrawInstancedOutlinePass(
    ID3D12GraphicsCommandList* cmdList,
    FrameGpuResources& frame,
    DescriptorAllocator* descriptorAllocator,
    ID3D12RootSignature* sceneRS,
    const std::vector<InstancedDrawRecord>& drawRecs)
{
    if (!mOutlinePassEnabled || drawRecs.empty() || !sceneRS)
        return;

    const PSOKey outlineKey = MakeOutlinePsoKey("object_instanced_outline");
    ID3D12PipelineState* outlinePso = GetScenePso(outlineKey, sceneRS);
    if (!outlinePso)
        return;

    cmdList->SetPipelineState(outlinePso);
    cmdList->OMSetStencilRef(1);
    UINT lastMat = UINT_MAX;

    for (const InstancedDrawRecord& rec : drawRecs)
    {
        if (!rec.mesh || rec.instanceCount == 0)
            continue;

        const D3D12_GPU_VIRTUAL_ADDRESS instVA =
            frame.requestUpload->GetGPUVirtualAddress() + rec.byteOffset;
        cmdList->SetGraphicsRootShaderResourceView(3, instVA);

        for (auto& pair : rec.mesh->DrawArgs)
        {
            Material* material = ResolveBatchMaterial(rec.mainMaterial, pair.second);
            if (!material || !material->HasValidTexture())
                continue;
            BindMaterialByIndex(cmdList, descriptorAllocator,
                material->mTextureHandle.Index, lastMat);
            break;
        }
        DrawMeshOutlineIndexed(cmdList, rec.mesh, rec.instanceCount);
    }
}

void RenderSystem::DrawBasicOpaquePass(
    World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator)
{
    UINT lastMat = UINT_MAX;
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (const auto* lod = world.GetComponent<LodComponent>(e); lod && lod->culled)
                return;
            if (rend.objectCBIndex >= kMaxSceneObjects) return;

            if (tf.dirtyFrames > 0)
            {
                ObjectConstants objConst{};
                XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf.GetWorldMatrix()));
                currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);
                --tf.dirtyFrames;
            }

            auto objectCB = currentFrameResource->ObjectCB->Resource();
            UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
            D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() +
                (UINT64)rend.objectCBIndex * objCBByteSize;

            auto vbv = rend.mesh->VertexBufferView();
            auto ibv = rend.mesh->IndexBufferView();
            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            for (auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = MaterialManager::Get().ResolveForDraw(e, pair.first, sub.initMaterial);
                if (!material || !material->HasValidTexture())
                    continue;

                cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
                BindMaterialByIndex(cmdList, descriptorAllocator,
                    material->mTextureHandle.Index, lastMat);
                cmdList->DrawIndexedInstanced(
                    sub.IndexCount, 1,
                    sub.StartIndexLocation,
                    sub.BaseVertexLocation, 0);
            }
        });
}

void RenderSystem::DrawBasicOutlinePass(
    World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    ID3D12RootSignature* sceneRS)
{
    if (!mOutlinePassEnabled || !sceneRS)
        return;

    const PSOKey outlineKey = MakeOutlinePsoKey("object_cb_outline");
    ID3D12PipelineState* outlinePso = GetScenePso(outlineKey, sceneRS);
    if (!outlinePso)
        return;

    cmdList->SetPipelineState(outlinePso);
    cmdList->OMSetStencilRef(1);
    UINT lastMat = UINT_MAX;

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& /*tf*/, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (const auto* lod = world.GetComponent<LodComponent>(e); lod && lod->culled)
                return;
            if (rend.objectCBIndex >= kMaxSceneObjects) return;

            auto objectCB = currentFrameResource->ObjectCB->Resource();
            UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
            D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() +
                (UINT64)rend.objectCBIndex * objCBByteSize;
            cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

            // Root signature still expects a texture table - bind first valid material.
            for (auto& pair : rend.mesh->DrawArgs)
            {
                Material* material = MaterialManager::Get().ResolveForDraw(
                    e, pair.first, pair.second.initMaterial);
                if (!material || !material->HasValidTexture())
                    continue;
                BindMaterialByIndex(cmdList, descriptorAllocator,
                    material->mTextureHandle.Index, lastMat);
                break;
            }
            DrawMeshOutlineIndexed(cmdList, rend.mesh, 1);
        });
}

void RenderSystem::BeginShadowDepthTarget(ID3D12GraphicsCommandList* cmdList)
{
    if (mShadowState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mShadowMap.Get(), mShadowState, D3D12_RESOURCE_STATE_DEPTH_WRITE));
        mShadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(kShadowMapSize);
    vp.Height = static_cast<float>(kShadowMapSize);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize) };
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);

    auto dsv = mShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void RenderSystem::DrawShadowCasters(
    World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    ID3D12RootSignature* shadowRS)
{
    cmdList->SetGraphicsRootSignature(shadowRS);
    cmdList->SetGraphicsRootConstantBufferView(
        1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    if (mDummyInstanceBuffer)
        cmdList->SetGraphicsRootShaderResourceView(2, mDummyInstanceBuffer->GetGPUVirtualAddress());

    PSOKey depthKey{};
    depthKey.shaderName = "object_depth";
    depthKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    depthKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    depthKey.rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    depthKey.rasterizerDesc.DepthBias = 100000;
    depthKey.rasterizerDesc.DepthBiasClamp = 0.0f;
    depthKey.rasterizerDesc.SlopeScaledDepthBias = 1.5f;
    depthKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    depthKey.depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    depthKey.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    ID3D12PipelineState* depthPso =
        PipelineStateManager::Get().GetOrCreatePSO(depthKey, shadowRS, nullptr, true);
    if (!depthPso)
        return;
    cmdList->SetPipelineState(depthPso);

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh || rend.mesh->indexCount == 0)
                return;
            if (const auto* lod = world.GetComponent<LodComponent>(e); lod && lod->culled)
                return;
            if (rend.objectCBIndex >= kMaxSceneObjects)
                return;

            // Always refresh world for casters; do not consume dirtyFrames (color pass still needs them).
            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf.GetWorldMatrix()));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

            auto objectCB = currentFrameResource->ObjectCB->Resource();
            const UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
            const D3D12_GPU_VIRTUAL_ADDRESS objCBAddress =
                objectCB->GetGPUVirtualAddress() + (UINT64)rend.objectCBIndex * objCBByteSize;
            cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

            auto vbv = rend.mesh->VertexBufferView();
            auto ibv = rend.mesh->IndexBufferView();
            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmdList->DrawIndexedInstanced(rend.mesh->indexCount, 1, 0, 0, 0);
        });
}

void RenderSystem::EndShadowMapAsSrv(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mShadowMap.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    mShadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}


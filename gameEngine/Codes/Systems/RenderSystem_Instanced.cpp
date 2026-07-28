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
// MAIN — Path2 color orchestrator only
// Stages live in RenderSystem_SubLogic.cpp
// =============================================================================

void RenderSystem::renderInstanced(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    (void)viewMatrix;
    (void)projMatrix;

    EnsureGpuResources(mDevice);

    const int frameIdx = ((currentFrameIndex % (int)kIndirectFrameCount) + (int)kIndirectFrameCount)
        % (int)kIndirectFrameCount;
    FrameGpuResources& frame = mFrames[frameIdx];

    BindSceneFrameState(cmdList, currentFrameResource, descriptorAllocator, nullptr, false);

    ID3D12RootSignature* sceneRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    if (!sceneRS)
    {
        renderBasic(world, cmdList, currentFrameResource, descriptorAllocator, viewMatrix, projMatrix);
        return;
    }

    if (TryDrawCachedInstancedFrame(
            cmdList, currentFrameResource, frame, descriptorAllocator, world))
        return;

    RebuildInstancedBatchCache(world, currentFrameResource);

    mLastStats = {};
    RestoreLodStats(mLastStats, mLodStatEnabled, mLodStatCulled, mLodStatSwitches,
        mLodStatLevels, mLodStatMs);
    mLastStats.path = RenderPath::Instanced;
    mLastStats.autoPath = mAutoRenderPath;
    mLastStats.pendingDirty = static_cast<uint32_t>(mPendingTransformDirty.size());
    mLastStats.batchCount = static_cast<uint32_t>(mCachedInstancedBatches.size());
    for (const auto& b : mCachedInstancedBatches)
        mLastStats.sourceCount += static_cast<uint32_t>(b.instances.size());

    DrawInstancedBatches(cmdList, currentFrameResource, frame,
        descriptorAllocator, mCachedOverrideEntities, world);
}


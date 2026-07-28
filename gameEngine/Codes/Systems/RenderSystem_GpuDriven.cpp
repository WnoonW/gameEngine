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
// MAIN — Path3 color orchestrator only
// Stages live in RenderSystem_SubLogic.cpp
// =============================================================================

void RenderSystem::renderComputeIndirect(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    EnsureGpuResources(mDevice);

    const int frameIdx = ((currentFrameIndex % (int)kIndirectFrameCount) + (int)kIndirectFrameCount)
        % (int)kIndirectFrameCount;
    FrameGpuResources& frame = mFrames[frameIdx];

    BindSceneFrameState(cmdList, currentFrameResource, descriptorAllocator, nullptr, false);
    mSrvAlloc = descriptorAllocator;

    BeginGpuDrivenFrameStats();

    // Previous use of this ring slot finished -> map cull counters into stats
    ResolveGpuCullReadback(frame);

    // --- CPU: scene structure + transforms + motion seeds ---
    UpdateGpuDrivenSceneData(world, currentFrameResource);

    ID3D12RootSignature* sceneRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);

    // --- Graphics: entities that cannot use GPU instance batches ---
    DrawMaterialOverrideEntities(world, cmdList, currentFrameResource, sceneRS);

    if (mSourceCpu.empty() || mGpuCpuBatches.empty() || mSubmeshDescsCpu.empty())
        return;

    // --- Compute: TRS upload -> motion -> compose world matrices ---
    const UINT numXforms = PrepareGpuDrivenMotionAndCompose(
        cmdList, frame, currentFrameIndex, world);

    // --- Compute: cull constants + cull/compact + build indirect args ---
    FillGpuDrivenCullConstants(frame, viewMatrix, projMatrix, numXforms);

    ID3D12CommandSignature* cmdSig = RootSignatureManager::Get().GetSceneCommandSignature();
    const PSOKey gfxKey = MakeOpaquePsoKey("object_instanced");
    ID3D12PipelineState* gfxPSO = GetScenePso(gfxKey, sceneRS);
    ID3D12RootSignature* buildRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::IndirectBuild);

    if (!buildRS || !sceneRS || !gfxPSO || !cmdSig || !mCullCompactPSO || !mBuildCommandsPSO)
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            OutputDebugStringA(
                "[RenderSystem] GPU-driven setup failed; fallback -> Instanced.\n");
            sLogged = true;
        }
        mRenderPath = RenderPath::Instanced;
        renderInstanced(world, cmdList, currentFrameResource, descriptorAllocator,
            currentFrameIndex, viewMatrix, projMatrix);
        return;
    }

    if (!DispatchGpuCullAndBuildCommands(cmdList, frame, descriptorAllocator, numXforms))
        return;

    // --- Graphics: ExecuteIndirect opaque + optional toon outline ---
    ExecuteIndirectOpaquePass(
        cmdList, currentFrameResource, frame, descriptorAllocator,
        sceneRS, gfxPSO, cmdSig);
    DrawGpuDrivenOutlinePass(cmdList, frame, descriptorAllocator, sceneRS);
}


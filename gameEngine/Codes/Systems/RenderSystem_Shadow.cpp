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
// MAIN — directional shadow orchestrator only
// Stages/helpers: SubLogic + Helpers
// =============================================================================

void RenderSystem::RenderShadowMap(
    World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int /*currentFrameIndex*/,
    const XMMATRIX& /*viewMatrix*/,
    const XMMATRIX& /*projMatrix*/)
{
    if (!cmdList || !currentFrameResource || !descriptorAllocator)
        return;

    EnsureShadowMap(descriptorAllocator);
    if (!mShadowMap || !mShadowDsvHeap)
        return;

    ID3D12RootSignature* shadowRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::Shadow);
    if (!shadowRS || !currentFrameResource->PassCB)
        return;

    BeginShadowDepthTarget(cmdList);

    if (mShadowsEnabled)
        DrawShadowCasters(world, cmdList, currentFrameResource, shadowRS);

    EndShadowMapAsSrv(cmdList);
}


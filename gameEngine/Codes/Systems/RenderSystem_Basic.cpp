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
// MAIN — Path1 color orchestrator only
// Stages live in RenderSystem_SubLogic.cpp
// =============================================================================

void RenderSystem::renderBasic(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    (void)viewMatrix;
    (void)projMatrix;

    ID3D12RootSignature* sceneRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    BindSceneFrameState(cmdList, currentFrameResource, descriptorAllocator, sceneRS, true);

    const PSOKey key = MakeOpaquePsoKey("object_cb");
    if (ID3D12PipelineState* pso = GetScenePso(key, sceneRS))
        cmdList->SetPipelineState(pso);
    cmdList->OMSetStencilRef(1);

    if (mDummyInstanceBuffer)
        cmdList->SetGraphicsRootShaderResourceView(3, mDummyInstanceBuffer->GetGPUVirtualAddress());

    DrawBasicOpaquePass(world, cmdList, currentFrameResource, descriptorAllocator);
    DrawBasicOutlinePass(world, cmdList, currentFrameResource, descriptorAllocator, sceneRS);
}


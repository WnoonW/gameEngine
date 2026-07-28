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
// MAIN — Hi-Z build (after SceneEnd)
// Resource helpers: RenderSystem_Helpers.cpp
// =============================================================================

void RenderSystem::BuildHiZ(
    ID3D12GraphicsCommandList* cmdList,
    DescriptorAllocator* descriptorAllocator,
    ID3D12Resource* sceneDepth,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSrvCpu,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvGpu,
    UINT width, UINT height)
{
    (void)sceneDepth;
    (void)sceneDepthSrvCpu;

    mLastStats.didBuildHiZ = false;
    mLastStats.hizMs = 0.f;

    if (!cmdList || !descriptorAllocator || !mDevice)
        return;
    if (!mHiZCopyPSO)
        return;
    if (width == 0 || height == 0)
        return;
    if (sceneDepthSrvGpu.ptr == 0)
        return;

    const auto t0 = std::chrono::high_resolution_clock::now();
    mSrvAlloc = descriptorAllocator;
    EnsureHiZResources(descriptorAllocator, width, height);

    // Still wrong size / not created (wait for PrepareHiZ after flush)
    if (!mHiZRing[0].texture || mHiZWidth != width || mHiZHeight != height)
        return;

    ID3D12RootSignature* hizRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::HiZBuild);
    HiZSlot& slot = mHiZRing[mHiZWriteSlot];
    if (!hizRS || !slot.texture || slot.uav.Index == UINT_MAX)
        return;

    ID3D12DescriptorHeap* heaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(hizRS);

    // ???�롯�??�기 ???�플 중인 ?�른 ?�롯�?분리
    if (slot.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            slot.texture.Get(), slot.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        slot.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    uint32_t consts[8] = { width, height, mHiZWidth, mHiZHeight, 0, 0, 0, 0 };
    cmdList->SetComputeRoot32BitConstants(0, 8, consts, 0);
    cmdList->SetComputeRootDescriptorTable(1, sceneDepthSrvGpu);
    cmdList->SetComputeRootDescriptorTable(2, slot.uav.GPU);
    cmdList->SetPipelineState(mHiZCopyPSO.Get());
    cmdList->Dispatch((mHiZWidth + 7) / 8, (mHiZHeight + 7) / 8, 1);

    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::UAV(slot.texture.Get());
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            slot.texture.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(2, b);
        slot.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    mHiZWriteSlot = (mHiZWriteSlot + 1u) % kHiZRingSize;
    ++mHiZBuildCount;

    mLastStats.didBuildHiZ = true;
    mLastStats.hizMs = ElapsedMs(t0);
    mLastStats.hizMips = mHiZMipCount;
    mLastStats.hizValid = IsHiZSampleReady();
}

void RenderSystem::PrepareHiZForSceneSize(DescriptorAllocator* alloc, UINT width, UINT height)
{
    // Caller must guarantee GPU idle (e.g. Flush after SceneViewport::Resize).
    width = (std::max)(1u, width);
    height = (std::max)(1u, height);
    if (mHiZRing[0].texture && mHiZWidth == width && mHiZHeight == height)
        return;

    DestroyHiZResources(alloc);
    // Actual create deferred to BuildHiZ / EnsureHiZResources
    mHiZWidth = 0;
    mHiZHeight = 0;
}


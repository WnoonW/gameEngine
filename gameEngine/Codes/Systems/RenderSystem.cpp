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
// MAIN — frame entry, public controls, lifecycle
// Sub-logic: RenderSystem_SubLogic.cpp | Helpers: RenderSystem_Helpers.cpp
// Paths: _GpuDriven / _Instanced / _Basic | Shadow / HiZ
// =============================================================================

void RenderSystem::render(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    // 1) Distance LOD / far cull before path selection
    UpdateEntityLods(world, viewMatrix);

    // 2) Optional auto path by object count
    UpdateAutoRenderPath(world);

    // 3) Dispatch selected path
    switch (mRenderPath)
    {
    case RenderPath::ComputeIndirect:
        if (mComputeIndirectReady
            && mCullCompactPSO
            && mBuildCommandsPSO
            && RootSignatureManager::Get().GetSceneCommandSignature())
        {
            renderComputeIndirect(world, cmdList, currentFrameResource, descriptorAllocator,
                currentFrameIndex, viewMatrix, projMatrix);
            return;
        }
        {
            static bool sLogged = false;
            if (!sLogged)
            {
                OutputDebugStringA(
                    "[RenderSystem] ComputeIndirect unavailable at render time; "
                    "fallback -> Instanced (path flag set to Instanced).\n");
                sLogged = true;
            }
            mRenderPath = RenderPath::Instanced;
        }
        renderInstanced(world, cmdList, currentFrameResource, descriptorAllocator,
            currentFrameIndex, viewMatrix, projMatrix);
        return;
    case RenderPath::Instanced:
        renderInstanced(world, cmdList, currentFrameResource, descriptorAllocator,
            currentFrameIndex, viewMatrix, projMatrix);
        return;
    case RenderPath::Basic:
    default:
        renderBasic(world, cmdList, currentFrameResource, descriptorAllocator, viewMatrix, projMatrix);
        return;
    }
}

void RenderSystem::SetRenderPath(RenderPath path)
{
    if (path == RenderPath::ComputeIndirect && !mComputeIndirectReady)
    {
        OutputDebugStringA(
            "[RenderSystem] SetRenderPath(ComputeIndirect) failed: not ready. "
            "Path unchanged. Use Instanced/Basic instead.\n");
        return;
    }

    // ?�동 지????Auto ?�제
    mAutoRenderPath = false;

    const char* name = "Unknown";
    switch (path)
    {
    case RenderPath::Basic: name = "Basic"; break;
    case RenderPath::Instanced: name = "Instanced"; break;
    case RenderPath::ComputeIndirect: name = "ComputeIndirect(GPU-driven)"; break;
    }
    char buf[160];
    sprintf_s(buf, "[RenderSystem] SetRenderPath -> %s (auto off)\n", name);
    OutputDebugStringA(buf);

    mRenderPath = path;
    if (path == RenderPath::ComputeIndirect)
        mGpuStructureDirty = true;
}

void RenderSystem::SetAutoRenderPathEnabled(bool enabled)
{
    mAutoRenderPath = enabled;
    char buf[96];
    sprintf_s(buf, "[RenderSystem] AutoRenderPath -> %s\n", enabled ? "ON" : "OFF");
    OutputDebugStringA(buf);
}

void RenderSystem::SetLodEnabled(bool enabled)
{
    if (mLodEnabled == enabled)
        return;
    mLodEnabled = enabled;
    mGpuStructureDirty = true;
    mInstancedCacheValid = false;
}

void RenderSystem::SetGpuMotionEnabled(bool enabled)
{
    if (mGpuMotionEnabled == enabled)
        return;
    mGpuMotionEnabled = enabled;
    // Force motion buffer re-upload on next Path3 frame (seed from current ECS)
    ++mMotionContentVersion;
}

bool RenderSystem::ShouldSkipCpuGravity() const
{
    return mGpuMotionEnabled
        && mUpdateMotionPSO
        && mRenderPath == RenderPath::ComputeIndirect
        && mComputeIndirectReady;
}

void RenderSystem::NotifyEntityMotionChanged(World& world, Entity e)
{
    auto it = mEntityToGpuSlot.find(e);
    if (it == mEntityToGpuSlot.end())
        return;
    const UINT slot = it->second;
    if (slot >= mMotionCpu.size())
        return;

    const uint32_t prev = mMotionCpu[slot].flags & 1u;
    FillMotionFromEntity(mMotionCpu[slot], world.GetComponent<GravityComponent>(e));
    const uint32_t now = mMotionCpu[slot].flags & 1u;
    if (prev != now)
    {
        if (now)
            ++mMotionActiveCount;
        else if (mMotionActiveCount > 0)
            --mMotionActiveCount;
    }
    ++mMotionContentVersion;
}

void RenderSystem::InvalidateDrawCache()
{
    mInstancedCacheValid = false;
    mGpuStructureDirty = true;
    mPendingTransformDirty.clear();
}

void RenderSystem::Initialize(ID3D12Device* device)
{
    mDevice = device;
    if (!mDevice)
        return;

    EnsureGpuResources(mDevice);

    ID3D12RootSignature* buildRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::IndirectBuild);
    ID3D12RootSignature* composeRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::ComposeWorld);

    auto cullBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\build_indirect_commands.hlsl", "CS_CullCompact", "cs_5_1");
    auto buildBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\build_indirect_commands.hlsl", "CS_BuildCommands", "cs_5_1");
    auto composeBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\compose_world.hlsl", "CS_ComposeWorld", "cs_5_1");
    auto motionBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\update_motion.hlsl", "CS_UpdateMotion", "cs_5_1");

    if (cullBlob && buildBlob && buildRS)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = buildRS;
        psoDesc.CS = { cullBlob->GetBufferPointer(), cullBlob->GetBufferSize() };
        if (SUCCEEDED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mCullCompactPSO))))
        {
            psoDesc.CS = { buildBlob->GetBufferPointer(), buildBlob->GetBufferSize() };
            if (SUCCEEDED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mBuildCommandsPSO))))
                mComputeIndirectReady = true;
        }
    }

    if (composeBlob && composeRS)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = composeRS;
        psoDesc.CS = { composeBlob->GetBufferPointer(), composeBlob->GetBufferSize() };
        if (FAILED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mComposeWorldPSO))))
            mComposeWorldPSO.Reset();
        else
            ValidateComposeWorldMatchesCpu();
    }

    ID3D12RootSignature* motionRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::UpdateMotion);
    if (motionBlob && motionRS)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = motionRS;
        psoDesc.CS = { motionBlob->GetBufferPointer(), motionBlob->GetBufferSize() };
        if (FAILED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mUpdateMotionPSO))))
            mUpdateMotionPSO.Reset();
    }

    ID3D12RootSignature* hizRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::HiZBuild);
    auto hizCopy = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\hiz_build.hlsl", "CS_CopyDepth", "cs_5_1");
    auto hizDown = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\hiz_build.hlsl", "CS_Downsample", "cs_5_1");
    if (hizRS && hizCopy && hizDown)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = hizRS;
        psoDesc.CS = { hizCopy->GetBufferPointer(), hizCopy->GetBufferSize() };
        if (SUCCEEDED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mHiZCopyPSO))))
        {
            psoDesc.CS = { hizDown->GetBufferPointer(), hizDown->GetBufferSize() };
            mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mHiZDownsamplePSO));
        }
    }

    if (!mComputeIndirectReady)
        OutputDebugStringA("[RenderSystem] Path3 GPU-driven unavailable (CS/PSO). Path1/2 OK.\n");
    else if (mComposeWorldPSO && mUpdateMotionPSO)
        OutputDebugStringA("[RenderSystem] Path3 ready (Motion + ComposeWorld + Cull + EI + HiZ).\n");
    else if (mComposeWorldPSO)
        OutputDebugStringA("[RenderSystem] Path3 ready (ComposeWorld + Cull + EI + HiZ; no Motion PSO).\n");
    else
        OutputDebugStringA("[RenderSystem] Path3 ready (no ComposeWorld PSO; fallback needed).\n");

    {
        InstanceWorld dummy{};
        StoreWorldTransposed(XMMatrixIdentity(), dummy.worldMatrix);
        const UINT64 sz = sizeof(InstanceWorld);
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(sz),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&mDummyInstanceBuffer)));
        void* mapped = nullptr;
        ThrowIfFailed(mDummyInstanceBuffer->Map(0, nullptr, &mapped));
        std::memcpy(mapped, &dummy, sizeof(dummy));
        mDummyInstanceBuffer->Unmap(0, nullptr);
    }

    mRenderPath = mComputeIndirectReady ? RenderPath::ComputeIndirect : RenderPath::Instanced;
}

void RenderSystem::Shutdown()
{
    DestroyHiZResources(mSrvAlloc);
    DestroyShadowMap(mSrvAlloc);
    DestroyGpuResources();
    mCullCompactPSO.Reset();
    mBuildCommandsPSO.Reset();
    mComposeWorldPSO.Reset();
    mUpdateMotionPSO.Reset();
    mHiZCopyPSO.Reset();
    mHiZDownsamplePSO.Reset();
    mComputeIndirectReady = false;
    mInstancedCacheValid = false;
    mCachedInstancedBatches.clear();
    mCachedOverrideEntities.clear();
    mTransformCpu.clear();
    mMotionCpu.clear();
    mSourceCpu.clear();
    mBatchDescsCpu.clear();
    mSubmeshDescsCpu.clear();
    mGpuCpuBatches.clear();
    mGpuOverrideEntities.clear();
    mEntityToGpuSlot.clear();
    mPendingTransformDirty.clear();
    mDummyHiZTexture.Reset();
    mSrvAlloc = nullptr;
    mDevice = nullptr;
}


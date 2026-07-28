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
// HELPERS — resource ensure/destroy, bind, frustum, readback
// Free (stateless) helpers: RenderDrawHelpers.h
// =============================================================================

void RenderSystem::BindSceneFrameState(
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    ID3D12RootSignature* sceneRS,
    bool bindPassAndShadow)
{
    if (descriptorAllocator)
    {
        ID3D12DescriptorHeap* heaps[] = { descriptorAllocator->GetHeap() };
        cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    }

    if (!bindPassAndShadow || !sceneRS)
        return;

    cmdList->SetGraphicsRootSignature(sceneRS);
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }
    BindShadowMapSrv(cmdList);
}

void RenderSystem::BindShadowMapSrv(ID3D12GraphicsCommandList* cmdList) const
{
    if (!cmdList || mShadowSrv.Index == UINT_MAX)
        return;
    cmdList->SetGraphicsRootDescriptorTable(4, mShadowSrv.GPU);
}

void RenderSystem::ExtractFrustumPlanes(const XMMATRIX& viewProj, float outPlanes[6][4])
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, viewProj);

    const float raw[6][4] = {
        { m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41 },
        { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 },
        { m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42 },
        { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 },
        { m._13, m._23, m._33, m._43 },
        { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 },
    };

    for (int i = 0; i < 6; ++i)
    {
        XMFLOAT4 f{ raw[i][0], raw[i][1], raw[i][2], raw[i][3] };
        XMVECTOR p = XMLoadFloat4(&f);
        XMVECTOR n = XMVectorSetW(p, 0.0f);
        float len = XMVectorGetX(XMVector3Length(n));
        if (len > 1e-6f)
            p = XMVectorScale(p, 1.0f / len);
        XMStoreFloat4(&f, p);
        outPlanes[i][0] = f.x;
        outPlanes[i][1] = f.y;
        outPlanes[i][2] = f.z;
        outPlanes[i][3] = f.w;
    }
}

UINT RenderSystem::FrameCBAlignedSize()
{
    return d3dUtil::CalcConstantBufferByteSize(sizeof(GpuDrivenFrameConstants));
}

void RenderSystem::EnsureGpuResources(ID3D12Device* device)
{
    if (mFrames[0].instanceBuffer)
        return;

    // Path2 InstanceWorld upload OR Path3 source/transform sizes
    const UINT64 reqBufSize = (std::max)(
        sizeof(GpuInstanceSource) * kMaxInstancesPerDraw,
        sizeof(InstanceWorld) * kMaxInstancesPerDraw);
    const UINT64 xformBufSize = sizeof(GpuTransform) * kMaxInstancesPerDraw;
    const UINT64 motionBufSize = sizeof(GpuMotion) * kMaxInstancesPerDraw;
    const UINT64 sourceBufSize = sizeof(GpuInstanceSource) * kMaxInstancesPerDraw;
    const UINT64 instBufSize = sizeof(InstanceWorld) * kMaxInstancesPerDraw;
    const UINT64 batchBufSize = sizeof(GpuBatchDesc) * kMaxGpuBatches;
    const UINT64 submeshBufSize = sizeof(GpuSubmeshDesc) * kMaxGpuSubmeshDraws;
    const UINT64 counterBufSize = sizeof(UINT) * kMaxGpuBatches;
    const UINT64 drawCmdBufSize = sizeof(IndirectCommand) * kMaxGpuSubmeshDraws;
    const UINT64 frameCbSize = FrameCBAlignedSize();
    const UINT64 counterReadbackSize = sizeof(UINT) * kMaxGpuBatches;

    for (UINT fi = 0; fi < kIndirectFrameCount; ++fi)
    {
        auto& f = mFrames[fi];

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(instBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.instanceBuffer)));
        f.instanceState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(counterBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.countBuffer)));
        f.countState = D3D12_RESOURCE_STATE_COMMON;

        // Step I: CPU-readable copy of batch counters (after GPU cull)
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(counterReadbackSize),
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&f.countReadback)));
        f.countReadbackPending = false;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(drawCmdBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.drawCmdBuffer)));
        f.drawCmdState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(reqBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.requestUpload)));
        ThrowIfFailed(f.requestUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.requestMapped)));

        // Step F1: TRS upload staging
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(xformBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.transformUpload)));
        ThrowIfFailed(f.transformUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.transformMapped)));

        // Step F2: motion seed upload
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(motionBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.motionUpload)));
        ThrowIfFailed(f.motionUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.motionMapped)));

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(batchBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.batchUpload)));
        ThrowIfFailed(f.batchUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.batchMapped)));

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(submeshBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.submeshUpload)));
        ThrowIfFailed(f.submeshUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.submeshMapped)));

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(frameCbSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.frameCBUpload)));
        ThrowIfFailed(f.frameCBUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.frameCBMapped)));

        // Step F1/F2: TRS DEFAULT (UAV for motion, SRV for compose)
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(xformBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.transformDefault)));
        f.transformDefaultState = D3D12_RESOURCE_STATE_COMMON;

        // Step F2: motion DEFAULT (persistent velocity on GPU)
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(motionBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.motionDefault)));
        f.motionDefaultState = D3D12_RESOURCE_STATE_COMMON;

        // Compose output / Cull input (UAV+SRV)
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(sourceBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.sourceDefault)));
        f.sourceDefaultState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(batchBufSize),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.batchDefault)));
        f.batchDefaultState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(submeshBufSize),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.submeshDefault)));
        f.submeshDefaultState = D3D12_RESOURCE_STATE_COMMON;
    }

    // 배치 카운???�괄 ?�로
    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(counterBufSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mCounterZeroUpload)));
    BYTE* mapped = nullptr;
    ThrowIfFailed(mCounterZeroUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    std::memset(mapped, 0, static_cast<size_t>(counterBufSize));
    mCounterZeroUpload->Unmap(0, nullptr);
}

void RenderSystem::DestroyGpuResources()
{
    for (UINT i = 0; i < kIndirectFrameCount; ++i)
    {
        auto& f = mFrames[i];
        if (f.requestUpload && f.requestMapped)
        {
            f.requestUpload->Unmap(0, nullptr);
            f.requestMapped = nullptr;
        }
        if (f.transformUpload && f.transformMapped)
        {
            f.transformUpload->Unmap(0, nullptr);
            f.transformMapped = nullptr;
        }
        if (f.motionUpload && f.motionMapped)
        {
            f.motionUpload->Unmap(0, nullptr);
            f.motionMapped = nullptr;
        }
        if (f.batchUpload && f.batchMapped)
        {
            f.batchUpload->Unmap(0, nullptr);
            f.batchMapped = nullptr;
        }
        if (f.submeshUpload && f.submeshMapped)
        {
            f.submeshUpload->Unmap(0, nullptr);
            f.submeshMapped = nullptr;
        }
        if (f.frameCBUpload && f.frameCBMapped)
        {
            f.frameCBUpload->Unmap(0, nullptr);
            f.frameCBMapped = nullptr;
        }
        f.requestUpload.Reset();
        f.transformUpload.Reset();
        f.motionUpload.Reset();
        f.batchUpload.Reset();
        f.submeshUpload.Reset();
        f.frameCBUpload.Reset();
        f.transformDefault.Reset();
        f.motionDefault.Reset();
        f.sourceDefault.Reset();
        f.batchDefault.Reset();
        f.submeshDefault.Reset();
        f.instanceBuffer.Reset();
        f.countBuffer.Reset();
        f.drawCmdBuffer.Reset();
        f.countReadback.Reset();
        f.countReadbackPending = false;
        f.countReadbackBatches = 0;
        f.countReadbackSources = 0;
        f.instanceState = D3D12_RESOURCE_STATE_COMMON;
        f.countState = D3D12_RESOURCE_STATE_COMMON;
        f.drawCmdState = D3D12_RESOURCE_STATE_COMMON;
        f.transformDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.motionDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.sourceDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.batchDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.submeshDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.uploadedTransformVersion = 0;
        f.uploadedMotionVersion = 0;
        f.composedTransformVersion = 0;
        f.uploadedMetaVersion = 0;
    }
    mCounterZeroUpload.Reset();
    mDummyInstanceBuffer.Reset();
}

void RenderSystem::EnsureShadowMap(DescriptorAllocator* alloc)
{
    if (mShadowMap || !mDevice || !alloc)
        return;

    mSrvAlloc = alloc;
    if (mShadowSrv.Index == UINT_MAX)
        mShadowSrv = alloc->Allocate();

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kShadowMapSize;
    desc.Height = kShadowMapSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear,
        IID_PPV_ARGS(&mShadowMap)));
    mShadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mShadowDsvHeap)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    mDevice->CreateDepthStencilView(
        mShadowMap.Get(), &dsvDesc, mShadowDsvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    mDevice->CreateShaderResourceView(mShadowMap.Get(), &srvDesc, mShadowSrv.CPU);
}

void RenderSystem::DestroyShadowMap(DescriptorAllocator* alloc)
{
    if (alloc && mShadowSrv.Index != UINT_MAX)
    {
        alloc->Free(mShadowSrv);
        mShadowSrv = {};
    }
    mShadowMap.Reset();
    mShadowDsvHeap.Reset();
    mShadowState = D3D12_RESOURCE_STATE_COMMON;
}

bool RenderSystem::IsHiZSampleReady() const
{
    // GPU가 최�? (kHiZRingSize-1) ?�레???�처�????�으므�?    // 링을 ??바�?채운 ?�에�??�플 ?�용
    return mHiZBuildCount >= kHiZRingSize
        && mHiZRing[0].texture != nullptr;
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderSystem::GetHiZSampleSrvGpu() const
{
    if (!IsHiZSampleReady())
    {
        if (mDummyHiZSrv.Index != UINT_MAX)
            return mDummyHiZSrv.GPU;
        return {};
    }
    // mHiZWriteSlot = ?�음????�?    // 마�?�?기록 = writeSlot-1, ?�전???�플 = writeSlot+1 (= 2?�레???? ring=3)
    const UINT readSlot = (mHiZWriteSlot + 1u) % kHiZRingSize;
    return mHiZRing[readSlot].srv.GPU;
}

void RenderSystem::EnsureHiZResources(DescriptorAllocator* alloc, UINT width, UINT height)
{
    if (!mDevice || !alloc)
        return;

    width = (std::max)(1u, width);
    height = (std::max)(1u, height);

    if (mHiZRing[0].texture && mHiZWidth == width && mHiZHeight == height)
        return;

    // Size mismatch with live resources: do NOT destroy in-flight (TDR).
    // Wait for PrepareHiZForSceneSize after GPU flush.
    if (mHiZRing[0].texture)
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            OutputDebugStringA(
                "[RenderSystem] HiZ size mismatch mid-flight; skip recreate until GPU idle prepare.\n");
            sLogged = true;
        }
        return;
    }

    mSrvAlloc = alloc;
    mHiZMipCount = 1;
    mHiZWidth = width;
    mHiZHeight = height;
    mHiZWriteSlot = 0;
    mHiZBuildCount = 0;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip = 0;
    srv.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice = 0;

    for (UINT i = 0; i < kHiZRingSize; ++i)
    {
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&mHiZRing[i].texture)));
        mHiZRing[i].state = D3D12_RESOURCE_STATE_COMMON;

        mHiZRing[i].srv = alloc->Allocate();
        mDevice->CreateShaderResourceView(mHiZRing[i].texture.Get(), &srv, mHiZRing[i].srv.CPU);

        mHiZRing[i].uav = alloc->Allocate();
        mDevice->CreateUnorderedAccessView(
            mHiZRing[i].texture.Get(), nullptr, &uav, mHiZRing[i].uav.CPU);
    }

    EnsureDummyHiZSrv(alloc);
}

void RenderSystem::EnsureDummyHiZSrv(DescriptorAllocator* alloc)
{
    if (!mDevice || !alloc)
        return;
    if (mDummyHiZSrv.Index != UINT_MAX && mDummyHiZTexture)
        return;

    if (!mDummyHiZTexture)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = 1;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &d,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&mDummyHiZTexture)));
    }

    if (mDummyHiZSrv.Index == UINT_MAX)
    {
        mDummyHiZSrv = alloc->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        mDevice->CreateShaderResourceView(mDummyHiZTexture.Get(), &srv, mDummyHiZSrv.CPU);
    }
}

void RenderSystem::DestroyHiZResources(DescriptorAllocator* alloc)
{
    if (alloc)
    {
        for (UINT i = 0; i < kHiZRingSize; ++i)
        {
            if (mHiZRing[i].srv.Index != UINT_MAX)
            {
                alloc->Free(mHiZRing[i].srv);
                mHiZRing[i].srv = {};
            }
            if (mHiZRing[i].uav.Index != UINT_MAX)
            {
                alloc->Free(mHiZRing[i].uav);
                mHiZRing[i].uav = {};
            }
            mHiZRing[i].texture.Reset();
            mHiZRing[i].state = D3D12_RESOURCE_STATE_COMMON;
        }
    }
    else
    {
        for (UINT i = 0; i < kHiZRingSize; ++i)
        {
            mHiZRing[i].texture.Reset();
            mHiZRing[i].srv = {};
            mHiZRing[i].uav = {};
            mHiZRing[i].state = D3D12_RESOURCE_STATE_COMMON;
        }
    }
    mHiZWidth = mHiZHeight = 0;
    mHiZMipCount = 1;
    mHiZWriteSlot = 0;
    mHiZBuildCount = 0;
}

void RenderSystem::ResolveGpuCullReadback(FrameGpuResources& frame)
{
    mLastStats.gpuCullReadbackValid = false;
    mLastStats.gpuVisibleInstances = 0;
    mLastStats.gpuSubmittedInstances = 0;
    mLastStats.gpuCulledInstances = 0;

    if (!frame.countReadback || !frame.countReadbackPending)
        return;

    const UINT numBatches = (std::min)(frame.countReadbackBatches, kMaxGpuBatches);
    if (numBatches == 0)
    {
        frame.countReadbackPending = false;
        return;
    }

    const UINT64 bytes = sizeof(UINT) * numBatches;
    D3D12_RANGE range{ 0, bytes };
    void* mapped = nullptr;
    if (FAILED(frame.countReadback->Map(0, &range, &mapped)) || !mapped)
        return;

    const UINT* counters = static_cast<const UINT*>(mapped);
    uint32_t visible = 0;
    for (UINT i = 0; i < numBatches; ++i)
    {
        uint32_t n = counters[i];
        if (i < mBatchDescsCpu.size() && n > mBatchDescsCpu[i].instanceCount)
            n = mBatchDescsCpu[i].instanceCount;
        visible += n;
    }

    D3D12_RANGE empty{};
    frame.countReadback->Unmap(0, &empty);

    const uint32_t submitted = frame.countReadbackSources;
    mLastStats.gpuCullReadbackValid = true;
    mLastStats.gpuVisibleInstances = visible;
    mLastStats.gpuSubmittedInstances = submitted;
    mLastStats.gpuCulledInstances = (submitted > visible) ? (submitted - visible) : 0;
    // Keep pending true so we keep showing last good until next copy overwrites
    // Actually after read, next Schedule will overwrite ??pending stays true after schedule
}

void RenderSystem::ScheduleGpuCullReadback(
    ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame,
    UINT numBatches, UINT numSources)
{
    if (!cmdList || !frame.countBuffer || !frame.countReadback || numBatches == 0)
        return;

    numBatches = (std::min)(numBatches, kMaxGpuBatches);
    const UINT64 bytes = sizeof(UINT) * numBatches;

    if (frame.countState != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            frame.countBuffer.Get(), frame.countState, D3D12_RESOURCE_STATE_COPY_SOURCE));
        frame.countState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    cmdList->CopyBufferRegion(
        frame.countReadback.Get(), 0,
        frame.countBuffer.Get(), 0,
        bytes);

    frame.countReadbackPending = true;
    frame.countReadbackBatches = numBatches;
    frame.countReadbackSources = numSources;
}


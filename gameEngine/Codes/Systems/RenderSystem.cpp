#include <DirectXMath.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <vector>
#include <unordered_set>
#include <cstring>
#include "RenderSystem.h"
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

namespace
{
    float ElapsedMs(std::chrono::high_resolution_clock::time_point t0)
    {
        using namespace std::chrono;
        return duration<float, std::milli>(high_resolution_clock::now() - t0).count();
    }
}

namespace
{
    void StoreWorldTransposed(const XMMATRIX& world, float out[16])
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixTranspose(world));
        std::memcpy(out, &m, sizeof(XMFLOAT4X4));
    }

    Material* ResolveSubmeshMaterial(const SubmeshGeometry& sub)
    {
        Material* material = sub.initMaterial;
        if (!material || !material->HasValidTexture())
        {
            if (!sub.initMaterialName.empty())
            {
                if (auto m = MaterialManager::Get().GetMaterial(sub.initMaterialName))
                    material = m->HasValidTexture() ? m.get() : nullptr;
            }
            if (!material)
                material = MaterialManager::Get().GetMissingTextureMaterial();
        }
        if (!material || !material->HasValidTexture())
            return nullptr;
        return material;
    }

    bool HasEntitySubMaterialOverride(Entity e)
    {
        const EntityMaterialData* data = MaterialManager::Get().GetEntityMaterialData(e);
        if (!data)
            return false;
        for (const auto& p : data->subMaterialNames)
        {
            if (!p.second.empty())
                return true;
        }
        return false;
    }

    Material* GetEntityMainMaterialOverride(Entity e)
    {
        const EntityMaterialData* data = MaterialManager::Get().GetEntityMaterialData(e);
        if (!data || data->mainMaterialName.empty())
            return nullptr;
        auto mat = MaterialManager::Get().GetMaterial(data->mainMaterialName);
        if (!mat || !mat->HasValidTexture())
            return nullptr;
        return mat.get();
    }

    Material* ResolveBatchMaterial(Material* mainOverride, const SubmeshGeometry& sub)
    {
        if (mainOverride && mainOverride->HasValidTexture())
            return mainOverride;
        return ResolveSubmeshMaterial(sub);
    }

    void DrawEntityWithResolvedMaterials(
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* frame,
        Entity e,
        TransformComponent& tf,
        RenderableComponent& rend)
    {
        if (!rend.visible || !rend.mesh)
            return;
        if (rend.objectCBIndex >= kMaxSceneObjects)
            return;

        if (tf.dirtyFrames > 0)
        {
            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf.GetWorldMatrix()));
            frame->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);
            --tf.dirtyFrames;
        }

        auto objectCB = frame->ObjectCB->Resource();
        const UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS objCBAddress =
            objectCB->GetGPUVirtualAddress() + (UINT64)rend.objectCBIndex * objCBByteSize;

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
            // Step E: heap Index → GPU handle (table of 1)
            if (material->mTextureHandle.Index != UINT_MAX)
            {
                cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);
            }
            cmdList->DrawIndexedInstanced(
                sub.IndexCount, 1,
                sub.StartIndexLocation,
                sub.BaseVertexLocation, 0);
        }
    }

    // Step E: bind SRV table slot by descriptor heap index (skip if same as last)
    bool BindMaterialByIndex(
        ID3D12GraphicsCommandList* cmdList,
        DescriptorAllocator* descriptorAllocator,
        UINT materialIndex,
        UINT& inoutLastIndex)
    {
        if (!descriptorAllocator || materialIndex == UINT_MAX)
            return false;
        if (materialIndex == inoutLastIndex)
            return true;

        D3D12_GPU_DESCRIPTOR_HANDLE h =
            descriptorAllocator->GetHeap()->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(materialIndex) * descriptorAllocator->GetDescriptorSize();
        cmdList->SetGraphicsRootDescriptorTable(2, h);
        inoutLastIndex = materialIndex;
        return true;
    }

    void FillSourceFromEntity(
        GpuInstanceSource& out,
        TransformComponent& tf,
        RenderableComponent& rend,
        BoundsComponent* bounds,
        UINT batchId)
    {
        StoreWorldTransposed(tf.GetWorldMatrix(), out.worldMatrix);
        out.batchId = batchId;
        out.flags = rend.visible ? 1u : 0u;
        out.pad2 = 0;
        out.pad3 = 0;
        if (bounds)
        {
            out.boundsCenterX = bounds->worldBounds.Center.x;
            out.boundsCenterY = bounds->worldBounds.Center.y;
            out.boundsCenterZ = bounds->worldBounds.Center.z;
            out.boundsExtentsX = bounds->worldBounds.Extents.x;
            out.boundsExtentsY = bounds->worldBounds.Extents.y;
            out.boundsExtentsZ = bounds->worldBounds.Extents.z;
        }
        else
        {
            out.boundsCenterX = out.boundsCenterY = out.boundsCenterZ = 0.f;
            out.boundsExtentsX = out.boundsExtentsY = out.boundsExtentsZ = 0.f;
        }
        out.boundsPad0 = 0.f;
        out.boundsPad1 = 0.f;
    }
}

UINT RenderSystem::FrameCBAlignedSize()
{
    return d3dUtil::CalcConstantBufferByteSize(sizeof(GpuDrivenFrameConstants));
}

void RenderSystem::InvalidateDrawCache()
{
    mInstancedCacheValid = false;
    mGpuStructureDirty = true;
    mPendingTransformDirty.clear();
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

    const char* name = "Unknown";
    switch (path)
    {
    case RenderPath::Basic: name = "Basic"; break;
    case RenderPath::Instanced: name = "Instanced"; break;
    case RenderPath::ComputeIndirect: name = "ComputeIndirect(GPU-driven)"; break;
    }
    char buf[160];
    sprintf_s(buf, "[RenderSystem] SetRenderPath -> %s\n", name);
    OutputDebugStringA(buf);

    mRenderPath = path;
    if (path == RenderPath::ComputeIndirect)
        mGpuStructureDirty = true;
}

void RenderSystem::Initialize(ID3D12Device* device)
{
    mDevice = device;
    if (!mDevice)
        return;

    EnsureGpuResources(mDevice);

    ID3D12RootSignature* buildRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::IndirectBuild);

    auto cullBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\build_indirect_commands.hlsl", "CS_CullCompact", "cs_5_1");
    auto buildBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\build_indirect_commands.hlsl", "CS_BuildCommands", "cs_5_1");

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
    else
        OutputDebugStringA("[RenderSystem] Path3 GPU-driven ready (CullCompact + BuildCommands + HiZ).\n");

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
        f.batchUpload.Reset();
        f.submeshUpload.Reset();
        f.frameCBUpload.Reset();
        f.sourceDefault.Reset();
        f.batchDefault.Reset();
        f.submeshDefault.Reset();
        f.instanceBuffer.Reset();
        f.countBuffer.Reset();
        f.drawCmdBuffer.Reset();
        f.instanceState = D3D12_RESOURCE_STATE_COMMON;
        f.countState = D3D12_RESOURCE_STATE_COMMON;
        f.drawCmdState = D3D12_RESOURCE_STATE_COMMON;
        f.sourceDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.batchDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.submeshDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.uploadedSourceVersion = 0;
        f.uploadedMetaVersion = 0;
    }
    mCounterZeroUpload.Reset();
    mDummyInstanceBuffer.Reset();
}

void RenderSystem::Shutdown()
{
    DestroyHiZResources(mSrvAlloc);
    DestroyGpuResources();
    mCullCompactPSO.Reset();
    mBuildCommandsPSO.Reset();
    mHiZCopyPSO.Reset();
    mHiZDownsamplePSO.Reset();
    mComputeIndirectReady = false;
    mInstancedCacheValid = false;
    mCachedInstancedBatches.clear();
    mCachedOverrideEntities.clear();
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

void RenderSystem::EnsureGpuResources(ID3D12Device* device)
{
    if (mFrames[0].instanceBuffer)
        return;

    // Path2 InstanceWorld upload OR Path3 GpuInstanceSource — take max
    const UINT64 reqBufSize = (std::max)(
        sizeof(GpuInstanceSource) * kMaxInstancesPerDraw,
        sizeof(InstanceWorld) * kMaxInstancesPerDraw);
    const UINT64 instBufSize = sizeof(InstanceWorld) * kMaxInstancesPerDraw;
    const UINT64 batchBufSize = sizeof(GpuBatchDesc) * kMaxGpuBatches;
    const UINT64 submeshBufSize = sizeof(GpuSubmeshDesc) * kMaxGpuSubmeshDraws;
    const UINT64 counterBufSize = sizeof(UINT) * kMaxGpuBatches;
    const UINT64 drawCmdBufSize = sizeof(IndirectCommand) * kMaxGpuSubmeshDraws;
    const UINT64 frameCbSize = FrameCBAlignedSize();

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

        // Step C: DEFAULT 힙 — CS SRV 읽기용 (프레임별 트리플버퍼)
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(reqBufSize),
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

    // 배치 카운터 일괄 제로
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

// ---------------------------------------------------------------------------
// Path3: rebuild contiguous (mesh, material) batches + source mirror
// ---------------------------------------------------------------------------
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
            if (!rend.mesh) return;
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

    mSourceCpu.clear();
    mBatchDescsCpu.clear();
    mSubmeshDescsCpu.clear();
    mGpuCpuBatches.clear();
    mEntityToGpuSlot.clear();

    mSourceCpu.reserve((std::min)(entries.size(), static_cast<size_t>(kMaxInstancesPerDraw)));

    UINT slot = 0;
    size_t i = 0;
    while (i < entries.size() && slot < kMaxInstancesPerDraw
        && mGpuCpuBatches.size() < kMaxGpuBatches)
    {
        Mesh* mesh = entries[i].mesh;
        Material* mainMat = entries[i].mainMat;
        const size_t groupBegin = i;

        // 같은 (mesh, material) 그룹 범위 확정
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

        // 서브메시/머티리얼 먼저 해석 — 유효할 때만 소스 커밋
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
            sd.batchId = 0; // 아래에서 채움
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
            GpuInstanceSource src{};
            FillSourceFromEntity(src, *en.tf, *en.rend, en.bounds, batchId);
            mEntityToGpuSlot[en.entity] = slot;
            mSourceCpu.push_back(src);
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

    ++mSourceContentVersion;
    ++mMetaVersion;
    mGpuStructureDirty = false;
}

bool RenderSystem::PatchOneGpuEntity(
    World& world, FrameResource* frameResource, Entity e, bool& anyPatched)
{
    auto* tf = world.GetComponent<TransformComponent>(e);
    auto* rend = world.GetComponent<RenderableComponent>(e);
    if (!tf || !rend || !rend->mesh)
        return false;

    auto it = mEntityToGpuSlot.find(e);
    if (it == mEntityToGpuSlot.end())
        return false;

    const UINT slot = it->second;
    if (slot >= mSourceCpu.size())
        return false;

    if (tf->dirtyFrames <= 0)
    {
        const uint32_t want = rend->visible ? 1u : 0u;
        if ((mSourceCpu[slot].flags & 1u) != want)
        {
            mSourceCpu[slot].flags = want;
            anyPatched = true;
        }
        return false; // no longer pending
    }

    BoundsComponent* bounds = world.GetComponent<BoundsComponent>(e);
    FillSourceFromEntity(
        mSourceCpu[slot], *tf, *rend, bounds, mSourceCpu[slot].batchId);

    if (frameResource && frameResource->ObjectCB
        && rend->objectCBIndex < kMaxSceneObjects)
    {
        ObjectConstants objConst{};
        XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf->GetWorldMatrix()));
        frameResource->ObjectCB->CopyData(static_cast<int>(rend->objectCBIndex), objConst);
    }
    --tf->dirtyFrames;
    anyPatched = true;
    return tf->dirtyFrames > 0;
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

    // MarkDirty() without entity → generation only: full scan once
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
    std::vector<Entity> stillPending;
    stillPending.reserve(mPendingTransformDirty.size());
    uint32_t patched = 0;

    for (Entity e : mPendingTransformDirty)
    {
        bool touched = false;
        const bool keep = PatchOneGpuEntity(world, frameResource, e, touched);
        if (touched)
        {
            anyPatched = true;
            ++patched;
        }
        if (keep)
            stillPending.push_back(e);
    }

    mPendingTransformDirty = std::move(stillPending);
    mLastProcessedDirtyGen = gen;
    mLastStats.dirtyPatched = patched;
    mLastStats.pendingDirty = static_cast<uint32_t>(mPendingTransformDirty.size());

    if (anyPatched)
        ++mSourceContentVersion;

    mLastStats.patchMs = ElapsedMs(t0);
}

void RenderSystem::UploadGpuDrivenFrameData(
    ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame)
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    mLastStats.didSourceUpload = false;
    mLastStats.didMetaUpload = false;
    mLastStats.usedDefaultHeapCopy = false;

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& st, D3D12_RESOURCE_STATES target)
    {
        if (st != target)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(res, st, target));
            st = target;
        }
    };

    // Source: staging memcpy → DEFAULT copy (프레임 버전 다를 때만)
    if (frame.uploadedSourceVersion != mSourceContentVersion && frame.requestMapped)
    {
        const UINT64 bytes = sizeof(GpuInstanceSource) * mSourceCpu.size();
        if (!mSourceCpu.empty() && bytes > 0)
        {
            std::memcpy(frame.requestMapped, mSourceCpu.data(), static_cast<size_t>(bytes));
            transition(frame.sourceDefault.Get(), frame.sourceDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                frame.sourceDefault.Get(), 0,
                frame.requestUpload.Get(), 0,
                bytes);
            transition(frame.sourceDefault.Get(), frame.sourceDefaultState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            mLastStats.usedDefaultHeapCopy = true;
        }
        frame.uploadedSourceVersion = mSourceContentVersion;
        mLastStats.didSourceUpload = true;
    }
    else
    {
        // CS가 읽을 수 있게 SRV 상태 보장
        transition(frame.sourceDefault.Get(), frame.sourceDefaultState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
}

void RenderSystem::render(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
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

// ---------------------------------------------------------------------------
// Path 1: Basic
// ---------------------------------------------------------------------------
void RenderSystem::renderBasic(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    (void)viewMatrix;
    (void)projMatrix;

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    PSOKey key{};
    key.shaderName = "object_cb";
    key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);

    if (sceneRS)
        cmdList->SetGraphicsRootSignature(sceneRS);
    if (pso)
        cmdList->SetPipelineState(pso);

    if (mDummyInstanceBuffer)
        cmdList->SetGraphicsRootShaderResourceView(3, mDummyInstanceBuffer->GetGPUVirtualAddress());

    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    UINT lastMat = UINT_MAX;
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
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

// ---------------------------------------------------------------------------
// Path 2 helpers
// ---------------------------------------------------------------------------
void RenderSystem::DrawInstancedBatches(
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    FrameGpuResources& frame,
    DescriptorAllocator* descriptorAllocator,
    const std::vector<Entity>& overrideEntities,
    World& world)
{
    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    if (!sceneRS)
        return;

    cmdList->SetGraphicsRootSignature(sceneRS);
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    UINT lastMat = UINT_MAX;

    if (!overrideEntities.empty())
    {
        PSOKey basicKey{};
        basicKey.shaderName = "object_cb";
        basicKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        basicKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        basicKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        if (ID3D12PipelineState* basicPso = PipelineStateManager::Get().GetOrCreatePSO(basicKey, sceneRS))
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
        lastMat = UINT_MAX; // PSO/path switch — force rebind
    }

    if (mCachedInstancedBatches.empty())
        return;

    PSOKey gfxKey{};
    gfxKey.shaderName = "object_instanced";
    gfxKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    gfxKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    gfxKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    ID3D12PipelineState* gfxPSO = PipelineStateManager::Get().GetOrCreatePSO(gfxKey, sceneRS);
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
    lastMat = UINT_MAX;

    BYTE* upload = frame.requestMapped;
    UINT uploadCursor = 0;

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

        uploadCursor += n;
    }
}

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

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    if (!sceneRS)
    {
        renderBasic(world, cmdList, currentFrameResource, descriptorAllocator, viewMatrix, projMatrix);
        return;
    }

    // Step B: dirty generation + pending 없으면 캐시 재사용 (ForEach 스킵)
    {
        auto incoming = TransformDirtyTracker::TakeEntities();
        if (!incoming.empty())
            mPendingTransformDirty.insert(
                mPendingTransformDirty.end(), incoming.begin(), incoming.end());

        const uint32_t gen = TransformDirtyTracker::Generation();
        const bool staticScene =
            mInstancedCacheValid
            && mPendingTransformDirty.empty()
            && gen == mPath2CachedDirtyGen;

        if (staticScene)
        {
            mLastStats = {};
            mLastStats.path = RenderPath::Instanced;
            mLastStats.skippedPatch = true;
            mLastStats.sourceCount = 0;
            for (const auto& b : mCachedInstancedBatches)
                mLastStats.sourceCount += static_cast<uint32_t>(b.instances.size());
            mLastStats.batchCount = static_cast<uint32_t>(mCachedInstancedBatches.size());
            DrawInstancedBatches(cmdList, currentFrameResource, frame,
                descriptorAllocator, mCachedOverrideEntities, world);
            return;
        }

        if (mInstancedCacheValid)
            mInstancedCacheValid = false;
    }

    std::map<std::pair<Mesh*, Material*>, std::vector<InstanceWorld>> batchMap;
    std::vector<Entity> overrideEntities;
    UINT totalObjects = 0;

    mPendingTransformDirty.clear();
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
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
                // multi-frame ObjectCB: 남은 dirty는 다음 프레임에도 캐시 무효
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

    mLastStats = {};
    mLastStats.path = RenderPath::Instanced;
    mLastStats.pendingDirty = static_cast<uint32_t>(mPendingTransformDirty.size());
    mLastStats.batchCount = static_cast<uint32_t>(mCachedInstancedBatches.size());
    for (const auto& b : mCachedInstancedBatches)
        mLastStats.sourceCount += static_cast<uint32_t>(b.instances.size());

    DrawInstancedBatches(cmdList, currentFrameResource, frame,
        descriptorAllocator, mCachedOverrideEntities, world);
}

// ---------------------------------------------------------------------------
// Path 3: GPU-driven — persistent source + global cull/compact + EI
// ---------------------------------------------------------------------------
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

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mSrvAlloc = descriptorAllocator;
    mLastStats = {};
    mLastStats.path = RenderPath::ComputeIndirect;
    mLastStats.cullEnabled = mGpuFrustumCull;
    mLastStats.occlusionEnabled = mGpuOcclusion;
    mLastStats.hizValid = IsHiZSampleReady();
    mLastStats.hizMips = mHiZMipCount;

    // 1) CPU: 구조 변경 시 배치 재빌드 + dirty 슬롯 패치
    if (mGpuStructureDirty)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        RebuildGpuDrivenScene(world);
        mLastStats.didRebuild = true;
        mLastStats.rebuildMs = ElapsedMs(t0);
        // 소스 미러는 최신. multi-frame ObjectCB용 dirtyFrames만 pending 유지.
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

    mLastStats.sourceCount = static_cast<uint32_t>(mSourceCpu.size());
    mLastStats.batchCount = static_cast<uint32_t>(mBatchDescsCpu.size());
    mLastStats.submeshDraws = static_cast<uint32_t>(mSubmeshDescsCpu.size());

    // Sub-override는 Basic으로
    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    if (sceneRS && !mGpuOverrideEntities.empty())
    {
        cmdList->SetGraphicsRootSignature(sceneRS);
        if (currentFrameResource && currentFrameResource->PassCB)
        {
            cmdList->SetGraphicsRootConstantBufferView(
                1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
        }
        PSOKey basicKey{};
        basicKey.shaderName = "object_cb";
        basicKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        basicKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        basicKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        if (ID3D12PipelineState* basicPso = PipelineStateManager::Get().GetOrCreatePSO(basicKey, sceneRS))
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

    if (mSourceCpu.empty() || mGpuCpuBatches.empty() || mSubmeshDescsCpu.empty())
        return;

    UploadGpuDrivenFrameData(cmdList, frame);

    // 2) Frame CB (카메라/컬링/오클루전 — 매 프레임)
    const XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
    mLastViewProj = viewProj;
    mLastRtWidth = (mHiZWidth > 0) ? mHiZWidth : 1;
    mLastRtHeight = (mHiZHeight > 0) ? mHiZHeight : 1;

    GpuDrivenFrameConstants cb{};
    ExtractFrustumPlanes(viewProj, cb.frustumPlanes);
    cb.numInstances = static_cast<uint32_t>(mSourceCpu.size());
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

    ID3D12RootSignature* buildRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::IndirectBuild);
    ID3D12CommandSignature* cmdSig = RootSignatureManager::Get().GetSceneCommandSignature();

    PSOKey gfxKey{};
    gfxKey.shaderName = "object_instanced";
    gfxKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    gfxKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    gfxKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    ID3D12PipelineState* gfxPSO = PipelineStateManager::Get().GetOrCreatePSO(gfxKey, sceneRS);

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

    // 3) 카운터 제로 (배치 전체 1회)
    {
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
    }

    // UAV 전환
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

    // 4) 전역 CullCompact 1회 (DEFAULT 힙 source/batch/submesh + HiZ t3)
    cmdList->SetComputeRootSignature(buildRS);
    cmdList->SetComputeRootConstantBufferView(0, frame.frameCBUpload->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(1, frame.sourceDefault->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(2, frame.batchDefault->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(3, frame.submeshDefault->GetGPUVirtualAddress());

    // Hi-Z: 링에서 "충분히 오래된" 슬롯만 샘플 (in-flight 쓰기와 분리)
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
        return;
    }
    cmdList->SetComputeRootDescriptorTable(4, hizGpu);

    cmdList->SetComputeRootUnorderedAccessView(5, frame.instanceBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(6, frame.countBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(7, frame.drawCmdBuffer->GetGPUVirtualAddress());

    cmdList->SetPipelineState(mCullCompactPSO.Get());
    const UINT numInst = static_cast<UINT>((std::min)(
        mSourceCpu.size(), static_cast<size_t>(kMaxInstancesPerDraw)));
    if (numInst == 0)
        return;
    cmdList->Dispatch((numInst + 63u) / 64u, 1, 1);

    {
        D3D12_RESOURCE_BARRIER uav[2];
        uav[0] = CD3DX12_RESOURCE_BARRIER::UAV(frame.instanceBuffer.Get());
        uav[1] = CD3DX12_RESOURCE_BARRIER::UAV(frame.countBuffer.Get());
        cmdList->ResourceBarrier(2, uav);
    }

    // 5) 전역 BuildCommands 1회
    cmdList->SetPipelineState(mBuildCommandsPSO.Get());
    const UINT numSubs = static_cast<UINT>((std::min)(
        mSubmeshDescsCpu.size(), static_cast<size_t>(kMaxGpuSubmeshDraws)));
    if (numSubs == 0)
        return;
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

    // 6) Step E: material heap Index로 테이블 바인딩 (동일 인덱스면 스킵)
    cmdList->SetGraphicsRootSignature(sceneRS);
    cmdList->SetPipelineState(gfxPSO);
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

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

        for (UINT si = 0; si < batch.submeshCount; ++si)
        {
            const UINT cmdIndex = batch.firstSubmesh + si;
            if (cmdIndex >= mSubmeshDescsCpu.size())
                break;
            if (si >= batch.materialIndices.size())
                break;

            BindMaterialByIndex(cmdList, descriptorAllocator,
                batch.materialIndices[si], lastMat);

            const UINT64 argOffset = (UINT64)cmdIndex * sizeof(IndirectCommand);
            cmdList->ExecuteIndirect(
                cmdSig, 1,
                frame.drawCmdBuffer.Get(), argOffset,
                nullptr, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Step D: Hierarchical-Z (triple-buffered ring)
// ---------------------------------------------------------------------------
bool RenderSystem::IsHiZSampleReady() const
{
    // GPU가 최대 (kHiZRingSize-1) 프레임 뒤처질 수 있으므로
    // 링을 한 바퀴 채운 뒤에만 샘플 허용
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
    // mHiZWriteSlot = 다음에 쓸 칸
    // 마지막 기록 = writeSlot-1, 안전한 샘플 = writeSlot+1 (= 2프레임 전, ring=3)
    const UINT readSlot = (mHiZWriteSlot + 1u) % kHiZRingSize;
    return mHiZRing[readSlot].srv.GPU;
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

    // 이 슬롯만 쓰기 — 샘플 중인 다른 슬롯과 분리
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

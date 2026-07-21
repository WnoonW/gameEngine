#include <DirectXMath.h>
#include <algorithm>
#include <map>
#include <vector>
#include <cstring>
#include "RenderSystem.h"
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
            cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);
            cmdList->DrawIndexedInstanced(
                sub.IndexCount, 1,
                sub.StartIndexLocation,
                sub.BaseVertexLocation, 0);
        }
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

    if (!mComputeIndirectReady)
        OutputDebugStringA("[RenderSystem] Path3 GPU-driven unavailable (CS/PSO). Path1/2 OK.\n");
    else
        OutputDebugStringA("[RenderSystem] Path3 GPU-driven ready (CullCompact + BuildCommands).\n");

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
        f.instanceBuffer.Reset();
        f.countBuffer.Reset();
        f.drawCmdBuffer.Reset();
        f.instanceState = D3D12_RESOURCE_STATE_COMMON;
        f.countState = D3D12_RESOURCE_STATE_COMMON;
        f.drawCmdState = D3D12_RESOURCE_STATE_COMMON;
        f.uploadedSourceVersion = 0;
        f.uploadedMetaVersion = 0;
    }
    mCounterZeroUpload.Reset();
    mDummyInstanceBuffer.Reset();
}

void RenderSystem::Shutdown()
{
    DestroyGpuResources();
    mCullCompactPSO.Reset();
    mBuildCommandsPSO.Reset();
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
        std::vector<UINT64> localMats;
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
            localMats.push_back(material->mTextureHandle.GPU.ptr);
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
        cpuBatch.materialGpuPtrs = std::move(localMats);

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

void RenderSystem::PatchGpuDrivenTransforms(World& world, FrameResource* frameResource)
{
    bool anyPatched = false;

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.mesh) return;

            auto it = mEntityToGpuSlot.find(e);
            if (it == mEntityToGpuSlot.end())
                return;

            const UINT slot = it->second;
            if (slot >= mSourceCpu.size())
                return;

            // dirty transform 또는 visible 변경 반영
            if (tf.dirtyFrames <= 0)
            {
                // visible 플래그만 동기화
                const uint32_t want = rend.visible ? 1u : 0u;
                if ((mSourceCpu[slot].flags & 1u) != want)
                {
                    mSourceCpu[slot].flags = want;
                    anyPatched = true;
                }
                return;
            }

            BoundsComponent* bounds = world.GetComponent<BoundsComponent>(e);
            FillSourceFromEntity(
                mSourceCpu[slot], tf, rend, bounds, mSourceCpu[slot].batchId);

            if (frameResource && frameResource->ObjectCB
                && rend.objectCBIndex < kMaxSceneObjects)
            {
                ObjectConstants objConst{};
                XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf.GetWorldMatrix()));
                frameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);
            }
            --tf.dirtyFrames;
            anyPatched = true;
        });

    if (anyPatched)
        ++mSourceContentVersion;
}

void RenderSystem::UploadGpuDrivenFrameData(FrameGpuResources& frame)
{
    if (frame.uploadedSourceVersion != mSourceContentVersion && frame.requestMapped)
    {
        if (!mSourceCpu.empty())
        {
            std::memcpy(
                frame.requestMapped,
                mSourceCpu.data(),
                sizeof(GpuInstanceSource) * mSourceCpu.size());
        }
        frame.uploadedSourceVersion = mSourceContentVersion;
    }

    if (frame.uploadedMetaVersion != mMetaVersion)
    {
        if (frame.batchMapped && !mBatchDescsCpu.empty())
        {
            std::memcpy(
                frame.batchMapped,
                mBatchDescsCpu.data(),
                sizeof(GpuBatchDesc) * mBatchDescsCpu.size());
        }
        if (frame.submeshMapped && !mSubmeshDescsCpu.empty())
        {
            std::memcpy(
                frame.submeshMapped,
                mSubmeshDescsCpu.data(),
                sizeof(GpuSubmeshDesc) * mSubmeshDescsCpu.size());
        }
        frame.uploadedMetaVersion = mMetaVersion;
    }
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
                cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);
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
            if (!material)
                continue;

            cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);
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

    if (mInstancedCacheValid)
    {
        bool anyDirty = false;
        world.ForEach<TransformComponent, RenderableComponent>(
            [&](Entity, TransformComponent& tf, RenderableComponent& rend)
            {
                if (!rend.visible || !rend.mesh) return;
                if (tf.dirtyFrames > 0)
                    anyDirty = true;
            });

        if (!anyDirty)
        {
            DrawInstancedBatches(cmdList, currentFrameResource, frame,
                mCachedOverrideEntities, world);
            return;
        }
        mInstancedCacheValid = false;
    }

    std::map<std::pair<Mesh*, Material*>, std::vector<InstanceWorld>> batchMap;
    std::vector<Entity> overrideEntities;
    UINT totalObjects = 0;

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

    DrawInstancedBatches(cmdList, currentFrameResource, frame,
        mCachedOverrideEntities, world);
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

    // 1) CPU: 구조 변경 시 배치 재빌드 + dirty 슬롯 패치(ObjectCB 포함)
    if (mGpuStructureDirty)
        RebuildGpuDrivenScene(world);
    PatchGpuDrivenTransforms(world, currentFrameResource);

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

    UploadGpuDrivenFrameData(frame);

    // 2) Frame CB (카메라/컬링 — 매 프레임)
    GpuDrivenFrameConstants cb{};
    ExtractFrustumPlanes(XMMatrixMultiply(viewMatrix, projMatrix), cb.frustumPlanes);
    cb.numInstances = static_cast<uint32_t>(mSourceCpu.size());
    cb.enableFrustumCull = mGpuFrustumCull ? 1u : 0u;
    cb.numBatches = static_cast<uint32_t>(mBatchDescsCpu.size());
    cb.numSubmeshDraws = static_cast<uint32_t>(mSubmeshDescsCpu.size());
    cb.maxInstances = kMaxInstancesPerDraw;
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

    // 4) 전역 CullCompact 1회
    cmdList->SetComputeRootSignature(buildRS);
    cmdList->SetComputeRootConstantBufferView(0, frame.frameCBUpload->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(1, frame.requestUpload->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(2, frame.batchUpload->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(3, frame.submeshUpload->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(4, frame.instanceBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(5, frame.countBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(6, frame.drawCmdBuffer->GetGPUVirtualAddress());

    cmdList->SetPipelineState(mCullCompactPSO.Get());
    const UINT numInst = static_cast<UINT>(mSourceCpu.size());
    cmdList->Dispatch((numInst + 63u) / 64u, 1, 1);

    {
        D3D12_RESOURCE_BARRIER uav[2];
        uav[0] = CD3DX12_RESOURCE_BARRIER::UAV(frame.instanceBuffer.Get());
        uav[1] = CD3DX12_RESOURCE_BARRIER::UAV(frame.countBuffer.Get());
        cmdList->ResourceBarrier(2, uav);
    }

    // 5) 전역 BuildCommands 1회
    cmdList->SetPipelineState(mBuildCommandsPSO.Get());
    const UINT numSubs = static_cast<UINT>(mSubmeshDescsCpu.size());
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

    // 6) 배치별 바인딩 + 서브메시 ExecuteIndirect (InstanceCount는 GPU가 채움)
    cmdList->SetGraphicsRootSignature(sceneRS);
    cmdList->SetPipelineState(gfxPSO);
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

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
            if (si >= batch.materialGpuPtrs.size())
                break;

            D3D12_GPU_DESCRIPTOR_HANDLE mat{};
            mat.ptr = batch.materialGpuPtrs[si];
            cmdList->SetGraphicsRootDescriptorTable(2, mat);

            const UINT64 argOffset = (UINT64)cmdIndex * sizeof(IndirectCommand);
            cmdList->ExecuteIndirect(
                cmdSig, 1,
                frame.drawCmdBuffer.Get(), argOffset,
                nullptr, 0);
        }
    }
}

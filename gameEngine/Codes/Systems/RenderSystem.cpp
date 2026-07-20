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
}

UINT RenderSystem::BuildCBAlignedSize()
{
    return d3dUtil::CalcConstantBufferByteSize(sizeof(IndirectBuildConstants));
}

void RenderSystem::Initialize(ID3D12Device* device)
{
    mDevice = device;
    if (!mDevice)
        return;

    EnsureIndirectResources(mDevice);

    ID3D12RootSignature* buildRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::IndirectBuild);

    auto csBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\build_indirect_commands.hlsl", "CS", "cs_5_1");
    auto csFinBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\build_indirect_commands.hlsl", "CSFinalize", "cs_5_1");

    if (!csBlob || !csFinBlob)
    {
        OutputDebugStringA("[RenderSystem] compute shaders failed; Indirect disabled.\n");
        mRenderPath = RenderPath::Direct;
        return;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = buildRS;
    psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
    if (FAILED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mIndirectBuildPSO))))
    {
        mRenderPath = RenderPath::Direct;
        return;
    }

    psoDesc.CS = { csFinBlob->GetBufferPointer(), csFinBlob->GetBufferSize() };
    if (FAILED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mIndirectFinalizePSO))))
    {
        mRenderPath = RenderPath::Direct;
        return;
    }

    // Dummy 1-element instance buffer for Direct path root SRV binding
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

    mIndirectReady = true;
}

void RenderSystem::DestroyIndirectResources()
{
    for (UINT i = 0; i < kIndirectFrameCount; ++i)
    {
        auto& f = mFrames[i];
        if (f.requestUpload && f.requestMapped)
        {
            f.requestUpload->Unmap(0, nullptr);
            f.requestMapped = nullptr;
        }
        if (f.buildCBUpload && f.buildCBMapped)
        {
            f.buildCBUpload->Unmap(0, nullptr);
            f.buildCBMapped = nullptr;
        }
        f.requestUpload.Reset();
        f.instanceBuffer.Reset();
        f.countBuffer.Reset();
        f.drawCmdBuffer.Reset();
        f.buildCBUpload.Reset();
        f.instanceState = D3D12_RESOURCE_STATE_COMMON;
        f.countState = D3D12_RESOURCE_STATE_COMMON;
        f.drawCmdState = D3D12_RESOURCE_STATE_COMMON;
    }
    mCountZeroUpload.Reset();
    mDummyInstanceBuffer.Reset();
}

void RenderSystem::Shutdown()
{
    DestroyIndirectResources();
    mIndirectBuildPSO.Reset();
    mIndirectFinalizePSO.Reset();
    mIndirectReady = false;
    mDevice = nullptr;
}

void RenderSystem::EnsureIndirectResources(ID3D12Device* device)
{
    if (mFrames[0].instanceBuffer)
        return;

    const UINT64 reqBufSize = sizeof(IndirectDrawRequest) * kMaxIndirectDraws;
    const UINT64 instBufSize = sizeof(InstanceWorld) * kMaxInstancesPerDraw;
    const UINT buildCbSlot = BuildCBAlignedSize();
    const UINT64 buildCbTotal = static_cast<UINT64>(buildCbSlot) * kMaxIndirectGroups;

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
            &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.countBuffer)));
        f.countState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(sizeof(IndirectCommand), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
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
            &CD3DX12_RESOURCE_DESC::Buffer(buildCbTotal),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.buildCBUpload)));
        ThrowIfFailed(f.buildCBUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.buildCBMapped)));
    }

    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT)),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mCountZeroUpload)));
    UINT zero = 0;
    BYTE* mapped = nullptr;
    ThrowIfFailed(mCountZeroUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    std::memcpy(mapped, &zero, sizeof(UINT));
    mCountZeroUpload->Unmap(0, nullptr);
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

void RenderSystem::render(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    if (mRenderPath == RenderPath::Indirect
        && mIndirectReady
        && mIndirectBuildPSO
        && mIndirectFinalizePSO
        && RootSignatureManager::Get().GetSceneCommandSignature())
    {
        renderIndirect(world, cmdList, currentFrameResource, descriptorAllocator,
            currentFrameIndex, viewMatrix, projMatrix);
        return;
    }

    renderDirect(world, cmdList, currentFrameResource, descriptorAllocator, viewMatrix, projMatrix);
}

void RenderSystem::renderDirect(ECS::World& world,
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

void RenderSystem::renderIndirect(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    // 메시 단위 인스턴싱 (CPU 업로드 + DrawIndexedInstanced).
    // GPU compact/EI는 컬링·카운터 레이스로 화면이 비는 문제가 있어,
    // 동일 메시 대량 복제 이득은 유지하면서 확실한 경로로 고정.
    (void)viewMatrix;
    (void)projMatrix;

    EnsureIndirectResources(mDevice);

    const int frameIdx = ((currentFrameIndex % (int)kIndirectFrameCount) + (int)kIndirectFrameCount)
        % (int)kIndirectFrameCount;
    FrameIndirectResources& frame = mFrames[frameIdx];

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    // mesh → 인스턴스 월드 행렬들 (오브젝트당 1개)
    std::map<Mesh*, std::vector<InstanceWorld>> meshInstances;
    UINT totalObjects = 0;

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            (void)e;
            if (!rend.visible || !rend.mesh) return;
            if (totalObjects >= kMaxInstancesPerDraw) return;

            if (tf.dirtyFrames > 0 && rend.objectCBIndex < kMaxSceneObjects)
            {
                ObjectConstants objConst{};
                XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf.GetWorldMatrix()));
                currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);
                --tf.dirtyFrames;
            }

            InstanceWorld inst{};
            StoreWorldTransposed(tf.GetWorldMatrix(), inst.worldMatrix);
            meshInstances[rend.mesh].push_back(inst);
            ++totalObjects;
        });

    if (meshInstances.empty())
        return;

    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    PSOKey gfxKey{};
    gfxKey.shaderName = "object_instanced";
    gfxKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    gfxKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    gfxKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    ID3D12PipelineState* gfxPSO = PipelineStateManager::Get().GetOrCreatePSO(gfxKey, sceneRS);
    if (!sceneRS || !gfxPSO)
    {
        // 인스턴스 PSO 실패 시 기존 Direct로
        renderDirect(world, cmdList, currentFrameResource, descriptorAllocator, viewMatrix, projMatrix);
        return;
    }

    cmdList->SetGraphicsRootSignature(sceneRS);
    cmdList->SetPipelineState(gfxPSO);
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    // requestUpload 앞부분을 인스턴스 월드 업로드로 재사용 (64B * N)
    BYTE* upload = frame.requestMapped;
    UINT uploadCursor = 0;

    for (auto& mpair : meshInstances)
    {
        Mesh* mesh = mpair.first;
        auto& instances = mpair.second;
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

        // 같은 인스턴스 버퍼로 모든 서브메시 드로우 (Blender 복제와 같은 구조)
        for (auto& pair : mesh->DrawArgs)
        {
            const auto& sub = pair.second;
            if (sub.IndexCount == 0)
                continue;

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
                continue;

            cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);
            cmdList->DrawIndexedInstanced(
                sub.IndexCount,
                n,
                sub.StartIndexLocation,
                sub.BaseVertexLocation,
                0);
        }

        uploadCursor += n;
    }
}

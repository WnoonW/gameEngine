#include <DirectXMath.h>
#include <algorithm>
#include <map>
#include <vector>
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
    struct DrawGroupKey
    {
        Mesh* mesh = nullptr;
        UINT64 materialGpu = 0;

        bool operator<(const DrawGroupKey& o) const
        {
            if (mesh != o.mesh)
                return mesh < o.mesh;
            return materialGpu < o.materialGpu;
        }
    };
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
        L"Resources\\Shaders\\build_indirect_commands.hlsl",
        "CS",
        "cs_5_1");

    if (!csBlob)
    {
        OutputDebugStringA("[RenderSystem] build_indirect_commands.hlsl compile failed; Indirect disabled.\n");
        mRenderPath = RenderPath::Direct;
        return;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = buildRS;
    psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };

    HRESULT hr = mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mIndirectBuildPSO));
    if (FAILED(hr))
    {
        OutputDebugStringA("[RenderSystem] CreateComputePipelineState failed; Indirect disabled.\n");
        mRenderPath = RenderPath::Direct;
        return;
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
        f.commandBuffer.Reset();
        f.countBuffer.Reset();
        f.buildCBUpload.Reset();
        f.commandState = D3D12_RESOURCE_STATE_COMMON;
        f.countState = D3D12_RESOURCE_STATE_COMMON;
    }
    mCountZeroUpload.Reset();
}

void RenderSystem::Shutdown()
{
    DestroyIndirectResources();
    mIndirectBuildPSO.Reset();
    mIndirectReady = false;
    mDevice = nullptr;
}

void RenderSystem::EnsureIndirectResources(ID3D12Device* device)
{
    if (mFrames[0].commandBuffer)
        return;

    const UINT64 cmdBufSize = sizeof(IndirectCommand) * kMaxIndirectDraws;
    const UINT64 reqBufSize = sizeof(IndirectDrawRequest) * kMaxIndirectDraws;
    const UINT buildCbSlot = BuildCBAlignedSize();
    const UINT64 buildCbTotal = static_cast<UINT64>(buildCbSlot) * kMaxIndirectGroups;
    const UINT64 countBufSize = sizeof(UINT) * kMaxIndirectGroups;

    for (UINT fi = 0; fi < kIndirectFrameCount; ++fi)
    {
        auto& f = mFrames[fi];

        // 버퍼는 COMMON으로 생성 (UAV 초기 상태는 드라이버가 무시함)
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(cmdBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.commandBuffer)));
        f.commandState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(countBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.countBuffer)));
        f.countState = D3D12_RESOURCE_STATE_COMMON;

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
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT) * kMaxIndirectGroups),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mCountZeroUpload)));

    std::vector<UINT> zeros(kMaxIndirectGroups, 0);
    BYTE* mapped = nullptr;
    ThrowIfFailed(mCountZeroUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    memcpy(mapped, zeros.data(), sizeof(UINT) * kMaxIndirectGroups);
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
        { m._13,         m._23,         m._33,         m._43         },
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

    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;

            XMMATRIX worldMat = tf.GetWorldMatrix();
            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(worldMat));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

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
    EnsureIndirectResources(mDevice);

    const int frameIdx = ((currentFrameIndex % static_cast<int>(kIndirectFrameCount)) + static_cast<int>(kIndirectFrameCount))
        % static_cast<int>(kIndirectFrameCount);
    FrameIndirectResources& frame = mFrames[frameIdx];

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    // --- 1) 수집 & 그룹 ---
    std::map<DrawGroupKey, std::vector<IndirectDrawRequest>> groups;

    const UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    const D3D12_GPU_VIRTUAL_ADDRESS objectCBBase =
        currentFrameResource->ObjectCB->Resource()->GetGPUVirtualAddress();

    UINT totalRequests = 0;
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (totalRequests >= kMaxIndirectDraws) return;

            XMMATRIX worldMat = tf.GetWorldMatrix();
            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(worldMat));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

            D3D12_GPU_VIRTUAL_ADDRESS objCbv =
                objectCBBase + (UINT64)rend.objectCBIndex * objCBByteSize;

            XMFLOAT3 center{ 0, 0, 0 };
            XMFLOAT3 extents{ 0, 0, 0 };
            if (BoundsComponent* bounds = world.GetComponent<BoundsComponent>(e))
            {
                center = bounds->worldBounds.Center;
                extents = bounds->worldBounds.Extents;
            }

            for (auto& pair : rend.mesh->DrawArgs)
            {
                if (totalRequests >= kMaxIndirectDraws)
                    break;

                const auto& sub = pair.second;
                Material* material = MaterialManager::Get().ResolveForDraw(e, pair.first, sub.initMaterial);
                if (!material || !material->HasValidTexture())
                    continue;

                IndirectDrawRequest req{};
                req.objectCbvLow = static_cast<uint32_t>(objCbv & 0xffffffffu);
                req.objectCbvHigh = static_cast<uint32_t>(objCbv >> 32);
                req.indexCount = sub.IndexCount;
                req.startIndexLocation = sub.StartIndexLocation;
                req.baseVertexLocation = sub.BaseVertexLocation;
                req.instanceCount = 1;
                req.boundsCenterX = center.x;
                req.boundsCenterY = center.y;
                req.boundsCenterZ = center.z;
                req.boundsExtentsX = extents.x;
                req.boundsExtentsY = extents.y;
                req.boundsExtentsZ = extents.z;

                DrawGroupKey key{ rend.mesh, material->mTextureHandle.GPU.ptr };
                groups[key].push_back(req);
                ++totalRequests;
            }
        });

    if (groups.empty())
        return;

    // --- 2) 모든 그룹 요청을 업로드 버퍼에 연속 배치 (덮어쓰기 버그 방지) ---
    std::vector<GroupJob> jobs;
    jobs.reserve(groups.size());

    UINT reqCursor = 0;
    UINT cmdCursor = 0;
    UINT groupIndex = 0;

    IndirectBuildConstants baseCB{};
    ExtractFrustumPlanes(XMMatrixMultiply(viewMatrix, projMatrix), baseCB.frustumPlanes);
    baseCB.enableFrustumCull = 0; // 안정화: 컬링은 데이터 검증 후 켜기

    const UINT buildCbAlign = BuildCBAlignedSize();

    for (auto& gpair : groups)
    {
        if (groupIndex >= kMaxIndirectGroups)
            break;
        if (reqCursor >= kMaxIndirectDraws)
            break;

        auto& reqs = gpair.second;
        if (reqs.empty() || !gpair.first.mesh)
            continue;

        const UINT n = static_cast<UINT>((std::min)(
            reqs.size(),
            static_cast<size_t>(kMaxIndirectDraws - reqCursor)));

        memcpy(
            frame.requestMapped + reqCursor * sizeof(IndirectDrawRequest),
            reqs.data(),
            sizeof(IndirectDrawRequest) * n);

        GroupJob job{};
        job.mesh = gpair.first.mesh;
        job.materialGpu = gpair.first.materialGpu;
        job.requestOffset = reqCursor;
        job.requestCount = n;
        job.commandOffset = cmdCursor;
        job.groupIndex = groupIndex;
        jobs.push_back(job);

        IndirectBuildConstants cb = baseCB;
        cb.numRequests = n;
        cb.commandWriteBase = cmdCursor;
        memcpy(
            frame.buildCBMapped + groupIndex * buildCbAlign,
            &cb,
            sizeof(IndirectBuildConstants));

        reqCursor += n;
        cmdCursor += n; // 최악 시 n개 출력 (컬링 없으면 n)
        ++groupIndex;
    }

    if (jobs.empty())
        return;

    // 카운터 슬롯 전부 0
    if (frame.countState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            frame.countBuffer.Get(), frame.countState, D3D12_RESOURCE_STATE_COPY_DEST));
        frame.countState = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    cmdList->CopyBufferRegion(
        frame.countBuffer.Get(), 0,
        mCountZeroUpload.Get(), 0,
        sizeof(UINT) * kMaxIndirectGroups);

    // command + count → UAV
    {
        D3D12_RESOURCE_BARRIER barriers[2];
        UINT nb = 0;
        if (frame.commandState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            barriers[nb++] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.commandBuffer.Get(), frame.commandState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            frame.commandState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (frame.countState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            barriers[nb++] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.countBuffer.Get(), frame.countState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            frame.countState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (nb)
            cmdList->ResourceBarrier(nb, barriers);
    }

    ID3D12RootSignature* buildRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::IndirectBuild);
    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    ID3D12CommandSignature* cmdSig = RootSignatureManager::Get().GetSceneCommandSignature();

    PSOKey gfxKey{};
    gfxKey.shaderName = "object_cb";
    gfxKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    gfxKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    gfxKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    ID3D12PipelineState* gfxPSO = PipelineStateManager::Get().GetOrCreatePSO(gfxKey, sceneRS);

    // --- 3) 그룹별 Compute (각기 다른 request 오프셋 / 카운터 슬롯) ---
    cmdList->SetPipelineState(mIndirectBuildPSO.Get());
    cmdList->SetComputeRootSignature(buildRS);
    cmdList->SetComputeRootUnorderedAccessView(2, frame.commandBuffer->GetGPUVirtualAddress());

    for (const GroupJob& job : jobs)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS reqVA =
            frame.requestUpload->GetGPUVirtualAddress()
            + static_cast<UINT64>(job.requestOffset) * sizeof(IndirectDrawRequest);

        const D3D12_GPU_VIRTUAL_ADDRESS buildCbVA =
            frame.buildCBUpload->GetGPUVirtualAddress()
            + static_cast<UINT64>(job.groupIndex) * buildCbAlign;

        const D3D12_GPU_VIRTUAL_ADDRESS countVA =
            frame.countBuffer->GetGPUVirtualAddress()
            + static_cast<UINT64>(job.groupIndex) * sizeof(UINT);

        cmdList->SetComputeRootConstantBufferView(0, buildCbVA);
        cmdList->SetComputeRootShaderResourceView(1, reqVA);
        cmdList->SetComputeRootUnorderedAccessView(3, countVA);

        const UINT gx = (job.requestCount + 63u) / 64u;
        cmdList->Dispatch(gx, 1, 1);
    }

    // UAV → INDIRECT_ARGUMENT
    {
        D3D12_RESOURCE_BARRIER barriers[2];
        barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
            frame.commandBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            frame.countBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        cmdList->ResourceBarrier(2, barriers);
        frame.commandState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        frame.countState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }

    // --- 4) 그룹별 ExecuteIndirect ---
    if (sceneRS)
        cmdList->SetGraphicsRootSignature(sceneRS);
    if (gfxPSO)
        cmdList->SetPipelineState(gfxPSO);

    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    for (const GroupJob& job : jobs)
    {
        auto vbv = job.mesh->VertexBufferView();
        auto ibv = job.mesh->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        D3D12_GPU_DESCRIPTOR_HANDLE matGpu{};
        matGpu.ptr = job.materialGpu;
        cmdList->SetGraphicsRootDescriptorTable(2, matGpu);

        const UINT64 cmdByteOffset =
            static_cast<UINT64>(job.commandOffset) * sizeof(IndirectCommand);
        const UINT64 countByteOffset =
            static_cast<UINT64>(job.groupIndex) * sizeof(UINT);

        cmdList->ExecuteIndirect(
            cmdSig,
            job.requestCount,
            frame.commandBuffer.Get(),
            cmdByteOffset,
            frame.countBuffer.Get(),
            countByteOffset);
    }
}

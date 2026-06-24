#include <DirectXMath.h>
#include "RenderSystem.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "constantStruct.h"

using namespace DirectX;

void RenderSystem::createCBV(ID3D12Device* device,
    std::vector<std::unique_ptr<FrameResource>>& frameResources,
    int gNumFrameResources,
    DescriptorAllocator& descriptorAllocator,
    Entity entity,                    
    ECS::World& world)                
{
    RenderableComponent* rend = world.GetComponent<RenderableComponent>(entity);
    if (!rend) return;

    if (mEntityCBVHandles.size() <= entity)
        mEntityCBVHandles.resize(entity + 1);

    mEntityCBVHandles[entity].resize(gNumFrameResources);

    for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
    {
        auto objectCB = frameResources[frameIndex]->ObjectCB->Resource();

        auto handle = descriptorAllocator.Allocate();
        mEntityCBVHandles[entity][frameIndex] = handle;

        UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = objectCB->GetGPUVirtualAddress() + (UINT64)rend->objectCBIndex * objCBByteSize;
        cbvDesc.SizeInBytes = objCBByteSize;

        device->CreateConstantBufferView(&cbvDesc, handle.CPU);
    }
}

void RenderSystem::renderIndexedInstanced(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    ID3D12RootSignature* lastRS = nullptr;
    ID3D12PipelineState* lastPSO = nullptr;

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;

            XMMATRIX worldMat = tf.GetWorldMatrix();
            XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
            XMMATRIX wvp = XMMatrixMultiply(worldMat, viewProj);

            if (rend.objectCBIndex >= RenderLimits::MaxObjectCount)
                return;

            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

            auto vbv = rend.mesh->VertexBufferView();
            auto ibv = rend.mesh->IndexBufferView();

            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);

            PSOKey key{};
            key.shaderName = "object";
            key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

            ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);

            if (sceneRS != lastRS) { cmdList->SetGraphicsRootSignature(sceneRS); lastRS = sceneRS; }
            if (pso != lastPSO) { cmdList->SetPipelineState(pso); lastPSO = pso; }

            for (auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                if (e < mEntityCBVHandles.size() &&
                    currentFrameIndex < static_cast<int>(mEntityCBVHandles[e].size()))
                {
                    cmdList->SetGraphicsRootDescriptorTable(0, mEntityCBVHandles[e][currentFrameIndex].GPU);
                }

                cmdList->SetGraphicsRootDescriptorTable(1, material->mTextureHandle.GPU);

                cmdList->DrawIndexedInstanced(
                    sub.IndexCount, 1,
                    sub.StartIndexLocation,
                    sub.BaseVertexLocation, 0);
            }
        });
}

void RenderSystem::renderExecuteIndirect1(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    cmdList->SetGraphicsRootSignature(sceneRS);

    PSOKey key{};
    key.shaderName = "object";
    key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);
    if (!pso)
        return;

    ComPtr<ID3D12CommandSignature> cmdSig =
        RootSignatureManager::Get().GetOrCreateCommandSignature();
    if (!cmdSig)
        return;

    IndirectDrawCommand* cmdBuffer =
        reinterpret_cast<IndirectDrawCommand*>(currentFrameResource->MappedArgumentBuffer);

    UINT writeIndex = 0;
    const UINT maxCommandCount = currentFrameResource->ArgumentBufferSize / sizeof(IndirectDrawCommand);

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (!rend.mesh->vertexBuffer || !rend.mesh->indexBuffer) return;

            XMMATRIX worldMat = tf.GetWorldMatrix();
            XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
            XMMATRIX wvp = XMMatrixMultiply(worldMat, viewProj);

            if (rend.objectCBIndex >= RenderLimits::MaxObjectCount)
                return;

            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

            auto vbv = rend.mesh->VertexBufferView();
            auto ibv = rend.mesh->IndexBufferView();
            cmdList->SetPipelineState(pso);
            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            if (e < mEntityCBVHandles.size() &&
                currentFrameIndex < static_cast<int>(mEntityCBVHandles[e].size()))
            {
                cmdList->SetGraphicsRootDescriptorTable(0, mEntityCBVHandles[e][currentFrameIndex].GPU);
            }

            for (auto& pair : rend.mesh->DrawArgs)
            {
                if (writeIndex >= maxCommandCount) break;

                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                cmdBuffer[writeIndex].IndexCountPerInstance = sub.IndexCount;
                cmdBuffer[writeIndex].InstanceCount = 1;
                cmdBuffer[writeIndex].StartIndexLocation = sub.StartIndexLocation;
                cmdBuffer[writeIndex].BaseVertexLocation = sub.BaseVertexLocation;
                cmdBuffer[writeIndex].StartInstanceLocation = 0;

                cmdList->SetGraphicsRootDescriptorTable(1, material->mTextureHandle.GPU);

                cmdList->ExecuteIndirect(
                    cmdSig.Get(),
                    1,
                    currentFrameResource->ArgumentBuffer.Get(),
                    writeIndex * sizeof(IndirectDrawCommand),
                    nullptr,
                    0);

                ++writeIndex;
            }
        });
}

void RenderSystem::renderExecuteIndirect(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    if (!sceneRS)
        return;

    PSOKey key{};
    key.shaderName = "object";
    key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);
    if (!pso)
        return;

    ComPtr<ID3D12CommandSignature> cmdSig =
        RootSignatureManager::Get().GetOrCreateCommandSignature();
    if (!cmdSig)
        return;

    cmdList->SetGraphicsRootSignature(sceneRS);
    cmdList->SetPipelineState(pso);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    IndirectDrawCommand* cmdBuffer =
        reinterpret_cast<IndirectDrawCommand*>(currentFrameResource->MappedArgumentBuffer);

    const UINT maxCommandCount =
        currentFrameResource->ArgumentBufferSize / sizeof(IndirectDrawCommand);

    struct DrawBindState
    {
        Mesh* mesh = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE cbv{};
        D3D12_GPU_DESCRIPTOR_HANDLE texture{};
    };

    DrawBindState activeState{};
    UINT writeIndex = 0;
    UINT batchStart = 0;
    UINT batchCount = 0;

    auto flushBatch = [&]()
    {
        if (batchCount == 0)
            return;

        cmdList->ExecuteIndirect(
            cmdSig.Get(),
            batchCount,
            currentFrameResource->ArgumentBuffer.Get(),
            batchStart * sizeof(IndirectDrawCommand),
            nullptr,
            0);

        batchCount = 0;
    };

    auto applyBindState = [&](Mesh* mesh,
        D3D12_GPU_DESCRIPTOR_HANDLE cbv,
        D3D12_GPU_DESCRIPTOR_HANDLE texture)
    {
        if (mesh != activeState.mesh)
        {
            auto vbv = mesh->VertexBufferView();
            auto ibv = mesh->IndexBufferView();
            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            activeState.mesh = mesh;
        }

        if (cbv.ptr != activeState.cbv.ptr)
        {
            cmdList->SetGraphicsRootDescriptorTable(0, cbv);
            activeState.cbv = cbv;
        }

        if (texture.ptr != activeState.texture.ptr)
        {
            cmdList->SetGraphicsRootDescriptorTable(1, texture);
            activeState.texture = texture;
        }
    };

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (!rend.mesh->vertexBuffer || !rend.mesh->indexBuffer) return;
            if (rend.objectCBIndex >= RenderLimits::MaxObjectCount) return;

            XMMATRIX worldMat = tf.GetWorldMatrix();
            XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
            XMMATRIX wvp = XMMatrixMultiply(worldMat, viewProj);

            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

            D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle{};
            if (e < mEntityCBVHandles.size() &&
                currentFrameIndex < static_cast<int>(mEntityCBVHandles[e].size()))
            {
                cbvHandle = mEntityCBVHandles[e][currentFrameIndex].GPU;
            }

            for (auto& pair : rend.mesh->DrawArgs)
            {
                if (writeIndex >= maxCommandCount)
                {
                    flushBatch();
                    return;
                }

                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                const D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = material->mTextureHandle.GPU;

                const bool stateChanged =
                    rend.mesh != activeState.mesh ||
                    cbvHandle.ptr != activeState.cbv.ptr ||
                    textureHandle.ptr != activeState.texture.ptr;

                if (stateChanged)
                {
                    flushBatch();
                    applyBindState(rend.mesh, cbvHandle, textureHandle);
                    batchStart = writeIndex;
                }

                cmdBuffer[writeIndex].IndexCountPerInstance = sub.IndexCount;
                cmdBuffer[writeIndex].InstanceCount = 1;
                cmdBuffer[writeIndex].StartIndexLocation = sub.StartIndexLocation;
                cmdBuffer[writeIndex].BaseVertexLocation = sub.BaseVertexLocation;
                cmdBuffer[writeIndex].StartInstanceLocation = 0;

                ++writeIndex;
                ++batchCount;
            }
        });

    flushBatch();
}
#include <DirectXMath.h>
#include "RenderSystem.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "constantStruct.h"

using namespace DirectX;

namespace
{
    D3D12_GPU_VIRTUAL_ADDRESS GetObjectCBGpuAddress(FrameResource* currentFrameResource, uint32_t objectCBIndex)
    {
        const UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        return currentFrameResource->ObjectCB->Resource()->GetGPUVirtualAddress()
            + static_cast<UINT64>(objectCBIndex) * objCBByteSize;
    }

    void UpdatePassCB(FrameResource* currentFrameResource,
        ID3D12GraphicsCommandList* cmdList,
        const XMMATRIX& viewMatrix,
        const XMMATRIX& projMatrix)
    {
        XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);

        PassConstants passConst{};
        XMStoreFloat4x4(&passConst.ViewProj, XMMatrixTranspose(viewProj));
        currentFrameResource->PassCB->CopyData(0, passConst);

        cmdList->SetGraphicsRootConstantBufferView(
            1,
            currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    void BindObjectCB(FrameResource* currentFrameResource,
        ID3D12GraphicsCommandList* cmdList,
        uint32_t objectCBIndex)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            0,
            GetObjectCBGpuAddress(currentFrameResource, objectCBIndex));
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

    UpdatePassCB(currentFrameResource, cmdList, viewMatrix, projMatrix);

    ID3D12RootSignature* lastRS = nullptr;
    ID3D12PipelineState* lastPSO = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS lastObjectCBAddress = 0;

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;

            XMMATRIX worldMat = tf.GetWorldMatrix();

            if (rend.objectCBIndex >= RenderLimits::MaxObjectCount)
                return;

            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(worldMat));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

            const D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress =
                GetObjectCBGpuAddress(currentFrameResource, rend.objectCBIndex);

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

            if (objectCBAddress != lastObjectCBAddress)
            {
                cmdList->SetGraphicsRootConstantBufferView(0, objectCBAddress);
                lastObjectCBAddress = objectCBAddress;
            }

            for (auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);

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

    UpdatePassCB(currentFrameResource, cmdList, viewMatrix, projMatrix);

    PSOKey key{};
    key.shaderName = "object";
    key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);
    if (!pso)
        return;

    ComPtr<ID3D12CommandSignature> cmdSig =
        RootSignatureManager::Get().GetOrCreateCommandSignature(sceneRS);
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

            if (rend.objectCBIndex >= RenderLimits::MaxObjectCount)
                return;

            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(worldMat));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

            auto vbv = rend.mesh->VertexBufferView();
            auto ibv = rend.mesh->IndexBufferView();
            cmdList->SetPipelineState(pso);
            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            BindObjectCB(currentFrameResource, cmdList, rend.objectCBIndex);

            for (auto& pair : rend.mesh->DrawArgs)
            {
                if (writeIndex >= maxCommandCount) break;

                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                cmdBuffer[writeIndex].objectCBIndex = rend.objectCBIndex;
                cmdBuffer[writeIndex].drawArgs.IndexCountPerInstance = sub.IndexCount;
                cmdBuffer[writeIndex].drawArgs.InstanceCount = 1;
                cmdBuffer[writeIndex].drawArgs.StartIndexLocation = sub.StartIndexLocation;
                cmdBuffer[writeIndex].drawArgs.BaseVertexLocation = sub.BaseVertexLocation;
                cmdBuffer[writeIndex].drawArgs.StartInstanceLocation = 0;

                cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);

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
        RootSignatureManager::Get().GetOrCreateCommandSignature(sceneRS);
    if (!cmdSig)
        return;

    cmdList->SetGraphicsRootSignature(sceneRS);
    cmdList->SetPipelineState(pso);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UpdatePassCB(currentFrameResource, cmdList, viewMatrix, projMatrix);

    IndirectDrawCommand* cmdBuffer =
        reinterpret_cast<IndirectDrawCommand*>(currentFrameResource->MappedArgumentBuffer);

    const UINT maxCommandCount =
        currentFrameResource->ArgumentBufferSize / sizeof(IndirectDrawCommand);

    struct DrawBindState
    {
        Mesh* mesh = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress = 0;
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
        D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress,
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

        if (objectCBAddress != activeState.objectCBAddress)
        {
            cmdList->SetGraphicsRootConstantBufferView(0, objectCBAddress);
            activeState.objectCBAddress = objectCBAddress;
        }

        if (texture.ptr != activeState.texture.ptr)
        {
            cmdList->SetGraphicsRootDescriptorTable(2, texture);
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

            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(worldMat));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

            const D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress =
                GetObjectCBGpuAddress(currentFrameResource, rend.objectCBIndex);

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
                    objectCBAddress != activeState.objectCBAddress ||
                    textureHandle.ptr != activeState.texture.ptr;

                if (stateChanged)
                {
                    flushBatch();
                    applyBindState(rend.mesh, objectCBAddress, textureHandle);
                    batchStart = writeIndex;
                }

                cmdBuffer[writeIndex].objectCBIndex = rend.objectCBIndex;
                cmdBuffer[writeIndex].drawArgs.IndexCountPerInstance = sub.IndexCount;
                cmdBuffer[writeIndex].drawArgs.InstanceCount = 1;
                cmdBuffer[writeIndex].drawArgs.StartIndexLocation = sub.StartIndexLocation;
                cmdBuffer[writeIndex].drawArgs.BaseVertexLocation = sub.BaseVertexLocation;
                cmdBuffer[writeIndex].drawArgs.StartInstanceLocation = 0;

                ++writeIndex;
                ++batchCount;
            }
        });

    flushBatch();
}
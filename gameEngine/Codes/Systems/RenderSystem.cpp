#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <vector>

#include "RenderSystem.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "constantStruct.h"

using namespace DirectX;

namespace
{
    void StoreTransposedWorld(ObjectConstants& dst, const XMMATRIX& worldMat)
    {
        XMStoreFloat4x4(&dst.World, XMMatrixTranspose(worldMat));
    }

    void UpdateAndBindPassCB(FrameResource* currentFrameResource,
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

    void BindPassCB(FrameResource* currentFrameResource, ID3D12GraphicsCommandList* cmdList)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1,
            currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    void BindInstanceBuffer(FrameResource* currentFrameResource, ID3D12GraphicsCommandList* cmdList)
    {
        if (currentFrameResource->InstanceBuffer)
        {
            cmdList->SetGraphicsRootShaderResourceView(
                4,
                currentFrameResource->InstanceBuffer->GetGPUVirtualAddress());
        }
    }

    bool HasValidBounds(const DirectX::BoundingBox& bounds)
    {
        return bounds.Extents.x > 0.0f
            || bounds.Extents.y > 0.0f
            || bounds.Extents.z > 0.0f;
    }

    void TransitionDepthStencil(const DepthStencilContext* depthCtx,
        ID3D12GraphicsCommandList* cmdList,
        D3D12_RESOURCE_STATES newState)
    {
        if (!depthCtx || !depthCtx->resource || !depthCtx->currentState)
            return;
        if (*depthCtx->currentState == newState)
            return;

        const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            depthCtx->resource,
            *depthCtx->currentState,
            newState);
        cmdList->ResourceBarrier(1, &barrier);
        *depthCtx->currentState = newState;
    }

    void PrepareDepthForSceneDraw(const DepthStencilContext* depthCtx, ID3D12GraphicsCommandList* cmdList)
    {
        if (!depthCtx || !depthCtx->resource || depthCtx->dsv.ptr == 0 || depthCtx->rtv.ptr == 0)
            return;

        TransitionDepthStencil(depthCtx, cmdList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList->ClearDepthStencilView(
            depthCtx->dsv,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f,
            0,
            0,
            nullptr);
        cmdList->OMSetRenderTargets(1, &depthCtx->rtv, true, &depthCtx->dsv);
    }
}

void RenderSystem::renderExecuteIndirect(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int /*currentFrameIndex*/,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix,
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
    const DepthStencilContext* depthCtx)
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

    if (MeshManager::sGlobalVertexBuffer && MeshManager::sGlobalIndexBuffer)
    {
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = MeshManager::sGlobalVertexBuffer->GetGPUVirtualAddress();
        vbv.StrideInBytes = sizeof(Vertex);
        vbv.SizeInBytes = MeshManager::sGlobalVertexCount * sizeof(Vertex);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = MeshManager::sGlobalIndexBuffer->GetGPUVirtualAddress();
        ibv.Format = DXGI_FORMAT_R32_UINT;
        ibv.SizeInBytes = MeshManager::sGlobalIndexCount * sizeof(uint32_t);

        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
    }

    UpdateAndBindPassCB(currentFrameResource, cmdList, viewMatrix, projMatrix);
    BindInstanceBuffer(currentFrameResource, cmdList);

    DirectX::BoundingFrustum frustum;
    DirectX::BoundingFrustum::CreateFromMatrix(frustum, projMatrix);
    const XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
    frustum.Transform(frustum, invView);

    ObjectConstants* instanceData =
        reinterpret_cast<ObjectConstants*>(currentFrameResource->MappedInstanceBuffer);

    const UINT maxInstanceCount =
        currentFrameResource->InstanceBufferSize / sizeof(ObjectConstants);

    mGroupIndex.clear();
    mDrawGroups.clear();
    mGroupDrawList.clear();
    mBindingRanges.clear();

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (!rend.mesh->vertexBuffer || !rend.mesh->indexBuffer) return;
            if (rend.objectCBIndex >= RenderLimits::MaxObjectCount) return;

            const XMMATRIX worldMat = tf.GetWorldMatrix();

            for (const auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                if (HasValidBounds(sub.Bounds))
                {
                    DirectX::BoundingBox worldBox;
                    sub.Bounds.Transform(worldBox, worldMat);
                    if (!frustum.Intersects(worldBox))
                        continue;
                }

                const DrawGroupKey groupKey{
                    rend.mesh,
                    material,
                    sub.IndexCount,
                    sub.StartIndexLocation,
                    sub.BaseVertexLocation
                };

                auto [it, inserted] = mGroupIndex.emplace(groupKey, mDrawGroups.size());
                if (inserted)
                {
                    DrawGroup group{};
                    group.mesh = rend.mesh;
                    group.material = material;
                    group.indexCount = sub.IndexCount;
                    group.startIndexLocation = sub.StartIndexLocation;
                    group.baseVertexLocation = sub.BaseVertexLocation;
                    group.localCenter = sub.Bounds.Center;
                    group.localExtents = sub.Bounds.Extents;
                    mDrawGroups.push_back(std::move(group));
                }

                mDrawGroups[it->second].worldMatrices.push_back(worldMat);
            }
        });

    UINT instanceWritePos = 0;

    for (const DrawGroup& group : mDrawGroups)
    {
        if (group.worldMatrices.empty())
            continue;

        const D3D12_GPU_DESCRIPTOR_HANDLE texHandle =
            group.material ? group.material->mTextureHandle.GPU : D3D12_GPU_DESCRIPTOR_HANDLE{};

        UINT cmdsThisRange = 0;
        size_t groupPos = 0;

        while (groupPos < group.worldMatrices.size())
        {
            if (instanceWritePos >= maxInstanceCount)
                break;

            const UINT remaining = maxInstanceCount - instanceWritePos;
            const UINT chunkSize = min(
                static_cast<UINT>(group.worldMatrices.size() - groupPos),
                remaining);
            if (chunkSize == 0)
                break;

            const UINT baseInstance = instanceWritePos;

            for (UINT i = 0; i < chunkSize; ++i)
            {
                StoreTransposedWorld(instanceData[baseInstance + i], group.worldMatrices[groupPos + i]);
            }

            GroupDrawData gdata{};
            gdata.baseInstance = baseInstance;
            gdata.instanceCount = chunkSize;
            gdata.indexCountPerInstance = group.indexCount;
            gdata.startIndexLocation = group.startIndexLocation;
            gdata.baseVertexLocation = group.baseVertexLocation;
            gdata.localCenter = group.localCenter;
            gdata.localExtents = group.localExtents;
            mGroupDrawList.push_back(gdata);

            cmdsThisRange += 1;
            instanceWritePos += chunkSize;
            groupPos += chunkSize;
        }

        if (cmdsThisRange > 0)
            mBindingRanges.push_back({ texHandle, cmdsThisRange });
    }

    const UINT numCommands = static_cast<UINT>(mGroupDrawList.size());
    bool ranBuildIndirect = false;

    if (numCommands > 0
        && currentFrameResource->MappedGroupData
        && currentFrameResource->GPUArgumentBuffer)
    {
        const size_t bytes = numCommands * sizeof(GroupDrawData);
        if (bytes <= currentFrameResource->GroupDataBufferSize)
        {
            memcpy(currentFrameResource->MappedGroupData, mGroupDrawList.data(), bytes);

            ID3D12RootSignature* buildRS =
                RootSignatureManager::Get().GetRootSignature(RootSignatureType::BuildIndirect);
            ID3D12PipelineState* buildPSO =
                PipelineStateManager::Get().GetOrCreateComputePSO("build_indirect", buildRS);

            if (buildRS && buildPSO)
            {
                if (depthSrvGpu.ptr != 0)
                {
                    TransitionDepthStencil(depthCtx, cmdList, DepthReadState);
                }

                D3D12_RESOURCE_BARRIER toUAV = {};
                toUAV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toUAV.Transition.pResource = currentFrameResource->GPUArgumentBuffer.Get();
                toUAV.Transition.StateBefore = currentFrameResource->mGPUArgCurrentState;
                toUAV.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                cmdList->ResourceBarrier(1, &toUAV);
                currentFrameResource->mGPUArgCurrentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

                cmdList->SetComputeRootSignature(buildRS);
                cmdList->SetPipelineState(buildPSO);
                cmdList->SetComputeRootShaderResourceView(
                    0,
                    currentFrameResource->GroupDataUploadBuffer->GetGPUVirtualAddress());
                cmdList->SetComputeRootUnorderedAccessView(
                    1,
                    currentFrameResource->GPUArgumentBuffer->GetGPUVirtualAddress());
                cmdList->SetComputeRoot32BitConstant(2, numCommands, 0);

                if (depthSrvGpu.ptr != 0)
                    cmdList->SetComputeRootDescriptorTable(3, depthSrvGpu);

                const UINT threadGroups = (numCommands + 63) / 64;
                cmdList->Dispatch(threadGroups, 1, 1);

                D3D12_RESOURCE_BARRIER uavVisible = {};
                uavVisible.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uavVisible.UAV.pResource = currentFrameResource->GPUArgumentBuffer.Get();
                cmdList->ResourceBarrier(1, &uavVisible);

                D3D12_RESOURCE_BARRIER toIndirect = {};
                toIndirect.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toIndirect.Transition.pResource = currentFrameResource->GPUArgumentBuffer.Get();
                toIndirect.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                toIndirect.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
                toIndirect.Transition.Subresource = 0;
                cmdList->ResourceBarrier(1, &toIndirect);

                currentFrameResource->mGPUArgCurrentState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
                ranBuildIndirect = true;
            }
        }
    }

    if (ranBuildIndirect)
    {
        cmdList->SetGraphicsRootSignature(sceneRS);
        cmdList->SetPipelineState(pso);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        BindPassCB(currentFrameResource, cmdList);
        BindInstanceBuffer(currentFrameResource, cmdList);
    }

    if (numCommands == 0)
        return;

    PrepareDepthForSceneDraw(depthCtx, cmdList);

    D3D12_GPU_DESCRIPTOR_HANDLE activeTexture{};
    UINT cmdCursor = 0;
    UINT batchStart = 0;
    UINT batchCount = 0;

    for (const BindingRange& range : mBindingRanges)
    {
        if (range.texture.ptr != activeTexture.ptr)
        {
            if (batchCount > 0)
            {
                cmdList->ExecuteIndirect(
                    cmdSig.Get(),
                    batchCount,
                    currentFrameResource->GPUArgumentBuffer.Get(),
                    batchStart * sizeof(IndirectDrawCommand),
                    nullptr,
                    0);
                batchCount = 0;
            }

            if (range.texture.ptr != 0)
                cmdList->SetGraphicsRootDescriptorTable(2, range.texture);

            activeTexture = range.texture;
            batchStart = cmdCursor;
        }

        batchCount += range.numCommands;
        cmdCursor += range.numCommands;
    }

    if (batchCount > 0)
    {
        cmdList->ExecuteIndirect(
            cmdSig.Get(),
            batchCount,
            currentFrameResource->GPUArgumentBuffer.Get(),
            batchStart * sizeof(IndirectDrawCommand),
            nullptr,
            0);
    }
}
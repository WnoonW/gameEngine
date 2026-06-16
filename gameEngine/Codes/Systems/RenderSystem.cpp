#include <DirectXMath.h>
#include <unordered_map>
#include <string>
#include <algorithm>

#include "RenderSystem.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "constantStruct.h"
#include "d3dUtil.h"

using namespace DirectX;

namespace
{
    struct BatchKey
    {
        Mesh* mesh = nullptr;
        Material* material = nullptr;
        std::string submeshName;

        bool operator==(const BatchKey& other) const
        {
            return mesh == other.mesh
                && material == other.material
                && submeshName == other.submeshName;
        }
    };

    struct BatchInstance
    {
        XMMATRIX worldViewProj = XMMatrixIdentity();
        const SubmeshGeometry* submesh = nullptr;
    };
}

namespace std
{
    template<>
    struct hash<BatchKey>
    {
        size_t operator()(const BatchKey& key) const
        {
            size_t hashValue = hash<void*>{}(key.mesh);
            hashValue ^= hash<void*>{}(key.material) << 1;
            hashValue ^= hash<string>{}(key.submeshName) << 2;
            return hashValue;
        }
    };
}

void RenderSystem::Initialize(ID3D12Device* device,
    std::vector<std::unique_ptr<FrameResource>>& frameResources,
    int gNumFrameResources,
    DescriptorAllocator& descriptorAllocator)
{
    mPassCBVHandles.resize(gNumFrameResources);
    mInstanceSRVHandles.resize(gNumFrameResources);
    mMaxInstancesPerFrame = static_cast<UINT>(
        frameResources[0]->InstanceDataBuffer->Resource()->GetDesc().Width / sizeof(InstanceData));

    for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
    {
        auto passCB = frameResources[frameIndex]->PassCB->Resource();
        auto instanceBuffer = frameResources[frameIndex]->InstanceDataBuffer->Resource();

        auto passHandle = descriptorAllocator.Allocate();
        mPassCBVHandles[frameIndex] = passHandle;

        D3D12_CONSTANT_BUFFER_VIEW_DESC passCbvDesc{};
        passCbvDesc.BufferLocation = passCB->GetGPUVirtualAddress();
        passCbvDesc.SizeInBytes = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));
        device->CreateConstantBufferView(&passCbvDesc, passHandle.CPU);

        auto instanceHandle = descriptorAllocator.Allocate();
        mInstanceSRVHandles[frameIndex] = instanceHandle;

        D3D12_SHADER_RESOURCE_VIEW_DESC instanceSrvDesc{};
        instanceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        instanceSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        instanceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        instanceSrvDesc.Buffer.FirstElement = 0;
        instanceSrvDesc.Buffer.NumElements = mMaxInstancesPerFrame;
        instanceSrvDesc.Buffer.StructureByteStride = sizeof(InstanceData);
        instanceSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(instanceBuffer, &instanceSrvDesc, instanceHandle.CPU);
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
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mLastRenderStats = {};
    mLastRenderStats.instanceBufferCapacity = static_cast<int>(mMaxInstancesPerFrame);

    XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
    std::unordered_map<BatchKey, std::vector<BatchInstance>> batches;

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity /*entity*/, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;

            ++mLastRenderStats.totalRenderableEntities;

            XMMATRIX worldMat = tf.GetWorldMatrix();
            XMMATRIX wvp = XMMatrixMultiply(worldMat, viewProj);

            for (auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                BatchKey key{
                    .mesh = rend.mesh,
                    .material = material,
                    .submeshName = pair.first
                };

                batches[key].push_back(BatchInstance{
                    .worldViewProj = wvp,
                    .submesh = &sub
                    });
            }
        });

    ID3D12RootSignature* instancingRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::Instancing);

    PSOKey psoKey{};
    psoKey.shaderName = "instancing";
    psoKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(psoKey, instancingRS);
    if (!pso) return;

    cmdList->SetGraphicsRootSignature(instancingRS);
    cmdList->SetPipelineState(pso);

    for (auto& [batchKey, instances] : batches)
    {
        if (instances.empty()) continue;

        const SubmeshGeometry& submesh = *instances[0].submesh;
        auto vbv = batchKey.mesh->VertexBufferView();
        auto ibv = batchKey.mesh->IndexBufferView();

        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        cmdList->SetGraphicsRootDescriptorTable(0, mPassCBVHandles[currentFrameIndex].GPU);
        cmdList->SetGraphicsRootDescriptorTable(1, batchKey.material->mTextureHandle.GPU);
        cmdList->SetGraphicsRootDescriptorTable(2, mInstanceSRVHandles[currentFrameIndex].GPU);

        UINT batchStart = 0;
        const UINT totalInstances = static_cast<UINT>(instances.size());
        if (static_cast<int>(totalInstances) > mLastRenderStats.peakInstancesPerBatch)
            mLastRenderStats.peakInstancesPerBatch = static_cast<int>(totalInstances);

        while (batchStart < totalInstances)
        {
            UINT chunkCount = totalInstances - batchStart;
            if (chunkCount > mMaxInstancesPerFrame)
                chunkCount = mMaxInstancesPerFrame;

            for (UINT i = 0; i < chunkCount; ++i)
            {
                InstanceData data{};
                XMStoreFloat4x4(
                    &data.WorldViewProj,
                    XMMatrixTranspose(instances[batchStart + i].worldViewProj));
                currentFrameResource->InstanceDataBuffer->CopyData(static_cast<int>(i), data);
            }

            PassConstants passConstants{};
            passConstants.InstanceOffset = 0;
            currentFrameResource->PassCB->CopyData(0, passConstants);

            cmdList->DrawIndexedInstanced(
                submesh.IndexCount,
                chunkCount,
                submesh.StartIndexLocation,
                submesh.BaseVertexLocation,
                0);

            batchStart += chunkCount;
            ++mLastRenderStats.drawBatches;
        }
    }
}
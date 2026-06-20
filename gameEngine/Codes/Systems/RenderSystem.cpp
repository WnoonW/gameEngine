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
    mIndirectArgsUAVHandles.resize(gNumFrameResources);
    mDrawCommandSRVHandles.resize(gNumFrameResources);
    mInstanceDataSRVHandles.resize(gNumFrameResources);
    mCompactedInstanceUAVHandles.resize(gNumFrameResources);

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

        // IndirectArgs UAV Descriptor
        auto indirectArgsBuffer = frameResources[frameIndex]->IndirectArgsUAVBuffer.Get();
        auto indirectHandle = descriptorAllocator.Allocate();
        mIndirectArgsUAVHandles[frameIndex] = indirectHandle;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = 8192;
        uavDesc.Buffer.StructureByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        device->CreateUnorderedAccessView(indirectArgsBuffer, nullptr, &uavDesc, indirectHandle.CPU);

        // DrawCommandBuffer SRV
        auto drawCmdBuffer = frameResources[frameIndex]->DrawCommandBuffer->Resource();
        auto drawCmdHandle = descriptorAllocator.Allocate();
        mDrawCommandSRVHandles[frameIndex] = drawCmdHandle;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = 4096;
        srvDesc.Buffer.StructureByteStride = sizeof(IndirectDrawCommand);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(drawCmdBuffer, &srvDesc, drawCmdHandle.CPU);


        // === InstanceDataBuffer SRV 생성 (Compute Shader용) ===
        auto instanceDataBuffer = frameResources[frameIndex]->InstanceDataBuffer->Resource();
        auto instanceDataHandle = descriptorAllocator.Allocate();
        mInstanceDataSRVHandles[frameIndex] = instanceDataHandle;

        D3D12_SHADER_RESOURCE_VIEW_DESC instanceDataSrvDesc{};
        instanceDataSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        instanceDataSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        instanceDataSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        instanceDataSrvDesc.Buffer.FirstElement = 0;
        instanceDataSrvDesc.Buffer.NumElements = mMaxInstancesPerFrame;
        instanceDataSrvDesc.Buffer.StructureByteStride = sizeof(InstanceData);
        instanceDataSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        device->CreateShaderResourceView(instanceDataBuffer, &instanceDataSrvDesc, instanceDataHandle.CPU);
    
        // === CompactedInstanceBuffer UAV 생성 ===
        auto compactedBuffer = frameResources[frameIndex]->CompactedInstanceBuffer.Get();
        auto compactedHandle = descriptorAllocator.Allocate();
        mCompactedInstanceUAVHandles[frameIndex] = compactedHandle;

        D3D12_UNORDERED_ACCESS_VIEW_DESC compactedUavDesc{};
        compactedUavDesc.Format = DXGI_FORMAT_UNKNOWN;
        compactedUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        compactedUavDesc.Buffer.FirstElement = 0;
        compactedUavDesc.Buffer.NumElements = mMaxInstancesPerFrame;   // 최대 인스턴스 수
        compactedUavDesc.Buffer.StructureByteStride = sizeof(InstanceData);
        compactedUavDesc.Buffer.CounterOffsetInBytes = 0;
        compactedUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        device->CreateUnorderedAccessView(compactedBuffer, nullptr, &compactedUavDesc, compactedHandle.CPU);
    }
}

void RenderSystem::render(World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    mLastRenderStats = {};

    struct BatchKey {
        Mesh* mesh;
        Material* material;
        std::string submeshName;

        bool operator==(const BatchKey& other) const {
            return mesh == other.mesh && material == other.material && submeshName == other.submeshName;
        }
    };

    struct BatchInstance {
        XMMATRIX worldViewProj;
        const SubmeshGeometry* submesh;
    };

    struct BatchDrawCall {
        BatchKey key;
        UINT argIndex;
    };

    struct BatchKeyHash {
        size_t operator()(const BatchKey& k) const {
            size_t h1 = std::hash<Mesh*>()(k.mesh);
            size_t h2 = std::hash<Material*>()(k.material);
            size_t h3 = std::hash<std::string>()(k.submeshName);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(1, descriptorHeaps);

    XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);

    std::unordered_map<BatchKey, std::vector<BatchInstance>, BatchKeyHash> batches;

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity /*entity*/, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh || !rend.material) return;

            XMMATRIX worldMat = tf.GetWorldMatrix();
            XMMATRIX wvp = XMMatrixMultiply(worldMat, viewProj);

            for (auto& pair : rend.mesh->DrawArgs)
            {
                const std::string& submeshName = pair.first;
                const SubmeshGeometry& sub = pair.second;

                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                BatchKey key{
                    .mesh = rend.mesh,
                    .material = material,
                    .submeshName = submeshName
                };
                batches[key].push_back({ wvp, &sub });
            }
        });

    std::vector<BatchDrawCall> drawCalls;
    UINT instanceOffset = 0;
    UINT argIndex = 0;

    for (auto& [key, instances] : batches)
    {
        if (instances.empty()) continue;

        for (size_t i = 0; i < instances.size(); ++i)
        {
            InstanceData data{};
            XMStoreFloat4x4(&data.WorldViewProj, XMMatrixTranspose(instances[i].worldViewProj));
            currentFrameResource->InstanceDataBuffer->CopyData(instanceOffset + static_cast<int>(i), data);
        }

        for (size_t chunkStart = 0; chunkStart < instances.size(); chunkStart += mMaxInstancesPerFrame)
        {
            UINT chunkCount = static_cast<UINT>(std::min<size_t>(mMaxInstancesPerFrame, instances.size() - chunkStart));
            const SubmeshGeometry* submesh = instances[0].submesh;

            IndirectDrawCommand drawCmd{};
            drawCmd.IndexCountPerInstance = submesh->IndexCount;
            drawCmd.StartIndexLocation = submesh->StartIndexLocation;
            drawCmd.BaseVertexLocation = submesh->BaseVertexLocation;
            drawCmd.InstanceCount = chunkCount;
            drawCmd.StartInstanceLocation = instanceOffset + static_cast<UINT>(chunkStart);  // ← 여기 추가
            drawCmd.MaterialIndex = 0;

            currentFrameResource->DrawCommandBuffer->CopyData(argIndex, drawCmd);

            drawCalls.push_back({ key, argIndex });
            argIndex++;
        }

        instanceOffset += static_cast<UINT>(instances.size());
    }

    // === Compute Shader Dispatch (배칭 후에 호출) ===
    if (currentFrameIndex < mIndirectArgsUAVHandles.size() && argIndex > 0)
    {
        DispatchFrustumCulling(cmdList, currentFrameResource, descriptorAllocator, currentFrameIndex, argIndex, viewProj);

        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            currentFrameResource->IndirectArgsUAVBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
        );
        cmdList->ResourceBarrier(1, &barrier);
    }

    mLastRenderStats.totalRenderableEntities = instanceOffset;
    mLastRenderStats.drawBatches = static_cast<int>(drawCalls.size());

    if (drawCalls.empty()) return;

    auto* cmdSig = PipelineStateManager::Get().GetDrawIndexedIndirectSignature();
    ID3D12RootSignature* instancingRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Instancing);

    cmdList->SetGraphicsRootSignature(instancingRS);

    PSOKey psoKey;
    psoKey.shaderName = "instancing";
    psoKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoKey.rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    psoKey.rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;

    ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(psoKey, instancingRS);
    cmdList->SetPipelineState(pso);

    for (auto& drawCall : drawCalls)
    {
        auto& key = drawCall.key;

        auto vbv = key.mesh->VertexBufferView();
        auto ibv = key.mesh->IndexBufferView();

        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        cmdList->SetGraphicsRootDescriptorTable(0, mPassCBVHandles[currentFrameIndex].GPU);
        cmdList->SetGraphicsRootDescriptorTable(1, key.material->mTextureHandle.GPU);
        cmdList->SetGraphicsRootDescriptorTable(2, mInstanceSRVHandles[currentFrameIndex].GPU);

        UINT64 argOffset = drawCall.argIndex * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);

        cmdList->ExecuteIndirect(
            cmdSig,
            1,
            currentFrameResource->IndirectArgsUAVBuffer.Get(),
            argOffset,
            nullptr,
            0
        );
    }
}

void RenderSystem::DispatchFrustumCulling(
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    UINT drawCommandCount,
    const DirectX::XMMATRIX& viewProj)
{
    if (!descriptorAllocator) return;

    if (currentFrameIndex >= mIndirectArgsUAVHandles.size())
    {
        OutputDebugStringA("[RenderSystem] currentFrameIndex out of range in DispatchFrustumCulling\n");
        return;
    }

    auto* computeRS = PipelineStateManager::Get().GetComputeRootSignature();
    auto* computePSO = PipelineStateManager::Get().GetOrCreateComputePSO(
        PSOKey{ .shaderName = "FrustumCullingCS", .isComputeShader = true },
        computeRS
    );

    if (!computePSO) return;

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(1, descriptorHeaps);

    struct CullConstants
    {
        XMMATRIX ViewProj;
        UINT       DrawCommandCount;
    };

    CullConstants cullData{};
    cullData.ViewProj = viewProj;
    cullData.DrawCommandCount = drawCommandCount;

    cmdList->SetComputeRootSignature(computeRS);
    cmdList->SetPipelineState(computePSO);


    cmdList->SetComputeRootDescriptorTable(0, mIndirectArgsUAVHandles[currentFrameIndex].GPU);      // u0 - IndirectArgs
    cmdList->SetComputeRootDescriptorTable(1, mCompactedInstanceUAVHandles[currentFrameIndex].GPU); // u1 - CompactedInstance (새로 추가)

    cmdList->SetComputeRoot32BitConstants(2, 17, &cullData, 0);                                      // b0 - ViewProj + Count

    cmdList->SetComputeRootDescriptorTable(3, mDrawCommandSRVHandles[currentFrameIndex].GPU);       // t0 - DrawCommands
    cmdList->SetComputeRootDescriptorTable(4, mInstanceDataSRVHandles[currentFrameIndex].GPU);      // t1 - InstanceData

    UINT threadGroupCount = (drawCommandCount + 63) / 64;
    cmdList->Dispatch(threadGroupCount, 1, 1);
}
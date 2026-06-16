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
    // 새 ECS 방식으로 컴포넌트 가져오기
    RenderableComponent* rend = world.GetComponent<RenderableComponent>(entity);
    if (!rend) return;   // 안전장치

    // 엔티티 인덱스에 맞춰 벡터 크기 확장
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

void RenderSystem::render(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    // Descriptor Heap 설정
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    ID3D12RootSignature* lastRS = nullptr;
    ID3D12PipelineState* lastPSO = nullptr;

    // === 새 ECS 방식으로 변경 ===
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;

            // 1. Transform → CB 업데이트
            XMMATRIX worldMat = tf.GetWorldMatrix();
            XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
            XMMATRIX wvp = XMMatrixMultiply(worldMat, viewProj);

            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));
            currentFrameResource->ObjectCB->CopyData(rend.objectCBIndex, objConst);

            // 2. Mesh 바인딩
            auto vbv = rend.mesh->VertexBufferView();
            auto ibv = rend.mesh->IndexBufferView();

            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // 3. RootSignature + PSO (Last State 캐싱)
            ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);

            PSOKey key{};
            key.shaderName = "object";
            key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

            ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);

            if (sceneRS != lastRS) { cmdList->SetGraphicsRootSignature(sceneRS); lastRS = sceneRS; }
            if (pso != lastPSO) { cmdList->SetPipelineState(pso); lastPSO = pso; }

            // 4. Material 바인딩 + 그리기
            for (auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                // CBV 바인딩
                if (e < mEntityCBVHandles.size() &&
                    currentFrameIndex < mEntityCBVHandles[e].size())
                {
                    cmdList->SetGraphicsRootDescriptorTable(0, mEntityCBVHandles[e][currentFrameIndex].GPU);
                }

                // Texture 바인딩
                cmdList->SetGraphicsRootDescriptorTable(1, material->mTextureHandle.GPU);

                cmdList->DrawIndexedInstanced(
                    sub.IndexCount, 1,
                    sub.StartIndexLocation,
                    sub.BaseVertexLocation, 0);
            }
        });
}
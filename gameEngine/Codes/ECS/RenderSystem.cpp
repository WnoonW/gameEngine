#include <DirectXMath.h>
#include "RenderSystem.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "constantStruct.h"
#include "AppStruct.h"

using namespace DirectX;

void RenderSystem::createCBV(ID3D12Device* device,
    std::vector<std::unique_ptr<FrameResource>>& frameResources,
    int gNumFrameResources,
    DescriptorAllocator& descriptorAllocator,
    Entity entity,
    Registry& registry)
{
    auto& rend = registry.getComponent<RenderableComponent>(entity);

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
        cbvDesc.BufferLocation = objectCB->GetGPUVirtualAddress() + (UINT64)rend.objectCBIndex * objCBByteSize;
        cbvDesc.SizeInBytes = objCBByteSize;

        device->CreateConstantBufferView(&cbvDesc, handle.CPU);
    }
}

void RenderSystem::render(Registry& registry,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    auto entities = registry.view<TransformComponent, RenderableComponent>();

    // Descriptor Heap 설정
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    for (Entity e : entities)
    {
        auto& tf = registry.getComponent<TransformComponent>(e);
        auto& rend = registry.getComponent<RenderableComponent>(e);

        if (!rend.visible) continue;

        // 1. 행렬 계산 및 CB 데이터 업데이트
        XMMATRIX world = tf.GetWorldMatrix();
        XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
        XMMATRIX wvp = XMMatrixMultiply(world, viewProj);

        ObjectConstants objConst{};
        XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));
        currentFrameResource->ObjectCB->CopyData(rend.objectCBIndex, objConst);

        // 2. Mesh
        Mesh* mesh = MeshManager::Get().GetMesh(rend.meshName);
        if (!mesh || mesh->DrawArgs.empty()) continue;

        auto vbv = mesh->VertexBufferView();
        auto ibv = mesh->IndexBufferView();

        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
        cmdList->SetGraphicsRootSignature(sceneRS);

        PSOKey key;
        key.shaderName = "object";
        key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);
        cmdList->SetPipelineState(pso);

        // =====================================================
        // 서브메시별 Material 바인딩 + 그리기
        // =====================================================
        for (auto& pair : mesh->DrawArgs)
        {
            const auto& sub = pair.second;
            Material* material = sub.material;
            


            // CBV 바인딩 (Root Parameter 0)
            if (!mEntityCBVHandles.empty() &&
                e < mEntityCBVHandles.size() &&
                currentFrameIndex < mEntityCBVHandles[e].size())
            {
                cmdList->SetGraphicsRootDescriptorTable(0, mEntityCBVHandles[e][currentFrameIndex].GPU);
            }

            // Texture 바인딩 (Root Parameter 1)
            cmdList->SetGraphicsRootDescriptorTable(1, material->mTextureHandle.GPU);

            // 서브메시 그리기
            cmdList->DrawIndexedInstanced(
                sub.IndexCount,
                1,
                sub.StartIndexLocation,
                sub.BaseVertexLocation,
                0
            );
        }
    }
}
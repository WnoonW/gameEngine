#include <DirectXMath.h>
#include "RenderSystem.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "constantStruct.h"
#include "d3dUtil.h"

using namespace DirectX;

void RenderSystem::render(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    (void)viewMatrix;
    (void)projMatrix;

    // Descriptor Heap 설정 (Texture SRV 때문에 필요)
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    PSOKey key{};
    key.shaderName = "object_cb";   // 새로 만든 셰이더
    key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);

    // 공통 상태는 루프 바깥에서 한 번만 설정 (순서 중요: RootSig -> PSO)
    if (sceneRS)
    {
        cmdList->SetGraphicsRootSignature(sceneRS);
    }
    if (pso)
    {
        cmdList->SetPipelineState(pso);
    }

    // PassCB (b1)는 한 번만 바인딩
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        auto passCB = currentFrameResource->PassCB->Resource();
        cmdList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());
    }

    // === ECS 렌더링 ===
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;

            // 1. Transform → ObjectCB 업데이트 (b0) — World 행렬만 저장
            XMMATRIX worldMat = tf.GetWorldMatrix();

            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(worldMat));
            currentFrameResource->ObjectCB->CopyData(rend.objectCBIndex, objConst);

            auto objectCB = currentFrameResource->ObjectCB->Resource();
            UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
            D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() +
                (UINT64)rend.objectCBIndex * objCBByteSize;

            // 2. Mesh 바인딩
            auto vbv = rend.mesh->VertexBufferView();
            auto ibv = rend.mesh->IndexBufferView();

            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // 3. Per-object 바인딩 + 그리기
            for (auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = MaterialManager::Get().ResolveForDraw(e, pair.first, sub.initMaterial);
                if (!material) continue;

                // ObjectCB 직접 바인딩 (b0)
                cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

                // Texture (t0) - root index 2 (Descriptor Table)
                cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);

                cmdList->DrawIndexedInstanced(
                    sub.IndexCount, 1,
                    sub.StartIndexLocation,
                    sub.BaseVertexLocation, 0);
            }
        });
}
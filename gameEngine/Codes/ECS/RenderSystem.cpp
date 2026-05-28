#include "RenderSystem.h"
#include "MatarialManager.h"
#include "MeshManager.h"
#include <DirectXMath.h>

using namespace DirectX;

void RenderSystem::render(Registry& registry,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    auto entities = registry.view<TransformComponent, RenderableComponent>();

    for (Entity e : entities) {
        auto& tf = registry.getComponent<TransformComponent>(e);
        auto& rend = registry.getComponent<RenderableComponent>(e);

        if (!rend.visible) continue;

        // 1. World + MVP 계산
        XMMATRIX world = tf.GetWorldMatrix();
        XMMATRIX mvp = XMMatrixMultiply(XMMatrixMultiply(world, viewMatrix), projMatrix);

        // 2. ObjectConstants 업데이트 (네 프로젝트에 맞는 구조체 사용)
        ObjectConstants objConst{};
        XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(world));
        XMStoreFloat4x4(&objConst.MVP, XMMatrixTranspose(mvp));
        // objConst에 네가 추가로 쓰는 값들 더 채우기

        currentFrameResource->ObjectCB->CopyData(rend.objectCBIndex, &objConst);

        // 3. Material & Mesh 가져오기
        auto material = MatarialManager::Get().GetMatarial(rend.materialName);
        auto mesh = MeshManager::Get().GetMesh(rend.meshName);

        if (!material || !mesh) continue;

        // 4. Root Signature & PSO 설정
        cmdList->SetGraphicsRootSignature(material->mRootSignature.Get());
        cmdList->SetPipelineState(material->mPSO.Get());

        // 5. CBV 설정 (objectCBIndex에 해당하는 CBV)
        //    → 네가 미리 FrameResource에 CBV를 만들어뒀다면 여기서 설정
        //    예시: cmdList->SetGraphicsRootDescriptorTable(1, handle);

        // 6. Vertex / Index Buffer 설정
        cmdList->IASetVertexBuffers(0, 1, &mesh->VertexBufferView);
        cmdList->IASetIndexBuffer(&mesh->IndexBufferView);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // 7. Draw
        cmdList->DrawIndexedInstanced(mesh->IndexCount, 1, 0, 0, 0);
    }
}
#include "RenderSystem.h"

// === 네 프로젝트 실제 경로에 맞게 include 수정 ===
#include "../V1_/V4/MatarialManager.h"
#include "../V1_/V3/MeshManager.h"
#include "../Structs/constantStruct.h"
#include "../Structs/AppStruct.h"
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

    for (Entity e : entities)
    {
        auto& tf = registry.getComponent<TransformComponent>(e);
        auto& rend = registry.getComponent<RenderableComponent>(e);

        if (!rend.visible) continue;

        // ==========================================
        // 1. World + ViewProj 계산
        // ==========================================
        XMMATRIX world = tf.GetWorldMatrix();
        XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
        XMMATRIX wvp = XMMatrixMultiply(world, viewProj);

        // ==========================================
        // 2. ObjectConstants 업데이트 (실제 멤버에 맞춤)
        // ==========================================
        ObjectConstants objConst{};
        XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));

        currentFrameResource->ObjectCB->CopyData(rend.objectCBIndex, objConst);

        // ==========================================
        // 3. Mesh & Material 가져오기
        // ==========================================
        Mesh* mesh = MeshManager::Get().GetMesh(rend.meshName);
        auto material = MatarialManager::Get().GetMatarial(rend.materialName);

        if (!mesh || !material) continue;

        // ==========================================
        // 4. RootSignature & PSO 설정
        //    → Matarial에 mRootSignature/mPSO가 없으므로
        //      일단 주석. 나중에 Material이나 RenderPass에서 관리 예정
        // ==========================================
        // cmdList->SetGraphicsRootSignature(???);
        // cmdList->SetPipelineState(???);

        // TODO: 현재는 RenderSystem 바깥에서 미리 SetGraphicsRootSignature + SetPipelineState 해두는 걸 추천
        //       (같은 Material끼리 묶어서 한 번만 설정하면 더 효율적)

        // ==========================================
        // 5. Vertex / Index Buffer 설정 (함수 호출로 수정!)
        // ==========================================
        auto vbv = mesh->VertexBufferView();   // 함수 호출
        auto ibv = mesh->IndexBufferView();    // 함수 호출

        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // ==========================================
        // 6. Draw (indexCount 직접 사용 가능)
        // ==========================================
        UINT indexCount = mesh->indexCount;

        // DrawArgs에 서브메시 정보가 있으면 우선 사용 (더 정확)
        if (!mesh->DrawArgs.empty())
        {
            // 첫 번째 서브메시 사용 (나중에 선택 기능 추가 가능)
            const auto& submesh = mesh->DrawArgs.begin()->second;
            indexCount = submesh.IndexCount;
        }

        if (indexCount > 0)
        {
            cmdList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
        }
    }
}
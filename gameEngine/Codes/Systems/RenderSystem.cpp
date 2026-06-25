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

void RenderSystem::renderExecuteIndirect(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    // ============================================================
    // renderExecuteIndirect - ExecuteIndirect + State Batching 기반 렌더링
    // ------------------------------------------------------------
    // 목적:
    //   - DrawCall을 간접 명령(IndirectDrawCommand)으로 미리 기록
    //   - 같은 Mesh / ObjectCB / Texture를 사용하는 연속 Draw를 "배치(Batch)"로 묶어
    //     ExecuteIndirect 호출 횟수와 상태 변경 횟수를 최소화
    // 주요 최적화 포인트:
    //   1. Mesh / CBV / Texture 상태가 바뀔 때만 IA, RootCBV, DescriptorTable 바인딩
    //   2. 상태가 동일한 DrawCommand들을 하나의 ExecuteIndirect로 묶어서 실행
    //   3. CommandSignature를 통해 DrawIndexed + 소량 Root Constant(objectCBIndex)를 동시에 제어
    // ============================================================

    // 1. Descriptor Heap 설정 (Texture SRV 바인딩을 위해 필요)
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    // 2. RootSignature / PSO 준비
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

    // CommandSignature 가져오기 (IndirectDrawCommand 해석 규칙 정의)
    //   - [0] Root Constants (1 DWORD) → Root Parameter 3 (objectCBIndex, 셰이더에서는 현재 미사용)
    //   - [1] DrawIndexedArguments
    ComPtr<ID3D12CommandSignature> cmdSig =
        RootSignatureManager::Get().GetOrCreateCommandSignature(sceneRS);
    if (!cmdSig)
        return;

    // 3. 그래픽스 파이프라인 상태 설정
    cmdList->SetGraphicsRootSignature(sceneRS);
    cmdList->SetPipelineState(pso);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 4. PassConstants 업데이트 및 바인딩 (ViewProj)
    UpdatePassCB(currentFrameResource, cmdList, viewMatrix, projMatrix);

    // ============================================================
    // 5. Indirect Argument Buffer 준비
    // ------------------------------------------------------------
    // FrameResource에 미리 생성된 Upload 버퍼를 CPU에서 직접 기록
    // 각 IndirectDrawCommand는 CommandSignature와 1:1 대응
    // ============================================================
    IndirectDrawCommand* cmdBuffer =
        reinterpret_cast<IndirectDrawCommand*>(currentFrameResource->MappedArgumentBuffer);

    const UINT maxCommandCount =
        currentFrameResource->ArgumentBufferSize / sizeof(IndirectDrawCommand);

    // ============================================================
    // 6. 상태 추적 및 배치(Batching) 변수
    // ------------------------------------------------------------
    // DrawBindState: 현재 GPU에 바인딩된 상태를 기억하여 중복 바인딩 방지
    //   - mesh           : Vertex/Index Buffer
    //   - objectCBAddress: ObjectConstants CBV (Root Parameter 0)
    //   - texture        : Material Texture SRV (Root Parameter 2)
    //
    // 배치 변수:
    //   - writeIndex  : ArgumentBuffer에 명령을 기록할 다음 위치
    //   - batchStart  : 현재 배치가 시작된 writeIndex
    //   - batchCount  : 현재 배치에 포함된 연속 DrawCommand 개수
    // ============================================================
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

    // ============================================================
    // 7. flushBatch() - 현재까지 모은 배치를 ExecuteIndirect로 실행
    // ------------------------------------------------------------
    // batchCount > 0 일 때만 호출.
    // ExecuteIndirect( batchCount, ArgumentBuffer, batchStart * stride )
    //   → GPU가 ArgumentBuffer에서 batchCount개의 IndirectDrawCommand를 연속으로 해석하여 Draw
    // ============================================================
    auto flushBatch = [&]()
    {
        if (batchCount == 0)
            return;

        cmdList->ExecuteIndirect(
            cmdSig.Get(),
            batchCount,
            currentFrameResource->ArgumentBuffer.Get(),
            batchStart * sizeof(IndirectDrawCommand),
            nullptr,   // count buffer 없음 (고정 개수)
            0);

        batchCount = 0;
    };

    // ============================================================
    // 8. applyBindState() - 상태가 바뀐 경우에만 실제 바인딩 수행
    // ------------------------------------------------------------
    // 변경된 항목만 선택적으로 바인딩 (상태 변경 최소화)
    //   - Mesh가 다르면 → IASetVertexBuffers / IASetIndexBuffer
    //   - ObjectCB가 다르면 → SetGraphicsRootConstantBufferView(0, ...)
    //   - Texture가 다르면 → SetGraphicsRootDescriptorTable(2, ...)
    // ============================================================
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

    // ============================================================
    // 9. 메인 루프: ECS 엔티티 순회 + Indirect Command 기록
    // ------------------------------------------------------------
    // ForEach<Transform, Renderable> 로 모든 렌더 대상 엔티티 순회
    //
    // 처리 흐름 (엔티티당):
    //   a. 유효성 검사 (visible, mesh 존재, 버퍼 존재, objectCBIndex 범위)
    //   b. WorldMatrix 계산 → ObjectConstants 업로드 → GPU 주소 계산
    //   c. Mesh의 모든 DrawArg(서브메시) 순회
    //      - material 결정 (SubMesh 전용 material 우선, 없으면 Renderable 기본 material)
    //      - 현재 상태(activeState)와 비교하여 stateChanged 여부 판정
    //      - stateChanged == true → flushBatch() + applyBindState() + 새 배치 시작
    //      - IndirectDrawCommand 채우기:
    //          · objectCBIndex (CommandSignature의 CONSTANT 인자)
    //          · D3D12_DRAW_INDEXED_ARGUMENTS (IndexCount, InstanceCount=1, Start/Offset 등)
    //      - writeIndex++, batchCount++
    //
    // 버퍼 초과 시: flush 후 즉시 return (안전 종료)
    // ============================================================
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

                // === 상태 변경 감지 ===
                const bool stateChanged =
                    rend.mesh != activeState.mesh ||
                    objectCBAddress != activeState.objectCBAddress ||
                    textureHandle.ptr != activeState.texture.ptr;

                if (stateChanged)
                {
                    // 이전 배치 실행 + 새 상태 바인딩 + 새 배치 시작점 기록
                    flushBatch();
                    applyBindState(rend.mesh, objectCBAddress, textureHandle);
                    batchStart = writeIndex;
                }

                // IndirectDrawCommand 기록 (CPU에서 ArgumentBuffer에 직접 씀)
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

    // 10. 마지막에 남은 배치 실행
    flushBatch();
}
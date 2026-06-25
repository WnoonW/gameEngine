#include <DirectXMath.h>
#include <map>
#include <tuple>
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
    // renderExecuteIndirect - ExecuteIndirect + Instancing 최적화 버전
    // ------------------------------------------------------------
    // 목적:
    //   - 같은 Mesh + 같은 Material 을 사용하는 여러 엔티티를 하나의 Indirect 명령으로 묶음
    //   - drawArgs.InstanceCount 를 실제 인스턴스 개수로 설정
    // 핵심 (2번 + 3번 솔루션 적용):
    //   - InstanceBuffer를 MaxInstanceCount(65536)로 독립 확대 (솔루션 2)
    //   - 대형 그룹은 chunk 단위로 여러 Indirect 명령 분할 (솔루션 3)
    //   - (mesh + material + draw args) 단위 그룹핑 + InstanceBuffer + SV_InstanceID
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

    // CommandSignature (첫 uint32 = baseInstance 를 root constant로 전달)
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
    // InstanceBuffer SRV 바인딩 (root parameter 4 = t1)
    // ============================================================
    if (currentFrameResource->InstanceBuffer)
    {
        cmdList->SetGraphicsRootShaderResourceView(
            4,
            currentFrameResource->InstanceBuffer->GetGPUVirtualAddress());
    }

    // ============================================================
    // Indirect Argument Buffer + Instance Data Buffer 준비
    // InstanceBuffer는 StructuredBuffer로 바인딩되어 SV_InstanceID 인덱싱에 사용됨
    // ============================================================
    IndirectDrawCommand* cmdBuffer =
        reinterpret_cast<IndirectDrawCommand*>(currentFrameResource->MappedArgumentBuffer);

    ObjectConstants* instanceData =
        reinterpret_cast<ObjectConstants*>(currentFrameResource->MappedInstanceBuffer);

    const UINT maxCommandCount =
        currentFrameResource->ArgumentBufferSize / sizeof(IndirectDrawCommand);

    const UINT maxInstanceCount =
        currentFrameResource->InstanceBufferSize / sizeof(ObjectConstants);
    // 솔루션 2 적용: maxInstanceCount이 이제 65536 (독립적 큰 버퍼)

    // ============================================================
    // 상태 추적 (mesh + texture 만)
    // ============================================================
    struct DrawBindState
    {
        Mesh* mesh = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE texture{};
    };

    DrawBindState activeState{};
    UINT writeIndex = 0;
    UINT batchStart = 0;
    UINT batchCount = 0;

    // flushBatch: 현재까지 모은 명령 배치를 ExecuteIndirect로 실행

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

    // applyBindState: mesh 또는 texture가 바뀔 때만 바인딩 (상태 변경 최소화)
    // mesh + texture 만 바인딩
    auto applyBindState = [&](Mesh* mesh, D3D12_GPU_DESCRIPTOR_HANDLE texture)
    {
        if (mesh != activeState.mesh)
        {
            auto vbv = mesh->VertexBufferView();
            auto ibv = mesh->IndexBufferView();
            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            activeState.mesh = mesh;
        }

        if (texture.ptr != activeState.texture.ptr)
        {
            cmdList->SetGraphicsRootDescriptorTable(2, texture);
            activeState.texture = texture;
        }
    };

    // ============================================================
    // 그룹 수집 + InstanceBuffer 기록 + InstanceCount 적용
    // ============================================================
    using DrawKey = std::tuple<Mesh*, Material*, UINT, UINT, INT>;

    std::map<DrawKey, std::vector<XMMATRIX>> groups;

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (!rend.mesh->vertexBuffer || !rend.mesh->indexBuffer) return;
            if (rend.objectCBIndex >= RenderLimits::MaxObjectCount) return;

            XMMATRIX worldMat = tf.GetWorldMatrix();

            // 기존 ObjectCB에도 기록 (다른 렌더 경로 호환)
            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(worldMat));
            currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);

            for (auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                DrawKey key = std::make_tuple(
                    rend.mesh, material,
                    sub.IndexCount, sub.StartIndexLocation, sub.BaseVertexLocation);

                groups[key].push_back(worldMat);
            }
        });

    UINT instanceWritePos = 0;
    bool emissionLimitReached = false;

    for (const auto& kv : groups)
    {
        if (emissionLimitReached) break;

        const auto& key = kv.first;
        const auto& worldList = kv.second;

        if (worldList.empty()) continue;

        Mesh* meshPtr = std::get<0>(key);
        Material* matPtr = std::get<1>(key);
        UINT idxCount = std::get<2>(key);
        UINT startIdx = std::get<3>(key);
        INT baseVert = std::get<4>(key);

        D3D12_GPU_DESCRIPTOR_HANDLE texHandle = matPtr ? matPtr->mTextureHandle.GPU : D3D12_GPU_DESCRIPTOR_HANDLE{};

        const bool stateChanged =
            (meshPtr != activeState.mesh) || (texHandle.ptr != activeState.texture.ptr);

        if (stateChanged)
        {
            flushBatch();
            applyBindState(meshPtr, texHandle);
            batchStart = writeIndex;
        }

        // ============================================================
        // 솔루션 3: 대형 그룹 Chunking 적용
        // - 한 그룹(worldList)이 남은 슬롯보다 크면 여러 Indirect 명령으로 분할
        // - 같은 (mesh + material + draw range) 이므로 IA/Texture 재바인딩 없이
        //   동일 배치(ExecuteIndirect) 안에 여러 chunk 명령 포함 가능
        // ============================================================
        size_t groupPos = 0;
        while (groupPos < worldList.size())
        {
            if (writeIndex >= maxCommandCount)
            {
                flushBatch();
                emissionLimitReached = true;
                break;
            }

            UINT remaining = (instanceWritePos < maxInstanceCount)
                ? (maxInstanceCount - instanceWritePos)
                : 0;

            if (remaining == 0)
            {
                flushBatch();
                emissionLimitReached = true;
                break;
            }

            UINT chunkSize = min((UINT)(worldList.size() - groupPos), remaining);
            if (chunkSize == 0) break;

            const UINT baseInstance = instanceWritePos;

            // chunk 단위로 InstanceBuffer 기록
            for (size_t i = 0; i < chunkSize; ++i)
            {
                ObjectConstants oc{};
                XMStoreFloat4x4(&oc.World, XMMatrixTranspose(worldList[groupPos + i]));
                instanceData[baseInstance + i] = oc;
            }
            instanceWritePos += chunkSize;

            // 한 덩어리(chunk)에 대한 IndirectDrawCommand 기록
            cmdBuffer[writeIndex].baseInstance = baseInstance;
            cmdBuffer[writeIndex].drawArgs.IndexCountPerInstance = idxCount;
            cmdBuffer[writeIndex].drawArgs.InstanceCount = chunkSize;
            cmdBuffer[writeIndex].drawArgs.StartIndexLocation = startIdx;
            cmdBuffer[writeIndex].drawArgs.BaseVertexLocation = baseVert;
            cmdBuffer[writeIndex].drawArgs.StartInstanceLocation = baseInstance;

            ++writeIndex;
            ++batchCount;

            groupPos += chunkSize;
        }
    }

    flushBatch();
}
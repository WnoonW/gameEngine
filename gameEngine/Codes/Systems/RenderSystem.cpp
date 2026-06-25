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
    // renderExecuteIndirect - GPU-Driven + Merged Geometry (단일 큰 VB/IB)
    // 모든 지오메트리를 하나의 Vertex/Index Buffer에 넣고 BaseVertex/StartIndex로 구분
    // IA 바인딩은 한 번, texture 변경 시에만 추가 바인딩 + ExecuteIndirect
    // CommandSignature CONSTANT로 per-draw baseInstance 전달
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

    // ============================================================
    // 모든 지오메트리를 하나의 큰 Vertex/Index Buffer에 병합 (가장 일반적인 GPU-Driven 방식)
    // Submesh별 BaseVertexLocation / StartIndexLocation 으로 구분
    // 이제 IA 바인딩은 한 번만 하면 됨 (texture 변경 시에만 추가 바인딩)
    // ============================================================
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
    // 상태 추적 (texture만 - IA는 글로벌 VB/IB 공유, Base/StartIndex로 구분)
    // ============================================================
    struct DrawBindState
    {
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

    // applyBindState: texture만 (IA는 글로벌 버퍼 공유, Base/StartIndex로 메시 구분)
    auto applyBindState = [&](D3D12_GPU_DESCRIPTOR_HANDLE texture)
    {
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

    std::vector<GroupDrawData> groupDrawList;

    // For correct binding: record mesh+texture + how many commands per binding group
    struct BindingRange {
        Mesh* mesh = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE texture{};
        UINT numCommands = 0;
    };
    std::vector<BindingRange> bindingRanges;

    UINT instanceWritePos = 0;

    for (const auto& kv : groups)
    {
        const auto& key = kv.first;
        const auto& worldList = kv.second;

        if (worldList.empty()) continue;

        Mesh* meshPtr = std::get<0>(key);
        Material* matPtr = std::get<1>(key);
        UINT idxCount = std::get<2>(key);
        UINT startIdx = std::get<3>(key);
        INT  baseVert = std::get<4>(key);

        D3D12_GPU_DESCRIPTOR_HANDLE texHandle = matPtr ? matPtr->mTextureHandle.GPU : D3D12_GPU_DESCRIPTOR_HANDLE{};

        UINT cmdsThisRange = 0;

        // InstanceBuffer + GroupDrawData 수집 (CS가 IndirectDrawCommand 기록)
        size_t groupPos = 0;
        while (groupPos < worldList.size())
        {
            if (instanceWritePos >= maxInstanceCount) break;

            UINT remaining = maxInstanceCount - instanceWritePos;
            UINT chunkSize = min((UINT)(worldList.size() - groupPos), remaining);
            if (chunkSize == 0) break;

            const UINT baseInstance = instanceWritePos;

            for (size_t i = 0; i < chunkSize; ++i)
            {
                ObjectConstants oc{};
                XMStoreFloat4x4(&oc.World, XMMatrixTranspose(worldList[groupPos + i]));
                instanceData[baseInstance + i] = oc;
            }

            GroupDrawData gdata{};
            gdata.baseInstance = baseInstance;
            gdata.instanceCount = chunkSize;
            gdata.indexCountPerInstance = idxCount;
            gdata.startIndexLocation = startIdx;
            gdata.baseVertexLocation = baseVert;
            groupDrawList.push_back(gdata);

            cmdsThisRange += 1;
            instanceWritePos += chunkSize;
            groupPos += chunkSize;
        }

        if (cmdsThisRange > 0) {
            bindingRanges.push_back({meshPtr, texHandle, cmdsThisRange});
        }
    }

    UINT numCommands = (UINT)groupDrawList.size();

    // ============================================================
    // Compute Shader로 IndirectDrawCommand 기록
    // ============================================================
    if (numCommands > 0 && currentFrameResource->MappedGroupData && currentFrameResource->GPUArgumentBuffer)
    {
        size_t bytes = numCommands * sizeof(GroupDrawData);
        if (bytes <= currentFrameResource->GroupDataBufferSize)
            memcpy(currentFrameResource->MappedGroupData, groupDrawList.data(), bytes);

        ID3D12RootSignature* buildRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::BuildIndirect);
        ID3D12PipelineState* buildPSO = PipelineStateManager::Get().GetOrCreateComputePSO("build_indirect", buildRS);

        if (buildRS && buildPSO)
        {
            cmdList->SetComputeRootSignature(buildRS);
            cmdList->SetPipelineState(buildPSO);

            cmdList->SetComputeRootShaderResourceView(0, currentFrameResource->GroupDataUploadBuffer->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(1, currentFrameResource->GPUArgumentBuffer->GetGPUVirtualAddress());
            cmdList->SetComputeRoot32BitConstant(2, numCommands, 0);

            UINT tg = (numCommands + 63) / 64;
            cmdList->Dispatch(tg, 1, 1);

            // UAV writes visible
            D3D12_RESOURCE_BARRIER uavVisible = {};
            uavVisible.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavVisible.UAV.pResource = currentFrameResource->GPUArgumentBuffer.Get();
            cmdList->ResourceBarrier(1, &uavVisible);

            // Transition for ExecuteIndirect
            D3D12_RESOURCE_BARRIER toIndirect = {};
            toIndirect.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toIndirect.Transition.pResource = currentFrameResource->GPUArgumentBuffer.Get();
            toIndirect.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            toIndirect.Transition.StateAfter  = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            toIndirect.Transition.Subresource = 0;
            cmdList->ResourceBarrier(1, &toIndirect);
        }
    }

    // ============================================================
    // 중요: Compute PSO에서 Graphics로 복원 (오류 수정)
    // ============================================================
    cmdList->SetGraphicsRootSignature(sceneRS);
    cmdList->SetPipelineState(pso);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Descriptor Heap은 처음에 이미 설정했지만, 안전하게 다시 설정
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    // PassCB와 InstanceBuffer SRV 다시 바인딩
    UpdatePassCB(currentFrameResource, cmdList, viewMatrix, projMatrix);
    if (currentFrameResource->InstanceBuffer)
    {
        cmdList->SetGraphicsRootShaderResourceView(
            4, currentFrameResource->InstanceBuffer->GetGPUVirtualAddress());
    }

    // ============================================================
    // ExecuteIndirect with texture batching only
    // (IA는 글로벌로 한 번 바인딩, BaseVertex/StartIndex로 메시 구분)
    // ============================================================
    // Reuse variables declared earlier in the function
    activeState = DrawBindState{};
    UINT cmdCursor = 0;
    batchStart = 0;
    batchCount = 0;

    for (const auto& br : bindingRanges)
    {
        bool stateChanged =
            (br.texture.ptr != activeState.texture.ptr);

        if (stateChanged)
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

            // Bind texture only (IA is global)
            if (br.texture.ptr != 0)
            {
                cmdList->SetGraphicsRootDescriptorTable(2, br.texture);
            }

            activeState.texture = br.texture;
            batchStart = cmdCursor;
        }

        batchCount += br.numCommands;
        cmdCursor += br.numCommands;
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
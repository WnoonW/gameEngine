#include "RenderSystem.h"
#include "../V1_/V4/MatarialManager.h"
#include "../V1_/V3/MeshManager.h"
#include "../Structs/constantStruct.h"
#include "../Structs/AppStruct.h"
#include <DirectXMath.h>

using namespace DirectX;

void RenderSystem::createCBV(ID3D12Device* device, std::vector<std::unique_ptr<FrameResource>>& frameResources,
    int gNumFrameResources, DescriptorAllocator& descriptorAllocator, Entity entity, Registry& registry)
{
	auto& rend = registry.getComponent<RenderableComponent>(entity);
    for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
    {
        auto objectCB = frameResources[frameIndex]->ObjectCB->Resource();

        auto handle = descriptorAllocator.Allocate();
        mCBVHandles.push_back(handle);
        UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
        cbvDesc.BufferLocation = objectCB->GetGPUVirtualAddress() + (UINT64)rend.objectCBIndex * objCBByteSize;
        cbvDesc.SizeInBytes = objCBByteSize;
        device->CreateConstantBufferView(&cbvDesc, handle.CPU);
    }
}

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
        // ==========================================
        cmdList->SetGraphicsRootSignature(material->mRootSignature.Get());
        cmdList->SetPipelineState(material->mPSO.Get());

        // ==========================================
        // 5. Vertex / Index Buffer 설정 (함수 호출로 수정!)
        // ==========================================
        auto vbv = mesh->VertexBufferView();   // 함수 호출
        auto ibv = mesh->IndexBufferView();    // 함수 호출

        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // ==========================================
		// 6. descriptor heap 설정 (필요시)
        // ==========================================
        ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
        cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
        //cmdList->SetGraphicsRootDescriptorTable(0, mCBVHandles[mCurrFrameIndex].GPU);
        cmdList->SetGraphicsRootDescriptorTable(1, material->mTextureHandle.GPU);

        // ==========================================
        // 7. Draw (indexCount 직접 사용 가능)
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
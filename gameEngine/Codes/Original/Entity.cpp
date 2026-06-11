#include "Entity.h"
#include <iostream>

Entity::Entity()
{
    std::cout << "[Entity] 생성됨 ID: " << mID << std::endl;
}

Entity::~Entity() = default;

void Entity::SetMesh(Mesh* mesh)
{
    mMesh = mesh;
}

void Entity::SetMaterial(Material1* material)
{
    mMaterial = material;
}

void Entity::Draw(ID3D12GraphicsCommandList* cmdList)
{
    if (!mMesh || !mMaterial) return;

    // 1. PSO & RootSignature 설정
    cmdList->SetPipelineState(mMaterial->GetPSO());
    cmdList->SetGraphicsRootSignature(mMaterial->GetRootSignature());

    // 2. Descriptor Heap 설정 (매우 중요!)
    if (mMaterial->GetDescriptorHeap()) {
        ID3D12DescriptorHeap* heaps[] = { mMaterial->GetDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        // SRV 바인딩 (t0)
        cmdList->SetGraphicsRootDescriptorTable(1, mMaterial->GetDescriptorHeap()->GetGPUDescriptorHandleForHeapStart());
    }

    // 3. Vertex Buffer View
    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    vbView.BufferLocation = mMesh->GetVertexBuffer()->GetGPUVirtualAddress();
    vbView.StrideInBytes = sizeof(Vertex);           // ← 32바이트 (Position + TexCoord + Normal)
    vbView.SizeInBytes = mMesh->GetVertexBuffer()->GetDesc().Width;

    // 4. Index Buffer View
    D3D12_INDEX_BUFFER_VIEW ibView = {};
    ibView.BufferLocation = mMesh->GetIndexBuffer()->GetGPUVirtualAddress();
    ibView.Format = DXGI_FORMAT_R32_UINT;
    ibView.SizeInBytes = mMesh->GetIndexBuffer()->GetDesc().Width;

    cmdList->IASetVertexBuffers(0, 1, &vbView);
    cmdList->IASetIndexBuffer(&ibView);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 5. 그리기
    cmdList->DrawIndexedInstanced(mMesh->GetIndexCount(), 1, 0, 0, 0);
}
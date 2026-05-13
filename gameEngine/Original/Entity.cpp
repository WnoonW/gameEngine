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

    cmdList->SetPipelineState(mMaterial->GetPSO());
    cmdList->SetGraphicsRootSignature(mMaterial->GetRootSignature());

    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    vbView.BufferLocation = mMesh->GetVertexBuffer()->GetGPUVirtualAddress();
    vbView.StrideInBytes = sizeof(float) * 5; // Position + TexCoord
    vbView.SizeInBytes = mMesh->GetVertexBuffer()->GetDesc().Width;

    D3D12_INDEX_BUFFER_VIEW ibView = {};
    ibView.BufferLocation = mMesh->GetIndexBuffer()->GetGPUVirtualAddress();
    ibView.Format = DXGI_FORMAT_R32_UINT;
    ibView.SizeInBytes = mMesh->GetIndexBuffer()->GetDesc().Width;

    cmdList->IASetVertexBuffers(0, 1, &vbView);
    cmdList->IASetIndexBuffer(&ibView);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmdList->DrawIndexedInstanced(mMesh->GetIndexCount(), 1, 0, 0, 0);
}
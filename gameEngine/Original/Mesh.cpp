#include "Mesh.h"
#include "../Common/d3dx12.h"
#include <iostream>

Mesh::Mesh() = default;
Mesh::~Mesh() = default;

bool Mesh::CreateMesh(const std::wstring& objPath, DataLoader& loader, ID3D12Device* device)
{
    if (!LoadFromFile(objPath, loader)) {
        OutputDebugString(L"Mesh 생성 실패: 파일 로드 실패\n");
        return false;
	}

	if (!CreateBuffers(device)) {
        OutputDebugString(L"Mesh 생성 실패: 버퍼 생성 실패\n");
        return false;
    }
    return true;
}

bool Mesh::LoadFromFile(const std::wstring& objPath, DataLoader& loader)
{
    bool success = loader.LoadOBJ(objPath, mMeshData, false, false);
    if (!success) {

        std::wstring text = L"Mesh 로드 실패";
        text += L"\n";
        OutputDebugString(text.c_str());

        return false;
    }

    std::wstring msg = L"[Mesh] OBJ 로드 성공 - 서브메시 수: "
        + std::to_wstring(mMeshData.subMeshes.size()) + L"\n";
    OutputDebugString(msg.c_str());

    return true;
}

bool Mesh::CreateBuffers(ID3D12Device* device)
{
    if (mMeshData.vertices.empty() || mMeshData.indices.empty()) {
        OutputDebugString(L"버퍼 생성 실패: 데이터 없음\n");
        return false;
    }

    // Vertex Buffer
    UINT vertexBufferSize = static_cast<UINT>(mMeshData.vertices.size() * sizeof(float));

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                                                 &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&mVertexBuffer));
    if (FAILED(hr)) return false;

    // 데이터 복사 (간단 버전 - 실제로는 Map/Unmap)
    void* mappedData;
    CD3DX12_RANGE readRange(0, 0);
    mVertexBuffer->Map(0, &readRange, &mappedData);
    memcpy(mappedData, mMeshData.vertices.data(), vertexBufferSize);
    mVertexBuffer->Unmap(0, nullptr);

    // Index Buffer
    mIndexCount = static_cast<UINT>(mMeshData.indices.size());
    UINT indexBufferSize = mIndexCount * sizeof(uint32_t);

    CD3DX12_RESOURCE_DESC indexDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                                         &indexDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                         nullptr, IID_PPV_ARGS(&mIndexBuffer));
    if (FAILED(hr)) return false;

    mIndexBuffer->Map(0, &readRange, &mappedData);
    memcpy(mappedData, mMeshData.indices.data(), indexBufferSize);
    mIndexBuffer->Unmap(0, nullptr);

    std::wstring msg = L"[Mesh] 버퍼 생성 완료 - Vertex: " + std::to_wstring(mMeshData.vertices.size())
        + L", Index: " + std::to_wstring(mIndexCount) + L"\n";
    OutputDebugString(msg.c_str());
    return true;
}

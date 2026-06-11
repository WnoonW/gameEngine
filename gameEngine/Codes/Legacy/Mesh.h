#pragma once
#include "DataLoader.h"
#include <d3d12.h>
#include <wrl.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

class Mesh
{
public:
    Mesh();
    ~Mesh();

	bool CreateMesh(const std::wstring& objPath, DataLoader& loader, ID3D12Device* device);

    // DataLoader를 활용한 로드
    bool LoadFromFile(const std::wstring& objPath, DataLoader& loader);

    // DX12 버퍼 생성
    bool CreateBuffers(ID3D12Device* device);

    // Getter
    ID3D12Resource* GetVertexBuffer() const { return mVertexBuffer.Get(); }
    ID3D12Resource* GetIndexBuffer() const { return mIndexBuffer.Get(); }
    UINT GetIndexCount() const { return mIndexCount; }

    const MeshData& GetMeshData() const { return mMeshData; }

private:
    MeshData mMeshData;
    ComPtr<ID3D12Resource> mVertexBuffer;
    ComPtr<ID3D12Resource> mIndexBuffer;
    UINT mIndexCount = 0;
};

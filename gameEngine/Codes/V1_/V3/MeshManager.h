#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <wrl.h>                    
#include "../../Common/d3dUtil.h"
#include "../V2/ResourceLoader.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct Mesh
{
    std::string name;

    // CPU 데이터 (필요 시 보관)
    Model cpuModel;

    // 서브메시별 드로우 인자 (인덱스 개수, 시작 인덱스 위치, 베이스 버텍스 위치)
    std::unordered_map<std::string, SubmeshGeometry> DrawArgs;

    // GPU 리소스 (SubMesh가 여러 개면 vector로 확장 가능)
    ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView()const
    {
        D3D12_VERTEX_BUFFER_VIEW vbv;
        vbv.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vbv.StrideInBytes = vertexStride;
        vbv.SizeInBytes = vertexCount * vertexStride;

        return vbv;
    }

    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_INDEX_BUFFER_VIEW IndexBufferView()const
    {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        ibv.Format = DXGI_FORMAT_R32_UINT;
        ibv.SizeInBytes = indexCount * sizeof(uint32_t);

        return ibv;
    }

    UINT vertexCount = 0;
    UINT indexCount = 0;
    UINT vertexStride = sizeof(Vertex);   // position + normal + texcoord

    // 업로드 힙 (GPU가 다 쓸 때까지 유지)
    ComPtr<ID3D12Resource> vertexUploadHeap;
    ComPtr<ID3D12Resource> indexUploadHeap;
};

class MeshManager
{
public:
    static MeshManager& Get()
    {
        static MeshManager instance;
        return instance;
    }   // 싱글톤 or App에서 주입

    // filepath로 로드 → GPU 버퍼 생성 → 캐싱
    bool CreateMesh(const std::string& name, const std::wstring& filepath,
        ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);

    Mesh* GetMesh(const std::string& name) const;

    void Shutdown();

private:
    std::unordered_map<std::string, std::shared_ptr<Mesh>> mMeshes;
};
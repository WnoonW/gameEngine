#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <wincodec.h>
#include <DirectXMath.h>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ==================== Vertex 구조체 ====================
struct Vertex {
    XMFLOAT3 Position;
    XMFLOAT2 TexCoord;
    XMFLOAT3 Normal;
};

struct SubMesh {
    std::string name;
    std::string materialName;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct MeshData {
    std::vector<SubMesh> subMeshes;
    std::vector<Vertex> vertices;      // 하위 호환용
    std::vector<uint32_t> indices;
};

class DataLoader
{
public:
    DataLoader();
    ~DataLoader();

    bool LoadTexture(ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        const std::wstring& filePath,
        ComPtr<ID3D12Resource>& outTexture,
        ComPtr<ID3D12Resource>& outUploadHeap);

    bool LoadOBJ(const std::wstring& filename, MeshData& outMeshData,
        bool flipZ = true, bool reverseWinding = true);

    bool LoadAsset(ID3D12Device* device, ID3D12CommandQueue* commandQueue,
        const std::wstring& filePath, void* outData);

private:
    bool LoadImageWithWIC(ID3D12Device* device, ID3D12CommandQueue* commandQueue,
        const std::wstring& filePath,
        ComPtr<ID3D12Resource>& texture, ComPtr<ID3D12Resource>& uploadHeap);

    bool LoadDDS(ID3D12Device* device, ID3D12CommandQueue* commandQueue,
        const std::wstring& filePath,
        ComPtr<ID3D12Resource>& texture, ComPtr<ID3D12Resource>& uploadHeap);
};
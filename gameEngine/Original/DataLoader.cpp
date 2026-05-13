#include "DataLoader.h"
#include "../Common/d3dx12.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <wincodec.h>
#include <dxgiformat.h>
#include <DirectXMath.h>
using namespace DirectX;
#pragma comment(lib, "windowscodecs.lib")

struct DDS_PIXELFORMAT {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwABitMask;
};

struct DDS_HEADER {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwHeight;
    DWORD dwWidth;
    DWORD dwPitchOrLinearSize;
    DWORD dwDepth;
    DWORD dwMipMapCount;
    DWORD dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCaps3;
    DWORD dwCaps4;
    DWORD dwReserved2;
};

#define DDS_MAGIC 0x20534444

DataLoader::DataLoader() = default;
DataLoader::~DataLoader() = default;

bool DataLoader::LoadTexture(ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    const std::wstring& filePath,
    ComPtr<ID3D12Resource>& outTexture,
    ComPtr<ID3D12Resource>& outUploadHeap)
{
    if (!device || !commandQueue) return false;

    std::wstring ext = filePath.substr(filePath.find_last_of(L'.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == L".dds") {
        return LoadDDS(device, commandQueue, filePath, outTexture, outUploadHeap);
    }
    else {
        return LoadImageWithWIC(device, commandQueue, filePath, outTexture, outUploadHeap);
    }
}

bool DataLoader::LoadImageWithWIC(ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    const std::wstring& filePath,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& uploadHeap)
{
    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return false;

    UINT width, height;
    frame->GetSize(&width, &height);

    ComPtr<IWICFormatConverter> converter;
    wicFactory->CreateFormatConverter(&converter);
    converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut);

    UINT stride = width * 4;
    UINT imageSize = stride * height;
    std::vector<BYTE> imageData(imageSize);

    hr = converter->CopyPixels(nullptr, stride, imageSize, imageData.data());
    if (FAILED(hr)) return false;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) return false;

    CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(imageSize);
    hr = device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE,
        &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&uploadHeap));
    if (FAILED(hr)) return false;

    std::cout << "[DataLoader] WIC 로드 성공 (PNG/JPG): " << width << "x" << height << std::endl;
    return true;
}

bool DataLoader::LoadDDS(ID3D12Device* device, ID3D12CommandQueue* commandQueue,
    const std::wstring& filePath,
    ComPtr<ID3D12Resource>& texture, ComPtr<ID3D12Resource>& uploadHeap)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    DWORD magic;
    file.read(reinterpret_cast<char*>(&magic), sizeof(DWORD));
    if (magic != DDS_MAGIC) return false;

    DDS_HEADER header;
    file.read(reinterpret_cast<char*>(&header), sizeof(DDS_HEADER));

    UINT width = header.dwWidth;
    UINT height = header.dwHeight;

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (header.ddspf.dwFlags & 0x4) {
        if (header.ddspf.dwFourCC == '1TXD') format = DXGI_FORMAT_BC1_UNORM;
        else if (header.ddspf.dwFourCC == '3TXD' || header.ddspf.dwFourCC == '5TXD') format = DXGI_FORMAT_BC3_UNORM;
    }
    else if (header.ddspf.dwRGBBitCount == 32) {
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    if (format == DXGI_FORMAT_UNKNOWN) {
        std::cerr << "지원하지 않는 DDS 포맷입니다." << std::endl;
        return false;
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = (header.dwMipMapCount > 0) ? header.dwMipMapCount : 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) return false;

    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    size_t dataSize = fileSize - sizeof(DWORD) - sizeof(DDS_HEADER);

    CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize);
    hr = device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE,
        &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&uploadHeap));
    if (FAILED(hr)) return false;

    std::cout << "[DataLoader] DDS 로드 성공: " << width << "x" << height << std::endl;
    return true;
}

bool DataLoader::LoadOBJ(const std::wstring& filePath, MeshData& outMeshData)
{
    std::ifstream file(filePath);
    if (!file.is_open()) return false;

    outMeshData.subMeshes.clear();

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT2> texcoords;
    std::vector<XMFLOAT3> normals;

    SubMesh currentSubMesh;
    currentSubMesh.name = "default";
    std::string currentMaterial = "";

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string prefix;
        iss >> prefix;

        if (prefix == "v") {
            float x, y, z; iss >> x >> y >> z;
            positions.emplace_back(x, y, z);
        }
        else if (prefix == "vt") {
            float u, v; iss >> u >> v;
            texcoords.emplace_back(u, v);
        }
        else if (prefix == "vn") {
            float nx, ny, nz; iss >> nx >> ny >> nz;
            normals.emplace_back(nx, ny, nz);
        }
        else if (prefix == "usemtl") {
            iss >> currentMaterial;
            currentSubMesh.materialName = currentMaterial;
        }
        else if (prefix == "f") {
            std::string token;
            std::vector<uint32_t> faceIndices;

            while (iss >> token) {
                size_t p1 = token.find('/');
                size_t p2 = token.find('/', p1 + 1);

                std::string vStr = token.substr(0, p1);
                std::string vtStr = (p1 != std::string::npos && p2 > p1 + 1) ? token.substr(p1 + 1, p2 - p1 - 1) : "";
                std::string vnStr = (p2 != std::string::npos) ? token.substr(p2 + 1) : "";

                uint32_t vIdx = std::stoul(vStr) - 1;
                uint32_t vtIdx = vtStr.empty() ? 0 : std::stoul(vtStr) - 1;
                uint32_t vnIdx = vnStr.empty() ? 0 : std::stoul(vnStr) - 1;

                // Vertex 구조체로 만들어서 추가
                Vertex vert{};
                vert.Position = positions[vIdx];
                if (vtIdx < texcoords.size()) vert.TexCoord = texcoords[vtIdx];
                if (vnIdx < normals.size())   vert.Normal = normals[vnIdx];

                currentSubMesh.vertices.push_back(vert);
                faceIndices.push_back((uint32_t)currentSubMesh.vertices.size() - 1);
            }

            // 삼각형 인덱스 추가 (삼각형 2개 = 6개 인덱스)
            if (faceIndices.size() >= 3) {
                for (size_t i = 1; i + 1 < faceIndices.size(); ++i) {
                    currentSubMesh.indices.push_back(faceIndices[0]);
                    currentSubMesh.indices.push_back(faceIndices[i]);
                    currentSubMesh.indices.push_back(faceIndices[i + 1]);
                }
            }
        }
    }

    if (!currentSubMesh.vertices.empty()) {
        outMeshData.subMeshes.push_back(currentSubMesh);
    }

    if (!outMeshData.subMeshes.empty()) {
        outMeshData.vertices = outMeshData.subMeshes[0].vertices;
        outMeshData.indices = outMeshData.subMeshes[0].indices;
    }

    std::cout << "[DataLoader] OBJ 로드 완료 - Vertices: " << outMeshData.vertices.size()
        << ", Indices: " << outMeshData.indices.size() << std::endl;
    return true;
}

bool DataLoader::LoadAsset(ID3D12Device* device, ID3D12CommandQueue* commandQueue,
    const std::wstring& filePath, void* outData)
{
    std::wstring ext = filePath.substr(filePath.find_last_of(L'.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == L".obj") {
        return LoadOBJ(filePath, *reinterpret_cast<MeshData*>(outData));
    }
    else if (ext == L".png" || ext == L".jpg" || ext == L".bmp" || ext == L".dds") {
        ComPtr<ID3D12Resource> tex, upload;
        return LoadTexture(device, commandQueue, filePath, tex, upload);
    }
    return false;
}
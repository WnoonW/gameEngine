#include "DataLoader.h"
#include "../Common/d3dx12.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <wincodec.h>
#include <dxgiformat.h>
#include <array>              
#include <unordered_map>
#include <DirectXMath.h>

#pragma comment(lib, "windowscodecs.lib")

// DDS 헤더 구조
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

// ==================== 고성능 OBJ 파서 ====================
bool DataLoader::LoadOBJ(const std::wstring& filename, MeshData& outMeshData,
    bool flipZ, bool reverseWinding)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        OutputDebugString((L"OBJ 파일 열기 실패: " + filename + L"\n").c_str());
        return false;
    }

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT2> texcoords;
    std::vector<XMFLOAT3> normals;

    // 중복 제거용 맵 (pos, tex, nor 조합으로 유니크한 vertex 생성)
    struct VertexKey {
        int pos, tex, nor;
        bool operator==(const VertexKey& o) const {
            return pos == o.pos && tex == o.tex && nor == o.nor;
        }
    };

    struct KeyHash {
        size_t operator()(const VertexKey& k) const {
            return ((size_t)k.pos * 73856093ULL) ^
                ((size_t)k.tex * 19349663ULL) ^
                ((size_t)k.nor * 83492791ULL);
        }
    };

    std::unordered_map<VertexKey, uint32_t, KeyHash> vertexMap;

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string prefix;
        iss >> prefix;

        if (prefix == "v") {
            float x, y, z;
            iss >> x >> y >> z;
            if (flipZ) z = -z;
            positions.emplace_back(x, y, z);
        }
        else if (prefix == "vt") {
            float u, v;
            iss >> u >> v;
            texcoords.emplace_back(u, v);
        }
        else if (prefix == "vn") {
            float x, y, z;
            iss >> x >> y >> z;
            normals.emplace_back(x, y, z);
        }
        else if (prefix == "f") {
            std::vector<std::array<int, 3>> faceCorners; // [pos, tex, nor]

            std::string token;
            while (iss >> token) {
                int p = -1, t = -1, n = -1;
                std::istringstream tokenStream(token);
                std::string part;

                if (std::getline(tokenStream, part, '/')) p = std::stoi(part) - 1;
                if (std::getline(tokenStream, part, '/')) if (!part.empty()) t = std::stoi(part) - 1;
                if (std::getline(tokenStream, part, '/')) if (!part.empty()) n = std::stoi(part) - 1;

                faceCorners.push_back({ p, t, n });
            }

            // Triangle 처리 + Winding
            if (faceCorners.size() == 3) {
                std::vector<uint32_t> triIndices;

                for (auto& corner : faceCorners) {
                    VertexKey key{ corner[0], corner[1], corner[2] };

                    if (vertexMap.find(key) == vertexMap.end()) {
                        uint32_t newIdx = static_cast<uint32_t>(outMeshData.vertices.size());
                        vertexMap[key] = newIdx;

                        Vertex vert{};
                        if (corner[0] >= 0) vert.Position = positions[corner[0]];
                        if (corner[1] >= 0 && corner[1] < static_cast<int>(texcoords.size()))
                            vert.TexCoord = texcoords[corner[1]];
                        if (corner[2] >= 0 && corner[2] < static_cast<int>(normals.size()))
                            vert.Normal = normals[corner[2]];

                        outMeshData.vertices.push_back(vert);
                    }
                    triIndices.push_back(vertexMap[key]);
                }

                if (reverseWinding) {
                    outMeshData.indices.push_back(triIndices[0]);
                    outMeshData.indices.push_back(triIndices[2]);
                    outMeshData.indices.push_back(triIndices[1]);
                }
                else {
                    outMeshData.indices.push_back(triIndices[0]);
                    outMeshData.indices.push_back(triIndices[1]);
                    outMeshData.indices.push_back(triIndices[2]);
                }
            }
        }
    }

    std::wstring msg = L"[DataLoader] OBJ Loaded (Normal+Tex+Dedup): " + filename +
        L" | Vertices: " + std::to_wstring(outMeshData.vertices.size()) +
        L" | Indices: " + std::to_wstring(outMeshData.indices.size());
    OutputDebugString(msg.c_str());

    return true;
}

bool DataLoader::LoadAsset(ID3D12Device* device, ID3D12CommandQueue* commandQueue,
    const std::wstring& filePath, void* outData)
{
    std::wstring ext = filePath.substr(filePath.find_last_of(L'.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == L".obj") {
        return LoadOBJ(filePath, *static_cast<MeshData*>(outData));
    }
    else if (ext == L".png" || ext == L".jpg" || ext == L".bmp" || ext == L".dds") {
        ComPtr<ID3D12Resource> tex, upload;
        return LoadTexture(device, commandQueue, filePath, tex, upload);
    }
    return false;
}
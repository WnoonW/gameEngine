#include "ResourceLoader.h"
#include <algorithm>
#include "../Common/DDSTextureLoader.h"
#include "WICTextureLoader.h"
#include <ResourceUploadBatch.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <string>
#include <stdexcept>

bool ResourceLoad(const std::wstring& filepath)
{
    return false;
}

bool TextureLoad(const std::wstring& filepath, Microsoft::WRL::ComPtr<ID3D12Resource>& texture, Microsoft::WRL::ComPtr<ID3D12Resource>& uploadHeap, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* commandQueue)
{
    if (filepath.empty())
        return false;

    size_t dotPos = filepath.find_last_of(L'.');
    if (dotPos == std::wstring::npos)
        return false;

    std::wstring ext = filepath.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == L".dds")
    {
        HRESULT hr = DirectX::CreateDDSTextureFromFile12(
            device, cmdList, filepath.c_str(),
            texture, uploadHeap);

        if (FAILED(hr))
        {
            // ★★★ 여기 추가 ★★★
            wchar_t buf[256];
            swprintf_s(buf, L"[TextureLoad] CreateDDSTextureFromFile12 failed. HRESULT=0x%08X, Path=%s\n",
                hr, filepath.c_str());
            OutputDebugStringW(buf);

            // GetLastError()도 같이 확인
            DWORD err = GetLastError();
            if (err != 0)
            {
                swprintf_s(buf, L"  -> GetLastError() = %d (0x%X)\n", err, err);
                OutputDebugStringW(buf);
            }
            return false;
        }
        return true;
    }
    else if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp")
    {
        // === WIC 로드 (PNG, JPG, BMP) ===
        DirectX::ResourceUploadBatch resourceUpload(device);
        resourceUpload.Begin();

        HRESULT hr = DirectX::CreateWICTextureFromFile(
            device,
            resourceUpload,
            filepath.c_str(),
            texture.GetAddressOf(),
            true   // Mipmap 자동 생성
        );

        if (FAILED(hr))
        {
            OutputDebugStringW(L"[ResourceLoader] Failed to load WIC texture.\n");
            return false;
        }

        // 업로드 실행 (cmdList를 사용)
        auto uploadResourcesFinished = resourceUpload.End(commandQueue);
        // 참고: 실제로는 CommandQueue를 직접 넘기는 게 더 정확합니다.

        // 동기 대기 (간단 구현용)
        uploadResourcesFinished.wait();

        return true;
    }
    else
    {
        OutputDebugStringW((L"Unsupported texture format: " + std::wstring(ext.begin(), ext.end())).c_str());
        return false;
    }
}


static FaceVertex parseFaceVertex(const std::string& token) {
    FaceVertex fv;
    std::stringstream ss(token);
    std::string part;
    int idx = 0;

    while (std::getline(ss, part, '/')) {
        if (!part.empty()) {
            int value = std::stoi(part);
            if (idx == 0) fv.pos = value;
            else if (idx == 1) fv.tex = value;
            else if (idx == 2) fv.nor = value;
        }
        ++idx;
    }
    return fv;
}

bool MeshLoad(const std::wstring& filepath, Model& outModel)
{
    // wstring → string 변환 (간단 버전)
    std::string path(filepath.begin(), filepath.end());
    std::ifstream file(path);
    if (!file.is_open())
    {
        // TODO: OutputDebugStringA("[MeshLoad] 파일 열기 실패\n");
        return false;
    }

    std::vector<DirectX::XMFLOAT3> temp_positions;
    std::vector<DirectX::XMFLOAT3> temp_normals;
    std::vector<DirectX::XMFLOAT2> temp_texcoords;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty()) continue;

        if (line[0] == 'v')
        {
            if (line[1] == ' ')
            {
                DirectX::XMFLOAT3 pos{};
                if (sscanf_s(line.c_str(), "v %f %f %f", &pos.x, &pos.y, &pos.z) == 3)
                    temp_positions.push_back(pos);
            }
            else if (line[1] == 'n')
            {
                DirectX::XMFLOAT3 n{};
                if (sscanf_s(line.c_str(), "vn %f %f %f", &n.x, &n.y, &n.z) == 3)
                    temp_normals.push_back(n);
            }
            else if (line[1] == 't')
            {
                DirectX::XMFLOAT2 uv{};
                if (sscanf_s(line.c_str(), "vt %f %f", &uv.x, &uv.y) == 2)
                    temp_texcoords.push_back(uv);
            }
        }
        else if (line[0] == 'f')
        {
            std::istringstream iss(line);
            std::string token;
            iss >> token; // 'f' skip

            std::vector<Vertex> faceVerts;

            while (iss >> token)
            {
                int v = 0, vt = 0, vn = 0;
                size_t p1 = token.find('/');
                if (p1 != std::string::npos)
                {
                    v = std::atoi(token.substr(0, p1).c_str());
                    size_t p2 = token.find('/', p1 + 1);
                    if (p2 != std::string::npos)
                    {
                        std::string vtStr = token.substr(p1 + 1, p2 - p1 - 1);
                        if (!vtStr.empty()) vt = std::atoi(vtStr.c_str());
                        std::string vnStr = token.substr(p2 + 1);
                        if (!vnStr.empty()) vn = std::atoi(vnStr.c_str());
                    }
                    else
                    {
                        std::string vtStr = token.substr(p1 + 1);
                        if (!vtStr.empty()) vt = std::atoi(vtStr.c_str());
                    }
                }
                else
                {
                    v = std::atoi(token.c_str());
                }

                if (v > 0)  v--;
                if (vt > 0) vt--;
                if (vn > 0) vn--;

                Vertex vert{};
                if (v >= 0 && v < (int)temp_positions.size()) vert.position = temp_positions[v];
                if (vn >= 0 && vn < (int)temp_normals.size())   vert.normal = temp_normals[vn];
                if (vt >= 0 && vt < (int)temp_texcoords.size()) vert.texcoord = temp_texcoords[vt];

                faceVerts.push_back(vert);
            }

            // Fan triangulation (quad 이상도 안전하게 처리)
            if (faceVerts.size() >= 3)
            {
                size_t base = outModel.vertices.size();
                for (auto& v : faceVerts)
                    outModel.vertices.push_back(v);

                for (size_t i = 1; i + 1 < faceVerts.size(); ++i)
                {
                    outModel.indices.push_back(static_cast<unsigned int>(base + 0));
                    outModel.indices.push_back(static_cast<unsigned int>(base + i));
                    outModel.indices.push_back(static_cast<unsigned int>(base + i + 1));
                }
            }
        }
    }

    file.close();
    return true;
}
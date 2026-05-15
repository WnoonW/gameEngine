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

bool TextureLoad(const std::wstring & filepath, Microsoft::WRL::ComPtr<ID3D12Resource>& texture, Microsoft::WRL::ComPtr<ID3D12Resource>& uploadHeap, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* commandQueue)
{
	if (filepath.empty())
		return false;

	size_t dotPos = filepath.find_last_of(L'.');
	if(dotPos == std::wstring::npos)
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

bool MeshLoad(const std::wstring & filepath, Model outModel)
{
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return {};
    }

    std::vector<DirectX::XMFLOAT3> temp_positions;
    std::vector<DirectX::XMFLOAT3> temp_normals;
    std::vector<DirectX::XMFLOAT2> temp_texcoords;

    // (pos, tex, nor) 조합으로 중복 제거
    std::unordered_map<std::string, unsigned int> index_map;

    std::string line;
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string prefix;
        ss >> prefix;

        if (prefix == "v") {
            DirectX::XMFLOAT3 pos{};
            ss >> pos.x >> pos.y >> pos.z;
            temp_positions.push_back(pos);
        }
        else if (prefix == "vn") {
            DirectX::XMFLOAT3 nor{};
            ss >> nor.x >> nor.y >> nor.z;
            temp_normals.push_back(nor);
        }
        else if (prefix == "vt") {
            DirectX::XMFLOAT2 tex{};
            ss >> tex.x >> tex.y;
            temp_texcoords.push_back(tex);
        }
        else if (prefix == "f") {
            std::vector<unsigned int> face_indices;
            std::string token;

            while (ss >> token) {
                FaceVertex fv = parseFaceVertex(token);

                // OBJ 인덱스는 1-based, 음수 지원
                auto get_index = [](int idx, size_t size) -> int {
                    return (idx < 0) ? static_cast<int>(size) + idx : idx - 1;
                    };

                int pos_idx = get_index(fv.pos, temp_positions.size());
                int nor_idx = (fv.nor != -1) ? get_index(fv.nor, temp_normals.size()) : -1;
                int tex_idx = (fv.tex != -1) ? get_index(fv.tex, temp_texcoords.size()) : -1;

                if (pos_idx < 0 || pos_idx >= (int)temp_positions.size()) continue;

                // 키 생성 (pos_tex_nor)
                std::string key = std::to_string(pos_idx) + "_" +
                    std::to_string(tex_idx) + "_" +
                    std::to_string(nor_idx);

                auto it = index_map.find(key);
                if (it != index_map.end()) {
                    face_indices.push_back(it->second);
                }
                else {
                    Vertex v{};
                    v.position = temp_positions[pos_idx];

                    if (nor_idx >= 0 && nor_idx < (int)temp_normals.size())
                        v.normal = temp_normals[nor_idx];
                    else
                        v.normal = { 0, 1, 0 }; // 기본값

                    if (tex_idx >= 0 && tex_idx < (int)temp_texcoords.size())
                        v.texcoord = temp_texcoords[tex_idx];

                    unsigned int new_index = static_cast<unsigned int>(outModel.vertices.size());
                    outModel.vertices.push_back(v);
                    index_map[key] = new_index;
                    face_indices.push_back(new_index);
                }
            }

            // 삼각형 팬 방식으로 triangulation
            for (size_t i = 0; i + 2 < face_indices.size(); ++i) {
                outModel.indices.push_back(face_indices[0]);
                outModel.indices.push_back(face_indices[i + 1]);
                outModel.indices.push_back(face_indices[i + 2]);
            }
        }
    }

    file.close();
	return true;
}



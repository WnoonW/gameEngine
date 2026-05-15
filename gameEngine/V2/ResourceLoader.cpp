#include "ResourceLoader.h"
#include <algorithm>
#include "../Common/DDSTextureLoader.h"
#include "WICTextureLoader.h"
#include <ResourceUploadBatch.h>

bool ResourceLoad(const std::wstring& filepath)
{
	return false;
}

bool TextureLoad(const std::wstring & filepath, Microsoft::WRL::ComPtr<ID3D12Resource> texture, Microsoft::WRL::ComPtr<ID3D12Resource> UploadHeap, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* commandQueue)
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
		// === DDS 로드 (DirectXTK12) ===
		HRESULT hr = DirectX::CreateDDSTextureFromFile12(
			device,                    
			cmdList,              
			filepath.c_str(),
			texture,                    
			UploadHeap
		);

		if (FAILED(hr))
		{
			OutputDebugStringW(L"Failed to load DDS texture.\n");
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

bool MeshLoad(const std::wstring & filepath)
{
	return false;
}

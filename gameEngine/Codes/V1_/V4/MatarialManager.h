#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <wrl.h>                    
#include "../../Common/d3dUtil.h"
#include "../V2/ResourceLoader.h"
using namespace DirectX;
using Microsoft::WRL::ComPtr;
//힙 생성, 텍스쳐 로드, SRV 생성, 셰이더 로드,

struct Matarial
{
	std::string name;
	ComPtr<ID3D12DescriptorHeap> mSrvHeap = nullptr;
	ComPtr<ID3D12Resource> mTexture = nullptr;
	ComPtr<ID3D12Resource> mTextureUploadHeap = nullptr;

	ComPtr<ID3DBlob> mvsByteCode = nullptr;
	ComPtr<ID3DBlob> mpsByteCode = nullptr;
	std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	UINT mSrvDescriptorSize = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE	mSrvCpuHandle = {};
	D3D12_GPU_DESCRIPTOR_HANDLE	mSrvGpuHandle = {};
};

class MatarialManager
{
	public:
	static MatarialManager& Get()
	{
		static MatarialManager instance;
		return instance;
	}

	bool CreateMatarial(const std::string& name, const std::wstring& filePath, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* commandQueue);
	std::shared_ptr<Matarial> GetMatarial(const std::string& name);
	void Shutdown();

private:
	std::unordered_map<std::string, std::shared_ptr<Matarial>> mMatarials;
};
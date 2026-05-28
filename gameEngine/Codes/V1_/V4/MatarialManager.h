#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <wrl.h>                    
#include "../../Common/d3dUtil.h"
#include "../V2/ResourceLoader.h"
#include "DescriptorAllocator.h"
using namespace DirectX;
using Microsoft::WRL::ComPtr;
//힙 생성, 텍스쳐 로드, SRV 생성, 셰이더 로드,

struct Matarial
{
    std::string name;

    // Texture
    ComPtr<ID3D12Resource> mTexture = nullptr;
    ComPtr<ID3D12Resource> mTextureUploadHeap = nullptr;
    DescriptorAllocator::DescriptorHandle mTextureHandle;

    // Shader
    ComPtr<ID3DBlob> mvsByteCode = nullptr;
    ComPtr<ID3DBlob> mpsByteCode = nullptr;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    //
    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12PipelineState> mPSO;
};

class MatarialManager
{
	public:
	static MatarialManager& Get()
	{
		static MatarialManager instance;
		return instance;
	}

	bool CreateMatarial(const std::string& name, const std::wstring& filePath, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* commandQueue, DescriptorAllocator& descriptorAllocator);
	std::shared_ptr<Matarial> GetMatarial(const std::string& name);
	void Shutdown();

private:
	std::unordered_map<std::string, std::shared_ptr<Matarial>> mMatarials;
};
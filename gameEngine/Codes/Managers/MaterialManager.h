#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <wrl.h>                    
#include "d3dUtil.h"
#include "ResourceLoader.h"
#include "DescriptorAllocator.h"
using namespace DirectX;
using Microsoft::WRL::ComPtr;
//힙 생성, 텍스쳐 로드, SRV 생성, 셰이더 로드,

struct Material
{
    std::string name;

    // Texture
    ComPtr<ID3D12Resource> mTexture = nullptr;
    ComPtr<ID3D12Resource> mTextureUploadHeap = nullptr;
    DescriptorAllocator::DescriptorHandle mTextureHandle;
};

class MaterialManager
{
	public:
	static MaterialManager& Get()
	{
		static MaterialManager instance;
		return instance;
	}

	bool CreateMaterial(const std::string& name, const std::wstring& filePath, 
                        ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, 
                        ID3D12CommandQueue* commandQueue, DescriptorAllocator& descriptorAllocator);

	std::shared_ptr<Material> GetMaterial(const std::string& name);
    std::shared_ptr<Material> GetDefaultMaterial();
	void Shutdown();

private:
	std::unordered_map<std::string, std::shared_ptr<Material>> mMaterials;
};
#include "MaterialManager.h"

bool MaterialManager::CreateMaterial(
    const std::string& name,
    const std::wstring& filePath,
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12CommandQueue* commandQueue,
    DescriptorAllocator& descriptorAllocator)
{
    Material mMaterial;
    mMaterial.name = name;

    // 1. Texture 로드
    if (!TextureLoad(filePath, mMaterial.mTexture, mMaterial.mTextureUploadHeap, device, cmdList, commandQueue))
        return false;

    // 2. SRV 생성
    mMaterial.mTextureHandle = descriptorAllocator.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = mMaterial.mTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = mMaterial.mTexture->GetDesc().MipLevels;
    device->CreateShaderResourceView(mMaterial.mTexture.Get(), &srvDesc, mMaterial.mTextureHandle.CPU);

    // 3. 저장
    mMaterials.emplace(name, std::make_shared<Material>(std::move(mMaterial)));
    return true;
}

std::shared_ptr<Material> MaterialManager::GetMaterial(const std::string& name)
{
	auto it = mMaterials.find(name);
	if (it != mMaterials.end())
	{
		return it->second;
	}
	return nullptr;
}

std::shared_ptr<Material> MaterialManager::GetDefaultMaterial()
{
    static std::string defaultName = "Default";
    auto mat = GetMaterial(defaultName);
    return mat;
}

void MaterialManager::Shutdown()
{
	for (auto& pair : mMaterials)
	{
		if (pair.second)
		{
			auto& mat = pair.second;

			// ComPtr 명시적 해제 (순서 중요)
			mat->mTextureUploadHeap.Reset();
			mat->mTexture.Reset();
		}
	}

	mMaterials.clear();
}

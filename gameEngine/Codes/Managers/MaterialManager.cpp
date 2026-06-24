#include "MaterialManager.h"

void MaterialManager::InitializeTextureTable(DescriptorAllocator& descriptorAllocator, UINT maxCount)
{
    mMaxTextureCount = maxCount;
    mCurrentTextureCount = 0;

    // 큰 연속 Descriptor 범위 할당
    mTextureTableBase = descriptorAllocator.Allocate(maxCount);
}

bool MaterialManager::CreateMaterial(const std::string& name,
    const std::wstring& filePath,
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12CommandQueue* commandQueue,
    DescriptorAllocator& descriptorAllocator)
{
    Material mat;
    mat.name = name;

    // 1. Texture 로드
    if (!TextureLoad(filePath, mat.mTexture, mat.mTextureUploadHeap, device, cmdList, commandQueue))
        return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = mat.mTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = mat.mTexture->GetDesc().MipLevels;

    // 텍스처 테이블 슬롯에 SRV를 직접 생성 (CopyDescriptorsSimple 사용 안 함)
    if (mCurrentTextureCount < mMaxTextureCount)
    {
        UINT destIndex = mCurrentTextureCount;
        const UINT descriptorSize = descriptorAllocator.GetDescriptorSize();

        CD3DX12_CPU_DESCRIPTOR_HANDLE destCPU(
            mTextureTableBase.CPU, destIndex, descriptorSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE destGPU(
            mTextureTableBase.GPU, destIndex, descriptorSize);

        device->CreateShaderResourceView(mat.mTexture.Get(), &srvDesc, destCPU);

        mat.mTextureHandle.CPU = destCPU;
        mat.mTextureHandle.GPU = destGPU;
        mat.mTextureHandle.Index = mTextureTableBase.Index + destIndex;
        mat.materialIndex = static_cast<int>(destIndex);
        mCurrentTextureCount++;
    }
    else
    {
        mat.materialIndex = 0;
    }

    mMaterials.emplace(name, std::make_shared<Material>(std::move(mat)));
    return true;
}

void MaterialManager::BindAllTextures(ID3D12GraphicsCommandList* cmdList, UINT rootParameterIndex)
{
    if (mCurrentTextureCount == 0)
        return;

    // 전체 텍스처 범위를 한 번에 바인딩
    cmdList->SetGraphicsRootDescriptorTable(rootParameterIndex, mTextureTableBase.GPU);
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

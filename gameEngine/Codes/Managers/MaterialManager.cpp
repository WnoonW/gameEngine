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

void MaterialManager::SetEntityMainMaterial(ECS::Entity entity, const std::string& materialName)
{
    if (entity == ECS::INVALID_ENTITY)
        return;

    if (!materialName.empty() && !GetMaterial(materialName))
        return;

    mEntityMaterials[entity].mainMaterialName = materialName;
}

void MaterialManager::SetEntitySubMaterial(ECS::Entity entity,
    const std::string& submeshKey,
    const std::string& materialName)
{
    if (entity == ECS::INVALID_ENTITY)
        return;

    if (materialName.empty())
    {
        auto it = mEntityMaterials.find(entity);
        if (it != mEntityMaterials.end())
            it->second.subMaterialNames.erase(submeshKey);
        return;
    }

    if (!GetMaterial(materialName))
        return;

    mEntityMaterials[entity].subMaterialNames[submeshKey] = materialName;
}

void MaterialManager::ClearEntityMaterialData(ECS::Entity entity)
{
    mEntityMaterials.erase(entity);
}

const EntityMaterialData* MaterialManager::GetEntityMaterialData(ECS::Entity entity) const
{
    auto it = mEntityMaterials.find(entity);
    return (it != mEntityMaterials.end()) ? &it->second : nullptr;
}

Material* MaterialManager::ResolveForDraw(ECS::Entity entity,
    const std::string& submeshKey,
    Material* initMaterial)
{
    if (const EntityMaterialData* entityMat = GetEntityMaterialData(entity))
    {
        auto subIt = entityMat->subMaterialNames.find(submeshKey);
        if (subIt != entityMat->subMaterialNames.end() && !subIt->second.empty())
        {
            if (auto mat = GetMaterial(subIt->second))
                return mat.get();
        }

        if (!entityMat->mainMaterialName.empty())
        {
            if (auto mat = GetMaterial(entityMat->mainMaterialName))
                return mat.get();
        }
    }

    if (initMaterial)
        return initMaterial;

    return GetDefaultMaterial().get();
}

std::vector<std::string> MaterialManager::GetLoadedMaterialNames() const
{
    std::vector<std::string> names;
    names.reserve(mMaterials.size());
    for (const auto& p : mMaterials) {
        names.push_back(p.first);
    }
    return names;
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
    mEntityMaterials.clear();
}

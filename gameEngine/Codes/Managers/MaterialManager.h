#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>
#include <wrl.h>                    
#include "d3dUtil.h"
#include "ResourceLoader.h"
#include "DescriptorAllocator.h"
#include "Entity.h"
using namespace DirectX;
using Microsoft::WRL::ComPtr;
//힙 생성, 텍스쳐 로드, SRV 생성, 셰이더 로드,

struct EntityMaterialData
{
    std::string mainMaterialName;
    std::unordered_map<std::string, std::string> subMaterialNames;
};

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
    std::vector<std::string> GetLoadedMaterialNames() const;

    void SetEntityMainMaterial(ECS::Entity entity, const std::string& materialName);
    void SetEntitySubMaterial(ECS::Entity entity, const std::string& submeshKey, const std::string& materialName);
    void ClearEntityMaterialData(ECS::Entity entity);
    const EntityMaterialData* GetEntityMaterialData(ECS::Entity entity) const;

    // Priority: Sub > Main > Init
    Material* ResolveForDraw(ECS::Entity entity,
        const std::string& submeshKey,
        Material* initMaterial);

	void Shutdown();

private:
	std::unordered_map<std::string, std::shared_ptr<Material>> mMaterials;
    std::unordered_map<ECS::Entity, EntityMaterialData> mEntityMaterials;
};
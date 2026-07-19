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

    // 텍스처 리소스 + SRV 슬롯이 모두 있어야 드로우에 쓸 수 있음
    bool HasValidTexture() const
    {
        return mTexture != nullptr && mTextureHandle.Index != UINT_MAX;
    }
};

class MaterialManager
{
public:
    static MaterialManager& Get()
    {
        static MaterialManager instance;
        return instance;
    }

    // 실패 전용 마젠타 1x1 텍스처 머티리얼. 다른 CreateMaterial 전에 한 번 호출.
    bool EnsureMissingTextureMaterial(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        DescriptorAllocator& descriptorAllocator);

    bool CreateMaterial(const std::string& name, const std::wstring& filePath,
                        ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                        ID3D12CommandQueue* commandQueue, DescriptorAllocator& descriptorAllocator);

    std::shared_ptr<Material> GetMaterial(const std::string& name);
    std::shared_ptr<Material> GetDefaultMaterial();
    // 바인딩/해석 실패 시 바인딩하는 디버그 머티리얼 (마젠타 솔리드)
    Material* GetMissingTextureMaterial();
    std::vector<std::string> GetLoadedMaterialNames() const;

    void SetEntityMainMaterial(ECS::Entity entity, const std::string& materialName);
    void SetEntitySubMaterial(ECS::Entity entity, const std::string& submeshKey, const std::string& materialName);
    void ClearEntityMaterialData(ECS::Entity entity);
    const EntityMaterialData* GetEntityMaterialData(ECS::Entity entity) const;

    // Priority: Sub > Main > Init > Default; 전부 실패 시 Missing(마젠타)
    Material* ResolveForDraw(ECS::Entity entity,
        const std::string& submeshKey,
        Material* initMaterial);

    void Shutdown();

private:
    static bool IsUsableMaterial(Material* mat);

    std::unordered_map<std::string, std::shared_ptr<Material>> mMaterials;
    std::unordered_map<ECS::Entity, EntityMaterialData> mEntityMaterials;
    std::string mMissingMaterialName = "__MissingTexture_Debug__";
};

#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <wrl.h>
#include "d3dUtil.h"
#include "ResourceLoader.h"
#include "DescriptorAllocator.h"

using Microsoft::WRL::ComPtr;

struct Material
{
    std::string name;
    ComPtr<ID3D12Resource> mTexture = nullptr;
    ComPtr<ID3D12Resource> mTextureUploadHeap = nullptr;
    DescriptorAllocator::DescriptorHandle mTextureHandle;   // 개별 SRV

    int materialIndex = -1;   // ExecuteIndirect에서 사용할 인덱스
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

    std::vector<std::string> GetAllMaterialNames() const;

    // === ExecuteIndirect용 함수 ===
    void InitializeTextureTable(DescriptorAllocator& descriptorAllocator, UINT maxCount = 1024);
    void BindAllTextures(ID3D12GraphicsCommandList* cmdList, UINT rootParameterIndex);

    int GetMaterialCount() const { return static_cast<int>(mMaterials.size()); }

    void Shutdown();

private:
    MaterialManager() = default;
    std::unordered_map<std::string, std::shared_ptr<Material>> mMaterials;
    int mNextMaterialIndex = 0;

    // 큰 텍스처 Descriptor Table
    DescriptorAllocator::DescriptorHandle mTextureTableBase;   // 시작 핸들
    UINT mMaxTextureCount = 0;
    UINT mCurrentTextureCount = 0;
};
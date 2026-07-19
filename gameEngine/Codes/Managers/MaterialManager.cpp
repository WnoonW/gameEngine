#include "MaterialManager.h"
#include "d3dx12.h"

namespace
{
    bool IsUsableMaterialShared(const std::shared_ptr<Material>& mat)
    {
        return mat && mat->HasValidTexture();
    }
}

bool MaterialManager::IsUsableMaterial(Material* mat)
{
    return mat && mat->HasValidTexture();
}

bool MaterialManager::EnsureMissingTextureMaterial(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    DescriptorAllocator& descriptorAllocator)
{
    if (!device || !cmdList)
        return false;

    if (auto existing = GetMaterial(mMissingMaterialName))
    {
        if (existing->HasValidTexture())
            return true;
    }

    Material mat;
    mat.name = mMissingMaterialName;

    // 1x1 R8G8B8A8 솔리드 마젠타 — "바인딩 실패" 전용 디버그 텍스처
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&mat.mTexture)));

    const UINT64 uploadSize = GetRequiredIntermediateSize(mat.mTexture.Get(), 0, 1);
    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(uploadSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mat.mTextureUploadHeap)));

    // R, G, B, A
    const UINT8 magentaPixel[4] = { 255, 0, 255, 255 };
    D3D12_SUBRESOURCE_DATA subData{};
    subData.pData = magentaPixel;
    subData.RowPitch = 4;
    subData.SlicePitch = 4;

    UpdateSubresources(
        cmdList,
        mat.mTexture.Get(),
        mat.mTextureUploadHeap.Get(),
        0, 0, 1,
        &subData);

    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mat.mTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    mat.mTextureHandle = descriptorAllocator.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mat.mTexture.Get(), &srvDesc, mat.mTextureHandle.CPU);

    mMaterials[mMissingMaterialName] = std::make_shared<Material>(std::move(mat));
    return true;
}

bool MaterialManager::CreateMaterial(
    const std::string& name,
    const std::wstring& filePath,
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12CommandQueue* commandQueue,
    DescriptorAllocator& descriptorAllocator)
{
    if (name == mMissingMaterialName)
        return false;

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

Material* MaterialManager::GetMissingTextureMaterial()
{
    auto mat = GetMaterial(mMissingMaterialName);
    return (mat && mat->HasValidTexture()) ? mat.get() : nullptr;
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
    // Sub / Main 오버라이드
    if (const EntityMaterialData* entityMat = GetEntityMaterialData(entity))
    {
        auto subIt = entityMat->subMaterialNames.find(submeshKey);
        if (subIt != entityMat->subMaterialNames.end() && !subIt->second.empty())
        {
            if (auto mat = GetMaterial(subIt->second))
            {
                if (IsUsableMaterialShared(mat))
                    return mat.get();
            }
        }

        if (!entityMat->mainMaterialName.empty())
        {
            if (auto mat = GetMaterial(entityMat->mainMaterialName))
            {
                if (IsUsableMaterialShared(mat))
                    return mat.get();
            }
        }
    }

    // OBJ usemtl 등 Init 레이어
    if (IsUsableMaterial(initMaterial))
        return initMaterial;

    // 기본 머티리얼
    if (auto def = GetDefaultMaterial())
    {
        if (IsUsableMaterialShared(def))
            return def.get();
    }

    // 전부 실패 → 마젠타 디버그 텍스처 (정상 검정 알베도와 구분)
    return GetMissingTextureMaterial();
}

std::vector<std::string> MaterialManager::GetLoadedMaterialNames() const
{
    std::vector<std::string> names;
    names.reserve(mMaterials.size());
    for (const auto& p : mMaterials)
    {
        // 스폰/선택 UI에 디버그 머티리얼은 노출하지 않음
        if (p.first == mMissingMaterialName)
            continue;
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

#include "ResourceManager.h"

ResourceManager& ResourceManager::Get()
{
    static ResourceManager instance;
    return instance;
}

void ResourceManager::Initialize()
{
    // 필요시 초기화 로직 추가
}

void ResourceManager::Shutdown()
{
    // 필요시 정리 로직 추가
}

Mesh* ResourceManager::GetMesh(const std::string& name)
{
    return MeshManager::Get().GetMesh(name);
}

std::shared_ptr<Material> ResourceManager::GetMaterial(const std::string& name)
{
    return MaterialManager::Get().GetMaterial(name);
}
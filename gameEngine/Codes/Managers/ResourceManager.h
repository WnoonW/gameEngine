#pragma once
#include <string>
#include "MeshManager.h"
#include "MaterialManager.h"

class ResourceManager
{
public:
    static ResourceManager& Get();

    void Initialize();   // 필요하면 사용
    void Shutdown();

    Mesh* GetMesh(const std::string& name);
    std::shared_ptr<Material> GetMaterial(const std::string& name);

private:
    ResourceManager() = default;
};
#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>

// OneMore scene file (.scene) — human-readable text format.
// Only serializes renderable entities (not the editor camera).

struct SceneEntityData
{
    std::string meshName;
    std::string mainMaterial;
    DirectX::XMFLOAT3 position{ 0, 0, 0 };
    DirectX::XMFLOAT3 rotation{ 0, 0, 0 };
    DirectX::XMFLOAT3 scale{ 1, 1, 1 };
    bool visible = true;

    bool hasGravity = false;
    bool gravityEnabled = true;
    float gravityStrength = 9.81f;
    DirectX::XMFLOAT3 gravityVelocity{ 0, 0, 0 };
    DirectX::XMFLOAT3 gravityAngularVelocity{ 0, 0, 0 };

    bool hasCollision = false;
    bool collisionEnabled = true;
    bool collisionStatic = false;
    float collisionRestitution = 0.0f;

    // submeshKey -> materialName
    std::unordered_map<std::string, std::string> subMaterials;
};

struct SceneFileData
{
    static constexpr int kCurrentVersion = 1;
    int version = kCurrentVersion;
    std::string name;
    std::vector<SceneEntityData> entities;
};

class SceneSerializer
{
public:
    // Write SceneFileData to path. Returns false on I/O error.
    static bool SaveToFile(const std::string& path, const SceneFileData& scene);

    // Read SceneFileData from path. Returns false on I/O or parse error.
    static bool LoadFromFile(const std::string& path, SceneFileData& outScene, std::string* outError = nullptr);

    // Default Scenes directory next to the exe (created if missing).
    static std::string DefaultScenesDirectory();
};

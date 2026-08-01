#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <DirectXMath.h>
#include "UiPresetSerializer.h"

// OneMore scene file (.scene) — human-readable text format.
// Serializes renderable entities + embedded UI preset payloads (not the editor camera).

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

// Scene association: which UI presets this level uses + visibility (saved with .scene).
struct SceneUiPresetEntry
{
    std::string name;
    bool visible = true; // shown when spawned / on scene load
    // Runtime only (not written to file): live instance from SpawnUiPreset
    uint32_t instanceId = 0;
};

struct SceneFileData
{
    static constexpr int kCurrentVersion = 1;
    int version = kCurrentVersion;
    std::string name;
    std::vector<SceneEntityData> entities;
    // UI presets to preload (+ optional spawn) when this scene loads.
    std::vector<SceneUiPresetEntry> uiPresets;
    // Full preset bodies embedded in the .scene (self-contained; also mirrored to UiPresets/).
    std::vector<UiPresetData> uiPresetPayloads;
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

    // List .scene stems in the default scenes directory.
    static std::vector<std::string> ListSceneNamesInDefaultDir();
    // Delete Scenes/<name>.scene (name with or without extension).
    static bool DeleteSceneFile(const std::string& nameOrPath);
};

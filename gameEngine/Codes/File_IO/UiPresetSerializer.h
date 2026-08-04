#pragma once
#include <string>
#include <vector>
#include <ostream>
#include <DirectXMath.h>
#include "ComponentStruct.h"

// OneMore UI preset file (.uipreset) — layout snapshot of in-game UI (not ImGui).
// Game scenes preload (register) presets by name, then Spawn when needed.
// Scenes also embed full preset payloads (ui_preset_data … end_ui_preset_data).

struct UiPresetElementData
{
    UiSpaceMode mode = UiSpaceMode::ScreenAlways;
    UiScaleMode scaleMode = UiScaleMode::Stretch;
    bool active = true;
    bool visible = true;
    int zOrder = 0;
    DirectX::XMFLOAT2 anchor{ 0.0f, 0.0f };
    DirectX::XMFLOAT2 pivot{ 0.5f, 0.5f };
    // layoutPercent: position=center 0..1, size=extent 0..1 of canvas
    DirectX::XMFLOAT2 position{ 0.5f, 0.5f };
    DirectX::XMFLOAT2 size{ 0.1f, 0.1f };
    float rotationRad = 0.0f;
    DirectX::XMFLOAT3 worldPos{ 0, 0, 0 }; // WorldBillboard only
    bool layoutPercent = true;
    float designW = 0.0f;
    float designH = 0.0f;
    bool snapLeft = false;
    bool snapRight = false;
    bool snapTop = false;
    bool snapBottom = false;

    std::string materialName;
    DirectX::XMFLOAT4 color{ 1, 1, 1, 1 };
    DirectX::XMFLOAT4 uvRect{ 0, 0, 1, 1 };

    bool hasButton = false;
    std::string actionId;
    bool interactable = true;
};

struct UiPresetData
{
    static constexpr int kCurrentVersion = 1;
    int version = kCurrentVersion;
    std::string name;
    // Default design resolution for elements that leave designW/H at 0.
    float designW = 0.0f;
    float designH = 0.0f;
    std::vector<UiPresetElementData> elements;
};

class UiPresetSerializer
{
public:
    static bool SaveToFile(const std::string& path, const UiPresetData& preset);
    static bool LoadFromFile(const std::string& path, UiPresetData& outPreset, std::string* outError = nullptr);

    // Write/read element blocks (shared by .uipreset and embedded scene payload).
    static void WriteElements(std::ostream& out, const UiPresetData& preset);
    // Apply one non-empty trimmed line while parsing elements. Returns true if handled.
    // curElement is null outside an element block; may be updated when line is "element"/"end".
    static bool ApplyElementLine(
        const std::string& line,
        UiPresetData& preset,
        UiPresetElementData*& curElement);

    // Default UiPresets directory next to the exe (created if missing).
    static std::string DefaultUiPresetsDirectory();

    // Resolve "name" or "name.uipreset" to a full path under the default dir (or absolute).
    static std::string ResolvePresetPath(const std::string& nameOrPath);

    // List preset stems (no extension) found in the default directory.
    static std::vector<std::string> ListPresetNamesInDefaultDir();

    // Delete UiPresets/<name>.uipreset (returns false if missing / IO error).
    static bool DeletePresetFile(const std::string& nameOrPath);
};

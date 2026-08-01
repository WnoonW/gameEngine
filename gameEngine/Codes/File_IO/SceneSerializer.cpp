#include "SceneSerializer.h"
#include "UiPresetSerializer.h"
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
    std::string Trim(const std::string& s)
    {
        size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r'))
            ++b;
        size_t e = s.size();
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
            --e;
        return s.substr(b, e - b);
    }

    bool ParseFloat3(const std::string& val, DirectX::XMFLOAT3& out)
    {
        float x = 0, y = 0, z = 0;
        if (sscanf_s(val.c_str(), "%f,%f,%f", &x, &y, &z) != 3)
            return false;
        out = { x, y, z };
        return true;
    }

    void WriteFloat3(std::ostream& out, const char* key, const DirectX::XMFLOAT3& v)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s=%.6g,%.6g,%.6g\n", key, v.x, v.y, v.z);
        out << buf;
    }

    std::string ExeDir()
    {
        char modulePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, modulePath, MAX_PATH) == 0)
            return {};
        char* slash = strrchr(modulePath, '\\');
        if (!slash)
            slash = strrchr(modulePath, '/');
        if (slash)
            *(slash + 1) = '\0';
        return modulePath;
    }
}

bool SceneSerializer::SaveToFile(const std::string& path, const SceneFileData& scene)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;

    out << "# OneMore Scene File\n";
    out << "version=" << SceneFileData::kCurrentVersion << "\n";
    out << "name=" << scene.name << "\n";
    out << "entity_count=" << scene.entities.size() << "\n";
    for (const auto& entry : scene.uiPresets)
    {
        if (entry.name.empty())
            continue;
        // ui_preset=name,visible  (visible: 1/0) — old files without ,flag load as visible=1
        out << "ui_preset=" << entry.name << "," << (entry.visible ? 1 : 0) << "\n";
    }
    out << "\n";

    // Full UI preset data (self-contained scene package)
    for (const auto& preset : scene.uiPresetPayloads)
    {
        if (preset.name.empty() && preset.elements.empty())
            continue;
        out << "ui_preset_data\n";
        out << "name=" << preset.name << "\n";
        out << "version=" << preset.version << "\n";
        out << "element_count=" << preset.elements.size() << "\n";
        if (preset.designW > 1.0f && preset.designH > 1.0f)
            out << "design=" << preset.designW << "," << preset.designH << "\n";
        UiPresetSerializer::WriteElements(out, preset);
        out << "end_ui_preset_data\n\n";
    }

    for (const auto& e : scene.entities)
    {
        out << "entity\n";
        out << "mesh=" << e.meshName << "\n";
        out << "mainMaterial=" << e.mainMaterial << "\n";
        WriteFloat3(out, "pos", e.position);
        WriteFloat3(out, "rot", e.rotation);
        WriteFloat3(out, "scl", e.scale);
        out << "visible=" << (e.visible ? 1 : 0) << "\n";

        if (e.hasGravity)
        {
            out << "gravity=1\n";
            out << "gravity_enabled=" << (e.gravityEnabled ? 1 : 0) << "\n";
            out << "gravity_strength=" << e.gravityStrength << "\n";
            WriteFloat3(out, "gravity_vel", e.gravityVelocity);
            WriteFloat3(out, "gravity_ang", e.gravityAngularVelocity);
        }
        else
        {
            out << "gravity=0\n";
        }

        if (e.hasCollision)
        {
            out << "collision=1\n";
            out << "collision_enabled=" << (e.collisionEnabled ? 1 : 0) << "\n";
            out << "collision_static=" << (e.collisionStatic ? 1 : 0) << "\n";
            out << "collision_restitution=" << e.collisionRestitution << "\n";
        }
        else
        {
            out << "collision=0\n";
        }

        for (const auto& sub : e.subMaterials)
        {
            // key may contain '=' rarely; use first '=' as separator only in parser for value
            out << "sub=" << sub.first << "=" << sub.second << "\n";
        }

        out << "end\n\n";
    }

    return static_cast<bool>(out);
}

bool SceneSerializer::LoadFromFile(const std::string& path, SceneFileData& outScene, std::string* outError)
{
    std::ifstream in(path);
    if (!in)
    {
        if (outError) *outError = "Failed to open file: " + path;
        return false;
    }

    outScene = SceneFileData{};
    SceneEntityData* cur = nullptr;
    bool inUiPresetData = false;
    UiPresetData* curPreset = nullptr;
    UiPresetElementData* curPresetEl = nullptr;

    std::string line;
    int lineNo = 0;
    while (std::getline(in, line))
    {
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        // --- Embedded UI preset payload ---
        if (line == "ui_preset_data")
        {
            outScene.uiPresetPayloads.emplace_back();
            curPreset = &outScene.uiPresetPayloads.back();
            curPresetEl = nullptr;
            inUiPresetData = true;
            cur = nullptr;
            continue;
        }
        if (line == "end_ui_preset_data")
        {
            if (curPreset && curPreset->version <= 0)
                curPreset->version = UiPresetData::kCurrentVersion;
            if (curPreset && !curPreset->name.empty())
            {
                const bool exists = std::any_of(
                    outScene.uiPresets.begin(), outScene.uiPresets.end(),
                    [&](const SceneUiPresetEntry& e) { return e.name == curPreset->name; });
                if (!exists)
                    outScene.uiPresets.push_back(SceneUiPresetEntry{ curPreset->name, true });
            }
            inUiPresetData = false;
            curPreset = nullptr;
            curPresetEl = nullptr;
            continue;
        }
        if (inUiPresetData && curPreset)
        {
            if (!UiPresetSerializer::ApplyElementLine(line, *curPreset, curPresetEl))
            {
                if (outError)
                    *outError = "UI preset parse error at line " + std::to_string(lineNo) + ": " + line;
                return false;
            }
            continue;
        }

        if (line == "entity")
        {
            outScene.entities.emplace_back();
            cur = &outScene.entities.back();
            continue;
        }
        if (line == "end")
        {
            cur = nullptr;
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos)
        {
            if (outError)
                *outError = "Parse error at line " + std::to_string(lineNo) + ": " + line;
            return false;
        }

        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));

        if (!cur)
        {
            if (key == "version")
                outScene.version = std::atoi(val.c_str());
            else if (key == "name")
                outScene.name = val;
            else if (key == "entity_count")
            { /* optional, ignored */ }
            else if (key == "ui_preset")
            {
                if (!val.empty())
                {
                    // Formats: "name" | "name,1" | "name,0" | "name,true" | "name,false"
                    SceneUiPresetEntry entry;
                    entry.visible = true;
                    const auto comma = val.find(',');
                    if (comma == std::string::npos)
                    {
                        entry.name = val;
                    }
                    else
                    {
                        entry.name = Trim(val.substr(0, comma));
                        const std::string flag = Trim(val.substr(comma + 1));
                        entry.visible = !(flag == "0" || flag == "false" || flag == "False");
                    }
                    if (entry.name.empty())
                        continue;
                    const bool exists = std::any_of(
                        outScene.uiPresets.begin(), outScene.uiPresets.end(),
                        [&](const SceneUiPresetEntry& e) { return e.name == entry.name; });
                    if (!exists)
                        outScene.uiPresets.push_back(std::move(entry));
                }
            }
            continue;
        }

        if (key == "mesh")
            cur->meshName = val;
        else if (key == "mainMaterial")
            cur->mainMaterial = val;
        else if (key == "pos")
        {
            if (!ParseFloat3(val, cur->position))
            {
                if (outError) *outError = "Bad pos at line " + std::to_string(lineNo);
                return false;
            }
        }
        else if (key == "rot")
        {
            if (!ParseFloat3(val, cur->rotation))
            {
                if (outError) *outError = "Bad rot at line " + std::to_string(lineNo);
                return false;
            }
        }
        else if (key == "scl")
        {
            if (!ParseFloat3(val, cur->scale))
            {
                if (outError) *outError = "Bad scl at line " + std::to_string(lineNo);
                return false;
            }
        }
        else if (key == "visible")
            cur->visible = (val == "1" || val == "true");
        else if (key == "gravity")
            cur->hasGravity = (val == "1" || val == "true");
        else if (key == "gravity_enabled")
            cur->gravityEnabled = (val == "1" || val == "true");
        else if (key == "gravity_strength")
            cur->gravityStrength = static_cast<float>(std::atof(val.c_str()));
        else if (key == "gravity_vel")
            ParseFloat3(val, cur->gravityVelocity);
        else if (key == "gravity_ang")
            ParseFloat3(val, cur->gravityAngularVelocity);
        else if (key == "collision")
            cur->hasCollision = (val == "1" || val == "true");
        else if (key == "collision_enabled")
            cur->collisionEnabled = (val == "1" || val == "true");
        else if (key == "collision_static")
            cur->collisionStatic = (val == "1" || val == "true");
        else if (key == "collision_restitution")
            cur->collisionRestitution = static_cast<float>(std::atof(val.c_str()));
        else if (key == "sub")
        {
            // sub=submeshKey=materialName
            const auto eq2 = val.find('=');
            if (eq2 == std::string::npos)
            {
                if (outError) *outError = "Bad sub= at line " + std::to_string(lineNo);
                return false;
            }
            const std::string subKey = Trim(val.substr(0, eq2));
            const std::string mat = Trim(val.substr(eq2 + 1));
            if (!subKey.empty())
                cur->subMaterials[subKey] = mat;
        }
    }

    if (outScene.version <= 0)
        outScene.version = SceneFileData::kCurrentVersion;

    return true;
}

std::string SceneSerializer::DefaultScenesDirectory()
{
    std::string dir = ExeDir() + "Scenes";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::vector<std::string> SceneSerializer::ListSceneNamesInDefaultDir()
{
    std::vector<std::string> names;
    try
    {
        const fs::path dir = DefaultScenesDirectory();
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return names;
        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".scene") continue;
            names.push_back(entry.path().stem().string());
        }
        std::sort(names.begin(), names.end());
    }
    catch (...) {}
    return names;
}

bool SceneSerializer::DeleteSceneFile(const std::string& nameOrPath)
{
    if (nameOrPath.empty())
        return false;
    try
    {
        fs::path p(nameOrPath);
        std::error_code ec;
        if (!p.is_absolute())
        {
            std::string stem = nameOrPath;
            if (stem.size() > 6 && stem.substr(stem.size() - 6) == ".scene")
                stem = stem.substr(0, stem.size() - 6);
            p = fs::path(DefaultScenesDirectory()) / (stem + ".scene");
        }
        if (!fs::is_regular_file(p, ec))
            return false;
        return fs::remove(p, ec) && !ec;
    }
    catch (...)
    {
        return false;
    }
}

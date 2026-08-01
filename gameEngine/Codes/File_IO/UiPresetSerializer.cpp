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

    bool ParseFloat2(const std::string& val, DirectX::XMFLOAT2& out)
    {
        float x = 0, y = 0;
        if (sscanf_s(val.c_str(), "%f,%f", &x, &y) != 2)
            return false;
        out = { x, y };
        return true;
    }

    bool ParseFloat3(const std::string& val, DirectX::XMFLOAT3& out)
    {
        float x = 0, y = 0, z = 0;
        if (sscanf_s(val.c_str(), "%f,%f,%f", &x, &y, &z) != 3)
            return false;
        out = { x, y, z };
        return true;
    }

    bool ParseFloat4(const std::string& val, DirectX::XMFLOAT4& out)
    {
        float x = 0, y = 0, z = 0, w = 0;
        if (sscanf_s(val.c_str(), "%f,%f,%f,%f", &x, &y, &z, &w) != 4)
            return false;
        out = { x, y, z, w };
        return true;
    }

    void WriteFloat2(std::ostream& out, const char* key, const DirectX::XMFLOAT2& v)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s=%.6g,%.6g\n", key, v.x, v.y);
        out << buf;
    }

    void WriteFloat3(std::ostream& out, const char* key, const DirectX::XMFLOAT3& v)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s=%.6g,%.6g,%.6g\n", key, v.x, v.y, v.z);
        out << buf;
    }

    void WriteFloat4(std::ostream& out, const char* key, const DirectX::XMFLOAT4& v)
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s=%.6g,%.6g,%.6g,%.6g\n", key, v.x, v.y, v.z, v.w);
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

    std::string StemName(const std::string& nameOrPath)
    {
        try
        {
            fs::path p(nameOrPath);
            if (p.has_extension() && p.extension() == ".uipreset")
                return p.stem().string();
            if (p.has_filename() && p.filename() != p)
                return p.stem().string();
            return nameOrPath;
        }
        catch (...)
        {
            return nameOrPath;
        }
    }
}

void UiPresetSerializer::WriteElements(std::ostream& out, const UiPresetData& preset)
{
    for (const auto& e : preset.elements)
    {
        out << "element\n";
        out << "mode=" << static_cast<int>(e.mode) << "\n";
        out << "active=" << (e.active ? 1 : 0) << "\n";
        out << "visible=" << (e.visible ? 1 : 0) << "\n";
        out << "zOrder=" << e.zOrder << "\n";
        WriteFloat2(out, "anchor", e.anchor);
        WriteFloat2(out, "pivot", e.pivot);
        WriteFloat2(out, "pos", e.position);
        WriteFloat2(out, "size", e.size);
        out << "rot=" << e.rotationRad << "\n";
        WriteFloat3(out, "worldPos", e.worldPos);
        out << "layoutPercent=" << (e.layoutPercent ? 1 : 0) << "\n";
        if (e.designW > 1.0f && e.designH > 1.0f)
            out << "design=" << e.designW << "," << e.designH << "\n";
        out << "material=" << e.materialName << "\n";
        WriteFloat4(out, "color", e.color);
        WriteFloat4(out, "uv", e.uvRect);
        out << "button=" << (e.hasButton ? 1 : 0) << "\n";
        if (e.hasButton)
        {
            out << "actionId=" << e.actionId << "\n";
            out << "interactable=" << (e.interactable ? 1 : 0) << "\n";
        }
        out << "end\n\n";
    }
}

bool UiPresetSerializer::ApplyElementLine(
    const std::string& line,
    UiPresetData& preset,
    UiPresetElementData*& curElement)
{
    if (line == "element")
    {
        preset.elements.emplace_back();
        curElement = &preset.elements.back();
        return true;
    }
    if (line == "end")
    {
        curElement = nullptr;
        return true;
    }

    const auto eq = line.find('=');
    if (eq == std::string::npos)
        return false;

    const std::string key = Trim(line.substr(0, eq));
    const std::string val = Trim(line.substr(eq + 1));

    if (!curElement)
    {
        if (key == "version")
            preset.version = std::atoi(val.c_str());
        else if (key == "name")
            preset.name = val;
        else if (key == "element_count")
        { /* optional */ }
        else if (key == "design")
        {
            DirectX::XMFLOAT2 d{};
            if (ParseFloat2(val, d))
            {
                preset.designW = d.x;
                preset.designH = d.y;
            }
        }
        else
            return false;
        return true;
    }

    if (key == "mode")
        curElement->mode = static_cast<UiSpaceMode>(std::atoi(val.c_str()));
    else if (key == "active")
        curElement->active = (val == "1" || val == "true");
    else if (key == "visible")
        curElement->visible = (val == "1" || val == "true");
    else if (key == "zOrder")
        curElement->zOrder = std::atoi(val.c_str());
    else if (key == "anchor")
        ParseFloat2(val, curElement->anchor);
    else if (key == "pivot")
        ParseFloat2(val, curElement->pivot);
    else if (key == "pos")
        ParseFloat2(val, curElement->position);
    else if (key == "size")
        ParseFloat2(val, curElement->size);
    else if (key == "rot")
        curElement->rotationRad = static_cast<float>(std::atof(val.c_str()));
    else if (key == "worldPos")
        ParseFloat3(val, curElement->worldPos);
    else if (key == "layoutPercent")
        curElement->layoutPercent = (val == "1" || val == "true");
    else if (key == "design")
    {
        DirectX::XMFLOAT2 d{};
        if (ParseFloat2(val, d))
        {
            curElement->designW = d.x;
            curElement->designH = d.y;
        }
    }
    else if (key == "material")
        curElement->materialName = val;
    else if (key == "color")
        ParseFloat4(val, curElement->color);
    else if (key == "uv")
        ParseFloat4(val, curElement->uvRect);
    else if (key == "button")
        curElement->hasButton = (val == "1" || val == "true");
    else if (key == "actionId")
        curElement->actionId = val;
    else if (key == "interactable")
        curElement->interactable = (val == "1" || val == "true");
    else
        return false;
    return true;
}

bool UiPresetSerializer::SaveToFile(const std::string& path, const UiPresetData& preset)
{
    try
    {
        fs::path p(path);
        if (p.has_parent_path())
        {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
        }
    }
    catch (...) {}

    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;

    out << "# OneMore UI Preset\n";
    out << "version=" << UiPresetData::kCurrentVersion << "\n";
    out << "name=" << (preset.name.empty() ? StemName(path) : preset.name) << "\n";
    out << "element_count=" << preset.elements.size() << "\n";
    if (preset.designW > 1.0f && preset.designH > 1.0f)
        out << "design=" << preset.designW << "," << preset.designH << "\n";
    out << "\n";
    WriteElements(out, preset);

    return static_cast<bool>(out);
}

bool UiPresetSerializer::LoadFromFile(const std::string& path, UiPresetData& outPreset, std::string* outError)
{
    std::ifstream in(path);
    if (!in)
    {
        if (outError) *outError = "Failed to open UI preset: " + path;
        return false;
    }

    outPreset = UiPresetData{};
    UiPresetElementData* cur = nullptr;

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

        if (!ApplyElementLine(line, outPreset, cur))
        {
            if (outError)
                *outError = "Parse error at line " + std::to_string(lineNo) + ": " + line;
            return false;
        }
    }

    if (outPreset.name.empty())
        outPreset.name = StemName(path);
    if (outPreset.version <= 0)
        outPreset.version = UiPresetData::kCurrentVersion;

    return true;
}

std::string UiPresetSerializer::DefaultUiPresetsDirectory()
{
    std::string dir = ExeDir() + "UiPresets";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string UiPresetSerializer::ResolvePresetPath(const std::string& nameOrPath)
{
    if (nameOrPath.empty())
        return {};

    try
    {
        fs::path p(nameOrPath);
        std::error_code ec;
        if (p.is_absolute() && fs::is_regular_file(p, ec))
            return p.string();

        // Already a relative path to an existing file
        if (fs::is_regular_file(p, ec))
            return fs::absolute(p, ec).string();

        std::string stem = nameOrPath;
        // strip extension if present
        if (stem.size() > 9 && stem.substr(stem.size() - 9) == ".uipreset")
            stem = stem.substr(0, stem.size() - 9);

        // strip directories for stem-only lookup
        try
        {
            fs::path asPath(stem);
            if (asPath.has_filename())
                stem = asPath.stem().string();
        }
        catch (...) {}

        const fs::path def = fs::path(DefaultUiPresetsDirectory()) / (stem + ".uipreset");
        return def.string();
    }
    catch (...)
    {
        return nameOrPath;
    }
}

std::vector<std::string> UiPresetSerializer::ListPresetNamesInDefaultDir()
{
    std::vector<std::string> names;
    try
    {
        const fs::path dir = DefaultUiPresetsDirectory();
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return names;

        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file(ec))
                continue;
            if (entry.path().extension() != ".uipreset")
                continue;
            names.push_back(entry.path().stem().string());
        }
        std::sort(names.begin(), names.end());
    }
    catch (...) {}
    return names;
}

bool UiPresetSerializer::DeletePresetFile(const std::string& nameOrPath)
{
    if (nameOrPath.empty())
        return false;
    try
    {
        const fs::path path = ResolvePresetPath(nameOrPath);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            return false;
        return fs::remove(path, ec) && !ec;
    }
    catch (...)
    {
        return false;
    }
}

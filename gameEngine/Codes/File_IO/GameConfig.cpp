#include "GameConfig.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>

std::string GameConfig::GetExeDirectory()
{
    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, modulePath, MAX_PATH) == 0)
        return {};

    char* slash = strrchr(modulePath, '\\');
    if (!slash)
        slash = strrchr(modulePath, '/');
    if (slash)
        *(slash + 1) = '\0';
    else
        modulePath[0] = '\0';
    return modulePath;
}

std::string GameConfig::JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    const char last = a.back();
    if (last == '\\' || last == '/')
        return a + b;
    return a + "\\" + b;
}

bool GameConfig::SetWorkingDirectoryToExe()
{
    auto hasResources = [](const std::string& dir) -> bool
    {
        if (dir.empty())
            return false;
        // Prefer a folder that actually contains shaders (runtime load path).
        const std::string shaders = JoinPath(JoinPath(dir, "Resources"), "Shaders");
        const DWORD attr = GetFileAttributesA(shaders.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    };

    char cwd[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, cwd);

    // 1) Visual Studio 디버그: 작업 디렉터리가 프로젝트 루트인 경우가 많음 → 유지
    if (hasResources(cwd) || hasResources("."))
        return true;

    const std::string exeDir = GetExeDirectory();
    if (exeDir.empty())
        return false;

    // 2) 추출 패키지 / exe 옆 Resources
    if (hasResources(exeDir))
        return SetCurrentDirectoryA(exeDir.c_str()) != 0;

    // 3) x64\Debug → 상위로 올라가며 Resources\Shaders 탐색 (프로젝트 루트 등)
    std::string probe = exeDir;
    while (!probe.empty() && (probe.back() == '\\' || probe.back() == '/'))
        probe.pop_back();

    for (int i = 0; i < 6; ++i)
    {
        std::string asDir = probe;
        if (!asDir.empty() && asDir.back() != '\\' && asDir.back() != '/')
            asDir.push_back('\\');

        if (hasResources(asDir))
            return SetCurrentDirectoryA(asDir.c_str()) != 0;

        const auto slash = probe.find_last_of("\\/");
        if (slash == std::string::npos || slash == 0)
            break;
        probe.resize(slash);
    }

    // 4) 최후: exe 폴더
    return SetCurrentDirectoryA(exeDir.c_str()) != 0;
}

bool GameConfig::LoadFromExeDirectory()
{
    const std::string path = JoinPath(GetExeDirectory(), "game.cfg");
    std::ifstream in(path);
    if (!in)
        return false;

    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // trim
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.erase(line.begin());
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(val.begin());

        if (key == "mode")
        {
            std::string lower = val;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            mode = (lower == "play" || lower == "game") ? Mode::Play : Mode::Editor;
        }
        else if (key == "title")
            title = val;
        else if (key == "scene")
            scenePath = val;
        else if (key == "mouse_look")
            mouseLookOnStart = (val == "1" || val == "true" || val == "True");
    }
    return true;
}

bool GameConfig::SaveToDirectory(const std::string& directory) const
{
    const std::string path = JoinPath(directory, "game.cfg");
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;

    out << "# OneMore packaged game config\n";
    out << "mode=" << (mode == Mode::Play ? "play" : "editor") << "\n";
    out << "title=" << title << "\n";
    out << "scene=" << scenePath << "\n";
    out << "mouse_look=" << (mouseLookOnStart ? 1 : 0) << "\n";
    return static_cast<bool>(out);
}

void GameConfig::ApplyCommandLine(const char* cmdLine)
{
    if (!cmdLine || !*cmdLine)
        return;

    std::string cmd = cmdLine;
    // crude tokenize
    std::istringstream iss(cmd);
    std::string token;
    while (iss >> token)
    {
        if (token == "--play" || token == "-play")
            mode = Mode::Play;
        else if (token == "--editor" || token == "-editor")
            mode = Mode::Editor;
        else if (token == "--scene" || token == "-scene")
        {
            std::string path;
            if (iss >> path)
            {
                // strip quotes
                if (!path.empty() && path.front() == '"')
                {
                    path.erase(path.begin());
                    while (!path.empty() && path.back() != '"')
                    {
                        std::string more;
                        if (!(iss >> more))
                            break;
                        path += " " + more;
                    }
                    if (!path.empty() && path.back() == '"')
                        path.pop_back();
                }
                scenePath = path;
                mode = Mode::Play;
            }
        }
    }
}

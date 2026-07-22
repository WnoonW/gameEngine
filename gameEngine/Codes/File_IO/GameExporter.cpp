#include "GameExporter.h"
#include "GameConfig.h"
#include "Engine.h"
#include "SceneSerializer.h"
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <system_error>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

namespace
{
    // Runtime이 반드시 필요로 하는 셰이더 (Debug 출력 폴더에 옛 파일만 있으면 추출본이 깨짐)
    const char* kRequiredShaders[] = {
        "build_indirect_commands.hlsl",
        "compose_world.hlsl",
        "update_motion.hlsl",
        "hiz_build.hlsl",
        "object.hlsl",
        "object_cb.hlsl",
        "texture.hlsl",
        "color.hlsl",
        "instancing.hlsl",
        "object_instanced.hlsl",
    };

    bool FileExists(const fs::path& p)
    {
        std::error_code ec;
        return fs::is_regular_file(p, ec);
    }

    bool ResourcesLooksComplete(const fs::path& resourcesRoot, std::string* missingOut = nullptr)
    {
        const fs::path shaders = resourcesRoot / "Shaders";
        std::error_code ec;
        if (!fs::is_directory(shaders, ec))
        {
            if (missingOut)
                *missingOut = "Resources/Shaders folder missing under " + resourcesRoot.string();
            return false;
        }

        std::string missing;
        for (const char* name : kRequiredShaders)
        {
            if (!FileExists(shaders / name))
            {
                if (!missing.empty())
                    missing += ", ";
                missing += name;
            }
        }
        if (!missing.empty())
        {
            if (missingOut)
                *missingOut = "Missing shaders: " + missing + " (in " + shaders.string() + ")";
            return false;
        }

        // 기본 에셋/텍스처도 최소한 확인
        if (!fs::is_directory(resourcesRoot / "Textures", ec) ||
            !fs::is_directory(resourcesRoot / "Assets", ec))
        {
            if (missingOut)
                *missingOut = "Resources/Textures or Resources/Assets missing";
            return false;
        }
        return true;
    }

    // exeDir, CWD, 상위 경로에서 "완전한" Resources 를 찾는다.
    fs::path FindBestResourcesSource(const std::string& exeDir, std::string& detail)
    {
        std::vector<fs::path> candidates;

        auto addCandidate = [&](const fs::path& p)
        {
            if (p.empty())
                return;
            std::error_code ec;
            const fs::path abs = fs::absolute(p, ec);
            if (ec)
                return;
            for (const auto& c : candidates)
            {
                if (c == abs)
                    return;
            }
            candidates.push_back(abs);
        };

        // 1) 현재 작업 디렉터리 (VS 디버그 = 프로젝트 루트인 경우 완전본)
        addCandidate(fs::path("Resources"));
        // 2) exe 옆 (추출/배포 시)
        addCandidate(fs::path(exeDir) / "Resources");
        // 3) exe 상위들 (x64/Debug → project root)
        {
            fs::path probe = exeDir;
            for (int i = 0; i < 6; ++i)
            {
                addCandidate(probe / "Resources");
                if (!probe.has_parent_path() || probe == probe.root_path())
                    break;
                probe = probe.parent_path();
            }
        }

        fs::path bestIncomplete;
        std::string bestMissing;
        for (const auto& c : candidates)
        {
            std::error_code ec;
            if (!fs::is_directory(c, ec))
                continue;
            std::string missing;
            if (ResourcesLooksComplete(c, &missing))
            {
                detail = "Using Resources from: " + fs::absolute(c).string();
                return fs::absolute(c);
            }
            if (bestIncomplete.empty())
            {
                bestIncomplete = c;
                bestMissing = missing;
            }
        }

        if (!bestIncomplete.empty())
        {
            detail = "Only incomplete Resources found. " + bestMissing;
            // still return incomplete so caller can decide — prefer fail
            return {};
        }
        detail = "No Resources folder found near the executable or working directory.";
        return {};
    }

    bool CopyDirectoryRecursive(const fs::path& src, const fs::path& dst, std::string& err)
    {
        std::error_code ec;
        if (!fs::exists(src, ec))
        {
            err = "Source missing: " + src.string();
            return false;
        }

        // Clean destination first so stale Debug shaders (e.g. build_indirect.hlsl) don't remain
        if (fs::exists(dst, ec))
            fs::remove_all(dst, ec);

        fs::create_directories(dst, ec);
        if (ec)
        {
            err = "Failed to create: " + dst.string() + " (" + ec.message() + ")";
            return false;
        }

        for (fs::recursive_directory_iterator it(src, ec), end; it != end; it.increment(ec))
        {
            if (ec)
            {
                err = "Iterate failed: " + ec.message();
                return false;
            }
            const fs::path& p = it->path();
            const fs::path rel = fs::relative(p, src, ec);
            if (ec)
            {
                err = "relative() failed: " + ec.message();
                return false;
            }
            const fs::path target = dst / rel;
            if (it->is_directory(ec))
            {
                fs::create_directories(target, ec);
            }
            else if (it->is_regular_file(ec))
            {
                fs::create_directories(target.parent_path(), ec);
                fs::copy_file(p, target, fs::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    err = "Copy failed: " + p.string() + " -> " + target.string() + " (" + ec.message() + ")";
                    return false;
                }
            }
        }
        return true;
    }

    std::string SanitizeFolderName(std::string name)
    {
        for (char& c : name)
        {
            if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                c == '\\' || c == '|' || c == '?' || c == '*')
                c = '_';
        }
        if (name.empty())
            name = "MyGame";
        return name;
    }
}

std::string GameExporter::SuggestExportDirectory(const std::string& gameTitle)
{
    return GameConfig::JoinPath(
        GameConfig::JoinPath(GameConfig::GetExeDirectory(), "GameExport"),
        SanitizeFolderName(gameTitle));
}

std::string GameExporter::BrowseForFolder(HWND owner, const char* title)
{
    // SHBrowseForFolder 는 COM 필요
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(co) || co == S_FALSE;

    char display[MAX_PATH] = {};
    BROWSEINFOA bi{};
    bi.hwndOwner = owner;
    bi.pszDisplayName = display;
    bi.lpszTitle = title ? title : "Select export folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    std::string result;
    if (pidl)
    {
        char path[MAX_PATH] = {};
        if (SHGetPathFromIDListA(pidl, path))
            result = path;
        CoTaskMemFree(pidl);
    }

    if (needUninit)
        CoUninitialize();
    return result;
}

GameExportResult GameExporter::Export(
    HWND owner,
    Engine& engine,
    const std::string& exportDir,
    const std::string& gameTitle)
{
    GameExportResult result;
    result.exportDirectory = exportDir;

    if (exportDir.empty())
    {
        result.message = "Export directory is empty";
        return result;
    }

    std::error_code ec;
    fs::create_directories(exportDir, ec);
    if (ec)
    {
        result.message = "Failed to create export directory: " + ec.message();
        return result;
    }

    const std::string exeDir = GameConfig::GetExeDirectory();
    if (exeDir.empty())
    {
        result.message = "Could not resolve executable directory";
        return result;
    }

    // 1) Save current scene into package
    const fs::path scenesDir = fs::path(exportDir) / "Scenes";
    fs::create_directories(scenesDir, ec);
    const std::string scenePath = (scenesDir / "game.scene").string();
    if (!engine.SaveSceneToFile(scenePath, gameTitle.empty() ? "game" : gameTitle))
    {
        result.message = "Failed to write Scenes/game.scene";
        return result;
    }

    // 2) Copy complete Resources (prefer project root over stale x64/Debug/Resources)
    const fs::path dstRes = fs::path(exportDir) / "Resources";
    std::string findDetail;
    const fs::path srcRes = FindBestResourcesSource(exeDir, findDetail);
    if (srcRes.empty())
    {
        result.message =
            "Could not find a complete Resources folder to export.\n\n"
            + findDetail +
            "\n\nTip: run the editor with the project Resources available\n"
            "(Visual Studio working directory = project root), then export again.";
        return result;
    }

    std::string copyErr;
    if (!CopyDirectoryRecursive(srcRes, dstRes, copyErr))
    {
        result.message = "Failed to copy Resources from:\n" + srcRes.string()
            + "\n\n" + copyErr;
        return result;
    }

    // Verify destination has required shaders
    {
        std::string missing;
        if (!ResourcesLooksComplete(dstRes, &missing))
        {
            result.message = "Export Resources incomplete after copy.\n" + missing;
            return result;
        }
    }

    // 3) Copy this executable
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    const fs::path exeSrc = modulePath;
    const fs::path exeDst = fs::path(exportDir) / exeSrc.filename();
    fs::copy_file(exeSrc, exeDst, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        result.message = "Failed to copy executable: " + ec.message();
        return result;
    }

    // 4) game.cfg (player mode)
    GameConfig cfg;
    cfg.mode = GameConfig::Mode::Play;
    cfg.title = gameTitle.empty() ? "OneMore Game" : gameTitle;
    cfg.scenePath = "Scenes/game.scene";
    cfg.mouseLookOnStart = true;
    if (!cfg.SaveToDirectory(exportDir))
    {
        result.message = "Failed to write game.cfg";
        return result;
    }

    // 5) Play.bat launcher (cd to this folder, run with --play; pause on failure)
    {
        const std::string batPath = GameConfig::JoinPath(exportDir, "Play.bat");
        std::ofstream bat(batPath, std::ios::trunc);
        if (bat)
        {
            const std::string exeName = exeSrc.filename().string();
            bat << "@echo off\r\n";
            bat << "cd /d \"%~dp0\"\r\n";
            bat << "echo Working dir: %CD%\r\n";
            bat << "if not exist \"Resources\\\" (\r\n";
            bat << "  echo ERROR: Resources folder missing.\r\n";
            bat << "  pause\r\n";
            bat << "  exit /b 1\r\n";
            bat << ")\r\n";
            bat << "if not exist \"game.cfg\" (\r\n";
            bat << "  echo ERROR: game.cfg missing.\r\n";
            bat << "  pause\r\n";
            bat << "  exit /b 1\r\n";
            bat << ")\r\n";
            // Do not use "start" — it can drop the working directory in some cases.
            bat << "\"" << exeName << "\" --play\r\n";
            bat << "set ERR=%ERRORLEVEL%\r\n";
            bat << "if not %ERR%==0 (\r\n";
            bat << "  echo.\r\n";
            bat << "  echo Game exited with error code %ERR%\r\n";
            bat << "  pause\r\n";
            bat << ")\r\n";
        }
    }

    // 6) README
    {
        const std::string readmePath = GameConfig::JoinPath(exportDir, "README.txt");
        std::ofstream readme(readmePath, std::ios::trunc);
        if (readme)
        {
            readme << cfg.title << "\n";
            readme << "====================\n\n";
            readme << "How to play:\n";
            readme << "  1. Double-click Play.bat  (or run the .exe with --play)\n";
            readme << "  2. Mouse look: LMB click (or auto on start)\n";
            readme << "  3. Move: WASD, Space/Shift, Q/E\n";
            readme << "  4. ESC: release mouse / quit\n\n";
            readme << "Contents:\n";
            readme << "  game.cfg          - player settings\n";
            readme << "  Scenes/game.scene - exported level\n";
            readme << "  Resources/        - meshes, textures, shaders\n";
            readme << "  " << exeSrc.filename().string() << " - game runtime\n";
        }
    }

    result.ok = true;
    result.message = "Game exported to:\n" + exportDir
        + "\n\n" + findDetail
        + "\n\nRun Play.bat inside that folder to play.";
    (void)owner;
    return result;
}

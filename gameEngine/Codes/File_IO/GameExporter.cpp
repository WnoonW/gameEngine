#include "GameExporter.h"
#include "GameConfig.h"
#include "Engine.h"
#include "SceneSerializer.h"
#include "UiPresetSerializer.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    // Shaders actually loaded by Engine/RenderSystem (do not list obsolete stubs)
    const char* kRequiredShaders[] = {
        "build_indirect_commands.hlsl",
        "compose_world.hlsl",
        "update_motion.hlsl",
        "hiz_build.hlsl",
        "object_cb.hlsl",
        "object_instanced.hlsl",
        "ui_quad.hlsl",
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

    // 1) Write game.scene — pack live UI into THIS file only (no editor UiPresets auto-create)
    const fs::path scenesDir = fs::path(exportDir) / "Scenes";
    fs::create_directories(scenesDir, ec);
    const std::string scenePath = (scenesDir / "game.scene").string();
    if (!engine.SaveSceneToFile(scenePath, gameTitle.empty() ? "game" : gameTitle, /*packLiveUiIntoFile=*/true))
    {
        result.message = "Failed to write Scenes/game.scene";
        return result;
    }

    // 1b) Copy all UiPresets (scene-listed + just-written startup snapshot)
    {
        const fs::path dstPresets = fs::path(exportDir) / "UiPresets";
        fs::create_directories(dstPresets, ec);
        const fs::path srcPresets = UiPresetSerializer::DefaultUiPresetsDirectory();
        std::error_code lec;
        if (fs::is_directory(srcPresets, lec))
        {
            std::string presetCopyErr;
            if (!CopyDirectoryRecursive(srcPresets, dstPresets, presetCopyErr))
            {
                OutputDebugStringA(("[Export] UiPresets copy warning: " + presetCopyErr + "\n").c_str());
            }
        }
        // Force-copy every scene-listed preset (overwrite so export matches editor state)
        for (const auto& entry : engine.GetSceneUiPresetList())
        {
            if (entry.name.empty())
                continue;
            const fs::path src = UiPresetSerializer::ResolvePresetPath(entry.name);
            const fs::path dst = dstPresets / (entry.name + ".uipreset");
            if (fs::is_regular_file(src, lec))
            {
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, lec);
            }
        }
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

    // 3) Copy Game.exe (play product) — never ship Editor.exe
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    const fs::path editorExe = modulePath;
    const fs::path exeDirPath = exeDir;

    // Prefer Game.exe next to Editor.exe (shared OutDir: bin/x64/Config/)
    fs::path gameExeSrc = exeDirPath / "Game.exe";
    if (!FileExists(gameExeSrc))
    {
        // Fallback: same stem path variants / sibling Debug-Release
        const fs::path alt = editorExe.parent_path() / "Game.exe";
        if (FileExists(alt))
            gameExeSrc = alt;
    }
    if (!FileExists(gameExeSrc))
    {
        result.message =
            "Game.exe not found next to the editor.\n\n"
            "Build the Game project (OneMore.sln → Game) so Game.exe\n"
            "is produced in the same output folder as Editor.exe:\n"
            + exeDir + "\n\n"
            "Export packages the play-only Game.exe, not the editor.";
        return result;
    }

    const fs::path exeDst = fs::path(exportDir) / "Game.exe";
    fs::copy_file(gameExeSrc, exeDst, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        result.message = "Failed to copy Game.exe: " + ec.message();
        return result;
    }

    // Optional: copy PDB for crash debugging (ignore failure)
    {
        fs::path pdb = gameExeSrc;
        pdb.replace_extension(".pdb");
        if (FileExists(pdb))
        {
            std::error_code pec;
            fs::copy_file(pdb, fs::path(exportDir) / "Game.pdb",
                fs::copy_options::overwrite_existing, pec);
        }
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

    // 5) Play.bat launcher (runs Game.exe only)
    {
        const std::string batPath = GameConfig::JoinPath(exportDir, "Play.bat");
        std::ofstream bat(batPath, std::ios::trunc);
        if (bat)
        {
            bat << "@echo off\r\n";
            bat << "cd /d \"%~dp0\"\r\n";
            bat << "echo Working dir: %CD%\r\n";
            bat << "if not exist \"Resources\\\" (\r\n";
            bat << "  echo ERROR: Resources folder missing.\r\n";
            bat << "  pause\r\n";
            bat << "  exit /b 1\r\n";
            bat << ")\r\n";
            bat << "if not exist \"Game.exe\" (\r\n";
            bat << "  echo ERROR: Game.exe missing.\r\n";
            bat << "  pause\r\n";
            bat << "  exit /b 1\r\n";
            bat << ")\r\n";
            bat << "\"Game.exe\"\r\n";
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
            readme << "  1. Double-click Play.bat  (or run Game.exe)\n";
            readme << "  2. Mouse look: LMB click (or auto on start)\n";
            readme << "  3. Move: WASD, Space/Shift\n";
            readme << "  4. ESC: quit\n\n";
            readme << "Contents:\n";
            readme << "  game.cfg          - player settings\n";
            readme << "  Scenes/game.scene - exported level\n";
            readme << "  Resources/        - meshes, textures, shaders\n";
            readme << "  Game.exe          - play-only runtime (no editor)\n";
        }
    }

    result.ok = true;
    result.message = "Game exported to:\n" + exportDir
        + "\n\n" + findDetail
        + "\n\nRun Play.bat inside that folder to play.";
    (void)owner;
    return result;
}

#pragma once
#include <string>
#include <Windows.h>

// Runtime mode + packaged game settings (game.cfg next to the .exe).
struct GameConfig
{
    enum class Mode
    {
        Editor, // full editor UI
        Play    // exported game / --play
    };

    Mode mode = Mode::Editor;
    std::string title = "OneMore Game";
    std::string scenePath; // relative to exe dir, e.g. Scenes/game.scene
    bool mouseLookOnStart = true;

    // Resolve paths relative to the executable directory.
    static std::string GetExeDirectory();
    static std::string JoinPath(const std::string& a, const std::string& b);

    // Call once at process start so "Resources/..." relative paths resolve next to the .exe
    // (double-click / Play.bat / different CWD all work).
    static bool SetWorkingDirectoryToExe();

    // Load game.cfg next to exe if present. Returns false if missing.
    bool LoadFromExeDirectory();

    // Write game.cfg into directory (export target root).
    bool SaveToDirectory(const std::string& directory) const;

    // Parse WinMain command line ("--play", "--scene path").
    void ApplyCommandLine(const char* cmdLine);
};

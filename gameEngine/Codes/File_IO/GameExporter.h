#pragma once
#include <string>
#include <Windows.h>

class Engine;

// Packages the current scene + runtime into a playable folder.
// Output layout:
//   <exportDir>/
//     game.cfg
//     README.txt
//     Play.bat
//     <exe name>
//     Resources/   (copied from running build)
//     Scenes/game.scene
struct GameExportResult
{
    bool ok = false;
    std::string exportDirectory;
    std::string message;
};

class GameExporter
{
public:
    // exportDir: folder that will contain the playable package (created if needed).
    // gameTitle: shown in game.cfg / window caption later.
    // engine: current editor scene source.
    static GameExportResult Export(
        HWND owner,
        Engine& engine,
        const std::string& exportDir,
        const std::string& gameTitle);

    // Folder picker (SHBrowseForFolder). Empty if cancelled.
    static std::string BrowseForFolder(HWND owner, const char* title);

    // Suggest default export path: <exeDir>/GameExport/<title>
    static std::string SuggestExportDirectory(const std::string& gameTitle);
};

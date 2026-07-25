#pragma warning(disable: 4005)
#include "AppHost.h"
#include "GameConfig.h"
#include "GameLogic.h"
#include "d3dUtil.h"
#include <string>

// Game.exe entry — play-only product (no editor UI).
// Game-specific C++ lives under Game/Source/ (this project), not Engine/Editor.
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*prevInstance*/,
                   PSTR cmdLine, int /*showCmd*/)
{
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        GameConfig::SetWorkingDirectoryToExe();

        GameConfig gameCfg;
        gameCfg.mode = GameConfig::Mode::Play;
        gameCfg.LoadFromExeDirectory();
        gameCfg.ApplyCommandLine(cmdLine);
        // Product is always play — ignore accidental mode=editor in cfg.
        gameCfg.mode = GameConfig::Mode::Play;

        {
            const DWORD attr = GetFileAttributesA("Resources");
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            {
                MessageBoxA(nullptr,
                    "Resources folder not found.\n\n"
                    "Run Play.bat from the export folder, or build with\n"
                    "working directory set to the project root.",
                    "Missing Resources", MB_OK | MB_ICONERROR);
                return 1;
            }
        }

        GameLogic::OnProcessStart();

        AppHost theApp(hInstance);
        theApp.SetGameConfig(gameCfg);
        if (!theApp.Initialize())
            return 0;

        const int code = theApp.Run();
        GameLogic::OnProcessExit();
        return code;
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "Exception", MB_OK | MB_ICONERROR);
        return 0;
    }
}

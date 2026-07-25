#pragma warning(disable: 4005)
#include "AppHost.h"
#include "GameConfig.h"
#include "d3dUtil.h"
#include <cstring>
#include <string>

// Editor.exe entry — always starts in Edit mode (unless --play for rare testing).
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
        gameCfg.mode = GameConfig::Mode::Editor;
        gameCfg.LoadFromExeDirectory();
        gameCfg.ApplyCommandLine(cmdLine);

        const bool cmdForcePlay =
            cmdLine && (strstr(cmdLine, "--play") || strstr(cmdLine, "-play"));

        // Product default: Editor. Only explicit --play switches this exe to play.
        if (!cmdForcePlay)
            gameCfg.mode = GameConfig::Mode::Editor;

        if (gameCfg.title.empty() || gameCfg.title == "OneMore Game")
            gameCfg.title = "OneMore Editor";

        AppHost theApp(hInstance);
        theApp.SetGameConfig(gameCfg);
        if (!theApp.Initialize())
            return 0;

        return theApp.Run();
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

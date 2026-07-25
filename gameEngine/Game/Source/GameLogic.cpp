#include "GameLogic.h"
#include <Windows.h>

namespace GameLogic
{
    void OnProcessStart()
    {
        OutputDebugStringA("[Game] GameLogic::OnProcessStart\n");
    }

    void OnProcessExit()
    {
        OutputDebugStringA("[Game] GameLogic::OnProcessExit\n");
    }
}

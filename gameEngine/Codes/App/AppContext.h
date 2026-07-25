#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include "Entity.h"
#include "GameConfig.h"

class Engine;
class ImGuiManager;

// Non-owning handles into the host app (InitDirect3DApp).
// Mode controllers read/write camera & client size through this.
struct AppContext
{
    HWND hwnd = nullptr;
    int* clientWidth = nullptr;
    int* clientHeight = nullptr;

    // Host viewport / scissor (editor ImGui present path).
    const D3D12_VIEWPORT* screenViewport = nullptr;
    const D3D12_RECT* scissorRect = nullptr;
    // Filled each Draw by host for editor OMSetRenderTargets.
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv{};

    Engine* engine = nullptr;
    ImGuiManager* imgui = nullptr;

    // Filled by the active mode each frame; read by host Draw.
    DirectX::XMFLOAT4X4* view = nullptr;
    DirectX::XMFLOAT4X4* proj = nullptr;
    DirectX::XMMATRIX* currentView = nullptr;
    DirectX::XMMATRIX* currentProj = nullptr;

    ECS::Entity* mainCamera = nullptr;

    const GameConfig* gameConfig = nullptr;
};

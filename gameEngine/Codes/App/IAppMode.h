#pragma once

#include <Windows.h>
#include <d3d12.h>
#include "GameTimer.h"
#include "SceneViewport.h"

struct AppContext;

// Play / Edit behaviour lives in separate IAppMode implementations.
// Host owns D3D + Engine + asset load; mode owns input, camera, UI, present path.
class IAppMode
{
public:
    virtual ~IAppMode() = default;

    // After assets + scene are ready, command list closed, fence flushed.
    // Host has already shown the main window for the selected mode.
    virtual void OnAfterInit(AppContext& ctx) = 0;

    // ImGui::NewFrame already called by host. dt is clamped.
    virtual void OnUpdate(AppContext& ctx, float dt) = 0;

    // Before command list reset / Scene RT ensure.
    virtual void OnBeginFrame(AppContext& ctx) = 0;

    // After 3D is drawn into SceneViewport. Mode presents to back buffer
    // (editor: ImGui panels, play: blit Scene RT).
    // backBuffer is the current swapchain color target (RENDER_TARGET state on entry).
    virtual void OnPresent(
        AppContext& ctx,
        ID3D12GraphicsCommandList* cmdList,
        SceneViewport& sceneVP,
        ID3D12Resource* backBuffer) = 0;

    virtual void OnResize(AppContext& ctx) = 0;

    // Return true if the message was fully handled (skip remaining mode-specific work).
    // Host still may forward to D3DApp::MsgProc unless noted.
    virtual bool OnMsg(AppContext& ctx, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) = 0;

    virtual void OnMouseDown(AppContext& ctx, WPARAM btnState, int x, int y) = 0;
    virtual void OnMouseUp(AppContext& ctx, WPARAM btnState, int x, int y) = 0;
    virtual void OnMouseMove(AppContext& ctx, WPARAM btnState, int x, int y) = 0;
    virtual void OnMouseWheel(AppContext& ctx, short wheelDelta, int x, int y) = 0;
    virtual void OnKeyDown(AppContext& ctx, WPARAM key) = 0;
    virtual void OnKeyUp(AppContext& ctx, WPARAM key) = 0;

    // Camera pose for ECS CameraComponent sync (host Draw).
    virtual void GetCameraPose(
        float& outX, float& outY, float& outZ,
        float& outPitch, float& outYaw) const = 0;

    virtual void OnDestroy(AppContext& ctx) = 0;
};

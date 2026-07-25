#pragma once

#include "IAppMode.h"
#include "MouseLookCapture.h"
#include <DirectXMath.h>

// Exported game / --play: no editor UI, full-window 3D, freefly only.
class PlayMode : public IAppMode
{
public:
    PlayMode() = default;
    ~PlayMode() override = default;

    void OnAfterInit(AppContext& ctx) override;
    void OnUpdate(AppContext& ctx, float dt) override;
    void OnBeginFrame(AppContext& ctx) override;
    void OnPresent(
        AppContext& ctx,
        ID3D12GraphicsCommandList* cmdList,
        SceneViewport& sceneVP,
        ID3D12Resource* backBuffer) override;
    void OnResize(AppContext& ctx) override;
    bool OnMsg(AppContext& ctx, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
    void OnMouseDown(AppContext& ctx, WPARAM btnState, int x, int y) override;
    void OnMouseUp(AppContext& ctx, WPARAM btnState, int x, int y) override;
    void OnMouseMove(AppContext& ctx, WPARAM btnState, int x, int y) override;
    void OnMouseWheel(AppContext& ctx, short wheelDelta, int x, int y) override;
    void OnKeyDown(AppContext& ctx, WPARAM key) override;
    void OnKeyUp(AppContext& ctx, WPARAM key) override;
    void GetCameraPose(
        float& outX, float& outY, float& outZ,
        float& outPitch, float& outYaw) const override;
    void OnDestroy(AppContext& ctx) override;

private:
    void UpdateCamera(AppContext& ctx, float dt);
    void SyncMouseLook(AppContext& ctx);
    void BindMouseLook(AppContext& ctx);
    void MatchSceneToClient(AppContext& ctx);

    MouseLookCapture mMouseLook;
    bool mMouseLookRequested = true;
    float mPendingMouseDx = 0.0f;
    float mPendingMouseDy = 0.0f;
    float mMouseSensitivity = 0.12f;

    float mTheta = 0.0f;
    float mPhi = 0.0f;
    float mCamX = 0.0f;
    float mCamY = 5.0f;
    float mCamZ = -10.0f;
    float mFlySpeed = 11.0f;

    bool mKeyW = false, mKeyS = false, mKeyA = false, mKeyD = false;
    bool mKeySpace = false, mKeyShift = false, mKeyCtrl = false;
};

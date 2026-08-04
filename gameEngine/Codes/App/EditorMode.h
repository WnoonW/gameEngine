#pragma once

#include "IAppMode.h"
#include "MouseLookCapture.h"
#include "ImGuiManager.h"
#include "Entity.h"
#include <DirectXMath.h>

using ECS::Entity;
using ECS::INVALID_ENTITY;

// Full editor: dock UI, picking, orbit manipulate, scene-panel mouse look.
class EditorMode : public IAppMode, public IFunctionCallback
{
public:
    EditorMode() = default;
    ~EditorMode() override = default;

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

    // ImGui toolbar / panel actions
    void buttonClicked(ButtonAction action) override;

    void SetCameraPose(float x, float y, float z, float pitch, float yaw)
    {
        mCamX = x; mCamY = y; mCamZ = z; mPhi = pitch; mTheta = yaw;
    }

private:
    void UpdateCamera(AppContext& ctx, float dt);
    void InitializeOrbitFromSelection(AppContext& ctx);
    void SyncMouseLook(AppContext& ctx);
    void BindMouseLook(AppContext& ctx);
    void ApplyOrbitZoomFromPixels(float dxPixels);
    void ApplyOrbitHeightFromPixels(float dyPixels);
    void EndRmbOrbitZoom(AppContext& ctx);
    bool IsMeshManipulateActive(AppContext& ctx) const;

    MouseLookCapture mMouseLook;
    bool mMouseLookRequested = false;
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
    bool mKeyQ = false, mKeyE = false, mKeyR = false, mKeyF = false;
    bool mKeySpace = false, mKeyShift = false, mKeyCtrl = false;

    bool mManipulateSelected = false;
    bool mManipulateUi = false;
    float mOrbitRadius = 10.0f;
    // World-Y crane offset (RMB drag U/D). Does not change orbit pitch/yaw.
    float mOrbitHeightOffset = 0.0f;
    Entity mOrbitTarget = INVALID_ENTITY;

    // Manipulate mesh: RMB drag L/R zooms, U/D world-Y height (no RMB pick).
    bool mRmbOrbitZoomActive = false;
    int mLastRmbZoomX = 0;
    int mLastRmbZoomY = 0;

    int mSpiralIndex = 0;

    // Cached for button callbacks (spawn in front of camera)
    AppContext* mCtx = nullptr;
};

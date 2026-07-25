#include "PlayMode.h"
#include "AppContext.h"
#include "FlyCameraMath.h"
#include "Engine.h"
#include "ImGuiManager.h"
#include "SceneViewport.h"
#include "d3dUtil.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <imgui.h>

using namespace DirectX;

void PlayMode::BindMouseLook(AppContext& ctx)
{
    // Full client area (no editor Scene panel).
    mMouseLook.Bind(ctx.hwnd, [hwnd = ctx.hwnd](RECT& out) -> bool
    {
        if (!hwnd)
            return false;
        RECT client{};
        GetClientRect(hwnd, &client);
        MapWindowPoints(hwnd, HWND_DESKTOP, reinterpret_cast<LPPOINT>(&client), 2);
        out = client;
        return true;
    });
}

void PlayMode::MatchSceneToClient(AppContext& ctx)
{
    if (!ctx.imgui || !ctx.clientWidth || !ctx.clientHeight)
        return;
    ctx.imgui->SetDesiredSceneSize(
        static_cast<UINT>((std::max)(1, *ctx.clientWidth)),
        static_cast<UINT>((std::max)(1, *ctx.clientHeight)));
}

void PlayMode::OnAfterInit(AppContext& ctx)
{
    BindMouseLook(ctx);

    if (ctx.imgui)
        ctx.imgui->SetPlayMode(true);

    if (ctx.gameConfig && !ctx.gameConfig->mouseLookOnStart)
        mMouseLookRequested = false;
    else
        mMouseLookRequested = true;

    if (mMouseLookRequested)
        mMouseLook.SetActive(true);

    UpdateCamera(ctx, 0.0f);
    SyncMouseLook(ctx);
}

void PlayMode::OnBeginFrame(AppContext& ctx)
{
    MatchSceneToClient(ctx);
}

void PlayMode::OnUpdate(AppContext& ctx, float dt)
{
    MatchSceneToClient(ctx);

    if (!mMouseLook.IsActive() && mMouseLookRequested)
        SyncMouseLook(ctx);

    UpdateCamera(ctx, dt);
    SyncMouseLook(ctx);
}

void PlayMode::OnPresent(
    AppContext& ctx,
    ID3D12GraphicsCommandList* cmdList,
    SceneViewport& sceneVP,
    ID3D12Resource* backBuffer)
{
    if (backBuffer
        && sceneVP.IsValid()
        && ctx.clientWidth && ctx.clientHeight
        && sceneVP.GetWidth() == static_cast<UINT>(*ctx.clientWidth)
        && sceneVP.GetHeight() == static_cast<UINT>(*ctx.clientHeight))
    {
        sceneVP.CopyColorTo(
            cmdList,
            backBuffer,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        // EndFrame expects RENDER_TARGET → PRESENT.
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_RENDER_TARGET));
    }

    // Close ImGui frame without drawing editor UI.
    ImGui::EndFrame();
}

void PlayMode::OnResize(AppContext& /*ctx*/)
{
    if (mMouseLook.IsActive())
        mMouseLook.RefreshPlacement();
}

bool PlayMode::OnMsg(AppContext& /*ctx*/, HWND /*hwnd*/, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // ESC: quit game
    if (msg == WM_KEYUP && wParam == VK_ESCAPE)
    {
        PostQuitMessage(0);
        return true;
    }

    // Anywhere LMB: re-enter mouse look
    if (msg == WM_LBUTTONDOWN)
    {
        mMouseLookRequested = true;
        mMouseLook.SetActive(true);
        return true;
    }

    if (mMouseLook.IsActive()
        && (msg == WM_MOVE || msg == WM_SIZE || msg == WM_EXITSIZEMOVE || msg == WM_DISPLAYCHANGE))
    {
        mMouseLook.RefreshPlacement();
    }

    if (msg == WM_ACTIVATE)
    {
        if (LOWORD(wParam) == WA_INACTIVE)
            mMouseLook.SetActive(false);
        else if (mMouseLookRequested)
            mMouseLook.SetActive(true);
        return false;
    }

    if (msg == WM_INPUT && mMouseLook.IsActive())
    {
        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size == 0)
            return true;

        std::vector<BYTE> data(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, data.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            return true;

        const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(data.data());
        if (raw->header.dwType == RIM_TYPEMOUSE
            && raw->data.mouse.usFlags == MOUSE_MOVE_RELATIVE)
        {
            const LONG mx = raw->data.mouse.lLastX;
            const LONG my = raw->data.mouse.lLastY;
            if (mx != 0 || my != 0)
            {
                mPendingMouseDx += XMConvertToRadians(mMouseSensitivity * static_cast<float>(mx));
                mPendingMouseDy += XMConvertToRadians(mMouseSensitivity * static_cast<float>(my));
            }
        }
        return true;
    }

    return false;
}

void PlayMode::OnMouseDown(AppContext& /*ctx*/, WPARAM /*btnState*/, int /*x*/, int /*y*/)
{
    // LMB handled in OnMsg for look re-entry. No picking in play mode.
}

void PlayMode::OnMouseUp(AppContext& /*ctx*/, WPARAM /*btnState*/, int /*x*/, int /*y*/)
{
}

void PlayMode::OnMouseMove(AppContext& /*ctx*/, WPARAM /*btnState*/, int /*x*/, int /*y*/)
{
}

void PlayMode::OnMouseWheel(AppContext& /*ctx*/, short wheelDelta, int /*x*/, int /*y*/)
{
    const float scroll = wheelDelta / 120.0f;
    mFlySpeed *= powf(1.2f, scroll);
    mFlySpeed = MathHelper::Clamp(mFlySpeed, 1.0f, 500.0f);
}

void PlayMode::OnKeyDown(AppContext& /*ctx*/, WPARAM wParam)
{
    switch (wParam)
    {
    case 'W': mKeyW = true; break;
    case 'S': mKeyS = true; break;
    case 'A': mKeyA = true; break;
    case 'D': mKeyD = true; break;
    case VK_SPACE: mKeySpace = true; break;
    case VK_SHIFT: mKeyShift = true; break;
    case VK_CONTROL: mKeyCtrl = true; break;
    }
}

void PlayMode::OnKeyUp(AppContext& /*ctx*/, WPARAM wParam)
{
    switch (wParam)
    {
    case 'W': mKeyW = false; break;
    case 'S': mKeyS = false; break;
    case 'A': mKeyA = false; break;
    case 'D': mKeyD = false; break;
    case VK_SPACE: mKeySpace = false; break;
    case VK_SHIFT: mKeyShift = false; break;
    case VK_CONTROL: mKeyCtrl = false; break;
    }
}

void PlayMode::GetCameraPose(
    float& outX, float& outY, float& outZ,
    float& outPitch, float& outYaw) const
{
    outX = mCamX;
    outY = mCamY;
    outZ = mCamZ;
    outPitch = mPhi;
    outYaw = mTheta;
}

void PlayMode::OnDestroy(AppContext& /*ctx*/)
{
    mMouseLook.SetActive(false);
}

void PlayMode::SyncMouseLook(AppContext& ctx)
{
    const bool wantActive = mMouseLookRequested
        && ctx.hwnd
        && GetForegroundWindow() == ctx.hwnd;
    mMouseLook.SetActive(wantActive);
}

void PlayMode::UpdateCamera(AppContext& ctx, float dt)
{
    if (!ctx.view || !ctx.proj || !ctx.currentView || !ctx.currentProj)
        return;

    if (mPendingMouseDx != 0.0f || mPendingMouseDy != 0.0f)
    {
        mTheta += mPendingMouseDx;
        mPhi += mPendingMouseDy;
        mPendingMouseDx = 0.0f;
        mPendingMouseDy = 0.0f;
    }

    mPhi = MathHelper::Clamp(mPhi, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);

    XMMATRIX yawRot = XMMatrixRotationY(mTheta);
    XMMATRIX rot = XMMatrixRotationRollPitchYaw(mPhi, mTheta, 0.0f);
    XMVECTOR lookForward = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rot);
    XMVECTOR horizRight = XMVector3TransformNormal(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), yawRot);
    XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    float speed = mFlySpeed * dt;
    if (mKeyCtrl)
        speed *= 2.0f;

    const auto axes = FlyCameraMath::BuildNormalizedAxes(
        mKeyW, mKeyS, mKeyA, mKeyD, mKeySpace, mKeyShift);

    XMVECTOR horizForward = FlyCameraMath::FlattenHorizForward(lookForward, yawRot);

    XMVECTOR pos = XMVectorSet(mCamX, mCamY, mCamZ, 1.0f);
    if (axes.fwd != 0.0f || axes.strafe != 0.0f || axes.ascend != 0.0f)
    {
        pos = XMVectorAdd(pos, XMVectorScale(horizForward, axes.fwd * speed));
        pos = XMVectorAdd(pos, XMVectorScale(horizRight, axes.strafe * speed));
        pos = XMVectorAdd(pos, XMVectorScale(worldUp, axes.ascend * speed));

        XMFLOAT3 p{};
        XMStoreFloat3(&p, pos);
        mCamX = p.x;
        mCamY = p.y;
        mCamZ = p.z;
    }

    lookForward = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rot);
    pos = XMVectorSet(mCamX, mCamY, mCamZ, 1.0f);
    XMMATRIX view = XMMatrixLookToLH(pos, lookForward, worldUp);
    XMStoreFloat4x4(ctx.view, view);
    *ctx.currentView = view;
    *ctx.currentProj = XMLoadFloat4x4(ctx.proj);
}

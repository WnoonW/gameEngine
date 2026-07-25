#include "EditorMode.h"
#include "AppContext.h"
#include "FlyCameraMath.h"
#include "Engine.h"
#include "ImGuiManager.h"
#include "SceneViewport.h"
#include "ComponentStruct.h"
#include "d3dUtil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <imgui.h>

using namespace DirectX;

namespace
{
    constexpr float kMinOrbitRadius = 2.0f;
    constexpr float kMaxOrbitRadius = 80.0f;
    constexpr float kDefaultOrbitPitch = -0.3f;
}

void EditorMode::BindMouseLook(AppContext& ctx)
{
    mMouseLook.Bind(ctx.hwnd, [imgui = ctx.imgui, hwnd = ctx.hwnd](RECT& out) -> bool
    {
        if (imgui && imgui->TryGetSceneScreenRect(out))
            return true;
        if (!hwnd)
            return false;
        RECT client{};
        GetClientRect(hwnd, &client);
        MapWindowPoints(hwnd, HWND_DESKTOP, reinterpret_cast<LPPOINT>(&client), 2);
        out = client;
        return true;
    });
}

void EditorMode::OnAfterInit(AppContext& ctx)
{
    mCtx = &ctx;
    BindMouseLook(ctx);

    if (ctx.imgui)
        ctx.imgui->SetPlayMode(false);

    UpdateCamera(ctx, 0.0f);
    SyncMouseLook(ctx);
}

void EditorMode::OnBeginFrame(AppContext& /*ctx*/)
{
    // Scene RT size is driven by ImGui Scene panel (host EnsureSceneViewport).
}

void EditorMode::OnUpdate(AppContext& ctx, float dt)
{
    mCtx = &ctx;
    if (!ctx.imgui || !ctx.engine)
        return;

    ctx.imgui->SetupDockspace(ctx.engine);
    ctx.imgui->DrawEditorPanels(ctx.engine);

    mManipulateSelected = ctx.imgui->IsManipulateSelected();

    // Scene drag box multi-select (Shift = additive)
    {
        float bx0, by0, bx1, by1;
        bool additive = false;
        if (ctx.imgui->ConsumeBoxSelection(bx0, by0, bx1, by1, additive))
        {
            const SceneViewport& sceneVP = ctx.imgui->GetSceneViewport();
            const float pickW = sceneVP.IsValid()
                ? static_cast<float>(sceneVP.GetWidth())
                : static_cast<float>(ctx.imgui->GetDesiredSceneWidth());
            const float pickH = sceneVP.IsValid()
                ? static_cast<float>(sceneVP.GetHeight())
                : static_cast<float>(ctx.imgui->GetDesiredSceneHeight());

            const float uiW = static_cast<float>((std::max)(1u, ctx.imgui->GetDesiredSceneWidth()));
            const float uiH = static_cast<float>((std::max)(1u, ctx.imgui->GetDesiredSceneHeight()));
            const float sx = pickW / uiW;
            const float sy = pickH / uiH;

            const size_t n = ctx.engine->SelectObjectsInRect(
                bx0 * sx, by0 * sy, bx1 * sx, by1 * sy,
                pickW, pickH,
                *ctx.currentView, *ctx.currentProj,
                additive);

            char buf[96];
            sprintf_s(buf, "[Select] box select count=%zu additive=%d\n", n, additive ? 1 : 0);
            OutputDebugStringA(buf);
        }
    }

    // Short LMB on Scene → mouse look
    if (ctx.imgui->ConsumeSceneCaptureClick())
    {
        mMouseLookRequested = true;
        SyncMouseLook(ctx);
    }

    UpdateCamera(ctx, dt);
    SyncMouseLook(ctx);
}

void EditorMode::OnPresent(
    AppContext& ctx,
    ID3D12GraphicsCommandList* cmdList,
    SceneViewport& /*sceneVP*/,
    ID3D12Resource* /*backBuffer*/)
{
    if (!ctx.imgui || !cmdList)
        return;

    // Bind back buffer + full-window viewport for ImGui.
    if (ctx.screenViewport && ctx.scissorRect)
    {
        cmdList->OMSetRenderTargets(1, &ctx.backBufferRtv, TRUE, nullptr);
        cmdList->RSSetViewports(1, ctx.screenViewport);
        cmdList->RSSetScissorRects(1, ctx.scissorRect);
    }

    ctx.imgui->Render(cmdList);
}

void EditorMode::OnResize(AppContext& /*ctx*/)
{
    if (mMouseLook.IsActive())
        mMouseLook.RefreshPlacement();
}

bool EditorMode::OnMsg(AppContext& /*ctx*/, HWND /*hwnd*/, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // ESC: release mouse look (do not quit)
    if (msg == WM_KEYUP && wParam == VK_ESCAPE)
    {
        if (mMouseLookRequested || mMouseLook.IsActive())
        {
            mMouseLookRequested = false;
            mMouseLook.SetActive(false);
        }
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
        return false; // let D3DApp also handle pause
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

void EditorMode::OnMouseDown(AppContext& ctx, WPARAM btnState, int x, int y)
{
    if (!(btnState & MK_RBUTTON) || !ctx.imgui || !ctx.engine)
        return;

    if (!mMouseLook.IsActive() && ImGui::GetIO().WantCaptureMouse)
        return;

    const SceneViewport& sceneVP = ctx.imgui->GetSceneViewport();
    const int pickW = sceneVP.IsValid()
        ? static_cast<int>(sceneVP.GetWidth())
        : (ctx.clientWidth ? *ctx.clientWidth : 1);
    const int pickH = sceneVP.IsValid()
        ? static_cast<int>(sceneVP.GetHeight())
        : (ctx.clientHeight ? *ctx.clientHeight : 1);

    int pickX = x;
    int pickY = y;
    if (mMouseLook.IsActive())
    {
        pickX = pickW / 2;
        pickY = pickH / 2;
    }
    else
    {
        RECT sceneScreen{};
        if (ctx.imgui->TryGetSceneScreenRect(sceneScreen))
        {
            POINT pt{ x, y };
            ClientToScreen(ctx.hwnd, &pt);
            pickX = pt.x - sceneScreen.left;
            pickY = pt.y - sceneScreen.top;
            if (pickX < 0 || pickY < 0 || pickX >= pickW || pickY >= pickH)
                return;
        }
    }

    const bool additive = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    Entity picked = ctx.engine->PickObject(
        pickX, pickY, static_cast<float>(pickW), static_cast<float>(pickH),
        *ctx.currentView, *ctx.currentProj,
        true, additive);
    if (picked != INVALID_ENTITY)
    {
        OutputDebugStringA("Object picked!\n");
        if (mManipulateSelected)
            InitializeOrbitFromSelection(ctx);
    }
}

void EditorMode::OnMouseUp(AppContext& /*ctx*/, WPARAM /*btnState*/, int /*x*/, int /*y*/)
{
}

void EditorMode::OnMouseMove(AppContext& /*ctx*/, WPARAM /*btnState*/, int /*x*/, int /*y*/)
{
}

void EditorMode::OnMouseWheel(AppContext& ctx, short wheelDelta, int /*x*/, int /*y*/)
{
    const float factor = 1.1f;
    const float scroll = wheelDelta / 120.0f;

    if (mManipulateSelected && ctx.engine && ctx.engine->GetSelectedEntity() != INVALID_ENTITY)
    {
        mOrbitRadius *= powf(factor, scroll);
        mOrbitRadius = MathHelper::Clamp(mOrbitRadius, kMinOrbitRadius, kMaxOrbitRadius);
    }
    else
    {
        mFlySpeed *= powf(1.2f, scroll);
        mFlySpeed = MathHelper::Clamp(mFlySpeed, 1.0f, 500.0f);
    }
}

void EditorMode::OnKeyDown(AppContext& ctx, WPARAM wParam)
{
    switch (wParam)
    {
    case 'W': mKeyW = true; break;
    case 'S': mKeyS = true; break;
    case 'A': mKeyA = true; break;
    case 'D': mKeyD = true; break;
    case 'Q': mKeyQ = true; break;
    case 'E': mKeyE = true; break;
    case 'R': mKeyR = true; break;
    case 'F': mKeyF = true; break;
    case VK_SPACE: mKeySpace = true; break;
    case VK_SHIFT: mKeyShift = true; break;
    case VK_CONTROL: mKeyCtrl = true; break;
    }

    // Editor-only: spiral spawn test objects
    if (wParam == VK_UP && ctx.engine)
    {
        const float idx = static_cast<float>(mSpiralIndex);
        constexpr float angleStep = 0.1f;
        constexpr float radiusStep = 0.1f;
        const float angle = idx * angleStep;
        const float radius = idx * radiusStep;
        const XMFLOAT3 pos = {
            radius * std::cos(angle),
            0.0f,
            radius * std::sin(angle)
        };
        ctx.engine->CreateRenderableEntity("bibian", "", pos);
        ++mSpiralIndex;
    }
}

void EditorMode::OnKeyUp(AppContext& /*ctx*/, WPARAM wParam)
{
    switch (wParam)
    {
    case 'W': mKeyW = false; break;
    case 'S': mKeyS = false; break;
    case 'A': mKeyA = false; break;
    case 'D': mKeyD = false; break;
    case 'Q': mKeyQ = false; break;
    case 'E': mKeyE = false; break;
    case 'R': mKeyR = false; break;
    case 'F': mKeyF = false; break;
    case VK_SPACE: mKeySpace = false; break;
    case VK_SHIFT: mKeyShift = false; break;
    case VK_CONTROL: mKeyCtrl = false; break;
    }
}

void EditorMode::GetCameraPose(
    float& outX, float& outY, float& outZ,
    float& outPitch, float& outYaw) const
{
    outX = mCamX;
    outY = mCamY;
    outZ = mCamZ;
    outPitch = mPhi;
    outYaw = mTheta;
}

void EditorMode::OnDestroy(AppContext& /*ctx*/)
{
    mMouseLook.SetActive(false);
    mCtx = nullptr;
}

void EditorMode::SyncMouseLook(AppContext& ctx)
{
    const bool wantActive = mMouseLookRequested
        && ctx.hwnd
        && GetForegroundWindow() == ctx.hwnd;
    mMouseLook.SetActive(wantActive);
}

void EditorMode::InitializeOrbitFromSelection(AppContext& ctx)
{
    if (!ctx.engine)
        return;

    const Entity selected = ctx.engine->GetSelectedEntity();
    TransformComponent* tf = ctx.engine->GetTransform(selected);
    if (!tf)
    {
        mOrbitTarget = INVALID_ENTITY;
        return;
    }

    mOrbitTarget = selected;

    XMVECTOR pivot = XMLoadFloat3(&tf->position);
    XMVECTOR camPos = XMVectorSet(mCamX, mCamY, mCamZ, 1.0f);
    XMVECTOR toCam = XMVectorSubtract(camPos, pivot);
    const float dist = XMVectorGetX(XMVector3Length(toCam));

    if (dist > 0.001f)
    {
        mOrbitRadius = MathHelper::Clamp(dist, kMinOrbitRadius, kMaxOrbitRadius);
        XMVECTOR offsetDir = XMVector3Normalize(toCam);
        XMFLOAT3 o{};
        XMStoreFloat3(&o, offsetDir);
        mPhi = asinf(MathHelper::Clamp(o.y, -1.0f, 1.0f));
        mTheta = atan2f(o.x, o.z) - tf->rotation.y;
    }
    else
    {
        mTheta = 0.0f;
        mPhi = kDefaultOrbitPitch;
        mOrbitRadius = kMinOrbitRadius;
    }
}

void EditorMode::UpdateCamera(AppContext& ctx, float dt)
{
    if (!ctx.engine || !ctx.view || !ctx.proj || !ctx.currentView || !ctx.currentProj)
        return;

    const Entity selected = ctx.engine->GetSelectedEntity();
    const bool thirdPersonMode = mManipulateSelected && selected != INVALID_ENTITY;

    if (thirdPersonMode && selected != mOrbitTarget)
        InitializeOrbitFromSelection(ctx);
    else if (!thirdPersonMode)
        mOrbitTarget = INVALID_ENTITY;

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
    XMFLOAT3 horizFwd{};
    XMFLOAT3 horizRgt{};
    XMStoreFloat3(&horizFwd, horizForward);
    XMStoreFloat3(&horizRgt, horizRight);

    if (thirdPersonMode)
    {
        TransformComponent* targetTf = ctx.engine->GetTransform(selected);
        if (!targetTf)
        {
            mOrbitTarget = INVALID_ENTITY;
            ctx.engine->ClearSelection();

            XMVECTOR pos = XMVectorSet(mCamX, mCamY, mCamZ, 1.0f);
            XMMATRIX view = XMMatrixLookToLH(pos, lookForward, worldUp);
            XMStoreFloat4x4(ctx.view, view);
            *ctx.currentView = view;
            *ctx.currentProj = XMLoadFloat4x4(ctx.proj);
            return;
        }

        if (axes.fwd != 0.0f || axes.strafe != 0.0f || axes.ascend != 0.0f)
            ctx.engine->MoveSelectedPlanar(
                axes.fwd, axes.strafe, axes.ascend, speed, horizFwd, horizRgt);

        XMVECTOR pivot = XMLoadFloat3(&targetTf->position);
        XMVECTOR camPos = FlyCameraMath::ComputeThirdPersonCameraPosition(
            pivot, targetTf->rotation.y, mPhi, mTheta, mOrbitRadius);

        XMMATRIX view = FlyCameraMath::BuildOrbitView(
            camPos, pivot, worldUp, mCamX, mCamY, mCamZ);
        XMStoreFloat4x4(ctx.view, view);
        *ctx.currentView = view;
        *ctx.currentProj = XMLoadFloat4x4(ctx.proj);
        return;
    }

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

void EditorMode::buttonClicked(ButtonAction action)
{
    if (!mCtx || !mCtx->engine || !mCtx->imgui)
        return;

    AppContext& ctx = *mCtx;

    if (action == ButtonAction::SpawnTestObject)
    {
        ctx.engine->CreateRenderableEntity("bibian", "", { 0, 1, 0 });
    }
    else if (action == ButtonAction::ToggleManipulateSelected)
    {
        mManipulateSelected = ctx.imgui->IsManipulateSelected();
        if (mManipulateSelected && ctx.engine->GetSelectedEntity() != INVALID_ENTITY)
            InitializeOrbitFromSelection(ctx);
        else
            mOrbitTarget = INVALID_ENTITY;
    }
    else if (action == ButtonAction::SpawnSelectedMesh)
    {
        std::string mesh = ctx.imgui->GetSelectedMesh();
        std::string mat = ctx.imgui->GetSelectedMaterial();
        if (mesh.empty())
            mesh = "bibian";

        constexpr float kSpawnDistance = 8.0f;
        XMMATRIX view = *ctx.currentView;
        XMVECTOR det;
        XMMATRIX invView = XMMatrixInverse(&det, view);
        XMVECTOR camPos = XMVector3TransformCoord(XMVectorZero(), invView);
        XMVECTOR lookDir = XMVector3Normalize(
            XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), invView));
        XMVECTOR spawnVec = XMVectorAdd(camPos, XMVectorScale(lookDir, kSpawnDistance));

        XMFLOAT3 spawnPos{};
        XMStoreFloat3(&spawnPos, spawnVec);
        ctx.engine->CreateRenderableEntity(mesh, mat, spawnPos);
    }
}

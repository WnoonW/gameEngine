#include "MouseLookCapture.h"
#include <imgui.h>

void MouseLookCapture::Bind(HWND hwnd, GetClipScreenRectFn getClipRect)
{
    mHwnd = hwnd;
    mGetClipRect = std::move(getClipRect);
}

void MouseLookCapture::SetActive(bool active)
{
    if (mActive == active)
    {
        if (active)
            RefreshPlacement();
        return;
    }

    mActive = active;
    ImGuiIO& io = ImGui::GetIO();
    if (active)
    {
        // Prevent ImGui from eating mouse while look is on (dock drag / panel focus).
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;

        while (ShowCursor(FALSE) >= 0) {}
        if (mHwnd)
            SetCapture(mHwnd);
        RefreshPlacement();
    }
    else
    {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

        while (ShowCursor(TRUE) < 0) {}
        ReleaseCapture();
        ClipCursor(nullptr);
    }
}

void MouseLookCapture::RefreshPlacement()
{
    if (!mActive)
        return;
    UpdateClip();
    CenterCursor();
}

void MouseLookCapture::CenterCursor()
{
    if (!mHwnd)
        return;

    RECT clip{};
    if (mGetClipRect && mGetClipRect(clip))
    {
        const POINT center{
            (clip.left + clip.right) / 2,
            (clip.top + clip.bottom) / 2
        };
        SetCursorPos(center.x, center.y);
        return;
    }

    RECT client{};
    GetClientRect(mHwnd, &client);
    POINT center{
        (client.right - client.left) / 2,
        (client.bottom - client.top) / 2
    };
    ClientToScreen(mHwnd, &center);
    SetCursorPos(center.x, center.y);
}

void MouseLookCapture::UpdateClip()
{
    if (!mHwnd)
        return;

    RECT clip{};
    if (mGetClipRect && mGetClipRect(clip))
    {
        ClipCursor(&clip);
        return;
    }

    RECT rect{};
    GetClientRect(mHwnd, &rect);
    MapWindowPoints(mHwnd, HWND_DESKTOP, reinterpret_cast<LPPOINT>(&rect), 2);
    ClipCursor(&rect);
}

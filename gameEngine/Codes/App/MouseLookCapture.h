#pragma once

#include <Windows.h>
#include <functional>

// Captures / releases the cursor for FPS-style look.
// Clip rect is provided by the mode (editor Scene panel vs play full client).
class MouseLookCapture
{
public:
    using GetClipScreenRectFn = std::function<bool(RECT& outScreenRect)>;

    void Bind(HWND hwnd, GetClipScreenRectFn getClipRect);

    bool IsActive() const { return mActive; }
    void SetActive(bool active);
    void RefreshPlacement(); // re-clip + re-center (move/resize)

private:
    void CenterCursor();
    void UpdateClip();

    HWND mHwnd = nullptr;
    GetClipScreenRectFn mGetClipRect;
    bool mActive = false;
};

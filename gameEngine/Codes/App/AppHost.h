#pragma once

#include <memory>
#include <string>
#include <DirectXMath.h>
#include "d3dApp.h"
#include "DescriptorAllocator.h"
#include "ImGuiManager.h"
#include "GameConfig.h"
#include "Engine.h"
#include "Entity.h"
#include "AppContext.h"
#include "IAppMode.h"

// Shared D3D + Engine host used by Editor.exe and Game.exe.
// Mode (edit vs play) is selected via GameConfig before Initialize().
class AppHost : public D3DApp
{
public:
    explicit AppHost(HINSTANCE hInstance);
    ~AppHost() override;

    bool Initialize() override;
    void SetGameConfig(const GameConfig& cfg);
    bool IsPlayMode() const { return mPlayMode; }

private:
    static DescriptorAllocator mGlobalDescriptorAllocator;
    ImGuiManager mImGuiManager;
    Engine mEngine;

    void OnResize() override;
    LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
    void Update(const GameTimer& gt) override;
    void Draw(const GameTimer& gt) override;
    void BeginFrame() override;
    void EndFrame() override;
    void OnDestroy() override;

    void OnMouseDown(WPARAM btnState, int x, int y) override;
    void OnMouseUp(WPARAM btnState, int x, int y) override;
    void OnMouseMove(WPARAM btnState, int x, int y) override;
    void OnMouseWheel(short wheelDelta, int x, int y) override;
    void OnKeyDown(WPARAM key) override;
    void OnKeyUp(WPARAM key) override;

    void InitializeCoreSystems();
    void LoadAssets();
    void CreateInitialScene();
    void RegisterMouseRawInput();
    void BuildAppContext();
    void CreateModeController();

    AppContext mCtx{};
    std::unique_ptr<IAppMode> mMode;

    DirectX::XMFLOAT4X4 mView = {};
    DirectX::XMFLOAT4X4 mProj = {};
    DirectX::XMMATRIX mCurrentView = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX mCurrentProj = DirectX::XMMatrixIdentity();

    ECS::Entity mMainCamera = ECS::INVALID_ENTITY;

    bool mPlayMode = false;
    GameConfig mGameConfig{};
    std::string mPendingSceneLoad;
};

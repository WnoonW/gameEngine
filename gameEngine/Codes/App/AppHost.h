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
// Editor also supports A (standalone Game.exe) and B (in-editor play session).
class AppHost : public D3DApp, public IEditorHost
{
public:
    explicit AppHost(HINSTANCE hInstance);
    ~AppHost() override;

    bool Initialize() override;
    void SetGameConfig(const GameConfig& cfg);
    bool IsPlayMode() const { return mPlayMode; }

    // IEditorHost
    bool PlayInEditor() override;
    void StopInEditorPlay() override;
    bool IsInEditorPlaying() const override
    {
        return mInEditorPlaying || mPendingStartInEditorPlay;
    }
    bool PlayStandalone() override;
    void StopStandalone() override;
    bool IsStandaloneRunning() const override;

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
    void ApplyDeferredSessionActions();
    bool SavePlaySnapshot(const std::string& absolutePath, std::string* outError = nullptr);
    bool RestorePlaySnapshot(const std::string& absolutePath, std::string* outError = nullptr);
    static std::string MakeScenesTempPath(const char* fileName);
    void CaptureModeCamera();
    void CloseStandaloneProcess(bool terminate);

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

    // B: in-editor play session (mode swap is always deferred — never mid-OnUpdate)
    bool mInEditorPlaying = false;
    bool mPendingStartInEditorPlay = false;
    bool mPendingStopInEditorPlay = false;
    std::string mInEditorSnapshotPath;
    float mSnapCamX = 0.0f, mSnapCamY = 5.0f, mSnapCamZ = -10.0f;
    float mSnapCamPitch = 0.0f, mSnapCamYaw = 0.0f;

    // A: standalone Game.exe child
    HANDLE mStandaloneProcess = nullptr;
};

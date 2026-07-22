#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <Windows.h>

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

#include <Backends/imgui_impl_win32.h>
#include <Backends/imgui_impl_dx12.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "DescriptorAllocator.h"
#include "SceneViewport.h"

enum class ButtonAction
{
    None,
    ResetScene,
    SpawnTestObject,
    ToggleWireframe,
    ReloadShaders,
    PrintECSStats,
    ToggleManipulateSelected,
    SpawnSelectedMesh,
    // ... 필요할 때마다 추가
};

class IFunctionCallback
{
public:
	virtual void buttonClicked(ButtonAction action) = 0;
};


class Engine;

class ImGuiManager
{
public:
    bool Initialize(HWND hwnd,
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        UINT numFramesInFlight,
        DXGI_FORMAT rtvFormat,
        DXGI_FORMAT depthFormat,
        DescriptorAllocator& globalDescriptorAllocator,
        IFunctionCallback* callback = nullptr);

    // for object creation from loaded assets
    const std::string& GetSelectedMesh() const { return mSelectedMesh; }
    const std::string& GetSelectedMaterial() const { return mSelectedMaterial; }

    void Shutdown();

    // 매 프레임 호출 (Update 또는 Render 시작 부분에서)
    void NewFrame();

    // ImGui 창을 다 그린 후에 호출 (CommandList에 실제 그리기)
    void Render(ID3D12GraphicsCommandList* cmdList);

	// ImGui Dockspace + 메인 메뉴
    void SetupDockspace(Engine* engine = nullptr);

    // 표시 중인 에디터 패널 일괄 그리기
    void DrawEditorPanels(Engine* engine = nullptr);

    void DrawScenePanel();
    void DrawHierarchyPanel(Engine* engine = nullptr);
    void DrawInspectorPanel(Engine* engine = nullptr);
    void DrawToolsPanel();
    void DrawProjectPanel();
    void DrawRenderPanel(Engine* engine = nullptr);
    void DrawHelpPanel();

    // Ensure offscreen Scene RT matches last panel size.
    // flushGpu must fully idle the GPU before resources are recreated.
    void EnsureSceneViewport(const std::function<void()>& flushGpu);

    SceneViewport& GetSceneViewport() { return mSceneViewport; }
    const SceneViewport& GetSceneViewport() const { return mSceneViewport; }

    UINT GetDesiredSceneWidth() const { return mDesiredSceneWidth; }
    UINT GetDesiredSceneHeight() const { return mDesiredSceneHeight; }
    void SetDesiredSceneSize(UINT width, UINT height)
    {
        mDesiredSceneWidth = width == 0 ? 1u : width;
        mDesiredSceneHeight = height == 0 ? 1u : height;
    }

    // true: skip editor dock UI (play / exported game)
    void SetPlayMode(bool play) { mPlayMode = play; }
    bool IsPlayMode() const { return mPlayMode; }

    // Scene 패널(Image) 클릭/영역 정보 — 카메라 마우스 고정 진입에 사용
    bool ConsumeSceneCaptureClick();
    bool IsSceneHovered() const { return mSceneHovered; }
    // 호출 시점에 HWND 기준으로 스크린 RECT를 계산한다 (창 이동 반영).
    bool TryGetSceneScreenRect(RECT& outRect) const;
    bool TryGetSceneClientRect(RECT& outRect) const;

    // Scene 드래그 박스 선택 결과 (Scene 이미지 로컬 픽셀). 한 번 읽으면 소비.
    // additive: Shift 누른 채 드래그
    bool ConsumeBoxSelection(float& outMinX, float& outMinY, float& outMaxX, float& outMaxY, bool& outAdditive);

    void SetManipulateSelected(bool on) { mManipulateSelected = on; }
    bool IsManipulateSelected() const { return mManipulateSelected; }

    // 커맨드 리스트가 닫힌 뒤 호출 (초기화 완료 후 창 위치 복원)
    void ApplyMainWindowPlacement();
    // 위치 복원·리사이즈 후 메인 창을 화면에 표시 (최대화 상태 포함)
    void PresentMainWindow();

private:
    void DrawMainMenuBar(Engine* engine);
    void ApplyDefaultDockLayout(ImGuiID dockspace_id, const ImVec2& workSize);
    void CaptureDockSplitRatios(ImGuiDockNode* node);
    void ApplyDockSplitRatios(ImGuiDockNode* node, ImVec2 size);
    void SyncDockLayoutToWorkSize(ImGuiID dockspace_id, const ImVec2& workSize);
    void DrawProjectSpawnContent();
    void DrawRenderContent(Engine* engine);
    void DrawHelpContent();
    void DrawToolsContent();

    // UI 설정 저장/불러오기 (exe 옆 editor_ui.cfg + imgui.ini)
    void ResolveConfigPaths();
    bool LoadUiSettings();
    bool SaveUiSettings();
    void MarkUiSettingsDirty();
    void AutosaveUiSettingsIfNeeded();
    void CaptureMainWindowPlacement();
    static bool IsPlacementOnScreen(const RECT& rc);

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
    ID3D12Device* m_Device = nullptr;
    HWND m_Hwnd = nullptr;
    IFunctionCallback* m_Callback = nullptr;
    DescriptorAllocator* m_DescriptorAllocator = nullptr;

    bool mManipulateSelected = false;
    bool mPlayMode = false;

    // View 메뉴로 열고 닫는 패널 (X로 닫으면 꺼짐, View에서 다시 켬)
    bool mShowScene = true;
    bool mShowHierarchy = true;
    bool mShowInspector = true;
    bool mShowTools = true;
    bool mShowProject = true;
    bool mShowRender = true;
    bool mShowHelp = true;
    bool mRequestResetLayout = false;

    // 레이아웃 버전을 올리면(코드 변경 시) 기본 도크 배치를 다시 깐다.
    static constexpr int kDockLayoutVersion = 5;
    static constexpr int kUiSettingsFileVersion = 1;
    int mAppliedDockLayoutVersion = 0;
    ImVec2 mLastDockWorkSize{ 0.0f, 0.0f };
    float mLastDockAspect = 0.0f; // workSize.x / workSize.y

    // 사용자가 조절한 도크 스플릿 비율 (nodeId → Child[0] 비율 0~1).
    // - SplitAxis X(좌우): 현재 창 너비의 %
    // - SplitAxis Y(상하): 현재 창 높이의 %
    // 창이 1:1 → 16:9 로 바뀌면 W/H가 다르게 변하므로 축별로 따로 환산한다.
    std::unordered_map<ImGuiID, float> mDockSplitRatios;
    bool mDockRatiosNeedSeed = true;

    // true면 시작 시 기본 레이아웃을 덮어쓰지 않고 저장된 도크/비율을 사용
    bool mRestoreDockFromSettings = false;
    bool mUiSettingsDirty = false;
    double mLastUiSettingsSaveTime = 0.0;

    // exe 디렉터리 기준 설정 경로 (ImGui io.IniFilename 은 수명 동안 유효해야 함)
    char mImGuiIniPath[MAX_PATH] = {};
    char mEditorCfgPath[MAX_PATH] = {};

    // 메인 HWND 위치/크기 (WINDOWPLACEMENT 기준, 복원 좌표)
    bool mHasSavedWindowPlacement = false;
    int mWindowNormalLeft = 0;
    int mWindowNormalTop = 0;
    int mWindowNormalRight = 0;
    int mWindowNormalBottom = 0;
    int mWindowShowCmd = SW_SHOWNORMAL; // SW_SHOWNORMAL / SW_SHOWMAXIMIZED

    std::string mSelectedMesh;
    std::string mSelectedMaterial;

    // 씬 파일 IO 상태
    std::string mLastScenePath;
    std::string mSceneStatus;
    bool mSceneStatusIsError = false;

    SceneViewport mSceneViewport;
    UINT mDesiredSceneWidth = 1;
    UINT mDesiredSceneHeight = 1;
    DXGI_FORMAT mDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    // DrawScenePanel에서 갱신. 클라이언트 좌표로 저장해 창 이동 시 재변환한다.
    bool mSceneHovered = false;
    bool mSceneCaptureClick = false;
    bool mSceneClientRectValid = false;
    float mSceneClientMinX = 0.0f;
    float mSceneClientMinY = 0.0f;
    float mSceneClientMaxX = 0.0f;
    float mSceneClientMaxY = 0.0f;

    // LMB 드래그 박스 선택 (Scene 이미지 로컬 좌표)
    bool mBoxDragging = false;
    bool mBoxSelectPending = false;
    bool mBoxSelectAdditive = false;
    ImVec2 mBoxStartScreen{ 0, 0 };
    ImVec2 mBoxEndScreen{ 0, 0 };
    float mBoxResultMinX = 0, mBoxResultMinY = 0;
    float mBoxResultMaxX = 0, mBoxResultMaxY = 0;
    static constexpr float kBoxDragThresholdPx = 4.0f;
};
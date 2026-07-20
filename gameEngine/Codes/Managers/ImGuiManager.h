#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <Windows.h>

#include <string>
#include <vector>
#include <functional>

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

    //void CustomUI(Engine* engine = nullptr);

    // for object creation from loaded assets
    const std::string& GetSelectedMesh() const { return mSelectedMesh; }
    const std::string& GetSelectedMaterial() const { return mSelectedMaterial; }

    void Shutdown();

    // 매 프레임 호출 (Update 또는 Render 시작 부분에서)
    void NewFrame();

    // ImGui 창을 다 그린 후에 호출 (CommandList에 실제 그리기)
    void Render(ID3D12GraphicsCommandList* cmdList);

	// ImGui Dockspace 설정
    void SetupDockspace();          
    void DrawScenePanel();
    void DrawHierarchyPanel(Engine* engine = nullptr);
    void DrawInspectorPanel(Engine* engine = nullptr);
    void DrawProjectPanel();

    // Ensure offscreen Scene RT matches last panel size.
    // flushGpu must fully idle the GPU before resources are recreated.
    void EnsureSceneViewport(const std::function<void()>& flushGpu);

    SceneViewport& GetSceneViewport() { return mSceneViewport; }
    const SceneViewport& GetSceneViewport() const { return mSceneViewport; }

    UINT GetDesiredSceneWidth() const { return mDesiredSceneWidth; }
    UINT GetDesiredSceneHeight() const { return mDesiredSceneHeight; }

    // Scene 패널(Image) 클릭/영역 정보 — 카메라 마우스 고정 진입에 사용
    bool ConsumeSceneCaptureClick();
    bool IsSceneHovered() const { return mSceneHovered; }
    // 호출 시점에 HWND 기준으로 스크린 RECT를 계산한다 (창 이동 반영).
    bool TryGetSceneScreenRect(RECT& outRect) const;
    bool TryGetSceneClientRect(RECT& outRect) const;

    void SetManipulateSelected(bool on) { mManipulateSelected = on; }
    bool IsManipulateSelected() const { return mManipulateSelected; }

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
    ID3D12Device* m_Device = nullptr;
    HWND m_Hwnd = nullptr;
    IFunctionCallback* m_Callback = nullptr;
    DescriptorAllocator* m_DescriptorAllocator = nullptr;

    bool mManipulateSelected = false;

    std::string mSelectedMesh;
    std::string mSelectedMaterial;

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
};

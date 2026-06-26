#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <Windows.h>

#include <string>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

#include "DescriptorAllocator.h"

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
        DescriptorAllocator& globalDescriptorAllocator,
        IFunctionCallback* callback = nullptr);

    void CustomUI(Engine* engine = nullptr);

    // for object creation from loaded assets
    const std::string& GetSelectedMesh() const { return mSelectedMesh; }
    const std::string& GetSelectedMaterial() const { return mSelectedMaterial; }

    void Shutdown();

    // 매 프레임 호출 (Update 또는 Render 시작 부분에서)
    void NewFrame();

    // ImGui 창을 다 그린 후에 호출 (CommandList에 실제 그리기)
    void Render(ID3D12GraphicsCommandList* cmdList);

    void SetManipulateSelected(bool on) { mManipulateSelected = on; }
    bool IsManipulateSelected() const { return mManipulateSelected; }

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
    ID3D12Device* m_Device = nullptr;
    IFunctionCallback* m_Callback = nullptr;

    bool mManipulateSelected = false;

    std::string mSelectedMesh;
    std::string mSelectedMaterial;
};
#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <Windows.h>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

#include "DescriptorAllocator.h"
#include "AppStruct.h"

enum class ButtonAction
{
    None,
    ResetScene,
    SpawnTestObject,
    ToggleWireframe,
    ReloadShaders,
    PrintECSStats,

    // Resolution presets
    SetRes_600x800,
    SetRes_1920x1080,
    SetRes_1200x800,
    // ... 필요할 때마다 추가
};

class IFunctionCallback
{
public:
	virtual void buttonClicked(ButtonAction action) = 0;
	virtual void RequestObjectSpawn(const std::string& meshName, const std::string& materialName) = 0;
};


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

    void CustomUI(const MeshInstanceStats& meshStats, const CullingStats& cullStats);

    void Shutdown();

    // 매 프레임 호출 (Update 또는 Render 시작 부분에서)
    void NewFrame();

    // ImGui 창을 다 그린 후에 호출 (CommandList에 실제 그리기)
    void Render(ID3D12GraphicsCommandList* cmdList);

    // 오브젝트(메시) 선택 및 생성용 별도 창
    void DrawObjectSelector();

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
    ID3D12Device* m_Device = nullptr;
    IFunctionCallback* m_Callback = nullptr;

    // Object selector state
    std::string m_SelectedMesh;
    std::string m_SelectedMaterial;
};
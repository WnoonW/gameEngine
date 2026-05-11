#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <string>
#include <memory>

class App
{
public:
    App();
    virtual ~App();

    bool Initialize(HINSTANCE hInstance, int nCmdShow, HWND hwnd);
    void Run();

    // 사용자/하위 시스템이 오버라이드할 수 있는 가상 메서드
    // → 나중에 ECS, MaterialManager, RenderSystem 등을 여기서 연결
    virtual void OnInitialize() {}   // EntityFactory, MaterialManager 초기화 등
    virtual void OnUpdate(float deltaTime) {}
    virtual void OnRender() {}       // RenderSystem::Draw() 호출 예정
    virtual void OnShutdown() {}
	virtual void OnResize() {}

    HWND GetHwnd() const { return m_hwnd; }
    ID3D12Device* GetDevice() const { return m_device.Get(); }

private:
    // Window
    HWND m_hwnd = nullptr;
    UINT m_width = 1280;
    UINT m_height = 720;
    std::wstring m_title = L"DirectX 12 Game Engine";

    // DX12 Core (다이어그램의 App Class 내부)
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;

    static const UINT FrameCount = 2;
    UINT m_frameIndex = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT m_rtvDescriptorSize = 0;
    UINT m_dsvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // Synchronization
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    // Timer (다이어그램의 Timer)
    LARGE_INTEGER m_frequency{};
    LARGE_INTEGER m_lastTime{};
    float m_deltaTime = 0.0f;

    // 내부 메서드
    void InitializeDX12();
    void CreateSwapChain();
    void CreateRenderTargetView();

    void Update();   // Timer + OnUpdate
    void Render();   // CommandList 기록 + OnRender
    void Present();
    void FlushCommandQueue();
};
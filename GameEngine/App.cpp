#include <d3dx12.h>
#include <iostream>
#include "App.h"

App::App()
{
    QueryPerformanceFrequency(&m_frequency);
    QueryPerformanceCounter(&m_lastTime);
}

App::~App()
{
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

bool App::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    InitializeDX12();
    CreateSwapChain();
    CreateRenderTargetView();

    OnInitialize();        // ← 여기서 ECS Registry, EntityFactory, MaterialManager 초기화 예정
    return true;
}

void App::Run()
{
    Update();
    Render();
    Present();
    FlushCommandQueue();
    OnShutdown();
}

//============================================================
// Timer + Update (다이어그램의 RUN 박스)
void App::Update()
{
    LARGE_INTEGER currentTime;
    QueryPerformanceCounter(&currentTime);
    m_deltaTime = static_cast<float>(currentTime.QuadPart - m_lastTime.QuadPart) / m_frequency.QuadPart;
    m_lastTime = currentTime;

    OnUpdate(m_deltaTime);   // ← PhysicsSystem, 기타 Update Systems 호출 예정
}

//============================================================
// Render (다이어그램의 Draw)
void App::Render()
{
    // CommandList 준비
    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), nullptr);

    // Render Target 전환
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &barrier);

    // 화면 클리어 (기본 색상)
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_frameIndex, m_rtvDescriptorSize);
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    OnRender();   // ← RenderSystem이 실제 DrawIndexedInstanced 등을 수행

    // Present 준비
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->Close();
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
}

void App::Present()
{
    m_swapChain->Present(1, 0);
    m_frameIndex = (m_frameIndex + 1) % FrameCount;
}

//============================================================
// 나머지 DX12 초기화 함수들 (BuildWindow, InitializeDX12 등)


void App::InitializeDX12()
{
    UINT dxgiFactoryFlags = 0;
#if defined(DEBUG) || defined(_DEBUG)
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
    dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    Microsoft::WRL::ComPtr<IDXGIFactory7> factory;
    CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory));

    // Device 생성
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));

    // Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));

    // Command Allocator & List
    m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator));
    m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList));
    m_commandList->Close();

    // Fence
    m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void App::CreateSwapChain()
{
    // ... (DXGI SwapChain 생성 - 표준 코드, 필요하면 더 자세히 드릴게요)
    // 현재는 생략했으나 실제 프로젝트에서는 DXGI_SWAP_CHAIN_DESC1로 생성
}

void App::CreateRenderTargetView()
{
    // RTV Heap + RenderTarget 생성 (생략된 부분)
    // 실제 프로젝트에서는 여기서도 완전히 구현됩니다.
}

void App::FlushCommandQueue()
{
    // GPU가 모든 명령을 끝낼 때까지 대기 (종료 시 필요)
}
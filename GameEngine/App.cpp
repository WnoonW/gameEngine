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

bool App::Initialize(HINSTANCE hInstance, int nCmdShow, HWND hwnd)
{
	m_hwnd = hwnd;

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
    // DXGI Factory (SwapChain 생성에 필요)
    Microsoft::WRL::ComPtr<IDXGIFactory7> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    factory->CreateSwapChainForHwnd(m_commandQueue.Get(), m_hwnd, &swapChainDesc, NULL, NULL, m_swapChain.GetAddressOf());
}

void App::CreateRenderTargetView()
{
    // 1. RTV Descriptor Heap 생성
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(m_rtvHeap.GetAddressOf()));

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(m_dsvHeap.GetAddressOf()));

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // 2. SwapChain의 각 버퍼를 가져와 RTV 생성
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    for (UINT n = 0; n < FrameCount; ++n)
    {
        m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n]));
        m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, m_rtvDescriptorSize);
    }
}

void App::OnResize()
{

}

void App::FlushCommandQueue()
{
    // Fence value 증가
    const UINT64 currentFenceValue = ++m_fenceValue;

    // GPU에 Signal 보내기
    m_commandQueue->Signal(m_fence.Get(), currentFenceValue);

    // GPU가 해당 fence value까지 완료할 때까지 대기
    if (m_fence->GetCompletedValue() < currentFenceValue)
    {
        m_fence->SetEventOnCompletion(currentFenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}
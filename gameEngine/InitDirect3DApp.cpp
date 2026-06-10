//***************************************************************************************
// Init Direct3D.cpp by Frank Luna (C) 2015 All Rights Reserved.
//
// Demonstrates the sample framework by initializing Direct3D, clearing 
// the screen, and displaying frame stats.
//
//***************************************************************************************

#include <DirectXColors.h>
#include "Codes/Common/d3dApp.h"
#include "Codes/V1_/V3/MeshManager.h"
#include "Codes/V1_/V4/MatarialManager.h"
#include "Codes/V1_/V4/DescriptorAllocator.h"
#include "Codes/ECS/Registry.h"
#include "Codes/ECS/ComponentStruct.h"
#include "Codes/ECS/RenderSystem.h"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>
#include "Codes/Structs/AppStruct.h"

using namespace DirectX;

class InitDirect3DApp : public D3DApp
{
public:
	InitDirect3DApp(HINSTANCE hInstance);
	~InitDirect3DApp();

	virtual bool Initialize()override;
	bool InitImGui() override;
private:
	static DescriptorAllocator mGlobalDescriptorAllocator;
	Registry mRegistry = {};
	RenderSystem mRenderSystem = {};
	Entity mEntity = {};
	std::vector<Entity> mEntities;
	uint32_t mNextObjectCBIndex = 0;

	Entity CreateRenderableEntity(
		const std::string& meshName,
		const std::string& materialName,
		XMFLOAT3 position = { 0, 0, 0 });

    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;
	virtual void BeginFrame()override;
	virtual void EndFrame()override;
	virtual void OnDestroy()override;


	virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
	virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
	virtual void OnMouseMove(WPARAM btnState, int x, int y)override;
	virtual void OnMouseWheel(short wheelDelta, int x, int y) override;

	virtual void OnKeyDown(WPARAM key)override;

private:
	float mTheta = 1.5f * XM_PI;
	float mPhi = XM_PIDIV4;
	float mRadius = 5.0f;
	float mTargetY = 0.0f;
	XMFLOAT4X4 mView = {};
	XMFLOAT4X4 mProj = {};
	POINT mLastMousePos = {0, 0};
};

DescriptorAllocator InitDirect3DApp::mGlobalDescriptorAllocator;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
				   PSTR cmdLine, int showCmd)
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
#endif

    try
    {
        InitDirect3DApp theApp(hInstance);
        if(!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch(DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

InitDirect3DApp::InitDirect3DApp(HINSTANCE hInstance)
: D3DApp(hInstance) 
{
}

InitDirect3DApp::~InitDirect3DApp()
{
}

bool InitDirect3DApp::Initialize()
{
	if (!D3DApp::Initialize())
		return false;

	ThrowIfFailed(mCommandList->Reset(mFrameResources[0]->CmdListAlloc.Get(), nullptr));

	//디스크립터
	mGlobalDescriptorAllocator.Initialize(md3dDevice.Get(), 8192);

	//메시
	bool meshResult = MeshManager::Get().CreateMesh("bibian", L"Resources/Assets/bibian.obj",
		md3dDevice.Get(), mCommandList.Get());
	bool meshResult1 = MeshManager::Get().CreateMesh("box", L"Resources/Assets/square.obj",
		md3dDevice.Get(), mCommandList.Get());

	if (!meshResult || !meshResult1)
	{
		MessageBoxA(nullptr, "Mesh Creation Failed!", "Error", MB_OK);
		return false;
	}

	//머티리얼
	MatarialManager::Get().CreateMatarial("Test", L"Resources/Textures/e.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	
	MatarialManager::Get().CreateMatarial("颜", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("颜2", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("眉睫", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("目", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("目光", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("白目", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("口线", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("口舌", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("齿", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("目影", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);

	MatarialManager::Get().CreateMatarial("体", L"Resources/Textures/体.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("肌", L"Resources/Textures/体.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	
	MatarialManager::Get().CreateMatarial("体2", L"Resources/Textures/髮.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("足", L"Resources/Textures/髮.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MatarialManager::Get().CreateMatarial("髮", L"Resources/Textures/髮.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	
	MatarialManager::Get().CreateMatarial("髮+", L"Resources/Textures/spa_h.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	
	//MatarialManager::Get().CreateMatarial("Test", L"Resources/Textures/结晶.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	//MatarialManager::Get().CreateMatarial("Test", L"Resources/Textures/武器金属.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	//MatarialManager::Get().CreateMatarial("Test", L"Resources/Textures/武器.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);

	Entity entity = mRegistry.createEntity();

	mRegistry.addComponent(entity, TransformComponent{ .position = {0, 0, 0} });
	mRegistry.addComponent(entity, RenderableComponent{
		.meshName = "bibian",
		.materialName = "Test",
		.objectCBIndex = mNextObjectCBIndex++
		});
	mRenderSystem.createCBV(md3dDevice.Get(), mFrameResources, gNumFrameResources, mGlobalDescriptorAllocator, entity, mRegistry);

	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
	FlushCommandQueue();
	return true;
}

bool InitDirect3DApp::InitImGui()
{
	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	//io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // IF using Docking Branch

	// Setup Platform/Renderer backends
	ImGui_ImplDX12_InitInfo init_info = {};
	init_info.Device = md3dDevice.Get();
	init_info.CommandQueue = mCommandQueue.Get();
	init_info.NumFramesInFlight = gNumFrameResources;
	init_info.RTVFormat = mBackBufferFormat;

	// Allocating SRV descriptors (for textures) is up to the application, so we provide callbacks.
	// The example_win32_directx12/main.cpp application include a simple free-list based allocator.
	init_info.SrvDescriptorHeap = mGlobalDescriptorAllocator.GetHeap();
	// ★ Allocate 콜백 (람다로 감싸기)
	init_info.SrvDescriptorAllocFn = [](
		ImGui_ImplDX12_InitInfo*,
		D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
		D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle)
		{
			DescriptorAllocator::DescriptorHandle h = mGlobalDescriptorAllocator.Allocate();
			*out_cpu_handle = h.CPU;
			*out_gpu_handle = h.GPU;
		};

	// ★ Free 콜백
	init_info.SrvDescriptorFreeFn = [](
		ImGui_ImplDX12_InitInfo*,
		D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
		D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
		{
			DescriptorAllocator::DescriptorHandle h;
			h.CPU = cpu_handle;
			h.GPU = gpu_handle;

			// Index를 모르기 때문에 Free 함수를 아래처럼 개선해야 제대로 동작합니다.
			mGlobalDescriptorAllocator.Free(h);
		};


	ImGui_ImplWin32_Init(mhMainWnd);           // Win32 입력 처리 초기화
	ImGui_ImplDX12_Init(&init_info);           // DX12 백엔드 초기화  ← 이게 제일 중요!
	// (before 1.91.6 the DirectX12 backend required a single SRV descriptor passed)
	// (there is a legacy version of ImGui_ImplDX12_Init() that supports those, but a future version of Dear ImGuii will requires more descriptors to be allocated)

	return true;
}

Entity InitDirect3DApp::CreateRenderableEntity(
	const std::string& meshName,
	const std::string& materialName,
	XMFLOAT3 position)
{
	Entity entity = mRegistry.createEntity();

	mRegistry.addComponent(entity, TransformComponent{ .position = position });
	mRegistry.addComponent(entity, RenderableComponent{
		.meshName = meshName,
		.materialName = materialName,
		.objectCBIndex = mNextObjectCBIndex++
		});

	// CBV 생성
	mRenderSystem.createCBV(md3dDevice.Get(), mFrameResources, gNumFrameResources,
		mGlobalDescriptorAllocator, entity, mRegistry);

	mEntities.push_back(entity);   // 관리 목록에 추가

	OutputDebugStringA(("Created Entity " + std::to_string(entity) + " with Mesh: " + meshName + ", Material: " + materialName + "\n").c_str());
	return entity;
}

void InitDirect3DApp::OnResize()
{
	D3DApp::OnResize();

	XMMATRIX proj = XMMatrixPerspectiveFovLH(
		XM_PIDIV4,
		AspectRatio(),           // ← 중요
		0.1f,
		1000.0f
	);
	XMStoreFloat4x4(&mProj, proj);
}

void InitDirect3DApp::Update(const GameTimer& gt)
{
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGui::ShowDemoWindow();
}

void InitDirect3DApp::BeginFrame()
{
	// 1. 다음 프레임으로 이동
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// 2. GPU가 이전 프레임 끝났는지 대기
	if (mFence->GetCompletedValue() < mCurrFrameResource->FenceValue)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);

		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->FenceValue, eventHandle));  // ← 이 줄 추가!!!
		if (eventHandle != nullptr)
		{
			// 핸들이 안전할 때만 호출
			WaitForSingleObject(eventHandle, INFINITE);
			CloseHandle(eventHandle);
		}
	}

	// 3. Allocator + CommandList Reset
	ThrowIfFailed(mCurrFrameResource->CmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(mCurrFrameResource->CmdListAlloc.Get(), nullptr));

	// 4. 렌더 타겟 준비 (BeginFrame에 두는 건 임시, 나중에 Draw로 옮겨도 됨)
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
}

void InitDirect3DApp::Draw(const GameTimer& gt)
{
	// ==========================================
	// 1. 구면 좌표로 View 행렬 계산
	// ==========================================
	float x = mRadius * sinf(mPhi) * cosf(mTheta);
	float z = mRadius * sinf(mPhi) * sinf(mTheta);
	float y = mRadius * cosf(mPhi) + mTargetY;   // ← 높이 보정

	XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
	XMVECTOR target = XMVectorSet(0.0f, mTargetY, 0.0f, 1.0f);   // 타겟도 같이 이동
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&mView, view);

	// ==========================================
	// 2. Projection
	// ==========================================
	XMMATRIX proj = XMLoadFloat4x4(&mProj);

	mRenderSystem.render(mRegistry, mCommandList.Get(), mCurrFrameResource, &mGlobalDescriptorAllocator, mCurrFrameResourceIndex, view, proj);

	// Rendering
	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());
}

void InitDirect3DApp::EndFrame()
{
	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	// Done recording commands.
	ThrowIfFailed(mCommandList->Close());

	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// swap the back and front buffers
	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	mCurrFrameResource->FenceValue = ++mCurrentFence;
	ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrFrameResource->FenceValue));
}

void InitDirect3DApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	mLastMousePos.x = x;
	mLastMousePos.y = y;

	SetCapture(mhMainWnd);
}

void InitDirect3DApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void InitDirect3DApp::OnMouseMove(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		// === 기존 왼쪽 드래그: 공전 ===
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

		mTheta -= dx;
		mPhi -= dy;

		// mPhi 제한 (너무 위아래로 가지 않게)
		mPhi = MathHelper::Clamp(mPhi, 0.1f, XM_PI - 0.1f);
	}
	else if ((btnState & MK_RBUTTON) != 0)
	{
		float dy = 0.005f * static_cast<float>(y - mLastMousePos.y);
		mTargetY += dy;                    // 타겟 Y 이동
		mTargetY = MathHelper::Clamp(mTargetY, -50.0f, 50.0f);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}

void InitDirect3DApp::OnMouseWheel(short wheelDelta, int x, int y)
{
	mRadius -= wheelDelta * 0.001f;                    // 감도 조절 (필요하면 0.001 ~ 0.005 사이로 조정)
	mRadius = MathHelper::Clamp(mRadius, 0.1f, 150.0f);
}

void InitDirect3DApp::OnKeyDown(WPARAM wParam)
{
	switch (wParam)
	{
	case VK_UP:          // ↑ 키
		CreateRenderableEntity("bibian","Test", {(float)mNextObjectCBIndex,0,0});
		break;
	case VK_ADD:         // + 키 (숫자패드)
	case VK_OEM_PLUS:    // + 키
		break;

	case VK_DOWN:        // ↓ 키
	case VK_SUBTRACT:    // - 키 (숫자패드)
	case VK_OEM_MINUS:   // - 키
		break;

	case 'R': 
		break;
	}
}

void InitDirect3DApp::OnDestroy()
{
	// === ImGui 종료 (추가) ===
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	MeshManager::Get().Shutdown();
	MatarialManager::Get().Shutdown();
	mGlobalDescriptorAllocator.Shutdown();

	D3DApp::OnDestroy();
}

#pragma warning(disable: 4005)
#pragma warning(disable: 28251)
#pragma warning(disable: 6387)
#include <DirectXColors.h>
#include "d3dApp.h"
#include "DescriptorAllocator.h"
#include "ImGuiManager.h"
#include "Engine.h"
#include "Entity.h"
#include "ComponentStruct.h"

using namespace DirectX;

class InitDirect3DApp : public D3DApp, public IFunctionCallback
{
public:
	InitDirect3DApp(HINSTANCE hInstance);
	~InitDirect3DApp();

	virtual bool Initialize()override;
private:
	static DescriptorAllocator mGlobalDescriptorAllocator;
	ImGuiManager mImGuiManager;
	Engine mEngine;
	int mSpiralIndex = 0;

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
	virtual void OnKeyUp(WPARAM key)override;

	virtual void buttonClicked(ButtonAction action) override;

private:
	void InitializeCoreSystems();
	void LoadAssets();
	void CreateInitialScene();

private:
	float mTheta = 0.0f; // yaw
	float mPhi = 0.0f; // pitch
	float mCamX = 0.0f;
	float mCamY = 5.0f;
	float mCamZ = -10.0f;
	float mFlySpeed = 20.0f;
	XMFLOAT4X4 mView = {};
	XMFLOAT4X4 mProj = {};
	POINT mLastMousePos = {0, 0};

	Entity mMainCamera = INVALID_ENTITY;   // ECS 메인 카메라

	// Camera key states (for OnKeyDown/OnKeyUp based movement)
	bool mKeyW = false;
	bool mKeyS = false;
	bool mKeyA = false;
	bool mKeyD = false;
	bool mKeyQ = false;
	bool mKeyE = false;
	bool mKeyR = false;
	bool mKeyF = false;
	bool mKeySpace = false;
	bool mKeyShift = false;
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

	InitializeCoreSystems();
	LoadAssets();
	CreateInitialScene();

	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
	FlushCommandQueue();

	return true;
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
	mImGuiManager.NewFrame();
	mImGuiManager.CustomUI();

	// === 카메라 조작 (Minecraft Creative 스타일) ===
	// WASD: look 방향 기준 XZ 이동
	// Space: 상승, Shift: 하강
	// 마우스 왼쪽: look (yaw/pitch)
	// QE: 상승/하강 (대안)
	float dt = gt.DeltaTime();
	float speed = mFlySpeed * dt;

	float yaw = mTheta;
	float pitch = mPhi;

	// forward (look dir projected)
	float fx = sinf(yaw) * cosf(pitch);
	float fy = sinf(pitch);
	float fz = cosf(yaw) * cosf(pitch);

	// right
	float rx = cosf(yaw);
	float rz = -sinf(yaw);

	if (mKeyW) {
		mCamX += fx * speed;
		mCamY += fy * speed;
		mCamZ += fz * speed;
	}
	if (mKeyS) {
		mCamX -= fx * speed;
		mCamY -= fy * speed;
		mCamZ -= fz * speed;
	}
	if (mKeyA) {
		mCamX -= rx * speed;
		mCamZ -= rz * speed;
	}
	if (mKeyD) {
		mCamX += rx * speed;
		mCamZ += rz * speed;
	}

	if (mKeySpace) mCamY += speed;
	if (mKeyShift) mCamY -= speed;

	// QE 대안 상승/하강
	if (mKeyQ) mCamY += speed;
	if (mKeyE) mCamY -= speed;

	// pitch clamp
	mPhi = MathHelper::Clamp(mPhi, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);

	mEngine.Update();
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
	// === View 계산 (Minecraft creative 스타일 free cam) ===
	float x = mCamX;
	float y = mCamY;
	float z = mCamZ;

	XMVECTOR posV   = XMVectorSet(x, y, z, 1.0f);
	XMVECTOR forwardV = XMVectorSet(
		sinf(mTheta) * cosf(mPhi),
		sinf(mPhi),
		cosf(mTheta) * cosf(mPhi),
		0.0f
	);
	XMVECTOR upV    = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookToLH(posV, forwardV, upV);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);

	// === ECS CameraComponent 동기화 (위치/회전 기록) ===
	if (mMainCamera != INVALID_ENTITY)
	{
		XMFLOAT3 camPos = { mCamX, mCamY, mCamZ };
		mEngine.SetCameraTransform(mMainCamera, camPos, { mPhi, mTheta, 0.0f });
	}

	// === PassCB 채우기 (ECS의 CameraComponent 활용하여 EyePos, Near/Far 등 채움) ===
	mEngine.FillPassCB(mCurrFrameResource, view, proj,
		static_cast<float>(mClientWidth),
		static_cast<float>(mClientHeight),
		AspectRatio(),
		gt.TotalTime(),
		gt.DeltaTime());

	// === Engine을 통해 렌더링 ===
	mEngine.Render(mCommandList.Get(), mCurrFrameResource, mCurrFrameResourceIndex, view, proj);

	mImGuiManager.Render(mCommandList.Get());
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

// =====================================================
// 핵심 시스템 초기화
// =====================================================
void InitDirect3DApp::InitializeCoreSystems()
{
	mGlobalDescriptorAllocator.Initialize(md3dDevice.Get(), 8192);

	mImGuiManager.Initialize(mhMainWnd, md3dDevice.Get(), mCommandQueue.Get(),
		gNumFrameResources, mBackBufferFormat, mGlobalDescriptorAllocator, this);

	// === Engine 초기화 ===
	mEngine.Initialize(md3dDevice.Get(), mFrameResources, gNumFrameResources, mGlobalDescriptorAllocator);
}

// =====================================================
// 에셋 로딩 (Material + Mesh)
// =====================================================
void InitDirect3DApp::LoadAssets()
{
	// Material 로딩
	MaterialManager::Get().CreateMaterial("Default", L"Resources/Textures/bricks.dds",
		md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("Test", L"Resources/Textures/e.png",
		md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);

	// 추가 Material들 (필요한 것만 남기거나 정리 가능)
	MaterialManager::Get().CreateMaterial("颜", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("颜2", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("眉睫", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("目", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("目光", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("白目", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("口线", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("口舌", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("齿", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("目影", L"Resources/Textures/颜.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);

	MaterialManager::Get().CreateMaterial("体", L"Resources/Textures/体.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("肌", L"Resources/Textures/体.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);

	MaterialManager::Get().CreateMaterial("体2", L"Resources/Textures/髮.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("足", L"Resources/Textures/髮.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("髮", L"Resources/Textures/髮.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);

	MaterialManager::Get().CreateMaterial("髮+", L"Resources/Textures/spa_h.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);


	// Mesh 로딩
	bool meshResult = MeshManager::Get().CreateMesh("bibian", L"Resources/Assets/bibian.obj",
		md3dDevice.Get(), mCommandList.Get());
	bool meshResult1 = MeshManager::Get().CreateMesh("box", L"Resources/Assets/square.obj",
		md3dDevice.Get(), mCommandList.Get());

	if (!meshResult || !meshResult1)
	{
		MessageBoxA(nullptr, "Mesh Creation Failed!", "Error", MB_OK);
	}
}

// =====================================================
// 초기 씬 구성 (엔티티 생성)
// =====================================================
void InitDirect3DApp::CreateInitialScene()
{
	// Minecraft creative 스타일 초기 위치 (0,0,0에서 떨어짐)
	mMainCamera = mEngine.CreateMainCamera({ 0.0f, 5.0f, -10.0f });

	// 기존 렌더 오브젝트
	mEngine.CreateRenderableEntity("bibian", "Test", { 0.0f, 0.0f, 0.0f });
}

#pragma region Input Handling
//입력처리
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
		// 왼쪽 드래그: look (yaw/pitch)
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

		mTheta += dx;
		mPhi -= dy;

		// pitch 제한 (free look)
		mPhi = MathHelper::Clamp(mPhi, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}

void InitDirect3DApp::OnMouseWheel(short wheelDelta, int x, int y)
{
	// 지수함수적(멱함수) 속도 조절: 한 칸당 약 20% 배율
	const float factor = 1.2f;
	mFlySpeed *= powf(factor, wheelDelta / 120.0f);
	mFlySpeed = MathHelper::Clamp(mFlySpeed, 1.0f, 500.0f);
}

void InitDirect3DApp::OnKeyDown(WPARAM wParam)
{
	// Camera controls via flags (used in Update with dt)
	// WASD now for XZ movement (not rotation)
	switch (wParam)
	{
	case 'W': mKeyW = true; break;
	case 'S': mKeyS = true; break;
	case 'A': mKeyA = true; break;
	case 'D': mKeyD = true; break;
	case 'Q': mKeyQ = true; break;
	case 'E': mKeyE = true; break;
	case 'R': mKeyR = true; break;
	case 'F': mKeyF = true; break;
	case VK_SPACE: mKeySpace = true; break;
	case VK_SHIFT: mKeyShift = true; break;
	}

	// Existing logic
	switch (wParam)
	{
	case VK_UP:
	{
		float idx = static_cast<float>(mSpiralIndex);
		float angleStep = 0.1f;
		float radiusStep = 0.1f;

		float angle = idx * angleStep;
		float radius = idx * radiusStep;

		XMFLOAT3 pos = {
			radius * std::cos(angle),
			0.0f,
			radius * std::sin(angle)
		};

		mEngine.CreateRenderableEntity("bibian", "Test", pos);
		mSpiralIndex++;
		break;
	}
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

void InitDirect3DApp::OnKeyUp(WPARAM wParam)
{
	switch (wParam)
	{
	case 'W': mKeyW = false; break;
	case 'S': mKeyS = false; break;
	case 'A': mKeyA = false; break;
	case 'D': mKeyD = false; break;
	case 'Q': mKeyQ = false; break;
	case 'E': mKeyE = false; break;
	case 'R': mKeyR = false; break;
	case 'F': mKeyF = false; break;
	case VK_SPACE: mKeySpace = false; break;
	case VK_SHIFT: mKeyShift = false; break;
	}
}

void InitDirect3DApp::buttonClicked(ButtonAction action)
{
	if (action == ButtonAction::SpawnTestObject)
	{
		mEngine.CreateRenderableEntity("bibian", "Test", { 0, 1, 0 });
	}
}
#pragma endregion

void InitDirect3DApp::OnDestroy()
{
	mEngine.Shutdown();

	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	MeshManager::Get().Shutdown();
	MaterialManager::Get().Shutdown();
	mGlobalDescriptorAllocator.Shutdown();

	D3DApp::OnDestroy();
}

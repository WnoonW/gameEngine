#pragma warning(disable: 4005)
#pragma warning(disable: 28251)
#pragma warning(disable: 6387)
#include <DirectXColors.h>
#include "d3dApp.h"
#include "MeshManager.h"
#include "MaterialManager.h"
#include "RootsignatureManager.h"
#include "DescriptorAllocator.h"
#include "ComponentStruct.h"
#include "RenderSystem.h"
#include "AppStruct.h"
#include "ImGuiManager.h"
#include "PipelineStateManager.h"
#include "ShaderManager.h"


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
	RenderSystem mRenderSystem = {};
	ECS::World mWorld;
	std::vector<Entity> mEntities;
	uint32_t mNextObjectCBIndex = 0;
	int mSpiralIndex = 0;

	Entity CreateRenderableEntity(
		Mesh* mesh,
		std::shared_ptr<Material> material,
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

	virtual void buttonClicked(ButtonAction action) override;
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
	mImGuiManager.Initialize(mhMainWnd, md3dDevice.Get(), mCommandQueue.Get(), gNumFrameResources, mBackBufferFormat, mGlobalDescriptorAllocator, this);
	
	ShaderManager::Get().Initialize();
	RootSignatureManager::Get().Initialize(md3dDevice.Get());
	PipelineStateManager::Get().Initialize(md3dDevice.Get());

	//머티리얼
	MaterialManager::Get().CreateMaterial("Default", L"Resources/Textures/bricks.dds", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	MaterialManager::Get().CreateMaterial("Test", L"Resources/Textures/e.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);

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

	//MaterialManager::Get().CreateMaterial("Test", L"Resources/Textures/结晶.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	//MaterialManager::Get().CreateMaterial("Test", L"Resources/Textures/武器金属.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);
	//MaterialManager::Get().CreateMaterial("Test", L"Resources/Textures/武器.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);


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

	Mesh* bibianMesh = MeshManager::Get().GetMesh("bibian");
	auto testMat = MaterialManager::Get().GetMaterial("Test");

	if (bibianMesh && testMat)
	{
		Entity e = mWorld.CreateEntity();
		mWorld.AddComponent(e, TransformComponent{ .position = {0,0,0} });
		mWorld.AddComponent(e, RenderableComponent{
			.mesh = MeshManager::Get().GetMesh("bibian"),
			.material = MaterialManager::Get().GetMaterial("Test"),
			.objectCBIndex = mNextObjectCBIndex++
			});

		mRenderSystem.createCBV(md3dDevice.Get(), mFrameResources, gNumFrameResources,
			mGlobalDescriptorAllocator, e, mWorld);
	}
	
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
	FlushCommandQueue();
	return true;
}

Entity InitDirect3DApp::CreateRenderableEntity(
	Mesh* mesh,
	std::shared_ptr<Material> material,
	XMFLOAT3 position)
{
	if (!mesh || !material)
	{
		OutputDebugStringA("CreateRenderableEntity failed: mesh or material is null\n");
		return INVALID_ENTITY;
	}

	Entity entity = mWorld.CreateEntity();

	mWorld.AddComponent(entity, TransformComponent{ .position = position });
	mWorld.AddComponent(entity, RenderableComponent{
		.mesh = mesh,           
		.material = material,   
		.objectCBIndex = mNextObjectCBIndex++
		});

	mRenderSystem.createCBV(md3dDevice.Get(), mFrameResources, gNumFrameResources,
		mGlobalDescriptorAllocator, entity, mWorld);

	mEntities.push_back(entity);

	OutputDebugStringA(("Created Entity " + std::to_string(entity) + "\n").c_str());
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
	mImGuiManager.NewFrame();
	mImGuiManager.CustomUI();
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

	mRenderSystem.render(mWorld, mCommandList.Get(), mCurrFrameResource, 
		&mGlobalDescriptorAllocator, mCurrFrameResourceIndex, view, proj);

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
	mRadius -= wheelDelta * 0.005f;                    // 감도 조절 (필요하면 0.001 ~ 0.005 사이로 조정)
	mRadius = MathHelper::Clamp(mRadius, 0.1f, 150.0f);
}

void InitDirect3DApp::OnKeyDown(WPARAM wParam)
{
	switch (wParam)
	{
	case VK_UP:
	{
		Mesh* mesh = MeshManager::Get().GetMesh("bibian");
		auto mat = MaterialManager::Get().GetMaterial("Test");
		if (mesh && mat)
		{
			float idx = static_cast<float>(mSpiralIndex);

			// === 회오리 파라미터 (여기서 조절하세요) ===
			float angleStep = 0.1f;     // 라디안 (작을수록 빽빽한 회오리)
			float radiusStep = 0.1f;     // 클수록 빨리 퍼짐

			float angle = idx * angleStep;
			float radius = idx * radiusStep;

			XMFLOAT3 pos = {
				radius * std::cos(angle),
				0.0f,
				radius * std::sin(angle)
			};

			CreateRenderableEntity(mesh, mat, pos);
			mSpiralIndex++;
		}
		break;
	}
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

void InitDirect3DApp::buttonClicked(ButtonAction action)
{
	if (action == ButtonAction::SpawnTestObject)
	{
		if (action == ButtonAction::SpawnTestObject)
		{
			Mesh* mesh = MeshManager::Get().GetMesh("bibian");
			auto mat = MaterialManager::Get().GetMaterial("Test");
			if (mesh && mat)
				CreateRenderableEntity(mesh, mat, { (float)mNextObjectCBIndex, 0, 0 });
		}
	}
}

void InitDirect3DApp::OnDestroy()
{
	// === ImGui 종료 (추가) ===
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	MeshManager::Get().Shutdown();
	MaterialManager::Get().Shutdown();
	mGlobalDescriptorAllocator.Shutdown();

	D3DApp::OnDestroy();
}

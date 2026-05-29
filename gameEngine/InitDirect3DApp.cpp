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

using namespace DirectX;

class InitDirect3DApp : public D3DApp
{
public:
	InitDirect3DApp(HINSTANCE hInstance);
	~InitDirect3DApp();

	virtual bool Initialize()override;

private:
	DescriptorAllocator mGlobalDescriptorAllocator;
	Registry mRegistry = {};
	RenderSystem mRenderSystem = {};
	Entity mEntity = {};
	uint32_t mNextObjectCBIndex = 0;

    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;
	virtual void BeginFrame()override;
	virtual void EndFrame()override;
	virtual void OnDestroy()override;


	virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
	virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
	virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

	virtual void OnKeyDown(WPARAM key)override;

private:
	float mTheta = 1.5f * XM_PI;
	float mPhi = XM_PIDIV4;
	float mRadius = 5.0f;

	POINT mLastMousePos = {0, 0};
};

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

	if (!meshResult)
	{
		MessageBoxA(nullptr, "Mesh Creation Failed!", "Error", MB_OK);
		return false;
	}

	//머티리얼
	MatarialManager::Get().CreateMatarial("Test", L"Resources/Textures/e.png", md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get(), mGlobalDescriptorAllocator);

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

void InitDirect3DApp::OnResize()
{
	D3DApp::OnResize();
}

void InitDirect3DApp::Update(const GameTimer& gt)
{

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
	XMMATRIX mView = XMMatrixLookAtLH(
		XMVectorSet(0, 5, -10, 0),   // eye
		XMVectorSet(0, 0, 0, 0),     // target
		XMVectorSet(0, 1, 0, 0)      // up
	);

	XMMATRIX mProj = XMMatrixPerspectiveFovLH(
		XM_PIDIV4,
		16.0f / 9.0f,
		0.1f,
		1000.0f
	);
	mRenderSystem.render(mRegistry, mCommandList.Get(), mCurrFrameResource, &mGlobalDescriptorAllocator, mView, mProj);
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
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

		mTheta -= dx;
		mPhi -= dy;
		mPhi = MathHelper::Clamp(mPhi, 0.1f, XM_PI - 0.1f);
	}
	else if (btnState & MK_RBUTTON)
	{
		float dx = 0.05f * static_cast<float>(x - mLastMousePos.x);
		float dy = 0.05f * static_cast<float>(y - mLastMousePos.y);
		mRadius += dx - dy;
		//mRadius = MathHelper::Clamp(mRadius, 3.0f, 15.0f);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}


void InitDirect3DApp::OnKeyDown(WPARAM wParam)
{
	switch (wParam)
	{
	case VK_UP:          // ↑ 키
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
	MeshManager::Get().Shutdown();
	MatarialManager::Get().Shutdown();
	mGlobalDescriptorAllocator.Shutdown();
	D3DApp::OnDestroy();
}
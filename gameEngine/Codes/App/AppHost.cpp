#pragma warning(disable: 4005)
#pragma warning(disable: 28251)
#pragma warning(disable: 6387)
#include "AppHost.h"
#include <DirectXColors.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>
#include "MeshManager.h"
#include "MaterialManager.h"
#include "DefaultAssets.h"
#include "EditorMode.h"
#include "PlayMode.h"
#include "FlyCameraMath.h"
#include "d3dx12.h"

using namespace DirectX;
using ECS::Entity;
using ECS::INVALID_ENTITY;

void AppHost::SetGameConfig(const GameConfig& cfg)
{
	mGameConfig = cfg;
	mPlayMode = (cfg.mode == GameConfig::Mode::Play);
	if (!cfg.scenePath.empty())
		mPendingSceneLoad = GameConfig::JoinPath(GameConfig::GetExeDirectory(), cfg.scenePath);
	if (!cfg.title.empty())
		mMainWndCaption = std::wstring(cfg.title.begin(), cfg.title.end());
}

DescriptorAllocator AppHost::mGlobalDescriptorAllocator;

AppHost::AppHost(HINSTANCE hInstance)
	: D3DApp(hInstance)
{
}

AppHost::~AppHost()
{
}

void AppHost::BuildAppContext()
{
	mCtx.hwnd = mhMainWnd;
	mCtx.clientWidth = &mClientWidth;
	mCtx.clientHeight = &mClientHeight;
	mCtx.screenViewport = &mScreenViewport;
	mCtx.scissorRect = &mScissorRect;
	mCtx.engine = &mEngine;
	mCtx.imgui = &mImGuiManager;
	mCtx.view = &mView;
	mCtx.proj = &mProj;
	mCtx.currentView = &mCurrentView;
	mCtx.currentProj = &mCurrentProj;
	mCtx.mainCamera = &mMainCamera;
	mCtx.gameConfig = &mGameConfig;
}

void AppHost::CreateModeController()
{
	if (mPlayMode)
		mMode = std::make_unique<PlayMode>();
	else
		mMode = std::make_unique<EditorMode>();
}

bool AppHost::Initialize()
{
	if (!D3DApp::Initialize())
		return false;

	RegisterMouseRawInput();

	ThrowIfFailed(mCommandList->Reset(mFrameResources[0]->CmdListAlloc.Get(), nullptr));
	MarkCommandListRecording(true);

	InitializeCoreSystems();
	LoadAssets();

	if (mPlayMode && !mPendingSceneLoad.empty())
	{
		std::string err;
		if (!mEngine.LoadSceneFromFile(mPendingSceneLoad, &err))
		{
			OutputDebugStringA(("[Play] Load scene failed: " + err + "\n").c_str());
			CreateInitialScene();
		}
		else
		{
			if (mMainCamera == INVALID_ENTITY)
				mMainCamera = mEngine.CreateMainCamera({ 0.0f, 5.0f, -5.0f });
		}
	}
	else
	{
		CreateInitialScene();
	}

	mEngine.SetAutoRenderPathEnabled(true);
	mEngine.SetGpuFrustumCullEnabled(true);
	mEngine.SetGpuOcclusionEnabled(true);

	ThrowIfFailed(mCommandList->Close());
	MarkCommandListRecording(false);
	ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
	FlushCommandQueue();

	BuildAppContext();
	CreateModeController();

	// Mode-specific window presentation
	if (mPlayMode)
	{
		ProcessPendingResize();
		ShowWindow(mhMainWnd, SW_SHOW);
		UpdateWindow(mhMainWnd);
		SetForegroundWindow(mhMainWnd);
		RECT cr{};
		if (GetClientRect(mhMainWnd, &cr))
		{
			mClientWidth = (std::max)(1L, cr.right - cr.left);
			mClientHeight = (std::max)(1L, cr.bottom - cr.top);
			mPendingResize = true;
			ProcessPendingResize();
			mImGuiManager.SetDesiredSceneSize(
				static_cast<UINT>(mClientWidth),
				static_cast<UINT>(mClientHeight));
			mImGuiManager.EnsureSceneViewport([this]() { FlushCommandQueue(); });
		}
	}
	else
	{
		mImGuiManager.ApplyMainWindowPlacement();
		ProcessPendingResize();
		mImGuiManager.PresentMainWindow();

		// Route ImGui toolbar actions to EditorMode.
		if (auto* editor = dynamic_cast<EditorMode*>(mMode.get()))
			mImGuiManager.SetCallback(editor);
	}

	if (mMode)
		mMode->OnAfterInit(mCtx);

	return true;
}

void AppHost::OnResize()
{
	D3DApp::OnResize();

	const float aspect = AspectRatio();
	const float fovY = 2.0f * atanf(tanf(XMConvertToRadians(70.0f) * 0.5f) / aspect);
	XMMATRIX proj = XMMatrixPerspectiveFovLH(fovY, aspect, 0.1f, 1000.0f);
	XMStoreFloat4x4(&mProj, proj);

	if (mMode)
		mMode->OnResize(mCtx);
}

void AppHost::RegisterMouseRawInput()
{
	RAWINPUTDEVICE rid{};
	rid.usUsagePage = 0x01;
	rid.usUsage = 0x02;
	rid.dwFlags = RIDEV_INPUTSINK;
	rid.hwndTarget = mhMainWnd;
	RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));
}

LRESULT AppHost::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (mMode)
	{
		const bool handled = mMode->OnMsg(mCtx, hwnd, msg, wParam, lParam);
		// Fully consumed raw input / ESC / play LMB — skip D3DApp for those.
		if (handled && (msg == WM_INPUT || msg == WM_KEYUP || msg == WM_LBUTTONDOWN))
			return 0;
	}

	return D3DApp::MsgProc(hwnd, msg, wParam, lParam);
}

void AppHost::Update(const GameTimer& gt)
{
	// ImGui frame always (WndProc / GetIO). Panels only in EditorMode::OnUpdate.
	mImGuiManager.NewFrame();

	float dt = gt.DeltaTime();
	if (dt > 0.033f)
		dt = 0.033f;

	if (mMode)
		mMode->OnUpdate(mCtx, dt);

	mEngine.Update(dt);
}

void AppHost::BeginFrame()
{
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	if (mFence->GetCompletedValue() < mCurrFrameResource->FenceValue)
	{
		HANDLE eventHandle = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->FenceValue, eventHandle));
		if (eventHandle != nullptr)
		{
			WaitForSingleObject(eventHandle, INFINITE);
			CloseHandle(eventHandle);
		}
	}

	if (mMode)
		mMode->OnBeginFrame(mCtx);

	mImGuiManager.EnsureSceneViewport([this]() { FlushCommandQueue(); });

	{
		const SceneViewport& sceneVP = mImGuiManager.GetSceneViewport();
		if (sceneVP.IsValid())
			mEngine.PrepareHiZForSceneSize(sceneVP.GetWidth(), sceneVP.GetHeight());
	}

	ThrowIfFailed(mCurrFrameResource->CmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(mCurrFrameResource->CmdListAlloc.Get(), nullptr));
	MarkCommandListRecording(true);

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	const float editorClear[] = { 0.12f, 0.12f, 0.14f, 1.0f };
	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), editorClear, 0, nullptr);
}

void AppHost::Draw(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mView);

	SceneViewport& sceneVP = mImGuiManager.GetSceneViewport();
	const float aspect = sceneVP.IsValid() ? sceneVP.GetAspectRatio() : AspectRatio();
	const float fovY = 2.0f * atanf(tanf(XMConvertToRadians(70.0f) * 0.5f) / aspect);
	XMMATRIX proj = XMMatrixPerspectiveFovLH(fovY, aspect, 0.1f, 1000.0f);
	XMStoreFloat4x4(&mProj, proj);
	mCurrentView = view;
	mCurrentProj = proj;

	if (mMainCamera != INVALID_ENTITY && mMode)
	{
		float camX = 0, camY = 0, camZ = 0;
		float camPitch = 0, camYaw = 0;
		mMode->GetCameraPose(camX, camY, camZ, camPitch, camYaw);

		// Prefer angles reconstructed from the actual view matrix.
		FlyCameraMath::ExtractPitchYawFromView(view, camPitch, camYaw);

		XMFLOAT3 camPos = { camX, camY, camZ };
		mEngine.SetCameraTransform(mMainCamera, camPos, { camPitch, camYaw, 0.0f });
	}

	if (sceneVP.IsValid())
	{
		const float sceneClear[] = {
			Colors::LightSteelBlue.f[0],
			Colors::LightSteelBlue.f[1],
			Colors::LightSteelBlue.f[2],
			Colors::LightSteelBlue.f[3]
		};
		sceneVP.Begin(mCommandList.Get(), sceneClear);

		mEngine.FillPassCB(mCurrFrameResource, view, proj,
			static_cast<float>(sceneVP.GetWidth()),
			static_cast<float>(sceneVP.GetHeight()),
			aspect,
			gt.TotalTime(),
			gt.DeltaTime());

		mEngine.Render(mCommandList.Get(), mCurrFrameResource, mCurrFrameResourceIndex, view, proj);

		sceneVP.End(mCommandList.Get());

		if (sceneVP.HasDepthSrv() && mEngine.IsGpuOcclusionEnabled())
		{
			mEngine.BuildHiZ(
				mCommandList.Get(),
				sceneVP.GetDepthResource(),
				sceneVP.GetDepthSrvCpu(),
				sceneVP.GetDepthSrvGpu(),
				sceneVP.GetWidth(),
				sceneVP.GetHeight());
		}
	}

	mCtx.backBufferRtv = CurrentBackBufferView();
	if (mMode)
	{
		mMode->OnPresent(
			mCtx,
			mCommandList.Get(),
			sceneVP,
			CurrentBackBuffer());
	}
}

void AppHost::EndFrame()
{
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(mCommandList->Close());
	MarkCommandListRecording(false);

	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	mCurrFrameResource->FenceValue = ++mCurrentFence;
	ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrFrameResource->FenceValue));
}

void AppHost::InitializeCoreSystems()
{
	mGlobalDescriptorAllocator.Initialize(md3dDevice.Get(), 8192);

	// Callback set later for EditorMode; nullptr is fine for play / early frames.
	mImGuiManager.Initialize(mhMainWnd, md3dDevice.Get(), mCommandQueue.Get(),
		gNumFrameResources, mBackBufferFormat, mDepthStencilFormat, mGlobalDescriptorAllocator, nullptr);

	mEngine.Initialize(md3dDevice.Get(), mFrameResources, gNumFrameResources, mGlobalDescriptorAllocator);
}

void AppHost::LoadAssets()
{
	DefaultAssets::Load(
		md3dDevice.Get(),
		mCommandList.Get(),
		mCommandQueue.Get(),
		mGlobalDescriptorAllocator);
}

void AppHost::CreateInitialScene()
{
	mMainCamera = mEngine.CreateMainCamera({ 0.0f, 5.0f, -5.0f });
	mEngine.CreateRenderableEntity("bibian", "Test", { 0.0f, 0.0f, 0.0f });
}

void AppHost::OnMouseDown(WPARAM btnState, int x, int y)
{
	if (mMode)
		mMode->OnMouseDown(mCtx, btnState, x, y);
}

void AppHost::OnMouseUp(WPARAM btnState, int x, int y)
{
	if (mMode)
		mMode->OnMouseUp(mCtx, btnState, x, y);
}

void AppHost::OnMouseMove(WPARAM btnState, int x, int y)
{
	if (mMode)
		mMode->OnMouseMove(mCtx, btnState, x, y);
}

void AppHost::OnMouseWheel(short wheelDelta, int x, int y)
{
	if (mMode)
		mMode->OnMouseWheel(mCtx, wheelDelta, x, y);
}

void AppHost::OnKeyDown(WPARAM key)
{
	if (mMode)
		mMode->OnKeyDown(mCtx, key);
}

void AppHost::OnKeyUp(WPARAM key)
{
	if (mMode)
		mMode->OnKeyUp(mCtx, key);
}

void AppHost::OnDestroy()
{
	if (mMode)
		mMode->OnDestroy(mCtx);
	mMode.reset();

	FlushCommandQueue();

	mImGuiManager.Shutdown();
	mEngine.Shutdown();

	MeshManager::Get().Shutdown();
	MaterialManager::Get().Shutdown();
	mGlobalDescriptorAllocator.Shutdown();

	D3DApp::OnDestroy();
}

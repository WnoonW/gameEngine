#pragma warning(disable: 4005)
#pragma warning(disable: 28251)
#pragma warning(disable: 6387)
#include "AppHost.h"
#include <DirectXColors.h>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <vector>
#include <string>
#include <filesystem>
#include "MeshManager.h"
#include "MaterialManager.h"
#include "DefaultAssets.h"
#include "EditorMode.h"
#include "PlayMode.h"
#include "FlyCameraMath.h"
#include "SceneSerializer.h"
#include "d3dx12.h"

using namespace DirectX;
using ECS::Entity;
using ECS::INVALID_ENTITY;

void AppHost::SetGameConfig(const GameConfig& cfg)
{
	mGameConfig = cfg;
	mPlayMode = (cfg.mode == GameConfig::Mode::Play);
	if (!cfg.scenePath.empty())
	{
		// Absolute path (drive letter or UNC) — use as-is; else relative to exe dir.
		const bool absWin =
			(cfg.scenePath.size() >= 2 && std::isalpha(static_cast<unsigned char>(cfg.scenePath[0])) && cfg.scenePath[1] == ':')
			|| (cfg.scenePath.size() >= 2 && cfg.scenePath[0] == '\\' && cfg.scenePath[1] == '\\');
		if (absWin)
			mPendingSceneLoad = cfg.scenePath;
		else
			mPendingSceneLoad = GameConfig::JoinPath(GameConfig::GetExeDirectory(), cfg.scenePath);
	}
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
	// Editor session hooks only when this process is the editor product.
	mCtx.editorHost = mPlayMode ? nullptr : static_cast<IEditorHost*>(this);
}

std::string AppHost::MakeScenesTempPath(const char* fileName)
{
	return GameConfig::JoinPath(SceneSerializer::DefaultScenesDirectory(), fileName);
}

void AppHost::CaptureModeCamera()
{
	if (!mMode)
		return;
	mMode->GetCameraPose(mSnapCamX, mSnapCamY, mSnapCamZ, mSnapCamPitch, mSnapCamYaw);
}

bool AppHost::SavePlaySnapshot(const std::string& absolutePath, std::string* outError)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(absolutePath).parent_path(), ec);
	if (!mEngine.SaveSceneToFile(absolutePath, "play_snapshot"))
	{
		if (outError)
			*outError = "Failed to save scene snapshot: " + absolutePath;
		return false;
	}
	return true;
}

bool AppHost::RestorePlaySnapshot(const std::string& absolutePath, std::string* outError)
{
	if (!mEngine.LoadSceneFromFile(absolutePath, outError))
		return false;
	if (mMainCamera == INVALID_ENTITY)
		mMainCamera = mEngine.CreateMainCamera({ 0.0f, 5.0f, -5.0f });
	return true;
}

void AppHost::CloseStandaloneProcess(bool terminate)
{
	if (!mStandaloneProcess)
		return;
	if (terminate && WaitForSingleObject(mStandaloneProcess, 0) == WAIT_TIMEOUT)
		TerminateProcess(mStandaloneProcess, 0);
	CloseHandle(mStandaloneProcess);
	mStandaloneProcess = nullptr;
}

bool AppHost::IsStandaloneRunning() const
{
	if (!mStandaloneProcess)
		return false;
	const DWORD wait = WaitForSingleObject(mStandaloneProcess, 0);
	return wait == WAIT_TIMEOUT;
}

bool AppHost::PlayStandalone()
{
	if (mPlayMode)
		return false;
	if (mInEditorPlaying)
	{
		MessageBoxA(mhMainWnd,
			"Stop in-editor Play (ESC) before launching standalone Game.exe.",
			"Play Standalone", MB_OK | MB_ICONINFORMATION);
		return false;
	}

	// Reap finished child
	if (mStandaloneProcess && !IsStandaloneRunning())
		CloseStandaloneProcess(false);

	if (IsStandaloneRunning())
	{
		// Bring existing game window to front is hard without hwnd; relaunch after stop.
		const int r = MessageBoxA(mhMainWnd,
			"Game.exe is already running.\n\nStop it and launch again?",
			"Play Standalone", MB_YESNO | MB_ICONQUESTION);
		if (r != IDYES)
			return false;
		CloseStandaloneProcess(true);
	}

	const std::string absScene = MakeScenesTempPath("_play_standalone.scene");
	const std::string relScene = "Scenes/_play_standalone.scene";
	std::string err;
	if (!SavePlaySnapshot(absScene, &err))
	{
		MessageBoxA(mhMainWnd, err.c_str(), "Play Standalone Failed", MB_OK | MB_ICONERROR);
		return false;
	}

	const std::string gameExe = GameConfig::JoinPath(GameConfig::GetExeDirectory(), "Game.exe");
	const DWORD attr = GetFileAttributesA(gameExe.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
	{
		MessageBoxA(mhMainWnd,
			"Game.exe not found next to the editor.\n\n"
			"Build the Game project (same configuration as Editor),\n"
			"so bin\\x64\\<Config>\\Game.exe exists.",
			"Play Standalone Failed", MB_OK | MB_ICONERROR);
		return false;
	}

	// Command line: first token is exe path (CreateProcess requirement when lpApplicationName is null).
	std::string cmdLine = "\"" + gameExe + "\" --scene \"" + relScene + "\"";
	std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
	cmdBuf.push_back('\0');

	STARTUPINFOA si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};

	// Inherit editor working directory (Resources resolution).
	const BOOL ok = CreateProcessA(
		nullptr,
		cmdBuf.data(),
		nullptr,
		nullptr,
		FALSE,
		0,
		nullptr,
		nullptr,
		&si,
		&pi);
	if (!ok)
	{
		char buf[256];
		sprintf_s(buf, "CreateProcess failed (error %lu).", GetLastError());
		MessageBoxA(mhMainWnd, buf, "Play Standalone Failed", MB_OK | MB_ICONERROR);
		return false;
	}

	CloseHandle(pi.hThread);
	mStandaloneProcess = pi.hProcess;
	OutputDebugStringA(("[Editor] Play Standalone: " + cmdLine + "\n").c_str());
	return true;
}

void AppHost::StopStandalone()
{
	CloseStandaloneProcess(true);
}

bool AppHost::PlayInEditor()
{
	if (mPlayMode)
		return false;
	// Already playing or start already queued (menu is inside EditorMode::OnUpdate —
	// must NOT destroy mMode until after OnUpdate returns).
	if (mInEditorPlaying || mPendingStartInEditorPlay)
		return true;

	if (IsStandaloneRunning())
	{
		MessageBoxA(mhMainWnd,
			"Standalone Game.exe is running.\nStop it first (Play menu → Stop Standalone).",
			"Play In Editor", MB_OK | MB_ICONINFORMATION);
		return false;
	}

	mInEditorSnapshotPath = MakeScenesTempPath("_editor_play_snapshot.scene");
	std::string err;
	if (!SavePlaySnapshot(mInEditorSnapshotPath, &err))
	{
		MessageBoxA(mhMainWnd, err.c_str(), "Play In Editor Failed", MB_OK | MB_ICONERROR);
		return false;
	}

	CaptureModeCamera();
	mPendingStopInEditorPlay = false;
	mPendingStartInEditorPlay = true;
	OutputDebugStringA("[Editor] Play In Editor queued (applies after frame update).\n");
	return true;
}

void AppHost::StopInEditorPlay()
{
	// Cancel a start that has not applied yet.
	if (mPendingStartInEditorPlay && !mInEditorPlaying)
	{
		mPendingStartInEditorPlay = false;
		return;
	}
	// May be called from PlayMode::OnMsg — defer mode swap until after Update/Msg.
	if (!mInEditorPlaying)
		return;
	mPendingStopInEditorPlay = true;
}

void AppHost::ApplyDeferredSessionActions()
{
	// Prefer stop over start if both somehow queued.
	if (mPendingStopInEditorPlay)
	{
		mPendingStopInEditorPlay = false;
		mPendingStartInEditorPlay = false;
		if (mInEditorPlaying)
		{
			if (mMode)
			{
				mMode->OnDestroy(mCtx);
				mMode.reset();
			}

			std::string err;
			if (!RestorePlaySnapshot(mInEditorSnapshotPath, &err))
			{
				OutputDebugStringA(("[Editor] Snapshot restore failed: " + err + "\n").c_str());
				if (mMainCamera == INVALID_ENTITY)
					mMainCamera = mEngine.CreateMainCamera({ 0.0f, 5.0f, -5.0f });
			}

			auto editor = std::make_unique<EditorMode>();
			editor->SetCameraPose(mSnapCamX, mSnapCamY, mSnapCamZ, mSnapCamPitch, mSnapCamYaw);
			mMode = std::move(editor);
			mInEditorPlaying = false;

			if (auto* ed = dynamic_cast<EditorMode*>(mMode.get()))
				mImGuiManager.SetCallback(ed);

			if (mMode)
				mMode->OnAfterInit(mCtx);

			OutputDebugStringA("[Editor] Play In Editor stopped; scene snapshot restored.\n");
		}
	}

	if (mPendingStartInEditorPlay)
	{
		mPendingStartInEditorPlay = false;
		if (mPlayMode || mInEditorPlaying)
			return;

		if (mMode)
		{
			mMode->OnDestroy(mCtx);
			mMode.reset();
		}
		mImGuiManager.SetCallback(nullptr);

		auto play = std::make_unique<PlayMode>();
		play->SetEmbeddedInEditor(true);
		play->SetCameraPose(mSnapCamX, mSnapCamY, mSnapCamZ, mSnapCamPitch, mSnapCamYaw);
		mMode = std::move(play);
		mInEditorPlaying = true;

		if (mMode)
			mMode->OnAfterInit(mCtx);

		OutputDebugStringA("[Editor] Play In Editor started (ESC = Stop).\n");
	}
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
		mImGuiManager.SetEditorHost(static_cast<IEditorHost*>(this));
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

	// B: ESC during embedded play requests stop mid-frame — apply after mode update.
	ApplyDeferredSessionActions();

	// Reap standalone child when it exits (no zombie handle).
	if (mStandaloneProcess && !IsStandaloneRunning())
		CloseStandaloneProcess(false);

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

		// PassCB (incl. light VP) before shadow + color
		mEngine.FillPassCB(mCurrFrameResource, view, proj,
			static_cast<float>(sceneVP.GetWidth()),
			static_cast<float>(sceneVP.GetHeight()),
			aspect,
			gt.TotalTime(),
			gt.DeltaTime());

		// Directional shadow map (depth-only; no Scene RT bound yet)
		mEngine.RenderShadowMap(
			mCommandList.Get(), mCurrFrameResource, mCurrFrameResourceIndex, view, proj);

		sceneVP.Begin(mCommandList.Get(), sceneClear);

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
	// Main material left empty (None) unless the user assigns one.
	mEngine.CreateRenderableEntity("bibian", "", { 0.0f, 0.0f, 0.0f });
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
	// Editor shortcuts: F5 = Play In Editor, Ctrl+F5 = Play Standalone
	if (!mPlayMode && key == VK_F5)
	{
		const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		if (ctrl)
			PlayStandalone();
		else if (!mInEditorPlaying)
			PlayInEditor();
		return;
	}

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
	CloseStandaloneProcess(true);

	if (mMode)
		mMode->OnDestroy(mCtx);
	mMode.reset();
	mInEditorPlaying = false;
	mPendingStartInEditorPlay = false;
	mPendingStopInEditorPlay = false;

	FlushCommandQueue();

	mImGuiManager.Shutdown();
	mEngine.Shutdown();

	MeshManager::Get().Shutdown();
	MaterialManager::Get().Shutdown();
	mGlobalDescriptorAllocator.Shutdown();

	D3DApp::OnDestroy();
}

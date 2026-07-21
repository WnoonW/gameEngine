#pragma warning(disable: 4005)
#pragma warning(disable: 28251)
#pragma warning(disable: 6387)
#include <DirectXColors.h>
#include <cmath>
#include <vector>
#include "d3dApp.h"
#include "DescriptorAllocator.h"
#include "ImGuiManager.h"
#include "Engine.h"
#include "Entity.h"
#include "ComponentStruct.h"
#include "MeshManager.h"
#include "MaterialManager.h"
#include "GeometryGenerator.h"

using namespace DirectX;

namespace
{
	constexpr float kMinOrbitRadius = 2.0f;
	constexpr float kMaxOrbitRadius = 80.0f;
	constexpr float kDefaultOrbitPitch = -0.3f;

	XMVECTOR FlattenHorizForward(XMVECTOR lookForward, XMMATRIX yawRot)
	{
		XMFLOAT3 hf{};
		XMStoreFloat3(&hf, lookForward);
		hf.y = 0.0f;
		XMVECTOR horizForward = XMLoadFloat3(&hf);
		const float fwdLenSq = XMVectorGetX(XMVector3LengthSq(horizForward));
		if (fwdLenSq < 1e-6f)
			return XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), yawRot);
		return XMVector3Normalize(horizForward);
	}

	XMVECTOR ComputeThirdPersonCameraPosition(XMVECTOR pivot, float objectYaw, float pitch, float theta, float radius)
	{
		const XMMATRIX objectRot = XMMatrixRotationY(objectYaw);
		const XMMATRIX orbitRot = XMMatrixRotationRollPitchYaw(pitch, theta, 0.0f);
		const XMMATRIX combined = XMMatrixMultiply(orbitRot, objectRot);
		const XMVECTOR offsetDir = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), combined);
		return XMVectorAdd(pivot, XMVectorScale(offsetDir, radius));
	}

	void ExtractPitchYawFromView(const XMMATRIX& view, float& outPitch, float& outYaw)
	{
		XMVECTOR det;
		XMMATRIX invView = XMMatrixInverse(&det, view);
		XMVECTOR forward = XMVector3Normalize(
			XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), invView));

		XMFLOAT3 f{};
		XMStoreFloat3(&f, forward);
		outPitch = asinf(MathHelper::Clamp(f.y, -1.0f, 1.0f));
		outYaw = atan2f(f.x, f.z);
	}

	XMMATRIX BuildOrbitView(XMVECTOR camPos, XMVECTOR pivot, XMVECTOR worldUp,
		float& outCamX, float& outCamY, float& outCamZ)
	{
		XMFLOAT3 p{};
		XMStoreFloat3(&p, camPos);
		outCamX = p.x;
		outCamY = p.y;
		outCamZ = p.z;
		return XMMatrixLookAtLH(camPos, pivot, worldUp);
	}
}

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
    virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
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
	void UpdateCamera(float dt);
	void InitializeOrbitFromSelection();
	void SetMouseLookActive(bool active);
	void CenterMouseCursor();
	void UpdateCursorClip();
	// 창 이동/리사이즈 후에도 clip·커서 중앙을 현재 Scene 스크린 위치에 맞춤.
	void RefreshMouseLookCursorPlacement();
	void SyncMouseLookState();
	void RegisterMouseRawInput();

private:
	float mTheta = 0.0f; // yaw
	float mPhi = 0.0f; // pitch
	float mCamX = 0.0f;
	float mCamY = 5.0f;
	float mCamZ = -10.0f;
	float mFlySpeed = 11.0f; // Minecraft creative fly speed (blocks/s)
	float mMouseSensitivity = 0.12f; // degrees per pixel
	XMFLOAT4X4 mView = {};
	XMFLOAT4X4 mProj = {};
	bool mMouseLookActive = false;
	// false로 시작: 앱 실행 직후부터 커서를 가두고 있으면 창 크기 조절/UI 클릭이 안 됨.
	bool mMouseLookRequested = false;
	float mPendingMouseDx = 0.0f;
	float mPendingMouseDy = 0.0f;

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
	bool mKeyCtrl = false;

	bool mManipulateSelected = false;
	bool mWasManipulateSelected = false;
	float mOrbitRadius = 10.0f;
	Entity mOrbitTarget = INVALID_ENTITY;

	// For picking
	DirectX::XMMATRIX mCurrentView = DirectX::XMMatrixIdentity();
	DirectX::XMMATRIX mCurrentProj = DirectX::XMMatrixIdentity();
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

	RegisterMouseRawInput();

	ThrowIfFailed(mCommandList->Reset(mFrameResources[0]->CmdListAlloc.Get(), nullptr));

	InitializeCoreSystems();
	LoadAssets();
	CreateInitialScene();

	// 렌더 경로 (기본 ComputeIndirect = GPU-driven)
	// mEngine.SetRenderPath(RenderPath::Basic);            // 1: 원본 per-object
	// mEngine.SetRenderPath(RenderPath::Instanced);        // 2: CPU 인스턴싱 폴백
	// mEngine.SetRenderPath(RenderPath::ComputeIndirect);  // 3: GPU-driven (기본)
	// mEngine.SetGpuFrustumCullEnabled(false);             // 경로3 컬링 (기본 true)
	mEngine.SetRenderPath(RenderPath::ComputeIndirect);
	mEngine.SetGpuFrustumCullEnabled(true);

	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
	FlushCommandQueue();

	UpdateCamera(0.0f);
	SyncMouseLookState();

	return true;
}

void InitDirect3DApp::OnResize()
{
	D3DApp::OnResize();

	const float aspect = AspectRatio();
	const float fovY = 2.0f * atanf(tanf(XMConvertToRadians(70.0f) * 0.5f) / aspect);
	XMMATRIX proj = XMMatrixPerspectiveFovLH(fovY, aspect, 0.1f, 1000.0f);
	XMStoreFloat4x4(&mProj, proj);

	// 스왑체인 리사이즈 직후 clip/센터를 현재 창 위치로 재적용.
	// 이유: 이동/최대화 후 이전 스크린 좌표 clip이 남으면 커서가 창 밖에 묶임.
	// 클라이언트 상대 Scene rect + ClientToScreen 변환이라 창 위치는 즉시 반영됨.
	if (mMouseLookActive)
		RefreshMouseLookCursorPlacement();
}

void InitDirect3DApp::RegisterMouseRawInput()
{
	RAWINPUTDEVICE rid{};
	rid.usUsagePage = 0x01;
	rid.usUsage = 0x02;
	rid.dwFlags = RIDEV_INPUTSINK;
	rid.hwndTarget = mhMainWnd;
	RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));
}

LRESULT InitDirect3DApp::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// ESC는 기본 MsgProc에서 앱 종료로 처리된다.
	// 마우스 룩 중에는 종료 대신 고정만 풀어 에디터 UI/창 조절이 가능하게 한다.
	if (msg == WM_KEYUP && wParam == VK_ESCAPE)
	{
		if (mMouseLookRequested || mMouseLookActive)
		{
			mMouseLookRequested = false;
			// 요청 플래그와 실제 캡처 상태를 같이 끈다.
			// 이유: 플래그만 끄면 다음 Sync/ACTIVATE에서 다시 켜질 수 있음.
			SetMouseLookActive(false);
			return 0;
		}
		// 룩이 꺼진 상태의 ESC는 기존처럼 base에서 종료 처리.
	}

	// 창 이동/크기 변경 중에는 Update가 pause되어 clip이 옛 스크린 좌표에 남을 수 있음.
	// 메시지 시점에 바로 재배치한다. 이유: pause 루프는 Sleep만 하고 DrawScenePanel을 안 돌림.
	if (mMouseLookActive
		&& (msg == WM_MOVE || msg == WM_SIZE || msg == WM_EXITSIZEMOVE || msg == WM_DISPLAYCHANGE))
	{
		RefreshMouseLookCursorPlacement();
	}

	if (msg == WM_ACTIVATE)
	{
		if (LOWORD(wParam) == WA_INACTIVE)
			SetMouseLookActive(false);
		else if (mMouseLookRequested)
			SetMouseLookActive(true);
	}
	else if (msg == WM_INPUT && mMouseLookActive)
	{
		UINT size = 0;
		GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
		if (size == 0)
			return 0;

		std::vector<BYTE> data(size);
		if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, data.data(), &size, sizeof(RAWINPUTHEADER)) != size)
			return 0;

		const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(data.data());
		if (raw->header.dwType == RIM_TYPEMOUSE
			&& raw->data.mouse.usFlags == MOUSE_MOVE_RELATIVE)
		{
			const LONG mx = raw->data.mouse.lLastX;
			const LONG my = raw->data.mouse.lLastY;
			if (mx != 0 || my != 0)
			{
				mPendingMouseDx += XMConvertToRadians(mMouseSensitivity * static_cast<float>(mx));
				mPendingMouseDy += XMConvertToRadians(mMouseSensitivity * static_cast<float>(my));
			}
		}
		return 0;
	}

	return D3DApp::MsgProc(hwnd, msg, wParam, lParam);
}

void InitDirect3DApp::Update(const GameTimer& gt)
{
	mImGuiManager.NewFrame();
	mImGuiManager.SetupDockspace(); 
	mImGuiManager.DrawScenePanel();
	mImGuiManager.DrawHierarchyPanel(&mEngine);
	mImGuiManager.DrawInspectorPanel(&mEngine);
	mImGuiManager.DrawProjectPanel();

	// Inspector 체크박스 → 앱의 3인칭 조작 모드 동기화
	mManipulateSelected = mImGuiManager.IsManipulateSelected();

	// Scene 드래그 박스 다중 선택 (Shift = 추가 선택)
	{
		float bx0, by0, bx1, by1;
		bool additive = false;
		if (mImGuiManager.ConsumeBoxSelection(bx0, by0, bx1, by1, additive))
		{
			const SceneViewport& sceneVP = mImGuiManager.GetSceneViewport();
			const float pickW = sceneVP.IsValid()
				? static_cast<float>(sceneVP.GetWidth())
				: static_cast<float>(mImGuiManager.GetDesiredSceneWidth());
			const float pickH = sceneVP.IsValid()
				? static_cast<float>(sceneVP.GetHeight())
				: static_cast<float>(mImGuiManager.GetDesiredSceneHeight());

			// UI 박스 좌표는 패널 픽셀 기준 → RT 해상도로 스케일
			const float uiW = static_cast<float>((std::max)(1u, mImGuiManager.GetDesiredSceneWidth()));
			const float uiH = static_cast<float>((std::max)(1u, mImGuiManager.GetDesiredSceneHeight()));
			const float sx = pickW / uiW;
			const float sy = pickH / uiH;

			const size_t n = mEngine.SelectObjectsInRect(
				bx0 * sx, by0 * sy, bx1 * sx, by1 * sy,
				pickW, pickH,
				mCurrentView, mCurrentProj,
				additive);

			char buf[96];
			sprintf_s(buf, "[Select] box select count=%zu additive=%d\n", n, additive ? 1 : 0);
			OutputDebugStringA(buf);
		}
	}

	// Scene 짧은 좌클릭 → 마우스 룩 (드래그 선택은 위 박스 처리)
	if (mImGuiManager.ConsumeSceneCaptureClick())
	{
		mMouseLookRequested = true;
		SyncMouseLookState();
	}

	float dt = gt.DeltaTime();
	if (dt > 0.033f)
		dt = 0.033f;

	UpdateCamera(dt);
	mEngine.Update(dt);

	// 패널 리사이즈로 Scene 영역이 바뀌면 clip rect도 다시 맞춰야 함.
	// 이유: ClipCursor가 옛 Scene 박스에 묶여 있으면 커서가 어색하게 막힘.
	SyncMouseLookState();
}

void InitDirect3DApp::InitializeOrbitFromSelection()
{
	const Entity selected = mEngine.GetSelectedEntity();
	TransformComponent* tf = mEngine.GetTransform(selected);
	if (!tf)
	{
		mOrbitTarget = INVALID_ENTITY;
		return;
	}

	mOrbitTarget = selected;

	XMVECTOR pivot = XMLoadFloat3(&tf->position);
	XMVECTOR camPos = XMVectorSet(mCamX, mCamY, mCamZ, 1.0f);
	XMVECTOR toCam = XMVectorSubtract(camPos, pivot);
	const float dist = XMVectorGetX(XMVector3Length(toCam));

	if (dist > 0.001f)
	{
		mOrbitRadius = MathHelper::Clamp(dist, kMinOrbitRadius, kMaxOrbitRadius);
		XMVECTOR offsetDir = XMVector3Normalize(toCam);
		XMFLOAT3 o{};
		XMStoreFloat3(&o, offsetDir);
		mPhi = asinf(MathHelper::Clamp(o.y, -1.0f, 1.0f));
		mTheta = atan2f(o.x, o.z) - tf->rotation.y;
	}
	else
	{
		mTheta = 0.0f;
		mPhi = kDefaultOrbitPitch;
		mOrbitRadius = kMinOrbitRadius;
	}
}

void InitDirect3DApp::UpdateCamera(float dt)
{
	const Entity selected = mEngine.GetSelectedEntity();
	const bool thirdPersonMode = mManipulateSelected && selected != INVALID_ENTITY;

	if (thirdPersonMode && selected != mOrbitTarget)
		InitializeOrbitFromSelection();
	else if (!thirdPersonMode)
		mOrbitTarget = INVALID_ENTITY;

	if (mPendingMouseDx != 0.0f || mPendingMouseDy != 0.0f)
	{
		mTheta += mPendingMouseDx;
		mPhi += mPendingMouseDy;
		mPendingMouseDx = 0.0f;
		mPendingMouseDy = 0.0f;
	}

	mPhi = MathHelper::Clamp(mPhi, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);

	XMMATRIX yawRot = XMMatrixRotationY(mTheta);
	XMMATRIX rot = XMMatrixRotationRollPitchYaw(mPhi, mTheta, 0.0f);
	XMVECTOR lookForward = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rot);
	XMVECTOR horizRight = XMVector3TransformNormal(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), yawRot);
	XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	float speed = mFlySpeed * dt;
	if (mKeyCtrl)
		speed *= 2.0f;

	float fwdAmt = (mKeyW ? 1.f : 0.f) - (mKeyS ? 1.f : 0.f);
	float strafeAmt = (mKeyD ? 1.f : 0.f) - (mKeyA ? 1.f : 0.f);
	float ascendAmt = (mKeySpace ? 1.f : 0.f) - (mKeyShift ? 1.f : 0.f);

	float horizLenSq = fwdAmt * fwdAmt + strafeAmt * strafeAmt;
	if (horizLenSq > 1.0f)
	{
		float invLen = 1.0f / sqrtf(horizLenSq);
		fwdAmt *= invLen;
		strafeAmt *= invLen;
	}

	XMVECTOR horizForward = FlattenHorizForward(lookForward, yawRot);
	XMFLOAT3 horizFwd{};
	XMFLOAT3 horizRgt{};
	XMStoreFloat3(&horizFwd, horizForward);
	XMStoreFloat3(&horizRgt, horizRight);

	if (thirdPersonMode)
	{
		TransformComponent* targetTf = mEngine.GetTransform(selected);
		if (!targetTf)
		{
			mOrbitTarget = INVALID_ENTITY;
			mEngine.ClearSelection();

			XMVECTOR pos = XMVectorSet(mCamX, mCamY, mCamZ, 1.0f);
			XMMATRIX view = XMMatrixLookToLH(pos, lookForward, worldUp);
			XMStoreFloat4x4(&mView, view);
			mCurrentView = view;
			mCurrentProj = XMLoadFloat4x4(&mProj);
			return;
		}

		if (fwdAmt != 0.0f || strafeAmt != 0.0f || ascendAmt != 0.0f)
			mEngine.MoveSelectedPlanar(fwdAmt, strafeAmt, ascendAmt, speed, horizFwd, horizRgt);

		XMVECTOR pivot = XMLoadFloat3(&targetTf->position);
		XMVECTOR camPos = ComputeThirdPersonCameraPosition(
			pivot, targetTf->rotation.y, mPhi, mTheta, mOrbitRadius);

		XMMATRIX view = BuildOrbitView(camPos, pivot, worldUp, mCamX, mCamY, mCamZ);
		XMStoreFloat4x4(&mView, view);
		mCurrentView = view;
		mCurrentProj = XMLoadFloat4x4(&mProj);
		return;
	}

	XMVECTOR pos = XMVectorSet(mCamX, mCamY, mCamZ, 1.0f);
	if (fwdAmt != 0.0f || strafeAmt != 0.0f || ascendAmt != 0.0f)
	{
		pos = XMVectorAdd(pos, XMVectorScale(horizForward, fwdAmt * speed));
		pos = XMVectorAdd(pos, XMVectorScale(horizRight, strafeAmt * speed));
		pos = XMVectorAdd(pos, XMVectorScale(worldUp, ascendAmt * speed));

		XMFLOAT3 p{};
		XMStoreFloat3(&p, pos);
		mCamX = p.x;
		mCamY = p.y;
		mCamZ = p.z;
	}

	lookForward = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rot);
	pos = XMVectorSet(mCamX, mCamY, mCamZ, 1.0f);
	XMMATRIX view = XMMatrixLookToLH(pos, lookForward, worldUp);
	XMStoreFloat4x4(&mView, view);
	mCurrentView = view;
	mCurrentProj = XMLoadFloat4x4(&mProj);
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

	// Scene RT 리사이즈는 커맨드 리스트가 열리기 전에 처리 (GPU idle 보장)
	mImGuiManager.EnsureSceneViewport([this]() { FlushCommandQueue(); });

	// 3. Allocator + CommandList Reset
	ThrowIfFailed(mCurrFrameResource->CmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(mCurrFrameResource->CmdListAlloc.Get(), nullptr));

	// 4. 백버퍼는 에디터 UI(ImGui)용. 3D는 Scene offscreen RT에 그림.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	const float editorClear[] = { 0.12f, 0.12f, 0.14f, 1.0f };
	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), editorClear, 0, nullptr);
}

void InitDirect3DApp::Draw(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mView);

	// === Scene 패널 비율로 projection 재계산 ===
	SceneViewport& sceneVP = mImGuiManager.GetSceneViewport();
	const float aspect = sceneVP.IsValid() ? sceneVP.GetAspectRatio() : AspectRatio();
	const float fovY = 2.0f * atanf(tanf(XMConvertToRadians(70.0f) * 0.5f) / aspect);
	XMMATRIX proj = XMMatrixPerspectiveFovLH(fovY, aspect, 0.1f, 1000.0f);
	XMStoreFloat4x4(&mProj, proj);
	mCurrentView = view;
	mCurrentProj = proj;

	// === ECS CameraComponent 동기화 (mView 기준 실제 시선 방향) ===
	if (mMainCamera != INVALID_ENTITY)
	{
		float camPitch = mPhi;
		float camYaw = mTheta;
		ExtractPitchYawFromView(view, camPitch, camYaw);

		XMFLOAT3 camPos = { mCamX, mCamY, mCamZ };
		mEngine.SetCameraTransform(mMainCamera, camPos, { camPitch, camYaw, 0.0f });
	}

	// === 1) Scene offscreen RT에 3D 렌더 ===
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
	}

	// === 2) 백버퍼에 ImGui (Scene 패널이 offscreen 결과를 Image로 표시) ===
	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

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
	// SyncInterval 0 = VSync OFF (프레임 상한을 모니터 주사율에 묶지 않음)
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
		gNumFrameResources, mBackBufferFormat, mDepthStencilFormat, mGlobalDescriptorAllocator, this);

	// === Engine 초기화 ===
	mEngine.Initialize(md3dDevice.Get(), mFrameResources, gNumFrameResources, mGlobalDescriptorAllocator);
}

// =====================================================
// 에셋 로딩 (Material + Mesh)
// =====================================================
void InitDirect3DApp::LoadAssets()
{
	// 바인딩 실패 시 쓸 마젠타 1x1 디버그 머티리얼 (다른 머티리얼보다 먼저)
	MaterialManager::Get().EnsureMissingTextureMaterial(
		md3dDevice.Get(), mCommandList.Get(), mGlobalDescriptorAllocator);

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


	// Mesh 로딩 (OBJ 모델 + GeometryGenerator 프리미티브)
	bool meshResult = MeshManager::Get().CreateMesh("bibian", L"Resources/Assets/bibian.obj",
		md3dDevice.Get(), mCommandList.Get());
	bool meshResult1 = MeshManager::Get().CreateMesh("box", L"Resources/Assets/square.obj",
		md3dDevice.Get(), mCommandList.Get());

	GeometryGenerator geo;
	bool primOk = true;
	primOk &= MeshManager::Get().CreateMeshFromGeometry(
		"cube", geo.CreateBox(1.0f, 1.0f, 1.0f, 0),
		md3dDevice.Get(), mCommandList.Get());
	primOk &= MeshManager::Get().CreateMeshFromGeometry(
		"sphere", geo.CreateSphere(1.0f, 20, 20),
		md3dDevice.Get(), mCommandList.Get());
	primOk &= MeshManager::Get().CreateMeshFromGeometry(
		"geosphere", geo.CreateGeosphere(1.0f, 2),
		md3dDevice.Get(), mCommandList.Get());
	primOk &= MeshManager::Get().CreateMeshFromGeometry(
		"cylinder", geo.CreateCylinder(0.5f, 0.5f, 2.0f, 20, 4),
		md3dDevice.Get(), mCommandList.Get());
	primOk &= MeshManager::Get().CreateMeshFromGeometry(
		"cone", geo.CreateCylinder(0.5f, 0.0f, 2.0f, 20, 4),
		md3dDevice.Get(), mCommandList.Get());
	primOk &= MeshManager::Get().CreateMeshFromGeometry(
		"grid", geo.CreateGrid(10.0f, 10.0f, 10, 10),
		md3dDevice.Get(), mCommandList.Get());

	if (!meshResult || !meshResult1 || !primOk)
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
	mMainCamera = mEngine.CreateMainCamera({ 0.0f, 5.0f, -5.0f });

	// 기존 렌더 오브젝트
	mEngine.CreateRenderableEntity("bibian", "Test", { 0.0f, 0.0f, 0.0f });
}

#pragma region Input Handling
//입력처리
void InitDirect3DApp::SetMouseLookActive(bool active)
{
	if (mMouseLookActive == active)
	{
		// 이미 켜져 있어도 매 프레임 clip/센터를 갱신한다.
		// 이유: Scene 패널 크기 변경 + 창 이동이 Update 경로로만 올 때도 즉시 반영.
		if (active)
			RefreshMouseLookCursorPlacement();
		return;
	}

	mMouseLookActive = active;
	ImGuiIO& io = ImGui::GetIO();
	if (active)
	{
		// 마우스 룩 중 ImGui가 마우스 이벤트를 먹으면 도킹 패널이 드래그될 수 있음.
		// 이유: 커서 숨김+캡처 상태에서도 ImGui는 Scene 위 마우스를 계속 받음.
		io.ConfigFlags |= ImGuiConfigFlags_NoMouse;

		while (ShowCursor(FALSE) >= 0) {}
		SetCapture(mhMainWnd);
		RefreshMouseLookCursorPlacement();
	}
	else
	{
		// 룩 해제 시 ImGui 마우스 입력 복구 — UI 클릭/창 조작 가능해야 함.
		io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

		while (ShowCursor(TRUE) < 0) {}
		ReleaseCapture();
		// clip 해제로 OS가 창 테두리/바깥으로 커서 이동 허용 → 창 크기 조절 가능.
		ClipCursor(nullptr);
	}
}

void InitDirect3DApp::CenterMouseCursor()
{
	if (!mhMainWnd)
		return;

	// 가능하면 Scene 뷰 중앙으로 보낸다.
	// 이유: 전체 클라이언트 중앙은 Hierarchy/Inspector 쪽일 수 있어, 고정 범위(Scene)와 맞춤.
	// TryGetSceneScreenRect는 호출 시점 ClientToScreen을 쓰므로 창 이동 후에도 맞음.
	RECT sceneRect{};
	if (mImGuiManager.TryGetSceneScreenRect(sceneRect))
	{
		const POINT center{
			(sceneRect.left + sceneRect.right) / 2,
			(sceneRect.top + sceneRect.bottom) / 2
		};
		SetCursorPos(center.x, center.y);
		return;
	}

	POINT center{
		mClientWidth / 2,
		mClientHeight / 2
	};
	ClientToScreen(mhMainWnd, &center);
	SetCursorPos(center.x, center.y);
}

void InitDirect3DApp::UpdateCursorClip()
{
	if (!mhMainWnd)
		return;

	// Scene 패널 Image 영역으로만 커서를 가둔다.
	// 이유: 전체 창 clip이면 룩 중에도 창 가장자리 동작이 꼬이고, 요청은 "Scene에만 고정"임.
	// 스크린 RECT는 매번 현재 창 위치 기준으로 재계산된다.
	RECT sceneRect{};
	if (mImGuiManager.TryGetSceneScreenRect(sceneRect))
	{
		ClipCursor(&sceneRect);
		return;
	}

	// Scene rect가 아직 없으면(첫 프레임 등) 임시로 클라이언트 전체.
	// 이유: clip 없이 SetCapture만 하면 커서가 창 밖으로 나가 입력이 끊길 수 있음.
	RECT rect{};
	GetClientRect(mhMainWnd, &rect);
	MapWindowPoints(mhMainWnd, HWND_DESKTOP, reinterpret_cast<LPPOINT>(&rect), 2);
	ClipCursor(&rect);
}

void InitDirect3DApp::RefreshMouseLookCursorPlacement()
{
	// clip 범위와 커서 중앙을 한 묶음으로 갱신.
	// 이유: 창만 옮기고 clip만 바꾸면 커서가 새 영역 밖에 남아 ClipCursor가 이상 동작할 수 있음.
	if (!mMouseLookActive)
		return;

	UpdateCursorClip();
	CenterMouseCursor();
}

void InitDirect3DApp::SyncMouseLookState()
{
	// WantCaptureMouse 조건은 제거했다.
	// 이유: Scene 자체가 ImGui 창이라, 그 조건을 쓰면 Scene 클릭 후에도 룩이 바로 꺼짐.
	// pause 중에는 끄지 않는다.
	// 이유: 이동/리사이즈 중 Update가 안 돌아 Sync로 끄는 경로가 없고,
	//       메시지 핸들러의 RefreshMouseLookCursorPlacement가 clip을 유지/갱신한다.
	//       pause 때 강제로 끄면 EXITSIZEMOVE 전후로 커서 표시가 깜빡임.
	const bool wantActive = mMouseLookRequested
		&& GetForegroundWindow() == mhMainWnd;
	SetMouseLookActive(wantActive);
}

void InitDirect3DApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	// 휠 클릭 토글은 제거/비활성.
	// 이유: 진입은 Scene 좌클릭, 해제는 ESC로 통일해 입력 경로를 단순화함.

	if (btnState & MK_RBUTTON)
	{
		// 마우스 룩 중이 아닐 때만 ImGui 점유를 존중한다.
		// 이유: 룩 중에는 NoMouse로 ImGui를 끄지만, 자유 모드에서는 UI 위 우클릭이 피킹되면 안 됨.
		if (!mMouseLookActive && ImGui::GetIO().WantCaptureMouse)
			return;

		// 피킹 해상도는 Scene RT 기준으로 맞춘다 (창 전체가 아님).
		// 이유: 3D는 Scene offscreen에 그려지므로 pick 정규화도 그 크기여야 함.
		const SceneViewport& sceneVP = mImGuiManager.GetSceneViewport();
		const int pickW = sceneVP.IsValid() ? static_cast<int>(sceneVP.GetWidth()) : mClientWidth;
		const int pickH = sceneVP.IsValid() ? static_cast<int>(sceneVP.GetHeight()) : mClientHeight;

		int pickX = x;
		int pickY = y;
		if (mMouseLookActive)
		{
			// 룩 중 커서는 Scene 중앙에 있으므로 중앙 픽셀로 피킹.
			pickX = pickW / 2;
			pickY = pickH / 2;
		}
		else
		{
			// 자유 모드: 창 클라이언트 좌표 → Scene 이미지 로컬 좌표.
			// 이유: OnMouseDown의 x,y는 윈도우 클라이언트 기준이고, Scene Image는 그 안 일부임.
			RECT sceneScreen{};
			if (mImGuiManager.TryGetSceneScreenRect(sceneScreen))
			{
				POINT pt{ x, y };
				ClientToScreen(mhMainWnd, &pt);
				pickX = pt.x - sceneScreen.left;
				pickY = pt.y - sceneScreen.top;
				if (pickX < 0 || pickY < 0 || pickX >= pickW || pickY >= pickH)
					return;
			}
		}

		// Shift+RMB = 토글 추가 선택
		const bool additive = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		Entity picked = mEngine.PickObject(
			pickX, pickY, (float)pickW, (float)pickH,
			mCurrentView, mCurrentProj,
			true, additive);
		if (picked != INVALID_ENTITY)
		{
			OutputDebugStringA("Object picked!\n");
			if (mManipulateSelected)
				InitializeOrbitFromSelection();
		}
	}
}

void InitDirect3DApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	(void)btnState;
	(void)x;
	(void)y;
}

void InitDirect3DApp::OnMouseMove(WPARAM btnState, int x, int y)
{
	(void)btnState;
	(void)x;
	(void)y;
}

void InitDirect3DApp::OnMouseWheel(short wheelDelta, int x, int y)
{
	(void)x;
	(void)y;

	const float factor = 1.1f;
	const float scroll = wheelDelta / 120.0f;

	if (mManipulateSelected && mEngine.GetSelectedEntity() != INVALID_ENTITY)
	{
		mOrbitRadius *= powf(factor, scroll);
		mOrbitRadius = MathHelper::Clamp(mOrbitRadius, kMinOrbitRadius, kMaxOrbitRadius);
	}
	else
	{
		mFlySpeed *= powf(1.2f, scroll);
		mFlySpeed = MathHelper::Clamp(mFlySpeed, 1.0f, 500.0f);
	}
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
	case VK_CONTROL: mKeyCtrl = true; break;
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
	case VK_CONTROL: mKeyCtrl = false; break;
	}
}

void InitDirect3DApp::buttonClicked(ButtonAction action)
{
	if (action == ButtonAction::SpawnTestObject)
	{
		mEngine.CreateRenderableEntity("bibian", "Test", { 0, 1, 0 });
	}
	else if (action == ButtonAction::ToggleManipulateSelected)
	{
		mManipulateSelected = mImGuiManager.IsManipulateSelected();
		if (mManipulateSelected && mEngine.GetSelectedEntity() != INVALID_ENTITY)
			InitializeOrbitFromSelection();
		else
			mOrbitTarget = INVALID_ENTITY;
	}
	else if (action == ButtonAction::SpawnSelectedMesh)
	{
		std::string mesh = mImGuiManager.GetSelectedMesh();
		std::string mat = mImGuiManager.GetSelectedMaterial();
		if (mesh.empty()) mesh = "bibian";

		// Scene 중앙 십자선 = 현재 뷰 시선 방향. 카메라 앞 고정 거리에 스폰.
		constexpr float kSpawnDistance = 8.0f;
		XMMATRIX view = mCurrentView;
		XMVECTOR det;
		XMMATRIX invView = XMMatrixInverse(&det, view);
		XMVECTOR camPos = XMVector3TransformCoord(XMVectorZero(), invView);
		XMVECTOR lookDir = XMVector3Normalize(
			XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), invView));
		XMVECTOR spawnVec = XMVectorAdd(camPos, XMVectorScale(lookDir, kSpawnDistance));

		XMFLOAT3 spawnPos{};
		XMStoreFloat3(&spawnPos, spawnVec);
		mEngine.CreateRenderableEntity(mesh, mat, spawnPos);
	}
}
#pragma endregion

void InitDirect3DApp::OnDestroy()
{
	SetMouseLookActive(false);
	FlushCommandQueue();

	mImGuiManager.Shutdown();
	mEngine.Shutdown();

	MeshManager::Get().Shutdown();
	MaterialManager::Get().Shutdown();
	mGlobalDescriptorAllocator.Shutdown();

	D3DApp::OnDestroy();
}

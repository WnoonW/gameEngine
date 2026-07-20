#include "ImGuiManager.h"
#include "Managers/MeshManager.h"
#include "Managers/MaterialManager.h"
#include "Engine.h"
#include "Entity.h"
#include <algorithm>
#include <vector>

namespace
{
    void LoadUIFonts(ImGuiIO& io)
    {
        io.Fonts->Clear();

        const float fontSize = 18.0f;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;

        const char* koreanFont = "C:\\Windows\\Fonts\\malgun.ttf";
        const char* japaneseFont = "C:\\Windows\\Fonts\\meiryo.ttc";
        const char* chineseFont = "C:\\Windows\\Fonts\\msyh.ttc";

        if (ImFont* font = io.Fonts->AddFontFromFileTTF(koreanFont, fontSize, &cfg))
        {
            ImFontConfig mergeCfg;
            mergeCfg.MergeMode = true;
            mergeCfg.PixelSnapH = true;
            mergeCfg.FontNo = 0;
            io.Fonts->AddFontFromFileTTF(japaneseFont, fontSize, &mergeCfg);
            io.Fonts->AddFontFromFileTTF(chineseFont, fontSize, &mergeCfg);
            return;
        }

        if (io.Fonts->AddFontFromFileTTF(chineseFont, fontSize, &cfg))
            return;

        io.Fonts->AddFontDefault();
    }
}

bool ImGuiManager::Initialize(
    HWND hwnd,
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    UINT numFramesInFlight,
    DXGI_FORMAT rtvFormat,
    DXGI_FORMAT depthFormat,
    DescriptorAllocator& globalDescriptorAllocator,
    IFunctionCallback* callback)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    LoadUIFonts(io);

    static DescriptorAllocator* s_DescriptorAllocator = nullptr;
    s_DescriptorAllocator = &globalDescriptorAllocator;

    m_Device = device;
    // ClipCursor/ClientToScreen 변환에 필요. 창 위치 변경 시 스크린 좌표를 다시 계산한다.
    m_Hwnd = hwnd;
    m_Callback = callback;
    m_DescriptorAllocator = &globalDescriptorAllocator;
    mDepthFormat = depthFormat;

    ImGui_ImplDX12_InitInfo init_info = {};
    init_info.Device = device;
    init_info.CommandQueue = commandQueue;
    init_info.NumFramesInFlight = numFramesInFlight;
    init_info.RTVFormat = rtvFormat;
    init_info.SrvDescriptorHeap = globalDescriptorAllocator.GetHeap();

    init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*,
        D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
        D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle)
        {
            auto h = s_DescriptorAllocator->Allocate();
            *out_cpu_handle = h.CPU;
            *out_gpu_handle = h.GPU;
        };

    init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                       D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
    {
        DescriptorAllocator::DescriptorHandle h;
        h.CPU = cpu_handle;
        h.GPU = gpu_handle;
        s_DescriptorAllocator->Free(h); 
    };

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(&init_info);

    mSceneViewport.Initialize(device, globalDescriptorAllocator, rtvFormat, depthFormat);

    return true;
}

namespace
{
    auto MainMaterialComboGetter = [](void* data, int idx) -> const char*
    {
        if (idx == 0) return "None";
        auto* names = static_cast<std::vector<std::string>*>(data);
        if (idx - 1 < 0 || idx - 1 >= (int)names->size()) return nullptr;
        return (*names)[idx - 1].c_str();
    };

    auto SubMaterialComboGetter = [](void* data, int idx) -> const char*
    {
        if (idx == 0) return "(Cascade)";
        auto* names = static_cast<std::vector<std::string>*>(data);
        if (idx - 1 < 0 || idx - 1 >= (int)names->size()) return nullptr;
        return (*names)[idx - 1].c_str();
    };
}

/*void ImGuiManager::CustomUI(Engine* engine)
{
    ImGui::Begin("V3.0-UI Debug Window");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

    if (ImGui::Button("Reset Scene"))
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::ResetScene);

    if (ImGui::Button("Spawn Test Object"))
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::SpawnTestObject);

    if (ImGui::Button("Toggle Wireframe"))
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::ToggleWireframe);

    if (ImGui::Checkbox("Manipulate Selected Object", &mManipulateSelected)) {
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::ToggleManipulateSelected);
    }
    if (mManipulateSelected) {
        ImGui::Text("3rd-person follow mode ON");
        ImGui::Text("Mouse: orbit | WASD: XZ move | Space/Shift: Y move | Wheel: zoom");
    }

    // === Loaded Meshes & Object Creator ===
    ImGui::Separator();
    ImGui::Text("Loaded Meshes & Spawn");

    auto meshNames = MeshManager::Get().GetLoadedMeshNames();
    if (meshNames.empty()) {
        ImGui::Text("No meshes loaded");
    } else {
        if (std::find(meshNames.begin(), meshNames.end(), mSelectedMesh) == meshNames.end() && !meshNames.empty()) {
            mSelectedMesh = meshNames[0];
        }

        int meshIdx = 0;
        for (size_t i = 0; i < meshNames.size(); ++i) {
            if (meshNames[i] == mSelectedMesh) {
                meshIdx = (int)i;
                break;
            }
        }

        auto meshGetter = [](void* data, int idx) -> const char* {
            auto* vec = (std::vector<std::string>*)data;
            if (idx < 0 || idx >= (int)vec->size()) return nullptr;
            return (*vec)[idx].c_str();
        };
        if (ImGui::Combo("Mesh", &meshIdx, meshGetter, &meshNames, (int)meshNames.size())) {
            mSelectedMesh = meshNames[meshIdx];
        }
    }

    auto matNames = MaterialManager::Get().GetLoadedMaterialNames();
    std::sort(matNames.begin(), matNames.end());
    if (!matNames.empty()) {
        int matIdx = 0;
        if (!mSelectedMaterial.empty())
        {
            auto it = std::find(matNames.begin(), matNames.end(), mSelectedMaterial);
            if (it != matNames.end())
                matIdx = 1 + (int)std::distance(matNames.begin(), it);
        }

        if (ImGui::Combo("Main Material (spawn)", &matIdx, MainMaterialComboGetter, &matNames, (int)matNames.size() + 1)) {
            mSelectedMaterial = (matIdx == 0) ? "" : matNames[matIdx - 1];
        }
    } else {
        mSelectedMaterial.clear();
    }

    if (ImGui::Button("Spawn Selected Mesh")) {
        if (m_Callback && !mSelectedMesh.empty()) {
            m_Callback->buttonClicked(ButtonAction::SpawnSelectedMesh);
        }
    }

    if (!mSelectedMesh.empty()) {
        const char* spawnMat = mSelectedMaterial.empty() ? "None (Init)" : mSelectedMaterial.c_str();
        ImGui::Text("Spawn: %s / %s", mSelectedMesh.c_str(), spawnMat);
    }

    if (engine)
    {
        Entity selected = engine->GetSelectedEntity();
        if (selected != INVALID_ENTITY)
        {
            ImGui::Separator();
            ImGui::Text("Selected Entity: %u", selected);

            bool hasGravity = engine->HasGravityComponent(selected);
            if (ImGui::Checkbox("Gravity Component", &hasGravity))
                engine->SetEntityGravityEnabled(selected, hasGravity);

            if (GravityComponent* gravity = engine->GetGravityComponent(selected))
            {
                ImGui::Indent();
                ImGui::Checkbox("Enabled##Gravity", &gravity->enabled);
                ImGui::DragFloat("Strength", &gravity->strength, 0.1f, 0.0f, 50.0f);
                ImGui::DragFloat3("Velocity", &gravity->velocity.x, 0.1f);
                ImGui::Unindent();
            }

            bool hasCollision = engine->HasCollisionComponent(selected);
            if (ImGui::Checkbox("Collision Component", &hasCollision))
                engine->SetEntityCollisionEnabled(selected, hasCollision);

            if (CollisionComponent* collision = engine->GetCollisionComponent(selected))
            {
                ImGui::Indent();
                ImGui::Checkbox("Enabled##Collision", &collision->enabled);
                ImGui::Checkbox("Static", &collision->isStatic);
                ImGui::DragFloat("Restitution", &collision->restitution, 0.01f, 0.0f, 1.0f);
                ImGui::Unindent();
            }
        }

        RenderableComponent* rend = engine->GetRenderable(selected);
        if (rend && rend->mesh)
        {
            ImGui::Separator();
            ImGui::Text("Entity Material (Sub > Main > Init)");

            auto matNames = MaterialManager::Get().GetLoadedMaterialNames();
            std::sort(matNames.begin(), matNames.end());

            const std::string mainMaterialName = engine->GetEntityMainMaterial(selected);
            int mainIdx = 0;
            if (!mainMaterialName.empty())
            {
                auto it = std::find(matNames.begin(), matNames.end(), mainMaterialName);
                if (it != matNames.end())
                    mainIdx = 1 + (int)std::distance(matNames.begin(), it);
            }

            if (ImGui::Combo("Main Material", &mainIdx, MainMaterialComboGetter, &matNames, (int)matNames.size() + 1))
            {
                std::string newMain = (mainIdx == 0) ? "" : matNames[mainIdx - 1];
                engine->SetEntityMainMaterial(selected, newMain);
            }

            ImGui::Text("Submesh Overrides");
            std::vector<std::string> submeshKeys;
            submeshKeys.reserve(rend->mesh->DrawArgs.size());
            for (const auto& pair : rend->mesh->DrawArgs)
                submeshKeys.push_back(pair.first);
            std::sort(submeshKeys.begin(), submeshKeys.end());

            for (const auto& key : submeshKeys)
            {
                const auto& sub = rend->mesh->DrawArgs.at(key);
                ImGui::PushID(key.c_str());
                ImGui::Text("%s (Init: %s)", key.c_str(),
                    sub.initMaterialName.empty() ? "Default" : sub.initMaterialName.c_str());

                const std::string currentSub = engine->GetEntitySubMaterial(selected, key);

                int subIdx = 0;
                if (!currentSub.empty())
                {
                    auto it = std::find(matNames.begin(), matNames.end(), currentSub);
                    if (it != matNames.end())
                        subIdx = 1 + (int)std::distance(matNames.begin(), it);
                }

                if (ImGui::Combo("Sub Material", &subIdx, SubMaterialComboGetter, &matNames, (int)matNames.size() + 1))
                {
                    std::string newSub = (subIdx == 0) ? "" : matNames[subIdx - 1];
                    engine->SetEntitySubMaterial(selected, key, newSub);
                }
                ImGui::PopID();
            }
        }
        else if (selected != INVALID_ENTITY && !engine->HasGravityComponent(selected) && !engine->HasCollisionComponent(selected))
        {
            ImGui::Separator();
            ImGui::Text("Selected entity has no RenderableComponent");
        }
    }

    ImGui::End();
}*/

#pragma region docking UI
// ==================== Step 1: DockSpace 구조 ====================

void ImGuiManager::SetupDockspace()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##MainDockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    // 최초 실행 시 레이아웃 자동 배치
    static bool first_time = true;
    if (first_time)
    {
        first_time = false;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        ImGuiID dock_id_left, dock_id_right, dock_id_down, dock_id_center;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.20f, &dock_id_left, &dock_id_center);
        ImGui::DockBuilderSplitNode(dock_id_center, ImGuiDir_Right, 0.25f, &dock_id_right, &dock_id_center);
        ImGui::DockBuilderSplitNode(dock_id_center, ImGuiDir_Down, 0.30f, &dock_id_down, &dock_id_center);

        ImGui::DockBuilderDockWindow("Scene", dock_id_center);
        ImGui::DockBuilderDockWindow("Hierarchy", dock_id_left);
        ImGui::DockBuilderDockWindow("Inspector", dock_id_right);
        ImGui::DockBuilderDockWindow("Project", dock_id_down);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::End();
}

// ==================== 각 패널 ====================

void ImGuiManager::DrawScenePanel()
{
    ImGui::Begin("Scene");

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const UINT width = static_cast<UINT>((std::max)(1.0f, avail.x));
    const UINT height = static_cast<UINT>((std::max)(1.0f, avail.y));
    mDesiredSceneWidth = width;
    mDesiredSceneHeight = height;

    // 매 프레임 초기화. 아래 Image/placeholder 기준으로 다시 채운다.
    // 이유: 패널이 안 그려지거나 가려진 프레임에 이전 hover/click이 남지 않게 하기 위함.
    mSceneHovered = false;
    mSceneClientRectValid = false;

    if (mSceneViewport.IsValid())
    {
        // GPU handle stays stable across Resize (SRV descriptor reused).
        ImTextureID texId = (ImTextureID)mSceneViewport.GetSrvGpu().ptr;
        ImGui::Image(texId, avail);

        // Image 아이템 기준으로 hover/영역/클릭을 읽는다.
        // 이유: 창 타이틀바나 다른 UI가 아니라 "Scene 뷰 본체" 클릭일 때만 마우스 고정을 켜야 함.
        mSceneHovered = ImGui::IsItemHovered();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();

        // 스크린 절대좌표가 아니라 클라이언트 상대좌표로 저장한다.
        // 이유: 창을 옮기면 스크린 좌표는 바로 무효가 되고, 이동 중 Update가 멈춰도
        //       ClientToScreen만 다시 하면 clip/센터를 맞출 수 있음.
        const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
        mSceneClientMinX = min.x - vpPos.x;
        mSceneClientMinY = min.y - vpPos.y;
        mSceneClientMaxX = max.x - vpPos.x;
        mSceneClientMaxY = max.y - vpPos.y;
        mSceneClientRectValid = (mSceneClientMaxX > mSceneClientMinX && mSceneClientMaxY > mSceneClientMinY);

        // 왼쪽 클릭으로 캡처 요청 플래그를 세운다 (앱 Update에서 Consume).
        // 이유: Win32 좌표만으로는 도킹된 Scene 패널 위인지 알기 어렵고, ImGui 아이템 hit-test가 정확함.
        if (mSceneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            mSceneCaptureClick = true;
    }
    else
    {
        ImGui::TextDisabled("Scene render target not ready (%u x %u)", width, height);
    }

    ImGui::End();
}

bool ImGuiManager::ConsumeSceneCaptureClick()
{
    // 한 번 읽으면 내린다. 이유: 같은 클릭으로 매 프레임 마우스 룩을 재진입하지 않게 하기 위함.
    const bool clicked = mSceneCaptureClick;
    mSceneCaptureClick = false;
    return clicked;
}

bool ImGuiManager::TryGetSceneClientRect(RECT& outRect) const
{
    if (!mSceneClientRectValid)
        return false;

    outRect.left = static_cast<LONG>(mSceneClientMinX);
    outRect.top = static_cast<LONG>(mSceneClientMinY);
    outRect.right = static_cast<LONG>(mSceneClientMaxX);
    outRect.bottom = static_cast<LONG>(mSceneClientMaxY);
    return outRect.right > outRect.left && outRect.bottom > outRect.top;
}

bool ImGuiManager::TryGetSceneScreenRect(RECT& outRect) const
{
    // 저장은 클라이언트 좌표, 사용 시점에 현재 창 위치로 스크린 변환한다.
    // 이유: 창 이동 직후에도 마지막 Scene 레이아웃을 새 스크린 위치에 바로 적용 가능.
    if (!m_Hwnd)
        return false;

    RECT clientRect{};
    if (!TryGetSceneClientRect(clientRect))
        return false;

    POINT tl{ clientRect.left, clientRect.top };
    POINT br{ clientRect.right, clientRect.bottom };
    if (!ClientToScreen(m_Hwnd, &tl) || !ClientToScreen(m_Hwnd, &br))
        return false;

    outRect.left = tl.x;
    outRect.top = tl.y;
    outRect.right = br.x;
    outRect.bottom = br.y;
    return outRect.right > outRect.left && outRect.bottom > outRect.top;
}

void ImGuiManager::EnsureSceneViewport(const std::function<void()>& flushGpu)
{
    if (!m_Device)
        return;

    const UINT width = (std::max)(1u, mDesiredSceneWidth);
    const UINT height = (std::max)(1u, mDesiredSceneHeight);

    if (mSceneViewport.IsValid()
        && mSceneViewport.GetWidth() == width
        && mSceneViewport.GetHeight() == height)
    {
        return;
    }

    if (flushGpu)
        flushGpu();

    mSceneViewport.Resize(width, height);
}

void ImGuiManager::DrawHierarchyPanel(Engine* engine)
{
    ImGui::Begin("Hierarchy");

    if (engine)
    {
        const size_t objectCount = engine->GetRenderableObjectCount();
        ImGui::Text("Objects: %zu", objectCount);
        ImGui::Separator();
    }

    ImGui::TextDisabled("Entity List will be here");
    ImGui::End();
}

void ImGuiManager::DrawInspectorPanel()
{
    ImGui::Begin("Inspector");
    ImGui::Text("Selected Object Properties");
    ImGui::End();
}

void ImGuiManager::DrawProjectPanel()
{
    ImGui::Begin("Project");
    ImGui::Text("Assets / Meshes / Materials");
    ImGui::End();
}
#pragma endregion 

void ImGuiManager::Shutdown()
{
    mSceneViewport.Shutdown();

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    m_Device = nullptr;
    m_Hwnd = nullptr;
    m_DescriptorAllocator = nullptr;
    m_Callback = nullptr;
}

void ImGuiManager::NewFrame()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::Render(ID3D12GraphicsCommandList* cmdList)
{
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
}
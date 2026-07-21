#include "ImGuiManager.h"
#include "Managers/MeshManager.h"
#include "Managers/MaterialManager.h"
#include "Engine.h"
#include "Entity.h"
#include "ComponentStruct.h"
#include <algorithm>
#include <cstdio>
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

        // Scene 뷰 중앙 십자선 (스폰 위치 가이드)
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
            constexpr float arm = 12.0f;
            constexpr float gap = 4.0f;
            constexpr float thickness = 1.5f;
            const ImU32 outline = IM_COL32(0, 0, 0, 200);
            const ImU32 cross = IM_COL32(255, 255, 255, 230);

            auto drawArm = [&](ImVec2 a, ImVec2 b)
            {
                drawList->AddLine(a, b, outline, thickness + 1.5f);
                drawList->AddLine(a, b, cross, thickness);
            };

            // 가로
            drawArm(ImVec2(center.x - arm, center.y), ImVec2(center.x - gap, center.y));
            drawArm(ImVec2(center.x + gap, center.y), ImVec2(center.x + arm, center.y));
            // 세로
            drawArm(ImVec2(center.x, center.y - arm), ImVec2(center.x, center.y - gap));
            drawArm(ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + arm));
            // 중앙 점
            drawList->AddCircleFilled(center, 1.5f, cross);
        }

        // 스크린 절대좌표가 아니라 클라이언트 상대좌표로 저장한다.
        // 이유: 창을 옮기면 스크린 좌표는 바로 무효가 되고, 이동 중 Update가 멈춰도
        //       ClientToScreen만 다시 하면 clip/센터를 맞출 수 있음.
        const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
        mSceneClientMinX = min.x - vpPos.x;
        mSceneClientMinY = min.y - vpPos.y;
        mSceneClientMaxX = max.x - vpPos.x;
        mSceneClientMaxY = max.y - vpPos.y;
        mSceneClientRectValid = (mSceneClientMaxX > mSceneClientMinX && mSceneClientMaxY > mSceneClientMinY);

        // --- LMB: 짧은 클릭 = 마우스 룩 / 드래그 = 박스 다중 선택 ---
        ImGuiIO& io = ImGui::GetIO();
        const bool shift = io.KeyShift;

        if (mSceneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            mBoxDragging = true;
            mBoxStartScreen = io.MousePos;
            mBoxEndScreen = io.MousePos;
            mBoxSelectAdditive = shift;
        }

        if (mBoxDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            mBoxEndScreen = io.MousePos;
            // 드래그 중 선택 박스 표시
            const float dx = mBoxEndScreen.x - mBoxStartScreen.x;
            const float dy = mBoxEndScreen.y - mBoxStartScreen.y;
            if ((dx * dx + dy * dy) >= kBoxDragThresholdPx * kBoxDragThresholdPx)
            {
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                ImVec2 a = mBoxStartScreen;
                ImVec2 b = mBoxEndScreen;
                if (a.x > b.x) std::swap(a.x, b.x);
                if (a.y > b.y) std::swap(a.y, b.y);
                // Scene 이미지 안으로 클램프
                a.x = (std::max)(min.x, (std::min)(a.x, max.x));
                a.y = (std::max)(min.y, (std::min)(a.y, max.y));
                b.x = (std::max)(min.x, (std::min)(b.x, max.x));
                b.y = (std::max)(min.y, (std::min)(b.y, max.y));
                dl->AddRectFilled(a, b, IM_COL32(80, 160, 255, 40));
                dl->AddRect(a, b, IM_COL32(80, 160, 255, 220), 0.0f, 0, 1.5f);
            }
        }

        if (mBoxDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            mBoxEndScreen = io.MousePos;
            const float dx = mBoxEndScreen.x - mBoxStartScreen.x;
            const float dy = mBoxEndScreen.y - mBoxStartScreen.y;
            const float dist2 = dx * dx + dy * dy;
            mBoxDragging = false;

            if (dist2 >= kBoxDragThresholdPx * kBoxDragThresholdPx)
            {
                // Scene 이미지 로컬 픽셀로 변환
                float x0 = mBoxStartScreen.x - min.x;
                float y0 = mBoxStartScreen.y - min.y;
                float x1 = mBoxEndScreen.x - min.x;
                float y1 = mBoxEndScreen.y - min.y;
                if (x0 > x1) std::swap(x0, x1);
                if (y0 > y1) std::swap(y0, y1);
                const float sw = max.x - min.x;
                const float sh = max.y - min.y;
                x0 = (std::max)(0.0f, (std::min)(x0, sw));
                y0 = (std::max)(0.0f, (std::min)(y0, sh));
                x1 = (std::max)(0.0f, (std::min)(x1, sw));
                y1 = (std::max)(0.0f, (std::min)(y1, sh));

                mBoxResultMinX = x0;
                mBoxResultMinY = y0;
                mBoxResultMaxX = x1;
                mBoxResultMaxY = y1;
                mBoxSelectPending = true;
                // 드래그 선택이면 마우스 룩 진입 안 함
            }
            else if (mSceneHovered)
            {
                // 짧은 클릭 → 기존처럼 마우스 룩 요청
                mSceneCaptureClick = true;
            }
        }
    }
    else
    {
        ImGui::TextDisabled("Scene render target not ready (%u x %u)", width, height);
        mBoxDragging = false;
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

bool ImGuiManager::ConsumeBoxSelection(float& outMinX, float& outMinY, float& outMaxX, float& outMaxY, bool& outAdditive)
{
    if (!mBoxSelectPending)
        return false;
    mBoxSelectPending = false;
    outMinX = mBoxResultMinX;
    outMinY = mBoxResultMinY;
    outMaxX = mBoxResultMaxX;
    outMaxY = mBoxResultMaxY;
    outAdditive = mBoxSelectAdditive;
    return true;
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

    if (!engine)
    {
        ImGui::TextDisabled("Engine not available");
        ImGui::End();
        return;
    }

    const size_t objectCount = engine->GetRenderableObjectCount();
    ImGui::Text("Objects: %zu", objectCount);

    const size_t selCount = engine->GetSelectedCount();
    if (selCount == 0)
        ImGui::TextDisabled("Selected: (none)");
    else if (selCount == 1)
        ImGui::Text("Selected: %u", engine->GetSelectedEntity());
    else
        ImGui::Text("Selected: %zu entities", selCount);

    ImGui::TextDisabled("Scene: drag LMB box select | Shift+drag add");
    ImGui::TextDisabled("List: click | Ctrl+click toggle");

    if (ImGui::Button("Clear Selection"))
        engine->ClearSelection();

    ImGui::Separator();

    auto entities = engine->GetRenderableEntities();
    if (entities.empty())
    {
        ImGui::TextDisabled("No renderable entities");
        ImGui::End();
        return;
    }

    if (ImGui::BeginListBox("##EntityList", ImVec2(-1, -1)))
    {
        // 대량 오브젝트에서도 보이는 행만 생성 (ImGui CPU 병목 제거)
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entities.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Entity e = entities[static_cast<size_t>(i)];
                RenderableComponent* rend = engine->GetRenderable(e);
                const char* meshName = (rend && rend->mesh) ? rend->mesh->name.c_str() : "(no mesh)";

                char label[128];
                snprintf(label, sizeof(label), "Entity %u  [%s]", e, meshName);

                const bool isSelected = engine->IsEntitySelected(e);
                if (ImGui::Selectable(label, isSelected))
                {
                    if (ImGui::GetIO().KeyCtrl)
                        engine->ToggleSelectedEntity(e);
                    else
                        engine->SetSelectedEntity(e);
                }
            }
        }
        ImGui::EndListBox();
    }

    ImGui::End();
}

void ImGuiManager::DrawInspectorPanel(Engine* engine)
{
    ImGui::Begin("Inspector");

    if (!engine)
    {
        ImGui::TextDisabled("Engine not available");
        ImGui::End();
        return;
    }

    // --- 선택 오브젝트 조작 (3인칭 팔로우) ---
    if (ImGui::Checkbox("Manipulate Selected Object", &mManipulateSelected))
    {
        if (m_Callback)
            m_Callback->buttonClicked(ButtonAction::ToggleManipulateSelected);
    }
    if (mManipulateSelected)
    {
        ImGui::TextWrapped(
            "3rd-person follow: Mouse orbit | WASD move | Space/Shift up/down | Wheel zoom");
    }

    ImGui::Separator();

    const std::vector<Entity> selectedList = engine->GetSelectedEntities();
    if (selectedList.empty())
    {
        ImGui::TextDisabled("No entity selected");
        ImGui::TextDisabled("Scene drag-box, RMB pick, or Hierarchy.");
        ImGui::End();
        return;
    }

    // UI 표시용 primary = 목록 첫 엔티티 (조작 값은 여기서 읽고, 변경 시 전체에 적용)
    const Entity primary = selectedList.front();

    if (selectedList.size() == 1)
        ImGui::Text("Entity: %u", primary);
    else
        ImGui::Text("Multi-select: %zu (edits apply to all)", selectedList.size());

    auto forEachSelected = [&](auto&& fn)
    {
        for (Entity e : selectedList)
            fn(e);
    };

    // --- Transform (드래그 델타를 전체에 적용) ---
    if (TransformComponent* tf = engine->GetTransform(primary))
    {
        ImGui::SeparatorText("Transform");

        XMFLOAT3 pos = tf->position;
        if (ImGui::DragFloat3("Position", &pos.x, 0.05f))
        {
            const XMFLOAT3 delta{
                pos.x - tf->position.x,
                pos.y - tf->position.y,
                pos.z - tf->position.z
            };
            forEachSelected([&](Entity e)
            {
                if (TransformComponent* t = engine->GetTransform(e))
                {
                    t->position.x += delta.x;
                    t->position.y += delta.y;
                    t->position.z += delta.z;
                    t->MarkDirty();
                }
            });
        }

        XMFLOAT3 rot = tf->rotation;
        if (ImGui::DragFloat3("Rotation", &rot.x, 0.01f))
        {
            const XMFLOAT3 delta{
                rot.x - tf->rotation.x,
                rot.y - tf->rotation.y,
                rot.z - tf->rotation.z
            };
            forEachSelected([&](Entity e)
            {
                if (TransformComponent* t = engine->GetTransform(e))
                {
                    t->rotation.x += delta.x;
                    t->rotation.y += delta.y;
                    t->rotation.z += delta.z;
                    t->MarkDirty();
                }
            });
        }

        XMFLOAT3 scl = tf->scale;
        if (ImGui::DragFloat3("Scale", &scl.x, 0.01f, 0.001f, 100.0f))
        {
            // 스케일은 절대값으로 맞춤 (상대 곱보다 직관적)
            forEachSelected([&](Entity e)
            {
                if (TransformComponent* t = engine->GetTransform(e))
                {
                    t->scale = scl;
                    t->MarkDirty();
                }
            });
        }
    }

    // --- Gravity ---
    ImGui::SeparatorText("Gravity");
    {
        bool hasGravity = engine->HasGravityComponent(primary);
        if (ImGui::Checkbox("Gravity Component", &hasGravity))
        {
            forEachSelected([&](Entity e)
            {
                engine->SetEntityGravityEnabled(e, hasGravity);
            });
        }

        if (GravityComponent* gravity = engine->GetGravityComponent(primary))
        {
            ImGui::Indent();
            bool gEnabled = gravity->enabled;
            if (ImGui::Checkbox("Enabled##Gravity", &gEnabled))
            {
                forEachSelected([&](Entity e)
                {
                    if (GravityComponent* g = engine->GetGravityComponent(e))
                        g->enabled = gEnabled;
                });
            }
            float strength = gravity->strength;
            if (ImGui::DragFloat("Strength", &strength, 0.1f, 0.0f, 50.0f))
            {
                forEachSelected([&](Entity e)
                {
                    if (GravityComponent* g = engine->GetGravityComponent(e))
                        g->strength = strength;
                });
            }
            XMFLOAT3 vel = gravity->velocity;
            if (ImGui::DragFloat3("Velocity", &vel.x, 0.1f))
            {
                forEachSelected([&](Entity e)
                {
                    if (GravityComponent* g = engine->GetGravityComponent(e))
                        g->velocity = vel;
                });
            }
            ImGui::Unindent();
        }
    }

    // --- Collision ---
    ImGui::SeparatorText("Collision");
    {
        bool hasCollision = engine->HasCollisionComponent(primary);
        if (ImGui::Checkbox("Collision Component", &hasCollision))
        {
            forEachSelected([&](Entity e)
            {
                engine->SetEntityCollisionEnabled(e, hasCollision);
            });
        }

        if (CollisionComponent* collision = engine->GetCollisionComponent(primary))
        {
            ImGui::Indent();
            bool cEnabled = collision->enabled;
            if (ImGui::Checkbox("Enabled##Collision", &cEnabled))
            {
                forEachSelected([&](Entity e)
                {
                    if (CollisionComponent* c = engine->GetCollisionComponent(e))
                        c->enabled = cEnabled;
                });
            }
            bool isStatic = collision->isStatic;
            if (ImGui::Checkbox("Static", &isStatic))
            {
                forEachSelected([&](Entity e)
                {
                    if (CollisionComponent* c = engine->GetCollisionComponent(e))
                        c->isStatic = isStatic;
                });
            }
            float restitution = collision->restitution;
            if (ImGui::DragFloat("Restitution", &restitution, 0.01f, 0.0f, 1.0f))
            {
                forEachSelected([&](Entity e)
                {
                    if (CollisionComponent* c = engine->GetCollisionComponent(e))
                        c->restitution = restitution;
                });
            }
            ImGui::Unindent();
        }
    }

    // --- Material / Visible ---
    RenderableComponent* rend = engine->GetRenderable(primary);
    if (rend && rend->mesh)
    {
        ImGui::SeparatorText("Material (Sub > Main > Init)");
        ImGui::Text("Mesh: %s", rend->mesh->name.c_str());
        if (selectedList.size() > 1)
            ImGui::TextDisabled("Material changes apply to all selected");

        auto matNames = MaterialManager::Get().GetLoadedMaterialNames();
        std::sort(matNames.begin(), matNames.end());

        const std::string mainMaterialName = engine->GetEntityMainMaterial(primary);
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
            forEachSelected([&](Entity e)
            {
                engine->SetEntityMainMaterial(e, newMain);
            });
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

            const std::string currentSub = engine->GetEntitySubMaterial(primary, key);

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
                forEachSelected([&](Entity e)
                {
                    // 같은 서브메시 키가 있는 메시만 적용
                    if (RenderableComponent* r = engine->GetRenderable(e))
                    {
                        if (r->mesh && r->mesh->DrawArgs.count(key))
                            engine->SetEntitySubMaterial(e, key, newSub);
                    }
                });
            }
            ImGui::PopID();
        }

        bool visible = rend->visible;
        if (ImGui::Checkbox("Visible", &visible))
        {
            forEachSelected([&](Entity e)
            {
                if (RenderableComponent* r = engine->GetRenderable(e))
                    r->visible = visible;
            });
        }
    }
    else if (!engine->HasGravityComponent(primary) && !engine->HasCollisionComponent(primary))
    {
        ImGui::Separator();
        ImGui::TextDisabled("Primary entity has no RenderableComponent");
    }

    ImGui::End();
}

void ImGuiManager::DrawProjectPanel()
{
    ImGui::Begin("Project");
    ImGui::Text("Spawn Object");
    ImGui::Separator();

    auto meshNames = MeshManager::Get().GetLoadedMeshNames();
    std::sort(meshNames.begin(), meshNames.end());

    if (meshNames.empty())
    {
        ImGui::TextDisabled("No meshes loaded");
    }
    else
    {
        if (mSelectedMesh.empty() ||
            std::find(meshNames.begin(), meshNames.end(), mSelectedMesh) == meshNames.end())
        {
            mSelectedMesh = meshNames[0];
        }

        int meshIdx = 0;
        for (size_t i = 0; i < meshNames.size(); ++i)
        {
            if (meshNames[i] == mSelectedMesh)
            {
                meshIdx = (int)i;
                break;
            }
        }

        auto meshGetter = [](void* data, int idx) -> const char*
        {
            auto* vec = static_cast<std::vector<std::string>*>(data);
            if (idx < 0 || idx >= (int)vec->size()) return nullptr;
            return (*vec)[idx].c_str();
        };

        if (ImGui::Combo("Mesh", &meshIdx, meshGetter, &meshNames, (int)meshNames.size()))
            mSelectedMesh = meshNames[meshIdx];
    }

    auto matNames = MaterialManager::Get().GetLoadedMaterialNames();
    std::sort(matNames.begin(), matNames.end());
    if (!matNames.empty())
    {
        int matIdx = 0;
        if (!mSelectedMaterial.empty())
        {
            auto it = std::find(matNames.begin(), matNames.end(), mSelectedMaterial);
            if (it != matNames.end())
                matIdx = 1 + (int)std::distance(matNames.begin(), it);
        }

        if (ImGui::Combo("Main Material", &matIdx, MainMaterialComboGetter, &matNames, (int)matNames.size() + 1))
            mSelectedMaterial = (matIdx == 0) ? "" : matNames[matIdx - 1];
    }
    else
    {
        mSelectedMaterial.clear();
    }

    const bool canSpawn = !mSelectedMesh.empty() && m_Callback != nullptr;
    if (!canSpawn)
        ImGui::BeginDisabled();

    if (ImGui::Button("Spawn Selected Mesh", ImVec2(-1, 0)))
    {
        if (m_Callback && !mSelectedMesh.empty())
            m_Callback->buttonClicked(ButtonAction::SpawnSelectedMesh);
    }

    if (!canSpawn)
        ImGui::EndDisabled();

    if (!mSelectedMesh.empty())
    {
        const char* spawnMat = mSelectedMaterial.empty() ? "None (Init/Default)" : mSelectedMaterial.c_str();
        ImGui::Text("Ready: %s / %s", mSelectedMesh.c_str(), spawnMat);
        ImGui::TextDisabled("Spawns at the Scene crosshair (view center).");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Loaded meshes: %zu", meshNames.size());
    if (ImGui::BeginListBox("##MeshList", ImVec2(-1, 120.0f)))
    {
        for (const auto& name : meshNames)
        {
            const bool selected = (name == mSelectedMesh);
            if (ImGui::Selectable(name.c_str(), selected))
                mSelectedMesh = name;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndListBox();
    }

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
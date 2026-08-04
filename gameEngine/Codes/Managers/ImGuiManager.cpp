#include "ImGuiManager.h"
#include "Managers/MeshManager.h"
#include "Managers/MaterialManager.h"
#include "Engine.h"
#include "Entity.h"
#include "ComponentStruct.h"
#include "SceneSerializer.h"
#include "UiPresetSerializer.h"
#include "GameExporter.h"
#include "AppContext.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

namespace
{
    void LoadUIFonts(ImGuiIO& io)
    {
        io.Fonts->Clear();

        // 도크 패널이 좁을 때 잘림을 줄이기 위해 기본 16px (이전 18은 라벨/수치가 자주 잘림)
        const float fontSize = 16.0f;
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
    // DPI 인식 — 고해상도/배율에서 레이아웃이 어긋나 잘리는 현상 완화
    ImGui_ImplWin32_EnableDpiAwareness();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigWindowsResizeFromEdges = true;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // 실행 파일 옆 imgui.ini / editor_ui.cfg 에 UI 상태 저장
    ResolveConfigPaths();
    io.IniFilename = mImGuiIniPath;
    LoadUiSettings();

    LoadUIFonts(io);

    // 좁은 도크 패널에서도 글/위젯이 최대한 보이도록 기본 스타일 조정
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowMinSize = ImVec2(160.0f, 120.0f);
        style.WindowPadding = ImVec2(6.0f, 6.0f);
        style.FramePadding = ImVec2(5.0f, 3.0f);
        style.ItemSpacing = ImVec2(6.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 3.0f);
        // 스크롤바를 얇게 (기본 ~14~16 대신)
        style.ScrollbarSize = 9.0f;
        style.ScrollbarRounding = 3.0f;
        style.GrabMinSize = 8.0f;
        style.ChildRounding = 3.0f;
        style.FrameRounding = 3.0f;
        style.WindowMenuButtonPosition = ImGuiDir_None;
        // 기본 위젯 폭 = 창 내용 영역 전체 (라벨 옆 잘림 방지에 유리)
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    }

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

    // 창 위치 복원은 커맨드 리스트가 닫힌 뒤 InitDirect3DApp 쪽에서 ApplyMainWindowPlacement() 호출.
    // (여기서 SetWindowPlacement → WM_SIZE → OnResize → Reset 하면 열린 리스트와 충돌)

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

    // Texture aspect of a loaded material (Width/Height). Returns false if unavailable.
    bool TryGetMaterialTextureAspect(const std::string& materialName, float& outAspect)
    {
        const std::string name = materialName.empty() ? "Default" : materialName;
        auto mat = MaterialManager::Get().GetMaterial(name);
        if (!mat || !mat->mTexture)
            return false;
        const D3D12_RESOURCE_DESC desc = mat->mTexture->GetDesc();
        if (desc.Height == 0 || desc.Width == 0)
            return false;
        outAspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
        return outAspect > 1e-6f;
    }

    // Fit (x0,y0)-(x1,y1) inside itself to imageAspect, centered. Coordinates may be unsorted.
    void FitRectToAspect(float& x0, float& y0, float& x1, float& y1, float imageAspect)
    {
        if (imageAspect <= 1e-6f)
            return;
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        const float boxW = x1 - x0;
        const float boxH = y1 - y0;
        if (boxW < 1.f || boxH < 1.f)
            return;

        const float boxAspect = boxW / boxH;
        float outW = boxW;
        float outH = boxH;
        if (boxAspect > imageAspect)
        {
            // Box wider than image → height-limited
            outH = boxH;
            outW = outH * imageAspect;
        }
        else
        {
            outW = boxW;
            outH = outW / imageAspect;
        }
        const float cx = 0.5f * (x0 + x1);
        const float cy = 0.5f * (y0 + y1);
        x0 = cx - 0.5f * outW;
        x1 = cx + 0.5f * outW;
        y0 = cy - 0.5f * outH;
        y1 = cy + 0.5f * outH;
    }

    // --- 좁은 패널용 레이아웃 헬퍼 (글/위젯 잘림 방지) ---

    constexpr ImGuiWindowFlags kPanelWindowFlags =
        ImGuiWindowFlags_AlwaysVerticalScrollbar |
        ImGuiWindowFlags_HorizontalScrollbar;

    float ContentRightX()
    {
        // 현재 줄에서 사용 가능한 오른쪽 끝 (window-local)
        return ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x;
    }

    void WrapPosBegin()
    {
        // 명시적 우측 경계 — nested child 에서도 줄바꿈이 동작
        ImGui::PushTextWrapPos(ContentRightX());
    }

    void WrapPosEnd()
    {
        ImGui::PopTextWrapPos();
    }

    void TextLine(const char* fmt, ...)
    {
        WrapPosBegin();
        va_list args;
        va_start(args, fmt);
        ImGui::TextV(fmt, args);
        va_end(args);
        WrapPosEnd();
    }

    void TextLineDisabled(const char* fmt, ...)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        WrapPosBegin();
        va_list args;
        va_start(args, fmt);
        ImGui::TextV(fmt, args);
        va_end(args);
        WrapPosEnd();
        ImGui::PopStyleColor();
    }

    void BulletLine(const char* text)
    {
        ImGui::Bullet();
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        WrapPosBegin();
        ImGui::TextUnformatted(text);
        WrapPosEnd();
    }

    // 긴 체크박스 라벨도 줄바꿈 (기본 Checkbox는 라벨 줄바꿈 없음 → 잘림의 주원인)
    bool CheckboxWrapped(const char* label, bool* v)
    {
        // "Enabled##Gravity" → 표시는 "Enabled", ID는 전체 문자열
        const char* display = label;
        char displayBuf[128];
        if (const char* hash = strstr(label, "##"))
        {
            const size_t n = static_cast<size_t>(hash - label);
            if (n >= sizeof(displayBuf)) 
            {
                // 너무 길면 전체 표시 (희귀)
                display = label;
            }
            else
            {
                memcpy(displayBuf, label, n);
                displayBuf[n] = '\0';
                display = displayBuf;
            }
        }

        ImGui::PushID(label);
        const bool changed = ImGui::Checkbox("##cb", v);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        WrapPosBegin();
        ImGui::TextUnformatted(display);
        WrapPosEnd();
        bool toggled = changed;
        if (ImGui::IsItemClicked())
        {
            *v = !*v;
            toggled = true;
        }
        ImGui::PopID();
        return toggled;
    }

    // DragFloat3 가로 3분할은 좁은 패널에서 수치가 잘림 → 항상 X/Y/Z 세로 배치
    bool DragFloat3Full(const char* label, float v[3], float speed,
        float v_min = 0.0f, float v_max = 0.0f)
    {
        ImGui::TextUnformatted(label);
        ImGui::PushID(label);
        bool changed = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::DragFloat("X", &v[0], speed, v_min, v_max, "X: %.3f");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::DragFloat("Y", &v[1], speed, v_min, v_max, "Y: %.3f");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::DragFloat("Z", &v[2], speed, v_min, v_max, "Z: %.3f");
        ImGui::PopID();
        return changed;
    }

    bool DragFloatFull(const char* label, float* v, float speed,
        float v_min = 0.0f, float v_max = 0.0f)
    {
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        char id[96];
        snprintf(id, sizeof(id), "##%s", label);
        return ImGui::DragFloat(id, v, speed, v_min, v_max);
    }

    bool SliderFloatFull(const char* label, float* v, float v_min, float v_max)
    {
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        char id[96];
        snprintf(id, sizeof(id), "##%s", label);
        return ImGui::SliderFloat(id, v, v_min, v_max);
    }

    bool ComboFull(const char* label, int* current,
        const char* (*getter)(void*, int), void* data, int count)
    {
        ImGui::TextUnformatted(label);
        // 선택 항목 미리보기를 위에도 표시 (콤보 프레임 안에서 잘려도 위 텍스트로 확인)
        if (current && *current >= 0 && getter)
        {
            if (const char* preview = getter(data, *current))
                TextLineDisabled("Selected: %s", preview);
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        char id[96];
        snprintf(id, sizeof(id), "##%s", label);
        return ImGui::Combo(id, current, getter, data, count);
    }

    // 좁으면 전체 폭 버튼, 넓으면 가로로 흐르며 자동 줄바꿈
    bool ButtonAutoWrap(const char* label)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float need = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;

        // 패널이 좁거나 버튼 하나가 거의 한 줄이면 full-width
        if (avail < 260.0f || need > avail * 0.9f)
        {
            if (ImGui::GetCursorPosX() > ImGui::GetCursorStartPos().x + 1.0f)
                ImGui::NewLine();
            return ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f));
        }

        if (need > avail && ImGui::GetCursorPosX() > ImGui::GetCursorStartPos().x + 1.0f)
            ImGui::NewLine();
        const bool pressed = ImGui::Button(label);
        ImGui::SameLine();
        return pressed;
    }

    void EndButtonAutoWrapRow()
    {
        if (ImGui::GetCursorPosX() > ImGui::GetCursorStartPos().x + 1.0f)
            ImGui::NewLine();
    }

    // Selectable 텍스트가 길 때 가로 스크롤이 생기도록 행 폭을 텍스트에 맞춤
    bool SelectableFull(const char* label, bool selected)
    {
        const float textW = ImGui::CalcTextSize(label, nullptr, true).x
            + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float rowW = (std::max)(ImGui::GetContentRegionAvail().x, textW);
        return ImGui::Selectable(label, selected, 0, ImVec2(rowW, 0.0f));
    }
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
        ImGui::Text("Mouse: orbit | WASD: XZ | Space/Ctrl: Y | Shift: faster | Wheel: speed | RMB drag: zoom");
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

        if (ImGui::Combo("Mesh Material (spawn)", &matIdx, MainMaterialComboGetter, &matNames, (int)matNames.size() + 1)) {
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
        const char* spawnMat = mSelectedMaterial.empty() ? "None" : mSelectedMaterial.c_str();
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
                if (ImGui::Checkbox("Enabled##Gravity", &gravity->enabled))
                    engine->NotifyEntityMotionChanged(selected);
                if (ImGui::DragFloat("Strength", &gravity->strength, 0.1f, 0.0f, 50.0f))
                    engine->NotifyEntityMotionChanged(selected);
                if (ImGui::DragFloat3("Velocity", &gravity->velocity.x, 0.1f))
                    engine->NotifyEntityMotionChanged(selected);
                if (ImGui::DragFloat3("Angular Vel", &gravity->angularVelocity.x, 0.01f))
                    engine->NotifyEntityMotionChanged(selected);
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
// ==================== DockSpace + 메인 메뉴 ====================

std::string ImGuiManager::DefaultScenesDirForDialog()
{
    return SceneSerializer::DefaultScenesDirectory();
}

std::string ImGuiManager::JoinPathDialog(const std::string& dir, const std::string& name)
{
    if (dir.empty())
        return name;
    try
    {
        return (std::filesystem::path(dir) / name).string();
    }
    catch (...)
    {
        std::string d = dir;
        if (!d.empty() && d.back() != '\\' && d.back() != '/')
            d.push_back('\\');
        return d + name;
    }
}

std::string ImGuiManager::ParentPathDialog(const std::string& dir)
{
    try
    {
        std::filesystem::path p(dir);
        if (p.has_parent_path() && p.parent_path() != p)
            return p.parent_path().string();
    }
    catch (...) {}
    return dir;
}

void ImGuiManager::OpenPathDialog(PathDialogMode mode)
{
    mPathDialogMode = mode;
    mPathDialogSelected = -1;
    mPathDialogOpenPopup = true;

    if (mode == PathDialogMode::ExportFolder)
    {
        // Start at GameExport parent (exe dir) so user can pick where packages go
        try
        {
            mPathDialogDir = std::filesystem::path(GameExporter::SuggestExportDirectory("x")).parent_path().string();
            std::error_code ec;
            std::filesystem::create_directories(mPathDialogDir, ec);
        }
        catch (...)
        {
            mPathDialogDir = DefaultScenesDirForDialog();
        }

        if (!mLastScenePath.empty())
        {
            try
            {
                const auto stem = std::filesystem::path(mLastScenePath).stem().string();
                if (!stem.empty())
                    strncpy_s(mPathDialogExportTitle, stem.c_str(), _TRUNCATE);
            }
            catch (...) {}
        }
        mPathDialogFileName.clear();
    }
    else
    {
        if (!mLastScenePath.empty())
        {
            try
            {
                const auto p = std::filesystem::path(mLastScenePath);
                mPathDialogDir = p.has_parent_path() ? p.parent_path().string() : DefaultScenesDirForDialog();
                mPathDialogFileName = p.filename().string();
            }
            catch (...)
            {
                mPathDialogDir = DefaultScenesDirForDialog();
                mPathDialogFileName = "scene.scene";
            }
        }
        else
        {
            mPathDialogDir = DefaultScenesDirForDialog();
            mPathDialogFileName = "scene.scene";
        }
        if (mode == PathDialogMode::LoadScene)
            mPathDialogFileName.clear();
    }

    // Keep InputText buffer in sync when the dialog opens
    strncpy_s(mPathDialogFileNameBuf, mPathDialogFileName.c_str(), _TRUNCATE);

    RefreshPathDialogListing();
}

void ImGuiManager::RefreshPathDialogListing()
{
    mPathDialogDirs.clear();
    mPathDialogFiles.clear();
    mPathDialogSelected = -1;

    if (mPathDialogDir.empty())
        return;

    std::error_code ec;
    if (!std::filesystem::is_directory(mPathDialogDir, ec))
        return;

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(mPathDialogDir, ec))
        {
            if (ec)
                break;
            std::error_code e2;
            if (entry.is_directory(e2))
            {
                mPathDialogDirs.push_back(entry.path().filename().string());
            }
            else if (entry.is_regular_file(e2) && mPathDialogMode != PathDialogMode::ExportFolder)
            {
                const auto ext = entry.path().extension().string();
                if (_stricmp(ext.c_str(), ".scene") == 0)
                    mPathDialogFiles.push_back(entry.path().filename().string());
            }
        }
    }
    catch (...) {}

    std::sort(mPathDialogDirs.begin(), mPathDialogDirs.end());
    std::sort(mPathDialogFiles.begin(), mPathDialogFiles.end());
}

void ImGuiManager::DrawPathDialog(Engine* engine)
{
    if (mPathDialogMode == PathDialogMode::None)
        return;

    if (mPathDialogOpenPopup)
    {
        ImGui::OpenPopup("##PathDialog");
        mPathDialogOpenPopup = false;
    }

    const char* title = "Path";
    switch (mPathDialogMode)
    {
    case PathDialogMode::SaveScene: title = "Save Scene"; break;
    case PathDialogMode::LoadScene: title = "Load Scene"; break;
    case PathDialogMode::ExportFolder: title = "Export Game — Parent Folder"; break;
    default: break;
    }

    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##PathDialog", nullptr, ImGuiWindowFlags_NoResize))
    {
        // Escape / click-out closed the popup; clear mode once it is gone.
        if (!ImGui::IsPopupOpen("##PathDialog"))
            mPathDialogMode = PathDialogMode::None;
        return;
    }

    ImGui::TextUnformatted(title);
    ImGui::Separator();
    ImGui::TextWrapped("%s", mPathDialogDir.c_str());

    if (ImGui::Button("Up"))
    {
        mPathDialogDir = ParentPathDialog(mPathDialogDir);
        RefreshPathDialogListing();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        RefreshPathDialogListing();
    ImGui::SameLine();
    if (ImGui::Button("Scenes"))
    {
        mPathDialogDir = DefaultScenesDirForDialog();
        RefreshPathDialogListing();
    }

    ImGui::BeginChild("##PathList", ImVec2(0, 240), true);
    int row = 0;
    for (const auto& d : mPathDialogDirs)
    {
        const bool selected = (mPathDialogSelected == row);
        if (ImGui::Selectable(("[D] " + d).c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
        {
            mPathDialogSelected = row;
            if (ImGui::IsMouseDoubleClicked(0))
            {
                mPathDialogDir = JoinPathDialog(mPathDialogDir, d);
                RefreshPathDialogListing();
                ImGui::EndChild();
                ImGui::EndPopup();
                return;
            }
        }
        ++row;
    }
    for (const auto& f : mPathDialogFiles)
    {
        const bool selected = (mPathDialogSelected == row);
        if (ImGui::Selectable(f.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
        {
            mPathDialogSelected = row;
            mPathDialogFileName = f;
            strncpy_s(mPathDialogFileNameBuf, f.c_str(), _TRUNCATE);
            if (ImGui::IsMouseDoubleClicked(0) && mPathDialogMode == PathDialogMode::LoadScene)
            {
                // confirm load on double-click
                const std::string path = JoinPathDialog(mPathDialogDir, f);
                if (engine)
                {
                    std::string err;
                    if (engine->LoadSceneFromFile(path, &err))
                    {
                        mLastScenePath = path;
                        mSceneStatus = "Scene loaded: " + path
                            + " (" + std::to_string(engine->GetRenderableObjectCount()) + " objects)";
                        mSceneStatusIsError = false;
                    }
                    else
                    {
                        mSceneStatus = "Failed to load: " + (err.empty() ? path : err);
                        mSceneStatusIsError = true;
                    }
                }
                mPathDialogMode = PathDialogMode::None;
                ImGui::CloseCurrentPopup();
                ImGui::EndChild();
                ImGui::EndPopup();
                return;
            }
        }
        ++row;
    }
    ImGui::EndChild();

    if (mPathDialogMode == PathDialogMode::SaveScene)
    {
        // Member buffer: stack temps drop ActiveId / look like "click then cancel".
        if (ImGui::InputText("File name", mPathDialogFileNameBuf, sizeof(mPathDialogFileNameBuf)))
            mPathDialogFileName = mPathDialogFileNameBuf;
        else
            mPathDialogFileName = mPathDialogFileNameBuf;
    }
    else if (mPathDialogMode == PathDialogMode::LoadScene)
    {
        ImGui::Text("Selected: %s", mPathDialogFileName.empty() ? "(none)" : mPathDialogFileName.c_str());
    }
    else if (mPathDialogMode == PathDialogMode::ExportFolder)
    {
        ImGui::InputText("Game title", mPathDialogExportTitle, sizeof(mPathDialogExportTitle));
        ImGui::TextDisabled("Package will be written to: <folder>\\%s", mPathDialogExportTitle);
    }

    ImGui::Separator();
    const bool canOk =
        (mPathDialogMode == PathDialogMode::SaveScene && !mPathDialogFileName.empty()) ||
        (mPathDialogMode == PathDialogMode::LoadScene && !mPathDialogFileName.empty()) ||
        (mPathDialogMode == PathDialogMode::ExportFolder);

    if (!canOk)
        ImGui::BeginDisabled();
    if (ImGui::Button("OK", ImVec2(120, 0)))
    {
        if (engine)
        {
            if (mPathDialogMode == PathDialogMode::SaveScene)
            {
                std::string file = mPathDialogFileName;
                if (file.find('.') == std::string::npos)
                    file += ".scene";
                const std::string path = JoinPathDialog(mPathDialogDir, file);
                if (engine->SaveSceneToFile(path))
                {
                    mLastScenePath = path;
                    mSceneStatus = "Scene saved: " + path;
                    mSceneStatusIsError = false;
                }
                else
                {
                    mSceneStatus = "Failed to save scene: " + path;
                    mSceneStatusIsError = true;
                }
            }
            else if (mPathDialogMode == PathDialogMode::LoadScene)
            {
                const std::string path = JoinPathDialog(mPathDialogDir, mPathDialogFileName);
                std::string err;
                if (engine->LoadSceneFromFile(path, &err))
                {
                    mLastScenePath = path;
                    mSceneStatus = "Scene loaded: " + path
                        + " (" + std::to_string(engine->GetRenderableObjectCount()) + " objects)";
                    mSceneStatusIsError = false;
                }
                else
                {
                    mSceneStatus = "Failed to load: " + (err.empty() ? path : err);
                    mSceneStatusIsError = true;
                }
            }
            else if (mPathDialogMode == PathDialogMode::ExportFolder)
            {
                std::string exportDir = mPathDialogDir;
                try
                {
                    exportDir = (std::filesystem::path(mPathDialogDir) / mPathDialogExportTitle).string();
                }
                catch (...)
                {
                    exportDir = GameExporter::SuggestExportDirectory(mPathDialogExportTitle);
                }

                const GameExportResult r = GameExporter::Export(m_Hwnd, *engine, exportDir, mPathDialogExportTitle);
                mSceneStatus = r.message;
                mSceneStatusIsError = !r.ok;
                if (r.ok)
                    MessageBoxA(m_Hwnd, r.message.c_str(), "Export Game", MB_OK | MB_ICONINFORMATION);
                else
                    MessageBoxA(m_Hwnd, r.message.c_str(), "Export Game Failed", MB_OK | MB_ICONERROR);
            }
        }
        else
        {
            mSceneStatus = "Engine not available";
            mSceneStatusIsError = true;
        }
        mPathDialogMode = PathDialogMode::None;
        ImGui::CloseCurrentPopup();
    }
    if (!canOk)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
    {
        mPathDialogMode = PathDialogMode::None;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void ImGuiManager::DrawMainMenuBar(Engine* engine)
{
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Save Scene...", "Ctrl+S"))
        {
            if (engine)
                OpenPathDialog(PathDialogMode::SaveScene);
            else
            {
                mSceneStatus = "Engine not available";
                mSceneStatusIsError = true;
            }
        }
        if (ImGui::MenuItem("Load Scene...", "Ctrl+O"))
        {
            if (engine)
                OpenPathDialog(PathDialogMode::LoadScene);
            else
            {
                mSceneStatus = "Engine not available";
                mSceneStatusIsError = true;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export Game..."))
        {
            if (engine)
                OpenPathDialog(PathDialogMode::ExportFolder);
            else
            {
                mSceneStatus = "Engine not available";
                mSceneStatusIsError = true;
            }
        }
        ImGui::Separator();
        if (!mLastScenePath.empty())
            ImGui::TextDisabled("%s", mLastScenePath.c_str());
        else
            ImGui::TextDisabled("No scene file yet");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Play"))
    {
        const bool hasHost = (mEditorHost != nullptr);
        const bool inEditorPlay = hasHost && mEditorHost->IsInEditorPlaying();
        const bool standaloneRun = hasHost && mEditorHost->IsStandaloneRunning();

        if (!hasHost)
            ImGui::TextDisabled("Play session unavailable");

        // B: same process
        if (ImGui::MenuItem("Play In Editor", "F5", false, hasHost && !inEditorPlay && !standaloneRun))
        {
            if (mEditorHost && mEditorHost->PlayInEditor())
            {
                mSceneStatus = "Play In Editor starting… ESC to stop (restores snapshot)";
                mSceneStatusIsError = false;
            }
            else if (mEditorHost)
            {
                mSceneStatus = "Play In Editor failed";
                mSceneStatusIsError = true;
            }
        }
        if (ImGui::MenuItem("Stop In Editor", "Esc", false, hasHost && inEditorPlay))
        {
            if (mEditorHost)
            {
                mEditorHost->StopInEditorPlay();
                mSceneStatus = "Stopping in-editor play...";
                mSceneStatusIsError = false;
            }
        }

        ImGui::Separator();

        // A: Game.exe child
        if (ImGui::MenuItem("Play Standalone (Game.exe)", "Ctrl+F5", false, hasHost && !inEditorPlay))
        {
            if (mEditorHost && mEditorHost->PlayStandalone())
            {
                mSceneStatus = "Standalone Game.exe launched (scene snapshot saved)";
                mSceneStatusIsError = false;
            }
            else if (mEditorHost)
            {
                mSceneStatus = "Play Standalone failed (is Game.exe built?)";
                mSceneStatusIsError = true;
            }
        }
        if (ImGui::MenuItem("Stop Standalone", nullptr, false, hasHost && standaloneRun))
        {
            if (mEditorHost)
            {
                mEditorHost->StopStandalone();
                mSceneStatus = "Standalone Game.exe stopped";
                mSceneStatusIsError = false;
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("In Editor = B (same process)");
        ImGui::TextDisabled("Standalone = A (real Game.exe)");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        if (ImGui::MenuItem("Scene", nullptr, &mShowScene)) MarkUiSettingsDirty();
        if (ImGui::MenuItem("Hierarchy", nullptr, &mShowHierarchy)) MarkUiSettingsDirty();
        if (ImGui::MenuItem("Tools", nullptr, &mShowTools)) MarkUiSettingsDirty();
        if (ImGui::MenuItem("Inspector", nullptr, &mShowInspector)) MarkUiSettingsDirty();
        ImGui::Separator();
        if (ImGui::MenuItem("Project", nullptr, &mShowProject)) MarkUiSettingsDirty();
        if (ImGui::MenuItem("UI", nullptr, &mShowUi)) MarkUiSettingsDirty();
        if (ImGui::MenuItem("Render", nullptr, &mShowRender)) MarkUiSettingsDirty();
        if (ImGui::MenuItem("Help", nullptr, &mShowHelp)) MarkUiSettingsDirty();
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
        {
            mRequestResetLayout = true;
            MarkUiSettingsDirty();
        }
        if (ImGui::MenuItem("Save UI Settings"))
        {
            // 스플리터 비율을 최신으로 잡은 뒤 즉시 저장
            if (ImGuiDockNode* root = ImGui::DockBuilderGetNode(ImGui::GetID("EditorDockSpace")))
                CaptureDockSplitRatios(root);
            SaveUiSettings();
            ImGui::SaveIniSettingsToDisk(mImGuiIniPath);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window"))
    {
        ImGui::TextDisabled("Drag title bars onto panels");
        ImGui::TextDisabled("to dock as tabs (click tab = that window).");
        ImGui::Separator();
        ImGui::TextDisabled("Bottom strip holds Project / UI / Render / Help.");
        ImGui::TextDisabled("Left strip: Hierarchy + Tools.");
        ImGui::Separator();
        ImGui::TextDisabled("Settings: editor_ui.cfg + imgui.ini");
        ImGui::TextDisabled("(next to the .exe, auto-saved)");
        ImGui::EndMenu();
    }

    // 우측 상태 (씬 IO / 힌트)
    {
        if (!mSceneStatus.empty())
        {
            const ImVec4 col = mSceneStatusIsError
                ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
                : ImVec4(0.55f, 0.9f, 0.55f, 1.0f);
            ImGui::SameLine();
            ImGui::TextColored(col, " | %s", mSceneStatus.c_str());
        }
        else
        {
            const float hintWidth = 220.0f;
            if (ImGui::GetWindowWidth() > hintWidth + 200.0f)
            {
                ImGui::SameLine(ImGui::GetWindowWidth() - hintWidth);
                ImGui::TextDisabled("File: Save/Load Scene");
            }
        }
    }

    ImGui::EndMainMenuBar();
}

void ImGuiManager::ApplyDefaultDockLayout(ImGuiID dockspace_id, const ImVec2& workSize)
{
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, workSize);

    // 1) 하단 전체 폭: 여러 메뉴가 탭으로 들어갈 공간
    ImGuiID dock_id_down = 0;
    ImGuiID dock_id_main = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.32f, &dock_id_down, &dock_id_main);

    // 2) 상단: 좌 Hierarchy 탭 영역 / 중앙 Scene / 우 Inspector
    ImGuiID dock_id_left = 0;
    ImGuiID dock_id_center = 0;
    ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Left, 0.20f, &dock_id_left, &dock_id_center);

    ImGuiID dock_id_right = 0;
    ImGui::DockBuilderSplitNode(dock_id_center, ImGuiDir_Right, 0.24f, &dock_id_right, &dock_id_center);

    // 같은 노드에 여러 창을 도킹하면 ImGui 탭 바가 생기고, 탭 클릭 시 그 창만 보인다.
    ImGui::DockBuilderDockWindow("Scene", dock_id_center);

    ImGui::DockBuilderDockWindow("Hierarchy", dock_id_left);
    ImGui::DockBuilderDockWindow("Tools", dock_id_left);

    ImGui::DockBuilderDockWindow("Inspector", dock_id_right);

    ImGui::DockBuilderDockWindow("Project", dock_id_down);
    ImGui::DockBuilderDockWindow("UI", dock_id_down);
    ImGui::DockBuilderDockWindow("Render", dock_id_down);
    ImGui::DockBuilderDockWindow("Help", dock_id_down);

    ImGui::DockBuilderFinish(dockspace_id);

    // 다음 프레임에 트리 Size를 읽어 비율 시드
    mDockSplitRatios.clear();
    mDockRatiosNeedSeed = true;
}

void ImGuiManager::CaptureDockSplitRatios(ImGuiDockNode* node)
{
    if (!node || !node->IsSplitNode())
        return;

    ImGuiDockNode* c0 = node->ChildNodes[0];
    ImGuiDockNode* c1 = node->ChildNodes[1];
    if (!c0 || !c1)
        return;

    const int axis = static_cast<int>(node->SplitAxis);
    // Size = 실제 표시 크기 (유저가 스플리터를 움직인 결과)
    float s0 = c0->Size[axis];
    float s1 = c1->Size[axis];
    if (s0 + s1 < 1.0f)
    {
        s0 = c0->SizeRef[axis];
        s1 = c1->SizeRef[axis];
    }

    const float sum = s0 + s1;
    if (sum > 1.0f)
    {
        float ratio = s0 / sum;
        if (ratio < 0.02f) ratio = 0.02f;
        if (ratio > 0.98f) ratio = 0.98f;
        const auto it = mDockSplitRatios.find(node->ID);
        if (it == mDockSplitRatios.end() || ImFabs(it->second - ratio) > 0.0005f)
        {
            mDockSplitRatios[node->ID] = ratio;
            MarkUiSettingsDirty();
        }
        else
        {
            mDockSplitRatios[node->ID] = ratio;
        }
    }

    CaptureDockSplitRatios(c0);
    CaptureDockSplitRatios(c1);
}

void ImGuiManager::ApplyDockSplitRatios(ImGuiDockNode* node, ImVec2 size)
{
    if (!node)
        return;

    // 부모 영역의 현재 픽셀 크기 (축별 % 환산의 기준)
    // - 좌우 스플릿: size.x (너비) 기준
    // - 상하 스플릿: size.y (높이) 기준
    // 창 종횡비(1:1 ↔ 16:9)가 바뀌면 size.x/size.y 비율이 달라지므로
    // 가로·세로 패널이 각각 올바른 %로 재계산된다.
    node->Size = size;
    node->SizeRef = size;

    if (!node->IsSplitNode())
        return;

    ImGuiDockNode* c0 = node->ChildNodes[0];
    ImGuiDockNode* c1 = node->ChildNodes[1];
    if (!c0 || !c1)
        return;

    const int axis = static_cast<int>(node->SplitAxis);
    float ratio = 0.5f;
    const auto it = mDockSplitRatios.find(node->ID);
    if (it != mDockSplitRatios.end())
    {
        ratio = it->second;
    }
    else
    {
        const float r0 = c0->SizeRef[axis];
        const float r1 = c1->SizeRef[axis];
        const float sum = r0 + r1;
        if (sum > 1.0f)
            ratio = r0 / sum;
    }

    if (ratio < 0.02f) ratio = 0.02f;
    if (ratio > 0.98f) ratio = 0.98f;

    // 축 방향만 비율로 나누고, 수직 축은 부모 크기를 그대로 상속
    // 예) 16:9로 넓어지면 좌우 패널 너비 = ratio * 새 width
    //     상하 패널 높이 = ratio * 새 height
    const float spacing = ImGui::GetStyle().DockingSeparatorSize;
    const float avail = (size[axis] > spacing + 1.0f) ? (size[axis] - spacing) : 1.0f;
    const float s0 = IM_TRUNC(avail * ratio + 0.5f);
    const float s1 = avail - s0;

    ImVec2 size0 = size;
    ImVec2 size1 = size;
    size0[axis] = s0;
    size1[axis] = s1;

    c0->SizeRef = size0;
    c1->SizeRef = size1;
    c0->Size = size0;
    c1->Size = size1;

    // 자식 트리도 새 parent 픽셀 크기를 기준으로 재귀 환산
    ApplyDockSplitRatios(c0, size0);
    ApplyDockSplitRatios(c1, size1);
}

void ImGuiManager::SyncDockLayoutToWorkSize(ImGuiID dockspace_id, const ImVec2& workSize)
{
    if (workSize.x <= 1.0f || workSize.y <= 1.0f)
        return;

    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
    if (!root)
        return;

    const float aspect = workSize.x / workSize.y;
    const bool sizeChanged =
        (workSize.x != mLastDockWorkSize.x) ||
        (workSize.y != mLastDockWorkSize.y);
    const bool aspectChanged =
        (mLastDockAspect <= 0.0f) ||
        (ImFabs(aspect - mLastDockAspect) > 0.0001f);

    // 스플리터 드래그 중에는 ImGui가 픽셀을 직접 바꾸므로 비율만 읽고, 강제 Apply는 하지 않는다.
    // (창 리사이즈 중이 아닐 때만 — 창 리사이즈와 스플리터를 구분)
    const bool splitterDrag =
        !sizeChanged &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f);

    if (mDockRatiosNeedSeed || mDockSplitRatios.empty())
    {
        CaptureDockSplitRatios(root);
        if (!mDockSplitRatios.empty())
            mDockRatiosNeedSeed = false;
    }

    if (splitterDrag)
    {
        // 유저가 패널 경계를 조절 중 → 현재 픽셀을 %로 갱신
        CaptureDockSplitRatios(root);
    }
    else
    {
        // 유휴 / 창 크기·종횡비 변경:
        // 저장된 % 를 현재 workSize(너비·높이 각각)에 다시 적용.
        // 1:1 → 16:9 처럼 가로만 크게 늘어나도
        //  좌우 스플릿은 새 width% , 상하 스플릿은 새 height% 로 계산된다.
        ImGui::DockBuilderSetNodeSize(dockspace_id, workSize);
        ApplyDockSplitRatios(root, workSize);
    }

    if (sizeChanged || aspectChanged)
    {
        mLastDockWorkSize = workSize;
        mLastDockAspect = aspect;
    }
}

void ImGuiManager::ResolveConfigPaths()
{
    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, modulePath, MAX_PATH) == 0)
    {
        // 실패 시 현재 작업 디렉터리 사용
        strncpy_s(mImGuiIniPath, "imgui.ini", _TRUNCATE);
        strncpy_s(mEditorCfgPath, "editor_ui.cfg", _TRUNCATE);
        return;
    }

    char* slash = strrchr(modulePath, '\\');
    if (!slash)
        slash = strrchr(modulePath, '/');
    if (slash)
        *(slash + 1) = '\0';
    else
        modulePath[0] = '\0';

    snprintf(mImGuiIniPath, sizeof(mImGuiIniPath), "%simgui.ini", modulePath);
    snprintf(mEditorCfgPath, sizeof(mEditorCfgPath), "%seditor_ui.cfg", modulePath);
}

void ImGuiManager::MarkUiSettingsDirty()
{
    mUiSettingsDirty = true;
}

bool ImGuiManager::LoadUiSettings()
{
    std::ifstream in(mEditorCfgPath);
    if (!in)
    {
        mRestoreDockFromSettings = false;
        mAppliedDockLayoutVersion = 0; // 첫 실행 → 기본 레이아웃
        return false;
    }

    mDockSplitRatios.clear();
    int fileVersion = 0;
    int dockLayoutVersion = 0;
    bool hasCustomDock = false;

    std::string line;
    while (std::getline(in, line))
    {
        // trim CR
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);

        auto asBool = [&](bool& out)
        {
            out = (val == "1" || val == "true" || val == "True");
        };

        if (key == "version")
            fileVersion = std::atoi(val.c_str());
        else if (key == "show_scene") asBool(mShowScene);
        else if (key == "show_hierarchy") asBool(mShowHierarchy);
        else if (key == "show_inspector") asBool(mShowInspector);
        else if (key == "show_tools") asBool(mShowTools);
        else if (key == "show_project") asBool(mShowProject);
        else if (key == "show_ui") asBool(mShowUi);
        else if (key == "show_render") asBool(mShowRender);
        else if (key == "show_help") asBool(mShowHelp);
        else if (key == "manipulate") asBool(mManipulateSelected);
        else if (key == "manipulate_ui") asBool(mManipulateUi);
        else if (key == "snap_mesh") asBool(mSnapMesh);
        else if (key == "snap_ui") asBool(mSnapUi);
        else if (key == "snap_mesh_threshold")
            mSnapMeshThreshold = static_cast<float>(std::atof(val.c_str()));
        else if (key == "snap_ui_threshold_percent")
            mSnapUiThresholdPercent = static_cast<float>(std::atof(val.c_str()));
        else if (key == "snap_ui_threshold_px")
        {
            // Legacy: ~px at 1080p height → rough percent
            const float px = static_cast<float>(std::atof(val.c_str()));
            mSnapUiThresholdPercent = (std::max)(0.1f, (std::min)(20.f, px / 10.8f));
        }
        else if (key == "selected_mesh") mSelectedMesh = val;
        else if (key == "selected_material") mSelectedMaterial = val;
        else if (key == "selected_ui_material") mSelectedUiMaterial = val;
        else if (key == "ui_create_fit_image_aspect") asBool(mUiCreateFitImageAspect);
        else if (key == "dock_layout_version")
            dockLayoutVersion = std::atoi(val.c_str());
        else if (key == "has_custom_dock")
            asBool(hasCustomDock);
        else if (key == "window_left")
            mWindowNormalLeft = std::atoi(val.c_str());
        else if (key == "window_top")
            mWindowNormalTop = std::atoi(val.c_str());
        else if (key == "window_right")
            mWindowNormalRight = std::atoi(val.c_str());
        else if (key == "window_bottom")
            mWindowNormalBottom = std::atoi(val.c_str());
        else if (key == "window_show_cmd")
            mWindowShowCmd = std::atoi(val.c_str());
        else if (key == "window_valid")
            asBool(mHasSavedWindowPlacement);
        else if (key == "split")
        {
            // split=<hexId>:<ratio>
            unsigned int id = 0;
            float ratio = 0.5f;
            if (sscanf_s(val.c_str(), "%x:%f", &id, &ratio) == 2)
            {
                if (ratio < 0.02f) ratio = 0.02f;
                if (ratio > 0.98f) ratio = 0.98f;
                mDockSplitRatios[static_cast<ImGuiID>(id)] = ratio;
            }
        }
    }

    // 잘못된 사각형이면 무시
    if (mHasSavedWindowPlacement)
    {
        RECT rc{
            mWindowNormalLeft, mWindowNormalTop,
            mWindowNormalRight, mWindowNormalBottom
        };
        if (rc.right - rc.left < 200 || rc.bottom - rc.top < 150 ||
            !IsPlacementOnScreen(rc))
        {
            mHasSavedWindowPlacement = false;
        }
        if (mWindowShowCmd != SW_SHOWNORMAL &&
            mWindowShowCmd != SW_SHOWMAXIMIZED &&
            mWindowShowCmd != SW_SHOWMINIMIZED)
        {
            mWindowShowCmd = SW_SHOWNORMAL;
        }
        // 시작 시 minimized 로 복원하지 않음
        if (mWindowShowCmd == SW_SHOWMINIMIZED)
            mWindowShowCmd = SW_SHOWNORMAL;
    }

    (void)fileVersion;

    const bool imguiIniExists = (GetFileAttributesA(mImGuiIniPath) != INVALID_FILE_ATTRIBUTES);
    // 저장된 도크 + imgui.ini + 레이아웃 버전이 맞을 때만 복원 (버전 올리면 기본 재배치)
    mRestoreDockFromSettings =
        hasCustomDock &&
        imguiIniExists &&
        dockLayoutVersion == kDockLayoutVersion;

    if (mRestoreDockFromSettings)
    {
        mAppliedDockLayoutVersion = kDockLayoutVersion;
        mDockRatiosNeedSeed = mDockSplitRatios.empty();
    }
    else
    {
        // 기본 레이아웃 강제 1회
        mAppliedDockLayoutVersion = 0;
        if (dockLayoutVersion != kDockLayoutVersion)
            mDockSplitRatios.clear();
        mDockRatiosNeedSeed = true;
    }

    mUiSettingsDirty = false;
    return true;
}

bool ImGuiManager::SaveUiSettings()
{
    std::ofstream out(mEditorCfgPath, std::ios::trunc);
    if (!out)
        return false;

    out << "# GameEngine editor UI settings — auto-saved\n";
    out << "version=" << kUiSettingsFileVersion << "\n";
    out << "show_scene=" << (mShowScene ? 1 : 0) << "\n";
    out << "show_hierarchy=" << (mShowHierarchy ? 1 : 0) << "\n";
    out << "show_inspector=" << (mShowInspector ? 1 : 0) << "\n";
    out << "show_tools=" << (mShowTools ? 1 : 0) << "\n";
    out << "show_project=" << (mShowProject ? 1 : 0) << "\n";
    out << "show_ui=" << (mShowUi ? 1 : 0) << "\n";
    out << "show_render=" << (mShowRender ? 1 : 0) << "\n";
    out << "show_help=" << (mShowHelp ? 1 : 0) << "\n";
    out << "manipulate=" << (mManipulateSelected ? 1 : 0) << "\n";
    out << "manipulate_ui=" << (mManipulateUi ? 1 : 0) << "\n";
    out << "snap_mesh=" << (mSnapMesh ? 1 : 0) << "\n";
    out << "snap_ui=" << (mSnapUi ? 1 : 0) << "\n";
    out << "snap_mesh_threshold=" << mSnapMeshThreshold << "\n";
    out << "snap_ui_threshold_percent=" << mSnapUiThresholdPercent << "\n";
    out << "selected_mesh=" << mSelectedMesh << "\n";
    out << "selected_material=" << mSelectedMaterial << "\n";
    out << "selected_ui_material=" << mSelectedUiMaterial << "\n";
    out << "ui_create_fit_image_aspect=" << (mUiCreateFitImageAspect ? 1 : 0) << "\n";
    out << "dock_layout_version=" << kDockLayoutVersion << "\n";
    out << "has_custom_dock=1\n";

    // 저장 직전에 최신 창 배치 반영
    CaptureMainWindowPlacement();
    out << "window_valid=" << (mHasSavedWindowPlacement ? 1 : 0) << "\n";
    out << "window_left=" << mWindowNormalLeft << "\n";
    out << "window_top=" << mWindowNormalTop << "\n";
    out << "window_right=" << mWindowNormalRight << "\n";
    out << "window_bottom=" << mWindowNormalBottom << "\n";
    out << "window_show_cmd=" << mWindowShowCmd << "\n";

    for (const auto& pair : mDockSplitRatios)
    {
        char line[128];
        snprintf(line, sizeof(line), "split=%08x:%.6f\n",
            static_cast<unsigned>(pair.first), pair.second);
        out << line;
    }

    mUiSettingsDirty = false;
    mLastUiSettingsSaveTime = ImGui::GetTime();
    return static_cast<bool>(out);
}

bool ImGuiManager::IsPlacementOnScreen(const RECT& rc)
{
    // 복원 사각형이 어떤 모니터와도 겹치지 않으면 잘못된 좌표로 간주
    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
    return mon != nullptr;
}

void ImGuiManager::CaptureMainWindowPlacement()
{
    if (!m_Hwnd || !IsWindow(m_Hwnd))
        return;

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(m_Hwnd, &wp))
        return;

    // rcNormalPosition = 복원 시 위치/크기 (최대화 중이어도 정상 크기 보관)
    const RECT& rc = wp.rcNormalPosition;
    int showCmd = static_cast<int>(wp.showCmd);
    if (showCmd == SW_SHOWMINIMIZED)
    {
        // 최소화 직전 상태로 저장 (flags 에 이전 maximize 정보가 있을 수 있음)
        if (wp.flags & WPF_RESTORETOMAXIMIZED)
            showCmd = SW_SHOWMAXIMIZED;
        else
            showCmd = SW_SHOWNORMAL;
    }

    const bool changed =
        !mHasSavedWindowPlacement ||
        mWindowNormalLeft != rc.left ||
        mWindowNormalTop != rc.top ||
        mWindowNormalRight != rc.right ||
        mWindowNormalBottom != rc.bottom ||
        mWindowShowCmd != showCmd;

    mWindowNormalLeft = rc.left;
    mWindowNormalTop = rc.top;
    mWindowNormalRight = rc.right;
    mWindowNormalBottom = rc.bottom;
    mWindowShowCmd = showCmd;
    mHasSavedWindowPlacement = true;

    if (changed)
        MarkUiSettingsDirty();
}

void ImGuiManager::ApplyMainWindowPlacement()
{
    if (!m_Hwnd || !IsWindow(m_Hwnd) || !mHasSavedWindowPlacement)
        return;

    RECT rc{
        mWindowNormalLeft, mWindowNormalTop,
        mWindowNormalRight, mWindowNormalBottom
    };
    if (rc.right - rc.left < 200 || rc.bottom - rc.top < 150)
        return;
    if (!IsPlacementOnScreen(rc))
        return;

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    GetWindowPlacement(m_Hwnd, &wp);
    wp.flags = 0;
    // 숨김 생성 직후에는 아직 ShowMainWindow 전이므로, Placement 로 바로 띄우지 않고
    // 좌표만 맞춘 뒤 SW_HIDE 를 유지한다. (로딩 중 창 깜빡임 방지)
    // 최대화는 ShowMainWindow 직전에 showCmd 로 다시 적용한다.
    wp.showCmd = SW_HIDE;
    wp.rcNormalPosition = rc;
    SetWindowPlacement(m_Hwnd, &wp);

}

void ImGuiManager::PresentMainWindow()
{
    if (!m_Hwnd || !IsWindow(m_Hwnd))
        return;

    const int cmd = (mHasSavedWindowPlacement && mWindowShowCmd == SW_SHOWMAXIMIZED)
        ? SW_SHOWMAXIMIZED
        : SW_SHOWNORMAL;

    ShowWindow(m_Hwnd, cmd);
    UpdateWindow(m_Hwnd);
    SetForegroundWindow(m_Hwnd);
}

void ImGuiManager::AutosaveUiSettingsIfNeeded()
{
    // 창 이동/리사이즈도 주기적으로 반영
    CaptureMainWindowPlacement();

    if (!mUiSettingsDirty)
        return;

    const double now = ImGui::GetTime();
    // 드래그 중 과도한 디스크 쓰기 방지
    if (now - mLastUiSettingsSaveTime < 2.0)
        return;

    SaveUiSettings();
    // 도크 구조(창 위치/탭 배치)도 함께 기록
    if (mImGuiIniPath[0] != '\0')
        ImGui::SaveIniSettingsToDisk(mImGuiIniPath);
}

void ImGuiManager::SetupDockspace(Engine* engine)
{
    if (mPlayMode)
        return;

    DrawMainMenuBar(engine);
    DrawPathDialog(engine);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 workPos = viewport->WorkPos;
    const ImVec2 workSize = viewport->WorkSize;

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    // 매 프레임 호스트를 클라이언트 work 영역에 고정
    ImGui::SetNextWindowPos(workPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(workSize, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##MainDockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    // 첫 실행 / Reset Layout / 레이아웃 버전 변경 시에만 기본 배치 재구성.
    // 그 외에는 imgui.ini + editor_ui.cfg 스플릿 비율을 유지한다.
    const bool needLayout =
        mRequestResetLayout ||
        mAppliedDockLayoutVersion != kDockLayoutVersion;

    if (needLayout)
    {
        ApplyDefaultDockLayout(dockspace_id, workSize);
        mAppliedDockLayoutVersion = kDockLayoutVersion;
        mRequestResetLayout = false;
        mRestoreDockFromSettings = true;
        mLastDockWorkSize = workSize;
        mLastDockAspect = (workSize.y > 0.0f) ? (workSize.x / workSize.y) : 1.0f;

        // 기본 배치(또는 Reset Layout) 시 패널을 모두 연다.
        mShowScene = true;
        mShowHierarchy = true;
        mShowInspector = true;
        mShowTools = true;
        mShowProject = true;
        mShowUi = true;
        mShowRender = true;
        mShowHelp = true;

        MarkUiSettingsDirty();
    }
    else
    {
        SyncDockLayoutToWorkSize(dockspace_id, workSize);
    }

    ImGui::End();
}

void ImGuiManager::DrawEditorPanels(Engine* engine)
{
    if (mPlayMode)
        return;

    // Apply deferred UI-create enable + locks before any panel reads the mode flag.
    TickUiImageCreateMode();

    const bool prevScene = mShowScene;
    const bool prevHierarchy = mShowHierarchy;
    const bool prevInspector = mShowInspector;
    const bool prevTools = mShowTools;
    const bool prevProject = mShowProject;
    const bool prevUi = mShowUi;
    const bool prevRender = mShowRender;
    const bool prevHelp = mShowHelp;
    const bool prevManip = mManipulateSelected;
    const bool prevManipUi = mManipulateUi;
    const bool prevSnapMesh = mSnapMesh;
    const bool prevSnapUi = mSnapUi;
    const float prevSnapMeshT = mSnapMeshThreshold;
    const float prevSnapUiT = mSnapUiThresholdPercent;
    const std::string prevMesh = mSelectedMesh;
    const std::string prevMat = mSelectedMaterial;
    const std::string prevUiMat = mSelectedUiMaterial;
    const bool prevUiFit = mUiCreateFitImageAspect;

    if (mShowScene)
        DrawScenePanel(engine);
    if (mShowHierarchy)
        DrawHierarchyPanel(engine);
    if (mShowTools)
        DrawToolsPanel();
    if (mShowInspector)
        DrawInspectorPanel(engine);
    if (mShowProject)
        DrawProjectPanel(engine);
    if (mShowUi)
        DrawUiPanel(engine);
    if (mShowRender)
        DrawRenderPanel(engine);
    if (mShowHelp)
        DrawHelpPanel();

    if (prevScene != mShowScene || prevHierarchy != mShowHierarchy ||
        prevInspector != mShowInspector || prevTools != mShowTools ||
        prevProject != mShowProject || prevUi != mShowUi ||
        prevRender != mShowRender || prevHelp != mShowHelp ||
        prevManip != mManipulateSelected ||
        prevManipUi != mManipulateUi ||
        prevSnapMesh != mSnapMesh || prevSnapUi != mSnapUi ||
        prevSnapMeshT != mSnapMeshThreshold || prevSnapUiT != mSnapUiThresholdPercent ||
        prevMesh != mSelectedMesh || prevMat != mSelectedMaterial ||
        prevUiMat != mSelectedUiMaterial || prevUiFit != mUiCreateFitImageAspect)
    {
        MarkUiSettingsDirty();
    }

    AutosaveUiSettingsIfNeeded();
}

// ==================== 각 패널 ====================

void ImGuiManager::DrawScenePanel(Engine* engine)
{
    if (!ImGui::Begin("Scene", &mShowScene))
    {
        ImGui::End();
        return;
    }

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

        // --- LMB ---
        // UI create mode: drag = place UI image rect (selected material)
        // Normal mode: short click = mouse look / drag = box multi-select
        ImGuiIO& io = ImGui::GetIO();
        const bool shift = io.KeyShift;

        if (mUiImageCreateMode)
        {
            // Banner (locks ticked in TickUiImageCreateMode — not here)
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const char* msg =
                    !IsUiCreateSceneInputOk()
                    ? "UI Create: ON — release mouse, then drag Scene  |  Complete in Project"
                    : (mUiCreateHasRect
                        ? "UI Create: rect set — press Complete in Project  |  re-drag OK"
                        : "UI Create: drag on Scene  |  then Complete in Project");
                dl->AddRectFilled(
                    ImVec2(min.x + 8.0f, min.y + 8.0f),
                    ImVec2(min.x + 8.0f + ImGui::CalcTextSize(msg).x + 12.0f, min.y + 28.0f),
                    IM_COL32(20, 20, 20, 180), 4.0f);
                dl->AddText(ImVec2(min.x + 14.0f, min.y + 12.0f), IM_COL32(120, 220, 160, 255), msg);
            }

            auto drawCreateRect = [&](ImVec2 a, ImVec2 b, bool confirmed)
            {
                if (a.x > b.x) std::swap(a.x, b.x);
                if (a.y > b.y) std::swap(a.y, b.y);
                a.x = (std::max)(min.x, (std::min)(a.x, max.x));
                a.y = (std::max)(min.y, (std::min)(a.y, max.y));
                b.x = (std::max)(min.x, (std::min)(b.x, max.x));
                b.y = (std::max)(min.y, (std::min)(b.y, max.y));
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                const ImU32 fill = confirmed ? IM_COL32(80, 220, 140, 70) : IM_COL32(80, 220, 140, 40);
                const ImU32 line = confirmed ? IM_COL32(80, 255, 160, 255) : IM_COL32(80, 220, 140, 220);
                dl->AddRectFilled(a, b, fill);
                dl->AddRect(a, b, line, 0.0f, 0, confirmed ? 2.5f : 2.0f);
            };

            auto applyCreateAspectIfNeeded = [&](float& x0, float& y0, float& x1, float& y1)
            {
                if (!mUiCreateFitImageAspect)
                    return;
                float aspect = 1.f;
                if (!TryGetMaterialTextureAspect(mSelectedUiMaterial, aspect))
                    return;
                FitRectToAspect(x0, y0, x1, y1, aspect);
            };

            if (mUiCreateHasRect && !mUiCreateDragging)
            {
                drawCreateRect(
                    ImVec2(min.x + mUiCreateMinX, min.y + mUiCreateMinY),
                    ImVec2(min.x + mUiCreateMaxX, min.y + mUiCreateMaxY),
                    true);
            }

            // Scene drag only after mouse-up + short lock from the Project Create click
            if (IsUiCreateSceneInputOk())
            {
                if (mSceneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    mUiCreateDragging = true;
                    mUiCreateStartScreen = io.MousePos;
                    mUiCreateEndScreen = io.MousePos;
                }

                if (mUiCreateDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    mUiCreateEndScreen = io.MousePos;
                    ImVec2 a = mUiCreateStartScreen;
                    ImVec2 b = mUiCreateEndScreen;
                    if (mUiCreateFitImageAspect)
                    {
                        float lx0 = a.x - min.x, ly0 = a.y - min.y;
                        float lx1 = b.x - min.x, ly1 = b.y - min.y;
                        applyCreateAspectIfNeeded(lx0, ly0, lx1, ly1);
                        a = ImVec2(min.x + lx0, min.y + ly0);
                        b = ImVec2(min.x + lx1, min.y + ly1);
                    }
                    drawCreateRect(a, b, false);
                }

                // Drag end: store preview only — mode stays ON until Complete/Cancel/Esc
                if (mUiCreateDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    mUiCreateEndScreen = io.MousePos;
                    mUiCreateDragging = false;

                    float x0 = mUiCreateStartScreen.x - min.x;
                    float y0 = mUiCreateStartScreen.y - min.y;
                    float x1 = mUiCreateEndScreen.x - min.x;
                    float y1 = mUiCreateEndScreen.y - min.y;
                    if (x0 > x1) std::swap(x0, x1);
                    if (y0 > y1) std::swap(y0, y1);
                    const float sw = max.x - min.x;
                    const float sh = max.y - min.y;
                    x0 = (std::max)(0.0f, (std::min)(x0, sw));
                    y0 = (std::max)(0.0f, (std::min)(y0, sh));
                    x1 = (std::max)(0.0f, (std::min)(x1, sw));
                    y1 = (std::max)(0.0f, (std::min)(y1, sh));

                    applyCreateAspectIfNeeded(x0, y0, x1, y1);
                    x0 = (std::max)(0.0f, (std::min)(x0, sw));
                    y0 = (std::max)(0.0f, (std::min)(y0, sh));
                    x1 = (std::max)(0.0f, (std::min)(x1, sw));
                    y1 = (std::max)(0.0f, (std::min)(y1, sh));

                    if ((x1 - x0) >= kBoxDragThresholdPx && (y1 - y0) >= kBoxDragThresholdPx)
                    {
                        mUiCreateMinX = x0;
                        mUiCreateMinY = y0;
                        mUiCreateMaxX = x1;
                        mUiCreateMaxY = y1;
                        mUiCreateHasRect = true;
                        OutputDebugStringA("[UI] preview rect set (mode stays ON)\n");
                    }
                }
            }

            mBoxDragging = false;
            mUiManipDragging = false;
        }
        else if (mManipulateUi && engine)
        {
            // --- UI click-drag-drop (Tools → Manipulate UI) ---
            // Free move while dragging; if Snap UI is on, always snap on drop.
            const float sw = max.x - min.x;
            const float sh = max.y - min.y;
            const float localX = io.MousePos.x - min.x;
            const float localY = io.MousePos.y - min.y;

            if (mSceneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                const Entity hit = engine->PickUiScreen(
                    localX, localY,
                    static_cast<UINT>((std::max)(1.0f, sw)),
                    static_cast<UINT>((std::max)(1.0f, sh)));
                if (hit != INVALID_ENTITY)
                {
                    if (!engine->IsEntitySelected(hit))
                        engine->SetSelectedEntity(hit);
                    mUiManipDragging = true;
                    mUiDragMoved = false;
                    mUiDragLastLocalX = localX;
                    mUiDragLastLocalY = localY;
                    mBoxDragging = false;
                }
                else
                {
                    // Missed UI: mesh box-select / short click path
                    mUiManipDragging = false;
                    mBoxDragging = true;
                    mBoxStartScreen = io.MousePos;
                    mBoxEndScreen = io.MousePos;
                    mBoxSelectAdditive = shift;
                }
            }

            if (mUiManipDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const float dx = localX - mUiDragLastLocalX;
                const float dy = localY - mUiDragLastLocalY;
                if (fabsf(dx) > 0.01f || fabsf(dy) > 0.01f)
                {
                    mUiDragMoved = true;
                    if (sw > 1.f && sh > 1.f)
                        engine->MoveSelectedUi(dx / sw, dy / sh);
                    mUiDragLastLocalX = localX;
                    mUiDragLastLocalY = localY;
                }

                ImDrawList* dl = ImGui::GetForegroundDrawList();
                const char* hint = mSnapUi ? "UI drag (snap on drop)" : "UI drag";
                dl->AddText(ImVec2(min.x + 10.f, min.y + 10.f), IM_COL32(180, 230, 255, 255), hint);
            }

            if (mUiManipDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                // Always snap on drop when Snap UI is enabled (including after a previous snap).
                if (mUiDragMoved && mSnapUi)
                    engine->SnapSelectedUi(mSnapUiThresholdPercent, sw, sh);

                mUiManipDragging = false;
                mUiDragMoved = false;
            }

            // Box select when UI manip is on but started on empty space
            if (mBoxDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !mUiManipDragging)
            {
                mBoxEndScreen = io.MousePos;
                const float dx = mBoxEndScreen.x - mBoxStartScreen.x;
                const float dy = mBoxEndScreen.y - mBoxStartScreen.y;
                if ((dx * dx + dy * dy) >= kBoxDragThresholdPx * kBoxDragThresholdPx)
                {
                    ImDrawList* dl = ImGui::GetForegroundDrawList();
                    ImVec2 a = mBoxStartScreen;
                    ImVec2 b = mBoxEndScreen;
                    if (a.x > b.x) std::swap(a.x, b.x);
                    if (a.y > b.y) std::swap(a.y, b.y);
                    a.x = (std::max)(min.x, (std::min)(a.x, max.x));
                    a.y = (std::max)(min.y, (std::min)(a.y, max.y));
                    b.x = (std::max)(min.x, (std::min)(b.x, max.x));
                    b.y = (std::max)(min.y, (std::min)(b.y, max.y));
                    dl->AddRectFilled(a, b, IM_COL32(80, 160, 255, 40));
                    dl->AddRect(a, b, IM_COL32(80, 160, 255, 220), 0.0f, 0, 1.5f);
                }
            }

            if (mBoxDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !mUiManipDragging)
            {
                mBoxEndScreen = io.MousePos;
                const float dx = mBoxEndScreen.x - mBoxStartScreen.x;
                const float dy = mBoxEndScreen.y - mBoxStartScreen.y;
                const float dist2 = dx * dx + dy * dy;
                mBoxDragging = false;

                if (dist2 >= kBoxDragThresholdPx * kBoxDragThresholdPx)
                {
                    float x0 = mBoxStartScreen.x - min.x;
                    float y0 = mBoxStartScreen.y - min.y;
                    float x1 = mBoxEndScreen.x - min.x;
                    float y1 = mBoxEndScreen.y - min.y;
                    if (x0 > x1) std::swap(x0, x1);
                    if (y0 > y1) std::swap(y0, y1);
                    x0 = (std::max)(0.0f, (std::min)(x0, sw));
                    y0 = (std::max)(0.0f, (std::min)(y0, sh));
                    x1 = (std::max)(0.0f, (std::min)(x1, sw));
                    y1 = (std::max)(0.0f, (std::min)(y1, sh));
                    mBoxResultMinX = x0;
                    mBoxResultMinY = y0;
                    mBoxResultMaxX = x1;
                    mBoxResultMaxY = y1;
                    mBoxSelectPending = true;
                }
                else if (mSceneHovered)
                {
                    mSceneCaptureClick = true;
                }
            }
        }
        else
        {
            mUiManipDragging = false;

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
                const float dx = mBoxEndScreen.x - mBoxStartScreen.x;
                const float dy = mBoxEndScreen.y - mBoxStartScreen.y;
                if ((dx * dx + dy * dy) >= kBoxDragThresholdPx * kBoxDragThresholdPx)
                {
                    ImDrawList* dl = ImGui::GetForegroundDrawList();
                    ImVec2 a = mBoxStartScreen;
                    ImVec2 b = mBoxEndScreen;
                    if (a.x > b.x) std::swap(a.x, b.x);
                    if (a.y > b.y) std::swap(a.y, b.y);
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
                }
                else if (mSceneHovered)
                {
                    mSceneCaptureClick = true;
                }
            }
        }
    }
    else
    {
        ImGui::TextDisabled("Scene render target not ready (%u x %u)", width, height);
        mBoxDragging = false;
        mUiManipDragging = false;
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

void ImGuiManager::TickUiImageCreateMode()
{
    // Deferred enable: Create was clicked last frame (or earlier this frame via API).
    // Applying here means Project never draws Cancel on the same submit as Create.
    if (mUiCreateEnablePending)
    {
        mUiCreateEnablePending = false;
        mUiImageCreateMode = true;
        mUiCreateWaitMouseRelease = true;
        mUiCreateSceneLockFrames = 3;
        // Longer than a double-click gap so a second click cannot hit Cancel.
        mUiCreateExitLockFrames = 24;
        mUiCreateDragging = false;
        mUiCreateHasRect = false;
        mUiCreatePending = false;
        mBoxDragging = false;
        mBoxSelectPending = false;
        mSceneCaptureClick = false;
        OutputDebugStringA("[UI] create mode ON\n");
    }

    if (!mUiImageCreateMode)
        return;

    if (mUiCreateWaitMouseRelease)
    {
        // Stay locked until LMB is fully up (Create click release settles).
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            mUiCreateWaitMouseRelease = false;
        return;
    }

    if (mUiCreateSceneLockFrames > 0)
        --mUiCreateSceneLockFrames;
    if (mUiCreateExitLockFrames > 0)
        --mUiCreateExitLockFrames;

    // Esc only after exit arm (same gate as Cancel). Never while typing in ImGui.
    if (IsUiCreateExitOk()
        && !ImGui::GetIO().WantTextInput
        && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        SetUiImageCreateMode(false);
        OutputDebugStringA("[UI] create mode OFF (Esc)\n");
    }
}

bool ImGuiManager::IsUiCreateSceneInputOk() const
{
    return mUiImageCreateMode
        && !mUiCreateWaitMouseRelease
        && mUiCreateSceneLockFrames <= 0;
}

bool ImGuiManager::IsUiCreateExitOk() const
{
    return mUiImageCreateMode
        && !mUiCreateWaitMouseRelease
        && mUiCreateExitLockFrames <= 0;
}

void ImGuiManager::SetUiImageCreateMode(bool on)
{
    if (on)
    {
        // Already on or already queued — do not reset in-progress rect/drag.
        if (mUiImageCreateMode || mUiCreateEnablePending)
            return;
        mUiCreateEnablePending = true;
        return;
    }

    mUiImageCreateMode = false;
    mUiCreateEnablePending = false;
    mUiCreateWaitMouseRelease = false;
    mUiCreateSceneLockFrames = 0;
    mUiCreateExitLockFrames = 0;
    mUiCreateHasRect = false;
    mUiCreateDragging = false;
    OutputDebugStringA("[UI] create mode OFF\n");
}

bool ImGuiManager::ConsumeUiImageCreate(float& outMinX, float& outMinY, float& outMaxX, float& outMaxY)
{
    if (!mUiCreatePending)
        return false;
    mUiCreatePending = false;
    outMinX = mUiCreateMinX;
    outMinY = mUiCreateMinY;
    outMaxX = mUiCreateMaxX;
    outMaxY = mUiCreateMaxY;
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
    if (!ImGui::Begin("Hierarchy", &mShowHierarchy, kPanelWindowFlags))
    {
        ImGui::End();
        return;
    }

    if (!engine)
    {
        TextLineDisabled("Engine not available");
        ImGui::End();
        return;
    }

    TextLine("Objects: %zu", engine->GetRenderableObjectCount());
    TextLine("UI images: %zu", engine->GetUiEntities().size());
    TextLine("FPS: %.1f", ImGui::GetIO().Framerate);

    const size_t selCount = engine->GetSelectedCount();
    if (selCount == 0)
        TextLineDisabled("Selected: (none)");
    else if (selCount == 1)
        TextLine("Selected: %u", engine->GetSelectedEntity());
    else
        TextLine("Selected: %zu entities", selCount);

    if (ImGui::Button("Clear Selection", ImVec2(-FLT_MIN, 0)))
        engine->ClearSelection();

    TextLineDisabled("3D: LMB drag box | Shift+drag add | RMB pick (off while Manipulate Mesh)");
    TextLineDisabled("UI: short LMB on image | list click");
    ImGui::Separator();

    auto entities = engine->GetRenderableEntities();
    auto uiEntities = engine->GetUiEntities();

    if (ImGui::BeginChild("##EntityList", ImVec2(0, 0), ImGuiChildFlags_Borders, kPanelWindowFlags))
    {
        ImGui::SeparatorText("3D Objects");
        if (entities.empty())
            TextLineDisabled("No renderable entities");
        else
        {
            for (Entity e : entities)
            {
                RenderableComponent* rend = engine->GetRenderable(e);
                const char* meshName = (rend && rend->mesh) ? rend->mesh->name.c_str() : "(no mesh)";
                char label[160];
                snprintf(label, sizeof(label), "Entity %u  [%s]", e, meshName);
                const bool isSelected = engine->IsEntitySelected(e);
                if (SelectableFull(label, isSelected))
                {
                    if (ImGui::GetIO().KeyCtrl)
                        engine->ToggleSelectedEntity(e);
                    else
                        engine->SetSelectedEntity(e);
                }
            }
        }

        ImGui::SeparatorText("UI Objects");
        if (uiEntities.empty())
            TextLineDisabled("No UI objects");
        else
        {
            for (Entity e : uiEntities)
            {
                UiImageComponent* img = engine->GetUiImage(e);
                UiElementComponent* el = engine->GetUiElement(e);
                const char* mat = (img && !img->materialName.empty()) ? img->materialName.c_str() : "?";
                const bool vis = el ? el->visible : false;
                char label[192];
                snprintf(label, sizeof(label), "UI %u  [%s]%s", e, mat, vis ? "" : " (hidden)");
                const bool isSelected = engine->IsEntitySelected(e);
                if (SelectableFull(label, isSelected))
                    engine->SetSelectedEntity(e);
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

void ImGuiManager::DrawToolsContent()
{
    ImGui::SeparatorText("Mesh");
    if (CheckboxWrapped("Manipulate Mesh", &mManipulateSelected))
    {
        if (m_Callback)
            m_Callback->buttonClicked(ButtonAction::ToggleManipulateSelected);
    }
    if (mManipulateSelected)
    {
        TextLine(
            "Orbit: mouse | Move: WASD | Space/Ctrl: Y | Shift: faster | Wheel: speed | RMB: L/R zoom, U/D cam Y");
        CheckboxWrapped("Snap Mesh (AABB faces)", &mSnapMesh);
        if (mSnapMesh)
        {
            DragFloatFull("Mesh snap distance", &mSnapMeshThreshold, 0.01f, 0.01f, 10.f);
            TextLineDisabled("Snaps whole-mesh bounds faces/centers to other meshes.");
            TextLineDisabled("Hold Shift while moving to bypass snap (and go faster).");
        }
    }
    else
    {
        TextLineDisabled("Enable to orbit / move selected mesh objects.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("UI");
    if (CheckboxWrapped("Manipulate UI", &mManipulateUi))
    {
        if (m_Callback)
            m_Callback->buttonClicked(ButtonAction::ToggleManipulateUi);
    }
    if (mManipulateUi)
    {
        TextLine("Scene: LMB on UI → drag → drop. Empty space: box select / mouse look.");
        CheckboxWrapped("Snap UI", &mSnapUi);
        if (mSnapUi)
        {
            DragFloatFull("UI snap distance (%)", &mSnapUiThresholdPercent, 0.05f, 0.1f, 20.f);
            TextLineDisabled("%% of canvas W (left/right) and H (top/bottom). Default 1%%.");
            TextLineDisabled("Drop: prefer edge snap (L/R/T/B); corners need closer approach.");
        }
    }
    else
    {
        TextLineDisabled("Enable to drag screen UI with the mouse on Scene.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    TextLine("Tips");
    BulletLine("This panel docks next to Hierarchy as a tab.");
    BulletLine("Drag any window title onto another to add a tab.");
    BulletLine("View menu re-opens closed panels.");
}

void ImGuiManager::DrawToolsPanel()
{
    if (!ImGui::Begin("Tools", &mShowTools, kPanelWindowFlags))
    {
        ImGui::End();
        return;
    }
    DrawToolsContent();
    ImGui::End();
}

void ImGuiManager::DrawInspectorPanel(Engine* engine)
{
    if (!ImGui::Begin("Inspector", &mShowInspector, kPanelWindowFlags))
    {
        ImGui::End();
        return;
    }

    if (!engine)
    {
        TextLineDisabled("Engine not available");
        ImGui::End();
        return;
    }

    const std::vector<Entity> selectedList = engine->GetSelectedEntities();
    if (selectedList.empty())
    {
        TextLineDisabled("No entity selected");
        TextLineDisabled("Scene drag-box, RMB pick, UI click, or Hierarchy.");
        ImGui::End();
        return;
    }

    // UI 표시용 primary = 목록 첫 엔티티 (조작 값은 여기서 읽고, 변경 시 전체에 적용)
    const Entity primary = selectedList.front();

    if (selectedList.size() == 1)
        TextLine("Entity: %u", primary);
    else
        TextLine("Multi-select: %zu (edits apply to all)", selectedList.size());

    auto forEachSelected = [&](auto&& fn)
    {
        for (Entity e : selectedList)
            fn(e);
    };

    // --- UI Object inspector (same Transform / Material style as 3D objects) ---
    if (UiElementComponent* uiEl = engine->GetUiElement(primary))
    {
        TextLineDisabled("UI Object");

        if (!ImGui::BeginTabBar("UiInspectorTabs",
                ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_DrawSelectedOverline))
        {
            ImGui::End();
            return;
        }

        // --- Transform (layout + optional world Transform for billboards) ---
        if (ImGui::BeginTabItem("Transform"))
        {
            if (uiEl->mode == UiSpaceMode::WorldBillboard)
            {
                if (TransformComponent* tf = engine->GetTransform(primary))
                {
                    TextLine("World position (billboard)");
                    XMFLOAT3 pos = tf->position;
                    if (DragFloat3Full("Position", &pos.x, 0.05f))
                    {
                        const XMFLOAT3 delta{
                            pos.x - tf->position.x,
                            pos.y - tf->position.y,
                            pos.z - tf->position.z
                        };
                        forEachSelected([&](Entity e)
                        {
                            if (!engine->GetUiElement(e))
                                return;
                            if (TransformComponent* t = engine->GetTransform(e))
                            {
                                t->position.x += delta.x;
                                t->position.y += delta.y;
                                t->position.z += delta.z;
                                t->MarkDirty(e);
                            }
                        });
                    }
                    ImGui::Spacing();
                    ImGui::Separator();
                }
                else
                {
                    TextLineDisabled("WorldBillboard without TransformComponent");
                }
            }

            TextLine("Layout");
            TextLineDisabled(uiEl->layoutPercent
                ? "Screen: center & size as %% of design/live canvas"
                : "Legacy pixel layout");

            if (uiEl->layoutPercent)
            {
                float center[2] = { uiEl->position.x * 100.f, uiEl->position.y * 100.f };
                float sizePct[2] = { uiEl->size.x * 100.f, uiEl->size.y * 100.f };
                if (DragFloatFull("Center X %", &center[0], 0.1f, 0.f, 100.f))
                {
                    const float v = center[0] * 0.01f;
                    const float d = v - uiEl->position.x;
                    forEachSelected([&](Entity e)
                    {
                        if (UiElementComponent* el = engine->GetUiElement(e))
                            el->position.x += d;
                    });
                }
                if (DragFloatFull("Center Y %", &center[1], 0.1f, 0.f, 100.f))
                {
                    const float v = center[1] * 0.01f;
                    const float d = v - uiEl->position.y;
                    forEachSelected([&](Entity e)
                    {
                        if (UiElementComponent* el = engine->GetUiElement(e))
                            el->position.y += d;
                    });
                }
                if (DragFloatFull("Width %", &sizePct[0], 0.1f, 0.1f, 100.f))
                {
                    const float v = sizePct[0] * 0.01f;
                    forEachSelected([&](Entity e)
                    {
                        if (UiElementComponent* el = engine->GetUiElement(e))
                            el->size.x = v;
                    });
                }
                if (DragFloatFull("Height %", &sizePct[1], 0.1f, 0.1f, 100.f))
                {
                    const float v = sizePct[1] * 0.01f;
                    forEachSelected([&](Entity e)
                    {
                        if (UiElementComponent* el = engine->GetUiElement(e))
                            el->size.y = v;
                    });
                }
            }
            else
            {
                float pos[2] = { uiEl->position.x, uiEl->position.y };
                float size[2] = { uiEl->size.x, uiEl->size.y };
                if (DragFloatFull("Pos X", &pos[0], 1.f))
                {
                    const float d = pos[0] - uiEl->position.x;
                    forEachSelected([&](Entity e)
                    {
                        if (UiElementComponent* el = engine->GetUiElement(e))
                            el->position.x += d;
                    });
                }
                if (DragFloatFull("Pos Y", &pos[1], 1.f))
                {
                    const float d = pos[1] - uiEl->position.y;
                    forEachSelected([&](Entity e)
                    {
                        if (UiElementComponent* el = engine->GetUiElement(e))
                            el->position.y += d;
                    });
                }
                if (DragFloatFull("Width", &size[0], 1.f, 1.f, 10000.f))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (UiElementComponent* el = engine->GetUiElement(e))
                            el->size.x = size[0];
                    });
                }
                if (DragFloatFull("Height", &size[1], 1.f, 1.f, 10000.f))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (UiElementComponent* el = engine->GetUiElement(e))
                            el->size.y = size[1];
                    });
                }
            }

            float rotDeg = uiEl->rotationRad * (180.f / 3.14159265f);
            if (DragFloatFull("Rotation (deg)", &rotDeg, 0.5f, -180.f, 180.f))
            {
                const float rad = rotDeg * (3.14159265f / 180.f);
                forEachSelected([&](Entity e)
                {
                    if (UiElementComponent* el = engine->GetUiElement(e))
                        el->rotationRad = rad;
                });
            }

            float pivot[2] = { uiEl->pivot.x, uiEl->pivot.y };
            if (DragFloatFull("Pivot X", &pivot[0], 0.01f, 0.f, 1.f))
            {
                forEachSelected([&](Entity e)
                {
                    if (UiElementComponent* el = engine->GetUiElement(e))
                        el->pivot.x = pivot[0];
                });
            }
            if (DragFloatFull("Pivot Y", &pivot[1], 0.01f, 0.f, 1.f))
            {
                forEachSelected([&](Entity e)
                {
                    if (UiElementComponent* el = engine->GetUiElement(e))
                        el->pivot.y = pivot[1];
                });
            }

            if (uiEl->designW > 1.f && uiEl->designH > 1.f)
            {
                TextLine("Design canvas: %.0f x %.0f", uiEl->designW, uiEl->designH);
                if (uiEl->layoutPercent && uiEl->size.y > 1e-6f)
                {
                    const float pxW = uiEl->size.x * uiEl->designW;
                    const float pxH = uiEl->size.y * uiEl->designH;
                    TextLineDisabled("Authored size ~ %.0fx%.0f px (aspect %.3f)",
                        pxW, pxH, pxW / pxH);
                }
            }
            else
            {
                TextLineDisabled("Design canvas unset (needed for Keep Aspect size)");
            }

            ImGui::EndTabItem();
        }

        // --- Material (image + tint) ---
        if (ImGui::BeginTabItem("Material"))
        {
            UiImageComponent* img = engine->GetUiImage(primary);
            if (!img)
            {
                TextLineDisabled("No UiImageComponent");
            }
            else
            {
                auto matNames = MaterialManager::Get().GetLoadedMaterialNames();
                std::sort(matNames.begin(), matNames.end());

                int matIdx = 0;
                if (!img->materialName.empty())
                {
                    auto it = std::find(matNames.begin(), matNames.end(), img->materialName);
                    if (it != matNames.end())
                        matIdx = 1 + (int)std::distance(matNames.begin(), it);
                }

                if (!matNames.empty())
                {
                    if (ComboFull("Material", &matIdx, MainMaterialComboGetter,
                            &matNames, (int)matNames.size() + 1))
                    {
                        const std::string newMat =
                            (matIdx == 0) ? "Default" : matNames[matIdx - 1];
                        forEachSelected([&](Entity e)
                        {
                            if (UiImageComponent* uiImg = engine->GetUiImage(e))
                                uiImg->materialName = newMat;
                        });
                    }
                }
                else
                {
                    TextLine("Material: %s",
                        img->materialName.empty() ? "(none)" : img->materialName.c_str());
                    TextLineDisabled("No materials loaded");
                }

                float col[4] = {
                    img->color.x, img->color.y, img->color.z, img->color.w
                };
                if (ImGui::ColorEdit4("Color##ui_tint", col))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (UiImageComponent* uiImg = engine->GetUiImage(e))
                            uiImg->color = { col[0], col[1], col[2], col[3] };
                    });
                }

                float uv[4] = {
                    img->uvRect.x, img->uvRect.y, img->uvRect.z, img->uvRect.w
                };
                if (DragFloatFull("UV u0", &uv[0], 0.01f, 0.f, 1.f) ||
                    DragFloatFull("UV v0", &uv[1], 0.01f, 0.f, 1.f) ||
                    DragFloatFull("UV u1", &uv[2], 0.01f, 0.f, 1.f) ||
                    DragFloatFull("UV v1", &uv[3], 0.01f, 0.f, 1.f))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (UiImageComponent* uiImg = engine->GetUiImage(e))
                            uiImg->uvRect = { uv[0], uv[1], uv[2], uv[3] };
                    });
                }

                TextLineDisabled("None in combo → Default material name.");
            }
            ImGui::EndTabItem();
        }

        // --- UI (flags / mode / delete) ---
        if (ImGui::BeginTabItem("UI"))
        {
            const char* modeName = "ScreenAlways";
            if (uiEl->mode == UiSpaceMode::ScreenConditional)
                modeName = "ScreenConditional";
            else if (uiEl->mode == UiSpaceMode::WorldBillboard)
                modeName = "WorldBillboard";
            TextLine("Space mode: %s", modeName);

            int scaleMode = static_cast<int>(uiEl->scaleMode);
            const char* scaleItems[] = {
                "Stretch (fill, may squash)",
                "Keep Aspect Fit",
                "Keep Aspect Fill"
            };
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("Scale Mode##ui_el_scale", &scaleMode, scaleItems, IM_ARRAYSIZE(scaleItems)))
            {
                const UiScaleMode sm = static_cast<UiScaleMode>(scaleMode);
                float gw = 0.f, gh = 0.f;
                engine->GetUiDesignResolution(gw, gh);
                forEachSelected([&](Entity e)
                {
                    if (UiElementComponent* el = engine->GetUiElement(e))
                    {
                        el->scaleMode = sm;
                        // Uniform* needs design canvas for stable aspect
                        if ((sm == UiScaleMode::UniformMin || sm == UiScaleMode::UniformMax)
                            && (el->designW < 1.f || el->designH < 1.f)
                            && gw > 1.f && gh > 1.f)
                        {
                            if (el->designW < 1.f) el->designW = gw;
                            if (el->designH < 1.f) el->designH = gh;
                        }
                    }
                });
            }
            TextLineDisabled("Per-widget. New UI inherits UI panel default below Create.");

            bool vis = uiEl->visible;
            if (CheckboxWrapped("Visible", &vis))
            {
                forEachSelected([&](Entity e)
                {
                    if (engine->GetUiElement(e))
                        engine->SetUiVisible(e, vis);
                });
            }
            bool act = uiEl->active;
            if (CheckboxWrapped("Active", &act))
            {
                forEachSelected([&](Entity e)
                {
                    if (engine->GetUiElement(e))
                        engine->SetUiActive(e, act);
                });
            }
            // Mode-specific meaning (see ComponentStruct UiElementShould*)
            if (uiEl->mode == UiSpaceMode::ScreenConditional)
                TextLineDisabled("Active: Conditional mode — must be ON to draw and click.");
            else if (uiEl->mode == UiSpaceMode::ScreenAlways)
                TextLineDisabled("Active: Always mode — draw uses Visible only; Active off = no click.");
            else
                TextLineDisabled("Active: WorldBillboard — draw uses Visible; Active off = no pointer.");

            int z = uiEl->zOrder;
            if (ImGui::DragInt("Z Order##ui_z", &z, 1, -10000, 10000))
            {
                forEachSelected([&](Entity e)
                {
                    if (UiElementComponent* el = engine->GetUiElement(e))
                        el->zOrder = z;
                });
            }

            ImGui::Spacing();
            if (ImGui::Button("Delete UI Object", ImVec2(-FLT_MIN, 0)))
            {
                // Destroy all selected UI entities
                std::vector<Entity> toKill;
                for (Entity e : selectedList)
                {
                    if (engine->GetUiElement(e))
                        toKill.push_back(e);
                }
                engine->ClearSelection();
                for (Entity e : toKill)
                    engine->DestroyUiEntity(e);
                ImGui::EndTabItem();
                ImGui::EndTabBar();
                ImGui::End();
                return;
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
        ImGui::End();
        return;
    }

    if (!ImGui::BeginTabBar("InspectorTabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_DrawSelectedOverline))
    {
        ImGui::End();
        return;
    }

    // --- Transform ---
    if (ImGui::BeginTabItem("Transform"))
    {
        if (TransformComponent* tf = engine->GetTransform(primary))
        {
            XMFLOAT3 pos = tf->position;
            if (DragFloat3Full("Position", &pos.x, 0.05f))
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
                        t->MarkDirty(e);
                    }
                });
            }

            XMFLOAT3 rot = tf->rotation;
            if (DragFloat3Full("Rotation", &rot.x, 0.01f))
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
                        t->MarkDirty(e);
                    }
                });
            }

            XMFLOAT3 scl = tf->scale;
            if (DragFloat3Full("Scale", &scl.x, 0.01f, 0.001f, 100.0f))
            {
                forEachSelected([&](Entity e)
                {
                    if (TransformComponent* t = engine->GetTransform(e))
                    {
                        t->scale = scl;
                        t->MarkDirty(e);
                    }
                });
            }
        }
        else
        {
            TextLineDisabled("No TransformComponent on primary entity");
        }
        ImGui::EndTabItem();
    }

    // --- Physics (Gravity + Collision) ---
    if (ImGui::BeginTabItem("Physics"))
    {
        ImGui::Separator();
        TextLine("Gravity");
        {
            bool hasGravity = engine->HasGravityComponent(primary);
            if (CheckboxWrapped("Gravity Component", &hasGravity))
            {
                forEachSelected([&](Entity e)
                {
                    engine->SetEntityGravityEnabled(e, hasGravity);
                });
            }

            if (GravityComponent* gravity = engine->GetGravityComponent(primary))
            {
                bool gEnabled = gravity->enabled;
                if (CheckboxWrapped("Enabled##Gravity", &gEnabled))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (GravityComponent* g = engine->GetGravityComponent(e))
                            g->enabled = gEnabled;
                        engine->NotifyEntityMotionChanged(e);
                    });
                }
                float strength = gravity->strength;
                if (DragFloatFull("Strength", &strength, 0.1f, 0.0f, 50.0f))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (GravityComponent* g = engine->GetGravityComponent(e))
                            g->strength = strength;
                        engine->NotifyEntityMotionChanged(e);
                    });
                }
                XMFLOAT3 vel = gravity->velocity;
                if (DragFloat3Full("Velocity", &vel.x, 0.1f))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (GravityComponent* g = engine->GetGravityComponent(e))
                            g->velocity = vel;
                        engine->NotifyEntityMotionChanged(e);
                    });
                }
                XMFLOAT3 ang = gravity->angularVelocity;
                if (DragFloat3Full("Angular Vel", &ang.x, 0.01f))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (GravityComponent* g = engine->GetGravityComponent(e))
                            g->angularVelocity = ang;
                        engine->NotifyEntityMotionChanged(e);
                    });
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        TextLine("Collision");
        {
            bool hasCollision = engine->HasCollisionComponent(primary);
            if (CheckboxWrapped("Collision Component", &hasCollision))
            {
                forEachSelected([&](Entity e)
                {
                    engine->SetEntityCollisionEnabled(e, hasCollision);
                });
            }

            if (CollisionComponent* collision = engine->GetCollisionComponent(primary))
            {
                bool cEnabled = collision->enabled;
                if (CheckboxWrapped("Enabled##Collision", &cEnabled))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (CollisionComponent* c = engine->GetCollisionComponent(e))
                            c->enabled = cEnabled;
                    });
                }
                bool isStatic = collision->isStatic;
                if (CheckboxWrapped("Static", &isStatic))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (CollisionComponent* c = engine->GetCollisionComponent(e))
                            c->isStatic = isStatic;
                    });
                }
                float restitution = collision->restitution;
                if (DragFloatFull("Restitution", &restitution, 0.01f, 0.0f, 1.0f))
                {
                    forEachSelected([&](Entity e)
                    {
                        if (CollisionComponent* c = engine->GetCollisionComponent(e))
                            c->restitution = restitution;
                    });
                }
            }
        }
        ImGui::EndTabItem();
    }

    // --- Material ---
    if (ImGui::BeginTabItem("Material"))
    {
        RenderableComponent* rend = engine->GetRenderable(primary);
        if (rend && rend->mesh)
        {
            TextLine("Mesh: %s", rend->mesh->name.c_str());
            TextLineDisabled("Priority: Sub > Main > Init");
            if (selectedList.size() > 1)
                TextLineDisabled("Material changes apply to all selected");

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

            if (ComboFull("Main Material", &mainIdx, MainMaterialComboGetter, &matNames, (int)matNames.size() + 1))
            {
                std::string newMain = (mainIdx == 0) ? "" : matNames[mainIdx - 1];
                forEachSelected([&](Entity e)
                {
                    engine->SetEntityMainMaterial(e, newMain);
                });
            }

            bool visible = rend->visible;
            if (CheckboxWrapped("Visible", &visible))
            {
                forEachSelected([&](Entity e)
                {
                    if (RenderableComponent* r = engine->GetRenderable(e))
                        r->visible = visible;
                });
            }

            ImGui::Spacing();
            ImGui::Separator();
            TextLine("Submesh Overrides");
            std::vector<std::string> submeshKeys;
            submeshKeys.reserve(rend->mesh->DrawArgs.size());
            for (const auto& pair : rend->mesh->DrawArgs)
                submeshKeys.push_back(pair.first);
            std::sort(submeshKeys.begin(), submeshKeys.end());

            for (const auto& key : submeshKeys)
            {
                const auto& sub = rend->mesh->DrawArgs.at(key);
                ImGui::PushID(key.c_str());
                TextLine("%s (Init: %s)", key.c_str(),
                    sub.initMaterialName.empty() ? "Default" : sub.initMaterialName.c_str());

                const std::string currentSub = engine->GetEntitySubMaterial(primary, key);

                int subIdx = 0;
                if (!currentSub.empty())
                {
                    auto it = std::find(matNames.begin(), matNames.end(), currentSub);
                    if (it != matNames.end())
                        subIdx = 1 + (int)std::distance(matNames.begin(), it);
                }

                if (ComboFull("Sub Material", &subIdx, SubMaterialComboGetter, &matNames, (int)matNames.size() + 1))
                {
                    std::string newSub = (subIdx == 0) ? "" : matNames[subIdx - 1];
                    forEachSelected([&](Entity e)
                    {
                        if (RenderableComponent* r = engine->GetRenderable(e))
                        {
                            if (r->mesh && r->mesh->DrawArgs.count(key))
                                engine->SetEntitySubMaterial(e, key, newSub);
                        }
                    });
                }
                ImGui::PopID();
            }
        }
        else
        {
            TextLineDisabled("Primary entity has no RenderableComponent");
        }
        ImGui::EndTabItem();
    }

    // --- Object (delete mesh / renderable) ---
    if (ImGui::BeginTabItem("Object"))
    {
        const bool canDelete = engine->GetRenderable(primary) != nullptr;
        TextLineDisabled("Deletes selected mesh objects from the scene (not asset files).");
        if (!canDelete)
            ImGui::BeginDisabled();
        if (ImGui::Button("Delete Mesh Object", ImVec2(-FLT_MIN, 0)))
        {
            std::vector<Entity> toKill;
            for (Entity e : selectedList)
            {
                if (engine->GetRenderable(e))
                    toKill.push_back(e);
            }
            engine->ClearSelection();
            for (Entity e : toKill)
                engine->DestroyRenderableEntity(e);
            ImGui::EndTabItem();
            ImGui::EndTabBar();
            ImGui::End();
            return;
        }
        if (!canDelete)
            ImGui::EndDisabled();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
}

void ImGuiManager::DrawProjectSpawnContent(Engine* engine)
{
    auto meshNames = MeshManager::Get().GetLoadedMeshNames();
    std::sort(meshNames.begin(), meshNames.end());

    if (meshNames.empty())
    {
        TextLineDisabled("No meshes loaded");
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

        if (ComboFull("Mesh", &meshIdx, meshGetter, &meshNames, (int)meshNames.size()))
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

        if (ComboFull("Mesh Material", &matIdx, MainMaterialComboGetter, &matNames, (int)matNames.size() + 1))
            mSelectedMaterial = (matIdx == 0) ? "" : matNames[matIdx - 1];
    }
    else
    {
        mSelectedMaterial.clear();
    }

    const bool canSpawn = !mSelectedMesh.empty() && m_Callback != nullptr;
    if (!canSpawn)
        ImGui::BeginDisabled();

    if (ImGui::Button("Spawn Selected Mesh", ImVec2(-FLT_MIN, 0)))
    {
        if (m_Callback && !mSelectedMesh.empty())
            m_Callback->buttonClicked(ButtonAction::SpawnSelectedMesh);
    }

    if (!canSpawn)
        ImGui::EndDisabled();

    if (!mSelectedMesh.empty())
    {
        const char* spawnMat = mSelectedMaterial.empty() ? "None (Init/Default)" : mSelectedMaterial.c_str();
        TextLine("Ready: %s / %s", mSelectedMesh.c_str(), spawnMat);
        TextLineDisabled("Mesh spawn only — UI Image material is chosen in the UI panel.");
    }

    ImGui::Separator();
    TextLineDisabled("In-game UI: View → UI (separate material picker for Create UI Image).");

    ImGui::Separator();
    TextLineDisabled("Loaded meshes: %zu", meshNames.size());

    if (ImGui::BeginChild("##MeshList", ImVec2(0, 0), ImGuiChildFlags_Borders, kPanelWindowFlags))
    {
        for (const auto& name : meshNames)
        {
            const bool selected = (name == mSelectedMesh);
            if (SelectableFull(name.c_str(), selected))
                mSelectedMesh = name;
            if (selected)
                ImGui::SetItemDefaultFocus();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", name.c_str());
        }
    }
    ImGui::EndChild();
}

void ImGuiManager::DrawRenderContent(Engine* engine)
{
    if (!engine)
    {
        TextLineDisabled("Engine not available");
        return;
    }

    const GpuDrivenFrameStats& st = engine->GetLastFrameStats();
    const char* pathName = "Basic";
    if (st.path == RenderPath::Instanced) pathName = "Instanced";
    else if (st.path == RenderPath::ComputeIndirect) pathName = "GPU-driven";

    ImGui::SeparatorText("Graphics Style");
    {
        int style = (engine->GetGraphicsStyle() == GraphicsStyle::Toon) ? 1 : 0;
        const char* styles[] = { "Realistic", "Toon" };
        if (ImGui::Combo("Style", &style, styles, 2))
            engine->SetGraphicsStyle(style == 1 ? GraphicsStyle::Toon : GraphicsStyle::Realistic);

        if (engine->GetGraphicsStyle() == GraphicsStyle::Toon)
        {
            float bands = engine->GetToonBands();
            if (SliderFloatFull("Toon Bands", &bands, 1.0f, 8.0f))
                engine->SetToonBands(bands);
            bool outline = engine->IsToonOutlineEnabled();
            if (CheckboxWrapped("Outline", &outline))
                engine->SetToonOutlineEnabled(outline);
            if (outline)
            {
                float ow = engine->GetOutlineWidth();
                if (SliderFloatFull("Outline Width", &ow, 0.005f, 0.08f))
                    engine->SetOutlineWidth(ow);
            }
        }
        else
        {
            float spec = engine->GetSpecularPower();
            if (SliderFloatFull("Specular Power", &spec, 4.0f, 128.0f))
                engine->SetSpecularPower(spec);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Lighting");
    {
        XMFLOAT3 amb = engine->GetAmbientLight();
        float a[3] = { amb.x, amb.y, amb.z };
        if (ImGui::ColorEdit3("Ambient", a))
            engine->SetAmbientLight({ a[0], a[1], a[2] });

        XMFLOAT3 sun = engine->GetSunStrength();
        float s[3] = { sun.x, sun.y, sun.z };
        if (ImGui::ColorEdit3("Sun Color", s))
            engine->SetSunStrength({ s[0], s[1], s[2] });

        XMFLOAT3 dir = engine->GetSunDirection();
        float d[3] = { dir.x, dir.y, dir.z };
        if (ImGui::DragFloat3("Sun Dir", d, 0.01f, -1.0f, 1.0f))
            engine->SetSunDirection({ d[0], d[1], d[2] });
        TextLineDisabled("Sun Dir = light travel direction (down ≈ 0,-1,0)");

        bool shadows = engine->IsShadowsEnabled();
        if (CheckboxWrapped("Shadows", &shadows))
            engine->SetShadowsEnabled(shadows);
        if (shadows)
        {
            float bias = engine->GetShadowBias();
            if (SliderFloatFull("Shadow Bias", &bias, 0.0005f, 0.02f))
                engine->SetShadowBias(bias);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Path");
    TextLine("Path: %s %s", pathName, st.autoPath ? "(Auto)" : "(Manual)");
    TextLine("Frustum Cull: %s", st.cullEnabled ? "ON" : "OFF");
    TextLine("Occlusion(HiZ): %s  valid=%s  mips=%u",
        st.occlusionEnabled ? "ON" : "OFF",
        st.hizValid ? "Y" : "N",
        st.hizMips);

    // 좁은 패널에서 버튼이 잘리지 않도록 자동 줄바꿈
    if (ButtonAutoWrap(st.autoPath ? "Auto Path: ON" : "Auto Path: OFF"))
        engine->SetAutoRenderPathEnabled(!engine->IsAutoRenderPathEnabled());
    if (ButtonAutoWrap("Force Instanced"))
        engine->SetRenderPath(RenderPath::Instanced);
    if (ButtonAutoWrap("Force GPU-driven"))
        engine->SetRenderPath(RenderPath::ComputeIndirect);
    EndButtonAutoWrapRow();

    if (ButtonAutoWrap(st.cullEnabled ? "Disable Frustum Cull" : "Enable Frustum Cull"))
        engine->SetGpuFrustumCullEnabled(!st.cullEnabled);
    if (ButtonAutoWrap(st.occlusionEnabled ? "Disable Occlusion" : "Enable Occlusion"))
        engine->SetGpuOcclusionEnabled(!st.occlusionEnabled);
    if (ButtonAutoWrap(st.gpuMotionEnabled ? "Disable GPU Motion" : "Enable GPU Motion"))
        engine->SetGpuMotionEnabled(!st.gpuMotionEnabled);
    EndButtonAutoWrapRow();

    if (ButtonAutoWrap(st.lodEnabled ? "Disable LOD" : "Enable LOD"))
        engine->SetLodEnabled(!st.lodEnabled);
    if (ButtonAutoWrap(engine->IsLodDistanceCullEnabled()
        ? "Disable DistCull" : "Enable DistCull"))
        engine->SetLodDistanceCullEnabled(!engine->IsLodDistanceCullEnabled());
    EndButtonAutoWrapRow();

    {
        float bias = engine->GetLodBias();
        if (SliderFloatFull("LOD Bias", &bias, 0.25f, 4.f))
            engine->SetLodBias(bias);
        float cullD = engine->GetLodCullDistance();
        if (SliderFloatFull("Cull Distance", &cullD, 20.f, 1000.f))
            engine->SetLodCullDistance(cullD);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Frame Stats");
    TextLine("Sources: %u  Batches: %u  SubDraws: %u",
        st.sourceCount, st.batchCount, st.submeshDraws);
    if (st.path == RenderPath::ComputeIndirect)
    {
        if (st.gpuCullReadbackValid)
        {
            const float keep = (st.gpuSubmittedInstances > 0)
                ? (100.f * static_cast<float>(st.gpuVisibleInstances)
                    / static_cast<float>(st.gpuSubmittedInstances))
                : 0.f;
            TextLine("GPU Cull (delayed): visible=%u  culled=%u  submitted=%u  keep=%.1f%%",
                st.gpuVisibleInstances, st.gpuCulledInstances,
                st.gpuSubmittedInstances, keep);
        }
        else
        {
            TextLine("GPU Cull (delayed): (warming up…)");
        }
    }
    TextLine("EI calls: %u  multi-runs: %u", st.eiCalls, st.multiEiRuns);
    TextLine("Rebuild: %s (%.3f ms)", st.didRebuild ? "Y" : "N", st.rebuildMs);
    TextLine("Patch: %s  dirtyIn=%u patched=%u pend=%u  %.3f ms",
        st.skippedPatch ? "SKIP" : (st.usedDirtyList ? "LIST" : "SCAN"),
        st.dirtyListIn, st.dirtyPatched, st.pendingDirty, st.patchMs);
    TextLine("Upload src=%s meta=%s defaultCopy=%s  %.3f ms",
        st.didSourceUpload ? "Y" : "N",
        st.didMetaUpload ? "Y" : "N",
        st.usedDefaultHeapCopy ? "Y" : "N",
        st.uploadMs);
    TextLine("Compose: %s  %.3f ms", st.didComposeWorld ? "Y" : "N", st.composeMs);
    TextLine("GPU Motion: %s  run=%s  active=%u  %.3f ms",
        st.gpuMotionEnabled ? "ON" : "OFF",
        st.didGpuMotion ? "Y" : "N",
        st.motionActive,
        st.motionMs);
    TextLine("LOD: %s  L0=%u L1=%u L2=%u L3=%u  culled=%u  sw=%u  %.3f ms",
        st.lodEnabled ? "ON" : "OFF",
        st.lodLevelCounts[0], st.lodLevelCounts[1],
        st.lodLevelCounts[2], st.lodLevelCounts[3],
        st.lodCulled, st.lodSwitches, st.lodMs);
    TextLine("HiZ build: %s  %.3f ms", st.didBuildHiZ ? "Y" : "N", st.hizMs);
}

void ImGuiManager::DrawHelpContent()
{
    ImGui::Separator();
    TextLine("Dock / Tabs");
    BulletLine("Bottom strip: Project / UI / Render / Help as dock tabs");
    BulletLine("Left strip: Hierarchy / Tools as dock tabs");
    BulletLine("Click a tab to show only that window");
    BulletLine("Drag a window title onto another panel to dock as a new tab");
    BulletLine("View menu: show/hide or re-open closed panels");
    BulletLine("View > Reset Layout restores the default arrangement");

    ImGui::Spacing();
    ImGui::Separator();
    TextLine("Workflow");
    BulletLine("Hierarchy: select entities (Ctrl multi-toggle)");
    BulletLine("Scene: LMB drag box select, Shift adds");
    BulletLine("Scene: short LMB click enters mouse look");
    BulletLine("Inspector: Transform / Physics / Material");
    BulletLine("Project: pick mesh + material, then Spawn");
    BulletLine("UI panel: create image (center%%), presets, scene preload visible");
    BulletLine("Tools: 3rd-person manipulate mode");
    BulletLine("Render: path, cull, LOD, frame stats");
    BulletLine("Play menu: Play In Editor (F5) / Standalone Game.exe (Ctrl+F5)");
    BulletLine("In-editor play: ESC stops and restores the scene snapshot");

    ImGui::Spacing();
    ImGui::Separator();
    TextLine("Scene View");
    BulletLine("Crosshair = spawn position (view center)");
    BulletLine("RMB pick selects entity (disabled during Manipulate Mesh — RMB drag zooms)");
    BulletLine("ESC releases mouse look (does not quit)");
}

void ImGuiManager::DrawUiPanelContent(Engine* engine)
{
    TextLineDisabled("Edit canvas = Scene dock size. Play canvas = window size.");

    // --- Create ---
    ImGui::SeparatorText("Create");
    {
        // UI-only material (not Project mesh spawn material).
        auto uiMatNames = MaterialManager::Get().GetLoadedMaterialNames();
        std::sort(uiMatNames.begin(), uiMatNames.end());
        if (!uiMatNames.empty())
        {
            int uiMatIdx = 0;
            if (!mSelectedUiMaterial.empty())
            {
                auto it = std::find(uiMatNames.begin(), uiMatNames.end(), mSelectedUiMaterial);
                if (it != uiMatNames.end())
                    uiMatIdx = 1 + (int)std::distance(uiMatNames.begin(), it);
            }

            auto uiMatGetter = [](void* data, int idx) -> const char*
            {
                auto* vec = static_cast<std::vector<std::string>*>(data);
                if (idx == 0) return "(Default)";
                if (!vec || idx < 1 || idx > (int)vec->size()) return nullptr;
                return (*vec)[idx - 1].c_str();
            };
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("UI Material##ui_create_mat", &uiMatIdx, uiMatGetter,
                    &uiMatNames, (int)uiMatNames.size() + 1))
            {
                mSelectedUiMaterial = (uiMatIdx == 0) ? "" : uiMatNames[uiMatIdx - 1];
            }
        }
        else
        {
            mSelectedUiMaterial.clear();
            TextLineDisabled("No materials loaded.");
        }

        const char* uiMat = mSelectedUiMaterial.empty() ? "Default" : mSelectedUiMaterial.c_str();
        TextLine("Create with: %s", uiMat);

        // Default scale mode for newly created UI (per-object can override in Inspector).
        if (engine)
        {
            int scaleMode = static_cast<int>(engine->GetUiScaleMode());
            const char* scaleItems[] = {
                "Stretch (fill, may squash)",
                "Keep Aspect Fit",
                "Keep Aspect Fill"
            };
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("Scale Mode##ui_create_scale", &scaleMode, scaleItems, IM_ARRAYSIZE(scaleItems)))
                engine->SetUiScaleMode(static_cast<UiScaleMode>(scaleMode));
            TextLineDisabled("Applied to new UI only. Change existing in Inspector → UI.");
        }

        CheckboxWrapped("Fit image aspect ratio", &mUiCreateFitImageAspect);
        if (mUiCreateFitImageAspect)
        {
            float aspect = 1.f;
            if (TryGetMaterialTextureAspect(mSelectedUiMaterial, aspect))
                TextLineDisabled("Drag box is fitted to texture aspect (%.3f).", aspect);
            else
                TextLineDisabled("Texture size unknown — free rect (aspect unlock).");
        }
        else
        {
            TextLineDisabled("Off: free drag size. On: keep texture W:H inside drag.");
        }

        ImGui::PushID("ui_image_create");
        if (mUiImageCreateMode)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.95f, 0.65f, 1.0f));
            TextLine("UI Create mode ON");
            ImGui::PopStyleColor();
            if (mUiCreateHasRect)
                TextLine("Rect: %.0f x %.0f px  (re-drag Scene to change)",
                    mUiCreateMaxX - mUiCreateMinX, mUiCreateMaxY - mUiCreateMinY);
            else
                TextLineDisabled("Drag a rectangle on the Scene panel.");

            const bool exitOk = IsUiCreateExitOk();
            const bool canComplete = exitOk && mUiCreateHasRect;

            if (!canComplete)
                ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.42f, 0.28f, 1.0f));
            ImGui::Button("Complete##ui_create_ok", ImVec2(-FLT_MIN, 0));
            if (canComplete && ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                mUiCreatePending = true;
                SetUiImageCreateMode(false);
                OutputDebugStringA("[UI] Complete pressed\n");
            }
            ImGui::PopStyleColor();
            if (!canComplete)
                ImGui::EndDisabled();

            if (!exitOk)
                ImGui::BeginDisabled();
            ImGui::Button("Cancel##ui_create_cancel", ImVec2(-FLT_MIN, 0));
            if (exitOk && ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                SetUiImageCreateMode(false);
                OutputDebugStringA("[UI] Cancel pressed\n");
            }
            if (!exitOk)
                ImGui::EndDisabled();

            if (!exitOk)
                TextLineDisabled("Wait a moment… then Complete / Cancel / Esc.");
            else
                TextLineDisabled("Mode ends only via Complete / Cancel / Esc.");
        }
        else
        {
            if (ImGui::Button("Create UI Image##ui_create_start", ImVec2(-FLT_MIN, 0)))
            {
                SetUiImageCreateMode(true);
                if (m_Callback)
                    m_Callback->buttonClicked(ButtonAction::BeginCreateUiImage);
            }
            if (mUiCreateEnablePending)
                TextLineDisabled("Starting UI Create mode…");
            else
                TextLineDisabled("1) Material / Scale / Aspect  2) Create  3) Drag  4) Complete");
        }
        ImGui::PopID();
    }

    // --- Presets ---
    ImGui::SeparatorText("Presets");
    if (!engine)
    {
        TextLineDisabled("Engine unavailable.");
        return;
    }

    const size_t liveUi = engine->GetUiEntities().size();
    TextLineDisabled("Save layout → scene preload → Spawn / export.");

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##ui_preset_name", mUiPresetNameBuf, sizeof(mUiPresetNameBuf));
    TextLineDisabled("Preset name (file: UiPresets/<name>.uipreset)");

    const bool canSave = liveUi > 0 && mUiPresetNameBuf[0] != '\0';
    if (!canSave)
        ImGui::BeginDisabled();
    if (ImGui::Button("Save Current UI as Preset", ImVec2(-FLT_MIN, 0)))
    {
        std::string err;
        if (engine->SaveCurrentUiAsPreset(mUiPresetNameBuf, &err))
        {
            // File + registry only — do not auto-add to scene or spawn UI
            mUiPresetLastMsg = std::string("Saved preset file: ") + mUiPresetNameBuf
                + " (use Add to Scene Preload to attach)";
        }
        else
            mUiPresetLastMsg = err.empty() ? "Save failed" : err;
    }
    if (!canSave)
        ImGui::EndDisabled();

    auto diskPresets = UiPresetSerializer::ListPresetNamesInDefaultDir();
    if (!diskPresets.empty())
    {
        if (mUiPresetDiskIndex >= (int)diskPresets.size())
            mUiPresetDiskIndex = 0;
        auto getter = [](void* data, int idx) -> const char*
        {
            auto* v = static_cast<std::vector<std::string>*>(data);
            if (idx < 0 || idx >= (int)v->size()) return nullptr;
            return (*v)[idx].c_str();
        };
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo("##ui_preset_disk", &mUiPresetDiskIndex, getter, &diskPresets, (int)diskPresets.size());

        if (ImGui::Button("Add to Scene Preload", ImVec2(-FLT_MIN, 0)))
        {
            const std::string& n = diskPresets[mUiPresetDiskIndex];
            engine->RegisterUiPresetFromFile(n, nullptr);
            // List only — press Visible when you want it shown (no auto-spawn)
            engine->AddSceneUiPreset(n, false);
            mUiPresetLastMsg = "Added to scene preload (hidden). Press Visible to show.";
        }
        if (ImGui::Button("Spawn Preset (test)", ImVec2(-FLT_MIN, 0)))
        {
            const std::string& n = diskPresets[mUiPresetDiskIndex];
            const uint32_t id = engine->SpawnUiPreset(n, true);
            if (id != 0)
                mUiPresetLastMsg = "Spawned instance " + std::to_string(id) + " (" + n + ")";
            else
                mUiPresetLastMsg = "Spawn failed: " + n;
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Delete Preset File", ImVec2(-FLT_MIN, 0)))
        {
            const std::string& n = diskPresets[mUiPresetDiskIndex];
            std::string err;
            if (engine->DeleteUiPreset(n, &err))
                mUiPresetLastMsg = "Deleted preset: " + n;
            else
                mUiPresetLastMsg = err.empty() ? "Delete failed" : err;
        }
        ImGui::PopStyleColor();
    }
    else
    {
        TextLineDisabled("No .uipreset files in UiPresets/ yet.");
    }

    // --- Delete scene files ---
    ImGui::SeparatorText("Scenes on Disk");
    {
        auto scenes = SceneSerializer::ListSceneNamesInDefaultDir();
        static int sceneDiskIndex = 0;
        if (!scenes.empty())
        {
            if (sceneDiskIndex >= (int)scenes.size())
                sceneDiskIndex = 0;
            auto getter = [](void* data, int idx) -> const char*
            {
                auto* v = static_cast<std::vector<std::string>*>(data);
                if (idx < 0 || idx >= (int)v->size()) return nullptr;
                return (*v)[idx].c_str();
            };
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::Combo("##scene_disk", &sceneDiskIndex, getter, &scenes, (int)scenes.size());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button("Delete Scene File", ImVec2(-FLT_MIN, 0)))
            {
                const std::string& n = scenes[sceneDiskIndex];
                if (SceneSerializer::DeleteSceneFile(n))
                    mUiPresetLastMsg = "Deleted scene: " + n + ".scene";
                else
                    mUiPresetLastMsg = "Failed to delete scene: " + n;
            }
            ImGui::PopStyleColor();
            TextLineDisabled("Deletes Scenes/<name>.scene (cannot undo).");
        }
        else
        {
            TextLineDisabled("No .scene files in Scenes/ yet.");
        }
    }

    ImGui::Spacing();
    TextLine("Scene preload (%zu):", engine->GetSceneUiPresetList().size());
    TextLineDisabled("Visible/Invisible is saved with the scene.");
    const std::vector<SceneUiPresetEntry> sceneList = engine->GetSceneUiPresetList();
    for (size_t i = 0; i < sceneList.size(); ++i)
    {
        const auto& entry = sceneList[i];
        ImGui::PushID(static_cast<int>(i) + 9000);

        const bool isVis = entry.visible;
        ImGui::PushStyleColor(ImGuiCol_Text, isVis
            ? ImVec4(0.45f, 0.95f, 0.55f, 1.0f)
            : ImVec4(0.65f, 0.65f, 0.65f, 1.0f));
        TextLine("%s  %s%s",
            entry.name.c_str(),
            isVis ? "[visible]" : "[hidden]",
            engine->HasUiPreset(entry.name) ? "" : "  (missing)");
        ImGui::PopStyleColor();

        if (isVis)
            ImGui::BeginDisabled();
        if (ImGui::SmallButton("Visible"))
            engine->SetSceneUiPresetVisible(entry.name, true);
        if (isVis)
            ImGui::EndDisabled();

        ImGui::SameLine();
        if (!isVis)
            ImGui::BeginDisabled();
        if (ImGui::SmallButton("Invisible"))
            engine->SetSceneUiPresetVisible(entry.name, false);
        if (!isVis)
            ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
            engine->RemoveSceneUiPreset(entry.name);

        ImGui::PopID();
    }

    TextLine("Registered: %zu  |  live instances: %zu",
        engine->GetRegisteredUiPresetNames().size(),
        engine->CountUiPresetInstances());

    if (ImGui::Button("Destroy All Preset Instances", ImVec2(-FLT_MIN, 0)))
    {
        engine->DestroyAllUiPresetInstances();
        mUiPresetLastMsg = "Destroyed all preset instances";
    }
    if (ImGui::Button("Clear All Live UI", ImVec2(-FLT_MIN, 0)))
    {
        engine->DestroyAllUiEntities();
        mUiPresetLastMsg = "Cleared all live UI";
    }

    if (!mUiPresetLastMsg.empty())
        TextLineDisabled("%s", mUiPresetLastMsg.c_str());
}

void ImGuiManager::DrawUiPanel(Engine* engine)
{
    if (!ImGui::Begin("UI", &mShowUi, kPanelWindowFlags))
    {
        ImGui::End();
        return;
    }
    DrawUiPanelContent(engine);
    ImGui::End();
}

void ImGuiManager::DrawProjectPanel(Engine* engine)
{
    if (!ImGui::Begin("Project", &mShowProject, kPanelWindowFlags))
    {
        ImGui::End();
        return;
    }
    DrawProjectSpawnContent(engine);
    ImGui::End();
}

void ImGuiManager::DrawRenderPanel(Engine* engine)
{
    if (!ImGui::Begin("Render", &mShowRender, kPanelWindowFlags))
    {
        ImGui::End();
        return;
    }
    DrawRenderContent(engine);
    ImGui::End();
}

void ImGuiManager::DrawHelpPanel()
{
    if (!ImGui::Begin("Help", &mShowHelp, kPanelWindowFlags))
    {
        ImGui::End();
        return;
    }
    DrawHelpContent();
    ImGui::End();
}
#pragma endregion 

void ImGuiManager::Shutdown()
{
    // 종료 직전 UI 설정 + ImGui 도크 레이아웃 저장
    if (ImGui::GetCurrentContext() != nullptr)
    {
        if (ImGuiDockNode* root = ImGui::DockBuilderGetNode(ImGui::GetID("EditorDockSpace")))
            CaptureDockSplitRatios(root);
        SaveUiSettings();
        if (mImGuiIniPath[0] != '\0')
            ImGui::SaveIniSettingsToDisk(mImGuiIniPath);
    }

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
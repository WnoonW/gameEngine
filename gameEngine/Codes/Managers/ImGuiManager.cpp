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
    DescriptorAllocator& globalDescriptorAllocator,
    IFunctionCallback* callback)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    LoadUIFonts(io);

    static DescriptorAllocator* s_DescriptorAllocator = nullptr;
    s_DescriptorAllocator = &globalDescriptorAllocator;

    m_Callback = callback;

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

void ImGuiManager::CustomUI(Engine* engine)
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
        ImGui::Text("Object manipulation mode ON (like camera)");
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
}

void ImGuiManager::Shutdown()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
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
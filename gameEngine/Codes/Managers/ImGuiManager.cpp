#include "ImGuiManager.h"
#include "MeshManager.h"
#include "MaterialManager.h"
#include <algorithm>
#include <vector>
#include <string>

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

void ImGuiManager::CustomUI(const MeshInstanceStats& meshStats, const CullingStats& cullStats)
{
    ImGui::Begin("V3.0-UI Debug Window");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

    ImGui::Separator();
    ImGui::Text("Mesh Instances");
    if (meshStats.countByMesh.empty())
    {
        ImGui::Text("  (none)");
    }
    else
    {
        std::vector<std::pair<std::string, int>> meshCounts(meshStats.countByMesh.begin(), meshStats.countByMesh.end());
        std::sort(meshCounts.begin(), meshCounts.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& [meshName, count] : meshCounts)
            ImGui::Text("  %s: %d", meshName.c_str(), count);
    }
    ImGui::Text("Total: %d", meshStats.totalInstances);

    ImGui::Separator();
    ImGui::Text("Capacity");
    ImGui::Text("  Object CB: %u / %u", meshStats.objectCBUsed, meshStats.objectCBCapacity);
    ImGui::Text("  Draw cmds: %u / %u", meshStats.estimatedDrawCommands, meshStats.drawCommandCapacity);
    ImGui::Text("  Descriptors: %u / %u", meshStats.descriptorsUsed, meshStats.descriptorCapacity);

    const bool nearObjectLimit = meshStats.objectCBUsed >= meshStats.objectCBCapacity * 9 / 10;
    const bool nearDrawLimit = meshStats.estimatedDrawCommands >= meshStats.drawCommandCapacity * 9 / 10;
    const bool nearDescriptorLimit = meshStats.descriptorsUsed >= meshStats.descriptorCapacity * 9 / 10;
    if (nearObjectLimit || nearDrawLimit || nearDescriptorLimit)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Warning: nearing capacity limit");

    if (!meshStats.lastCreateError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Last spawn error: %s", meshStats.lastCreateError.c_str());

    ImGui::Separator();
    ImGui::Text("Culling");
    ImGui::Text("  Candidates (pre-cull): %u", cullStats.totalCandidates);
    ImGui::Text("  Frustum culled: %u", cullStats.frustumCulled);
    ImGui::Text("  After frustum: %u", cullStats.passedFrustum);
    ImGui::Text("  Sent to occlusion compute: %u", cullStats.occlusionTested);
    ImGui::Text("  Occlusion culled: %u  (shader stub)", cullStats.occlusionCulled);
    ImGui::Text("  Final indirect commands: %u", cullStats.finalDrawCommands);

    if (cullStats.totalCandidates > 0)
    {
        float frustumRate = (cullStats.frustumCulled * 100.0f) / cullStats.totalCandidates;
        ImGui::Text("  Frustum cull %%: %.1f", frustumRate);
    }

    ImGui::Separator();
    if (ImGui::Button("Reset Scene"))
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::ResetScene);

    if (ImGui::Button("Spawn Test Object"))
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::SpawnTestObject);

    if (ImGui::Button("Toggle Wireframe"))
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::ToggleWireframe);

    ImGui::Separator();
    ImGui::Text("Resolution Presets");
    if (ImGui::Button("600 x 800"))   if (m_Callback) m_Callback->buttonClicked(ButtonAction::SetRes_600x800);
    ImGui::SameLine();
    if (ImGui::Button("1920 x 1080")) if (m_Callback) m_Callback->buttonClicked(ButtonAction::SetRes_1920x1080);
    ImGui::SameLine();
    if (ImGui::Button("1200 x 800"))  if (m_Callback) m_Callback->buttonClicked(ButtonAction::SetRes_1200x800);

    ImGui::TextDisabled("(Resizes the window)");

    ImGui::End();
}

void ImGuiManager::DrawObjectSelector()
{
    auto meshNames = MeshManager::Get().GetAllMeshNames();
    auto matNames = MaterialManager::Get().GetAllMaterialNames();

    // 초기 선택 설정 (첫 호출 시)
    if (m_SelectedMesh.empty() && !meshNames.empty())
    {
        m_SelectedMesh = meshNames[0];
    }
    if (m_SelectedMaterial.empty() && !matNames.empty())
    {
        // "Test" 우선, 없으면 첫번째
        auto it = std::find(matNames.begin(), matNames.end(), "Test");
        m_SelectedMaterial = (it != matNames.end()) ? "Test" : matNames[0];
    }

    ImGui::Begin("Object Selector");

    ImGui::Text("Loaded Meshes");
    if (meshNames.empty())
    {
        ImGui::TextDisabled("(no meshes loaded)");
    }
    else
    {
        if (ImGui::BeginListBox("##MeshList", ImVec2(-FLT_MIN, 5 * ImGui::GetTextLineHeightWithSpacing())))
        {
            for (const auto& name : meshNames)
            {
                bool isSelected = (name == m_SelectedMesh);
                if (ImGui::Selectable(name.c_str(), isSelected))
                {
                    m_SelectedMesh = name;
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndListBox();
        }
    }

    ImGui::Separator();

    ImGui::Text("Material");
    if (matNames.empty())
    {
        ImGui::TextDisabled("(no materials)");
    }
    else
    {
        // Combo for material
        int currentMatIdx = 0;
        for (size_t i = 0; i < matNames.size(); ++i)
        {
            if (matNames[i] == m_SelectedMaterial)
            {
                currentMatIdx = static_cast<int>(i);
                break;
            }
        }

        if (ImGui::BeginCombo("##MatCombo", m_SelectedMaterial.c_str()))
        {
            for (int i = 0; i < static_cast<int>(matNames.size()); ++i)
            {
                bool isSel = (i == currentMatIdx);
                if (ImGui::Selectable(matNames[i].c_str(), isSel))
                {
                    m_SelectedMaterial = matNames[i];
                }
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    bool canSpawn = !m_SelectedMesh.empty() && !m_SelectedMaterial.empty();
    if (!canSpawn)
        ImGui::BeginDisabled();

    if (ImGui::Button("Spawn Selected", ImVec2(-FLT_MIN, 0)))
    {
        if (m_Callback)
        {
            m_Callback->RequestObjectSpawn(m_SelectedMesh, m_SelectedMaterial);
        }
    }

    if (!canSpawn)
        ImGui::EndDisabled();

    ImGui::TextDisabled("Spawns at origin (0,0,0). Use + key for spiral spawn.");

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
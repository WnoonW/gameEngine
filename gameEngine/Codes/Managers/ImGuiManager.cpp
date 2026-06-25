#include "ImGuiManager.h"
#include <algorithm>
#include <vector>

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
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

void ImGuiManager::CustomUI(const MeshInstanceStats& stats)
{
    ImGui::Begin("V3.0-UI Debug Window");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

    ImGui::Separator();
    ImGui::Text("Mesh Instances");
    if (stats.countByMesh.empty())
    {
        ImGui::Text("  (none)");
    }
    else
    {
        std::vector<std::pair<std::string, int>> meshCounts(stats.countByMesh.begin(), stats.countByMesh.end());
        std::sort(meshCounts.begin(), meshCounts.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& [meshName, count] : meshCounts)
            ImGui::Text("  %s: %d", meshName.c_str(), count);
    }
    ImGui::Text("Total: %d", stats.totalInstances);

    ImGui::Separator();
    ImGui::Text("Capacity");
    ImGui::Text("  Object CB: %u / %u", stats.objectCBUsed, stats.objectCBCapacity);
    ImGui::Text("  Draw cmds: %u / %u", stats.estimatedDrawCommands, stats.drawCommandCapacity);
    ImGui::Text("  Descriptors: %u / %u", stats.descriptorsUsed, stats.descriptorCapacity);

    const bool nearObjectLimit = stats.objectCBUsed >= stats.objectCBCapacity * 9 / 10;
    const bool nearDrawLimit = stats.estimatedDrawCommands >= stats.drawCommandCapacity * 9 / 10;
    const bool nearDescriptorLimit = stats.descriptorsUsed >= stats.descriptorCapacity * 9 / 10;
    if (nearObjectLimit || nearDrawLimit || nearDescriptorLimit)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Warning: nearing capacity limit");

    if (!stats.lastCreateError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Last spawn error: %s", stats.lastCreateError.c_str());

    ImGui::Separator();
    if (ImGui::Button("Reset Scene"))
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::ResetScene);

    if (ImGui::Button("Spawn Test Object"))
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::SpawnTestObject);

    if (ImGui::Button("Toggle Wireframe"))
        if (m_Callback) m_Callback->buttonClicked(ButtonAction::ToggleWireframe);

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
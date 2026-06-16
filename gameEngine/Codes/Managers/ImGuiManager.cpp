#include "ImGuiManager.h"

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

void ImGuiManager::SetDebugStatsProvider(std::function<EngineDebugStats()> provider)
{
    m_DebugStatsProvider = std::move(provider);
}

void ImGuiManager::CustomUI()
{
    ImGui::Begin("V3.0-UI Debug Window");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

    if (m_DebugStatsProvider)
    {
        EngineDebugStats stats = m_DebugStatsProvider();
        const RenderStats& renderStats = stats.renderStats;

        ImGui::Separator();
        ImGui::Text("Renderable Entities: %d", renderStats.totalRenderableEntities);
        ImGui::Text("Draw Batches: %d", renderStats.drawBatches);
        ImGui::Text("Peak Instances / Batch: %d / %d",
            renderStats.peakInstancesPerBatch,
            renderStats.instanceBufferCapacity);

        ImGui::Separator();
        ImGui::Text("Object Counts");
        if (stats.objectCounts.empty())
        {
            ImGui::TextUnformatted("(none)");
        }
        else
        {
            if (ImGui::BeginTable("ObjectCounts", 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Mesh");
                ImGui::TableSetupColumn("Material");
                ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                ImGui::TableHeadersRow();

                for (const ObjectCountInfo& info : stats.objectCounts)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(info.meshName.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(info.materialName.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", info.count);
                }

                ImGui::EndTable();
            }
        }
        ImGui::Separator();
    }

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
#include "ImGuiManager.h"
#include "Managers/MeshManager.h"
#include "Managers/MaterialManager.h"
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

void ImGuiManager::CustomUI()
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
    if (!matNames.empty()) {
        if (std::find(matNames.begin(), matNames.end(), mSelectedMaterial) == matNames.end() && !matNames.empty()) {
            mSelectedMaterial = matNames[0];
        }

        int matIdx = 0;
        for (size_t i = 0; i < matNames.size(); ++i) {
            if (matNames[i] == mSelectedMaterial) {
                matIdx = (int)i;
                break;
            }
        }

        auto matGetter = [](void* data, int idx) -> const char* {
            auto* vec = (std::vector<std::string>*)data;
            if (idx < 0 || idx >= (int)vec->size()) return nullptr;
            return (*vec)[idx].c_str();
        };
        if (ImGui::Combo("Material", &matIdx, matGetter, &matNames, (int)matNames.size())) {
            mSelectedMaterial = matNames[matIdx];
        }
    } else {
        mSelectedMaterial = "Default";
    }

    if (ImGui::Button("Spawn Selected Mesh")) {
        if (m_Callback && !mSelectedMesh.empty()) {
            m_Callback->buttonClicked(ButtonAction::SpawnSelectedMesh);
        }
    }

    if (!mSelectedMesh.empty()) {
        ImGui::Text("Selected: %s / %s", mSelectedMesh.c_str(), mSelectedMaterial.c_str());
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
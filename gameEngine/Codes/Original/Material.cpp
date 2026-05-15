#include "Material.h"
#include "../Common/d3dx12.h"
#include <d3dcompiler.h>
#include <iostream>
#include <format>

Material1::Material1() = default;
Material1::~Material1() = default;

bool Material1::CreateMaterial(ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    DataLoader& loader,
    const std::wstring& vsPath,
    const std::wstring& psPath,
    const std::vector<std::wstring>& texturePaths)
{
    if (!device || !commandQueue) return false;

    ComPtr<ID3DBlob> vsBlob, psBlob;
    HRESULT hr = D3DCompileFromFile(vsPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    if (FAILED(hr)) return false;

    hr = D3DCompileFromFile(psPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main", "ps_5_0", 0, 0, &psBlob, nullptr);
    if (FAILED(hr)) return false;

    if (!CreateRootSignature(device)) return false;
    if (!CreatePSO(device, vsBlob.Get(), psBlob.Get())) return false;

    // 텍스처 로드
    for (const auto& path : texturePaths) {
        ComPtr<ID3D12Resource> tex, upload;
        if (loader.LoadTexture(device, commandQueue, path, tex, upload)) {
            mTextures.push_back(tex);
        }
    }

    // Descriptor Heap 생성
    if (!mTextures.empty()) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = static_cast<UINT>(mTextures.size());
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap));
        if (FAILED(hr)) return false;
    }

    auto msg = std::format(L"[Material] 생성 완료 - 텍스처 수: {}", mTextures.size());
    OutputDebugStringW(msg.c_str());
    return true;
}

bool Material1::CreateRootSignature(ID3D12Device* device)
{
    D3D12_ROOT_PARAMETER rootParams[2] = {};

    // CBV (b0)
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // SRV Descriptor Table (t0)
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &range;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 0;
    rootSigDesc.pStaticSamplers = nullptr;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signatureBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {

            std::string msg = std::format("[Material] 생성 실패 - 에러: {}",
                (const char*)errorBlob->GetBufferPointer());

            OutputDebugStringA(msg.c_str());
        }
        return false;
    }

    hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
        signatureBlob->GetBufferSize(), IID_PPV_ARGS(&mRootSignature));
    return SUCCEEDED(hr);
}

bool Material1::CreatePSO(ID3D12Device* device, ID3DBlob* vsBlob, ID3DBlob* psBlob)
{
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleDesc.Count = 1;

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO));
    return SUCCEEDED(hr);
}
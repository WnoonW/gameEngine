#include <stdexcept>
#include <sstream>
#include "d3dx12.h"
#include "PipelineStateManager.h"
#include "ShaderManager.h"

bool PSOKey::operator==(const PSOKey& other) const
{
    return shaderName == other.shaderName &&
        memcmp(&blendDesc, &other.blendDesc, sizeof(D3D12_BLEND_DESC)) == 0 &&
        memcmp(&rasterizerDesc, &other.rasterizerDesc, sizeof(D3D12_RASTERIZER_DESC)) == 0 &&
        memcmp(&depthStencilDesc, &other.depthStencilDesc, sizeof(D3D12_DEPTH_STENCIL_DESC)) == 0 &&
        topologyType == other.topologyType;
}

size_t std::hash<PSOKey>::operator()(const PSOKey& key) const
{
    size_t hashValue = std::hash<std::string>{}(key.shaderName);

    // 간단한 해시 조합 (필요하면 더 정교하게 개선 가능)
    hashValue ^= std::hash<uint32_t>{}(key.blendDesc.RenderTarget[0].BlendEnable) << 1;
    hashValue ^= std::hash<uint32_t>{}(key.rasterizerDesc.CullMode) << 2;
    hashValue ^= std::hash<uint32_t>{}(key.depthStencilDesc.DepthEnable) << 3;

    return hashValue;
}

PipelineStateManager& PipelineStateManager::Get()
{
    static PipelineStateManager instance;
    return instance;
}

void PipelineStateManager::Initialize(ID3D12Device* device)
{
    mDevice = device;
}

void PipelineStateManager::Shutdown()
{
    mPSOCache.clear();
    mDevice = nullptr;
}

ID3D12PipelineState* PipelineStateManager::GetOrCreatePSO(
    const PSOKey& key,
    ID3D12RootSignature* rootSignature,
    ID3D12Device* device)
{
    if (device == nullptr)
        device = mDevice;

    auto it = mPSOCache.find(key);
    if (it != mPSOCache.end())
        return it->second.Get();

    // ==================== PSO 생성 ====================
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature;
    psoDesc.BlendState = key.blendDesc;
    psoDesc.RasterizerState = key.rasterizerDesc;
    psoDesc.DepthStencilState = key.depthStencilDesc;
    psoDesc.PrimitiveTopologyType = key.topologyType;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    psoDesc.SampleMask = UINT_MAX;

    // 셰이더 경로
    std::wstring wShaderName(key.shaderName.begin(), key.shaderName.end());
    std::wstring shaderPath = L"Resources\\Shaders\\" + wShaderName + L".hlsl";

    auto vsBlob = ShaderManager::Get().GetVertexShader(shaderPath);
    auto psBlob = ShaderManager::Get().GetPixelShader(shaderPath);

    // ==================== 중요: null 체크 ====================
    if (!vsBlob || !psBlob)
    {
        OutputDebugStringA(("Shader Compile Failed: " + key.shaderName + "\n").c_str());
        return nullptr;   // 또는 throw
    }

    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

    // Input Layout
    static const D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    psoDesc.InputLayout.pInputElementDescs = inputElementDescs;
    psoDesc.InputLayout.NumElements = _countof(inputElementDescs);

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));

    if (FAILED(hr))
    {
        OutputDebugStringA(("CreateGraphicsPipelineState Failed: " + key.shaderName + "\n").c_str());
        return nullptr;
    }

    mPSOCache[key] = pso;
    return pso.Get();
}
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
    InitializeCommandSignatures(device);
    InitializeComputePipeline(device);
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
    if (key.isComputeShader)
    {
        return GetOrCreateComputePSO(key, rootSignature, device);
    }

    if (!device) device = mDevice;

    // === Graphics PSO 캐싱 (PSOKey 전체를 키로 사용) ===
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

ID3D12PipelineState* PipelineStateManager::GetOrCreateComputePSO(
    const PSOKey& key,
    ID3D12RootSignature* rootSignature,
    ID3D12Device* device)
{
    if (!device) device = mDevice;

    std::string cacheKey = key.shaderName;

    // 캐시에 이미 있으면 반환
    auto it = mComputePSOCache.find(cacheKey);
    if (it != mComputePSOCache.end())
        return it->second.Get();

    // === 쉐이더 경로 생성 (Resources\Shaders\ 추가) ===
    std::wstring wShaderName(key.shaderName.begin(), key.shaderName.end());
    std::wstring shaderPath = L"Resources\\Shaders\\" + wShaderName + L".hlsl";

    // Compute Shader 컴파일
    auto computeBlob = ShaderManager::Get().GetComputeShader(shaderPath, "main");

    if (!computeBlob)
    {
        OutputDebugStringA("[PipelineStateManager] Compute Shader Compile Failed!\n");
        return nullptr;
    }

    // Compute PSO 생성
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = rootSignature;
    desc.CS = {
        computeBlob->GetBufferPointer(),
        computeBlob->GetBufferSize()
    };

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));

    if (FAILED(hr))
    {
        OutputDebugStringA("[PipelineStateManager] CreateComputePipelineState Failed!\n");
        return nullptr;
    }

    mComputePSOCache[cacheKey] = pso;
    return pso.Get();
}

void PipelineStateManager::InitializeCommandSignatures(ID3D12Device* device)
{
    D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
    argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs = &argDesc;

    ThrowIfFailed(device->CreateCommandSignature(&sigDesc, nullptr,
        IID_PPV_ARGS(&mDrawIndexedIndirectSignature)));
}

ID3D12CommandSignature* PipelineStateManager::GetDrawIndexedIndirectSignature() const
{
    return mDrawIndexedIndirectSignature.Get();
}

void PipelineStateManager::InitializeComputePipeline(ID3D12Device* device)
{
    if (!device) device = mDevice;

    // === Compute Root Signature 생성 (한 번만) ===
    if (!mComputeRootSignature)
    {
        CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // u0
        CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0

        CD3DX12_ROOT_PARAMETER1 rootParams[3];

        rootParams[0].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL); // u0 - IndirectArgsUAV
        rootParams[1].InitAsConstants(1, 0, 0, D3D12_SHADER_VISIBILITY_ALL);             // b0 - MaxInstanceCount
        rootParams[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL); // t0 - DrawCommand SRV

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
        computeRootSignatureDesc.Init_1_1(_countof(rootParams), rootParams, 0, nullptr);

        Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

        HRESULT hr = D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc,
            D3D_ROOT_SIGNATURE_VERSION_1_1, &signatureBlob, &errorBlob);

        if (FAILED(hr))
        {
            if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
            assert(false && "Compute Root Signature Serialize Failed");
        }

        device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(), IID_PPV_ARGS(&mComputeRootSignature));
    }
}

ID3D12RootSignature* PipelineStateManager::GetComputeRootSignature() const
{
    return mComputeRootSignature.Get();
}

ID3D12PipelineState* PipelineStateManager::GetFrustumCullingPSO() const
{
    return mFrustumCullingPSO.Get();
}
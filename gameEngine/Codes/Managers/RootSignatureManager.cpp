#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "d3dx12.h"
#include "constantStruct.h"
#include <stdexcept>

RootSignatureManager& RootSignatureManager::Get()
{
    static RootSignatureManager instance;
    return instance;
}

void RootSignatureManager::Initialize(ID3D12Device* device)
{
    if (mDevice != nullptr)
        return; // 이미 초기화됨

    mDevice = device;

    CreateSceneRootSignature();
}

void RootSignatureManager::Shutdown()
{
    mRootSignatures.clear();
    mCommandSignature.Reset();
    mDevice = nullptr;
}

ID3D12RootSignature* RootSignatureManager::GetRootSignature(RootSignatureType type)
{
    auto it = mRootSignatures.find(type);
    if (it != mRootSignatures.end())
    {
        return it->second.Get();
    }

    throw std::runtime_error("RootSignature not found. Did you call Initialize()?");
    return nullptr;
}

// =====================================================
// Scene용 Root Signature 생성
// =====================================================
void RootSignatureManager::CreateSceneRootSignature()
{
    mCommandSignature.Reset();

    CD3DX12_DESCRIPTOR_RANGE cbvRange(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    // [0] CBV (b0) - ObjectConstants
    // [1] SRV (t0) - per-draw texture
    CD3DX12_ROOT_PARAMETER slotRootParameter[2];
    slotRootParameter[0].InitAsDescriptorTable(1, &cbvRange, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    // === 3. Static Sampler ===
    CD3DX12_STATIC_SAMPLER_DESC samplerDesc(
        0,                                      // shaderRegister (s0)
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    // === 4. Root Signature Desc ===
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        2, slotRootParameter,
        1, &samplerDesc,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    // === 5. Serialize & Create (기존과 동일) ===
    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;

    HRESULT hr = D3D12SerializeRootSignature(
        &rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(),
        errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
        OutputDebugStringA((char*)errorBlob->GetBufferPointer());

    ThrowIfFailed(hr);

    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(mDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&rootSig)));

    mRootSignatures[RootSignatureType::Scene] = rootSig;
    PipelineStateManager::Get().InvalidateCache();
}


// =====================================================
// Command Signature 생성
// =====================================================
ComPtr<ID3D12CommandSignature> RootSignatureManager::GetOrCreateCommandSignature()
{
    if (mCommandSignature != nullptr)
        return mCommandSignature;

    D3D12_INDIRECT_ARGUMENT_DESC argumentDescs[1] = {};
    argumentDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC cmdDesc = {};
    cmdDesc.NumArgumentDescs = 1;
    cmdDesc.pArgumentDescs = argumentDescs;
    cmdDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    cmdDesc.NodeMask = 0;

    // DRAW_INDEXED만 사용하므로 root argument 변경 없음 → rootSignature는 nullptr
    ThrowIfFailed(mDevice->CreateCommandSignature(
        &cmdDesc,
        nullptr,
        IID_PPV_ARGS(&mCommandSignature)));

    return mCommandSignature;
}
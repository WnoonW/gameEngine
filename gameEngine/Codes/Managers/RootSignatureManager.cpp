#include "RootSignatureManager.h"
#include "d3dx12.h"
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
    D3D12_ROOT_PARAMETER slotRootParameter[2] = {};

    // b0 : Constant Buffer (CBV)
    D3D12_DESCRIPTOR_RANGE cbvRange = {};
    cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbvRange.NumDescriptors = 1;
    cbvRange.BaseShaderRegister = 0;
    cbvRange.RegisterSpace = 0;
    cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    slotRootParameter[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    slotRootParameter[0].DescriptorTable.NumDescriptorRanges = 1;
    slotRootParameter[0].DescriptorTable.pDescriptorRanges = &cbvRange;
    slotRootParameter[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // t0 : Texture (SRV)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    slotRootParameter[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    slotRootParameter[1].DescriptorTable.NumDescriptorRanges = 1;
    slotRootParameter[1].DescriptorTable.pDescriptorRanges = &srvRange;
    slotRootParameter[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_STATIC_SAMPLER_DESC samplerDesc(
        0,                                      // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        2, slotRootParameter,
        1, &samplerDesc,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

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
}
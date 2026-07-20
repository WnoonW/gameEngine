#include "RootSignatureManager.h"
#include "../Structs/IndirectDrawStructs.h"
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
        return;

    mDevice = device;

    CreateSceneRootSignature();
    CreateIndirectBuildRootSignature();
    CreateSceneCommandSignature();
}

void RootSignatureManager::Shutdown()
{
    mSceneCommandSignature.Reset();
    mRootSignatures.clear();
    mDevice = nullptr;
}

ID3D12RootSignature* RootSignatureManager::GetRootSignature(RootSignatureType type)
{
    auto it = mRootSignatures.find(type);
    if (it != mRootSignatures.end())
        return it->second.Get();

    throw std::runtime_error("RootSignature not found. Did you call Initialize()?");
}

ID3D12CommandSignature* RootSignatureManager::GetSceneCommandSignature()
{
    return mSceneCommandSignature.Get();
}

void RootSignatureManager::CreateSceneRootSignature()
{
    CD3DX12_ROOT_PARAMETER slotRootParameter[3];

    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsConstantBufferView(1);

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    slotRootParameter[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC samplerDesc(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        3, slotRootParameter,
        1, &samplerDesc,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3D12SerializeRootSignature(
        &rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(),
        errorBlob.GetAddressOf());

    if (errorBlob)
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

void RootSignatureManager::CreateIndirectBuildRootSignature()
{
    // b0 constants | t0 requests (root SRV) | u0 commands | u1 counter
    CD3DX12_ROOT_PARAMETER params[4];
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsShaderResourceView(0);
    params[2].InitAsUnorderedAccessView(0);
    params[3].InitAsUnorderedAccessView(1);

    CD3DX12_ROOT_SIGNATURE_DESC desc(
        4, params,
        0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1,
        serialized.GetAddressOf(), error.GetAddressOf());
    if (error)
        OutputDebugStringA((char*)error->GetBufferPointer());
    ThrowIfFailed(hr);

    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(mDevice->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&rootSig)));

    mRootSignatures[RootSignatureType::IndirectBuild] = rootSig;
}

void RootSignatureManager::CreateSceneCommandSignature()
{
    ID3D12RootSignature* sceneRS = GetRootSignature(RootSignatureType::Scene);

    D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    args[0].ConstantBufferView.RootParameterIndex = 0; // ObjectCB b0
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.pArgumentDescs = args;
    desc.NumArgumentDescs = _countof(args);
    desc.ByteStride = sizeof(IndirectCommand);
    desc.NodeMask = 0;

    ThrowIfFailed(mDevice->CreateCommandSignature(
        &desc,
        sceneRS,
        IID_PPV_ARGS(&mSceneCommandSignature)));
}

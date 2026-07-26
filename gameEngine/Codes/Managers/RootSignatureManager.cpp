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
    CreateShadowRootSignature();
    CreateIndirectBuildRootSignature();
    CreateHiZBuildRootSignature();
    CreateComposeWorldRootSignature();
    CreateUpdateMotionRootSignature();
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
    // b0 ObjectCB | b1 PassCB | table t0 albedo | root SRV t1 instances | table t2 shadow
    CD3DX12_ROOT_PARAMETER slotRootParameter[5];

    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsConstantBufferView(1);

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = 1;
    shadowRange.BaseShaderRegister = 2; // t2
    shadowRange.RegisterSpace = 0;
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    slotRootParameter[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[3].InitAsShaderResourceView(1); // t1 instance worlds
    slotRootParameter[4].InitAsDescriptorTable(1, &shadowRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC samplers[2];
    samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(
        1, // s1 comparison shadow
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        0.0f, 16,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        5, slotRootParameter,
        2, samplers,
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

void RootSignatureManager::CreateShadowRootSignature()
{
    // Depth-only casters: b0 ObjectCB | b1 PassCB | t1 instances (optional)
    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsConstantBufferView(1);
    params[2].InitAsShaderResourceView(1);

    CD3DX12_ROOT_SIGNATURE_DESC desc(
        3, params,
        0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

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

    mRootSignatures[RootSignatureType::Shadow] = rootSig;
}

void RootSignatureManager::CreateIndirectBuildRootSignature()
{
    // b0 frame | t0 source | t1 batches | t2 submeshes
    // t3 HiZ (table) | s0 point | u0 compact | u1 counters | u2 drawCmds
    CD3DX12_DESCRIPTOR_RANGE hizRange;
    hizRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3); // t3

    CD3DX12_ROOT_PARAMETER params[8];
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsShaderResourceView(0);
    params[2].InitAsShaderResourceView(1);
    params[3].InitAsShaderResourceView(2);
    params[4].InitAsDescriptorTable(1, &hizRange, D3D12_SHADER_VISIBILITY_ALL);
    params[5].InitAsUnorderedAccessView(0);
    params[6].InitAsUnorderedAccessView(1);
    params[7].InitAsUnorderedAccessView(2);

    CD3DX12_STATIC_SAMPLER_DESC samp(
        0,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC desc(
        8, params,
        1, &samp,
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

void RootSignatureManager::CreateHiZBuildRootSignature()
{
    // b0 sizes | t0 src tex | u0 dst mip
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsConstants(8, 0); // srcW,srcH,dstW,dstH,srcMip,pad...
    params[1].InitAsDescriptorTable(1, &srvRange);
    params[2].InitAsDescriptorTable(1, &uavRange);

    CD3DX12_ROOT_SIGNATURE_DESC desc(
        3, params,
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

    mRootSignatures[RootSignatureType::HiZBuild] = rootSig;
}

void RootSignatureManager::CreateComposeWorldRootSignature()
{
    // b0 num | t0 transforms | u0 GpuInstanceSource out
    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsConstants(4, 0);
    params[1].InitAsShaderResourceView(0);
    params[2].InitAsUnorderedAccessView(0);

    CD3DX12_ROOT_SIGNATURE_DESC desc(
        3, params,
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

    mRootSignatures[RootSignatureType::ComposeWorld] = rootSig;
}

void RootSignatureManager::CreateUpdateMotionRootSignature()
{
    // b0 num/max/dt | u0 transforms | u1 motion
    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsConstants(4, 0);
    params[1].InitAsUnorderedAccessView(0);
    params[2].InitAsUnorderedAccessView(1);

    CD3DX12_ROOT_SIGNATURE_DESC desc(
        3, params,
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

    mRootSignatures[RootSignatureType::UpdateMotion] = rootSig;
}

void RootSignatureManager::CreateSceneCommandSignature()
{
    // Instanced path: DRAW_INDEXED only (worlds in t1 SRV)
    D3D12_INDIRECT_ARGUMENT_DESC args[1] = {};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.pArgumentDescs = args;
    desc.NumArgumentDescs = 1;
    desc.ByteStride = sizeof(IndirectCommand);
    desc.NodeMask = 0;

    // DRAW_INDEXED only does not require a root signature
    ThrowIfFailed(mDevice->CreateCommandSignature(
        &desc,
        nullptr,
        IID_PPV_ARGS(&mSceneCommandSignature)));
}

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
    // Direct CBV (b0: Object, b1: Pass) + Descriptor Table for texture (t0)
    CD3DX12_ROOT_PARAMETER slotRootParameter[3];

    // b0 - ObjectCB (direct root CBV)
    slotRootParameter[0].InitAsConstantBufferView(0);

    // b1 - PassCB (direct root CBV)
    slotRootParameter[1].InitAsConstantBufferView(1);

    // t0 - Texture SRV (descriptor table)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    slotRootParameter[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC samplerDesc(
        0,                                      // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        3, slotRootParameter,
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

void RootSignatureManager::CreateCommandSignature()
{
    // 1. 간접 명령 버퍼에 들어갈 구조체를 정확히 정의
    struct IndirectCommand
    {
        D3D12_GPU_VIRTUAL_ADDRESS cbv;              // ConstantBufferView 주소
        D3D12_DRAW_ARGUMENTS      drawArguments;    // Draw 파라미터
    };

    // 2. Command Signature 생성
    D3D12_INDIRECT_ARGUMENT_DESC argumentDescs[2] = {};

    argumentDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    argumentDescs[0].ConstantBufferView.RootParameterIndex = 0;   // Root Signature의 해당 인덱스

    argumentDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC cmdSigDesc = {};
    cmdSigDesc.pArgumentDescs = argumentDescs;
    cmdSigDesc.NumArgumentDescs = _countof(argumentDescs);
    cmdSigDesc.ByteStride = sizeof(IndirectCommand);        // 가장 중요!

    ComPtr<ID3D12CommandSignature> commandSignature;
    mDevice->CreateCommandSignature(
        &cmdSigDesc,
        nullptr,
        IID_PPV_ARGS(&commandSignature)
    );
}

#include "MatarialManager.h"

bool MatarialManager::CreateMatarial(
    const std::string& name,
    const std::wstring& filePath,
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12CommandQueue* commandQueue,
    DescriptorAllocator& descriptorAllocator)
{
    Matarial mMatarial;
    mMatarial.name = name;

    // 1. Texture 로드
    if (!TextureLoad(filePath, mMatarial.mTexture, mMatarial.mTextureUploadHeap, device, cmdList, commandQueue))
        return false;

    // 2. SRV 생성
    mMatarial.mTextureHandle = descriptorAllocator.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = mMatarial.mTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = mMatarial.mTexture->GetDesc().MipLevels;
    device->CreateShaderResourceView(mMatarial.mTexture.Get(), &srvDesc, mMatarial.mTextureHandle.CPU);

    // 3. Shader 컴파일
    mMatarial.mvsByteCode = d3dUtil::CompileShader(L"Resources\\Shaders\\object.hlsl", nullptr, "VS", "vs_5_0");
    mMatarial.mpsByteCode = d3dUtil::CompileShader(L"Resources\\Shaders\\object.hlsl", nullptr, "PS", "ps_5_0");

    // 4. Input Layout
    mMatarial.mInputLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // =====================================================
    // 5. Root Signature 생성 (ObjectCB + Texture SRV)
    // =====================================================
    CD3DX12_ROOT_PARAMETER slotRootParameter[2];

    // b0 : ObjectConstants (CBV)
    CD3DX12_DESCRIPTOR_RANGE cbvRange;
    cbvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    slotRootParameter[0].InitAsDescriptorTable(1, &cbvRange, D3D12_SHADER_VISIBILITY_ALL);

    // t0 : Texture (SRV)
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    slotRootParameter[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

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

    ThrowIfFailed(device->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mMatarial.mRootSignature)));

    // =====================================================
    // 6. PSO 생성
    // =====================================================
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { mMatarial.mInputLayout.data(), (UINT)mMatarial.mInputLayout.size() };
    psoDesc.pRootSignature = mMatarial.mRootSignature.Get();
    psoDesc.VS = { mMatarial.mvsByteCode->GetBufferPointer(), mMatarial.mvsByteCode->GetBufferSize() };
    psoDesc.PS = { mMatarial.mpsByteCode->GetBufferPointer(), mMatarial.mpsByteCode->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;   // 네 스왑체인 포맷에 맞게 수정
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mMatarial.mPSO)));

    // 7. 저장
    mMatarials.emplace(name, std::make_shared<Matarial>(std::move(mMatarial)));
    return true;
}

std::shared_ptr<Matarial> MatarialManager::GetMatarial(const std::string& name)
{
	auto it = mMatarials.find(name);
	if (it != mMatarials.end())
	{
		return it->second;
	}
	return nullptr;
}

std::shared_ptr<Matarial> MatarialManager::GetDefaultMaterial()
{
    static std::string defaultName = "Default";
    auto mat = GetMatarial(defaultName);
    return mat;
}

void MatarialManager::Shutdown()
{
	for (auto& pair : mMatarials)
	{
		if (pair.second)
		{
			auto& mat = pair.second;

			// ComPtr 명시적 해제 (순서 중요)
			mat->mpsByteCode.Reset();
			mat->mvsByteCode.Reset();
			mat->mTextureUploadHeap.Reset();
			mat->mTexture.Reset();
			mat->mInputLayout.clear();
		}
	}

	mMatarials.clear();
}

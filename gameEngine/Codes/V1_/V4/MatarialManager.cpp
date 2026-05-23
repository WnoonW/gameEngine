#include "MatarialManager.h"

bool MatarialManager::CreateMatarial(const std::string& name, const std::wstring& filePath, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* commandQueue)
{
	Matarial mMatarial;
	mMatarial.name = name;
	if (!TextureLoad(filePath, mMatarial.mTexture, mMatarial.mTextureUploadHeap, device, cmdList, commandQueue))
	{ return false; }

	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc;
	srvHeapDesc.NumDescriptors = 1;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	srvHeapDesc.NodeMask = 0;
	ThrowIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mMatarial.mSrvHeap)));

	mMatarial.mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	mMatarial.mSrvCpuHandle = mMatarial.mSrvHeap->GetCPUDescriptorHandleForHeapStart();
	mMatarial.mSrvGpuHandle = mMatarial.mSrvHeap->GetGPUDescriptorHandleForHeapStart();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = mMatarial.mTexture->GetDesc().Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = mMatarial.mTexture->GetDesc().MipLevels;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc.Texture2D.PlaneSlice = 0;
	device->CreateShaderResourceView(mMatarial.mTexture.Get(), &srvDesc, mMatarial.mSrvCpuHandle);

	mMatarial.mvsByteCode = d3dUtil::CompileShader(L"Resources\\Shaders\\object.hlsl", nullptr, "VS", "vs_5_0");
	mMatarial.mpsByteCode = d3dUtil::CompileShader(L"Resources\\Shaders\\object.hlsl", nullptr, "PS", "ps_5_0");

	mMatarial.mInputLayout = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};


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
			mat->mSrvHeap.Reset();

			mat->mInputLayout.clear();
		}
	}

	mMatarials.clear();
}

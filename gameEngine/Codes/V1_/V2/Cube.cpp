#include "Cube.h"
#include "ResourceLoader.h"
#include "../V3/MeshManager.h"

void Cube::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandAllocator* cmdAllocator, ID3D12CommandQueue* cmdQueue, std::vector<std::unique_ptr<FrameResource>>& frameResources, int gNumFrameResources)
{
	BuildDescriptorHeaps(device);
	BuildConstantBuffers(device, frameResources, gNumFrameResources);
	BuildShaderResourceViews(device, cmdList, cmdQueue);
	BuildRootSignature(device);
	BuildShadersAndInputLayout();
	BuildBoxGeometry(device, cmdList);
	BuildPSO(device);
}

void Cube::BuildDescriptorHeaps(ID3D12Device* device)
{
	D3D12_DESCRIPTOR_HEAP_DESC cbvsrvHeapDesc;
	cbvsrvHeapDesc.NumDescriptors = 4;
	cbvsrvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvsrvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	cbvsrvHeapDesc.NodeMask = 0;
	ThrowIfFailed(device->CreateDescriptorHeap(&cbvsrvHeapDesc, IID_PPV_ARGS(&mCbvSrvHeap)));

	mCbvSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	mCbvSrvHeapHandle = mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart();
	mCbvGpuHandleStart = mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart();
}

void Cube::BuildConstantBuffers(ID3D12Device* device, std::vector<std::unique_ptr<FrameResource>>& frameResources, int gNumFrameResources)
{
	for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
	{
		auto objectCB = frameResources[frameIndex]->ObjectCB->Resource();

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
		cbvDesc.BufferLocation = objectCB->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

		device->CreateConstantBufferView(&cbvDesc, mCbvSrvHeapHandle);
		mCbvSrvHeapHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}
}

void Cube::BuildShaderResourceViews(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* cmdQueue)
{
	//TextureLoad(L"Textures/bricks.dds", mTexture, mTextureUploadHeap, device, cmdList, cmdQueue);
	TextureLoad(L"Resources/Textures/e.png", mTexture, mTextureUploadHeap, device, cmdList, cmdQueue);
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = mTexture->GetDesc().Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = mTexture->GetDesc().MipLevels;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc.Texture2D.PlaneSlice = 0;
	device->CreateShaderResourceView(mTexture.Get(), &srvDesc, mCbvSrvHeapHandle);
}

void Cube::BuildRootSignature(ID3D12Device* device)
{
	CD3DX12_ROOT_PARAMETER slotRootParameter[2];

	// [0] CBV Table (ObjectConstants) - 회전할 부분
	CD3DX12_DESCRIPTOR_RANGE cbvRange;
	cbvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);  // b0
	slotRootParameter[0].InitAsDescriptorTable(1, &cbvRange);

	// [1] SRV Table (Texture) - 고정할 부분
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);  // t0
	slotRootParameter[1].InitAsDescriptorTable(1, &srvRange);

	CD3DX12_STATIC_SAMPLER_DESC samplerDesc(
		0,                                      // shaderRegister (s0)
		D3D12_FILTER_MIN_MAG_MIP_LINEAR,        // 필터링
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,        // U
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,        // V
		D3D12_TEXTURE_ADDRESS_MODE_WRAP         // W
	);

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, slotRootParameter, 1, &samplerDesc, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig = nullptr;
	Microsoft::WRL::ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if(errorBlob != nullptr)
	{
		OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(device->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(&mRootSignature)));
}

void Cube::BuildShadersAndInputLayout()
{
	HRESULT hr = S_OK;

	mvsByteCode = d3dUtil::CompileShader(L"Resources\\Shaders\\object.hlsl", nullptr, "VS", "vs_5_0");
	mpsByteCode = d3dUtil::CompileShader(L"Resources\\Shaders\\object.hlsl", nullptr, "PS", "ps_5_0");

	mInputLayout = 	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
}

void Cube::BuildBoxGeometry(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
	mMesh = MeshManager::Get().GetMesh("bibian");
/*	Model model;
	if (!MeshLoad(L"Resources/Assets/bibian.obj", model))
		return;

	for (size_t i = 0; i < model.submeshes.size(); ++i)
	{
		const auto& sub = model.submeshes[i];
		char buf[256];
		sprintf_s(buf, "[Submesh %zu] material=%s, vertices=%zu, indices=%zu\n",
			i, sub.materialName.c_str(), sub.vertices.size(), sub.indices.size());
		OutputDebugStringA(buf);
	}

	if (model.submeshes.empty())
		return;

	// ============================================
	// 1. 모든 서브메시를 하나의 큰 버퍼로 합치기
	// ============================================
	std::vector<Vertex> allVertices;
	std::vector<uint32_t> allIndices;

	struct SubmeshOffset
	{
		UINT indexCount;
		UINT startIndexLocation;
		UINT baseVertexLocation;
	};
	std::vector<SubmeshOffset> offsets;

	for (const auto& sub : model.submeshes)
	{
		UINT baseVertex = (UINT)allVertices.size();
		UINT startIndex = (UINT)allIndices.size();

		// 정점 추가
		allVertices.insert(allVertices.end(), sub.vertices.begin(), sub.vertices.end());

		// 인덱스 추가 (baseVertex를 더해줘야 함)
		for (auto index : sub.indices)
		{
			allIndices.push_back(index + baseVertex);
		}

		// 오프셋 기록
		offsets.push_back({ (UINT)sub.indices.size(), startIndex, baseVertex });
	}

	// ============================================
	// 2. 버퍼 생성
	// ============================================
	const UINT vbByteSize = (UINT)allVertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)allIndices.size() * sizeof(uint32_t);

	mBoxGeo = std::make_unique<MeshGeometry>();
	mBoxGeo->Name = "meshGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &mBoxGeo->VertexBufferCPU));
	CopyMemory(mBoxGeo->VertexBufferCPU->GetBufferPointer(), allVertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &mBoxGeo->IndexBufferCPU));
	CopyMemory(mBoxGeo->IndexBufferCPU->GetBufferPointer(), allIndices.data(), ibByteSize);

	mBoxGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, allVertices.data(), vbByteSize, mBoxGeo->VertexBufferUploader);
	mBoxGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, allIndices.data(), ibByteSize, mBoxGeo->IndexBufferUploader);

	mBoxGeo->VertexByteStride = sizeof(Vertex);
	mBoxGeo->VertexBufferByteSize = vbByteSize;
	mBoxGeo->IndexFormat = DXGI_FORMAT_R32_UINT;
	mBoxGeo->IndexBufferByteSize = ibByteSize;

	// ============================================
	// 3. 각 서브메시 등록 (submesh_0, submesh_1 ...)
	// ============================================
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		if (offsets[i].indexCount == 0) continue;

		SubmeshGeometry submesh;
		submesh.IndexCount = offsets[i].indexCount;
		submesh.StartIndexLocation = offsets[i].startIndexLocation;
		submesh.BaseVertexLocation = 0;                    // ← 여기 중요! 0으로 고정

		std::string key = "submesh_" + std::to_string(i);
		mBoxGeo->DrawArgs[key] = submesh;
	}*/
}

void Cube::BuildPSO(ID3D12Device* device)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
	ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	psoDesc.pRootSignature = mRootSignature.Get();
	psoDesc.VS = { reinterpret_cast<BYTE*>(mvsByteCode->GetBufferPointer()), mvsByteCode->GetBufferSize() };
	psoDesc.PS = { reinterpret_cast<BYTE*>(mpsByteCode->GetBufferPointer()), mpsByteCode->GetBufferSize() };
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(mPSO.GetAddressOf())));
}

void Cube::Update(const GameTimer& gt, float mRadius, float mTheta, float mPhi, FrameResource* mCurrFrameResource, int currFrameIndex)
{
	float x = mRadius * sinf(mPhi) * cosf(mTheta);
	float z = mRadius * sinf(mPhi) * sinf(mTheta);
	float y = mRadius * cosf(mPhi);

	XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&mView, view);

	XMMATRIX world = XMLoadFloat4x4(&mWorld);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);
	XMMATRIX worldViewProj = world * view * proj;

	mCurrFrameIndex = currFrameIndex;

	ObjectConstants objConstants;
	XMStoreFloat4x4(&objConstants.WorldViewProj, XMMatrixTranspose(worldViewProj));
	mCurrFrameResource->ObjectCB->CopyData(0, objConstants);
}

void Cube::Draw(ID3D12GraphicsCommandList* cmdList)
{
	ID3D12DescriptorHeap* descriptorHeaps[] = { mCbvSrvHeap.Get() };
	cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
	cmdList->SetGraphicsRootSignature(mRootSignature.Get());
	cmdList->IASetVertexBuffers(0, 1, &mMesh->VertexBufferView());
	cmdList->IASetIndexBuffer(&mMesh->IndexBufferView());
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle = mCbvGpuHandleStart;
	cbvHandle.ptr += mCurrFrameIndex * mCbvSrvDescriptorSize;   // 핵심!
	cmdList->SetGraphicsRootDescriptorTable(0, cbvHandle);

	D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = mCbvGpuHandleStart;
	srvHandle.ptr += 3 * mCbvSrvDescriptorSize;
	cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);

	cmdList->SetPipelineState(mPSO.Get());

	// 모든 서브메시 그리기
	for (auto& pair : mMesh->DrawArgs)
	{
		auto& sub = pair.second;
		cmdList->DrawIndexedInstanced(sub.IndexCount, 1, sub.StartIndexLocation, sub.BaseVertexLocation, 0);
	}
 
	//선택된 서브메시만 그리기 (디버그용)
/*	std::string targetKey = "submesh_" + std::to_string(mSelectedSubmeshIndex);
	auto it = mBoxGeo->DrawArgs.find(targetKey);
	if (it != mBoxGeo->DrawArgs.end())
	{
		auto& sub = it->second;

		// 디버그용 (원하면 주석 처리)
		char buf[128];
		sprintf_s(buf, "Drawing Submesh: %s (IndexCount=%u)\n",
			targetKey.c_str(), sub.IndexCount);
		OutputDebugStringA(buf);

		cmdList->DrawIndexedInstanced(
			sub.IndexCount,
			1,
			sub.StartIndexLocation,
			sub.BaseVertexLocation,
			0);
	}*/
}

void Cube::OnResize(float ratio)
{
	XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, ratio, 1.0f, 1000.0f);
	XMStoreFloat4x4(&mProj, P);
}


void Cube::NextSubmesh()
{
	if (!mMesh) return;

	size_t total = mMesh->DrawArgs.size();
	if (total == 0) return;

	mSelectedSubmeshIndex++;
	if (mSelectedSubmeshIndex >= (int)total)
		mSelectedSubmeshIndex = 0;   // 마지막에서 처음으로 순환
}

void Cube::PrevSubmesh()
{
	if (!mMesh) return;

	size_t total = mMesh->DrawArgs.size();
	if (total == 0) return;

	mSelectedSubmeshIndex--;
	if (mSelectedSubmeshIndex < 0)
		mSelectedSubmeshIndex = (int)total - 1;   // 처음에서 마지막으로 순환
}

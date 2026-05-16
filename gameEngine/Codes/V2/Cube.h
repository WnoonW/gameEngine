#pragma once
#include <DirectXMath.h>
#include <wrl.h>
#include <memory>
#include "../Common/d3dx12.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GameTimer.h"
//struct
#include "../Structs/modelStruct.h"
#include "../Structs/constantStruct.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class Cube
{

public:
	void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandAllocator* cmdAllocator, ID3D12CommandQueue* cmdQueue);

	void BuildDescriptorHeaps(ID3D12Device* device);
	void BuildConstantBuffers(ID3D12Device* device);
	void BuildShaderResourceViews(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* cmdQueue);
	void BuildRootSignature(ID3D12Device* device);
	void BuildShadersAndInputLayout();
	void BuildBoxGeometry(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);
	void BuildPSO(ID3D12Device* device);

	void Update(const GameTimer& gt, float mRadius, float mTheta, float mPhi);
	void Draw(ID3D12GraphicsCommandList* cmdList);
	void OnResize(float ratio);

private:
	ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
	ComPtr<ID3D12DescriptorHeap> mCbvSrvHeap = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE mCbvSrvHeapHandle;

	ComPtr<ID3D12Resource> mTexture = nullptr;
	ComPtr<ID3D12Resource> mTextureUploadHeap = nullptr;

	std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;

	std::unique_ptr<MeshGeometry> mBoxGeo = nullptr;

	ComPtr<ID3DBlob> mvsByteCode = nullptr;
	ComPtr<ID3DBlob> mpsByteCode = nullptr;

	std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	ComPtr<ID3D12PipelineState> mPSO = nullptr;

	XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
	XMFLOAT4X4 mView = MathHelper::Identity4x4();
	XMFLOAT4X4 mProj = MathHelper::Identity4x4();

	

public:
	void NextSubmesh();
	void PrevSubmesh();
	int mSelectedSubmeshIndex = 0;
};
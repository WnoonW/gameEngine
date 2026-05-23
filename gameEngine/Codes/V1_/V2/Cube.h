#pragma once
#include <DirectXMath.h>
#include <wrl.h>
#include <memory>
#include "../../Common/d3dx12.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GameTimer.h"
#include "../V3/MeshManager.h"
#include "../V4/MatarialManager.h"
//struct
#include "../../Structs/modelStruct.h"
#include "../../Structs/constantStruct.h"
#include "../../Structs/AppStruct.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class Cube
{
public:
	void Shutdown();

public:
	void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandAllocator* cmdAllocator, ID3D12CommandQueue* cmdQueue, std::vector<std::unique_ptr<FrameResource>>& frameResources, int gNumFrameResources, DescriptorAllocator& descriptorAllocator);

	void BuildConstantBuffers(ID3D12Device* device, std::vector<std::unique_ptr<FrameResource>>& frameResources, int gNumFrameResources, DescriptorAllocator& descriptorAllocator);
	void BuildRootSignature(ID3D12Device* device);
	void BuildBoxGeometry(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);
	void BuildPSO(ID3D12Device* device);

	void Update(const GameTimer& gt, float mRadius, float mTheta, float mPhi, FrameResource* mCurrFrameResource, int currFrameIndex);
	void Draw(ID3D12GraphicsCommandList* cmdList, DescriptorAllocator& descriptorAllocator);
	void OnResize(float ratio);

	Mesh* mMesh = nullptr;
	std::shared_ptr<Matarial> mMatarial = nullptr;
private:
	ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

	int mCurrFrameIndex = 0;

	// Cube.h에 추가 추천
	std::vector<DescriptorAllocator::DescriptorHandle> mCBVHandles;  // frame 수만큼 저장

	ComPtr<ID3D12PipelineState> mPSO = nullptr;

	XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
	XMFLOAT4X4 mView = MathHelper::Identity4x4();
	XMFLOAT4X4 mProj = MathHelper::Identity4x4();


public:
	void NextSubmesh();
	void PrevSubmesh();
	int mSelectedSubmeshIndex = 0;
};
#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include "../Common/MathHelper.h"

static constexpr D3D12_RESOURCE_STATES DepthReadState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct DepthStencilContext
{
    ID3D12Resource* resource = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_RESOURCE_STATES* currentState = nullptr;
};

struct PassConstants
{
	DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
};

struct ObjectConstants
{
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
};

struct IndirectDrawCommand
{
	// Instancing 지원을 위해 baseInstance로 재해석됨.
	// CommandSignature의 CONSTANT 인자를 통해 root b13으로 전달.
	// 셰이더에서 baseInstance + SV_InstanceID 로 인스턴스 데이터 인덱싱.
	uint32_t baseInstance = 0;
	D3D12_DRAW_INDEXED_ARGUMENTS drawArgs{};
};

// Compute Shader에서 IndirectDrawCommand를 기록할 때 사용하는 입력 데이터
struct GroupDrawData
{
    uint32_t baseInstance;
    uint32_t instanceCount;
    uint32_t indexCountPerInstance;
    uint32_t startIndexLocation;
    int32_t  baseVertexLocation;

    // per-submesh local AABB (for frustum culling in compute etc.)
    DirectX::XMFLOAT3 localCenter;
    DirectX::XMFLOAT3 localExtents;
};
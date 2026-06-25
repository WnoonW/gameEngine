#pragma once
#include <DirectXMath.h>
#include "../Common/MathHelper.h"

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
	uint32_t objectCBIndex = 0;
	D3D12_DRAW_INDEXED_ARGUMENTS drawArgs{};
};
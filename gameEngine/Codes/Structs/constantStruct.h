#pragma once
#include <DirectXMath.h>
#include "../Common/MathHelper.h"

struct ObjectConstants
{
	DirectX::XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
};

using IndirectDrawCommand = D3D12_DRAW_INDEXED_ARGUMENTS;
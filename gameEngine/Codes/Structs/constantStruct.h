#pragma once
#include <DirectXMath.h>
#include "../Common/MathHelper.h"

inline constexpr UINT kMaxRenderableInstances = 16384;

struct ObjectConstants
{
	DirectX::XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
};

struct PassConstants
{
	UINT InstanceOffset = 0;
	UINT padding[3] = {};
};

struct InstanceData
{
	DirectX::XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
};
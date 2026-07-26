#pragma once
#include <DirectXMath.h>
#include "../Common/MathHelper.h"
#include "../Common/d3dUtil.h"   // for Light and MaxLights

struct PassConstants
{
    DirectX::XMFLOAT4X4 View = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvView = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 Proj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT3   EyePosW = { 0.0f, 0.0f, 0.0f };
    float               cbPerObjectPad1 = 0.0f;
    DirectX::XMFLOAT2   RenderTargetSize = { 0.0f, 0.0f };
    DirectX::XMFLOAT2   InvRenderTargetSize = { 0.0f, 0.0f };
    float               NearZ = 0.0f;
    float               FarZ = 0.0f;
    float               TotalTime = 0.0f;
    float               DeltaTime = 0.0f;
    DirectX::XMFLOAT4   AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
    // Lights[0] = primary directional (Direction = ray travel dir, Strength = RGB intensity)
    Light               Lights[MaxLights];

    // Graphics style (must match HLSL cbPass tail)
    // 0 = Realistic (smooth Lambert + Blinn), 1 = Toon (banded NdotL)
    int                 GraphicsStyle = 0;
    float               ToonBands = 3.0f;
    float               OutlineWidth = 0.025f; // object-space expand for inverted-hull outline
    float               SpecularPower = 32.0f;

    // Shadow (row-vector HLSL: mul(pos, LightViewProj) with transposed CPU store)
    DirectX::XMFLOAT4X4 LightViewProj = MathHelper::Identity4x4();
    float               ShadowBias = 0.003f;
    float               ShadowEnabled = 1.0f;
    float               ShadowSoftness = 1.0f; // unused (hard shadows for now)
    float               cbPassPadShadow = 0.0f;
};

struct ObjectConstants
{
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
};


// Per-object path
// Root: b0 object | b1 pass | table t0 texture | t1 instances | table t2 shadow

#include "lighting_common.hlsli"

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
};

cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3   gEyePosW;
    float    cbPassPad1;

    float2   gRenderTargetSize;
    float2   gInvRenderTargetSize;

    float    gNearZ;
    float    gFarZ;
    float    gTotalTime;
    float    gDeltaTime;

    float4   gAmbientLight;
    Light    gLights[16];
    int      gGraphicsStyle;
    float    gToonBands;
    float    gOutlineWidth;
    float    gSpecularPower;

    float4x4 gLightViewProj;
    float    gShadowBias;
    float    gShadowEnabled;
    float    gShadowSoftness;
    float    gShadowPad;
};

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);
Texture2D gShadowMap : register(t2);
SamplerComparisonState gShadowSampler : register(s1);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 Normal : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = worldPos.xyz;
    float4 viewPos = mul(worldPos, gView);
    vout.PosH = mul(viewPos, gProj);

    vout.NormalW = mul(float4(vin.Normal, 0.0f), gWorld).xyz;
    vout.TexC = vin.TexC;
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    // Fully opaque: no clip, no alpha. (clip/expansion previously caused edge holes.)
    float3 albedo = gTexture.Sample(gSampler, pin.TexC).rgb;
    float3 N = normalize(pin.NormalW);
    float3 V = normalize(gEyePosW - pin.PosW);

    float shadow = SampleShadowMap(
        gShadowMap, gShadowSampler, gLightViewProj,
        pin.PosW, gShadowBias, gShadowEnabled);

    float3 lit = ShadeLit(
        albedo, N, V,
        gAmbientLight.rgb,
        gLights[0],
        gGraphicsStyle,
        gToonBands,
        gSpecularPower,
        shadow);
    return float4(lit, 1.0f);
}

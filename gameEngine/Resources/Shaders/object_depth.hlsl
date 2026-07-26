// Depth-only shadow caster (per-object). No pixel shader.

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
};

struct Light
{
    float3 Strength;
    float  FalloffStart;
    float3 Direction;
    float  FalloffEnd;
    float3 Position;
    float  SpotPower;
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

struct VertexIn
{
    float3 PosL : POSITION;
    float3 Normal : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosH = mul(worldPos, gLightViewProj);
    return vout;
}

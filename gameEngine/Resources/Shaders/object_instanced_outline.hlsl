// Inverted-hull outline for instanced path (toon style).
// Draw with CullMode = FRONT after the main opaque pass.

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
};

StructuredBuffer<float4x4> gInstances : register(t1);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 Normal : NORMAL;
    float2 TexC : TEXCOORD;
    uint   InstanceId : SV_InstanceID;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float4x4 world = gInstances[vin.InstanceId];
    float3 n = normalize(vin.Normal);
    float3 posL = vin.PosL + n * gOutlineWidth;
    float4 worldPos = mul(float4(posL, 1.0f), world);
    float4 viewPos = mul(worldPos, gView);
    vout.PosH = mul(viewPos, gProj);
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    return float4(0.02f, 0.02f, 0.04f, 1.0f);
}

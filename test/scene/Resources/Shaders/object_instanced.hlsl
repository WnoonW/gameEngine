// Instanced path
// Root: b1 pass | table t0 single texture | t1 instances (space0)

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
};

StructuredBuffer<float4x4> gInstances : register(t1);

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

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
    float3 Normal : NORMAL;
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float4x4 world = gInstances[vin.InstanceId];
    float4 worldPos = mul(float4(vin.PosL, 1.0f), world);
    float4 viewPos  = mul(worldPos, gView);
    vout.PosH       = mul(viewPos, gProj);
    vout.Normal = vin.Normal;
    vout.TexC = vin.TexC;
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    return gTexture.Sample(gSampler, pin.TexC);
}

// Root signature는 C++ (RootSignatureManager)에서만 정의.
// [0] CBV b0 (ObjectConstants) - 하위 호환
// [1] CBV b1 (PassConstants)
// [2] SRV table t0 (texture)
// [3] Root Constants b13 (baseInstance - indirect)
// [4] SRV t1 (Instance StructuredBuffer)

struct ObjectConstants
{
    float4x4 World;
};

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
};

cbuffer cbPerPass : register(b1)
{
    float4x4 gViewProj;
};

StructuredBuffer<ObjectConstants> gInstanceData : register(t1);   // instancing용

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 Normal : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 Normal : NORMAL;
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
    VertexOut vout;

    // ExecuteIndirect + StartInstanceLocation + InstanceCount 사용 시
    // instanceID 는 StartInstanceLocation 부터 시작함
    float4x4 world = gInstanceData[instanceID].World;

    vout.PosH = mul(mul(float4(vin.PosL, 1.0f), world), gViewProj);
    vout.Normal = vin.Normal;
    vout.TexC = vin.TexC;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 color = gTexture.Sample(gSampler, pin.TexC);
    return color;
}
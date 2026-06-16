cbuffer cbPass : register(b0)
{
    uint gInstanceOffset;
    uint3 cbPassPad;
};

struct InstanceData
{
    float4x4 gWorldViewProj;
};

StructuredBuffer<InstanceData> gInstanceData : register(t1);

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

    float4x4 wvp = gInstanceData[gInstanceOffset + instanceID].gWorldViewProj;
    vout.PosH = mul(float4(vin.PosL, 1.0f), wvp);
    vout.Normal = vin.Normal;
    vout.TexC = vin.TexC;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    return gTexture.Sample(gSampler, pin.TexC);
}
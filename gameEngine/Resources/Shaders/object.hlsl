// Root signature는 C++ (RootSignatureManager)에서만 정의.
// slot 0: Object CBV (b0), slot 1: Pass CBV (b1), slot 2: Texture SRV (t0)
// slot 3: root constants (indirect 전용) - 셰이더에서 참조하지 않음

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
};

cbuffer cbPerPass : register(b1)
{
    float4x4 gViewProj;
};

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

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    vout.PosH = mul(mul(float4(vin.PosL, 1.0f), gWorld), gViewProj);
    vout.Normal = vin.Normal;
    vout.TexC = vin.TexC;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 color = gTexture.Sample(gSampler, pin.TexC);
    return color;
}
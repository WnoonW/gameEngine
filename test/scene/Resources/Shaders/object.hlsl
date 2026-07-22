
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
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
	
	// Transform to homogeneous clip space.
    vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);

    // Pass normal.
    vout.Normal = vin.Normal;
    // Pass texture coordinates.
    vout.TexC = vin.TexC;
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    //float4 pink = float4(1.0f, 0.0f, 1.0f, 1.0f);
    float4 color = gTexture.Sample(gSampler, pin.TexC);
    return color;
}
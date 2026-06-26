cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
};

cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
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
	
	// World transform first (ObjectCB), then ViewProj from PassCB
    float4 worldPos = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosH = mul(worldPos, gViewProj);

    // Pass normal (raw for now — can add WorldIT later if needed)
    vout.Normal = vin.Normal;
    // Pass texture coordinates.
    vout.TexC = vin.TexC;
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 color = gTexture.Sample(gSampler, pin.TexC);
    return color;
}

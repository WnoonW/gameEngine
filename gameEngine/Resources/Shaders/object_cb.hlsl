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
    // Light Lights[16] omitted since not used in this shader
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
    float4 viewPos  = mul(worldPos, gView);
    vout.PosH       = mul(viewPos, gProj);

    // Pass normal (raw for now — can add WorldIT later if needed)
    vout.Normal = vin.Normal;
    // Pass texture coordinates.
    vout.TexC = vin.TexC;
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 color = gTexture.Sample(gSampler, pin.TexC);

    // 텍스처 값을 제대로 받지 못한 경우 (black에 가까운 샘플) 보라색(magenta)으로 표시
    // 디버그용: texture binding 실패나 잘못된 uv 등을 쉽게 파악할 수 있게 함
    if (color.r < 0.01f && color.g < 0.01f && color.b < 0.01f) {
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }

    return color;
}

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
    // 샘플 그대로 출력.
    // 바인딩/해석 실패 디버그는 CPU에서 마젠타 1x1 텍스처 머티리얼을 붙이는 방식으로 처리
    // (정상 검정 알베도를 "실패"로 오인하지 않음).
    return gTexture.Sample(gSampler, pin.TexC);
}

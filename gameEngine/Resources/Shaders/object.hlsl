// Root signature는 C++ (RootSignatureManager)에서만 정의.
// [0] CBV b0 (ObjectConstants) - 하위 호환
// [1] CBV b1 (PassConstants)
// [2] SRV table t0 (texture)
// [3] Root Constants b13 (baseInstance - indirect)
// CommandSignature의 CONSTANT로 per-draw 전달 (여러 메시 그룹 처리에 유용)
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

// Per-draw root constant from CommandSignature (CONSTANT argument)
uint gBaseInstance : register(b13);

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

    // gBaseInstance는 CommandSignature가 per-command으로 설정해줌
    // StartInstanceLocation은 0으로 설정하는 것을 권장
    uint idx = gBaseInstance + instanceID;
    float4x4 world = gInstanceData[idx].World;

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
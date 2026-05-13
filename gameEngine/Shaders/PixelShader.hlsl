//Texture2D gTexture : register(t0);
//SamplerState gSampler : register(s0);

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET
{
    
    return float4(input.TexCoord, 0.0f, 1.0f); // 텍스처 좌표를 색상으로 사용 (디버깅용)
    //return gTexture.Sample(gSampler, input.TexCoord);
}
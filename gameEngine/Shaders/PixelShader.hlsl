Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET
{
    return gTexture.Sample(gSampler, input.TexCoord);
}
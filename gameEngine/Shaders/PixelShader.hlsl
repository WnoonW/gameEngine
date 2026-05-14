struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
    float3 Normal : NORMAL;
};

float4 main(PS_INPUT input) : SV_TARGET
{
    // 텍스처 없이도 보이게 하기 위한 테스트 코드
    float3 color = float3(0.8, 0.6, 0.4); // 기본 색상
    return float4(color, 1.0);
}
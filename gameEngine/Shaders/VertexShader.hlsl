struct VS_INPUT
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD;
    float3 Normal : NORMAL;
};

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
    float3 Normal : NORMAL;
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output;

    // 현재는 변환 없이 그대로 출력 (나중에 World-View-Projection 행렬 추가 예정)
    output.Position = float4(input.Position, 1.0f);
    output.TexCoord = input.TexCoord;
    output.Normal = input.Normal;

    return output;
}
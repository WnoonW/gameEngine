// Image-based UI / VFX billboard quads.
// Root: b0 frame CB | table t0 texture | s0 linear wrap

cbuffer cbUiFrame : register(b0)
{
    float2 gScreenSize; // pixels
    float2 gPad0;
    // 0 = screen ortho (pos already in clip-ish xy), 1 = unused here (CPU builds clip)
    int    gMode;
    int    gPad1;
    int    gPad2;
    int    gPad3;
};

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VSIn
{
    float2 Pos : POSITION;   // clip-space xy (-1..1) already from CPU
    float2 UV  : TEXCOORD0;
    float4 Color : COLOR;
};

struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR;
};

VSOut VS(VSIn vin)
{
    VSOut o;
    o.PosH = float4(vin.Pos, 0.0, 1.0);
    o.UV = vin.UV;
    o.Color = vin.Color;
    return o;
}

float4 PS(VSOut pin) : SV_Target
{
    float4 tex = gTexture.Sample(gSampler, pin.UV);
    float4 c = tex * pin.Color;
    // Premultiplied-friendly alpha: discard full transparent
    clip(c.a - 0.001);
    return c;
}

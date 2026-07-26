// Hi-Z mip0: scene depth → R32 max-ready buffer
// Scene depth is Texture2DMS (4x MSAA); we copy sample 0 (conservative enough for cull).

Texture2DMS<float> gSrcMS : register(t0);
RWTexture2D<float> gDst : register(u0);

cbuffer cbHiZ : register(b0)
{
    uint gSrcWidth;
    uint gSrcHeight;
    uint gDstWidth;
    uint gDstHeight;
    uint gSrcMip;
    uint gPad0;
    uint gPad1;
    uint gPad2;
};

[numthreads(8, 8, 1)]
void CS_CopyDepth(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gDstWidth || dtid.y >= gDstHeight)
        return;

    uint x = min(dtid.x, max(gSrcWidth, 1u) - 1u);
    uint y = min(dtid.y, max(gSrcHeight, 1u) - 1u);
    // Sample 0 of MSAA depth
    float d = gSrcMS.Load(int2(x, y), 0).r;
    if (d <= 0.0f)
        d = 1.0f; // avoid all-zero HiZ wiping the scene via over-cull
    gDst[dtid.xy] = d;
}

// Kept for PSO compatibility (unused in simplified path)
[numthreads(8, 8, 1)]
void CS_Downsample(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gDstWidth || dtid.y >= gDstHeight)
        return;
    // Fallback: sample0 only neighborhood max (no mips on MS depth)
    uint2 s = dtid.xy * 2;
    float d0 = gSrcMS.Load(int2(s.x, s.y), 0).r;
    float d1 = gSrcMS.Load(int2(min(s.x + 1, gSrcWidth - 1), s.y), 0).r;
    float d2 = gSrcMS.Load(int2(s.x, min(s.y + 1, gSrcHeight - 1)), 0).r;
    float d3 = gSrcMS.Load(int2(min(s.x + 1, gSrcWidth - 1), min(s.y + 1, gSrcHeight - 1)), 0).r;
    gDst[dtid.xy] = max(max(d0, d1), max(d2, d3));
}

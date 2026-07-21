// Hi-Z mip0 only: scene depth → R32 max-ready buffer
// (downsample pyramid removed — SRV+UAV same-resource issues / validation)

Texture2D          gSrc : register(t0); // R24 depth or R32 HiZ
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
    // R24 depth SRV → float; clear depth is 1.0
    float d = gSrc.Load(int3(x, y, 0)).r;
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
    uint2 s = dtid.xy * 2;
    int mip = (int)gSrcMip;
    float d0 = gSrc.Load(int3(s.x, s.y, mip)).r;
    float d1 = gSrc.Load(int3(min(s.x + 1, gSrcWidth - 1), s.y, mip)).r;
    float d2 = gSrc.Load(int3(s.x, min(s.y + 1, gSrcHeight - 1), mip)).r;
    float d3 = gSrc.Load(int3(min(s.x + 1, gSrcWidth - 1), min(s.y + 1, gSrcHeight - 1), mip)).r;
    gDst[dtid.xy] = max(max(d0, d1), max(d2, d3));
}

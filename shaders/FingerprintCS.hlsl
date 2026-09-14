cbuffer FingerprintParams : register(b0)
{
    uint PointIndex;
    uint Width;
    uint Height;
    uint TileW;
    uint TileH;
    uint3 FingerprintPad;
};

Texture2D<float4>   FingerprintCurrent  : register(t0);
Texture2D<float4>   FingerprintPrevious : register(t1);
RWByteAddressBuffer FingerprintSums     : register(u0);

groupshared uint g_luma[16];
groupshared uint g_delta[16];

[numthreads(8, 8, 1)]
void CSFingerprint(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi < 16) {
        g_luma[gi] = 0;
        g_delta[gi] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x < Width && id.y < Height) {
        const float3 c = FingerprintCurrent.Load(int3(id.xy, 0)).rgb;
        const float3 p = FingerprintPrevious.Load(int3(id.xy, 0)).rgb;
        const float lc = saturate(dot(c, float3(0.299, 0.587, 0.114))) * 255.0;
        const float lp = saturate(dot(p, float3(0.299, 0.587, 0.114))) * 255.0;
        const uint tx = min(id.x / max(TileW, 1u), 3u);
        const uint ty = min(id.y / max(TileH, 1u), 3u);
        const uint tile = ty * 4 + tx;
        InterlockedAdd(g_luma[tile], (uint)round(lc));
        InterlockedAdd(g_delta[tile], (uint)round(abs(lc - lp)));
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 16 && (g_luma[gi] != 0 || g_delta[gi] != 0)) {
        const uint base = (PointIndex * 16 + gi) * 2 * 4;
        uint ignored;
        FingerprintSums.InterlockedAdd(base, g_luma[gi], ignored);
        FingerprintSums.InterlockedAdd(base + 4, g_delta[gi], ignored);
    }
}

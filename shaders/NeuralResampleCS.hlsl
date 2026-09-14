cbuffer ResampleParams : register(b0)
{
    uint Mode;
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

Texture2D<float4>   Src0 : register(t0);
Texture2D<float4>   Src1 : register(t1);
Texture2D<float4>   Src2 : register(t2);
RWTexture2D<float4> Dest : register(u0);

float4 LoadAt(int2 p, int2 maxCoord, bool residual)
{
    const int3 c = int3(clamp(p, int2(0, 0), maxCoord), 0);
    return residual ? (Src2.Load(c) - Src1.Load(c)) : Src0.Load(c);
}

float4 AreaSample(uint2 srcSize, uint2 dstSize, uint2 d, bool residual)
{
    const float2 scale = float2(srcSize) / float2(dstSize);
    const float2 lo = float2(d) * scale;
    const float2 hi = lo + scale;
    const int2 first = int2(floor(lo));
    const int2 maxCoord = int2(int(srcSize.x) - 1, int(srcSize.y) - 1);
    float4 sum = 0.0;
    float weight = 0.0;
    [unroll]
    for (int y = 0; y < 4; ++y) {
        [unroll]
        for (int x = 0; x < 4; ++x) {
            const int2 p = first + int2(x, y);
            const float2 a = max(lo, float2(p));
            const float2 b = min(hi, float2(p) + 1.0);
            const float2 cover = max(b - a, 0.0);
            const float w = cover.x * cover.y;
            sum += w * LoadAt(p, maxCoord, residual);
            weight += w;
        }
    }
    return sum / max(weight, 1e-6);
}

float4 CatmullRomSample(uint2 srcSize, uint2 dstSize, uint2 d, bool residual)
{
    const float2 srcPos = (float2(d) + 0.5) * (float2(srcSize) / float2(dstSize)) - 0.5;
    const int2 base = int2(floor(srcPos));
    const float2 f = srcPos - float2(base);
    const float2 f2 = f * f;
    const float2 f3 = f2 * f;
    float wx[4], wy[4];
    wx[0] = 0.5 * (-f3.x + 2.0 * f2.x - f.x);
    wx[1] = 0.5 * (3.0 * f3.x - 5.0 * f2.x + 2.0);
    wx[2] = 0.5 * (-3.0 * f3.x + 4.0 * f2.x + f.x);
    wx[3] = 0.5 * (f3.x - f2.x);
    wy[0] = 0.5 * (-f3.y + 2.0 * f2.y - f.y);
    wy[1] = 0.5 * (3.0 * f3.y - 5.0 * f2.y + 2.0);
    wy[2] = 0.5 * (-3.0 * f3.y + 4.0 * f2.y + f.y);
    wy[3] = 0.5 * (f3.y - f2.y);
    const int2 maxCoord = int2(int(srcSize.x) - 1, int(srcSize.y) - 1);
    float4 sum = 0.0;
    [unroll]
    for (int y = 0; y < 4; ++y) {
        [unroll]
        for (int x = 0; x < 4; ++x) {
            sum += (wx[x] * wy[y]) * LoadAt(base + int2(x - 1, y - 1), maxCoord, residual);
        }
    }
    return sum;
}

float4 Filtered(uint2 srcSize, uint2 dstSize, uint2 d, bool residual)
{
    return Mode == 0 ? AreaSample(srcSize, dstSize, d, residual) : CatmullRomSample(srcSize, dstSize, d, residual);
}

[numthreads(8, 8, 1)]
void CSNeuralResample(uint3 id : SV_DispatchThreadID)
{
    uint dw, dh, sw, sh;
    Dest.GetDimensions(dw, dh);
    Src0.GetDimensions(sw, sh);
    if (id.x >= dw || id.y >= dh) {
        return;
    }
    Dest[id.xy] = saturate(Filtered(uint2(sw, sh), uint2(dw, dh), id.xy, false));
}

[numthreads(8, 8, 1)]
void CSNeuralCompose(uint3 id : SV_DispatchThreadID)
{
    uint dw, dh, mw, mh;
    Dest.GetDimensions(dw, dh);
    Src2.GetDimensions(mw, mh);
    if (id.x >= dw || id.y >= dh) {
        return;
    }
    const float4 base = Src0.Load(int3(id.xy, 0));
    const float4 residual = Filtered(uint2(mw, mh), uint2(dw, dh), id.xy, true);
    Dest[id.xy] = float4(saturate(base.rgb + residual.rgb), base.a);
}

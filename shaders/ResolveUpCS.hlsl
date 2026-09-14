Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Dest : register(u0);

float4 TapClamped(int2 coord, int2 maxCoord)
{
    return Source.Load(int3(clamp(coord, int2(0, 0), maxCoord), 0));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint dw, dh, sw, sh;
    Dest.GetDimensions(dw, dh);
    Source.GetDimensions(sw, sh);
    if (id.x >= dw || id.y >= dh)
        return;

    const float2 srcPos =
        (float2(id.xy) + 0.5f) * (float2(sw, sh) / float2(dw, dh)) - 0.5f;
    const int2 base = int2(floor(srcPos));
    const float2 f = srcPos - float2(base);

    float wx[4];
    float wy[4];
    {
        const float2 f2 = f * f;
        const float2 f3 = f2 * f;
        wx[0] = 0.5f * (-f3.x + 2.0f * f2.x - f.x);
        wx[1] = 0.5f * (3.0f * f3.x - 5.0f * f2.x + 2.0f);
        wx[2] = 0.5f * (-3.0f * f3.x + 4.0f * f2.x + f.x);
        wx[3] = 0.5f * (f3.x - f2.x);
        wy[0] = 0.5f * (-f3.y + 2.0f * f2.y - f.y);
        wy[1] = 0.5f * (3.0f * f3.y - 5.0f * f2.y + 2.0f);
        wy[2] = 0.5f * (-3.0f * f3.y + 4.0f * f2.y + f.y);
        wy[3] = 0.5f * (f3.y - f2.y);
    }

    const int2 maxCoord = int2(int(sw) - 1, int(sh) - 1);
    float4 color = 0.0f;
    [unroll]
    for (int y = 0; y < 4; ++y)
    {
        [unroll]
        for (int x = 0; x < 4; ++x)
        {
            color += (wx[x] * wy[y]) *
                TapClamped(base + int2(x - 1, y - 1), maxCoord);
        }
    }
    Dest[id.xy] = max(color, 0.0f);
}

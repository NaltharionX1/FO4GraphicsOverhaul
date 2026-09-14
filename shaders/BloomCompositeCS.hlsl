cbuffer BloomConstants : register(b0)
{
    uint g_srcW;
    uint g_srcH;
    uint g_dstW;
    uint g_dstH;
    float g_p0;
    float g_p1;
    float g_p2;
    float g_p3;
};

Texture2D<float4> Frame : register(t0);
Texture2D<float4> Glow : register(t1);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void CSBloomComposite(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstW || id.y >= g_dstH) {
        return;
    }
    float4 source = Frame.Load(int3(int2(id.xy), 0));
    float2 uv = (float2(id.xy) + 0.5) / float2(g_dstW, g_dstH);
    float3 glow = Glow.SampleLevel(LinearClamp, uv, 0).rgb;

    Output[id.xy] = float4(saturate(source.rgb + g_p0 * glow), source.a);
}

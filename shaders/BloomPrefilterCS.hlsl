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

Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void CSBloomPrefilter(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstW || id.y >= g_dstH) {
        return;
    }
    float2 uv = (float2(id.xy) + 0.5) / float2(g_dstW, g_dstH);
    float3 c = Source.SampleLevel(LinearClamp, uv, 0).rgb;

    float brightness = max(c.r, max(c.g, c.b));
    float knee = g_p0 * g_p1 + 1e-5;
    float soft = clamp(brightness - g_p0 + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contribution = max(soft, brightness - g_p0) / max(brightness, 1e-4);

    Output[id.xy] = float4(c * contribution, 1.0);
}

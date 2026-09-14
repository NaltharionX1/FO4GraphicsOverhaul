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
void CSBloomDown(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstW || id.y >= g_dstH) {
        return;
    }
    float2 texel = 1.0 / float2(g_srcW, g_srcH);
    float2 uv = (float2(id.xy) + 0.5) / float2(g_dstW, g_dstH);

    float3 c = Source.SampleLevel(LinearClamp, uv + texel * float2(-0.5, -0.5), 0).rgb;
    c += Source.SampleLevel(LinearClamp, uv + texel * float2(0.5, -0.5), 0).rgb;
    c += Source.SampleLevel(LinearClamp, uv + texel * float2(-0.5, 0.5), 0).rgb;
    c += Source.SampleLevel(LinearClamp, uv + texel * float2(0.5, 0.5), 0).rgb;

    Output[id.xy] = float4(c * 0.25, 1.0);
}

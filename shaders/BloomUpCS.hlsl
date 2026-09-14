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

Texture2D<float4> Lower : register(t0);
Texture2D<float4> Same : register(t1);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void CSBloomUp(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstW || id.y >= g_dstH) {
        return;
    }
    float2 texel = g_p0 / float2(g_srcW, g_srcH);
    float2 uv = (float2(id.xy) + 0.5) / float2(g_dstW, g_dstH);

    float3 c = Lower.SampleLevel(LinearClamp, uv + texel * float2(-1.0, -1.0), 0).rgb;
    c += Lower.SampleLevel(LinearClamp, uv + texel * float2(0.0, -1.0), 0).rgb * 2.0;
    c += Lower.SampleLevel(LinearClamp, uv + texel * float2(1.0, -1.0), 0).rgb;
    c += Lower.SampleLevel(LinearClamp, uv + texel * float2(-1.0, 0.0), 0).rgb * 2.0;
    c += Lower.SampleLevel(LinearClamp, uv, 0).rgb * 4.0;
    c += Lower.SampleLevel(LinearClamp, uv + texel * float2(1.0, 0.0), 0).rgb * 2.0;
    c += Lower.SampleLevel(LinearClamp, uv + texel * float2(-1.0, 1.0), 0).rgb;
    c += Lower.SampleLevel(LinearClamp, uv + texel * float2(0.0, 1.0), 0).rgb * 2.0;
    c += Lower.SampleLevel(LinearClamp, uv + texel * float2(1.0, 1.0), 0).rgb;
    c *= (1.0 / 16.0);

    float3 same = Same.Load(int3(int2(id.xy), 0)).rgb;
    Output[id.xy] = float4(same + c, 1.0);
}

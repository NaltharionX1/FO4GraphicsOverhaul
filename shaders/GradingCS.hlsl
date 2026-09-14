cbuffer GradingConstants : register(b0)
{
    float g_saturation;
    float g_brightness;
    float g_contrast;
    float g_tintStrength;
    float3 g_tint;
    float g_pad0;
    uint g_width;
    uint g_height;
    uint g_pad1;
    uint g_pad2;
};

Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

static const float3 kLumaWeights = float3(0.2125, 0.7154, 0.0721);

[numthreads(8, 8, 1)]
void CSGrading(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height) {
        return;
    }
    float4 source = SourceColor.Load(int3(int2(id.xy), 0));
    float3 c = source.rgb;

    c *= g_brightness;

    c = (c - 0.5) * g_contrast + 0.5;

    float luma = dot(c, kLumaWeights);
    c = lerp(luma.xxx, c, g_saturation);

    float tintLuma = dot(c, kLumaWeights);
    c = lerp(c, tintLuma * g_tint, g_tintStrength);

    OutputColor[id.xy] = float4(saturate(c), source.a);
}

#ifndef TARGET_COMPONENTS
#define TARGET_COMPONENTS 4
#endif

cbuffer AoIntegrateConstants : register(b0)
{
    uint2 g_targetExtent;
    uint2 g_sourceExtent;
    uint g_blendMode;
    uint g_aoScale;
    uint2 g_pad;
};

Texture2D<uint> g_ourAoTerm : register(t0);
Texture2D<float4> g_existingTarget : register(t1);

#if TARGET_COMPONENTS == 1
#define TARGET_TYPE float
TARGET_TYPE PackTarget(float4 v) { return v.x; }
#elif TARGET_COMPONENTS == 2
#define TARGET_TYPE float2
TARGET_TYPE PackTarget(float4 v) { return v.xy; }
#elif TARGET_COMPONENTS == 3
#define TARGET_TYPE float3
TARGET_TYPE PackTarget(float4 v) { return v.xyz; }
#else
#define TARGET_TYPE float4
TARGET_TYPE PackTarget(float4 v) { return v; }
#endif

RWTexture2D<TARGET_TYPE> g_target : register(u0);

[numthreads(8, 8, 1)]
void CSAoIntegrate(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_targetExtent)) {
        return;
    }
    uint2 sourcePixel = min(id.xy * g_sourceExtent / max(g_targetExtent, uint2(1, 1)),
                            g_sourceExtent - uint2(1, 1));

    uint encoded = g_ourAoTerm.Load(int3(sourcePixel, 0)).x;
    float visibility = saturate((float)encoded / (float)max(g_aoScale, 1u));

    float4 result = visibility.xxxx;
    if (g_blendMode == 1) {
        result = min(g_existingTarget.Load(int3(id.xy, 0)), result);
    }
    g_target[id.xy] = PackTarget(result);
}

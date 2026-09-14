cbuffer DecodeConstants : register(b0)
{
    row_major float4x4 g_invProj;
    float2 g_rcpFrameDim;
    float2 g_frameDim;
    float g_viewZSign;
    float3 g_decodePadding;
};

Texture2D<float> g_srcRawDepth : register(t0);
Texture2D<float2> g_srcRawNormal : register(t1);

RWTexture2D<float> g_outViewDepth : register(u0);
RWTexture2D<uint> g_outViewNormal : register(u1);

float3 DecodeViewNormal(float2 enc)
{
    float2 e = enc * 4.0 - 2.0;
    float e2 = dot(e, e);
    float2 xy = e * sqrt(max(0.0, 1.0 - e2 * 0.25));
    float z = -(1.0 - e2 * 0.5);
    return normalize(float3(xy, z));
}

uint PackNormal(float3 unpacked)
{
    float3 v = saturate(unpacked * 0.5 + 0.5);
    return (uint(v.x * 2047 + 0.5)) | (uint(v.y * 2047 + 0.5) << 11) |
           (uint(v.z * 1023 + 0.5) << 22);
}

[numthreads(8, 8, 1)]
void CSDecodeGBuffer(uint2 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= (uint)g_frameDim.x || dtid.y >= (uint)g_frameDim.y) {
        return;
    }

    int3 px = int3(dtid, 0);
    float rawDepth = g_srcRawDepth.Load(px);
    float2 encodedNormal = g_srcRawNormal.Load(px);

    float viewZ;
    if (rawDepth < 0.01) {
        viewZ = 0.0;
    } else {
        float localZ = (rawDepth - 0.01) / 0.99;
        float2 uv = (float2(dtid) + 0.5) * g_rcpFrameDim;
        float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        float4 h = mul(float4(ndc, localZ, 1.0), g_invProj);
        viewZ = (h.z / h.w) * g_viewZSign;
    }

    g_outViewDepth[dtid] = viewZ;
    g_outViewNormal[dtid] = PackNormal(DecodeViewNormal(encodedNormal));
}

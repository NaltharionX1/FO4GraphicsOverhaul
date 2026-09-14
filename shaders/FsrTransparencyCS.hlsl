// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

Texture2D<float4> OpaqueColor : register(t0);
Texture2D<float4> FinalColor : register(t1);

RWTexture2D<float> TransparencyCompositionMask : register(u0);

cbuffer FsrMaskConstants : register(b0)
{
    uint2 RenderSize;
    float TransparencyScale;
    float Padding;
};

float3 Tonemap(float3 c)
{
    return c / (1.0 + c);
}

[numthreads(8, 8, 1)] void CSFsrTransparency(uint3 dispatchID : SV_DispatchThreadID)
{
    if (any(dispatchID.xy >= RenderSize)) {
        return;
    }

    const float3 o = Tonemap(max(OpaqueColor[dispatchID.xy].rgb, 0.0));
    const float3 f = Tonemap(max(FinalColor[dispatchID.xy].rgb, 0.0));

    const float3 d = abs(f - o);
    const float diff = max(d.r, max(d.g, d.b));

    TransparencyCompositionMask[dispatchID.xy] = saturate(diff * TransparencyScale);
}

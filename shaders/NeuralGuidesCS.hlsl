// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

Texture2D<float>   NeuralDepthSrc : register(t0);
RWTexture2D<float> NeuralDepthDst : register(u0);

[numthreads(8, 8, 1)]
void CSNeuralDepth(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    NeuralDepthDst.GetDimensions(w, h);
    if (id.x < w && id.y < h) {
        NeuralDepthDst[id.xy] = NeuralDepthSrc.Load(int3(id.xy, 0));
    }
}

Texture2D<float4>        UiAlphaBackbuffer : register(t3);
Texture2D<float4>        UiAlphaHudless    : register(t4);
RWTexture2D<unorm float> UiAlphaDst        : register(u3);

[numthreads(8, 8, 1)]
void CSUiAlpha(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    UiAlphaDst.GetDimensions(w, h);
    if (id.x < w && id.y < h) {
        const float3 d = abs(UiAlphaBackbuffer.Load(int3(id.xy, 0)).rgb - UiAlphaHudless.Load(int3(id.xy, 0)).rgb);
        UiAlphaDst[id.xy] = saturate(max(d.r, max(d.g, d.b)) * 8.0);
    }
}

Texture2D<float4>   NeuralColorSrc : register(t2);
RWTexture2D<float4> NeuralColorDst : register(u2);

[numthreads(8, 8, 1)]
void CSNeuralColor(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    NeuralColorDst.GetDimensions(w, h);
    if (id.x < w && id.y < h) {
        NeuralColorDst[id.xy] = NeuralColorSrc.Load(int3(id.xy, 0));
    }
}

Texture2D<float4>        FpMaskPre  : register(t5);
Texture2D<float4>        FpMaskPost : register(t6);
RWTexture2D<unorm float> FpMaskDst  : register(u4);

[numthreads(8, 8, 1)]
void CSFirstPersonMask(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    FpMaskDst.GetDimensions(w, h);
    if (id.x < w && id.y < h) {
        const float3 d = abs(FpMaskPre.Load(int3(id.xy, 0)).rgb - FpMaskPost.Load(int3(id.xy, 0)).rgb);
        FpMaskDst[id.xy] = 1.0 - saturate(max(d.r, max(d.g, d.b)) * 1000.0);
    }
}

Texture2D<float>         FgCondDepthSrc : register(t7);
Texture2D<unorm float>   FgCondMask     : register(t8);
RWTexture2D<float>       FgCondDepthDst : register(u5);

[numthreads(8, 8, 1)]
void CSFgDepthConditioned(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    FgCondDepthDst.GetDimensions(w, h);
    if (id.x < w && id.y < h) {
        const float depth = FgCondDepthSrc.Load(int3(id.xy, 0));
        const float mask = FgCondMask.Load(int3(id.xy, 0));
        FgCondDepthDst[id.xy] = lerp(min(depth, 0.1), depth, mask);
    }
}

Texture2D<float2>        FgCondMotionSrc : register(t9);
RWTexture2D<float2>      FgCondMotionDst : register(u6);

[numthreads(8, 8, 1)]
void CSFgMotionConditioned(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    FgCondMotionDst.GetDimensions(w, h);
    if (id.x < w && id.y < h) {
        const float2 mv = FgCondMotionSrc.Load(int3(id.xy, 0));
        const float mask = FgCondMask.Load(int3(id.xy, 0));
        FgCondMotionDst[id.xy] = lerp(float2(0.0, 0.0), mv, mask);
    }
}

cbuffer DilateParams : register(b0)
{
    float DilateNear;
    float DilateFar;
    float DilateUseNear;
    float DilatePad;
};
Texture2D<float2>        DilateMotionSrc : register(t10);
Texture2D<float>         DilateDepthSrc  : register(t11);
RWTexture2D<float2>      DilateMotionDst : register(u7);

[numthreads(8, 8, 1)]
void CSDilateMotion(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    DilateMotionDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) {
        return;
    }
    const int2 p = int2(id.xy);
    const float depth = DilateDepthSrc.Load(int3(p, 0));
    const float2 own = DilateMotionSrc.Load(int3(p, 0));
    float2 longest = own;
    float longestLen = dot(own, own);
    [unroll]
    for (int dy = -2; dy <= 2; ++dy) {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx) {
            const int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(int(w) - 1, int(h) - 1));
            const float nd = DilateDepthSrc.Load(int3(q, 0));
            const float2 nmv = DilateMotionSrc.Load(int3(q, 0));
            const float nlen = dot(nmv, nmv);
            if (nd < depth && nlen > longestLen) {
                longest = nmv;
                longestLen = nlen;
            }
        }
    }
    float nearFactor = 0.0;
    if (DilateUseNear > 0.5 && DilateFar > DilateNear && DilateNear > 0.0) {
        const float z = DilateNear * DilateFar / max(DilateFar - depth * (DilateFar - DilateNear), 1e-6);
        nearFactor = smoothstep(4096.0 * 2.5, 0.0, z);
    }
    DilateMotionDst[id.xy] = lerp(longest, own, nearFactor);
}

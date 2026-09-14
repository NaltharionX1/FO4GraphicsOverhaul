// SPDX-License-Identifier: GPL-3.0-or-later
// Uses XeGTAO, Copyright (C) 2016-2021 Intel Corporation, MIT License.

#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0

#define XE_GTAO_USE_DEFAULT_CONSTANTS 0

#define XE_GTAO_PREFILTER_SOURCE_IS_VIEWSPACE 1

#define VA_SATURATE saturate

#include "XeGTAO.h"

cbuffer GTAOConstantBuffer : register(b0)
{
    GTAOConstants g_GTAOConsts;
}

cbuffer GtaoExtensionConstants : register(b2)
{
    float g_minScreenRadius;
    float g_depthFadeMul;
    float g_depthFadeAdd;
    float g_extensionPadding;
}

#include "XeGTAO_fxc.hlsli"

SamplerState g_samplerPointClamp : register(s0);

Texture2D<float> g_srcRawDepth : register(t0);
RWTexture2D<lpfloat> g_outWorkingDepthMIP0 : register(u0);
RWTexture2D<lpfloat> g_outWorkingDepthMIP1 : register(u1);
RWTexture2D<lpfloat> g_outWorkingDepthMIP2 : register(u2);
RWTexture2D<lpfloat> g_outWorkingDepthMIP3 : register(u3);
RWTexture2D<lpfloat> g_outWorkingDepthMIP4 : register(u4);

[numthreads(8, 8, 1)]
void CSPrefilterDepths16x16(uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID)
{
    XeGTAO_PrefilterDepths16x16(dispatchThreadID, groupThreadID, g_GTAOConsts, g_srcRawDepth,
        g_samplerPointClamp, g_outWorkingDepthMIP0, g_outWorkingDepthMIP1, g_outWorkingDepthMIP2,
        g_outWorkingDepthMIP3, g_outWorkingDepthMIP4);
}

RWTexture2D<uint> g_outNormalmap : register(u0);

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGenerateNormals(const uint2 pixCoord : SV_DispatchThreadID)
{
    float3 viewspaceNormal = XeGTAO_ComputeViewspaceNormal(pixCoord, g_GTAOConsts, g_srcRawDepth,
        g_samplerPointClamp);
    g_outNormalmap[pixCoord] = XeGTAO_FLOAT3_to_R11G11B10_UNORM(saturate(viewspaceNormal * 0.5 + 0.5));
}

Texture2D<lpfloat> g_srcWorkingDepth : register(t0);
Texture2D<uint> g_srcNormalmap : register(t1);
RWTexture2D<uint> g_outWorkingAOTerm : register(u0);
RWTexture2D<unorm float> g_outWorkingEdges : register(u1);

lpfloat3 LoadNormal(int2 pos)
{
    uint packedInput = g_srcNormalmap.Load(int3(pos, 0)).x;
    float3 unpackedOutput = XeGTAO_R11G11B10_UNORM_to_FLOAT3(packedInput);
    return (lpfloat3)normalize(unpackedOutput * 2.0.xxx - 1.0.xxx);
}

lpfloat2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)
{
    uint index = HilbertIndex(pixCoord.x, pixCoord.y);
    index += 288 * (temporalIndex % 64);
    return lpfloat2(frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114)));
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOLow(const uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 1, 2, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex),
        LoadNormal(pixCoord), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp,
        g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOMedium(const uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 2, 2, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex),
        LoadNormal(pixCoord), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp,
        g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOHigh(const uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 3, 3, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex),
        LoadNormal(pixCoord), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp,
        g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOUltra(const uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 9, 3, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex),
        LoadNormal(pixCoord), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp,
        g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOExtreme(const uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 16, 4, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex),
        LoadNormal(pixCoord), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp,
        g_outWorkingAOTerm, g_outWorkingEdges);
}

Texture2D<uint> g_srcWorkingAOTerm : register(t0);
Texture2D<lpfloat> g_srcWorkingEdges : register(t1);
RWTexture2D<uint> g_outFinalAOTerm : register(u0);

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoisePass(const uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges,
        g_samplerPointClamp, g_outFinalAOTerm, false);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoiseLastPass(const uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges,
        g_samplerPointClamp, g_outFinalAOTerm, true);
}

//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "ObjectVelocity.hlsli"

// SV_Position must stay the same expression DepthViewerVS uses, or the depth-EQUAL test
// against the Z pre-pass fails on the very geometry this pass exists to cover.
VSOutput main(float3 position : POSITION)
{
    VSOutput vsOutput;
    vsOutput.pos = mul(curWVP, float4(position, 1.0));
    vsOutput.curClip = vsOutput.pos;
    vsOutput.prevClip = mul(prevWVP, float4(position, 1.0));
    return vsOutput;
}

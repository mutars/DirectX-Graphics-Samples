//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "ObjectVelocity.hlsli"
#include "PixelPacking_Velocity.hlsli"

uint main(VSOutput input) : SV_Target0
{
    float2 curNdc = input.curClip.xy / input.curClip.w;
    float2 prevNdc = input.prevClip.xy / input.prevClip.w;
    float2 curPixel = (curNdc * float2(0.5, -0.5) + 0.5) * viewportSize.xy;
    float2 prevPixel = (prevNdc * float2(0.5, -0.5) + 0.5) * viewportSize.xy;

    float curZ = input.curClip.z / input.curClip.w;
    float prevZ = input.prevClip.z / input.prevClip.w;
    if (zParams.y > 0.5)
    {
        curZ = 1.0 / (zParams.x * curZ + 1.0);
        prevZ = 1.0 / (zParams.x * prevZ + 1.0);
    }

    return PackVelocity(float3(prevPixel - curPixel, prevZ - curZ));
}

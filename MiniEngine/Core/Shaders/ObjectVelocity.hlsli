//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#ifndef __OBJECT_VELOCITY_HLSLI__
#define __OBJECT_VELOCITY_HLSLI__

cbuffer ObjectVelocity : register(b0)
{
    float4x4 curWVP;
    float4x4 prevWVP;
    float4 viewportSize;    // width, height, 1/width, 1/height
    float4 zParams;         // (zFar - zNear) / zNear, useLinearZ, 0, 0
};

struct VSOutput
{
    float4 pos : SV_Position;
    float4 curClip : TexCoord0;
    float4 prevClip : TexCoord1;
};

#endif // __OBJECT_VELOCITY_HLSLI__

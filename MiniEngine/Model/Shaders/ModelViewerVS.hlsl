//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author(s):  James Stanard
//             Alex Nankervis
//

#include "Common.hlsli"

cbuffer VSConstants : register(b0)
{
    float4x4 modelToProjection;
    float4x4 modelToShadow;
    float3 ViewerPos;
    float4x4 modelToWorld;
};

cbuffer StartVertex : register(b1)
{
    uint materialIdx;
};

struct VSInput
{
    float3 position : POSITION;
    float2 texcoord0 : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : WorldPos;
    float2 texCoord : TexCoord0;
    float3 viewDir : TexCoord1;
    float3 shadowCoord : TexCoord2;
    float3 normal : Normal;
    float3 tangent : Tangent;
    float3 bitangent : Bitangent;
#if ENABLE_TRIANGLE_ID
    uint vertexID : TexCoord3;
#endif
};

[RootSignature(Renderer_RootSig)]
VSOutput main(VSInput vsInput, uint vertexID : SV_VertexID)
{
    VSOutput vsOutput;

    // modelToShadow already carries the model transform, so shadowCoord stays on object positions.
    float3 worldPos = mul(modelToWorld, float4(vsInput.position, 1.0)).xyz;

    vsOutput.position = mul(modelToProjection, float4(vsInput.position, 1.0));
    vsOutput.worldPos = worldPos;
    vsOutput.texCoord = vsInput.texcoord0;
    vsOutput.viewDir = worldPos - ViewerPos;
    vsOutput.shadowCoord = mul(modelToShadow, float4(vsInput.position, 1.0)).xyz;

    // Rigid transforms only -- no inverse transpose needed.
    vsOutput.normal = mul((float3x3)modelToWorld, vsInput.normal);
    vsOutput.tangent = mul((float3x3)modelToWorld, vsInput.tangent);
    vsOutput.bitangent = mul((float3x3)modelToWorld, vsInput.bitangent);

#if ENABLE_TRIANGLE_ID
    vsOutput.vertexID = materialIdx << 24 | (vertexID & 0xFFFF);
#endif

    return vsOutput;
}

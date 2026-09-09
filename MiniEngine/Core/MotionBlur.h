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
// Author:  James Stanard 
//

#pragma once

#include "EngineTuning.h"
#include "VectorMath.h"

#include <cstdint>
#include <d3d12.h>

// Forward declarations
namespace Math { class Camera; }
class ColorBuffer;
class CommandContext;

namespace MotionBlur
{
    extern BoolVar Enable;

    // One DrawIndexed against the geometry buffers carried by VelocityGeometry.
    struct VelocityRange
    {
        uint32_t indexCount;
        uint32_t startIndex;
        int32_t baseVertex;
    };

    struct VelocityObject
    {
        Math::Matrix4 world;
        Math::Matrix4 prevWorld;
        const VelocityRange* ranges;
        uint32_t rangeCount;
    };

    // POSITION must be a float3 at vertex offset 0; one vertex/index buffer pair is shared by
    // every object, which is why the ranges carry the per-object baseVertex/startIndex.
    struct VelocityGeometry
    {
        D3D12_VERTEX_BUFFER_VIEW vertexBuffer;
        D3D12_INDEX_BUFFER_VIEW indexBuffer;
        const VelocityObject* objects;
        uint32_t objectCount;
    };

    void Initialize( void );
    void Shutdown( void );

    void GenerateCameraVelocityBuffer( CommandContext& Context, const Math::Camera& camera, bool UseLinearZ = true );
    void GenerateCameraVelocityBuffer( CommandContext& Context, const Math::Matrix4& reprojectionMatrix, float nearClip, float farClip, bool UseLinearZ = true);
    void GenerateCameraVelocityBuffer( CommandContext& Context, const Math::Camera& camera, bool UseLinearZ,
        const VelocityGeometry& dynamic, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor );

    // Generate motion blur only associated with the camera.  Does not handle fast-moving objects well, but
    // does not require a full screen velocity buffer.
    void RenderCameraBlur( CommandContext& Context, const Math::Camera& camera, bool UseLinearZ = true );
    void RenderCameraBlur( CommandContext& Context, const Math::Matrix4& reprojectionMatrix, float nearClip, float farClip, bool UseLinearZ = true);

    // Generate proper motion blur that takes into account the velocity of each pixel.  Requires a pre-generated
    // velocity buffer (R16G16_FLOAT preferred.)
    void RenderObjectBlur( CommandContext& Context, ColorBuffer& velocityBuffer );
}

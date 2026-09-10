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
//

#pragma once

#include <d3d12.h>
#include <cstdint>

#include "../Core/MotionBlur.h"
#include "../Core/Math/BoundingBox.h"

class GraphicsContext;
class ShadowCamera;
class ModelH3D;
class ExpVar;

namespace Math
{
    class Camera;
}

namespace Sponza
{
    void Startup( Math::Camera& camera );
    void Update( float deltaT );
    void Cleanup( void );

    // Restart the animation clock at the t=0 pose with a settled history, so callers whose warm-up
    // length differs can measure at the same orbit phase. The first tick after it carries zero
    // object motion and the second zero two-frame motion.
    void ResetSceneTime( void );

    void RenderScene(
        GraphicsContext& gfxContext,
        const Math::Camera& camera,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissor,
        bool skipDiffusePass = false,
        bool skipShadowMap = false );

    const ModelH3D& GetModel();

    MotionBlur::VelocityGeometry DynamicGeometry();

    struct DynamicObjectView
    {
        const char* name;
        Math::Matrix4 world;
        Math::AxisAlignedBox bounds;
    };

    uint32_t DynamicObjectCount();
    DynamicObjectView DynamicObject( uint32_t index );

    extern Math::Vector3 m_SunDirection;
    extern ShadowCamera m_SunShadow;
    extern ExpVar m_AmbientIntensity;
    extern ExpVar m_SunLightIntensity;

}

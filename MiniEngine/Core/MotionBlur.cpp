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

#include "pch.h"
#include "MotionBlur.h"
#include "Camera.h"
#include "BufferManager.h"
#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "CommandContext.h"
#include "Camera.h"
#include "TemporalEffects.h"
#include "PostEffects.h"
#include "SystemTime.h"
#include "RootSignature.h"
#include "PipelineState.h"

#include "CompiledShaders/ScreenQuadCommonVS.h"
#include "CompiledShaders/CameraMotionBlurPrePassCS.h"
#include "CompiledShaders/CameraMotionBlurPrePassLinearZCS.h"
#include "CompiledShaders/MotionBlurPrePassCS.h"
#include "CompiledShaders/MotionBlurFinalPassCS.h"
#include "CompiledShaders/MotionBlurFinalPassPS.h"
#include "CompiledShaders/CameraVelocityCS.h"
#include "CompiledShaders/TemporalBlendCS.h"
#include "CompiledShaders/BoundNeighborhoodCS.h"
#include "CompiledShaders/ObjectVelocityVS.h"
#include "CompiledShaders/ObjectVelocityPS.h"
#include "CompiledShaders/DlssMotionVectorsCS.h"

using namespace Graphics;
using namespace Math;

namespace MotionBlur
{
    BoolVar Enable("Graphics/Motion Blur/Enable", false);

    ComputePSO s_CameraMotionBlurPrePassCS[2] = { {L"Motion Blur: Camera Motion Blur Pre-Pass CS"}, { L"Motion Blur: Camera Motion Blur Pre-Pass Linear Z CS" } };
    ComputePSO s_MotionBlurPrePassCS(L"Motion Blur: Motion Blur Pre-Pass CS");
    ComputePSO s_MotionBlurFinalPassCS(L"Motion Blur: Motion Blur Final Pass CS");
    GraphicsPSO s_MotionBlurFinalPassPS(L"Motion Blur: Motion Blur Final Pass PS");
    ComputePSO s_CameraVelocityCS[2] = { { L"Motion Blur: Camera Velocity CS" },{ L"Motion Blur: Camera Velocity Linear Z CS" } };

    RootSignature s_ObjectVelocityRS;
    GraphicsPSO s_ObjectVelocityPSO(L"Motion Blur: Object Velocity PSO");

    ComputePSO s_DlssMotionVectorsCS(L"DLSS: Motion Vectors CS");

    // alignas(16): ComputeContext::SetDynamicConstantBufferView ASSERTs the source pointer is
    // 16-byte aligned (root CBV requirement). An all-float struct is only 4-byte aligned on the stack.
    struct alignas(16) DlssMVCB
    {
        float JitterDeltaX;
        float JitterDeltaY;
        float _pad[2];
    };
}

void MotionBlur::Initialize( void )
{
#define CreatePSO( ObjName, ShaderByteCode ) \
    ObjName.SetRootSignature(g_CommonRS); \
    ObjName.SetComputeShader(ShaderByteCode, sizeof(ShaderByteCode) ); \
    ObjName.Finalize();

    if (g_bTypedUAVLoadSupport_R11G11B10_FLOAT)
    {
        CreatePSO(s_MotionBlurFinalPassCS, g_pMotionBlurFinalPassCS);
    }
    else
    {
        s_MotionBlurFinalPassPS.SetRootSignature(g_CommonRS);
        s_MotionBlurFinalPassPS.SetRasterizerState( RasterizerTwoSided );
        s_MotionBlurFinalPassPS.SetBlendState( BlendPreMultiplied );
        s_MotionBlurFinalPassPS.SetDepthStencilState( DepthStateDisabled );
        s_MotionBlurFinalPassPS.SetSampleMask(0xFFFFFFFF);
        s_MotionBlurFinalPassPS.SetInputLayout(0, nullptr);
        s_MotionBlurFinalPassPS.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        s_MotionBlurFinalPassPS.SetVertexShader( g_pScreenQuadCommonVS, sizeof(g_pScreenQuadCommonVS) );
        s_MotionBlurFinalPassPS.SetPixelShader( g_pMotionBlurFinalPassPS, sizeof(g_pMotionBlurFinalPassPS) );
        s_MotionBlurFinalPassPS.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
        s_MotionBlurFinalPassPS.Finalize();

    }
    CreatePSO( s_CameraMotionBlurPrePassCS[0], g_pCameraMotionBlurPrePassCS );
    CreatePSO( s_CameraMotionBlurPrePassCS[1], g_pCameraMotionBlurPrePassLinearZCS );
    CreatePSO( s_MotionBlurPrePassCS, g_pMotionBlurPrePassCS );
    CreatePSO( s_CameraVelocityCS[0], g_pCameraVelocityCS );
    CreatePSO( s_CameraVelocityCS[1], g_pCameraVelocityCS );
    CreatePSO( s_DlssMotionVectorsCS, g_pDlssMotionVectorsCS );

#undef CreatePSO

    s_ObjectVelocityRS.Reset(1, 0);
    s_ObjectVelocityRS[0].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_ALL);
    s_ObjectVelocityRS.Finalize(L"Motion Blur: Object Velocity", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    const D3D12_INPUT_ELEMENT_DESC objectVelocityVertElem[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Two-sided to match the cutout Z pre-pass, which is the only reason a back face can own a
    // depth value this pass has to test EQUAL against.
    s_ObjectVelocityPSO.SetRootSignature(s_ObjectVelocityRS);
    s_ObjectVelocityPSO.SetRasterizerState(RasterizerTwoSided);
    s_ObjectVelocityPSO.SetBlendState(BlendDisable);
    s_ObjectVelocityPSO.SetDepthStencilState(DepthStateTestEqual);
    s_ObjectVelocityPSO.SetInputLayout(_countof(objectVelocityVertElem), objectVelocityVertElem);
    s_ObjectVelocityPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    s_ObjectVelocityPSO.SetRenderTargetFormat(g_VelocityBuffer.GetFormat(), g_SceneDepthBuffer.GetFormat());
    s_ObjectVelocityPSO.SetVertexShader(g_pObjectVelocityVS, sizeof(g_pObjectVelocityVS));
    s_ObjectVelocityPSO.SetPixelShader(g_pObjectVelocityPS, sizeof(g_pObjectVelocityPS));
    s_ObjectVelocityPSO.Finalize();
}

void MotionBlur::Shutdown( void )
{
}

// Linear Z ends up being faster since we haven't officially decompressed the depth buffer.  You 
// would think that it might be slower to use linear Z because we have to convert it back to
// hyperbolic Z for the reprojection.  Nevertheless, the reduced bandwidth and decompress eliminate
// make Linear Z the better choice.  (The choice also lets you evict the depth buffer from ESRAM.)

namespace MotionBlur
{
    namespace
    {
        enum class VelocityHistory { PreviousFrame, TwoFramesBack };

        void DispatchCameraVelocity( CommandContext& BaseContext, const Matrix4& reprojectionMatrix, float nearClip, float farClip,
            bool UseLinearZ, ColorBuffer& target )
        {
            ComputeContext& Context = BaseContext.GetComputeContext();

            Context.SetRootSignature(g_CommonRS);

            uint32_t Width = g_SceneColorBuffer.GetWidth();
            uint32_t Height = g_SceneColorBuffer.GetHeight();

            float RcpHalfDimX = 2.0f / Width;
            float RcpHalfDimY = 2.0f / Height;
            float RcpZMagic = nearClip / (farClip - nearClip);

            Matrix4 preMult = Matrix4(
                Vector4( RcpHalfDimX, 0.0f, 0.0f, 0.0f ),
                Vector4( 0.0f, -RcpHalfDimY, 0.0f, 0.0f),
                Vector4( 0.0f, 0.0f, UseLinearZ ? RcpZMagic : 1.0f, 0.0f ),
                Vector4( -1.0f, 1.0f, UseLinearZ ? -RcpZMagic : 0.0f, 1.0f )
            );

            Matrix4 postMult = Matrix4(
                Vector4( 1.0f / RcpHalfDimX, 0.0f, 0.0f, 0.0f ),
                Vector4( 0.0f, -1.0f / RcpHalfDimY, 0.0f, 0.0f ),
                Vector4( 0.0f, 0.0f, 1.0f, 0.0f ),
                Vector4( 1.0f / RcpHalfDimX, 1.0f / RcpHalfDimY, 0.0f, 1.0f ) );


            Matrix4 CurToPrevXForm = postMult * reprojectionMatrix * preMult;

            Context.SetDynamicConstantBufferView(3, sizeof(CurToPrevXForm), &CurToPrevXForm);
            Context.TransitionResource(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            ColorBuffer& LinearDepth = g_LinearDepth[ TemporalEffects::GetFrameIndexMod2() ];
            if (UseLinearZ)
                Context.TransitionResource(LinearDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            else
                Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            Context.SetPipelineState(s_CameraVelocityCS[UseLinearZ ? 1 : 0]);
            Context.SetDynamicDescriptor(1, 0, UseLinearZ ? LinearDepth.GetSRV() : g_SceneDepthBuffer.GetDepthSRV());
            Context.SetDynamicDescriptor(2, 0, target.GetUAV());
            Context.Dispatch2D(Width, Height);
        }

        void RenderObjectVelocity( CommandContext& BaseContext, const Camera& camera, bool UseLinearZ,
            const VelocityGeometry& dynamic, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor,
            ColorBuffer& target, VelocityHistory history )
        {
            GraphicsContext& Context = BaseContext.GetGraphicsContext();

            Context.TransitionResource(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
            Context.SetRenderTarget(target.GetRTV(), g_SceneDepthBuffer.GetDSV_DepthReadOnly());
            Context.SetViewportAndScissor(viewport, scissor);

            Context.SetRootSignature(s_ObjectVelocityRS);
            Context.SetPipelineState(s_ObjectVelocityPSO);
            Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            Context.SetVertexBuffer(0, dynamic.vertexBuffer);
            Context.SetIndexBuffer(dynamic.indexBuffer);

            __declspec(align(16)) struct
            {
                Matrix4 curWVP;
                Matrix4 prevWVP;
                float viewportSize[4];
                float zParams[4];
            } vsConstants;

            const float Width = (float)g_SceneColorBuffer.GetWidth();
            const float Height = (float)g_SceneColorBuffer.GetHeight();
            vsConstants.viewportSize[0] = Width;
            vsConstants.viewportSize[1] = Height;
            vsConstants.viewportSize[2] = 1.0f / Width;
            vsConstants.viewportSize[3] = 1.0f / Height;
            vsConstants.zParams[0] = (camera.GetFarClip() - camera.GetNearClip()) / camera.GetNearClip();
            vsConstants.zParams[1] = UseLinearZ ? 1.0f : 0.0f;
            vsConstants.zParams[2] = 0.0f;
            vsConstants.zParams[3] = 0.0f;

            const bool twoFrame = history == VelocityHistory::TwoFramesBack;
            const Matrix4& prevViewProj = twoFrame ? camera.GetPrevPrevViewProjMatrix() : camera.GetPreviousViewProjMatrix();

            for (uint32_t objectIndex = 0; objectIndex < dynamic.objectCount; ++objectIndex)
            {
                const VelocityObject& object = dynamic.objects[objectIndex];

                vsConstants.curWVP = camera.GetViewProjMatrix() * object.world;
                vsConstants.prevWVP = prevViewProj * (twoFrame ? object.prevPrevWorld : object.prevWorld);
                Context.SetDynamicConstantBufferView(0, sizeof(vsConstants), &vsConstants);

                for (uint32_t rangeIndex = 0; rangeIndex < object.rangeCount; ++rangeIndex)
                {
                    const VelocityRange& range = object.ranges[rangeIndex];
                    Context.DrawIndexed(range.indexCount, range.startIndex, range.baseVertex);
                }
            }
        }
    }
}

void MotionBlur::GenerateCameraVelocityBuffer( CommandContext& BaseContext, const Camera& camera, bool UseLinearZ )
{
    GenerateCameraVelocityBuffer(BaseContext, camera.GetReprojectionMatrix(), camera.GetNearClip(), camera.GetFarClip(), UseLinearZ);
}

void MotionBlur::GenerateCameraVelocityBuffer( CommandContext& BaseContext, const Matrix4& reprojectionMatrix, float nearClip, float farClip, bool UseLinearZ)
{
    ScopedTimer _prof(L"Generate Camera Velocity", BaseContext);

    DispatchCameraVelocity(BaseContext, reprojectionMatrix, nearClip, farClip, UseLinearZ, g_VelocityBuffer);
}

void MotionBlur::GenerateCameraVelocityBuffer( CommandContext& BaseContext, const Camera& camera, bool UseLinearZ,
    const VelocityGeometry& dynamic, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor )
{
    GenerateCameraVelocityBuffer(BaseContext, camera, UseLinearZ);

    if (dynamic.objectCount == 0)
        return;

    RenderObjectVelocity(BaseContext, camera, UseLinearZ, dynamic, viewport, scissor,
        g_VelocityBuffer, VelocityHistory::PreviousFrame);
}

void MotionBlur::GenerateTwoFrameVelocityBuffer( CommandContext& BaseContext, const Camera& camera, bool UseLinearZ,
    const VelocityGeometry& dynamic, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor )
{
    ScopedTimer _prof(L"Generate Two-Frame Velocity", BaseContext);

    DispatchCameraVelocity(BaseContext, camera.GetReprojectionMatrix2(), camera.GetNearClip(), camera.GetFarClip(),
        UseLinearZ, g_VelocityBuffer2);

    if (dynamic.objectCount != 0)
        RenderObjectVelocity(BaseContext, camera, UseLinearZ, dynamic, viewport, scissor,
            g_VelocityBuffer2, VelocityHistory::TwoFramesBack);

    RepackMotionVectors(BaseContext, g_VelocityBuffer2, g_TwoFrameMotionBuffer, 0.0f, 0.0f);
}

void MotionBlur::RepackMotionVectors( CommandContext& BaseContext, ColorBuffer& src, ColorBuffer& dst, float jitterDeltaX, float jitterDeltaY )
{
    ComputeContext& Context = BaseContext.GetComputeContext();

    Context.SetRootSignature(g_CommonRS);
    Context.SetPipelineState(s_DlssMotionVectorsCS);

    DlssMVCB cb{ jitterDeltaX, jitterDeltaY, { 0.0f, 0.0f } };
    Context.SetDynamicConstantBufferView(3, sizeof(cb), &cb);

    Context.TransitionResource(src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Context.FlushResourceBarriers();

    // g_CommonRS: slot 1 = SRV (t0-t9), slot 2 = UAV (u0-u9)
    Context.SetDynamicDescriptor(1, 0, src.GetSRV());
    Context.SetDynamicDescriptor(2, 0, dst.GetUAV());

    Context.Dispatch2D(src.GetWidth(), src.GetHeight());

    Context.TransitionResource(dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}


void MotionBlur::RenderCameraBlur( CommandContext& BaseContext, const Camera& camera, bool UseLinearZ )
{
    RenderCameraBlur(BaseContext, camera.GetReprojectionMatrix(), camera.GetNearClip(), camera.GetFarClip(), UseLinearZ);
}

void MotionBlur::RenderCameraBlur( CommandContext& BaseContext, const Matrix4& reprojectionMatrix, float nearClip, float farClip, bool UseLinearZ)
{
    ScopedTimer _prof(L"MotionBlur", BaseContext);

    if (!Enable)
        return;

    ComputeContext& Context = BaseContext.GetComputeContext();

    Context.SetRootSignature(g_CommonRS);

    uint32_t Width = g_SceneColorBuffer.GetWidth();
    uint32_t Height = g_SceneColorBuffer.GetHeight();

    float RcpHalfDimX = 2.0f / Width;
    float RcpHalfDimY = 2.0f / Height;
    float RcpZMagic = nearClip / (farClip - nearClip);

    Matrix4 preMult = Matrix4(
        Vector4( RcpHalfDimX, 0.0f, 0.0f, 0.0f ),
        Vector4( 0.0f, -RcpHalfDimY, 0.0f, 0.0f),
        Vector4( 0.0f, 0.0f, UseLinearZ ? RcpZMagic : 1.0f, 0.0f ),
        Vector4( -1.0f, 1.0f, UseLinearZ ? -RcpZMagic : 0.0f, 1.0f )
    );

    Matrix4 postMult = Matrix4(
        Vector4( 1.0f / RcpHalfDimX, 0.0f, 0.0f, 0.0f ),
        Vector4( 0.0f, -1.0f / RcpHalfDimY, 0.0f, 0.0f ),
        Vector4( 0.0f, 0.0f, 1.0f, 0.0f ),
        Vector4( 1.0f / RcpHalfDimX, 1.0f / RcpHalfDimY, 0.0f, 1.0f ) );

    Matrix4 CurToPrevXForm = postMult * reprojectionMatrix * preMult;

    Context.SetDynamicConstantBufferView(3, sizeof(CurToPrevXForm), &CurToPrevXForm);

    ColorBuffer& LinearDepth = g_LinearDepth[ TemporalEffects::GetFrameIndexMod2() ];
    if (UseLinearZ)
        Context.TransitionResource(LinearDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    else
        Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (Enable)
    {
        Context.TransitionResource(g_VelocityBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Context.TransitionResource(g_MotionPrepBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        Context.SetPipelineState(s_CameraMotionBlurPrePassCS[UseLinearZ ? 1 : 0]);
        Context.SetDynamicDescriptor(1, 0, g_SceneColorBuffer.GetSRV());
        Context.SetDynamicDescriptor(1, 1, UseLinearZ ? LinearDepth.GetSRV() : g_SceneDepthBuffer.GetDepthSRV());
        Context.SetDynamicDescriptor(2, 0, g_MotionPrepBuffer.GetUAV());
        Context.SetDynamicDescriptor(2, 1, g_VelocityBuffer.GetUAV());
        Context.Dispatch2D(g_MotionPrepBuffer.GetWidth(), g_MotionPrepBuffer.GetHeight());

        if (g_bTypedUAVLoadSupport_R11G11B10_FLOAT)
        {
            Context.SetPipelineState(s_MotionBlurFinalPassCS);
            Context.SetConstants(0, 1.0f / Width, 1.0f / Height);

            Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Context.TransitionResource(g_VelocityBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Context.TransitionResource(g_MotionPrepBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Context.SetDynamicDescriptor(2, 0, g_SceneColorBuffer.GetUAV());
            Context.SetDynamicDescriptor(1, 0, g_VelocityBuffer.GetSRV());
            Context.SetDynamicDescriptor(1, 1, g_MotionPrepBuffer.GetSRV());

            Context.Dispatch2D(Width, Height);

            Context.InsertUAVBarrier(g_SceneColorBuffer);
        }
        else
        {
            GraphicsContext& GrContext = BaseContext.GetGraphicsContext();
            GrContext.SetRootSignature(g_CommonRS);
            GrContext.SetPipelineState(s_MotionBlurFinalPassPS);
            GrContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
            GrContext.TransitionResource(g_VelocityBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            GrContext.TransitionResource(g_MotionPrepBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            GrContext.SetDynamicDescriptor(1, 0, g_VelocityBuffer.GetSRV());
            GrContext.SetDynamicDescriptor(1, 1, g_MotionPrepBuffer.GetSRV());
            GrContext.SetConstants(0, 1.0f / Width, 1.0f / Height);
            GrContext.SetRenderTarget(g_SceneColorBuffer.GetRTV());
            GrContext.SetViewportAndScissor(0, 0, Width, Height);
            GrContext.Draw(3);
        }
    }
    else
    {
        Context.SetPipelineState(s_CameraVelocityCS[UseLinearZ ? 1 : 0]);
        Context.SetDynamicDescriptor(1, 0, UseLinearZ ? LinearDepth.GetSRV() : g_SceneDepthBuffer.GetDepthSRV());
        Context.SetDynamicDescriptor(2, 0, g_VelocityBuffer.GetUAV());
        Context.Dispatch2D(Width, Height);
    }
}

void MotionBlur::RenderObjectBlur( CommandContext& BaseContext, ColorBuffer& velocityBuffer )
{
    ScopedTimer _prof(L"MotionBlur", BaseContext);

    if (!Enable)
        return;

    uint32_t Width = g_SceneColorBuffer.GetWidth();
    uint32_t Height = g_SceneColorBuffer.GetHeight();

    ComputeContext& Context = BaseContext.GetComputeContext();

    Context.SetRootSignature(g_CommonRS);

    Context.TransitionResource(g_MotionPrepBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(velocityBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Context.SetDynamicDescriptor(2, 0, g_MotionPrepBuffer.GetUAV());
    Context.SetDynamicDescriptor(1, 0, g_SceneColorBuffer.GetSRV());
    Context.SetDynamicDescriptor(1, 1, velocityBuffer.GetSRV());

    Context.SetPipelineState(s_MotionBlurPrePassCS);
    Context.Dispatch2D(g_MotionPrepBuffer.GetWidth(), g_MotionPrepBuffer.GetHeight());

    if (g_bTypedUAVLoadSupport_R11G11B10_FLOAT)
    {
        Context.SetPipelineState(s_MotionBlurFinalPassCS);

        Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Context.TransitionResource(velocityBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(g_MotionPrepBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        Context.SetDynamicDescriptor(2, 0, g_SceneColorBuffer.GetUAV());
        Context.SetDynamicDescriptor(1, 0, velocityBuffer.GetSRV());
        Context.SetDynamicDescriptor(1, 1, g_MotionPrepBuffer.GetSRV());
        Context.SetConstants(0, 1.0f / Width, 1.0f / Height);

        Context.Dispatch2D(Width, Height);

        Context.InsertUAVBarrier(g_SceneColorBuffer);
    }
    else
    {
        GraphicsContext& GrContext = BaseContext.GetGraphicsContext();
        GrContext.SetRootSignature(g_CommonRS);
        GrContext.SetPipelineState(s_MotionBlurFinalPassPS);

        GrContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
        GrContext.TransitionResource(velocityBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        GrContext.TransitionResource(g_MotionPrepBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        GrContext.SetDynamicDescriptor(1, 0, velocityBuffer.GetSRV());
        GrContext.SetDynamicDescriptor(1, 1, g_MotionPrepBuffer.GetSRV());
        GrContext.SetConstants(0, 1.0f / Width, 1.0f / Height);
        GrContext.SetRenderTarget(g_SceneColorBuffer.GetRTV());
        GrContext.SetViewportAndScissor(0, 0, Width, Height);

        GrContext.Draw(3);
    }
}


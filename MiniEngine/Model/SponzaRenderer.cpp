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
// Author(s):  Alex Nankervis
//             James Stanard
//

// From Core
#include "GraphicsCore.h"
#include "BufferManager.h"
#include "Camera.h"
#include "CommandContext.h"
#include "TemporalEffects.h"
#include "SSAO.h"
#include "SystemTime.h"
#include "ShadowCamera.h"
#include "ParticleEffects.h"
#include "SponzaRenderer.h"
#include "Renderer.h"

// From Model
#include "ModelH3D.h"

// From ModelViewer
#include "LightManager.h"

#include "CompiledShaders/DepthViewerVS.h"
#include "CompiledShaders/DepthViewerPS.h"
#include "CompiledShaders/ModelViewerVS.h"
#include "CompiledShaders/ModelViewerPS.h"

#include <cmath>
#include <cstdlib>
#include <unordered_map>

using namespace Math;
using namespace Graphics;

namespace Sponza
{
    void RenderLightShadows(GraphicsContext& gfxContext, const Camera& camera);

    enum eObjectFilter { kOpaque = 0x1, kCutout = 0x2, kTransparent = 0x4, kAll = 0xF, kNone = 0x0 };
    void RenderObjects( GraphicsContext& Context, const Matrix4& ViewProjMat, const Vector3& viewerPos, eObjectFilter Filter = kAll );

    void BuildDynamicObjects( void );
    void PoseDynamicObjects( void );
    void MirrorVelocityObjects( void );

    enum eDynamicObject { kVaseSpinning = 0, kVaseSteady, kVaseOccluded, kPlanterOscillating, kDynamicObjectCount };

    struct DynamicObjectState
    {
        const char* name = nullptr;
        AxisAlignedBox selection;
        Vector3 pivot = Vector3(kZero);
        Vector3 placement = Vector3(kZero);
        AxisAlignedBox bounds;
        std::vector<MotionBlur::VelocityRange> ranges;
        Matrix4 world = Matrix4(kIdentity);
        Matrix4 prevWorld = Matrix4(kIdentity);
        Matrix4 prevPrevWorld = Matrix4(kIdentity);
    };

    // A contiguous slice of one mesh's index range that belongs to a dynamic object; the rest of
    // the mesh keeps drawing with the identity model matrix.
    struct MeshSplit
    {
        uint32_t startIndex;
        uint32_t indexCount;
        uint32_t objectIndex;
    };

    std::vector<DynamicObjectState> m_DynamicObjects;
    std::vector<std::vector<MeshSplit>> m_MeshSplits;
    std::vector<MotionBlur::VelocityObject> m_VelocityObjects;
    float m_SceneTime = 0.0f;

    // Constant-speed shapes throughout: a sinusoid would pass through zero once a cycle, and no
    // per-tick speed floor can hold across that stall.
    const float kVaseSpinRate = 1.0f;
    const float kVaseSlideRadius = 3.0f;
    const float kVaseSlideRate = 3.0f;
    const float kSteadyVaseRadius = 40.0f;
    const float kSteadyVaseRate = -1.0f;
    const float kOccludedVaseRadius = 30.0f;
    const float kOccludedVaseRate = 1.5f;

    // The planter is a slow pendulum, one swing per 300 fixed 1/90 s ticks. SINE, so it starts at
    // full speed through the rest pose and no measured tick sits near a turning point.
    const float kPlanterSwingAmplitude = 0.6f;
    const float kPlanterSwingRate = 2.0f * 3.14159265f * 90.0f / 300.0f;

    GraphicsPSO m_DepthPSO = { (L"Sponza: Depth PSO") };
    GraphicsPSO m_CutoutDepthPSO = { (L"Sponza: Cutout Depth PSO") };
    GraphicsPSO m_ModelPSO = { (L"Sponza: Color PSO") };
    GraphicsPSO m_CutoutModelPSO = { (L"Sponza: Cutout Color PSO") };
    GraphicsPSO m_ShadowPSO(L"Sponza: Shadow PSO");
    GraphicsPSO m_CutoutShadowPSO(L"Sponza: Cutout Shadow PSO");

    ModelH3D m_Model;
    std::vector<bool> m_pMaterialIsCutout;

    Vector3 m_SunDirection;
    ShadowCamera m_SunShadow;

    ExpVar m_AmbientIntensity("Sponza/Lighting/Ambient Intensity", 0.1f, -16.0f, 16.0f, 0.1f);
    ExpVar m_SunLightIntensity("Sponza/Lighting/Sun Light Intensity", 4.0f, 0.0f, 16.0f, 0.1f);
    NumVar m_SunOrientation("Sponza/Lighting/Sun Orientation", -0.5f, -100.0f, 100.0f, 0.1f );
    NumVar m_SunInclination("Sponza/Lighting/Sun Inclination", 0.75f, 0.0f, 1.0f, 0.01f );
    NumVar ShadowDimX("Sponza/Lighting/Shadow Dim X", 5000, 1000, 10000, 100 );
    NumVar ShadowDimY("Sponza/Lighting/Shadow Dim Y", 3000, 1000, 10000, 100 );
    NumVar ShadowDimZ("Sponza/Lighting/Shadow Dim Z", 3000, 1000, 10000, 100 );
}

void Sponza::Startup( Camera& Camera )
{
    DXGI_FORMAT ColorFormat = g_SceneColorBuffer.GetFormat();
    DXGI_FORMAT NormalFormat = g_SceneNormalBuffer.GetFormat();
    DXGI_FORMAT DepthFormat = g_SceneDepthBuffer.GetFormat();
    //DXGI_FORMAT ShadowFormat = g_ShadowBuffer.GetFormat();

    D3D12_INPUT_ELEMENT_DESC vertElem[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Depth-only (2x rate)
    m_DepthPSO.SetRootSignature(Renderer::m_RootSig);
    m_DepthPSO.SetRasterizerState(RasterizerDefault);
    m_DepthPSO.SetBlendState(BlendNoColorWrite);
    m_DepthPSO.SetDepthStencilState(DepthStateReadWrite);
    m_DepthPSO.SetInputLayout(_countof(vertElem), vertElem);
    m_DepthPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_DepthPSO.SetRenderTargetFormats(0, nullptr, DepthFormat);
    m_DepthPSO.SetVertexShader(g_pDepthViewerVS, sizeof(g_pDepthViewerVS));
    m_DepthPSO.Finalize();

    // Depth-only shading but with alpha testing
    m_CutoutDepthPSO = m_DepthPSO;
    m_CutoutDepthPSO.SetPixelShader(g_pDepthViewerPS, sizeof(g_pDepthViewerPS));
    m_CutoutDepthPSO.SetRasterizerState(RasterizerTwoSided);
    m_CutoutDepthPSO.Finalize();

    // Depth-only but with a depth bias and/or render only backfaces
    m_ShadowPSO = m_DepthPSO;
    m_ShadowPSO.SetRasterizerState(RasterizerShadow);
    m_ShadowPSO.SetRenderTargetFormats(0, nullptr, g_ShadowBuffer.GetFormat());
    m_ShadowPSO.Finalize();

    // Shadows with alpha testing
    m_CutoutShadowPSO = m_ShadowPSO;
    m_CutoutShadowPSO.SetPixelShader(g_pDepthViewerPS, sizeof(g_pDepthViewerPS));
    m_CutoutShadowPSO.SetRasterizerState(RasterizerShadowTwoSided);
    m_CutoutShadowPSO.Finalize();

    DXGI_FORMAT formats[2] = { ColorFormat, NormalFormat };

    // Full color pass
    m_ModelPSO = m_DepthPSO;
    m_ModelPSO.SetBlendState(BlendDisable);
    m_ModelPSO.SetDepthStencilState(DepthStateTestEqual);
    m_ModelPSO.SetRenderTargetFormats(2, formats, DepthFormat);
    m_ModelPSO.SetVertexShader( g_pModelViewerVS, sizeof(g_pModelViewerVS) );
    m_ModelPSO.SetPixelShader( g_pModelViewerPS, sizeof(g_pModelViewerPS) );
    m_ModelPSO.Finalize();

    m_CutoutModelPSO = m_ModelPSO;
    m_CutoutModelPSO.SetRasterizerState(RasterizerTwoSided);
    m_CutoutModelPSO.Finalize();

    ASSERT(m_Model.Load(L"Sponza/sponza.h3d"), "Failed to load model");
    ASSERT(m_Model.GetMeshCount() > 0, "Model contains no meshes");

    BuildDynamicObjects();

    // The caller of this function can override which materials are considered cutouts
    m_pMaterialIsCutout.resize(m_Model.GetMaterialCount());
    for (uint32_t i = 0; i < m_Model.GetMaterialCount(); ++i)
    {
        const ModelH3D::Material& mat = m_Model.GetMaterial(i);
        if (std::string(mat.texDiffusePath).find("thorn") != std::string::npos ||
            std::string(mat.texDiffusePath).find("plant") != std::string::npos ||
            std::string(mat.texDiffusePath).find("chain") != std::string::npos)
        {
            m_pMaterialIsCutout[i] = true;
        }
        else
        {
            m_pMaterialIsCutout[i] = false;
        }
    }

    ParticleEffects::InitFromJSON(L"Sponza/particles.json");

    float modelRadius = Length(m_Model.GetBoundingBox().GetDimensions()) * 0.5f;
    const Vector3 eye = m_Model.GetBoundingBox().GetCenter() + Vector3(modelRadius * 0.5f, 0.0f, 0.0f);
    Camera.SetEyeAtUp( eye, Vector3(kZero), Vector3(kYUnitVector) );

    Lighting::CreateRandomLights(m_Model.GetBoundingBox().GetMin(), m_Model.GetBoundingBox().GetMax());
}

const ModelH3D& Sponza::GetModel()
{
    return Sponza::m_Model;
}

namespace
{
    struct WeldKey
    {
        int32_t x, y, z;
        bool operator==(const WeldKey& other) const { return x == other.x && y == other.y && z == other.z; }
    };

    struct WeldKeyHash
    {
        size_t operator()(const WeldKey& key) const
        {
            size_t hash = 1469598103934665603ull;
            const int32_t components[3] = { key.x, key.y, key.z };
            for (int32_t component : components)
            {
                hash ^= (size_t)(uint32_t)component;
                hash *= 1099511628211ull;
            }
            return hash;
        }
    };

    bool BoxContains(const AxisAlignedBox& outer, const AxisAlignedBox& inner)
    {
        const Vector3 lo = inner.GetMin() - outer.GetMin();
        const Vector3 hi = outer.GetMax() - inner.GetMax();
        return (float)lo.GetX() >= 0.0f && (float)lo.GetY() >= 0.0f && (float)lo.GetZ() >= 0.0f
            && (float)hi.GetX() >= 0.0f && (float)hi.GetY() >= 0.0f && (float)hi.GetZ() >= 0.0f;
    }
}

void Sponza::BuildDynamicObjects( void )
{
    m_DynamicObjects.clear();
    m_DynamicObjects.resize(kDynamicObjectCount);
    m_MeshSplits.clear();
    m_MeshSplits.resize(m_Model.GetMeshCount());
    m_VelocityObjects.clear();
    m_SceneTime = 0.0f;

    // `placement` moves a selected object away from its authored spot, so the fixture can frame all
    // four from one camera with the occluded one behind a column. Offsets are target centre minus
    // the authored bowl centre; the planter keeps its authored spot.
    m_DynamicObjects[kVaseSpinning].name = "vase_spinning";
    m_DynamicObjects[kVaseSpinning].selection = AxisAlignedBox(Vector3(800.0f, -10.0f, -255.0f), Vector3(870.0f, 60.0f, -195.0f));
    m_DynamicObjects[kVaseSpinning].placement = Vector3(-213.00f, -0.12f, 136.55f);
    m_DynamicObjects[kVaseSteady].name = "vase_steady";
    m_DynamicObjects[kVaseSteady].selection = AxisAlignedBox(Vector3(90.0f, -10.0f, -255.0f), Vector3(150.0f, 60.0f, -195.0f));
    m_DynamicObjects[kVaseSteady].placement = Vector3(390.75f, 14.88f, 41.55f);
    m_DynamicObjects[kVaseOccluded].name = "vase_occluded";
    m_DynamicObjects[kVaseOccluded].selection = AxisAlignedBox(Vector3(800.0f, -10.0f, 120.0f), Vector3(870.0f, 60.0f, 190.0f));
    m_DynamicObjects[kVaseOccluded].placement = Vector3(-329.60f, 3.22f, -215.15f);
    m_DynamicObjects[kPlanterOscillating].name = "planter_oscillating";
    m_DynamicObjects[kPlanterOscillating].selection = AxisAlignedBox(Vector3(440.0f, 90.0f, -260.0f), Vector3(540.0f, 222.0f, -180.0f));

    const uint32_t VertexStride = m_Model.GetVertexStride();

    for (uint32_t meshIndex = 0; meshIndex < m_Model.GetMeshCount(); ++meshIndex)
    {
        const ModelH3D::Mesh& mesh = m_Model.GetMesh(meshIndex);

        ASSERT(mesh.indexCount % 3 == 0, "Sponza mesh index stream is not a triangle list");

        const uint16_t* indices = (const uint16_t*)(m_Model.m_pIndexData + mesh.indexDataByteOffset);
        const unsigned char* positions = m_Model.m_pVertexData + mesh.vertexDataByteOffset + mesh.attrib[ModelH3D::attrib_position].offset;

        std::vector<uint32_t> parent(mesh.vertexCount);
        for (uint32_t vertex = 0; vertex < mesh.vertexCount; ++vertex)
            parent[vertex] = vertex;

        auto find = [&parent](uint32_t vertex)
        {
            while (parent[vertex] != vertex)
            {
                parent[vertex] = parent[parent[vertex]];
                vertex = parent[vertex];
            }
            return vertex;
        };

        // Union by lowest index, so the representative of a component does not depend on the order
        // the triangles happen to be walked.
        auto unite = [&parent, &find](uint32_t a, uint32_t b)
        {
            a = find(a);
            b = find(b);
            if (a < b)
                parent[b] = a;
            else if (b < a)
                parent[a] = b;
        };

        // Position weld: the exporter splits vertices at normal/UV seams, so two triangles of one
        // object share an edge only after the duplicates are re-joined by position.
        std::unordered_map<WeldKey, uint32_t, WeldKeyHash> weld;
        weld.reserve(mesh.vertexCount);
        for (uint32_t vertex = 0; vertex < mesh.vertexCount; ++vertex)
        {
            const float* position = (const float*)(positions + vertex * mesh.vertexStride);
            const WeldKey key = { (int32_t)lroundf(position[0] * 100.0f), (int32_t)lroundf(position[1] * 100.0f), (int32_t)lroundf(position[2] * 100.0f) };
            const auto existing = weld.find(key);
            if (existing == weld.end())
                weld.emplace(key, vertex);
            else
                unite(existing->second, vertex);
        }

        for (uint32_t i = 0; i + 2 < mesh.indexCount; i += 3)
        {
            ASSERT(indices[i] < mesh.vertexCount && indices[i + 1] < mesh.vertexCount && indices[i + 2] < mesh.vertexCount,
                "Sponza mesh index is out of range for its vertex block");
            unite(indices[i], indices[i + 1]);
            unite(indices[i], indices[i + 2]);
        }

        std::unordered_map<uint32_t, AxisAlignedBox> componentBounds;
        for (uint32_t i = 0; i + 2 < mesh.indexCount; i += 3)
        {
            AxisAlignedBox& bounds = componentBounds[find(indices[i])];
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                const float* position = (const float*)(positions + indices[i + corner] * mesh.vertexStride);
                bounds.AddPoint(Vector3(position[0], position[1], position[2]));
            }
        }

        std::unordered_map<uint32_t, uint32_t> selectedRoots;
        for (const auto& entry : componentBounds)
        {
            for (uint32_t objectIndex = 0; objectIndex < m_DynamicObjects.size(); ++objectIndex)
            {
                DynamicObjectState& object = m_DynamicObjects[objectIndex];
                if (!BoxContains(object.selection, entry.second))
                    continue;

                selectedRoots.emplace(entry.first, objectIndex);
                object.bounds.AddBoundingBox(entry.second);
                break;
            }
        }

        // A merged mesh may interleave the triangles of several components, so a selected component
        // is emitted as one range per contiguous run of its triangles, not one range per component.
        const uint32_t meshStartIndex = mesh.indexDataByteOffset / (uint32_t)sizeof(uint16_t);
        const int32_t meshBaseVertex = (int32_t)(mesh.vertexDataByteOffset / VertexStride);

        bool runOpen = false;
        uint32_t runRoot = 0;
        uint32_t runObject = 0;
        uint32_t runStart = 0;
        uint32_t runEnd = 0;

        auto closeRun = [&]()
        {
            if (!runOpen)
                return;
            m_DynamicObjects[runObject].ranges.push_back({ runEnd - runStart, meshStartIndex + runStart, meshBaseVertex });
            m_MeshSplits[meshIndex].push_back({ meshStartIndex + runStart, runEnd - runStart, runObject });
            runOpen = false;
        };

        for (uint32_t i = 0; i + 2 < mesh.indexCount; i += 3)
        {
            const uint32_t root = find(indices[i]);
            const auto selected = selectedRoots.find(root);

            if (runOpen && (selected == selectedRoots.end() || root != runRoot || runEnd != i))
                closeRun();

            if (selected == selectedRoots.end())
                continue;

            if (!runOpen)
            {
                runOpen = true;
                runRoot = root;
                runObject = selected->second;
                runStart = i;
            }
            runEnd = i + 3;
        }
        closeRun();
    }

    // ASSERT compiles out in non-Debug, and a silently missing object would make the velocity pass
    // pass vacuously, so the empty case is also a hard failure in every configuration.
    for (const DynamicObjectState& object : m_DynamicObjects)
    {
        ASSERT(!object.ranges.empty(), "Dynamic object selection box matched no geometry");
        if (object.ranges.empty())
        {
            printf("  [dyn] FATAL %s selected no geometry -- sponza.h3d changed\n", object.name);
            fflush(stdout);
            std::abort();
        }
    }

    for (const eDynamicObject vase : { kVaseSpinning, kVaseSteady, kVaseOccluded })
        m_DynamicObjects[vase].pivot = m_DynamicObjects[vase].bounds.GetCenter();

    // The planter hangs from its chains, so it swings about the top of its own bounds.
    const AxisAlignedBox& planterBounds = m_DynamicObjects[kPlanterOscillating].bounds;
    m_DynamicObjects[kPlanterOscillating].pivot = Vector3(
        (float)planterBounds.GetCenter().GetX(),
        (float)planterBounds.GetMax().GetY(),
        (float)planterBounds.GetCenter().GetZ());

    m_VelocityObjects.reserve(m_DynamicObjects.size());
    for (const DynamicObjectState& object : m_DynamicObjects)
    {
        uint32_t triangles = 0;
        for (const MotionBlur::VelocityRange& range : object.ranges)
            triangles += range.indexCount / 3;

        // VRTF: raw printf, not Utility::Printf -- this is a stdout witness the harness reads, and
        // Utility::Print routes to OutputDebugString unless _CONSOLE is defined, which it is not here.
        printf("  [dyn] %s ranges=%u tris=%u bounds=(%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f)\n",
            object.name, (uint32_t)object.ranges.size(), triangles,
            (float)object.bounds.GetMin().GetX(), (float)object.bounds.GetMin().GetY(), (float)object.bounds.GetMin().GetZ(),
            (float)object.bounds.GetMax().GetX(), (float)object.bounds.GetMax().GetY(), (float)object.bounds.GetMax().GetZ());

        m_VelocityObjects.push_back({ object.world, object.prevWorld, object.prevPrevWorld, object.ranges.data(), (uint32_t)object.ranges.size() });
    }

    // The objects are authored at the origin and moved by `placement`, so leaving world at the
    // identity here would make the first rendered frame carry the whole offset as one teleport.
    ResetSceneTime();
}

void Sponza::PoseDynamicObjects( void )
{
    // Displacement around a circle that leaves the placed spot along +z, shared by all three vases:
    // the fixture camera looks down -x, so an x-first phase spends the measured window moving along
    // the view axis and projects to almost no pixels. Zero at t=0, so `placement` IS the t=0 centre.
    auto slideOffset = [](float radius, float angle)
    {
        return Vector3(radius * (1.0f - cosf(angle)), 0.0f, radius * sinf(angle));
    };

    // Constant screen speed with no stall at an extreme.
    auto orbit = [&slideOffset](DynamicObjectState& object, float radius, float rate)
    {
        object.world = Matrix4(AffineTransform::MakeTranslation(object.placement + slideOffset(radius, rate * m_SceneTime)));
    };

    DynamicObjectState& spinning = m_DynamicObjects[kVaseSpinning];
    const Vector3 slide = slideOffset(kVaseSlideRadius, kVaseSlideRate * m_SceneTime);
    spinning.world = Matrix4(AffineTransform::MakeTranslation(spinning.placement + spinning.pivot + slide))
        * Matrix4(AffineTransform::MakeYRotation(kVaseSpinRate * m_SceneTime))
        * Matrix4(AffineTransform::MakeTranslation(-spinning.pivot));

    orbit(m_DynamicObjects[kVaseSteady], kSteadyVaseRadius, kSteadyVaseRate);
    orbit(m_DynamicObjects[kVaseOccluded], kOccludedVaseRadius, kOccludedVaseRate);

    // Swings about the top of its own bounds, so it is not the identity at t=0: the planter hangs
    // tilted when frozen.
    DynamicObjectState& planter = m_DynamicObjects[kPlanterOscillating];
    planter.world = Matrix4(AffineTransform::MakeTranslation(planter.placement + planter.pivot))
        * Matrix4(AffineTransform::MakeXRotation(kPlanterSwingAmplitude * sinf(kPlanterSwingRate * m_SceneTime)))
        * Matrix4(AffineTransform::MakeTranslation(-planter.pivot));
}

void Sponza::MirrorVelocityObjects( void )
{
    ASSERT(m_VelocityObjects.size() == m_DynamicObjects.size(), "Velocity objects must mirror the dynamic objects one to one");
    for (size_t objectIndex = 0; objectIndex < m_DynamicObjects.size(); ++objectIndex)
    {
        m_VelocityObjects[objectIndex].world = m_DynamicObjects[objectIndex].world;
        m_VelocityObjects[objectIndex].prevWorld = m_DynamicObjects[objectIndex].prevWorld;
        m_VelocityObjects[objectIndex].prevPrevWorld = m_DynamicObjects[objectIndex].prevPrevWorld;
    }
}

void Sponza::ResetSceneTime( void )
{
    if (m_DynamicObjects.empty())
        return;

    m_SceneTime = 0.0f;
    PoseDynamicObjects();

    for (DynamicObjectState& object : m_DynamicObjects)
    {
        object.prevWorld = object.world;
        object.prevPrevWorld = object.world;
    }

    MirrorVelocityObjects();
}

void Sponza::Update( float deltaT )
{
    if (m_DynamicObjects.empty())
        return;

    for (DynamicObjectState& object : m_DynamicObjects)
    {
        object.prevPrevWorld = object.prevWorld;
        object.prevWorld = object.world;
    }

    m_SceneTime += deltaT;

    PoseDynamicObjects();
    MirrorVelocityObjects();
}

MotionBlur::VelocityGeometry Sponza::DynamicGeometry()
{
    MotionBlur::VelocityGeometry geometry;
    geometry.vertexBuffer = m_Model.GetVertexBuffer();
    geometry.indexBuffer = m_Model.GetIndexBuffer();
    geometry.objects = m_VelocityObjects.data();
    geometry.objectCount = (uint32_t)m_VelocityObjects.size();
    return geometry;
}

uint32_t Sponza::DynamicObjectCount()
{
    return (uint32_t)m_DynamicObjects.size();
}

Sponza::DynamicObjectView Sponza::DynamicObject( uint32_t index )
{
    ASSERT(index < m_DynamicObjects.size());
    const DynamicObjectState& object = m_DynamicObjects[index];
    return { object.name, object.world, object.bounds };
}

void Sponza::Cleanup( void )
{
    m_DynamicObjects.clear();
    m_MeshSplits.clear();
    m_VelocityObjects.clear();
    m_SceneTime = 0.0f;

    m_Model.Clear();
    Lighting::Shutdown();
    ParticleEffects::Shutdown(); // VRTF: static TextureRefs must drop before the cache dies (see ParticleEffects.h)
    // VRTF: TextureManager::Shutdown() removed -- Renderer::Shutdown() (which ModelViewer::Cleanup
    // runs right after this) owns cache shutdown and nulls its IBL TextureRef globals FIRST.
    // Clearing the cache here deletes ManagedTextures the Renderer globals still reference ->
    // use-after-free in TextureRef::operator=(nullptr_t) on every legacy-path shutdown
    // (intermittent 0xC0000005 child exits; deterministic under PageHeap).
}

void Sponza::RenderObjects( GraphicsContext& gfxContext, const Matrix4& ViewProjMat, const Vector3& viewerPos, eObjectFilter Filter )
{
    struct VSConstants
    {
        Matrix4 modelToProjection;
        Matrix4 modelToShadow;
        XMFLOAT3 viewerPos;
        Matrix4 modelToWorld;
    } vsConstants;

    const Matrix4 identity(kIdentity);
    bool dynamicConstants = false;

    auto upload = [&](const Matrix4& world)
    {
        vsConstants.modelToProjection = ViewProjMat * world;
        vsConstants.modelToShadow = m_SunShadow.GetShadowMatrix() * world;
        vsConstants.modelToWorld = world;
        XMStoreFloat3(&vsConstants.viewerPos, viewerPos);
        gfxContext.SetDynamicConstantBufferView(Renderer::kMeshConstants, sizeof(vsConstants), &vsConstants);
    };

    auto drawStatic = [&](uint32_t indexCount, uint32_t startIndex, uint32_t baseVertex)
    {
        if (dynamicConstants)
        {
            upload(identity);
            dynamicConstants = false;
        }
        gfxContext.DrawIndexed(indexCount, startIndex, baseVertex);
    };

    upload(identity);

    __declspec(align(16)) uint32_t materialIdx = 0xFFFFFFFFul;

    uint32_t VertexStride = m_Model.GetVertexStride();

    for (uint32_t meshIndex = 0; meshIndex < m_Model.GetMeshCount(); meshIndex++)
    {
        const ModelH3D::Mesh& mesh = m_Model.GetMesh(meshIndex);

        uint32_t indexCount = mesh.indexCount;
        uint32_t startIndex = mesh.indexDataByteOffset / sizeof(uint16_t);
        uint32_t baseVertex = mesh.vertexDataByteOffset / VertexStride;

        if (mesh.materialIndex != materialIdx)
        {
            if ( m_pMaterialIsCutout[mesh.materialIndex] && !(Filter & kCutout) ||
                !m_pMaterialIsCutout[mesh.materialIndex] && !(Filter & kOpaque) )
                continue;

            materialIdx = mesh.materialIndex;
            gfxContext.SetDescriptorTable(Renderer::kMaterialSRVs, m_Model.GetSRVs(materialIdx));

            gfxContext.SetDynamicConstantBufferView(Renderer::kCommonCBV, sizeof(uint32_t), &materialIdx);
        }

        uint32_t cursor = startIndex;
        for (const MeshSplit& split : m_MeshSplits[meshIndex])
        {
            if (split.startIndex > cursor)
                drawStatic(split.startIndex - cursor, cursor, baseVertex);

            upload(m_DynamicObjects[split.objectIndex].world);
            dynamicConstants = true;
            gfxContext.DrawIndexed(split.indexCount, split.startIndex, baseVertex);

            cursor = split.startIndex + split.indexCount;
        }

        if (cursor < startIndex + indexCount)
            drawStatic(startIndex + indexCount - cursor, cursor, baseVertex);
    }
}

void Sponza::RenderLightShadows(GraphicsContext& gfxContext, const Camera& camera)
{
    using namespace Lighting;

    ScopedTimer _prof(L"RenderLightShadows", gfxContext);

    static uint32_t LightIndex = 0;
    if (LightIndex >= MaxLights)
        return;

    m_LightShadowTempBuffer.BeginRendering(gfxContext);
    {
        gfxContext.SetPipelineState(m_ShadowPSO);
        RenderObjects(gfxContext, m_LightShadowMatrix[LightIndex], camera.GetPosition(), kOpaque);
        gfxContext.SetPipelineState(m_CutoutShadowPSO);
        RenderObjects(gfxContext, m_LightShadowMatrix[LightIndex], camera.GetPosition(), kCutout);
    }
    //m_LightShadowTempBuffer.EndRendering(gfxContext);

    gfxContext.TransitionResource(m_LightShadowTempBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gfxContext.TransitionResource(m_LightShadowArray, D3D12_RESOURCE_STATE_COPY_DEST);

    gfxContext.CopySubresource(m_LightShadowArray, LightIndex, m_LightShadowTempBuffer, 0);

    gfxContext.TransitionResource(m_LightShadowArray, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ++LightIndex;
}

void Sponza::RenderScene(
    GraphicsContext& gfxContext,
    const Camera& camera,
    const D3D12_VIEWPORT& viewport,
    const D3D12_RECT& scissor,
    bool skipDiffusePass,
    bool skipShadowMap)
{
    Renderer::UpdateGlobalDescriptors();

    uint32_t FrameIndex = TemporalEffects::GetFrameIndexMod2();

    float costheta = cosf(m_SunOrientation);
    float sintheta = sinf(m_SunOrientation);
    float cosphi = cosf(m_SunInclination * 3.14159f * 0.5f);
    float sinphi = sinf(m_SunInclination * 3.14159f * 0.5f);
    m_SunDirection = Normalize(Vector3( costheta * cosphi, sinphi, sintheta * cosphi ));

    __declspec(align(16)) struct
    {
        Vector3 sunDirection;
        Vector3 sunLight;
        Vector3 ambientLight;
        float ShadowTexelSize[4];

        float InvTileDim[4];
        uint32_t TileCount[4];
        uint32_t FirstLightIndex[4];

		uint32_t FrameIndexMod2;
    } psConstants;

    psConstants.sunDirection = m_SunDirection;
    psConstants.sunLight = Vector3(1.0f, 1.0f, 1.0f) * m_SunLightIntensity;
    psConstants.ambientLight = Vector3(1.0f, 1.0f, 1.0f) * m_AmbientIntensity;
    psConstants.ShadowTexelSize[0] = 1.0f / g_ShadowBuffer.GetWidth();
    psConstants.InvTileDim[0] = 1.0f / Lighting::LightGridDim;
    psConstants.InvTileDim[1] = 1.0f / Lighting::LightGridDim;
    psConstants.TileCount[0] = Math::DivideByMultiple(g_SceneColorBuffer.GetWidth(), Lighting::LightGridDim);
    psConstants.TileCount[1] = Math::DivideByMultiple(g_SceneColorBuffer.GetHeight(), Lighting::LightGridDim);
    psConstants.FirstLightIndex[0] = Lighting::m_FirstConeLight;
    psConstants.FirstLightIndex[1] = Lighting::m_FirstConeShadowedLight;
	psConstants.FrameIndexMod2 = FrameIndex;

    // Set the default state for command lists
    auto& pfnSetupGraphicsState = [&](void)
    {
        gfxContext.SetRootSignature(Renderer::m_RootSig);
        gfxContext.SetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Renderer::s_TextureHeap.GetHeapPointer());
        gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gfxContext.SetIndexBuffer(m_Model.GetIndexBuffer());
        gfxContext.SetVertexBuffer(0, m_Model.GetVertexBuffer());
    };

    pfnSetupGraphicsState();

    RenderLightShadows(gfxContext, camera);

    {
        ScopedTimer _prof(L"Z PrePass", gfxContext);

        gfxContext.SetDynamicConstantBufferView(Renderer::kMaterialConstants, sizeof(psConstants), &psConstants);

        {
            ScopedTimer _prof2(L"Opaque", gfxContext);
            {
                gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
                gfxContext.ClearDepth(g_SceneDepthBuffer);
                gfxContext.SetPipelineState(m_DepthPSO);
                gfxContext.SetDepthStencilTarget(g_SceneDepthBuffer.GetDSV());
                gfxContext.SetViewportAndScissor(viewport, scissor);
            }
            RenderObjects(gfxContext, camera.GetViewProjMatrix(), camera.GetPosition(), kOpaque );
        }

        {
            ScopedTimer _prof2(L"Cutout", gfxContext);
            {
                gfxContext.SetPipelineState(m_CutoutDepthPSO);
            }
            RenderObjects(gfxContext, camera.GetViewProjMatrix(), camera.GetPosition(), kCutout );
        }
    }

    SSAO::Render(gfxContext, camera);

    if (!skipDiffusePass)
    {
        Lighting::FillLightGrid(gfxContext, camera);

        if (!SSAO::DebugDraw)
        {
            ScopedTimer _prof(L"Main Render", gfxContext);
            {
                gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
                gfxContext.TransitionResource(g_SceneNormalBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
                gfxContext.ClearColor(g_SceneColorBuffer);
            }
        }
    }

    if (!skipShadowMap)
    {
        if (!SSAO::DebugDraw)
        {
            pfnSetupGraphicsState();
            {
                ScopedTimer _prof2(L"Render Shadow Map", gfxContext);

                m_SunShadow.UpdateMatrix(-m_SunDirection, Vector3(0, -500.0f, 0), Vector3(ShadowDimX, ShadowDimY, ShadowDimZ),
                    (uint32_t)g_ShadowBuffer.GetWidth(), (uint32_t)g_ShadowBuffer.GetHeight(), 16);

                g_ShadowBuffer.BeginRendering(gfxContext);
                gfxContext.SetPipelineState(m_ShadowPSO);
                RenderObjects(gfxContext, m_SunShadow.GetViewProjMatrix(), camera.GetPosition(), kOpaque);
                gfxContext.SetPipelineState(m_CutoutShadowPSO);
                RenderObjects(gfxContext, m_SunShadow.GetViewProjMatrix(), camera.GetPosition(), kCutout);
                g_ShadowBuffer.EndRendering(gfxContext);
            }
        }
    }

    if (!skipDiffusePass)
    {
        if (!SSAO::DebugDraw)
        {
            if (SSAO::AsyncCompute)
            {
                gfxContext.Flush();
                pfnSetupGraphicsState();

                // Make the 3D queue wait for the Compute queue to finish SSAO
                g_CommandManager.GetGraphicsQueue().StallForProducer(g_CommandManager.GetComputeQueue());
            }

            {
                ScopedTimer _prof2(L"Render Color", gfxContext);

                gfxContext.TransitionResource(g_SSAOFullScreen, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

                gfxContext.SetDescriptorTable(Renderer::kCommonSRVs, Renderer::m_CommonTextures);
                gfxContext.SetDynamicConstantBufferView(Renderer::kMaterialConstants, sizeof(psConstants), &psConstants);

                {
                    gfxContext.SetPipelineState(m_ModelPSO);
                    gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
                    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[]{ g_SceneColorBuffer.GetRTV(), g_SceneNormalBuffer.GetRTV() };
                    gfxContext.SetRenderTargets(ARRAYSIZE(rtvs), rtvs, g_SceneDepthBuffer.GetDSV_DepthReadOnly());
                    gfxContext.SetViewportAndScissor(viewport, scissor);
                }
                RenderObjects( gfxContext, camera.GetViewProjMatrix(), camera.GetPosition(), Sponza::kOpaque );

                gfxContext.SetPipelineState(m_CutoutModelPSO);
                RenderObjects( gfxContext, camera.GetViewProjMatrix(), camera.GetPosition(), Sponza::kCutout );
            }
        }
    }
}

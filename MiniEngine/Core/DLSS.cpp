// clang-format off
#include "pch.h"
#include "DLSS.h"
#include "GraphicsCore.h"
#include "CommandListManager.h"
#include "CommandContext.h"
#include "BufferManager.h"
#include "TemporalEffects.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CompiledShaders/DlssMotionVectorsCS.h"
// clang-format on

#include <cstdio>

using namespace Graphics;

// NGX (DLSS) is reached ONLY through ngx_shim.dll — a release-CRT wrapper exposing a flat C ABI.
// MiniEngine is built with the debug CRT (/MDd); NGX's debug loader lib fails feature-create inside
// the vrtf_runner process while its release loader (/MD) succeeds, and the release loader cannot link
// into a /MDd binary (CRT clash). The shim isolates that: this TU includes NO NGX headers and passes
// only raw ID3D12* pointers + PODs across the boundary. See src/ngx_shim/ngx_shim.cpp.
namespace DLSS
{
    BoolVar Enable("Graphics/AA/DLSS/Enable", false);

    static const char* s_QualityLabels[] = {
        "UltraPerformance",
        "Performance",
        "Balanced",
        "Quality",
        "DLAA",
    };
    EnumVar QualityMode("Graphics/AA/DLSS/Quality", 1, _countof(s_QualityLabels), s_QualityLabels);

#if defined(VRTF_HAVE_NGX)
    // ngx_shim.dll C ABI (raw D3D12 pointers as void*; quality 0..4; flags 0/1).
    using PFN_init        = int (*)(void* device);
    using PFN_optimal     = int (*)(unsigned dW, unsigned dH, int quality, unsigned* oW, unsigned* oH);
    using PFN_create      = int (*)(void* cmdList, unsigned rW, unsigned rH, unsigned dW, unsigned dH, int quality, int hdr, int depthInverted, int mvLowRes);
    using PFN_evaluate    = int (*)(void* cmdList, void* color, void* depth, void* motion, void* output, float jitterX, float jitterY, int reset, float mvScaleX, float mvScaleY, unsigned rW, unsigned rH);
    using PFN_releaseFeat = void (*)(void);
    using PFN_shutdown    = void (*)(void* device);

    static HMODULE         s_Shim        = nullptr;
    static PFN_init        s_ngxInit     = nullptr;
    static PFN_optimal     s_ngxOptimal  = nullptr;
    static PFN_create      s_ngxCreate   = nullptr;
    static PFN_evaluate    s_ngxEval     = nullptr;
    static PFN_releaseFeat s_ngxRelease  = nullptr;
    static PFN_shutdown    s_ngxShutdown = nullptr;
#endif

    static bool     s_Initialized  = false;
    static bool     s_Supported    = false;
    static bool     s_HaveFeature  = false;
    static uint32_t s_LastRenderW  = 0;
    static uint32_t s_LastRenderH  = 0;
    static uint32_t s_LastDisplayW = 0;
    static uint32_t s_LastDisplayH = 0;
    static int      s_LastQuality  = -1;
    static bool     s_NeedsReset   = true;

    static ComputePSO s_DlssMotionVectorsCS(L"DLSS: Motion Vectors CS");

    // alignas(16): ComputeContext::SetDynamicConstantBufferView ASSERTs the source pointer is
    // 16-byte aligned (root CBV requirement). An all-float struct is only 4-byte aligned on the stack.
    struct alignas(16) DlssMVCB
    {
        float JitterDeltaX;
        float JitterDeltaY;
        float _pad[2];
    };
}

void DLSS::Initialize(void)
{
    if (s_Initialized)
        return;
    s_Initialized = true;

    s_DlssMotionVectorsCS.SetRootSignature(g_CommonRS);
    s_DlssMotionVectorsCS.SetComputeShader(g_pDlssMotionVectorsCS, sizeof(g_pDlssMotionVectorsCS));
    s_DlssMotionVectorsCS.Finalize();

#if defined(VRTF_HAVE_NGX)
    s_Shim = LoadLibraryA("ngx_shim.dll");
    if (!s_Shim)
    {
        std::printf("[DLSS] ngx_shim.dll not found (%lu) — DLSS disabled.\n", GetLastError());
        return;
    }
    s_ngxInit     = reinterpret_cast<PFN_init>(GetProcAddress(s_Shim, "vrtf_ngx_init"));
    s_ngxOptimal  = reinterpret_cast<PFN_optimal>(GetProcAddress(s_Shim, "vrtf_ngx_optimal"));
    s_ngxCreate   = reinterpret_cast<PFN_create>(GetProcAddress(s_Shim, "vrtf_ngx_create"));
    s_ngxEval     = reinterpret_cast<PFN_evaluate>(GetProcAddress(s_Shim, "vrtf_ngx_evaluate"));
    s_ngxRelease  = reinterpret_cast<PFN_releaseFeat>(GetProcAddress(s_Shim, "vrtf_ngx_release_feature"));
    s_ngxShutdown = reinterpret_cast<PFN_shutdown>(GetProcAddress(s_Shim, "vrtf_ngx_shutdown"));
    if (!s_ngxInit || !s_ngxOptimal || !s_ngxCreate || !s_ngxEval || !s_ngxRelease || !s_ngxShutdown)
    {
        std::printf("[DLSS] ngx_shim.dll missing exports — DLSS disabled.\n");
        return;
    }

    s_Supported = (s_ngxInit(g_Device) != 0);
    std::printf(s_Supported ? "[DLSS] Initialized; DLSS supported (via ngx_shim).\n"
                            : "[DLSS] DLSS not supported on this GPU/driver — disabled.\n");
#endif
}

void DLSS::Shutdown(void)
{
#if defined(VRTF_HAVE_NGX)
    if (s_Shim)
    {
        if (s_ngxRelease)  s_ngxRelease();
        if (s_ngxShutdown) s_ngxShutdown(g_Device);
        FreeLibrary(s_Shim);
        s_Shim = nullptr;
    }
#endif
    s_Supported   = false;
    s_Initialized = false;
    s_HaveFeature = false;
}

bool DLSS::IsSupported(void)
{
    return s_Supported;
}

void DLSS::GetRenderDimensions(uint32_t DisplayWidth, uint32_t DisplayHeight, uint32_t& RenderWidth, uint32_t& RenderHeight)
{
#if defined(VRTF_HAVE_NGX)
    if (s_Supported && s_ngxOptimal)
    {
        unsigned int optW = 0, optH = 0;
        if (s_ngxOptimal(DisplayWidth, DisplayHeight, (int)QualityMode, &optW, &optH) && optW > 0 && optH > 0)
        {
            RenderWidth  = optW;
            RenderHeight = optH;
            return;
        }
    }
#endif
    RenderWidth  = DisplayWidth;
    RenderHeight = DisplayHeight;
}

bool DLSS::Resolve(CommandContext& Context, ColorBuffer& Output)
{
    if (!s_Supported)
        return false;

#if defined(VRTF_HAVE_NGX)
    const uint32_t renderW  = g_SceneColorBuffer.GetWidth();
    const uint32_t renderH  = g_SceneColorBuffer.GetHeight();
    const uint32_t displayW = Output.GetWidth();
    const uint32_t displayH = Output.GetHeight();
    const int      quality  = (int)QualityMode;

    const bool dimsChanged    = (renderW  != s_LastRenderW  || renderH  != s_LastRenderH ||
                                 displayW != s_LastDisplayW || displayH != s_LastDisplayH);
    const bool qualityChanged = (quality != s_LastQuality);

    if (dimsChanged || qualityChanged)
    {
        s_ngxRelease();
        s_HaveFeature  = false;
        s_LastRenderW  = renderW;
        s_LastRenderH  = renderH;
        s_LastDisplayW = displayW;
        s_LastDisplayH = displayH;
        s_LastQuality  = quality;
        s_NeedsReset   = true;
    }

    float jitterX = 0.5f, jitterY = 0.5f;
    TemporalEffects::GetJitterOffset(jitterX, jitterY);
    // DLSS wants the sub-pixel jitter offset in RENDER pixels. MiniEngine jitters via the viewport
    // top-left, so GetJitterOffset is already in pixels in [0,1) with 0.5 = neutral. Sign verified
    // visually (flip if the image shimmers).
    const float jitterPixX = jitterX - 0.5f;
    const float jitterPixY = jitterY - 0.5f;

    {
        ComputeContext& cc = Context.GetComputeContext();
        cc.SetRootSignature(g_CommonRS);
        cc.SetPipelineState(s_DlssMotionVectorsCS);

        // MiniEngine's g_VelocityBuffer is built from the unjittered reprojection matrix (jitter is a
        // viewport offset, not in the matrix), so it is already jitter-free — do not subtract jitter.
        DlssMVCB cb{ 0.0f, 0.0f, 0.0f, 0.0f };
        cc.SetDynamicConstantBufferView(3, sizeof(cb), &cb);

        cc.TransitionResource(g_VelocityBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cc.TransitionResource(g_DLSSMotionBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cc.FlushResourceBarriers();

        // g_CommonRS: slot 1 = SRV (t0-t9), slot 2 = UAV (u0-u9)
        cc.SetDynamicDescriptor(1, 0, g_VelocityBuffer.GetSRV());
        cc.SetDynamicDescriptor(2, 0, g_DLSSMotionBuffer.GetUAV());

        cc.Dispatch2D(renderW, renderH);
    }

    // NGX reads color/depth/MV as non-pixel SRVs and writes Output as a UAV. Set those states on
    // MiniEngine's command list; the shim then records the NGX create/evaluate onto the same list.
    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_DLSSMotionBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(Output,             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Context.FlushResourceBarriers();

    if (!s_HaveFeature)
    {
        const int hdr = 1, depthInverted = 1, mvLowRes = 1; // MiniEngine: HDR scene color, reversed-Z, render-res MV
        if (!s_ngxCreate(Context.GetCommandList(), renderW, renderH, displayW, displayH, quality, hdr, depthInverted, mvLowRes))
        {
            std::printf("[DLSS] shim CreateFeature failed — falling back to stock path.\n");
            s_Supported = false;
            return false;
        }
        s_HaveFeature = true;
    }

    if (!s_ngxEval(Context.GetCommandList(),
                   g_SceneColorBuffer.GetResource(),
                   g_SceneDepthBuffer.GetResource(),
                   g_DLSSMotionBuffer.GetResource(),
                   Output.GetResource(),
                   jitterPixX, jitterPixY,
                   s_NeedsReset ? 1 : 0,
                   1.0f, 1.0f,
                   renderW, renderH))
    {
        std::printf("[DLSS] shim Evaluate failed — falling back to stock path.\n");
        s_Supported = false;
        return false;
    }

    s_NeedsReset = false;

    // NGX issues raw D3D12 barriers outside MiniEngine's CommandContext tracking.
    // Re-declare the resources in the states NGX left them for subsequent MiniEngine use.
    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_DLSSMotionBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(Output,             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
#else
    return false;
#endif
}


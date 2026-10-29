// clang-format off
#include "pch.h"
#include "DLSS.h"
#include "UpscalerBackend.h"
#include "GraphicsCore.h"
#include "CommandListManager.h"
#include "CommandContext.h"
#include "BufferManager.h"
#include "Display.h"
#include "TemporalEffects.h"
#include "MotionBlur.h"
// clang-format on

#include <cstdio>
#include <cstring>

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

    static const UpscalerBackend* s_Backend      = nullptr;
    static UpscalerStatus         s_Status       = UpscalerStatus::Unsupported;
    static bool                   s_Configured   = false;
    static bool                   s_Initialized  = false;
    static bool                   s_Supported    = false;
    static bool                   s_HaveFeature  = false;
    static uint32_t               s_LastRenderW  = 0;
    static uint32_t               s_LastRenderH  = 0;
    static uint32_t               s_LastDisplayW = 0;
    static uint32_t               s_LastDisplayH = 0;
    static int                    s_LastQuality  = -1;
    static uint32_t               s_ResetEvaluates = 1;
    static float                  s_CameraNear        = 1.0f;
    static float                  s_CameraFar         = 10000.0f;
    static float                  s_CameraVerticalFov = DirectX::XM_PIDIV4;
}

namespace
{
#if defined(VRTF_HAVE_NGX)
    DLSS::UpscalerStatus ngxConfigure()
    {
        return DLSS::UpscalerStatus::Active;
    }

    DLSS::UpscalerStatus ngxInitialize(ID3D12Device* device)
    {
        using namespace DLSS;
        s_Shim = LoadLibraryA("ngx_shim.dll");
        if (!s_Shim)
        {
            std::printf("[DLSS] ngx_shim.dll not found (%lu) — DLSS disabled.\n", GetLastError());
            return UpscalerStatus::Unsupported;
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
            return UpscalerStatus::Unsupported;
        }

        const bool supported = s_ngxInit(device) != 0;
        std::printf(supported ? "[DLSS] Initialized; DLSS supported (via ngx_shim).\n"
                              : "[DLSS] DLSS not supported on this GPU/driver — disabled.\n");
        return supported ? UpscalerStatus::Active : UpscalerStatus::Unsupported;
    }

    void ngxShutdown()
    {
        using namespace DLSS;
        if (s_Shim)
        {
            if (s_ngxRelease)  s_ngxRelease();
            if (s_ngxShutdown) s_ngxShutdown(g_Device);
            FreeLibrary(s_Shim);
            s_Shim = nullptr;
        }
    }

    bool ngxOptimalRenderSize(uint32_t displayWidth, uint32_t displayHeight, int quality, uint32_t& renderWidth, uint32_t& renderHeight)
    {
        unsigned int optW = 0, optH = 0;
        if (!DLSS::s_ngxOptimal(displayWidth, displayHeight, quality, &optW, &optH) || optW == 0 || optH == 0)
            return false;
        renderWidth  = optW;
        renderHeight = optH;
        return true;
    }

    bool ngxCreateFeature(const DLSS::UpscalerFrame& frame)
    {
        const int hdr = 1, depthInverted = 1, mvLowRes = 1; // MiniEngine: HDR scene color, reversed-Z, render-res MV
        return DLSS::s_ngxCreate(frame.commandList, frame.renderWidth, frame.renderHeight, frame.displayWidth, frame.displayHeight, frame.quality, hdr, depthInverted, mvLowRes) != 0;
    }

    bool ngxEvaluate(const DLSS::UpscalerFrame& frame)
    {
        return DLSS::s_ngxEval(frame.commandList, frame.color, frame.depth, frame.motionVectors, frame.output, frame.jitterX, frame.jitterY, frame.reset ? 1 : 0, 1.0f, 1.0f,
                               frame.renderWidth, frame.renderHeight) != 0;
    }

    void ngxReleaseFeature()
    {
        DLSS::s_ngxRelease();
    }

    constexpr DLSS::UpscalerBackend kNgxBackend{
        "ngx", ngxConfigure, ngxInitialize, ngxShutdown, ngxOptimalRenderSize, ngxCreateFeature, ngxEvaluate, ngxReleaseFeature,
    };
#else
    DLSS::UpscalerStatus ngxAbsent()
    {
        return DLSS::UpscalerStatus::Unsupported;
    }

    constexpr DLSS::UpscalerBackend kNgxBackend{ "ngx", ngxAbsent };
#endif

    const DLSS::UpscalerBackend* backendNamed(const char* name)
    {
        if (std::strcmp(name, "ngx") == 0)
            return &DLSS::NgxBackend();
        if (std::strcmp(name, "streamline") == 0)
            return &DLSS::StreamlineBackend();
        if (std::strcmp(name, "fsr31") == 0)
            return &DLSS::Fsr31Backend();
        return nullptr;
    }
}

const DLSS::UpscalerBackend& DLSS::NgxBackend()
{
    return kNgxBackend;
}

const wchar_t* DLSS::UpscalerModuleDirectory()
{
    static wchar_t directory[MAX_PATH]{};
    if (directory[0] == L'\0')
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&UpscalerModuleDirectory), &module);
        GetModuleFileNameW(module, directory, MAX_PATH);
        if (wchar_t* const slash = std::wcsrchr(directory, L'\\'))
            slash[1] = L'\0';
    }
    return directory;
}

void DLSS::Configure(void)
{
    if (s_Configured)
        return;
    s_Configured = true;

    char name[32]{};
    const DWORD length = GetEnvironmentVariableA("VRTF_UPSCALER", name, sizeof(name));
    s_Backend = length == 0 ? &NgxBackend() : length < sizeof(name) ? backendNamed(name) : nullptr;
    if (!s_Backend)
    {
        std::printf("[DLSS] VRTF_UPSCALER=%s names no upscaler (ngx, streamline, fsr31).\n", name);
        s_Status = UpscalerStatus::Broken;
        return;
    }
    s_Status = s_Backend->configure();

    char alsoLoaded[32]{};
    const DWORD alsoLength = GetEnvironmentVariableA("VRTF_UPSCALER_ALSO_LOADED", alsoLoaded, sizeof(alsoLoaded));
    if (alsoLength == 0)
        return;
    const UpscalerBackend* const second = alsoLength < sizeof(alsoLoaded) ? backendNamed(alsoLoaded) : nullptr;
    if (!second || second == s_Backend || second->configure() != UpscalerStatus::Active)
    {
        std::printf("[DLSS] VRTF_UPSCALER_ALSO_LOADED=%s loads no second upscaler beside %s.\n", alsoLoaded, s_Backend->name);
        s_Status = UpscalerStatus::Broken;
    }
}

void DLSS::Initialize(void)
{
    if (s_Initialized)
        return;
    s_Initialized = true;

    Configure();
    if (s_Status != UpscalerStatus::Active)
        return;
    s_Status    = s_Backend->initialize(g_Device);
    s_Supported = s_Status == UpscalerStatus::Active;
}

void DLSS::Shutdown(void)
{
    if (s_Backend && s_Backend->shutdown)
        s_Backend->shutdown();
    s_Supported   = false;
    s_Initialized = false;
    s_Configured  = false;
    s_HaveFeature = false;
}

bool DLSS::IsSupported(void)
{
    return s_Supported;
}

int DLSS::Status(void)
{
    return static_cast<int>(s_Status);
}

void DLSS::SetCameraParameters(float NearClip, float FarClip, float VerticalFov)
{
    s_CameraNear        = NearClip;
    s_CameraFar         = FarClip;
    s_CameraVerticalFov = VerticalFov;
}

void DLSS::RequestHistoryReset(uint32_t Evaluates)
{
    if (Evaluates > s_ResetEvaluates)
        s_ResetEvaluates = Evaluates;
}

void DLSS::GetRenderDimensions(uint32_t DisplayWidth, uint32_t DisplayHeight, uint32_t& RenderWidth, uint32_t& RenderHeight)
{
    if (s_Supported && s_Backend->optimalRenderSize(DisplayWidth, DisplayHeight, (int)QualityMode, RenderWidth, RenderHeight))
        return;
    RenderWidth  = DisplayWidth;
    RenderHeight = DisplayHeight;
}

bool DLSS::Resolve(CommandContext& Context, ColorBuffer& Output)
{
    if (!s_Supported)
        return false;

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
        s_Backend->releaseFeature();
        s_HaveFeature  = false;
        s_LastRenderW  = renderW;
        s_LastRenderH  = renderH;
        s_LastDisplayW = displayW;
        s_LastDisplayH = displayH;
        s_LastQuality  = quality;
        RequestHistoryReset(1);
    }

    float jitterX = 0.5f, jitterY = 0.5f;
    TemporalEffects::GetJitterOffset(jitterX, jitterY);
    // DLSS wants the sub-pixel jitter offset in RENDER pixels. MiniEngine jitters via the viewport
    // top-left, so GetJitterOffset is already in pixels in [0,1) with 0.5 = neutral. Sign verified
    // visually (flip if the image shimmers).
    const float jitterPixX = jitterX - 0.5f;
    const float jitterPixY = jitterY - 0.5f;

    // MiniEngine's g_VelocityBuffer is built from the unjittered reprojection matrix (jitter is a
    // viewport offset, not in the matrix), so it is already jitter-free — do not subtract jitter.
    MotionBlur::RepackMotionVectors(Context, g_VelocityBuffer, g_DLSSMotionBuffer, 0.0f, 0.0f);

    // NGX reads color/depth/MV as non-pixel SRVs and writes Output as a UAV. Set those states on
    // MiniEngine's command list; the shim then records the NGX create/evaluate onto the same list.
    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_DLSSMotionBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(Output,             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Context.FlushResourceBarriers();

    const UpscalerFrame frame{
        Context.GetCommandList(),
        g_SceneColorBuffer.GetResource(),
        g_SceneDepthBuffer.GetResource(),
        g_DLSSMotionBuffer.GetResource(),
        Output.GetResource(),
        renderW,
        renderH,
        displayW,
        displayH,
        quality,
        jitterPixX,
        jitterPixY,
        s_ResetEvaluates > 0,
        static_cast<uint32_t>(Graphics::GetFrameCount()),
        Graphics::GetFrameTime() * 1000.0f,
        s_CameraNear,
        s_CameraFar,
        s_CameraVerticalFov,
    };

    if (!s_HaveFeature)
    {
        if (!s_Backend->createFeature(frame))
        {
            std::printf("[DLSS] %s CreateFeature failed — falling back to stock path.\n", s_Backend->name);
            s_Supported = false;
            s_Status    = UpscalerStatus::Broken;
            return false;
        }
        s_HaveFeature = true;
    }

    if (!s_Backend->evaluate(frame))
    {
        std::printf("[DLSS] %s Evaluate failed — falling back to stock path.\n", s_Backend->name);
        s_Supported = false;
        s_Status    = UpscalerStatus::Broken;
        return false;
    }

    if (s_ResetEvaluates > 0)
        --s_ResetEvaluates;

    // NGX issues raw D3D12 barriers outside MiniEngine's CommandContext tracking.
    // Re-declare the resources in the states NGX left them for subsequent MiniEngine use.
    Context.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(g_DLSSMotionBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.TransitionResource(Output,             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

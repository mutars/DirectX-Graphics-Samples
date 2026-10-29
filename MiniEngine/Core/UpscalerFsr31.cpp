// clang-format off
#include "pch.h"
#include "UpscalerBackend.h"
// clang-format on

#include <cstdio>

#if defined(VRTF_HAVE_FFX_API)
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/dx12/ffx_api_dx12.h>

#include <string>

namespace
{
    struct FfxApi
    {
        PfnFfxCreateContext  createContext{};
        PfnFfxDestroyContext destroyContext{};
        PfnFfxDispatch       dispatch{};
        PfnFfxQuery          query{};
    };

    FfxApi        s_Api;
    ID3D12Device* s_Device  = nullptr;
    ffxContext    s_Context = nullptr;

    template <class Function>
    bool resolve(HMODULE module, const char* name, Function& out)
    {
        out = reinterpret_cast<Function>(GetProcAddress(module, name));
        if (!out)
            std::printf("[FSR3.1] amd_fidelityfx_dx12.dll exports no %s\n", name);
        return out != nullptr;
    }

    uint32_t qualityMode(int quality)
    {
        switch (quality)
        {
        case 0:
            return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
        case 1:
            return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
        case 2:
            return FFX_UPSCALE_QUALITY_MODE_BALANCED;
        case 3:
            return FFX_UPSCALE_QUALITY_MODE_QUALITY;
        default:
            return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
        }
    }

    void message(uint32_t, const wchar_t* text)
    {
        std::printf("[FSR3.1] %ls\n", text);
    }

    DLSS::UpscalerStatus configure()
    {
        const std::wstring path   = std::wstring(DLSS::UpscalerModuleDirectory()) + L"amd_fidelityfx_dx12.dll";
        const HMODULE      module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module)
        {
            std::printf("[FSR3.1] %ls not loaded (%lu)\n", path.c_str(), GetLastError());
            return DLSS::UpscalerStatus::Broken;
        }
        const bool resolved = resolve(module, "ffxCreateContext", s_Api.createContext) && resolve(module, "ffxDestroyContext", s_Api.destroyContext)
            && resolve(module, "ffxDispatch", s_Api.dispatch) && resolve(module, "ffxQuery", s_Api.query);
        return resolved ? DLSS::UpscalerStatus::Active : DLSS::UpscalerStatus::Broken;
    }

    DLSS::UpscalerStatus initialize(ID3D12Device* device)
    {
        s_Device = device;
        std::printf("[FSR3.1] Initialized.\n");
        return DLSS::UpscalerStatus::Active;
    }

    void releaseFeature()
    {
        if (!s_Context)
            return;
        s_Api.destroyContext(&s_Context, nullptr);
        s_Context = nullptr;
    }

    void shutdown()
    {
        releaseFeature();
        s_Device = nullptr;
    }

    bool optimalRenderSize(uint32_t displayWidth, uint32_t displayHeight, int quality, uint32_t& renderWidth, uint32_t& renderHeight)
    {
        uint32_t width = 0, height = 0;
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query{};
        query.header.type      = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        query.displayWidth     = displayWidth;
        query.displayHeight    = displayHeight;
        query.qualityMode      = qualityMode(quality);
        query.pOutRenderWidth  = &width;
        query.pOutRenderHeight = &height;
        if (s_Api.query(nullptr, &query.header) != FFX_API_RETURN_OK || width == 0 || height == 0)
            return false;
        renderWidth  = width;
        renderHeight = height;
        return true;
    }

    bool createFeature(const DLSS::UpscalerFrame& frame)
    {
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device      = s_Device;

        ffxCreateContextDescUpscale desc{};
        desc.header.type    = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext   = &backend.header;
        desc.flags          = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        desc.maxRenderSize  = { frame.renderWidth, frame.renderHeight };
        desc.maxUpscaleSize = { frame.displayWidth, frame.displayHeight };
        desc.fpMessage      = message;
        const ffxReturnCode_t result = s_Api.createContext(&s_Context, &desc.header, nullptr);
        if (result != FFX_API_RETURN_OK)
        {
            std::printf("[FSR3.1] ffxCreateContext failed (%u)\n", result);
            s_Context = nullptr;
            return false;
        }
        return true;
    }

    bool evaluate(const DLSS::UpscalerFrame& frame)
    {
        ffxDispatchDescUpscale desc{};
        desc.header.type             = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.commandList             = frame.commandList;
        desc.color                   = ffxApiGetResourceDX12(frame.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.depth                   = ffxApiGetResourceDX12(frame.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.motionVectors           = ffxApiGetResourceDX12(frame.motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.output                  = ffxApiGetResourceDX12(frame.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        desc.jitterOffset            = { frame.jitterX, frame.jitterY };
        desc.motionVectorScale       = { 1.0f, 1.0f };
        desc.renderSize              = { frame.renderWidth, frame.renderHeight };
        desc.upscaleSize             = { frame.displayWidth, frame.displayHeight };
        desc.enableSharpening        = false;
        desc.sharpness               = 0.0f;
        desc.frameTimeDelta          = frame.frameTimeMs;
        desc.preExposure             = 1.0f;
        desc.reset                   = frame.reset;
        desc.cameraNear              = frame.cameraNear;
        desc.cameraFar               = frame.cameraFar;
        desc.cameraFovAngleVertical  = frame.cameraVerticalFov;
        desc.viewSpaceToMetersFactor = 1.0f;
        desc.flags                   = 0;
        const ffxReturnCode_t result = s_Api.dispatch(&s_Context, &desc.header);
        if (result != FFX_API_RETURN_OK)
        {
            std::printf("[FSR3.1] ffxDispatch failed (%u)\n", result);
            return false;
        }
        return true;
    }

    constexpr DLSS::UpscalerBackend kFsr31Backend{
        "fsr31", configure, initialize, shutdown, optimalRenderSize, createFeature, evaluate, releaseFeature,
    };
}
#else
namespace
{
    DLSS::UpscalerStatus fsr31Absent()
    {
        std::printf("[FSR3.1] this build has no FidelityFX API headers\n");
        return DLSS::UpscalerStatus::Broken;
    }

    constexpr DLSS::UpscalerBackend kFsr31Backend{ "fsr31", fsr31Absent };
}
#endif

const DLSS::UpscalerBackend& DLSS::Fsr31Backend()
{
    return kFsr31Backend;
}

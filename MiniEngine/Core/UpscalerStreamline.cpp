// clang-format off
#include "pch.h"
#include "UpscalerBackend.h"
// clang-format on

#include <cstdio>

#if defined(VRTF_HAVE_STREAMLINE)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>

#include <iterator>
#include <string>

namespace
{
    struct StreamlineApi
    {
        PFun_slInit*                   init{};
        PFun_slShutdown*               shutdown{};
        PFun_slSetD3DDevice*           setD3DDevice{};
        PFun_slIsFeatureSupported*     isFeatureSupported{};
        PFun_slGetFeatureFunction*     getFeatureFunction{};
        PFun_slSetTag*                 setTag{};
        PFun_slSetConstants*           setConstants{};
        PFun_slGetNewFrameToken*       getNewFrameToken{};
        PFun_slEvaluateFeature*        evaluateFeature{};
        PFun_slDLSSGetOptimalSettings* dlssGetOptimalSettings{};
        PFun_slDLSSSetOptions*         dlssSetOptions{};
    };

    StreamlineApi        s_Api;
    bool                 s_Initialized = false;
    std::wstring         s_PluginDirectory;
    const wchar_t*       s_PluginPaths[1]{};
    const sl::Feature    s_Features[] = { sl::kFeatureDLSS };
    const sl::ViewportHandle s_Viewport{ 0u };

    void logMessage(sl::LogType type, const char* message)
    {
        if (type != sl::LogType::eInfo)
            std::printf("[Streamline] %s", message);
    }

    template <class Function>
    bool resolve(HMODULE module, const char* name, Function*& out)
    {
        out = reinterpret_cast<Function*>(GetProcAddress(module, name));
        if (!out)
            std::printf("[Streamline] sl.interposer.dll exports no %s\n", name);
        return out != nullptr;
    }

    template <class Function>
    bool resolveFeatureFunction(const char* name, Function*& out)
    {
        void* function = nullptr;
        const sl::Result result = s_Api.getFeatureFunction(sl::kFeatureDLSS, name, function);
        out = result == sl::Result::eOk ? reinterpret_cast<Function*>(function) : nullptr;
        if (!out)
            std::printf("[Streamline] slGetFeatureFunction(%s) failed (%d)\n", name, static_cast<int>(result));
        return out != nullptr;
    }

    sl::DLSSMode dlssMode(int quality)
    {
        switch (quality)
        {
        case 0:
            return sl::DLSSMode::eUltraPerformance;
        case 1:
            return sl::DLSSMode::eMaxPerformance;
        case 2:
            return sl::DLSSMode::eBalanced;
        case 3:
            return sl::DLSSMode::eMaxQuality;
        default:
            return sl::DLSSMode::eDLAA;
        }
    }

    sl::DLSSOptions dlssOptions(uint32_t displayWidth, uint32_t displayHeight, int quality)
    {
        sl::DLSSOptions options{};
        options.mode            = dlssMode(quality);
        options.outputWidth     = displayWidth;
        options.outputHeight    = displayHeight;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        return options;
    }

    sl::float4x4 rowMajor(DirectX::FXMMATRIX matrix)
    {
        DirectX::XMFLOAT4X4 stored;
        DirectX::XMStoreFloat4x4(&stored, matrix);
        sl::float4x4 out;
        for (uint32_t row = 0; row < 4; ++row)
            out.setRow(row, sl::float4(stored.m[row][0], stored.m[row][1], stored.m[row][2], stored.m[row][3]));
        return out;
    }

    sl::Constants constantsOf(const DLSS::UpscalerFrame& frame)
    {
        const float             aspect     = static_cast<float>(frame.renderWidth) / static_cast<float>(frame.renderHeight);
        const DirectX::XMMATRIX viewToClip = DirectX::XMMatrixPerspectiveFovRH(frame.cameraVerticalFov, aspect, frame.cameraFar, frame.cameraNear);

        sl::Constants constants{};
        constants.cameraViewToClip     = rowMajor(viewToClip);
        constants.clipToCameraView     = rowMajor(DirectX::XMMatrixInverse(nullptr, viewToClip));
        constants.clipToLensClip       = rowMajor(DirectX::XMMatrixIdentity());
        constants.clipToPrevClip       = rowMajor(DirectX::XMMatrixIdentity());
        constants.prevClipToClip       = rowMajor(DirectX::XMMatrixIdentity());
        constants.jitterOffset         = sl::float2(frame.jitterX, frame.jitterY);
        constants.mvecScale            = sl::float2(1.0f / static_cast<float>(frame.renderWidth), 1.0f / static_cast<float>(frame.renderHeight));
        constants.cameraPinholeOffset  = sl::float2(0.0f, 0.0f);
        constants.cameraPos            = sl::float3(0.0f, 0.0f, 0.0f);
        constants.cameraUp             = sl::float3(0.0f, 1.0f, 0.0f);
        constants.cameraRight          = sl::float3(1.0f, 0.0f, 0.0f);
        constants.cameraFwd            = sl::float3(0.0f, 0.0f, -1.0f);
        constants.cameraNear           = frame.cameraNear;
        constants.cameraFar            = frame.cameraFar;
        constants.cameraFOV            = frame.cameraVerticalFov;
        constants.cameraAspectRatio    = aspect;
        constants.depthInverted        = sl::Boolean::eTrue;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D      = sl::Boolean::eFalse;
        constants.reset                = frame.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        return constants;
    }

    DLSS::UpscalerStatus configure()
    {
        s_PluginDirectory               = std::wstring(DLSS::UpscalerModuleDirectory()) + L"streamline";
        const std::wstring interposer   = s_PluginDirectory + L"\\sl.interposer.dll";
        const HMODULE      module       = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module)
        {
            std::printf("[Streamline] %ls not loaded (%lu)\n", interposer.c_str(), GetLastError());
            return DLSS::UpscalerStatus::Broken;
        }
        const bool resolved = resolve(module, "slInit", s_Api.init) && resolve(module, "slShutdown", s_Api.shutdown)
            && resolve(module, "slSetD3DDevice", s_Api.setD3DDevice) && resolve(module, "slIsFeatureSupported", s_Api.isFeatureSupported)
            && resolve(module, "slGetFeatureFunction", s_Api.getFeatureFunction) && resolve(module, "slSetTag", s_Api.setTag)
            && resolve(module, "slSetConstants", s_Api.setConstants) && resolve(module, "slGetNewFrameToken", s_Api.getNewFrameToken)
            && resolve(module, "slEvaluateFeature", s_Api.evaluateFeature);
        if (!resolved)
            return DLSS::UpscalerStatus::Broken;

        s_PluginPaths[0] = s_PluginDirectory.c_str();
        sl::Preferences preferences{};
        preferences.logLevel           = sl::LogLevel::eDefault;
        preferences.logMessageCallback = logMessage;
        preferences.pathsToPlugins     = s_PluginPaths;
        preferences.numPathsToPlugins  = 1;
        preferences.flags              = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
        preferences.featuresToLoad     = s_Features;
        preferences.numFeaturesToLoad  = 1;
        preferences.engine             = sl::EngineType::eCustom;
        preferences.engineVersion      = "MiniEngine";
        preferences.projectId          = "5c1e7d2a-93b4-4f60-8e1d-2a7b9c4f6e30";
        preferences.renderAPI          = sl::RenderAPI::eD3D12;
        const sl::Result result        = s_Api.init(preferences, sl::kSDKVersion);
        if (result != sl::Result::eOk)
        {
            std::printf("[Streamline] slInit failed (%d)\n", static_cast<int>(result));
            return DLSS::UpscalerStatus::Broken;
        }
        s_Initialized = true;
        return DLSS::UpscalerStatus::Active;
    }

    DLSS::UpscalerStatus initialize(ID3D12Device* device)
    {
        const sl::Result deviceResult = s_Api.setD3DDevice(device);
        if (deviceResult != sl::Result::eOk)
        {
            std::printf("[Streamline] slSetD3DDevice failed (%d)\n", static_cast<int>(deviceResult));
            return DLSS::UpscalerStatus::Broken;
        }

        LUID            luid = device->GetAdapterLuid();
        sl::AdapterInfo adapter{};
        adapter.deviceLUID            = reinterpret_cast<uint8_t*>(&luid);
        adapter.deviceLUIDSizeInBytes = sizeof(luid);
        const sl::Result supported    = s_Api.isFeatureSupported(sl::kFeatureDLSS, adapter);
        if (supported != sl::Result::eOk)
        {
            std::printf("[Streamline] DLSS not supported on this adapter (%d)\n", static_cast<int>(supported));
            return DLSS::UpscalerStatus::Unsupported;
        }

        if (!resolveFeatureFunction("slDLSSGetOptimalSettings", s_Api.dlssGetOptimalSettings) || !resolveFeatureFunction("slDLSSSetOptions", s_Api.dlssSetOptions))
            return DLSS::UpscalerStatus::Broken;
        std::printf("[Streamline] Initialized; DLSS supported (manual hooking).\n");
        return DLSS::UpscalerStatus::Active;
    }

    void shutdown()
    {
        if (!s_Initialized)
            return;
        s_Api.shutdown();
        s_Initialized = false;
    }

    bool optimalRenderSize(uint32_t displayWidth, uint32_t displayHeight, int quality, uint32_t& renderWidth, uint32_t& renderHeight)
    {
        sl::DLSSOptimalSettings settings{};
        if (s_Api.dlssGetOptimalSettings(dlssOptions(displayWidth, displayHeight, quality), settings) != sl::Result::eOk || settings.optimalRenderWidth == 0
            || settings.optimalRenderHeight == 0)
            return false;
        renderWidth  = settings.optimalRenderWidth;
        renderHeight = settings.optimalRenderHeight;
        return true;
    }

    bool createFeature(const DLSS::UpscalerFrame&)
    {
        return true;
    }

    bool evaluate(const DLSS::UpscalerFrame& frame)
    {
        const sl::Result optionsResult = s_Api.dlssSetOptions(s_Viewport, dlssOptions(frame.displayWidth, frame.displayHeight, frame.quality));
        if (optionsResult != sl::Result::eOk)
        {
            std::printf("[Streamline] slDLSSSetOptions failed (%d)\n", static_cast<int>(optionsResult));
            return false;
        }

        sl::Resource      color{ sl::ResourceType::eTex2d, frame.color, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        sl::Resource      depth{ sl::ResourceType::eTex2d, frame.depth, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        sl::Resource      motionVectors{ sl::ResourceType::eTex2d, frame.motionVectors, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        sl::Resource      output{ sl::ResourceType::eTex2d, frame.output, nullptr, nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        const sl::Extent  renderExtent{ 0, 0, frame.renderWidth, frame.renderHeight };
        const sl::Extent  displayExtent{ 0, 0, frame.displayWidth, frame.displayHeight };
        const sl::ResourceTag tags[] = {
            { &color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
            { &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
            { &motionVectors, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
            { &output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &displayExtent },
        };
        sl::CommandBuffer* const commandList = frame.commandList;
        const sl::Result         tagResult   = s_Api.setTag(s_Viewport, tags, static_cast<uint32_t>(std::size(tags)), commandList);
        if (tagResult != sl::Result::eOk)
        {
            std::printf("[Streamline] slSetTag failed (%d)\n", static_cast<int>(tagResult));
            return false;
        }

        sl::FrameToken*  token      = nullptr;
        uint32_t         frameIndex = frame.frameIndex;
        const sl::Result tokenResult = s_Api.getNewFrameToken(token, &frameIndex);
        if (tokenResult != sl::Result::eOk || !token)
        {
            std::printf("[Streamline] slGetNewFrameToken failed (%d)\n", static_cast<int>(tokenResult));
            return false;
        }

        const sl::Result constantsResult = s_Api.setConstants(constantsOf(frame), *token, s_Viewport);
        if (constantsResult != sl::Result::eOk)
        {
            std::printf("[Streamline] slSetConstants failed (%d)\n", static_cast<int>(constantsResult));
            return false;
        }

        const sl::BaseStructure* inputs[] = { &s_Viewport };
        const sl::Result         result   = s_Api.evaluateFeature(sl::kFeatureDLSS, *token, inputs, static_cast<uint32_t>(std::size(inputs)), commandList);
        if (result != sl::Result::eOk)
        {
            std::printf("[Streamline] slEvaluateFeature failed (%d)\n", static_cast<int>(result));
            return false;
        }
        return true;
    }

    void releaseFeature()
    {
    }

    constexpr DLSS::UpscalerBackend kStreamlineBackend{
        "streamline", configure, initialize, shutdown, optimalRenderSize, createFeature, evaluate, releaseFeature,
    };
}
#else
namespace
{
    DLSS::UpscalerStatus streamlineAbsent()
    {
        std::printf("[Streamline] this build has no Streamline headers\n");
        return DLSS::UpscalerStatus::Broken;
    }

    constexpr DLSS::UpscalerBackend kStreamlineBackend{ "streamline", streamlineAbsent };
}
#endif

const DLSS::UpscalerBackend& DLSS::StreamlineBackend()
{
    return kStreamlineBackend;
}

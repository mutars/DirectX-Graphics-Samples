#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace DLSS
{
    struct UpscalerFrame
    {
        ID3D12GraphicsCommandList* commandList;
        ID3D12Resource*            color;
        ID3D12Resource*            depth;
        ID3D12Resource*            motionVectors;
        ID3D12Resource*            output;
        uint32_t                   renderWidth;
        uint32_t                   renderHeight;
        uint32_t                   displayWidth;
        uint32_t                   displayHeight;
        int                        quality;
        float                      jitterX;
        float                      jitterY;
        bool                       reset;
        uint32_t                   frameIndex;
        float                      frameTimeMs;
        float                      cameraNear;
        float                      cameraFar;
        float                      cameraVerticalFov;
    };

    enum class UpscalerStatus : int
    {
        Active      = 0,
        Unsupported = 1,
        Broken      = 2,
    };

    struct UpscalerBackend
    {
        const char* name;
        UpscalerStatus (*configure)();
        UpscalerStatus (*initialize)(ID3D12Device* device);
        void (*shutdown)();
        bool (*optimalRenderSize)(uint32_t displayWidth, uint32_t displayHeight, int quality, uint32_t& renderWidth, uint32_t& renderHeight);
        bool (*createFeature)(const UpscalerFrame& frame);
        bool (*evaluate)(const UpscalerFrame& frame);
        void (*releaseFeature)();
    };

    const UpscalerBackend& NgxBackend();
    const UpscalerBackend& StreamlineBackend();
    const UpscalerBackend& Fsr31Backend();

    const wchar_t* UpscalerModuleDirectory();
}

//
// DLSS (NVIDIA NGX super-resolution) for MiniEngine.
//
// Peer of TemporalEffects: when DLSS::Enable && DLSS::IsSupported(), the engine renders the
// scene at a low NATIVE resolution and DLSS::Resolve upscales the pre-tonemap HDR scene color
// to DISPLAY resolution -- replacing both the TAA resolve and the present-time spatial upscale.
// The public surface deliberately exposes NO NGX types (the SDK is included only in DLSS.cpp),
// so consumers stay decoupled from the vendor headers and from whether the SDK is present.
//

#pragma once

#include "EngineTuning.h"

#include <cstdint>

class CommandContext;
class ColorBuffer;

namespace DLSS
{
    // TAA-style temporal upscaler toggle. Default false -> the engine is byte-identical to
    // stock MiniEngine when DLSS is off.
    extern BoolVar Enable;

    // Maps to NVSDK_NGX_PerfQuality_Value in DLSS.cpp (UltraPerformance..DLAA). Selecting a
    // mode changes the optimal render resolution returned by GetRenderDimensions.
    extern EnumVar QualityMode;

    // One-time NGX init (device-level). Must run after Graphics::Initialize (needs g_Device) and
    // must NEVER throw: on any failure (non-RTX, driver/SDK missing) it leaves IsSupported()==false
    // so all callers fall back to the stock path. Safe to call once per process.
    void Initialize(void);

    // Releases the live feature (if any) and tears down NGX. Must run before Graphics::Shutdown.
    void Shutdown(void);

    // True only when NGX initialized and the SuperSampling feature is available on this GPU/driver.
    // Callers MUST gate Resolve / GetRenderDimensions on this; false on every non-RTX machine.
    bool IsSupported(void);

    // Optimal render (native) dimensions for the current QualityMode and the given display size,
    // via NGX_DLSS_GET_OPTIMAL_SETTINGS. Drives g_NativeResolutionOverride. If !IsSupported(),
    // returns the display size unchanged (no scaling).
    void GetRenderDimensions(uint32_t DisplayWidth, uint32_t DisplayHeight, uint32_t& RenderWidth, uint32_t& RenderHeight);

    // Per-frame evaluate. Inputs are the engine globals at their native render resolution
    // (g_SceneColorBuffer pre-tonemap HDR, g_SceneDepthBuffer, the converted unjittered motion
    // vectors, and the current TAA jitter offset); Output is the display-resolution UAV target.
    // Lazily (re)creates the NGX feature whenever the render/display dimensions or QualityMode
    // change, and flags an internal history reset on that frame. Returns false (and latches
    // IsSupported() to false) when !IsSupported() or feature-create/evaluate fails, so the caller
    // falls back to the stock present path for this and every subsequent frame.
    bool Resolve(CommandContext& Context, ColorBuffer& Output);
}

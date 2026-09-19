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

#include <cstdint>

namespace Display
{
    // VRTF: what a replacement swapchain differs from the boot one by. keepOldChain keeps the
    // outgoing chain referenced (modelling a game that has not dropped it yet); secondQueue creates
    // the replacement on a private HIGH-priority DIRECT queue (the FidelityFX frame-interpolation
    // shape) instead of the graphics queue. No size: always the current g_DisplayWidth/Height.
    struct RecreateRequest
    {
        uint32_t keepOldChain;
        uint32_t secondQueue;
    };

    void Initialize(void);
    void Shutdown(void);
    void Resize(uint32_t width, uint32_t height);
    void Present(void);

    // VRTF: replaces the live swapchain on the SAME HWND. Returns the DXGI create HRESULT verbatim
    // (`long`, so this header stays windows.h-free) and never asserts it -- a refusal is the
    // observable the recreate gate measures, and it leaves no chain, which makes Present a no-op.
    long Recreate(const RecreateRequest& request);

    // VRTF: drops the chain a keepOldChain recreate held onto. False when none is held.
    bool ReleaseKeptSwapChain(void);
}

namespace Graphics
{
    extern uint32_t g_DisplayWidth;
    extern uint32_t g_DisplayHeight;
    extern bool g_bEnableHDROutput;

    // Returns the number of elapsed frames since application start
    uint64_t GetFrameCount(void);

    // The amount of time elapsed during the last completed frame.  The CPU and/or
    // GPU may be idle during parts of the frame.  The frame time measures the time
    // between calls to present each frame.
    float GetFrameTime(void);

    // The total number of frames per second
    float GetFrameRate(void);

    extern bool g_bEnableHDROutput;
}

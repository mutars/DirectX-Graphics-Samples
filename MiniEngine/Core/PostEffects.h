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

#include "pch.h"
#include "EngineTuning.h"

class ComputeContext;
class ColorBuffer;

namespace PostEffects
{
    extern BoolVar EnableHDR;			// Turn on tone mapping features

    // Tone mapping parameters
    extern ExpVar Exposure;				// Brightness scaler when adapative exposure is disabled
    extern BoolVar EnableAdaptation;	// Automatically adjust brightness based on perceived luminance

    // Adapation parameters
    extern ExpVar MinExposure;
    extern ExpVar MaxExposure;
    extern NumVar TargetLuminance;
    extern NumVar AdaptationRate;

    // Bloom parameters
    extern BoolVar BloomEnable;
    extern NumVar BloomThreshold;
    extern NumVar BloomStrength;

    void Initialize( void );
    void Shutdown( void );

    // Render on native g_SceneColorBuffer (stock path; DLSS off). Byte-identical to original.
    void Render( void );

    // Render on an explicit target (DLSS on: pass g_DLSSOutputBuffer at display res).
    void Render( ColorBuffer& SceneColor );

    // Copy the contents of the post effects buffer onto the main scene buffer
    void CopyBackPostBuffer( ComputeContext& Context );
}

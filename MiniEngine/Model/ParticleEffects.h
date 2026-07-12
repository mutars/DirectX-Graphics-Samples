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

#include <string>

namespace ParticleEffects
{
    void InitFromJSON(const std::wstring& InitJsonFile);
    // VRTF: releases the static particle TextureRefs BEFORE TextureManager::Shutdown --
    // otherwise they destruct at DLL detach against an already-cleared cache (use-after-free).
    void Shutdown(void);
}

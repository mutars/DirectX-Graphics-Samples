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

#include "pch.h"
#include "Display.h"
#include "GraphicsCore.h"
#include "BufferManager.h"
#include "ColorBuffer.h"
#include "SystemTime.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "ImageScaling.h"
#include "TemporalEffects.h"
#include "DLSS.h"

#pragma comment(lib, "dxgi.lib") 

// This macro determines whether to detect if there is an HDR display and enable HDR10 output.
// Currently, with HDR display enabled, the pixel magnfication functionality is broken.
#define CONDITIONALLY_ENABLE_HDR_OUTPUT 1

namespace GameCore { extern HWND g_hWnd; }

#include "CompiledShaders/ScreenQuadPresentVS.h"
#include "CompiledShaders/BufferCopyPS.h"
#include "CompiledShaders/PresentSDRPS.h"
#include "CompiledShaders/PresentHDRPS.h"
#include "CompiledShaders/CompositeSDRPS.h"
#include "CompiledShaders/ScaleAndCompositeSDRPS.h"
#include "CompiledShaders/CompositeHDRPS.h"
#include "CompiledShaders/BlendUIHDRPS.h"
#include "CompiledShaders/ScaleAndCompositeHDRPS.h"
#include "CompiledShaders/MagnifyPixelsPS.h"
#include "CompiledShaders/GenerateMipsLinearCS.h"
#include "CompiledShaders/GenerateMipsLinearOddCS.h"
#include "CompiledShaders/GenerateMipsLinearOddXCS.h"
#include "CompiledShaders/GenerateMipsLinearOddYCS.h"
#include "CompiledShaders/GenerateMipsGammaCS.h"
#include "CompiledShaders/GenerateMipsGammaOddCS.h"
#include "CompiledShaders/GenerateMipsGammaOddXCS.h"
#include "CompiledShaders/GenerateMipsGammaOddYCS.h"

#define SWAP_CHAIN_BUFFER_COUNT 3

DXGI_FORMAT SwapChainFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

using namespace Math;
using namespace ImageScaling;
using namespace Graphics;

namespace
{
    float s_FrameTime = 0.0f;
    uint64_t s_FrameIndex = 0;
    int64_t s_FrameStartTick = 0;

    BoolVar s_EnableVSync("Timing/VSync", true);
    BoolVar s_LimitTo30Hz("Timing/Limit To 30Hz", false);
    BoolVar s_DropRandomFrames("Timing/Drop Random Frames", false);
}

namespace Graphics
{
    void PreparePresentSDR();
    void PreparePresentHDR();
    void CompositeOverlays( GraphicsContext& Context );

    enum eResolution { k720p, k900p, k1080p, k1440p, k1800p, k2160p };
    enum eEQAAQuality { kEQAA1x1, kEQAA1x8, kEQAA1x16 };

    const uint32_t kNumPredefinedResolutions = 6;

    const char* ResolutionLabels[] = { "1280x720", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160" };
    EnumVar NativeResolution("Graphics/Display/Native Resolution", k1080p, kNumPredefinedResolutions, ResolutionLabels);
#ifdef _GAMING_DESKTOP
    // This can set the window size to common dimensions.  It's also possible for the window to take on other dimensions
    // through resizing or going full-screen.
    EnumVar DisplayResolution("Graphics/Display/Display Resolution", k1080p, kNumPredefinedResolutions, ResolutionLabels);
#endif

    bool g_bEnableHDROutput = false;
    NumVar g_HDRPaperWhite("Graphics/Display/Paper White (nits)", 200.0f, 100.0f, 500.0f, 50.0f);
    NumVar g_MaxDisplayLuminance("Graphics/Display/Peak Brightness (nits)", 1000.0f, 500.0f, 10000.0f, 100.0f);
    const char* HDRModeLabels[] = { "HDR", "SDR", "Side-by-Side" };
    EnumVar HDRDebugMode("Graphics/Display/HDR Debug Mode", 0, 3, HDRModeLabels);

    uint32_t g_NativeWidth = 0;
    uint32_t g_NativeHeight = 0;
    // VRTF: when nonzero, overrides the NativeResolution EnumVar so the scene can render at an
    // arbitrary HMD per-eye size that survives Display::Present's per-frame SetNativeResolution snap.
    uint32_t g_NativeResolutionOverrideWidth = 0;
    uint32_t g_NativeResolutionOverrideHeight = 0;
    // VRTF: direct-topology HUD tail (null = stock path). When set, PreparePresentSDR composes
    // PresentSource into g_PreDisplayBuffer, invokes the hook so the consumer draws the HUD
    // straight onto it, then runs a profile-neutral final output pass to the display plane.
    void (*g_vrtfDirectTailHook)(GraphicsContext&, ColorBuffer& preDisplay) = nullptr;
    uint32_t g_DisplayWidth = 1920;
    uint32_t g_DisplayHeight = 1080;
    ColorBuffer g_PreDisplayBuffer;

    void ResolutionToUINT(eResolution res, uint32_t& width, uint32_t& height)
    {
        switch (res)
        {
        default:
        case k720p:
            width = 1280;
            height = 720;
            break;
        case k900p:
            width = 1600;
            height = 900;
            break;
        case k1080p:
            width = 1920;
            height = 1080;
            break;
        case k1440p:
            width = 2560;
            height = 1440;
            break;
        case k1800p:
            width = 3200;
            height = 1800;
            break;
        case k2160p:
            width = 3840;
            height = 2160;
            break;
        }
    }

    void SetNativeResolution(void)
    {
        uint32_t NativeWidth, NativeHeight;

        if (g_NativeResolutionOverrideWidth != 0 && g_NativeResolutionOverrideHeight != 0)
        {
            NativeWidth = g_NativeResolutionOverrideWidth;
            NativeHeight = g_NativeResolutionOverrideHeight;
        }
        else
        {
            ResolutionToUINT(eResolution((int)NativeResolution), NativeWidth, NativeHeight);
        }

        if (g_NativeWidth == NativeWidth && g_NativeHeight == NativeHeight)
            return;
        DEBUGPRINT("Changing native resolution to %ux%u", NativeWidth, NativeHeight);

        g_NativeWidth = NativeWidth;
        g_NativeHeight = NativeHeight;

        g_CommandManager.IdleGPU();

        InitializeRenderingBuffers(NativeWidth, NativeHeight);
    }

    void SetDisplayResolution(void)
    {
#ifdef _GAMING_DESKTOP
        static int SelectedDisplayRes = DisplayResolution;
        if (SelectedDisplayRes == DisplayResolution)
            return;

        SelectedDisplayRes = DisplayResolution;
        ResolutionToUINT((eResolution)SelectedDisplayRes, g_DisplayWidth, g_DisplayHeight);
        DEBUGPRINT("Changing display resolution to %ux%u", g_DisplayWidth, g_DisplayHeight);

        g_CommandManager.IdleGPU();

        Display::Resize(g_DisplayWidth, g_DisplayHeight);

        SetWindowPos(GameCore::g_hWnd, 0, 0, 0, g_DisplayWidth, g_DisplayHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    }

    ColorBuffer g_DisplayPlane[SWAP_CHAIN_BUFFER_COUNT];
    UINT g_CurrentBuffer = 0;

    IDXGISwapChain1* s_SwapChain1 = nullptr;

    // VRTF: the recreate seam's state. s_KeptSwapChain holds the outgoing chain of a keepOldChain
    // recreate (a game that replaced its chain without releasing the old one yet). s_PresentQueue is
    // the private present queue a secondQueue recreate creates the chain on, plus the copy machinery
    // that queue needs (see BindDisplayPlanes). The queue OUTLIVES the mode -- it is created lazily
    // and released at Shutdown, while a later recreate can put the chain back on the graphics queue --
    // so s_ChainOnPresentQueue, not the queue pointer, is what says which shape is live.
    IDXGISwapChain1*    s_KeptSwapChain = nullptr;
    ID3D12CommandQueue* s_PresentQueue = nullptr;
    ID3D12Fence*        s_PresentFence = nullptr;
    UINT64              s_PresentFenceValue = 0;

    bool                       s_ChainOnPresentQueue = false;
    ID3D12Resource*            s_RealBackBuffer[SWAP_CHAIN_BUFFER_COUNT] = {};
    ID3D12CommandAllocator*    s_PresentAllocator = nullptr;
    ID3D12GraphicsCommandList* s_PresentList = nullptr;
    ID3D12Fence*               s_PresentCopyFence = nullptr;
    UINT64                     s_PresentCopyFenceValue = 0;
    HANDLE                     s_PresentCopyEvent = nullptr;

    RootSignature s_PresentRS;
    GraphicsPSO s_BlendUIPSO(L"Core: BlendUI");
    GraphicsPSO s_BlendUIHDRPSO(L"Core: BlendUIHDR");
    GraphicsPSO PresentSDRPS(L"Core: PresentSDR");
    GraphicsPSO PresentHDRPS(L"Core: PresentHDR");
    GraphicsPSO CompositeSDRPS(L"Core: CompositeSDR");
    GraphicsPSO ScaleAndCompositeSDRPS(L"Core: ScaleAndCompositeSDR");
    GraphicsPSO CompositeHDRPS(L"Core: CompositeHDR");
    GraphicsPSO ScaleAndCompositeHDRPS(L"Core: ScaleAndCompositeHDR");
    GraphicsPSO MagnifyPixelsPS(L"Core: MagnifyPixels");

    const char* FilterLabels[] = { "Bilinear", "Sharpening", "Bicubic", "Lanczos" };
    EnumVar UpsampleFilter("Graphics/Display/Scaling Filter", kSharpening, kFilterCount, FilterLabels);

    enum DebugZoomLevel { kDebugZoomOff, kDebugZoom2x, kDebugZoom4x, kDebugZoom8x, kDebugZoom16x, kDebugZoomCount };
    const char* DebugZoomLabels[] = { "Off", "2x Zoom", "4x Zoom", "8x Zoom", "16x Zoom" };
    EnumVar DebugZoom("Graphics/Display/Magnify Pixels", kDebugZoomOff, kDebugZoomCount, DebugZoomLabels);
}

// VRTF: the ONE swapchain desc both Initialize and Recreate create from. A replacement chain that
// disagreed with the boot chain would make the recreate gates measure that difference instead of the
// interception seam under test.
static DXGI_SWAP_CHAIN_DESC1 VrtfSwapChainDesc(void)
{
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = g_DisplayWidth;
    swapChainDesc.Height = g_DisplayHeight;
    swapChainDesc.Format = SwapChainFormat;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    // VRTF: STRETCH, not NONE -- the faithful-resolution seam resizes the swapchain to the per-eye
    // G size while the window keeps its desktop size; NONE displays an unscaled top-left CROP that
    // hides most of the frame (HUD included). STRETCH shows the whole frame, aspect-squashed.
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // VRTF: UNSPECIFIED, not IGNORE. On an IGNORE flip chain DWM holds a second set of the chain's
    // buffers, and that set stays charged to dwm.exe after the chain, its window and the process are
    // gone, until DWM restarts -- window shown, hidden or cloaked.
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    return swapChainDesc;
}

// VRTF: the present queue's own CPU sync point, used before reusing the copy list and before
// releasing anything a copy reads or writes. The queue is private, so g_CommandManager::IdleGPU says
// nothing about it.
static void IdlePresentQueue(void)
{
    if (s_PresentQueue == nullptr || s_PresentCopyFence == nullptr || s_PresentCopyEvent == nullptr)
        return;

    ++s_PresentCopyFenceValue;
    s_PresentQueue->Signal(s_PresentCopyFence, s_PresentCopyFenceValue);
    if (s_PresentCopyFence->GetCompletedValue() < s_PresentCopyFenceValue)
    {
        s_PresentCopyFence->SetEventOnCompletion(s_PresentCopyFenceValue, s_PresentCopyEvent);
        WaitForSingleObject(s_PresentCopyEvent, INFINITE);
    }
}

static void ReleaseDisplayPlanes(void)
{
    IdlePresentQueue();

    for (uint32_t i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
    {
        g_DisplayPlane[i].Destroy();
        if (s_RealBackBuffer[i] != nullptr)
        {
            s_RealBackBuffer[i]->Release();
            s_RealBackBuffer[i] = nullptr;
        }
    }
}

// VRTF: in present-queue mode the engine renders into OWNED replacement planes and the present queue
// copies each one into the real back buffer at present time -- the FidelityFX interpolation shape
// (FrameInterpolationSwapchainDX12.cpp: replacementSwapBuffers + presentPassthrough). Binding the
// planes straight to the chain instead removes the device on the first present (measured:
// GetDeviceRemovedReason 0x887A002B, DXGI_ERROR_ACCESS_DENIED): DXGI refuses writes to a flip-model
// back buffer from any queue but the chain's own, and MiniEngine's whole present path records on the
// graphics queue. The replacement carries the real buffers' exact size and format, which is what
// CopyResource requires.
static void BindDisplayPlanes(void)
{
    for (uint32_t i = 0; i < SWAP_CHAIN_BUFFER_COUNT; ++i)
    {
        if (s_ChainOnPresentQueue)
        {
            g_DisplayPlane[i].Create(L"VRTF Replacement BackBuffer", g_DisplayWidth, g_DisplayHeight, 1, SwapChainFormat);
            ASSERT_SUCCEEDED(s_SwapChain1->GetBuffer(i, MY_IID_PPV_ARGS(&s_RealBackBuffer[i])));
        }
        else
        {
            ComPtr<ID3D12Resource> DisplayPlane;
            ASSERT_SUCCEEDED(s_SwapChain1->GetBuffer(i, MY_IID_PPV_ARGS(&DisplayPlane)));
            g_DisplayPlane[i].CreateFromSwapChain(L"Primary SwapChain Buffer", DisplayPlane.Detach());
        }
    }
}

void Display::Resize(uint32_t width, uint32_t height)
{
    g_CommandManager.IdleGPU();

    g_DisplayWidth = width;
    g_DisplayHeight = height;

    DEBUGPRINT("Changing display resolution to %ux%u", width, height);

    g_PreDisplayBuffer.Create(L"PreDisplay Buffer", width, height, 1, SwapChainFormat);

    // VRTF: before ResizeBuffers, never after -- present-queue mode holds one reference per real back
    // buffer, and an outstanding back-buffer reference makes ResizeBuffers fail.
    ReleaseDisplayPlanes();

    ASSERT(s_SwapChain1 != nullptr);
    ASSERT_SUCCEEDED(s_SwapChain1->ResizeBuffers(SWAP_CHAIN_BUFFER_COUNT, width, height, SwapChainFormat, 0));

    BindDisplayPlanes();

    g_CurrentBuffer = 0;

    g_CommandManager.IdleGPU();

    ResizeDisplayDependentBuffers(g_NativeWidth, g_NativeHeight);
}

// Initialize the DirectX resources required to run.
void Display::Initialize(void)
{
    ASSERT(s_SwapChain1 == nullptr, "Graphics has already been initialized");

    // VRTF: reroute cells force an alpha-capable 8-bit swapchain. The default
    // R10G10B10A2's 2-bit alpha cannot carry HUD coverage in the carve-back layer
    // (the layer must match the HUD target's format exactly -- vr-test-framework
    // docs/hud-detection/design/4 §0), so those cells opt into R8G8B8A8 end to end.
    {
        char vrtf8bit[8] = {};
        if (GetEnvironmentVariableA("VRTF_HUD_8BIT", vrtf8bit, sizeof(vrtf8bit)) > 0 && vrtf8bit[0] == '1')
            SwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
    ASSERT_SUCCEEDED(CreateDXGIFactory2(0, MY_IID_PPV_ARGS(&dxgiFactory)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = VrtfSwapChainDesc();

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsSwapChainDesc = {};
    fsSwapChainDesc.Windowed = TRUE;

    ASSERT_SUCCEEDED(dxgiFactory->CreateSwapChainForHwnd(
        g_CommandManager.GetCommandQueue(),
        GameCore::g_hWnd,
        &swapChainDesc,
        &fsSwapChainDesc,
        nullptr,
        &s_SwapChain1));

#if CONDITIONALLY_ENABLE_HDR_OUTPUT
    {
        // VRTF: the test engine ALWAYS forces SDR. Every exact-color oracle (reprojection
        // markers, DDR fill, truth masks) and every blessed capture is sRGB-calibrated; with the
        // desktop in HDR this detect flips the present path to Rec2020/PQ and silently re-encodes
        // all captures (live 2026-08-07: Windows HDR turned three exact-color gates red while
        // tolerance-based gates passed). The HUD fixture's direct/truth paths also patch only
        // PreparePresentSDR.
        const bool vrtfForceSdr = true;

        IDXGISwapChain4* swapChain = (IDXGISwapChain4*)s_SwapChain1;
        ComPtr<IDXGIOutput> output;
        ComPtr<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 outputDesc;
        UINT colorSpaceSupport;

        // Query support for ST.2084 on the display and set the color space accordingly
        if (!vrtfForceSdr &&
            SUCCEEDED(swapChain->GetContainingOutput(&output)) && SUCCEEDED(output.As(&output6)) &&
            SUCCEEDED(output6->GetDesc1(&outputDesc)) && outputDesc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 &&
            SUCCEEDED(swapChain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &colorSpaceSupport)) &&
            (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
            SUCCEEDED(swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)))
        {
            g_bEnableHDROutput = true;
        }
    }
#endif // End CONDITIONALLY_ENABLE_HDR_OUTPUT

    BindDisplayPlanes();

    s_PresentRS.Reset(4, 2);
    s_PresentRS[0].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2);
    s_PresentRS[1].InitAsConstants(0, 6, D3D12_SHADER_VISIBILITY_ALL);
    s_PresentRS[2].InitAsBufferSRV(2, D3D12_SHADER_VISIBILITY_PIXEL);
    s_PresentRS[3].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 2);
    s_PresentRS.InitStaticSampler(0, SamplerLinearClampDesc);
    s_PresentRS.InitStaticSampler(1, SamplerPointClampDesc);
    s_PresentRS.Finalize(L"Present");

    // Initialize PSOs
    s_BlendUIPSO.SetRootSignature(s_PresentRS);
    s_BlendUIPSO.SetRasterizerState( RasterizerTwoSided );
    s_BlendUIPSO.SetBlendState( BlendPreMultiplied );
    s_BlendUIPSO.SetDepthStencilState( DepthStateDisabled );
    s_BlendUIPSO.SetSampleMask(0xFFFFFFFF);
    s_BlendUIPSO.SetInputLayout(0, nullptr);
    s_BlendUIPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    s_BlendUIPSO.SetVertexShader( g_pScreenQuadPresentVS, sizeof(g_pScreenQuadPresentVS) );
    s_BlendUIPSO.SetPixelShader( g_pBufferCopyPS, sizeof(g_pBufferCopyPS) );
    s_BlendUIPSO.SetRenderTargetFormat(SwapChainFormat, DXGI_FORMAT_UNKNOWN);
    s_BlendUIPSO.Finalize();

    s_BlendUIHDRPSO = s_BlendUIPSO;
    s_BlendUIHDRPSO.SetPixelShader(g_pBlendUIHDRPS, sizeof(g_pBlendUIHDRPS));
    s_BlendUIHDRPSO.Finalize();

#define CreatePSO( ObjName, ShaderByteCode ) \
    ObjName = s_BlendUIPSO; \
    ObjName.SetBlendState( BlendDisable ); \
    ObjName.SetPixelShader(ShaderByteCode, sizeof(ShaderByteCode) ); \
    ObjName.Finalize();

    CreatePSO(PresentSDRPS, g_pPresentSDRPS);
    CreatePSO(CompositeSDRPS, g_pCompositeSDRPS);
    CreatePSO(ScaleAndCompositeSDRPS, g_pScaleAndCompositeSDRPS);
    CreatePSO(CompositeHDRPS, g_pCompositeHDRPS);
    CreatePSO(ScaleAndCompositeHDRPS, g_pScaleAndCompositeHDRPS);
    CreatePSO(MagnifyPixelsPS, g_pMagnifyPixelsPS);

    PresentHDRPS = PresentSDRPS;
    PresentHDRPS.SetPixelShader(g_pPresentHDRPS, sizeof(g_pPresentHDRPS));
    DXGI_FORMAT SwapChainFormats[2] = { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM };
    PresentHDRPS.SetRenderTargetFormats(2, SwapChainFormats, DXGI_FORMAT_UNKNOWN );
    PresentHDRPS.Finalize();

#undef CreatePSO

    SetNativeResolution();

    g_PreDisplayBuffer.Create(L"PreDisplay Buffer", g_DisplayWidth, g_DisplayHeight, 1, SwapChainFormat);
    ImageScaling::Initialize(g_PreDisplayBuffer.GetFormat());
}

long Display::Recreate(const Display::RecreateRequest& request)
{
    if (s_SwapChain1 == nullptr)
        return E_UNEXPECTED;

    g_CommandManager.IdleGPU();

    ReleaseDisplayPlanes();

    if (request.keepOldChain != 0)
    {
        Display::ReleaseKeptSwapChain();
        s_KeptSwapChain = s_SwapChain1;
    }
    else
    {
        s_SwapChain1->Release();
    }
    s_SwapChain1 = nullptr;

    s_ChainOnPresentQueue = request.secondQueue != 0;

    ID3D12CommandQueue* presentQueue = g_CommandManager.GetCommandQueue();
    if (s_ChainOnPresentQueue)
    {
        if (s_PresentQueue == nullptr)
        {
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
            ASSERT_SUCCEEDED(g_Device->CreateCommandQueue(&queueDesc, MY_IID_PPV_ARGS(&s_PresentQueue)));
            ASSERT_SUCCEEDED(g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, MY_IID_PPV_ARGS(&s_PresentFence)));
            ASSERT_SUCCEEDED(g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, MY_IID_PPV_ARGS(&s_PresentCopyFence)));
            s_PresentCopyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            ASSERT(s_PresentCopyEvent != nullptr);
            ASSERT_SUCCEEDED(g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, MY_IID_PPV_ARGS(&s_PresentAllocator)));
            ASSERT_SUCCEEDED(g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_PresentAllocator, nullptr, MY_IID_PPV_ARGS(&s_PresentList)));
            s_PresentList->Close();
        }
        presentQueue = s_PresentQueue;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
    ASSERT_SUCCEEDED(CreateDXGIFactory2(0, MY_IID_PPV_ARGS(&dxgiFactory)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = VrtfSwapChainDesc();

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsSwapChainDesc = {};
    fsSwapChainDesc.Windowed = TRUE;

    const HRESULT created = dxgiFactory->CreateSwapChainForHwnd(
        presentQueue,
        GameCore::g_hWnd,
        &swapChainDesc,
        &fsSwapChainDesc,
        nullptr,
        &s_SwapChain1);

    if (FAILED(created))
    {
        s_SwapChain1 = nullptr;
        return created;
    }

    BindDisplayPlanes();

    g_CurrentBuffer = 0;

    g_CommandManager.IdleGPU();

    return S_OK;
}

bool Display::ReleaseKeptSwapChain(void)
{
    if (s_KeptSwapChain == nullptr)
        return false;

    s_KeptSwapChain->Release();
    s_KeptSwapChain = nullptr;
    return true;
}

void Display::Shutdown( void )
{
    Display::ReleaseKeptSwapChain();

    // VRTF: planes and real back buffers first -- the release idles the present queue, and the chain
    // cannot go while a copy into its buffers is still in flight.
    ReleaseDisplayPlanes();

    // VRTF: a refused Recreate leaves no chain, so teardown cannot assume one.
    if (s_SwapChain1 != nullptr)
    {
        s_SwapChain1->SetFullscreenState(FALSE, nullptr);
        s_SwapChain1->Release();
        s_SwapChain1 = nullptr;
    }

    if (s_PresentList != nullptr)
    {
        s_PresentList->Release();
        s_PresentList = nullptr;
    }
    if (s_PresentAllocator != nullptr)
    {
        s_PresentAllocator->Release();
        s_PresentAllocator = nullptr;
    }
    if (s_PresentCopyFence != nullptr)
    {
        s_PresentCopyFence->Release();
        s_PresentCopyFence = nullptr;
    }
    if (s_PresentFence != nullptr)
    {
        s_PresentFence->Release();
        s_PresentFence = nullptr;
    }
    if (s_PresentCopyEvent != nullptr)
    {
        CloseHandle(s_PresentCopyEvent);
        s_PresentCopyEvent = nullptr;
    }
    if (s_PresentQueue != nullptr)
    {
        s_PresentQueue->Release();
        s_PresentQueue = nullptr;
    }
    s_ChainOnPresentQueue = false;

    g_PreDisplayBuffer.Destroy();
}

void Graphics::PreparePresentHDR(void)
{
    GraphicsContext& Context = GraphicsContext::Begin(L"Present");

    const bool dlssActive = DLSS::Enable && DLSS::IsSupported();
    ColorBuffer& PresentSource = dlssActive ? g_DLSSOutputBuffer : g_SceneColorBuffer;
    bool NeedsScaling = dlssActive ? false : (g_NativeWidth != g_DisplayWidth || g_NativeHeight != g_DisplayHeight);

    Context.SetRootSignature(s_PresentRS);
    Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Context.TransitionResource(PresentSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Context.SetDynamicDescriptor(0, 0, PresentSource.GetSRV());

    ColorBuffer& Dest = DebugZoom == kDebugZoomOff ? g_DisplayPlane[g_CurrentBuffer] : g_PreDisplayBuffer;

    // On Windows, prefer scaling and compositing in one step via pixel shader
    Context.SetRootSignature(s_PresentRS);
    Context.TransitionResource(g_OverlayBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (DebugZoom == kDebugZoomOff)
    {
        Context.SetDynamicDescriptor(0, 1, g_OverlayBuffer.GetSRV());
        Context.SetPipelineState(NeedsScaling ? ScaleAndCompositeHDRPS : CompositeHDRPS);
    }
    else
    {
        Context.SetDynamicDescriptor(0, 1, GetDefaultTexture(kBlackTransparent2D));
        Context.SetPipelineState(NeedsScaling ? ScaleAndCompositeHDRPS : PresentHDRPS);
    }
    Context.SetConstants(1, (float)g_HDRPaperWhite / 10000.0f, (float)g_MaxDisplayLuminance,
        0.7071f / g_NativeWidth, 0.7071f / g_NativeHeight);
    Context.TransitionResource(Dest, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Context.SetRenderTarget(Dest.GetRTV());
    Context.SetViewportAndScissor(0, 0, g_DisplayWidth, g_DisplayHeight);
    Context.Draw(3);

    // Magnify without stretching
    if (DebugZoom != kDebugZoomOff)
    {
        Context.SetPipelineState(MagnifyPixelsPS);
        Context.TransitionResource(g_PreDisplayBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_RENDER_TARGET);
        Context.SetRenderTarget(g_DisplayPlane[g_CurrentBuffer].GetRTV());
        Context.SetDynamicDescriptor(0, 0, g_PreDisplayBuffer.GetSRV());
        Context.SetViewportAndScissor(0, 0, g_DisplayWidth, g_DisplayHeight);
        Context.SetConstants(1, 1.0f / ((int)DebugZoom + 1.0f));
        Context.Draw(3);

        CompositeOverlays(Context);
    }

    Context.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_PRESENT);

    // Close the final context to be executed before frame present.
    Context.Finish();
}

void Graphics::CompositeOverlays( GraphicsContext& Context )
{
    // Now blend (or write) the UI overlay
    Context.SetRootSignature(s_PresentRS);
    Context.TransitionResource(g_OverlayBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Context.SetDynamicDescriptor(0, 0, g_OverlayBuffer.GetSRV());
    Context.SetPipelineState(g_bEnableHDROutput ? s_BlendUIHDRPSO : s_BlendUIPSO);
    Context.SetConstants(1, (float)g_HDRPaperWhite / 10000.0f, (float)g_MaxDisplayLuminance);
    Context.Draw(3);
}

void Graphics::PreparePresentSDR(void)
{
    GraphicsContext& Context = GraphicsContext::Begin(L"Present");

    Context.SetRootSignature(s_PresentRS);
    Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const bool dlssActive = DLSS::Enable && DLSS::IsSupported();
    ColorBuffer& PresentSource = dlssActive ? g_DLSSOutputBuffer : g_SceneColorBuffer;
    bool NeedsScaling = dlssActive ? false : (g_NativeWidth != g_DisplayWidth || g_NativeHeight != g_DisplayHeight);

    // We're going to be reading these buffers to write to the swap chain buffer(s)
    Context.TransitionResource(PresentSource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Context.SetDynamicDescriptor(0, 0, PresentSource.GetSRV());

    // VRTF: direct-topology tail. Compose PresentSource into g_PreDisplayBuffer (the ONE
    // display-profile encode), let the hook draw the HUD onto it, then a profile-neutral point
    // copy to the display plane. No overlay reads on this path. DebugZoom falls through to the
    // stock path (the fixture never sets it; correctness over realism there).
    if (g_vrtfDirectTailHook != nullptr && DebugZoom == kDebugZoomOff)
    {
        if (NeedsScaling)
        {
            ImageScaling::Upscale(Context, g_PreDisplayBuffer, g_SceneColorBuffer, eScalingFilter((int)UpsampleFilter));
        }
        else
        {
            Context.SetPipelineState(PresentSDRPS);
            Context.TransitionResource(g_PreDisplayBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Context.SetRenderTarget(g_PreDisplayBuffer.GetRTV());
            Context.SetViewportAndScissor(0, 0, g_DisplayWidth, g_DisplayHeight);
            Context.Draw(3);
        }

        g_vrtfDirectTailHook(Context, g_PreDisplayBuffer);

        Context.SetRootSignature(s_PresentRS);
        Context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Context.SetPipelineState(MagnifyPixelsPS);
        Context.TransitionResource(g_PreDisplayBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Context.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_RENDER_TARGET);
        Context.SetRenderTarget(g_DisplayPlane[g_CurrentBuffer].GetRTV());
        Context.SetDynamicDescriptor(0, 0, g_PreDisplayBuffer.GetSRV());
        Context.SetViewportAndScissor(0, 0, g_DisplayWidth, g_DisplayHeight);
        Context.SetConstants(1, 1.0f);
        Context.Draw(3);
    }
    // On Windows, prefer scaling and compositing in one step via pixel shader
    else if (DebugZoom == kDebugZoomOff && (UpsampleFilter == kSharpening || !NeedsScaling))
    {
        Context.TransitionResource(g_OverlayBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Context.SetDynamicDescriptor(0, 1, g_OverlayBuffer.GetSRV());
        Context.SetPipelineState(NeedsScaling ? ScaleAndCompositeSDRPS : CompositeSDRPS);
        Context.SetConstants(1, 0.7071f / g_NativeWidth, 0.7071f / g_NativeHeight);
        Context.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_RENDER_TARGET);
        Context.SetRenderTarget(g_DisplayPlane[g_CurrentBuffer].GetRTV());
        Context.SetViewportAndScissor(0, 0, g_DisplayWidth, g_DisplayHeight);
        Context.Draw(3);
    }
    else
    {
        ColorBuffer& Dest = DebugZoom == kDebugZoomOff ? g_DisplayPlane[g_CurrentBuffer] : g_PreDisplayBuffer;

        // Scale or Copy
        if (NeedsScaling)
        {
            ImageScaling::Upscale(Context, Dest, g_SceneColorBuffer, eScalingFilter((int)UpsampleFilter));
        }
        else
        {
            Context.SetPipelineState(PresentSDRPS);
            Context.TransitionResource(Dest, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Context.SetRenderTarget(Dest.GetRTV());
            Context.SetViewportAndScissor(0, 0, g_NativeWidth, g_NativeHeight);
            Context.Draw(3);
        }

        // Magnify without stretching
        if (DebugZoom != kDebugZoomOff)
        {
            Context.SetPipelineState(MagnifyPixelsPS);
            Context.TransitionResource(g_PreDisplayBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Context.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_RENDER_TARGET);
            Context.SetRenderTarget(g_DisplayPlane[g_CurrentBuffer].GetRTV());
            Context.SetDynamicDescriptor(0, 0, g_PreDisplayBuffer.GetSRV());
            Context.SetViewportAndScissor(0, 0, g_DisplayWidth, g_DisplayHeight);
            Context.SetConstants(1, 1.0f / ((int)DebugZoom + 1.0f));
            Context.Draw(3);
        }

        CompositeOverlays(Context);
    }


    Context.TransitionResource(g_DisplayPlane[g_CurrentBuffer], D3D12_RESOURCE_STATE_PRESENT);

    // Close the final context to be executed before frame present.
    Context.Finish();
}

void Display::Present(void)
{
    // VRTF: a refused Recreate leaves no chain AND no display planes, so the whole frame is skipped
    // -- composing into a destroyed plane's RTV would remove the device instead of reporting the
    // refusal the test is measuring.
    if (s_SwapChain1 == nullptr)
        return;

    if (g_bEnableHDROutput)
        PreparePresentHDR();
    else
        PreparePresentSDR();

    UINT PresentInterval = s_EnableVSync ? std::min(4, (int)Round(s_FrameTime * 60.0f)) : 0;

    // VRTF: the interpolation wrapper's shape (presentPassthrough). The chain presents on a queue that
    // never saw this frame's render work: the graphics queue signals and the present queue waits, then
    // the replacement plane is copied into the real back buffer on the chain's own queue -- the only
    // queue allowed to write it. The replacement is read out of COMMON by implicit promotion and decays
    // back to it, so MiniEngine's tracked state is untouched.
    if (s_ChainOnPresentQueue)
    {
        ++s_PresentFenceValue;
        g_CommandManager.GetCommandQueue()->Signal(s_PresentFence, s_PresentFenceValue);
        s_PresentQueue->Wait(s_PresentFence, s_PresentFenceValue);

        if (s_PresentCopyFence->GetCompletedValue() < s_PresentCopyFenceValue)
        {
            s_PresentCopyFence->SetEventOnCompletion(s_PresentCopyFenceValue, s_PresentCopyEvent);
            WaitForSingleObject(s_PresentCopyEvent, INFINITE);
        }

        ASSERT_SUCCEEDED(s_PresentAllocator->Reset());
        ASSERT_SUCCEEDED(s_PresentList->Reset(s_PresentAllocator, nullptr));

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = s_RealBackBuffer[g_CurrentBuffer];
        barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        s_PresentList->ResourceBarrier(1, &barrier);

        s_PresentList->CopyResource(s_RealBackBuffer[g_CurrentBuffer], g_DisplayPlane[g_CurrentBuffer].GetResource());

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        s_PresentList->ResourceBarrier(1, &barrier);

        ASSERT_SUCCEEDED(s_PresentList->Close());

        ID3D12CommandList* copyList[] = { s_PresentList };
        s_PresentQueue->ExecuteCommandLists(1, copyList);
        s_PresentQueue->Signal(s_PresentCopyFence, ++s_PresentCopyFenceValue);
    }

    s_SwapChain1->Present(PresentInterval, 0);

    g_CurrentBuffer = (g_CurrentBuffer + 1) % SWAP_CHAIN_BUFFER_COUNT;

    // Test robustness to handle spikes in CPU time
    //if (s_DropRandomFrames)
    //{
    //	if (std::rand() % 25 == 0)
    //		BusyLoopSleep(0.010);
    //}

    int64_t CurrentTick = SystemTime::GetCurrentTick();

    if (s_EnableVSync)
    {
        // With VSync enabled, the time step between frames becomes a multiple of 16.666 ms.  We need
        // to add logic to vary between 1 and 2 (or 3 fields).  This delta time also determines how
        // long the previous frame should be displayed (i.e. the present interval.)
        s_FrameTime = (s_LimitTo30Hz ? 2.0f : 1.0f) / 60.0f;
        if (s_DropRandomFrames)
        {
            if (std::rand() % 50 == 0)
                s_FrameTime += (1.0f / 60.0f);
        }
    }
    else
    {
        // When running free, keep the most recent total frame time as the time step for
        // the next frame simulation.  This is not super-accurate, but assuming a frame
        // time varies smoothly, it should be close enough.
        s_FrameTime = (float)SystemTime::TimeBetweenTicks(s_FrameStartTick, CurrentTick);
    }

    s_FrameStartTick = CurrentTick;

    ++s_FrameIndex;

    TemporalEffects::Update((uint32_t)s_FrameIndex);

    SetNativeResolution();
    SetDisplayResolution();
}

uint64_t Graphics::GetFrameCount(void)
{
    return s_FrameIndex;
}

float Graphics::GetFrameTime(void)
{
    return s_FrameTime;
}

float Graphics::GetFrameRate(void)
{
    return s_FrameTime == 0.0f ? 0.0f : 1.0f / s_FrameTime;
}

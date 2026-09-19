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
#include "GameCore.h"
#include "GameInput.h"

#ifdef _GAMING_DESKTOP

// I can't find the GameInput.h header in the GDK for Desktop yet
#include <Xinput.h>
// VRTF: xinput1_4, the DLL current games import (xinput9_1_0 is the legacy subset in its own DLL).
#pragma comment(lib, "xinput.lib")

#define USE_KEYBOARD_MOUSE
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

// VRTF: the desktop Windows.Gaming.Input path (VRTF_GAMEPAD_API=winrt), through the ABI headers WRL
// wraps (this engine builds with /permissive). The engine's global Color class would capture the
// SDK's `typedef struct Color Color;` inside ABI::Windows::UI (elaborated-type lookup reaches the
// enclosing scope), so that struct is declared there first.
namespace ABI { namespace Windows { namespace UI { struct Color; } } }
#include <windows.gaming.input.h>
#include <wrl/wrappers/corewrappers.h>
#include <roapi.h>
#include <vector>
#pragma comment(lib, "runtimeobject.lib")

#else

// This is what we should use on *all* platforms, but see previous comment
#include <GameInput.h>

// This should be handled by GameInput.h, but we'll borrow values from XINPUT.
#define XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE  (7849.0f / 32768.0f)
#define XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE (8689.0f / 32768.0f)

#endif

namespace GameCore
{
    extern HWND g_hWnd;
}

namespace
{
    bool s_Buttons[2][GameInput::kNumDigitalInputs];
    float s_HoldDuration[GameInput::kNumDigitalInputs] = { 0.0f };
    float s_Analogs[GameInput::kNumAnalogInputs];
    float s_AnalogsTC[GameInput::kNumAnalogInputs];

#ifdef USE_KEYBOARD_MOUSE

    IDirectInput8A* s_DI;
    IDirectInputDevice8A* s_Keyboard;
    IDirectInputDevice8A* s_Mouse;

    DIMOUSESTATE2 s_MouseState;
    unsigned char s_Keybuffer[256];
    unsigned char s_DXKeyMapping[GameInput::kNumKeys]; // map DigitalInput enum to DX key codes 

#endif

    inline float FilterAnalogInput( int val, int deadZone )
    {
        if (val < 0)
        {
            if (val > -deadZone)
                return 0.0f;
            else
                return (val + deadZone) / (32768.0f - deadZone);
        }
        else
        {
            if (val < deadZone)
                return 0.0f;
            else
                return (val - deadZone) / (32767.0f - deadZone);
        }
    }

#ifdef _GAMING_DESKTOP
    // VRTF_GAMEPAD_API=winrt, decided once in Initialize like the other fixture latches. Unset keeps
    // the XInput slot-0 read; winrt binds one of the pads Windows.Gaming.Input lists the way a
    // shipped game binds one: the slot offered on two consecutive passes over the list, tracked
    // through one pending slot, every slot visited per pass. Only the bound pad is read, so a list
    // whose shape changes between passes binds nothing.
    bool s_UseWinRt = false;
    Microsoft::WRL::ComPtr<ABI::Windows::Gaming::Input::IGamepadStatics> s_GamepadStatics;
    std::vector<Microsoft::WRL::ComPtr<ABI::Windows::Gaming::Input::IGamepad>> s_Gamepads;
    int32_t s_PendingSlot = -1;
    int32_t s_BoundSlot = -1;
    GameInput::GamepadState s_BoundState = {};
    EventRegistrationToken s_GamepadAddedToken = {};
    EventRegistrationToken s_GamepadRemovedToken = {};
    volatile LONG s_GamepadListDirty = 0;

    inline float FilterAnalogInput( float val, float deadZone )
    {
        if (val < -deadZone)
            return (val + deadZone) / (1.0f - deadZone);
        else if (val > deadZone)
            return (val - deadZone) / (1.0f - deadZone);
        else
            return 0.0f;
    }

    void WgiInitialize()
    {
        using namespace ABI::Windows::Foundation;
        using namespace ABI::Windows::Gaming::Input;
        using namespace Microsoft::WRL;
        using namespace Microsoft::WRL::Wrappers;

        const HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Gaming_Input_Gamepad).Get(), IID_PPV_ARGS(&s_GamepadStatics));
        ASSERT(SUCCEEDED(hr), "Windows.Gaming.Input.Gamepad activation failed.");
        if (FAILED(hr))
            return;

        // Windows raises these off-thread; the list is re-read on the next Update, as a game does.
        auto markDirty = Callback<IEventHandler<Gamepad*>>([](IInspectable*, IGamepad*) -> HRESULT
        {
            InterlockedExchange(&s_GamepadListDirty, 1);
            return S_OK;
        });
        s_GamepadStatics->add_GamepadAdded(markDirty.Get(), &s_GamepadAddedToken);
        s_GamepadStatics->add_GamepadRemoved(markDirty.Get(), &s_GamepadRemovedToken);
        InterlockedExchange(&s_GamepadListDirty, 1);
    }

    void WgiShutdown()
    {
        if (s_GamepadStatics)
        {
            s_GamepadStatics->remove_GamepadAdded(s_GamepadAddedToken);
            s_GamepadStatics->remove_GamepadRemoved(s_GamepadRemovedToken);
        }
        s_Gamepads.clear();
        s_PendingSlot = -1;
        s_BoundSlot = -1;
        s_BoundState = {};
        s_GamepadStatics.Reset();
    }

    void WgiEnumerate()
    {
        using namespace ABI::Windows::Gaming::Input;

        s_Gamepads.clear();
        s_PendingSlot = -1;
        s_BoundSlot = -1;
        s_BoundState = {};
        Microsoft::WRL::ComPtr<__FIVectorView_1_Windows__CGaming__CInput__CGamepad> view;
        if (FAILED(s_GamepadStatics->get_Gamepads(&view)) || !view)
            return;
        unsigned int size = 0;
        view->get_Size(&size);
        for (unsigned int i = 0; i < size; ++i)
        {
            Microsoft::WRL::ComPtr<IGamepad> pad;
            if (SUCCEEDED(view->GetAt(i, &pad)) && pad)
                s_Gamepads.push_back(pad);
        }
    }

    // One pass per Update until a slot is bound: the slot the previous pass left pending binds, any
    // other slot visited becomes the pending one. The bound pad's reading feeds the engine's own
    // digital/analog state exactly as the XInput slot-0 read in Update does.
    void WgiUpdate()
    {
        using namespace ABI::Windows::Gaming::Input;

        if (InterlockedExchange(&s_GamepadListDirty, 0) != 0)
            WgiEnumerate();

        if (s_BoundSlot < 0)
        {
            for (size_t i = 0; i < s_Gamepads.size() && s_BoundSlot < 0; ++i)
            {
                if (s_PendingSlot == (int32_t)i)
                    s_BoundSlot = (int32_t)i;
                else
                    s_PendingSlot = (int32_t)i;
            }
            if (s_BoundSlot < 0)
                return;
        }

        GamepadReading reading = {};
        s_BoundState = {};
        s_BoundState.Result = s_Gamepads[s_BoundSlot]->GetCurrentReading(&reading);
        if (FAILED(s_BoundState.Result))
            return;
        s_BoundState.Timestamp = reading.Timestamp;
        s_BoundState.Buttons = (uint32_t)reading.Buttons;
        s_BoundState.LeftTrigger = reading.LeftTrigger;
        s_BoundState.RightTrigger = reading.RightTrigger;
        s_BoundState.LeftThumbstickX = reading.LeftThumbstickX;
        s_BoundState.LeftThumbstickY = reading.LeftThumbstickY;
        s_BoundState.RightThumbstickX = reading.RightThumbstickX;
        s_BoundState.RightThumbstickY = reading.RightThumbstickY;

        const GameInput::GamepadState& bound = s_BoundState;
        const uint32_t Buttons = bound.Buttons;
        if (Buttons & GamepadButtons_DPadUp) s_Buttons[0][GameInput::kDPadUp] = true;
        if (Buttons & GamepadButtons_DPadDown) s_Buttons[0][GameInput::kDPadDown] = true;
        if (Buttons & GamepadButtons_DPadLeft) s_Buttons[0][GameInput::kDPadLeft] = true;
        if (Buttons & GamepadButtons_DPadRight) s_Buttons[0][GameInput::kDPadRight] = true;
        if (Buttons & GamepadButtons_Menu) s_Buttons[0][GameInput::kStartButton] = true;
        if (Buttons & GamepadButtons_View) s_Buttons[0][GameInput::kBackButton] = true;
        if (Buttons & GamepadButtons_LeftThumbstick) s_Buttons[0][GameInput::kLThumbClick] = true;
        if (Buttons & GamepadButtons_RightThumbstick) s_Buttons[0][GameInput::kRThumbClick] = true;
        if (Buttons & GamepadButtons_LeftShoulder) s_Buttons[0][GameInput::kLShoulder] = true;
        if (Buttons & GamepadButtons_RightShoulder) s_Buttons[0][GameInput::kRShoulder] = true;
        if (Buttons & GamepadButtons_A) s_Buttons[0][GameInput::kAButton] = true;
        if (Buttons & GamepadButtons_B) s_Buttons[0][GameInput::kBButton] = true;
        if (Buttons & GamepadButtons_X) s_Buttons[0][GameInput::kXButton] = true;
        if (Buttons & GamepadButtons_Y) s_Buttons[0][GameInput::kYButton] = true;

        static const float kAnalogStickDeadZone = 0.18f;

        s_Analogs[GameInput::kAnalogLeftTrigger]  = (float)bound.LeftTrigger;
        s_Analogs[GameInput::kAnalogRightTrigger] = (float)bound.RightTrigger;
        s_Analogs[GameInput::kAnalogLeftStickX]   = FilterAnalogInput((float)bound.LeftThumbstickX, kAnalogStickDeadZone);
        s_Analogs[GameInput::kAnalogLeftStickY]   = FilterAnalogInput((float)bound.LeftThumbstickY, kAnalogStickDeadZone);
        s_Analogs[GameInput::kAnalogRightStickX]  = FilterAnalogInput((float)bound.RightThumbstickX, kAnalogStickDeadZone);
        s_Analogs[GameInput::kAnalogRightStickY]  = FilterAnalogInput((float)bound.RightThumbstickY, kAnalogStickDeadZone);
    }
#endif

#ifdef USE_KEYBOARD_MOUSE
    void KbmBuildKeyMapping()
    {
        s_DXKeyMapping[GameInput::kKey_escape] = 1;
        s_DXKeyMapping[GameInput::kKey_1] = 2;
        s_DXKeyMapping[GameInput::kKey_2] = 3;
        s_DXKeyMapping[GameInput::kKey_3] = 4;
        s_DXKeyMapping[GameInput::kKey_4] = 5;
        s_DXKeyMapping[GameInput::kKey_5] = 6;
        s_DXKeyMapping[GameInput::kKey_6] = 7;
        s_DXKeyMapping[GameInput::kKey_7] = 8;
        s_DXKeyMapping[GameInput::kKey_8] = 9;
        s_DXKeyMapping[GameInput::kKey_9] = 10;
        s_DXKeyMapping[GameInput::kKey_0] = 11;
        s_DXKeyMapping[GameInput::kKey_minus] = 12;
        s_DXKeyMapping[GameInput::kKey_equals] = 13;
        s_DXKeyMapping[GameInput::kKey_back] = 14;
        s_DXKeyMapping[GameInput::kKey_tab] = 15;
        s_DXKeyMapping[GameInput::kKey_q] = 16;
        s_DXKeyMapping[GameInput::kKey_w] = 17;
        s_DXKeyMapping[GameInput::kKey_e] = 18;
        s_DXKeyMapping[GameInput::kKey_r] = 19;
        s_DXKeyMapping[GameInput::kKey_t] = 20;
        s_DXKeyMapping[GameInput::kKey_y] = 21;
        s_DXKeyMapping[GameInput::kKey_u] = 22;
        s_DXKeyMapping[GameInput::kKey_i] = 23;
        s_DXKeyMapping[GameInput::kKey_o] = 24;
        s_DXKeyMapping[GameInput::kKey_p] = 25;
        s_DXKeyMapping[GameInput::kKey_lbracket] = 26;
        s_DXKeyMapping[GameInput::kKey_rbracket] = 27;
        s_DXKeyMapping[GameInput::kKey_return] = 28;
        s_DXKeyMapping[GameInput::kKey_lcontrol] = 29;
        s_DXKeyMapping[GameInput::kKey_a] = 30;
        s_DXKeyMapping[GameInput::kKey_s] = 31;
        s_DXKeyMapping[GameInput::kKey_d] = 32;
        s_DXKeyMapping[GameInput::kKey_f] = 33;
        s_DXKeyMapping[GameInput::kKey_g] = 34;
        s_DXKeyMapping[GameInput::kKey_h] = 35;
        s_DXKeyMapping[GameInput::kKey_j] = 36;
        s_DXKeyMapping[GameInput::kKey_k] = 37;
        s_DXKeyMapping[GameInput::kKey_l] = 38;
        s_DXKeyMapping[GameInput::kKey_semicolon] = 39;
        s_DXKeyMapping[GameInput::kKey_apostrophe] = 40;
        s_DXKeyMapping[GameInput::kKey_grave] = 41;
        s_DXKeyMapping[GameInput::kKey_lshift] = 42;
        s_DXKeyMapping[GameInput::kKey_backslash] = 43;
        s_DXKeyMapping[GameInput::kKey_z] = 44;
        s_DXKeyMapping[GameInput::kKey_x] = 45;
        s_DXKeyMapping[GameInput::kKey_c] = 46;
        s_DXKeyMapping[GameInput::kKey_v] = 47;
        s_DXKeyMapping[GameInput::kKey_b] = 48;
        s_DXKeyMapping[GameInput::kKey_n] = 49;
        s_DXKeyMapping[GameInput::kKey_m] = 50;
        s_DXKeyMapping[GameInput::kKey_comma] = 51;
        s_DXKeyMapping[GameInput::kKey_period] = 52;
        s_DXKeyMapping[GameInput::kKey_slash] = 53;
        s_DXKeyMapping[GameInput::kKey_rshift] = 54;
        s_DXKeyMapping[GameInput::kKey_multiply] = 55;
        s_DXKeyMapping[GameInput::kKey_lalt] = 56;
        s_DXKeyMapping[GameInput::kKey_space] = 57;
        s_DXKeyMapping[GameInput::kKey_capital] = 58;
        s_DXKeyMapping[GameInput::kKey_f1] = 59;
        s_DXKeyMapping[GameInput::kKey_f2] = 60;
        s_DXKeyMapping[GameInput::kKey_f3] = 61;
        s_DXKeyMapping[GameInput::kKey_f4] = 62;
        s_DXKeyMapping[GameInput::kKey_f5] = 63;
        s_DXKeyMapping[GameInput::kKey_f6] = 64;
        s_DXKeyMapping[GameInput::kKey_f7] = 65;
        s_DXKeyMapping[GameInput::kKey_f8] = 66;
        s_DXKeyMapping[GameInput::kKey_f9] = 67;
        s_DXKeyMapping[GameInput::kKey_f10] = 68;
        s_DXKeyMapping[GameInput::kKey_numlock] = 69;
        s_DXKeyMapping[GameInput::kKey_scroll] = 70;
        s_DXKeyMapping[GameInput::kKey_numpad7] = 71;
        s_DXKeyMapping[GameInput::kKey_numpad8] = 72;
        s_DXKeyMapping[GameInput::kKey_numpad9] = 73;
        s_DXKeyMapping[GameInput::kKey_subtract] = 74;
        s_DXKeyMapping[GameInput::kKey_numpad4] = 75;
        s_DXKeyMapping[GameInput::kKey_numpad5] = 76;
        s_DXKeyMapping[GameInput::kKey_numpad6] = 77;
        s_DXKeyMapping[GameInput::kKey_add] = 78;
        s_DXKeyMapping[GameInput::kKey_numpad1] = 79;
        s_DXKeyMapping[GameInput::kKey_numpad2] = 80;
        s_DXKeyMapping[GameInput::kKey_numpad3] = 81;
        s_DXKeyMapping[GameInput::kKey_numpad0] = 82;
        s_DXKeyMapping[GameInput::kKey_decimal] = 83;
        s_DXKeyMapping[GameInput::kKey_f11] = 87;
        s_DXKeyMapping[GameInput::kKey_f12] = 88;
        s_DXKeyMapping[GameInput::kKey_numpadenter] = 156;
        s_DXKeyMapping[GameInput::kKey_rcontrol] = 157;
        s_DXKeyMapping[GameInput::kKey_divide] = 181;
        s_DXKeyMapping[GameInput::kKey_sysrq] = 183;
        s_DXKeyMapping[GameInput::kKey_ralt] = 184;
        s_DXKeyMapping[GameInput::kKey_pause] = 197;
        s_DXKeyMapping[GameInput::kKey_home] = 199;
        s_DXKeyMapping[GameInput::kKey_up] = 200;
        s_DXKeyMapping[GameInput::kKey_pgup] = 201;
        s_DXKeyMapping[GameInput::kKey_left] = 203;
        s_DXKeyMapping[GameInput::kKey_right] = 205;
        s_DXKeyMapping[GameInput::kKey_end] = 207;
        s_DXKeyMapping[GameInput::kKey_down] = 208;
        s_DXKeyMapping[GameInput::kKey_pgdn] = 209;
        s_DXKeyMapping[GameInput::kKey_insert] = 210;
        s_DXKeyMapping[GameInput::kKey_delete] = 211;
        s_DXKeyMapping[GameInput::kKey_lwin] = 219;
        s_DXKeyMapping[GameInput::kKey_rwin] = 220;
        s_DXKeyMapping[GameInput::kKey_apps] = 221;
    }

    void KbmZeroInputs()
    {
        memset(&s_MouseState, 0, sizeof(DIMOUSESTATE2));
        memset(s_Keybuffer, 0, sizeof(s_Keybuffer));
    }

    void KbmInitialize()
    {
        KbmBuildKeyMapping();

        if (FAILED(DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&s_DI, nullptr)))
            ASSERT(false, "DirectInput8 initialization failed.");

        if (FAILED(s_DI->CreateDevice(GUID_SysKeyboard, &s_Keyboard, nullptr)))
            ASSERT(false, "Keyboard CreateDevice failed.");
        if (FAILED(s_Keyboard->SetDataFormat(&c_dfDIKeyboard)))
            ASSERT(false, "Keyboard SetDataFormat failed.");
        if (FAILED(s_Keyboard->SetCooperativeLevel(GameCore::g_hWnd, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE)))
            ASSERT(false, "Keyboard SetCooperativeLevel failed.");

        DIPROPDWORD dipdw;
        dipdw.diph.dwSize = sizeof(DIPROPDWORD);
        dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        dipdw.diph.dwObj = 0;
        dipdw.diph.dwHow = DIPH_DEVICE;
        dipdw.dwData = 10;
        if (FAILED(s_Keyboard->SetProperty(DIPROP_BUFFERSIZE, &dipdw.diph)))
            ASSERT(false, "Keyboard set buffer size failed.");

        if (FAILED(s_DI->CreateDevice(GUID_SysMouse, &s_Mouse, nullptr)))
            ASSERT(false, "Mouse CreateDevice failed.");
        if (FAILED(s_Mouse->SetDataFormat(&c_dfDIMouse2)))
            ASSERT(false, "Mouse SetDataFormat failed.");
        if (FAILED(s_Mouse->SetCooperativeLevel(GameCore::g_hWnd, DISCL_FOREGROUND | DISCL_EXCLUSIVE)))
            ASSERT(false, "Mouse SetCooperativeLevel failed.");

        KbmZeroInputs();
    }

    void KbmShutdown()
    {
        if (s_Keyboard)
        {
            s_Keyboard->Unacquire();
            s_Keyboard->Release();
            s_Keyboard = nullptr;
        }
        if (s_Mouse)
        {
            s_Mouse->Unacquire();
            s_Mouse->Release();
            s_Mouse = nullptr;
        }
        if (s_DI)
        {
            s_DI->Release();
            s_DI = nullptr;
        }
    }

    void KbmUpdate()
    {
        HWND foreground = GetForegroundWindow();
        bool visible = IsWindowVisible(foreground) != 0;

        if (foreground != GameCore::g_hWnd // wouldn't be able to acquire
            || !visible)
        {
            KbmZeroInputs();
        }
        else
        {
            s_Mouse->Acquire();
            s_Mouse->GetDeviceState(sizeof(DIMOUSESTATE2), &s_MouseState);
            s_Keyboard->Acquire();
            s_Keyboard->GetDeviceState(sizeof(s_Keybuffer), s_Keybuffer);
        }
    }

#endif

}

void GameInput::Initialize()
{
    ZeroMemory(s_Buttons, sizeof(s_Buttons) );
    ZeroMemory(s_Analogs, sizeof(s_Analogs) );

#ifdef _GAMING_DESKTOP
    char api[16] = {};
    s_UseWinRt = GetEnvironmentVariableA("VRTF_GAMEPAD_API", api, sizeof(api)) > 0 && strcmp(api, "winrt") == 0;
    if (s_UseWinRt)
        WgiInitialize();
#endif

#ifdef USE_KEYBOARD_MOUSE
    KbmInitialize();
#endif
}

void GameInput::Shutdown()
{
#ifdef _GAMING_DESKTOP
    WgiShutdown();
#endif

#ifdef USE_KEYBOARD_MOUSE
    KbmShutdown();
#endif
}

void GameInput::Update( float frameDelta )
{
    memcpy(s_Buttons[1], s_Buttons[0], sizeof(s_Buttons[0]));
    memset(s_Buttons[0], 0, sizeof(s_Buttons[0]));
    memset(s_Analogs, 0, sizeof(s_Analogs));

#ifdef _GAMING_DESKTOP

#define SET_BUTTON_VALUE(InputEnum, GameInputMask) \
        s_Buttons[0][InputEnum] = !!(newInputState.Gamepad.wButtons & GameInputMask);

    XINPUT_STATE newInputState;
    if (s_UseWinRt)
        WgiUpdate();
    else if (ERROR_SUCCESS == XInputGetState(0, &newInputState))
    {
        SET_BUTTON_VALUE(kDPadUp, XINPUT_GAMEPAD_DPAD_UP);
        SET_BUTTON_VALUE(kDPadDown, XINPUT_GAMEPAD_DPAD_DOWN);
        SET_BUTTON_VALUE(kDPadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
        SET_BUTTON_VALUE(kDPadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
        SET_BUTTON_VALUE(kStartButton, XINPUT_GAMEPAD_START);
        SET_BUTTON_VALUE(kBackButton, XINPUT_GAMEPAD_BACK);
        SET_BUTTON_VALUE(kLThumbClick, XINPUT_GAMEPAD_LEFT_THUMB);
        SET_BUTTON_VALUE(kRThumbClick, XINPUT_GAMEPAD_RIGHT_THUMB);
        SET_BUTTON_VALUE(kLShoulder, XINPUT_GAMEPAD_LEFT_SHOULDER);
        SET_BUTTON_VALUE(kRShoulder, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        SET_BUTTON_VALUE(kAButton, XINPUT_GAMEPAD_A);
        SET_BUTTON_VALUE(kBButton, XINPUT_GAMEPAD_B);
        SET_BUTTON_VALUE(kXButton, XINPUT_GAMEPAD_X);
        SET_BUTTON_VALUE(kYButton, XINPUT_GAMEPAD_Y);

        s_Analogs[kAnalogLeftTrigger]   = newInputState.Gamepad.bLeftTrigger / 255.0f;
        s_Analogs[kAnalogRightTrigger]  = newInputState.Gamepad.bRightTrigger / 255.0f;
        s_Analogs[kAnalogLeftStickX]    = FilterAnalogInput(newInputState.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE );
        s_Analogs[kAnalogLeftStickY]    = FilterAnalogInput(newInputState.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE );
        s_Analogs[kAnalogRightStickX]   = FilterAnalogInput(newInputState.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE );
        s_Analogs[kAnalogRightStickY]   = FilterAnalogInput(newInputState.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE );
    }
#else
    IGameInputReading* pGIR = nullptr;
    if (s_pGameInput != nullptr)
        s_pGameInput->GetCurrentReading(GameInputKindGamepad, nullptr, &pGIR);
    bool IsGamepadPresent = (pGIR != nullptr);

    if (IsGamepadPresent)
    {
        GameInputGamepadState newInputState;
        pGIR->GetGamepadState(&newInputState);
        pGIR->Release();

#define SET_BUTTON_VALUE(InputEnum, GameInputMask) \
        s_Buttons[0][InputEnum] = !!(newInputState.buttons & GameInputMask);

        SET_BUTTON_VALUE(kDPadUp, GameInputGamepadDPadUp);
        SET_BUTTON_VALUE(kDPadDown, GameInputGamepadDPadDown);
        SET_BUTTON_VALUE(kDPadLeft, GameInputGamepadDPadLeft);
        SET_BUTTON_VALUE(kDPadRight, GameInputGamepadDPadRight);
        SET_BUTTON_VALUE(kStartButton, GameInputGamepadMenu);
        SET_BUTTON_VALUE(kBackButton, GameInputGamepadView);
        SET_BUTTON_VALUE(kLThumbClick, GameInputGamepadLeftThumbstick);
        SET_BUTTON_VALUE(kRThumbClick, GameInputGamepadRightThumbstick);
        SET_BUTTON_VALUE(kLShoulder, GameInputGamepadLeftShoulder);
        SET_BUTTON_VALUE(kRShoulder, GameInputGamepadRightShoulder);
        SET_BUTTON_VALUE(kAButton, GameInputGamepadA);
        SET_BUTTON_VALUE(kBButton, GameInputGamepadB);
        SET_BUTTON_VALUE(kXButton, GameInputGamepadX);
        SET_BUTTON_VALUE(kYButton, GameInputGamepadY);

        s_Analogs[kAnalogLeftTrigger]   = newInputState.leftTrigger;
        s_Analogs[kAnalogRightTrigger]  = newInputState.rightTrigger;
        s_Analogs[kAnalogLeftStickX]    = FilterAnalogInput(newInputState.leftThumbstickX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        s_Analogs[kAnalogLeftStickY]    = FilterAnalogInput(newInputState.leftThumbstickY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        s_Analogs[kAnalogRightStickX]   = FilterAnalogInput(newInputState.rightThumbstickX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        s_Analogs[kAnalogRightStickY]   = FilterAnalogInput(newInputState.rightThumbstickY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    }
#endif

#ifdef USE_KEYBOARD_MOUSE
    KbmUpdate();

    for (uint32_t i = 0; i < kNumKeys; ++i)
    {
        s_Buttons[0][i] = (s_Keybuffer[s_DXKeyMapping[i]] & 0x80) != 0;
    }

    for (uint32_t i = 0; i < 8; ++i)
    {
        if (s_MouseState.rgbButtons[i] > 0) s_Buttons[0][kMouse0 + i] = true;
    }

    s_Analogs[kAnalogMouseX] = (float)s_MouseState.lX * .0018f;
    s_Analogs[kAnalogMouseY] = (float)s_MouseState.lY * -.0018f;

    if (s_MouseState.lZ > 0)
        s_Analogs[kAnalogMouseScroll] = 1.0f;
    else if (s_MouseState.lZ < 0)
        s_Analogs[kAnalogMouseScroll] = -1.0f;
#endif

    // Update time duration for buttons pressed
    for (uint32_t i = 0; i < kNumDigitalInputs; ++i)
    {
        if (s_Buttons[0][i])
        {
            if (!s_Buttons[1][i])
                s_HoldDuration[i] = 0.0f;
            else
                s_HoldDuration[i] += frameDelta;
        }
    }

    for (uint32_t i = 0; i < kNumAnalogInputs; ++i)
    {
        s_AnalogsTC[i] = s_Analogs[i] * frameDelta;
    }

}

bool GameInput::IsAnyPressed( void )
{
    return s_Buttons[0] != 0;
}

bool GameInput::IsPressed( DigitalInput di )
{
    return s_Buttons[0][di];
}

bool GameInput::IsFirstPressed( DigitalInput di )
{
    return s_Buttons[0][di] && !s_Buttons[1][di];
}

bool GameInput::IsReleased( DigitalInput di )
{
    return !s_Buttons[0][di];
}

bool GameInput::IsFirstReleased( DigitalInput di )
{
    return !s_Buttons[0][di] && s_Buttons[1][di];
}

float GameInput::GetDurationPressed( DigitalInput di )
{
    return s_HoldDuration[di];
}

float GameInput::GetAnalogInput( AnalogInput ai )
{
    return s_Analogs[ai];
}

float GameInput::GetTimeCorrectedAnalogInput( AnalogInput ai )
{
    return s_AnalogsTC[ai];
}

uint32_t GameInput::GetGamepadCount()
{
#ifdef _GAMING_DESKTOP
    return (uint32_t)s_Gamepads.size();
#else
    return 0;
#endif
}

int32_t GameInput::GetBoundGamepadSlot()
{
#ifdef _GAMING_DESKTOP
    return s_BoundSlot;
#else
    return -1;
#endif
}

bool GameInput::GetBoundGamepadReading( GamepadState& out )
{
#ifdef _GAMING_DESKTOP
    if (s_BoundSlot < 0)
        return false;
    out = s_BoundState;
    return true;
#else
    (void)out;
    return false;
#endif
}

/*
 * goley-revival-linux — Open-Source DirectInput8 (dinput8.dll) Keyboard & Arrow Key Shim
 * for Linux (Wine / Proton / UMU)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Why this exists:
 *   1. In Goley Revival (`BinaryTrU.exe`), footballer movement is controlled by Arrow Keys
 *      (Up/Down/Left/Right), which `BinaryTrU.exe` maps via DirectInput8 to extended scancodes
 *      DIK_UP (0xC8), DIK_DOWN (0xD0), DIK_LEFT (0xCB), DIK_RIGHT (0xCD).
 *   2. In Wine / Proton (especially with dwVersion >= 0x0800), DirectInput8 enables RawInput
 *      hooks. Under XWayland or Turkish/standard layouts, arrow keys can arrive with unextended
 *      scancodes (DIK_NUMPAD8 0x48, DIK_NUMPAD2 0x50, DIK_NUMPAD4 0x4B, DIK_NUMPAD6 0x4D)
 *      or RawInput dropped events, causing footballer movement to fail completely in-game
 *      even while W/A/S/D work normally via standard keypresses.
 *   3. This lightweight 32-bit dinput8.dll proxy intercepts DirectInput8Create -> CreateDevice
 *      for GUID_SysKeyboard, remaps non-extended nav/arrow scancodes, and performs active
 *      state synchronization with GetAsyncKeyState when the game window is in the foreground.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dinput.h>

#ifdef GOLEY_NOSTDLIB
void __main(void) {}
#endif

typedef struct ProxyDI8 {
    IDirectInput8AVtbl *lpVtbl;
    IDirectInput8A *real_di8;
    ULONG ref_count;
} ProxyDI8;

typedef struct ProxyDevice {
    IDirectInputDevice8AVtbl *lpVtbl;
    IDirectInputDevice8A *real_dev;
    ULONG ref_count;
} ProxyDevice;

static HMODULE g_real_dinput8 = NULL;
static HMODULE g_self_module = NULL;
static HWND g_gameHwnd = NULL;

static HMODULE get_real_dinput8(void) {
    if (g_real_dinput8) return g_real_dinput8;

    WCHAR sysDir[MAX_PATH];
    UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len > 0 && len + 14 < MAX_PATH) {
        lstrcatW(sysDir, L"\\dinput8.dll");
        HMODULE h = LoadLibraryW(sysDir);
        if (h && h != g_self_module) {
            g_real_dinput8 = h;
            return g_real_dinput8;
        }
    }
    g_real_dinput8 = LoadLibraryA("C:\\windows\\system32\\dinput8.dll");
    return g_real_dinput8;
}

static BYTE s_dik_down[256];
static BYTE s_remapped[256];
static DWORD s_seq = 1000;

typedef struct {
    BYTE vk;
    BYTE dik;
    BYTE numpad_dik;
} NavKeyMapping;

static const NavKeyMapping kNavKeys[] = {
    { VK_UP,     0xC8, 0x48 }, // DIK_UP (0xC8), DIK_NUMPAD8 (0x48)
    { VK_DOWN,   0xD0, 0x50 }, // DIK_DOWN (0xD0), DIK_NUMPAD2 (0x50)
    { VK_LEFT,   0xCB, 0x4B }, // DIK_LEFT (0xCB), DIK_NUMPAD4 (0x4B)
    { VK_RIGHT,  0xCD, 0x4D }, // DIK_RIGHT (0xCD), DIK_NUMPAD6 (0x4D)
    { VK_HOME,   0xC7, 0x47 }, // DIK_HOME (0xC7), DIK_NUMPAD7 (0x47)
    { VK_END,    0xCF, 0x4F }, // DIK_END (0xCF), DIK_NUMPAD1 (0x4F)
    { VK_PRIOR,  0xC9, 0x49 }, // DIK_PRIOR (0xC9), DIK_NUMPAD9 (0x49)
    { VK_NEXT,   0xD1, 0x51 }, // DIK_NEXT (0xD1), DIK_NUMPAD3 (0x51)
    { VK_INSERT, 0xD2, 0x52 }, // DIK_INSERT (0xD2), DIK_NUMPAD0 (0x52)
    { VK_DELETE, 0xD3, 0x53 }, // DIK_DELETE (0xD3), DIK_DECIMAL (0x53)
};
#define NUM_NAV_KEYS (sizeof(kNavKeys) / sizeof(kNavKeys[0]))

// Forward declarations for Device VTable
static HRESULT WINAPI ProxyDev_QueryInterface(IDirectInputDevice8A *This, const IID* const riid, void **ppvObject);
static ULONG WINAPI ProxyDev_AddRef(IDirectInputDevice8A *This);
static ULONG WINAPI ProxyDev_Release(IDirectInputDevice8A *This);
static HRESULT WINAPI ProxyDev_GetCapabilities(IDirectInputDevice8A *This, LPDIDEVCAPS lpDIDevCaps);
static HRESULT WINAPI ProxyDev_EnumObjects(IDirectInputDevice8A *This, LPDIENUMDEVICEOBJECTSCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags);
static HRESULT WINAPI ProxyDev_GetProperty(IDirectInputDevice8A *This, const GUID* const rguidProp, LPDIPROPHEADER pdiph);
static HRESULT WINAPI ProxyDev_SetProperty(IDirectInputDevice8A *This, const GUID* const rguidProp, LPCDIPROPHEADER pdiph);
static HRESULT WINAPI ProxyDev_Acquire(IDirectInputDevice8A *This);
static HRESULT WINAPI ProxyDev_Unacquire(IDirectInputDevice8A *This);
static HRESULT WINAPI ProxyDev_GetDeviceState(IDirectInputDevice8A *This, DWORD cbData, LPVOID lpvData);
static HRESULT WINAPI ProxyDev_GetDeviceData(IDirectInputDevice8A *This, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags);
static HRESULT WINAPI ProxyDev_SetDataFormat(IDirectInputDevice8A *This, LPCDIDATAFORMAT lpdf);
static HRESULT WINAPI ProxyDev_SetEventNotification(IDirectInputDevice8A *This, HANDLE hEvent);
static HRESULT WINAPI ProxyDev_SetCooperativeLevel(IDirectInputDevice8A *This, HWND hwnd, DWORD dwFlags);
static HRESULT WINAPI ProxyDev_GetObjectInfo(IDirectInputDevice8A *This, LPDIDEVICEOBJECTINSTANCEA pdidoi, DWORD dwObj, DWORD dwHow);
static HRESULT WINAPI ProxyDev_GetDeviceInfo(IDirectInputDevice8A *This, LPDIDEVICEINSTANCEA pdidi);
static HRESULT WINAPI ProxyDev_RunControlPanel(IDirectInputDevice8A *This, HWND hwndOwner, DWORD dwFlags);
static HRESULT WINAPI ProxyDev_Initialize(IDirectInputDevice8A *This, HINSTANCE hinst, DWORD dwVersion, const GUID* const rguid);
static HRESULT WINAPI ProxyDev_CreateEffect(IDirectInputDevice8A *This, const GUID* const rguid, LPCDIEFFECT lpeff, LPDIRECTINPUTEFFECT *ppdeff, LPUNKNOWN punkOuter);
static HRESULT WINAPI ProxyDev_EnumEffects(IDirectInputDevice8A *This, LPDIENUMEFFECTSCALLBACKA lpCallback, LPVOID pvRef, DWORD dwEffType);
static HRESULT WINAPI ProxyDev_GetEffectInfo(IDirectInputDevice8A *This, LPDIEFFECTINFOA pdei, const GUID* const rguid);
static HRESULT WINAPI ProxyDev_GetForceFeedbackState(IDirectInputDevice8A *This, LPDWORD pdwOut);
static HRESULT WINAPI ProxyDev_SendForceFeedbackCommand(IDirectInputDevice8A *This, DWORD dwFlags);
static HRESULT WINAPI ProxyDev_EnumCreatedEffectObjects(IDirectInputDevice8A *This, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK lpCallback, LPVOID pvRef, DWORD fl);
static HRESULT WINAPI ProxyDev_Escape(IDirectInputDevice8A *This, LPDIEFFESCAPE pesc);
static HRESULT WINAPI ProxyDev_Poll(IDirectInputDevice8A *This);
static HRESULT WINAPI ProxyDev_SendDeviceData(IDirectInputDevice8A *This, DWORD cbObjectData, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD fl);
static HRESULT WINAPI ProxyDev_EnumEffectsInFile(IDirectInputDevice8A *This, LPCSTR lpszFileName, LPDIENUMEFFECTSINFILECALLBACK pec, LPVOID pvRef, DWORD dwFlags);
static HRESULT WINAPI ProxyDev_WriteEffectToFile(IDirectInputDevice8A *This, LPCSTR lpszFileName, DWORD dwEntries, LPDIFILEEFFECT rgDiFileEft, DWORD dwFlags);
static HRESULT WINAPI ProxyDev_BuildActionMap(IDirectInputDevice8A *This, LPDIACTIONFORMATA lpdiaf, LPCSTR lpszUserName, DWORD dwFlags);
static HRESULT WINAPI ProxyDev_SetActionMap(IDirectInputDevice8A *This, LPDIACTIONFORMATA lpdiaf, LPCSTR lpszUserName, DWORD dwFlags);
static HRESULT WINAPI ProxyDev_GetImageInfo(IDirectInputDevice8A *This, LPDIDEVICEIMAGEINFOHEADERA lpdiDevImageInfoHeader);

static IDirectInputDevice8AVtbl s_deviceVtbl = {
    ProxyDev_QueryInterface,
    ProxyDev_AddRef,
    ProxyDev_Release,
    ProxyDev_GetCapabilities,
    ProxyDev_EnumObjects,
    ProxyDev_GetProperty,
    ProxyDev_SetProperty,
    ProxyDev_Acquire,
    ProxyDev_Unacquire,
    ProxyDev_GetDeviceState,
    ProxyDev_GetDeviceData,
    ProxyDev_SetDataFormat,
    ProxyDev_SetEventNotification,
    ProxyDev_SetCooperativeLevel,
    ProxyDev_GetObjectInfo,
    ProxyDev_GetDeviceInfo,
    ProxyDev_RunControlPanel,
    ProxyDev_Initialize,
    ProxyDev_CreateEffect,
    ProxyDev_EnumEffects,
    ProxyDev_GetEffectInfo,
    ProxyDev_GetForceFeedbackState,
    ProxyDev_SendForceFeedbackCommand,
    ProxyDev_EnumCreatedEffectObjects,
    ProxyDev_Escape,
    ProxyDev_Poll,
    ProxyDev_SendDeviceData,
    ProxyDev_EnumEffectsInFile,
    ProxyDev_WriteEffectToFile,
    ProxyDev_BuildActionMap,
    ProxyDev_SetActionMap,
    ProxyDev_GetImageInfo
};

static HRESULT WINAPI ProxyDev_QueryInterface(IDirectInputDevice8A *This_, const IID* const riid, void **ppvObject) {
    ProxyDevice *This = (ProxyDevice*)This_;
    if (!ppvObject) return E_POINTER;
    if (riid && (
        (riid->Data1 == 0 && riid->Data2 == 0 && riid->Data3 == 0) || // IUnknown
        (riid->Data1 == 0x54D41080 && riid->Data2 == 0xDC15 && riid->Data3 == 0x4833) || // IDirectInputDevice8A
        (riid->Data1 == 0x54D41081 && riid->Data2 == 0xDC15 && riid->Data3 == 0x4833)    // IDirectInputDevice8W
    )) {
        This->ref_count++;
        *ppvObject = This;
        return S_OK;
    }
    return This->real_dev->lpVtbl->QueryInterface(This->real_dev, riid, ppvObject);
}

static ULONG WINAPI ProxyDev_AddRef(IDirectInputDevice8A *This_) {
    ProxyDevice *This = (ProxyDevice*)This_;
    This->real_dev->lpVtbl->AddRef(This->real_dev);
    return ++This->ref_count;
}

static ULONG WINAPI ProxyDev_Release(IDirectInputDevice8A *This_) {
    ProxyDevice *This = (ProxyDevice*)This_;
    ULONG r = This->real_dev->lpVtbl->Release(This->real_dev);
    if (--This->ref_count == 0) {
        HeapFree(GetProcessHeap(), 0, This);
    }
    return r;
}

static HRESULT WINAPI ProxyDev_GetCapabilities(IDirectInputDevice8A *This_, LPDIDEVCAPS lpDIDevCaps) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->GetCapabilities(This->real_dev, lpDIDevCaps);
}

static HRESULT WINAPI ProxyDev_EnumObjects(IDirectInputDevice8A *This_, LPDIENUMDEVICEOBJECTSCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->EnumObjects(This->real_dev, lpCallback, pvRef, dwFlags);
}

static HRESULT WINAPI ProxyDev_GetProperty(IDirectInputDevice8A *This_, const GUID* const rguidProp, LPDIPROPHEADER pdiph) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->GetProperty(This->real_dev, rguidProp, pdiph);
}

static HRESULT WINAPI ProxyDev_SetProperty(IDirectInputDevice8A *This_, const GUID* const rguidProp, LPCDIPROPHEADER pdiph) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->SetProperty(This->real_dev, rguidProp, pdiph);
}

static HRESULT WINAPI ProxyDev_Acquire(IDirectInputDevice8A *This_) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->Acquire(This->real_dev);
}

static HRESULT WINAPI ProxyDev_Unacquire(IDirectInputDevice8A *This_) {
    ProxyDevice *This = (ProxyDevice*)This_;
    for (int i = 0; i < 256; i++) {
        s_dik_down[i] = 0;
        s_remapped[i] = 0;
    }
    return This->real_dev->lpVtbl->Unacquire(This->real_dev);
}

static HRESULT WINAPI ProxyDev_GetDeviceState(IDirectInputDevice8A *This_, DWORD cbData, LPVOID lpvData) {
    ProxyDevice *This = (ProxyDevice*)This_;
    HRESULT hr = This->real_dev->lpVtbl->GetDeviceState(This->real_dev, cbData, lpvData);
    if (FAILED(hr) || !lpvData || cbData < 256) {
        return hr;
    }

    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    BOOL isFg = (fg != NULL && (fg == g_gameHwnd || fgPid == GetCurrentProcessId()));

    BYTE *keys = (BYTE*)lpvData;
    if (isFg) {
        for (size_t k = 0; k < NUM_NAV_KEYS; k++) {
            if (GetAsyncKeyState(kNavKeys[k].vk) & 0x8000) {
                keys[kNavKeys[k].dik] |= 0x80;
            }
        }
    }
    return hr;
}

static HRESULT WINAPI ProxyDev_GetDeviceData(IDirectInputDevice8A *This_, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) {
    ProxyDevice *This = (ProxyDevice*)This_;
    DWORD capacity = (pdwInOut) ? *pdwInOut : 0;

    HRESULT hr = This->real_dev->lpVtbl->GetDeviceData(This->real_dev, cbObjectData, rgdod, pdwInOut, dwFlags);
    if (FAILED(hr) || !rgdod || !pdwInOut || cbObjectData != sizeof(DIDEVICEOBJECTDATA)) {
        return hr;
    }

    BOOL isPeek = (dwFlags & 1) != 0;
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    BOOL isFg = (fg != NULL && (fg == g_gameHwnd || fgPid == GetCurrentProcessId()));

    DWORD count = *pdwInOut;

    // Pass 1: Remap any unextended scancodes in the returned buffer
    for (DWORD i = 0; i < count; i++) {
        DWORD ofs = rgdod[i].dwOfs;
        BOOL isDown = (rgdod[i].dwData & 0x80) != 0;

        for (size_t k = 0; k < NUM_NAV_KEYS; k++) {
            if (ofs == kNavKeys[k].numpad_dik) {
                if (isDown) {
                    SHORT vkState = GetAsyncKeyState(kNavKeys[k].vk);
                    if (vkState & 0x8000) {
                        rgdod[i].dwOfs = kNavKeys[k].dik;
                        if (!isPeek) s_remapped[kNavKeys[k].numpad_dik] = 1;
                        ofs = kNavKeys[k].dik;
                    }
                } else {
                    if (s_remapped[kNavKeys[k].numpad_dik]) {
                        rgdod[i].dwOfs = kNavKeys[k].dik;
                        if (!isPeek) s_remapped[kNavKeys[k].numpad_dik] = 0;
                        ofs = kNavKeys[k].dik;
                    }
                }
                break;
            }
        }

        if (!isPeek && (ofs & 0xFF) == ofs) {
            s_dik_down[ofs] = isDown ? 0x80 : 0x00;
        }
    }

    // Pass 2: Fallback active synthesis for Arrow keys (VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT)
    if (!isPeek && isFg) {
        DWORD now = GetTickCount();
        for (size_t k = 0; k < 4; k++) {
            BYTE dik = kNavKeys[k].dik;
            BYTE vk  = kNavKeys[k].vk;
            BYTE wanted = (GetAsyncKeyState(vk) & 0x8000) ? 0x80 : 0x00;

            if (wanted != s_dik_down[dik]) {
                if (*pdwInOut < capacity) {
                    rgdod[*pdwInOut].dwOfs = dik;
                    rgdod[*pdwInOut].dwData = wanted;
                    rgdod[*pdwInOut].dwTimeStamp = now;
                    rgdod[*pdwInOut].dwSequence = ++s_seq;
                    rgdod[*pdwInOut].uAppData = 0;
                    (*pdwInOut)++;
                    s_dik_down[dik] = wanted;
                }
            }
        }
    } else if (!isPeek && !isFg) {
        DWORD now = GetTickCount();
        for (size_t k = 0; k < 4; k++) {
            BYTE dik = kNavKeys[k].dik;
            if (s_dik_down[dik] != 0) {
                if (*pdwInOut < capacity) {
                    rgdod[*pdwInOut].dwOfs = dik;
                    rgdod[*pdwInOut].dwData = 0;
                    rgdod[*pdwInOut].dwTimeStamp = now;
                    rgdod[*pdwInOut].dwSequence = ++s_seq;
                    rgdod[*pdwInOut].uAppData = 0;
                    (*pdwInOut)++;
                    s_dik_down[dik] = 0;
                }
            }
        }
    }

    return hr;
}

static HRESULT WINAPI ProxyDev_SetDataFormat(IDirectInputDevice8A *This_, LPCDIDATAFORMAT lpdf) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->SetDataFormat(This->real_dev, lpdf);
}

static HRESULT WINAPI ProxyDev_SetEventNotification(IDirectInputDevice8A *This_, HANDLE hEvent) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->SetEventNotification(This->real_dev, hEvent);
}

static HRESULT WINAPI ProxyDev_SetCooperativeLevel(IDirectInputDevice8A *This_, HWND hwnd, DWORD dwFlags) {
    ProxyDevice *This = (ProxyDevice*)This_;
    if (hwnd) g_gameHwnd = hwnd;
    return This->real_dev->lpVtbl->SetCooperativeLevel(This->real_dev, hwnd, dwFlags);
}

static HRESULT WINAPI ProxyDev_GetObjectInfo(IDirectInputDevice8A *This_, LPDIDEVICEOBJECTINSTANCEA pdidoi, DWORD dwObj, DWORD dwHow) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->GetObjectInfo(This->real_dev, pdidoi, dwObj, dwHow);
}

static HRESULT WINAPI ProxyDev_GetDeviceInfo(IDirectInputDevice8A *This_, LPDIDEVICEINSTANCEA pdidi) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->GetDeviceInfo(This->real_dev, pdidi);
}

static HRESULT WINAPI ProxyDev_RunControlPanel(IDirectInputDevice8A *This_, HWND hwndOwner, DWORD dwFlags) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->RunControlPanel(This->real_dev, hwndOwner, dwFlags);
}

static HRESULT WINAPI ProxyDev_Initialize(IDirectInputDevice8A *This_, HINSTANCE hinst, DWORD dwVersion, const GUID* const rguid) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->Initialize(This->real_dev, hinst, dwVersion, rguid);
}

static HRESULT WINAPI ProxyDev_CreateEffect(IDirectInputDevice8A *This_, const GUID* const rguid, LPCDIEFFECT lpeff, LPDIRECTINPUTEFFECT *ppdeff, LPUNKNOWN punkOuter) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->CreateEffect(This->real_dev, rguid, lpeff, ppdeff, punkOuter);
}

static HRESULT WINAPI ProxyDev_EnumEffects(IDirectInputDevice8A *This_, LPDIENUMEFFECTSCALLBACKA lpCallback, LPVOID pvRef, DWORD dwEffType) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->EnumEffects(This->real_dev, lpCallback, pvRef, dwEffType);
}

static HRESULT WINAPI ProxyDev_GetEffectInfo(IDirectInputDevice8A *This_, LPDIEFFECTINFOA pdei, const GUID* const rguid) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->GetEffectInfo(This->real_dev, pdei, rguid);
}

static HRESULT WINAPI ProxyDev_GetForceFeedbackState(IDirectInputDevice8A *This_, LPDWORD pdwOut) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->GetForceFeedbackState(This->real_dev, pdwOut);
}

static HRESULT WINAPI ProxyDev_SendForceFeedbackCommand(IDirectInputDevice8A *This_, DWORD dwFlags) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->SendForceFeedbackCommand(This->real_dev, dwFlags);
}

static HRESULT WINAPI ProxyDev_EnumCreatedEffectObjects(IDirectInputDevice8A *This_, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK lpCallback, LPVOID pvRef, DWORD fl) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->EnumCreatedEffectObjects(This->real_dev, lpCallback, pvRef, fl);
}

static HRESULT WINAPI ProxyDev_Escape(IDirectInputDevice8A *This_, LPDIEFFESCAPE pesc) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->Escape(This->real_dev, pesc);
}

static HRESULT WINAPI ProxyDev_Poll(IDirectInputDevice8A *This_) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->Poll(This->real_dev);
}

static HRESULT WINAPI ProxyDev_SendDeviceData(IDirectInputDevice8A *This_, DWORD cbObjectData, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD fl) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->SendDeviceData(This->real_dev, cbObjectData, rgdod, pdwInOut, fl);
}

static HRESULT WINAPI ProxyDev_EnumEffectsInFile(IDirectInputDevice8A *This_, LPCSTR lpszFileName, LPDIENUMEFFECTSINFILECALLBACK pec, LPVOID pvRef, DWORD dwFlags) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->EnumEffectsInFile(This->real_dev, lpszFileName, pec, pvRef, dwFlags);
}

static HRESULT WINAPI ProxyDev_WriteEffectToFile(IDirectInputDevice8A *This_, LPCSTR lpszFileName, DWORD dwEntries, LPDIFILEEFFECT rgDiFileEft, DWORD dwFlags) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->WriteEffectToFile(This->real_dev, lpszFileName, dwEntries, rgDiFileEft, dwFlags);
}

static HRESULT WINAPI ProxyDev_BuildActionMap(IDirectInputDevice8A *This_, LPDIACTIONFORMATA lpdiaf, LPCSTR lpszUserName, DWORD dwFlags) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->BuildActionMap(This->real_dev, lpdiaf, lpszUserName, dwFlags);
}

static HRESULT WINAPI ProxyDev_SetActionMap(IDirectInputDevice8A *This_, LPDIACTIONFORMATA lpdiaf, LPCSTR lpszUserName, DWORD dwFlags) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->SetActionMap(This->real_dev, lpdiaf, lpszUserName, dwFlags);
}

static HRESULT WINAPI ProxyDev_GetImageInfo(IDirectInputDevice8A *This_, LPDIDEVICEIMAGEINFOHEADERA lpdiDevImageInfoHeader) {
    ProxyDevice *This = (ProxyDevice*)This_;
    return This->real_dev->lpVtbl->GetImageInfo(This->real_dev, lpdiDevImageInfoHeader);
}

// Forward declarations for DI8 VTable
static HRESULT WINAPI ProxyDI8_QueryInterface(IDirectInput8A *This, const IID* const riid, void **ppvObject);
static ULONG WINAPI ProxyDI8_AddRef(IDirectInput8A *This);
static ULONG WINAPI ProxyDI8_Release(IDirectInput8A *This);
static HRESULT WINAPI ProxyDI8_CreateDevice(IDirectInput8A *This, const GUID* const rguid, LPDIRECTINPUTDEVICE8A *lplpDirectInputDevice, LPUNKNOWN pUnkOuter);
static HRESULT WINAPI ProxyDI8_EnumDevices(IDirectInput8A *This, DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags);
static HRESULT WINAPI ProxyDI8_GetDeviceStatus(IDirectInput8A *This, const GUID* const rguidInstance);
static HRESULT WINAPI ProxyDI8_RunControlPanel(IDirectInput8A *This, HWND hwndOwner, DWORD dwFlags);
static HRESULT WINAPI ProxyDI8_Initialize(IDirectInput8A *This, HINSTANCE hinst, DWORD dwVersion);
static HRESULT WINAPI ProxyDI8_FindDevice(IDirectInput8A *This, const GUID* const rguid, LPCSTR pszName, LPGUID pguidInstance);
static HRESULT WINAPI ProxyDI8_EnumDevicesBySemantics(IDirectInput8A *This, LPCSTR ptszUserName, LPDIACTIONFORMATA lpdiActionFormat, LPDIENUMDEVICESBYSEMANTICSCBA lpCallback, LPVOID pvRef, DWORD dwFlags);
static HRESULT WINAPI ProxyDI8_ConfigureDevices(IDirectInput8A *This, LPDICONFIGUREDEVICESCALLBACK lpdiCallback, LPDICONFIGUREDEVICESPARAMSA lpdiCDParams, DWORD dwFlags, LPVOID pvRefData);

static IDirectInput8AVtbl s_di8Vtbl = {
    ProxyDI8_QueryInterface,
    ProxyDI8_AddRef,
    ProxyDI8_Release,
    ProxyDI8_CreateDevice,
    ProxyDI8_EnumDevices,
    ProxyDI8_GetDeviceStatus,
    ProxyDI8_RunControlPanel,
    ProxyDI8_Initialize,
    ProxyDI8_FindDevice,
    ProxyDI8_EnumDevicesBySemantics,
    ProxyDI8_ConfigureDevices
};

static HRESULT WINAPI ProxyDI8_QueryInterface(IDirectInput8A *This_, const IID* const riid, void **ppvObject) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    if (!ppvObject) return E_POINTER;
    if (riid && (
        (riid->Data1 == 0 && riid->Data2 == 0 && riid->Data3 == 0) || // IUnknown
        (riid->Data1 == 0xBF798030 && riid->Data2 == 0x483A && riid->Data3 == 0x4DA2) || // IDirectInput8A
        (riid->Data1 == 0xBF798031 && riid->Data2 == 0x483A && riid->Data3 == 0x4DA2)    // IDirectInput8W
    )) {
        This->ref_count++;
        *ppvObject = This;
        return S_OK;
    }
    return This->real_di8->lpVtbl->QueryInterface(This->real_di8, riid, ppvObject);
}

static ULONG WINAPI ProxyDI8_AddRef(IDirectInput8A *This_) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    This->real_di8->lpVtbl->AddRef(This->real_di8);
    return ++This->ref_count;
}

static ULONG WINAPI ProxyDI8_Release(IDirectInput8A *This_) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    ULONG r = This->real_di8->lpVtbl->Release(This->real_di8);
    if (--This->ref_count == 0) {
        HeapFree(GetProcessHeap(), 0, This);
    }
    return r;
}

static HRESULT WINAPI ProxyDI8_CreateDevice(IDirectInput8A *This_, const GUID* const rguid, LPDIRECTINPUTDEVICE8A *lplpDirectInputDevice, LPUNKNOWN pUnkOuter) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    HRESULT hr = This->real_di8->lpVtbl->CreateDevice(This->real_di8, rguid, lplpDirectInputDevice, pUnkOuter);
    if (FAILED(hr) || !lplpDirectInputDevice || !*lplpDirectInputDevice) {
        return hr;
    }

    // Check if rguid == GUID_SysKeyboard: {6F1D2B61-D5A0-11CF-BFC7-444553540000}
    if (rguid && rguid->Data1 == 0x6F1D2B61 && rguid->Data2 == 0xD5A0 && rguid->Data3 == 0x11CF) {
        ProxyDevice *pdev = (ProxyDevice*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ProxyDevice));
        if (pdev) {
            pdev->lpVtbl = &s_deviceVtbl;
            pdev->real_dev = *lplpDirectInputDevice;
            pdev->ref_count = 1;
            *lplpDirectInputDevice = (IDirectInputDevice8A*)pdev;
        }
    }
    return hr;
}

static HRESULT WINAPI ProxyDI8_EnumDevices(IDirectInput8A *This_, DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    return This->real_di8->lpVtbl->EnumDevices(This->real_di8, dwDevType, lpCallback, pvRef, dwFlags);
}

static HRESULT WINAPI ProxyDI8_GetDeviceStatus(IDirectInput8A *This_, const GUID* const rguidInstance) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    return This->real_di8->lpVtbl->GetDeviceStatus(This->real_di8, rguidInstance);
}

static HRESULT WINAPI ProxyDI8_RunControlPanel(IDirectInput8A *This_, HWND hwndOwner, DWORD dwFlags) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    return This->real_di8->lpVtbl->RunControlPanel(This->real_di8, hwndOwner, dwFlags);
}

static HRESULT WINAPI ProxyDI8_Initialize(IDirectInput8A *This_, HINSTANCE hinst, DWORD dwVersion) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    return This->real_di8->lpVtbl->Initialize(This->real_di8, hinst, dwVersion);
}

static HRESULT WINAPI ProxyDI8_FindDevice(IDirectInput8A *This_, const GUID* const rguid, LPCSTR pszName, LPGUID pguidInstance) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    return This->real_di8->lpVtbl->FindDevice(This->real_di8, rguid, pszName, pguidInstance);
}

static HRESULT WINAPI ProxyDI8_EnumDevicesBySemantics(IDirectInput8A *This_, LPCSTR ptszUserName, LPDIACTIONFORMATA lpdiActionFormat, LPDIENUMDEVICESBYSEMANTICSCBA lpCallback, LPVOID pvRef, DWORD dwFlags) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    return This->real_di8->lpVtbl->EnumDevicesBySemantics(This->real_di8, ptszUserName, lpdiActionFormat, lpCallback, pvRef, dwFlags);
}

static HRESULT WINAPI ProxyDI8_ConfigureDevices(IDirectInput8A *This_, LPDICONFIGUREDEVICESCALLBACK lpdiCallback, LPDICONFIGUREDEVICESPARAMSA lpdiCDParams, DWORD dwFlags, LPVOID pvRefData) {
    ProxyDI8 *This = (ProxyDI8*)This_;
    return This->real_di8->lpVtbl->ConfigureDevices(This->real_di8, lpdiCallback, lpdiCDParams, dwFlags, pvRefData);
}

// Exported DirectInput8 functions
HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID *ppvOut,
    LPUNKNOWN punkOuter
) {
    typedef HRESULT (WINAPI *PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    HMODULE hReal = get_real_dinput8();
    if (!hReal) return DIERR_GENERIC;

    PFN_DirectInput8Create pfn = (PFN_DirectInput8Create)GetProcAddress(hReal, "DirectInput8Create");
    if (!pfn) return DIERR_GENERIC;

    HRESULT hr = pfn(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    if (FAILED(hr) || !ppvOut || !*ppvOut) {
        return hr;
    }

    ProxyDI8 *pdi8 = (ProxyDI8*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ProxyDI8));
    if (pdi8) {
        pdi8->lpVtbl = &s_di8Vtbl;
        pdi8->real_di8 = (IDirectInput8A*)*ppvOut;
        pdi8->ref_count = 1;
        *ppvOut = pdi8;
    }
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void) {
    typedef HRESULT (WINAPI *PFN)(void);
    HMODULE h = get_real_dinput8();
    PFN p = h ? (PFN)GetProcAddress(h, "DllCanUnloadNow") : NULL;
    return p ? p() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
    typedef HRESULT (WINAPI *PFN)(REFCLSID, REFIID, LPVOID*);
    HMODULE h = get_real_dinput8();
    PFN p = h ? (PFN)GetProcAddress(h, "DllGetClassObject") : NULL;
    return p ? p(rclsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllRegisterServer(void) {
    typedef HRESULT (WINAPI *PFN)(void);
    HMODULE h = get_real_dinput8();
    PFN p = h ? (PFN)GetProcAddress(h, "DllRegisterServer") : NULL;
    return p ? p() : S_OK;
}

HRESULT WINAPI DllUnregisterServer(void) {
    typedef HRESULT (WINAPI *PFN)(void);
    HMODULE h = get_real_dinput8();
    PFN p = h ? (PFN)GetProcAddress(h, "DllUnregisterServer") : NULL;
    return p ? p() : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self_module = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

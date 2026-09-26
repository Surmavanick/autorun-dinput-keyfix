/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Surmavanick
 * https://github.com/Surmavanick/autorun-dinput-keyfix
 */
/*
 * harness.exe - stand-in for NFSHP2.exe to exercise NFSHP2KeyFix.asi under
 * plain Wine: imports DirectInputCreateA from dinput.dll exactly like the game
 * (so the plugin's IAT hook applies), creates a window, loads the plugin from
 * scripts/, creates the system keyboard device with the DirectInput 7 keyboard
 * data format, and prints which DIK codes are pressed every 250 ms.
 *
 * Build: see build.sh (zig cc -target x86-windows-gnu -O1 -o harness.exe harness.c -ldinput -luser32 -lkernel32)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef struct { DWORD dwOfs, dwData, dwTimeStamp, dwSequence; } DOD;
typedef struct { const GUID *pguid; DWORD dwOfs; DWORD dwType; DWORD dwFlags; } OBJFMT;
typedef struct { DWORD dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs; OBJFMT *rgodf; } DATAFMT;

static const GUID GUID_Key_ = {0x55728220,0xD33C,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID GUID_SysKeyboard_ = {0x6F1D2B61,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};

__declspec(dllimport) HRESULT WINAPI DirectInputCreateA(HINSTANCE, DWORD, void **, void *);

typedef HRESULT (WINAPI *PFN_CreateDevice)(void *, const GUID *, void **, void *);
typedef HRESULT (WINAPI *PFN_SetDataFormat)(void *, DATAFMT *);
typedef HRESULT (WINAPI *PFN_SetCoop)(void *, HWND, DWORD);
typedef HRESULT (WINAPI *PFN_Acquire)(void *);
typedef HRESULT (WINAPI *PFN_GetDeviceState)(void *, DWORD, void *);
typedef HRESULT (WINAPI *PFN_GetDeviceData)(void *, DWORD, DOD *, DWORD *, DWORD);
typedef HRESULT (WINAPI *PFN_SetProperty)(void *, const GUID *, void *);

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    OBJFMT objs[256]; DATAFMT fmt; void *di = NULL, *dev = NULL; void **vt;
    HRESULT hr; int i, ticks = 0, maxticks = argc > 1 ? atoi(argv[1]) : 40;
    WNDCLASSA wc = {0}; HWND hwnd; MSG msg;
    HMODULE plugin;
    setvbuf(stdout, NULL, _IONBF, 0);
    plugin = LoadLibraryA("scripts\\NFSHP2KeyFix.asi");
    printf("plugin: %p (err %lu)\n", (void *)plugin, GetLastError());

    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "kfharness";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "kfharness", "kfharness", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 200, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(hwnd);

    hr = DirectInputCreateA(GetModuleHandleA(NULL), 0x0700, &di, NULL);
    printf("DirectInputCreateA -> %08lx di=%p\n", hr, di);
    if (FAILED(hr)) return 1;
    vt = *(void ***)di;
    hr = ((PFN_CreateDevice)vt[3])(di, &GUID_SysKeyboard_, &dev, NULL);
    printf("CreateDevice(SysKeyboard) -> %08lx dev=%p\n", hr, dev);
    if (FAILED(hr)) return 1;
    vt = *(void ***)dev;
    for (i = 0; i < 256; i++) { objs[i].pguid = &GUID_Key_; objs[i].dwOfs = i; objs[i].dwType = 0x80000000 | (i << 8) | 0x4 /* DIDFT_BUTTON|MAKEINSTANCE|ANYINSTANCE-ish */; objs[i].dwFlags = 0; }
    /* c_dfDIKeyboard: 256 buttons, dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i) */
    for (i = 0; i < 256; i++) objs[i].dwType = 0x80000000 | 0x0000000C | ((i & 0xFFFF) << 8); /* DIDFT_OPTIONAL|DIDFT_BUTTON|MAKEINSTANCE */
    fmt.dwSize = sizeof fmt; fmt.dwObjSize = sizeof(OBJFMT); fmt.dwFlags = 2 /* DIDF_RELAXIS */; fmt.dwDataSize = 256; fmt.dwNumObjs = 256; fmt.rgodf = objs;
    hr = ((PFN_SetDataFormat)vt[11])(dev, &fmt);
    printf("SetDataFormat -> %08lx\n", hr);
    hr = ((PFN_SetCoop)vt[13])(dev, hwnd, 0x06 /* DISCL_NONEXCLUSIVE|DISCL_FOREGROUND */);
    printf("SetCooperativeLevel -> %08lx\n", hr);
    hr = ((PFN_Acquire)vt[7])(dev);
    printf("Acquire -> %08lx\n", hr);
    fflush(stdout);

    while (ticks < maxticks)
    {
        BYTE st[256]; char line[512]; int n = 0; DWORD cnt = 0;
        /* inject keys the way Autorun does: plain virtual keys, no scan code */
        if (ticks == 4)  keybd_event(VK_UP, 0, 0, 0);
        if (ticks == 10) keybd_event(VK_UP, 0, KEYEVENTF_KEYUP, 0);
        if (ticks == 13) keybd_event(VK_SPACE, 0, 0, 0);
        if (ticks == 19) keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0);
        if (ticks == 22) keybd_event(VK_LEFT, 0x4B, 0, 0);        /* scan code but no extended flag */
        if (ticks == 28) keybd_event(VK_LEFT, 0x4B, KEYEVENTF_KEYUP, 0);
        if (ticks == 31) keybd_event('C', 0, 0, 0);
        if (ticks == 37) keybd_event('C', 0, KEYEVENTF_KEYUP, 0);
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        hr = ((PFN_GetDeviceState)vt[9])(dev, 256, st);
        for (i = 0; i < 256; i++) if (st[i] & 0x80) n += sprintf(line + n, "%02x ", i);
        cnt = 0; ((PFN_GetDeviceData)vt[10])(dev, sizeof(DOD), NULL, &cnt, 1);
        if (n || (ticks % 8) == 0) printf("t=%2d GetDeviceState -> %08lx pressed=[%s] pending-events=%lu\n", ticks, hr, line, cnt);
        fflush(stdout);
        Sleep(250); ticks++;
    }
    DestroyWindow(hwnd);
    return 0;
}

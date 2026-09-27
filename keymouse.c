/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Surmavanick
 * https://github.com/Surmavanick/autorun-dinput-keyfix
 *
 * KeyMouse.dll - mouse helpers inside a game process, made for Autorun (Wine
 * on the Nintendo Switch), where only the A and B buttons can be mouse
 * buttons and every other Joy-Con button can only send a keyboard key.
 *
 *  1. Key -> held mouse button: map e.g. ZR to F13 in the game's keys.txt and
 *     this DLL holds the LEFT mouse button while F13 is down, so ZR + right
 *     stick drags a selection box in an RTS.
 *  2. SwapButtons=1 swaps the left and right mouse buttons for THIS game only:
 *     the game's windows are subclassed (WM_LBUTTON* <-> WM_RBUTTON*, MK_ bits)
 *     and GetAsyncKeyState/GetKeyState in the executable's import table are
 *     redirected so VK_LBUTTON/VK_RBUTTON polling agrees with the messages.
 *
 * Load it any way you like: as an ASI through the Ultimate ASI Loader, through
 * a launcher that loads modules (VEG MOD's config.xml <module lib="KeyMouse.dll"/>),
 * as EBUEulaX.dll for Age of Empires II 1.0c / UserPatch, or as a dsound.dll
 * proxy next to any game that imports DirectSoundCreate (see below). On wine-nx
 * the last two are the safe ones: they start the worker thread from a call the
 * game makes AFTER the module is fully loaded, never from DllMain.
 *
 * Configuration: KeyMouse.ini next to the DLL
 *   [KeyMouse]
 *   Left=0x7C       ; VK code that holds the left mouse button (F13), 0 = off
 *   Right=0x7D      ; VK code that holds the right mouse button (F14), 0 = off
 *   Middle=0        ; VK code for the middle button
 *   Poll=4          ; polling interval in ms
 *   SwapButtons=0   ; 1 = swap left and right mouse buttons in this game
 *   TouchJumpPx=48  ; a left click right after a cursor jump this big is a touch tap
 *   TouchWindowMs=250 ; ...within this many ms, and is NOT swapped (touch keeps selecting)
 *
 * "Left"/"Right" mean what the GAME sees, also when SwapButtons=1.
 * Log: KeyMouse.log next to the DLL.
 *
 * Build: zig cc -target x86-windows-gnu -shared -O2 -o KeyMouse.dll keymouse.c -luser32 -lkernel32
 *        zig cc -target x86-windows-gnu -shared -O2 -DEBUEULA_EXPORT -o EBUEulaX.dll keymouse.c -luser32 -lkernel32   (AoE2 1.0c/UserPatch)
 *        zig cc -target x86-windows-gnu -shared -O2 -DDSOUND_PROXY -o dsound.dll keymouse.c dsound.def -luser32 -lkernel32   (dsound.dll proxy)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

static HMODULE g_self; static HANDLE g_log = INVALID_HANDLE_VALUE; static HANDLE g_thread; static volatile int g_stop;
static int g_vk[3], g_poll = 4, g_swap, g_touch_px = 48, g_touch_ms = 250;
static POINT g_last_pos; static int g_have_pos; static DWORD g_jump_tick; static int g_touch_left; static DWORD g_touch_clicks;
static const DWORD down_flag[3] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN };
static const DWORD up_flag[3]   = { MOUSEEVENTF_LEFTUP,   MOUSEEVENTF_RIGHTUP,   MOUSEEVENTF_MIDDLEUP };

/* subclassed windows */
typedef struct { HWND hwnd; WNDPROC orig; } SUB;
static SUB g_subs[16]; static int g_nsubs;
static DWORD g_swapped_msgs;
typedef SHORT (WINAPI *PFN_KeyState)(int);
static PFN_KeyState orig_GetAsyncKeyState, orig_GetKeyState;

static void logf(const char *fmt, ...)
{
    char buf[512]; int n; DWORD w; va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = wsprintfA(buf, "[%08lu] ", (unsigned long)GetTickCount());
    va_start(ap, fmt); n += wvsprintfA(buf + n, fmt, ap); va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';
    WriteFile(g_log, buf, n, &w, NULL); FlushFileBuffers(g_log);
}

static void patch_ptr(void **slot, void *val)
{
    DWORD old = 0;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *slot = val;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

/* ---- key -> mouse button ---- */
static void send_button(int i, int down)
{
    INPUT in; int phys = i;
    if (g_swap && i < 2) phys = 1 - i; /* the game sees the swapped button, so press the other physical one */
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? down_flag[phys] : up_flag[phys];
    SendInput(1, &in, sizeof in);
}

/* ---- button swap: window subclass ---- */
static int swap_vk(int vk)
{
    if (vk == VK_LBUTTON) return VK_RBUTTON;
    if (vk == VK_RBUTTON) return VK_LBUTTON;
    return vk;
}

static SHORT WINAPI hk_GetAsyncKeyState(int vk) { return orig_GetAsyncKeyState(swap_vk(vk)); }
static SHORT WINAPI hk_GetKeyState(int vk) { return orig_GetKeyState(swap_vk(vk)); }

static WPARAM swap_mk(WPARAM wp)
{
    WPARAM r = wp & ~(WPARAM)(MK_LBUTTON | MK_RBUTTON);
    if (wp & MK_LBUTTON) r |= MK_RBUTTON;
    if (wp & MK_RBUTTON) r |= MK_LBUTTON;
    return r;
}

static LRESULT CALLBACK swap_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = NULL; int i; UINT m = msg; int keep = 0;
    for (i = 0; i < g_nsubs; i++) if (g_subs[i].hwnd == h) { orig = g_subs[i].orig; break; }
    if (!orig) return DefWindowProcA(h, msg, wp, lp);
    if (msg == WM_MOUSEMOVE)
    {
        POINT p; p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
        /* a touch tap moves the cursor in one big jump; the sticks move it in small steps */
        if (g_have_pos && (abs(p.x - g_last_pos.x) > g_touch_px || abs(p.y - g_last_pos.y) > g_touch_px)) g_jump_tick = GetTickCount();
        g_last_pos = p; g_have_pos = 1;
    }
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (g_jump_tick && GetTickCount() - g_jump_tick <= (DWORD)g_touch_ms) { g_touch_left = 1; keep = 1; g_touch_clicks++; }
        else { g_touch_left = 0; m = (msg == WM_LBUTTONDOWN) ? WM_RBUTTONDOWN : WM_RBUTTONDBLCLK; }
        break;
    case WM_LBUTTONUP:
        if (g_touch_left) { keep = 1; g_touch_left = 0; } else m = WM_RBUTTONUP;
        break;
    case WM_RBUTTONDOWN:   m = WM_LBUTTONDOWN;   break;
    case WM_RBUTTONUP:     m = WM_LBUTTONUP;     break;
    case WM_RBUTTONDBLCLK: m = WM_LBUTTONDBLCLK; break;
    }
    if (m != msg) { g_swapped_msgs++; if (g_swapped_msgs <= 12) logf("swapped msg %04x -> %04x wp=%08lx lp=%08lx time=%lu", msg, m, (unsigned long)wp, (unsigned long)lp, (unsigned long)GetMessageTime()); }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && !keep && !g_touch_left) wp = swap_mk(wp);
    if (msg == WM_NCDESTROY)
    {
        LRESULT r = CallWindowProcA(orig, h, msg, wp, lp);
        for (i = 0; i < g_nsubs; i++) if (g_subs[i].hwnd == h) { g_subs[i] = g_subs[--g_nsubs]; break; }
        return r;
    }
    return CallWindowProcA(orig, h, m, wp, lp);
}

static BOOL CALLBACK subclass_enum(HWND h, LPARAM lp)
{
    DWORD pid = 0; int i; WNDPROC prev; char cls[64];
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    for (i = 0; i < g_nsubs; i++) if (g_subs[i].hwnd == h) return TRUE;
    if (g_nsubs >= 16) return TRUE;
    prev = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)swap_wndproc);
    if (!prev) { logf("subclass of %08lx failed (err %lu)", (unsigned long)(ULONG_PTR)h, (unsigned long)GetLastError()); return TRUE; }
    g_subs[g_nsubs].hwnd = h; g_subs[g_nsubs].orig = prev; g_nsubs++;
    GetClassNameA(h, cls, sizeof cls);
    logf("subclassed window %08lx class \"%s\" for button swap", (unsigned long)(ULONG_PTR)h, cls);
    return TRUE;
}

/* ---- IAT patch of the game executable (GetAsyncKeyState / GetKeyState) ---- */
static void **find_iat_entry(HMODULE mod, const char *dll, const char *func)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!dir.VirtualAddress) return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++)
    {
        IMAGE_THUNK_DATA *oft, *ft;
        if (lstrcmpiA((const char *)(base + imp->Name), dll) != 0) continue;
        oft = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++)
        {
            IMAGE_IMPORT_BY_NAME *ibn;
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
            if (lstrcmpA((const char *)ibn->Name, func) == 0) return (void **)&ft->u1.Function;
        }
    }
    return NULL;
}

static void install_swap_hooks(void)
{
    HMODULE exe = GetModuleHandleA(NULL); void **slot;
    slot = find_iat_entry(exe, "user32.dll", "GetAsyncKeyState");
    if (slot) { orig_GetAsyncKeyState = (PFN_KeyState)*slot; patch_ptr(slot, (void *)hk_GetAsyncKeyState); logf("IAT user32!GetAsyncKeyState swapped"); }
    slot = find_iat_entry(exe, "user32.dll", "GetKeyState");
    if (slot) { orig_GetKeyState = (PFN_KeyState)*slot; patch_ptr(slot, (void *)hk_GetKeyState); logf("IAT user32!GetKeyState swapped"); }
    if (!orig_GetAsyncKeyState && !orig_GetKeyState) logf("no GetAsyncKeyState/GetKeyState import in the exe (messages are still swapped)");
}

/* ---- polling thread ---- */
static void start_poll_thread(void);

static DWORD WINAPI poll_thread(LPVOID arg)
{
    int held[3] = { 0, 0, 0 }, i, tick = 0; DWORD presses = 0, last_log;
    /* Autorun's loader (wine-nx) may still remap this module's pages right after DllMain; a thread
       that calls imports through the IAT during that window dies with a page fault. Wait it out
       without touching a single import. */
    { volatile unsigned long spin; for (spin = 0; spin < 60000000UL; spin++) ; }
    last_log = GetTickCount();
    logf("poll thread started (left=%02x right=%02x middle=%02x, %d ms, swap=%d)", g_vk[0], g_vk[1], g_vk[2], g_poll, g_swap);
    while (!g_stop)
    {
        for (i = 0; i < 3; i++)
        {
            int now;
            if (!g_vk[i]) continue;
            now = (GetAsyncKeyState(g_vk[i]) & 0x8000) != 0;
            if (now != held[i]) { held[i] = now; send_button(i, now); if (now) presses++; }
        }
        if (g_swap && (tick++ % (500 / g_poll + 1)) == 0) EnumWindows(subclass_enum, 0);
        if (GetTickCount() - last_log > 10000) { logf("%lu key-button presses, %lu swapped clicks, %lu touch taps kept, %d windows subclassed", (unsigned long)presses, (unsigned long)g_swapped_msgs, (unsigned long)g_touch_clicks, g_nsubs); last_log = GetTickCount(); }
        Sleep(g_poll);
    }
    for (i = 0; i < 3; i++) if (held[i]) send_button(i, 0);
    return 0;
}

#ifdef EBUEULA_EXPORT
/*
 * Built as EBUEulaX.dll: Age of Empires II (1.0c, UserPatch) loads this DLL at
 * start-up and calls EBUEula(...) (cdecl, 4-5 args) to show the license
 * agreement; a nonzero return means "accepted". Returning 1 here skips the
 * dialog, and because the game loads the DLL anyway it doubles as the loader
 * for the helpers above. The game FreeLibrary()s the module right after, so
 * DllMain pins it in memory with an extra reference.
 */
__declspec(dllexport) int __cdecl EBUEula(const char *key, ...)
{
    logf("EBUEula(\"%s\") called - reporting the license as accepted", key ? key : "(null)");
    start_poll_thread(); /* the module is fully loaded by now: safe to start the worker */
    return 1;
}
#endif

static void start_poll_thread(void)
{
    if (g_thread) return;
    g_thread = CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
    logf("worker thread %s", g_thread ? "created" : "FAILED");
}

#ifdef DSOUND_PROXY
/*
 * Built as dsound.dll: sits next to a game that imports DirectSoundCreate (with
 * "dsound"="native,builtin" in the prefix's DllOverrides), forwards every export
 * to the system dsound.dll and starts the worker on the game's first
 * DirectSoundCreate call. No ASI loader needed, no thread from DllMain.
 */
static HMODULE g_real_dsound;
static FARPROC real_dsound(const char *name)
{
    if (!g_real_dsound)
    {
        char path[MAX_PATH]; UINT n = GetSystemDirectoryA(path, MAX_PATH);
        lstrcpyA(path + n, "\\dsound.dll");
        g_real_dsound = LoadLibraryA(path);
        logf("system dsound.dll %s (%s)", g_real_dsound ? "loaded" : "FAILED", path);
    }
    return g_real_dsound ? GetProcAddress(g_real_dsound, name) : NULL;
}
#define DSERR_NODRIVER_ 0x88780078
typedef HRESULT (WINAPI *PFN_DSCreate)(const GUID *, void **, void *);
typedef HRESULT (WINAPI *PFN_DSEnum)(void *, void *);
typedef HRESULT (WINAPI *PFN_DSFullDuplex)(const GUID *, const GUID *, const void *, const void *, HWND, DWORD, void **, void **, void **, void *);
typedef HRESULT (WINAPI *PFN_DSDeviceID)(const GUID *, GUID *);
typedef HRESULT (WINAPI *PFN_DSClassObject)(const void *, const void *, void **);
typedef HRESULT (WINAPI *PFN_DSVoid)(void);

__declspec(dllexport) HRESULT WINAPI DirectSoundCreate(const GUID *dev, void **out, void *outer)
{
    PFN_DSCreate fn = (PFN_DSCreate)real_dsound("DirectSoundCreate");
    start_poll_thread(); /* the game is running normally by now: safe to start the worker */
    logf("DirectSoundCreate forwarded (%s)", fn ? "ok" : "no export");
    return fn ? fn(dev, out, outer) : (HRESULT)DSERR_NODRIVER_;
}
__declspec(dllexport) HRESULT WINAPI DirectSoundCreate8(const GUID *dev, void **out, void *outer)
{
    PFN_DSCreate fn = (PFN_DSCreate)real_dsound("DirectSoundCreate8");
    start_poll_thread();
    return fn ? fn(dev, out, outer) : (HRESULT)DSERR_NODRIVER_;
}
__declspec(dllexport) HRESULT WINAPI DirectSoundCaptureCreate(const GUID *dev, void **out, void *outer)
{ PFN_DSCreate fn = (PFN_DSCreate)real_dsound("DirectSoundCaptureCreate"); return fn ? fn(dev, out, outer) : (HRESULT)DSERR_NODRIVER_; }
__declspec(dllexport) HRESULT WINAPI DirectSoundCaptureCreate8(const GUID *dev, void **out, void *outer)
{ PFN_DSCreate fn = (PFN_DSCreate)real_dsound("DirectSoundCaptureCreate8"); return fn ? fn(dev, out, outer) : (HRESULT)DSERR_NODRIVER_; }
__declspec(dllexport) HRESULT WINAPI DirectSoundEnumerateA(void *cb, void *ctx)
{ PFN_DSEnum fn = (PFN_DSEnum)real_dsound("DirectSoundEnumerateA"); return fn ? fn(cb, ctx) : (HRESULT)DSERR_NODRIVER_; }
__declspec(dllexport) HRESULT WINAPI DirectSoundEnumerateW(void *cb, void *ctx)
{ PFN_DSEnum fn = (PFN_DSEnum)real_dsound("DirectSoundEnumerateW"); return fn ? fn(cb, ctx) : (HRESULT)DSERR_NODRIVER_; }
__declspec(dllexport) HRESULT WINAPI DirectSoundCaptureEnumerateA(void *cb, void *ctx)
{ PFN_DSEnum fn = (PFN_DSEnum)real_dsound("DirectSoundCaptureEnumerateA"); return fn ? fn(cb, ctx) : (HRESULT)DSERR_NODRIVER_; }
__declspec(dllexport) HRESULT WINAPI DirectSoundCaptureEnumerateW(void *cb, void *ctx)
{ PFN_DSEnum fn = (PFN_DSEnum)real_dsound("DirectSoundCaptureEnumerateW"); return fn ? fn(cb, ctx) : (HRESULT)DSERR_NODRIVER_; }
__declspec(dllexport) HRESULT WINAPI DirectSoundFullDuplexCreate(const GUID *cap, const GUID *rend, const void *cdesc, const void *bdesc, HWND hwnd, DWORD level, void **fd, void **cbuf, void **buf, void *outer)
{ PFN_DSFullDuplex fn = (PFN_DSFullDuplex)real_dsound("DirectSoundFullDuplexCreate"); return fn ? fn(cap, rend, cdesc, bdesc, hwnd, level, fd, cbuf, buf, outer) : (HRESULT)DSERR_NODRIVER_; }
__declspec(dllexport) HRESULT WINAPI GetDeviceID(const GUID *src, GUID *dst)
{ PFN_DSDeviceID fn = (PFN_DSDeviceID)real_dsound("GetDeviceID"); return fn ? fn(src, dst) : (HRESULT)DSERR_NODRIVER_; }
__declspec(dllexport) HRESULT WINAPI DllGetClassObject(const void *clsid, const void *iid, void **out)
{ PFN_DSClassObject fn = (PFN_DSClassObject)real_dsound("DllGetClassObject"); return fn ? fn(clsid, iid, out) : (HRESULT)0x80040111; /* CLASS_E_CLASSNOTAVAILABLE */ }
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void)
{ PFN_DSVoid fn = (PFN_DSVoid)real_dsound("DllCanUnloadNow"); return fn ? fn() : S_FALSE; }
#endif

static void plugin_path(char *out, const char *name)
{
    DWORD n = GetModuleFileNameA(g_self, out, MAX_PATH);
    while (n && out[n - 1] != '\\' && out[n - 1] != '/') n--;
    lstrcpyA(out + n, name);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char path[MAX_PATH];
        g_self = inst; DisableThreadLibraryCalls(inst);
        plugin_path(path, "KeyMouse.log");
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        plugin_path(path, "KeyMouse.ini");
        g_vk[0] = GetPrivateProfileIntA("KeyMouse", "Left", 0x7C, path);
        g_vk[1] = GetPrivateProfileIntA("KeyMouse", "Right", 0x7D, path);
        g_vk[2] = GetPrivateProfileIntA("KeyMouse", "Middle", 0, path);
        g_poll = GetPrivateProfileIntA("KeyMouse", "Poll", 4, path);
        g_swap = GetPrivateProfileIntA("KeyMouse", "SwapButtons", 0, path);
        g_touch_px = GetPrivateProfileIntA("KeyMouse", "TouchJumpPx", 48, path);
        g_touch_ms = GetPrivateProfileIntA("KeyMouse", "TouchWindowMs", 250, path);
        if (g_poll < 1) g_poll = 1;
        logf("KeyMouse loaded in pid %lu; ini=%s swap=%d", (unsigned long)GetCurrentProcessId(), path, g_swap);
#if defined(EBUEULA_EXPORT) || defined(DSOUND_PROXY)
        { HMODULE pin = NULL; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)DllMain, &pin); logf("pinned in memory: %s", pin ? "yes" : "no"); }
#endif
        if (g_swap) install_swap_hooks();
#if !defined(EBUEULA_EXPORT) && !defined(DSOUND_PROXY)
        start_poll_thread(); /* plain KeyMouse.dll / .asi: the loader's caller has no later hook to use */
#endif
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_stop = 1;
        if (g_log != INVALID_HANDLE_VALUE) { logf("unloading"); CloseHandle(g_log); }
    }
    return TRUE;
}

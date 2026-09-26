/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Surmavanick
 * https://github.com/Surmavanick/autorun-dinput-keyfix
 */
/*
 * NFSHP2KeyFix.asi - Need for Speed: Hot Pursuit 2 keyboard bridge for
 * Autorun (wine-nx) on the Nintendo Switch.
 *
 * Autorun turns Joy-Con buttons into injected keyboard events. Those events
 * reach the game's window messages (the menus work) but never reach the
 * DirectInput keyboard device that the race reads. This plugin, loaded by the
 * Ultimate ASI Loader (dinput.dll) from scripts/, hooks the game's
 * DirectInputCreateA import, wraps the system keyboard device and overlays
 * the DIK state array with keys seen through:
 *   - WM_KEYDOWN/WM_KEYUP retrieved by the game's thread (WH_GETMESSAGE hook)
 *   - GetAsyncKeyState
 *   - GetKeyboardState (the thread's own key state)
 * It also synthesizes buffered DIDEVICEOBJECTDATA events and never lets
 * Acquire fail. Everything it does is written to scripts/NFSHP2KeyFix.log.
 *
 * Build (zig):  zig cc -target x86-windows-gnu -shared -O2 -o NFSHP2KeyFix.asi keyfix.c -luser32 -lkernel32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ---- minimal DirectInput 7 (ANSI) definitions, no dinput.h needed ---- */
#define KF_DI_OK            ((HRESULT)0)
#define KF_DI_BUFFEROVERFLOW ((HRESULT)1)
#define KF_DIGDD_PEEK       1

typedef struct { DWORD dwOfs, dwData, dwTimeStamp, dwSequence; } KF_DIDOD;

static const GUID KF_GUID_SysKeyboard  = {0x6F1D2B61,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID KF_GUID_SysMouse     = {0x6F1D2B60,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID KF_IID_IDirectInput7A = {0x9A4CB684,0x236D,0x11D3,{0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE}};

typedef HRESULT (WINAPI *PFN_DICreateA)(HINSTANCE, DWORD, void **, void *);
typedef HRESULT (WINAPI *PFN_CreateDevice)(void *, const GUID *, void **, void *);
typedef HRESULT (WINAPI *PFN_CreateDeviceEx)(void *, const GUID *, const GUID *, void **, void *);
typedef HRESULT (WINAPI *PFN_Acquire)(void *);
typedef HRESULT (WINAPI *PFN_GetDeviceState)(void *, DWORD, void *);
typedef HRESULT (WINAPI *PFN_GetDeviceData)(void *, DWORD, KF_DIDOD *, DWORD *, DWORD);
typedef HRESULT (WINAPI *PFN_SetCoop)(void *, HWND, DWORD);
typedef HRESULT (WINAPI *PFN_QI)(void *, const GUID *, void **);
typedef ULONG   (WINAPI *PFN_Release)(void *);

/* IDirectInput7A vtable slots */
#define VT_DI_CreateDevice   3
#define VT_DI_CreateDeviceEx 9
/* IDirectInputDevice7A vtable slots */
#define VT_DEV_Acquire        7
#define VT_DEV_GetDeviceState 9
#define VT_DEV_GetDeviceData  10
#define VT_DEV_SetCoop        13

/* ---- state ---- */
static HMODULE g_self;
static HANDLE  g_log = INVALID_HANDLE_VALUE;
static HHOOK   g_msghook;
static int     g_hooks_ready;

static PFN_DICreateA      orig_create;
static PFN_CreateDevice   orig_cd, orig_cd7;
static PFN_CreateDeviceEx orig_cdex;
static PFN_Acquire        orig_acq;
static PFN_GetDeviceState orig_gds;
static PFN_GetDeviceData  orig_gdd;
static PFN_SetCoop        orig_coop;
static void **g_dev_vtbl;          /* the one device vtable we patched */

static void *g_kbd[8]; static int g_nkbd;

static BYTE vk2dik[256];
static volatile BYTE msgkeys[256];  /* by VK, from window messages */
static BYTE prev_dik[256], synth[256];

static KF_DIDOD evq[256]; static DWORD evq_head, evq_count, evq_seq; static int evq_overflow;

/* statistics for the log */
static DWORD n_calls, n_data_calls, n_failed, n_acq, n_coop, n_msgev;
static DWORD n_src_msg, n_src_async, n_src_state, n_src_orig;
static HRESULT last_fail;
static DWORD last_log_tick, last_logged_sum;

/* ---- logging (kernel32/user32 only) ---- */
static void logf(const char *fmt, ...)
{
    char buf[600]; int n; DWORD w; va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = wsprintfA(buf, "[%08lu] ", (unsigned long)GetTickCount());
    va_start(ap, fmt); n += wvsprintfA(buf + n, fmt, ap); va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';
    WriteFile(g_log, buf, n, &w, NULL);
    FlushFileBuffers(g_log);
}

static const char *guidstr(const GUID *g)
{
    static char s[64];
    if (!g) return "(null)";
    if (IsEqualGUID(g, &KF_GUID_SysKeyboard)) return "SysKeyboard";
    if (IsEqualGUID(g, &KF_GUID_SysMouse)) return "SysMouse";
    wsprintfA(s, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
              (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
              g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return s;
}

static void patch_ptr(void **slot, void *val)
{
    DWORD old = 0;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *slot = val;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

static int is_kbd(void *dev)
{
    int i; for (i = 0; i < g_nkbd; i++) if (g_kbd[i] == dev) return 1; return 0;
}

/* ---- key sources ---- */
static LRESULT CALLBACK getmsg_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && lp)
    {
        const MSG *m = (const MSG *)lp;
        if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) { msgkeys[m->wParam & 0xFF] = 1; n_msgev++; }
        else if (m->message == WM_KEYUP || m->message == WM_SYSKEYUP) msgkeys[m->wParam & 0xFF] = 0;
    }
    return CallNextHookEx(g_msghook, code, wp, lp);
}

static void build_vk_table(void)
{
    UINT sc, vk;
    for (sc = 1; sc < 0x80; sc++)
    {
        vk = MapVirtualKeyA(sc, 3 /* MAPVK_VSC_TO_VK_EX */);
        if (vk && vk < 256 && !vk2dik[vk]) vk2dik[vk] = (BYTE)sc;
    }
    /* extended keys (DIK = scan | 0x80) and generic modifiers */
    vk2dik[VK_UP] = 0xC8; vk2dik[VK_DOWN] = 0xD0; vk2dik[VK_LEFT] = 0xCB; vk2dik[VK_RIGHT] = 0xCD;
    vk2dik[VK_HOME] = 0xC7; vk2dik[VK_END] = 0xCF; vk2dik[VK_PRIOR] = 0xC9; vk2dik[VK_NEXT] = 0xD1;
    vk2dik[VK_INSERT] = 0xD2; vk2dik[VK_DELETE] = 0xD3; vk2dik[VK_DIVIDE] = 0xB5;
    vk2dik[VK_RCONTROL] = 0x9D; vk2dik[VK_RMENU] = 0xB8; vk2dik[VK_LWIN] = 0xDB; vk2dik[VK_RWIN] = 0xDC;
    vk2dik[VK_APPS] = 0xDD; vk2dik[VK_NUMLOCK] = 0x45; vk2dik[VK_PAUSE] = 0xC5; vk2dik[VK_SNAPSHOT] = 0xB7;
    vk2dik[VK_SHIFT] = 0x2A; vk2dik[VK_CONTROL] = 0x1D; vk2dik[VK_MENU] = 0x38;
    if (!vk2dik[VK_LSHIFT]) vk2dik[VK_LSHIFT] = 0x2A;
    if (!vk2dik[VK_RSHIFT]) vk2dik[VK_RSHIFT] = 0x36;
    if (!vk2dik[VK_LCONTROL]) vk2dik[VK_LCONTROL] = 0x1D;
    if (!vk2dik[VK_LMENU]) vk2dik[VK_LMENU] = 0x38;
    if (!vk2dik[VK_RETURN]) vk2dik[VK_RETURN] = 0x1C;
    if (!vk2dik[VK_ESCAPE]) vk2dik[VK_ESCAPE] = 0x01;
    if (!vk2dik[VK_SPACE]) vk2dik[VK_SPACE] = 0x39;
    if (!vk2dik[VK_TAB]) vk2dik[VK_TAB] = 0x0F;
    if (!vk2dik[VK_BACK]) vk2dik[VK_BACK] = 0x0E;
    if (!vk2dik[VK_OEM_COMMA]) vk2dik[VK_OEM_COMMA] = 0x33;
    if (!vk2dik[VK_OEM_PERIOD]) vk2dik[VK_OEM_PERIOD] = 0x34;
    if (!vk2dik[VK_OEM_MINUS]) vk2dik[VK_OEM_MINUS] = 0x0C;
    if (!vk2dik[VK_OEM_PLUS]) vk2dik[VK_OEM_PLUS] = 0x0D;
    {
        int n = 0, i; for (i = 0; i < 256; i++) if (vk2dik[i]) n++;
        logf("vk->dik table: %d entries (UP=%02x DOWN=%02x LEFT=%02x RIGHT=%02x SPACE=%02x C=%02x)",
             n, vk2dik[VK_UP], vk2dik[VK_DOWN], vk2dik[VK_LEFT], vk2dik[VK_RIGHT], vk2dik[VK_SPACE], vk2dik['C']);
    }
}

static void ensure_hooks(void)
{
    if (g_hooks_ready) return;
    g_hooks_ready = 1;
    build_vk_table();
    g_msghook = SetWindowsHookExA(WH_GETMESSAGE, getmsg_hook, g_self, GetCurrentThreadId());
    logf("WH_GETMESSAGE hook on thread %lu: %s (err %lu)", (unsigned long)GetCurrentThreadId(),
         g_msghook ? "installed" : "FAILED", (unsigned long)(g_msghook ? 0 : GetLastError()));
}

static void push_event(DWORD ofs, DWORD data)
{
    KF_DIDOD *e;
    if (evq_count == 256) { evq_head = (evq_head + 1) & 255; evq_count--; evq_overflow = 1; }
    e = &evq[(evq_head + evq_count) & 255];
    e->dwOfs = ofs; e->dwData = data; e->dwTimeStamp = GetTickCount(); e->dwSequence = ++evq_seq;
    evq_count++;
}

static void update_sources(void)
{
    BYTE cur[256], kbstate[256];
    int vk, i, any_msg = 0, any_async = 0, any_state = 0, have_state;
    ZeroMemory(cur, sizeof cur);
    have_state = GetKeyboardState(kbstate);
    for (vk = 1; vk < 256; vk++)
    {
        BYTE dik = vk2dik[vk]; int p;
        if (!dik) continue;
        p = 0;
        if (msgkeys[vk]) { p = 1; any_msg = 1; }
        if (GetAsyncKeyState(vk) & 0x8000) { p = 1; any_async = 1; }
        if (have_state && (kbstate[vk] & 0x80)) { p = 1; any_state = 1; }
        if (p) cur[dik] = 0x80;
    }
    if (any_msg) n_src_msg++; if (any_async) n_src_async++; if (any_state) n_src_state++;
    for (i = 0; i < 256; i++) if (cur[i] != prev_dik[i]) push_event((DWORD)i, cur[i]);
    CopyMemory(prev_dik, cur, 256);
    CopyMemory(synth, cur, 256);
}

static void maybe_log_stats(void)
{
    DWORD now = GetTickCount(), sum;
    if (now - last_log_tick < 3000) return;
    last_log_tick = now;
    sum = n_calls + n_data_calls + n_failed + n_src_msg + n_src_async + n_src_state + n_src_orig + n_msgev;
    if (sum == last_logged_sum) return;
    last_logged_sum = sum;
    {
        char pressed[96]; int i, n = 0; pressed[0] = 0;
        for (i = 0; i < 256 && n < 80; i++) if (synth[i]) n += wsprintfA(pressed + n, "%02x ", i);
        logf("stats: state=%lu data=%lu fail=%lu(last %08lx) msgev=%lu | frames with keys from msg=%lu async=%lu kbstate=%lu original=%lu | now=[%s]",
             (unsigned long)n_calls, (unsigned long)n_data_calls, (unsigned long)n_failed, (unsigned long)last_fail,
             (unsigned long)n_msgev, (unsigned long)n_src_msg, (unsigned long)n_src_async, (unsigned long)n_src_state,
             (unsigned long)n_src_orig, pressed);
    }
}

/* ---- device hooks ---- */
static HRESULT WINAPI hk_Acquire(void *self)
{
    HRESULT hr = orig_acq(self);
    if (!is_kbd(self)) return hr;
    if (n_acq < 6) logf("Acquire(kbd %08lx) -> %08lx%s", (unsigned long)(ULONG_PTR)self, (unsigned long)hr, FAILED(hr) ? " (masked to DI_OK)" : "");
    n_acq++;
    return FAILED(hr) ? KF_DI_OK : hr;
}

static HRESULT WINAPI hk_SetCoop(void *self, HWND hwnd, DWORD flags)
{
    HRESULT hr = orig_coop(self, hwnd, flags);
    if (is_kbd(self) && n_coop < 6)
        logf("SetCooperativeLevel(kbd %08lx, hwnd %08lx, flags %lx) -> %08lx", (unsigned long)(ULONG_PTR)self,
             (unsigned long)(ULONG_PTR)hwnd, (unsigned long)flags, (unsigned long)hr);
    if (is_kbd(self)) n_coop++;
    return hr;
}

static HRESULT WINAPI hk_GetDeviceState(void *self, DWORD cb, void *data)
{
    HRESULT hr = orig_gds(self, cb, data);
    BYTE *d = (BYTE *)data; int i;
    if (!is_kbd(self)) return hr;
    if (n_calls == 0) logf("GetDeviceState(kbd %08lx, cb=%lu) first call -> %08lx", (unsigned long)(ULONG_PTR)self, (unsigned long)cb, (unsigned long)hr);
    n_calls++;
    if (!data || cb < 256) return hr;
    if (FAILED(hr)) { ZeroMemory(data, cb); n_failed++; last_fail = hr; }
    else for (i = 0; i < 256; i++) if (d[i] & 0x80) { n_src_orig++; break; }
    update_sources();
    for (i = 0; i < 256; i++) if (synth[i]) d[i] = 0x80;
    maybe_log_stats();
    return KF_DI_OK;
}

static HRESULT WINAPI hk_GetDeviceData(void *self, DWORD cbobj, KF_DIDOD *rgdod, DWORD *inout, DWORD flags)
{
    DWORD want, got = 0, i, idx; HRESULT hr;
    if (!is_kbd(self) || !inout) return orig_gdd(self, cbobj, rgdod, inout, flags);
    if (n_data_calls == 0) logf("GetDeviceData(kbd, cbobj=%lu, rgdod=%s, n=%lu, flags=%lx) first call", (unsigned long)cbobj,
                                rgdod ? "buf" : "NULL", (unsigned long)*inout, (unsigned long)flags);
    n_data_calls++;
    want = *inout;
    update_sources();
    hr = orig_gdd(self, cbobj, rgdod, inout, flags);
    if (SUCCEEDED(hr)) got = *inout; else got = 0;
    if (!rgdod)
    {
        *inout = (want == INFINITE) ? got + evq_count : (got + evq_count > want ? want : got + evq_count);
        if (!(flags & KF_DIGDD_PEEK)) { evq_count = 0; evq_head = 0; }
        return KF_DI_OK;
    }
    i = got; idx = 0;
    while (i < want && idx < evq_count)
    {
        KF_DIDOD *src = &evq[(evq_head + idx) & 255];
        BYTE *dst = (BYTE *)rgdod + i * cbobj;
        ZeroMemory(dst, cbobj);
        CopyMemory(dst, src, cbobj < sizeof(KF_DIDOD) ? cbobj : sizeof(KF_DIDOD));
        i++; idx++;
    }
    if (!(flags & KF_DIGDD_PEEK)) { evq_head = (evq_head + idx) & 255; evq_count -= idx; }
    *inout = i;
    maybe_log_stats();
    if (evq_overflow) { evq_overflow = 0; return KF_DI_BUFFEROVERFLOW; }
    return KF_DI_OK;
}

static void register_kbd(void *dev)
{
    void **vt;
    if (!dev) return;
    if (!is_kbd(dev) && g_nkbd < 8) g_kbd[g_nkbd++] = dev;
    vt = *(void ***)dev;
    if (g_dev_vtbl == vt) return;
    if (g_dev_vtbl) { logf("second device vtable %08lx (first %08lx) - not patched", (unsigned long)(ULONG_PTR)vt, (unsigned long)(ULONG_PTR)g_dev_vtbl); return; }
    g_dev_vtbl = vt;
    orig_acq  = (PFN_Acquire)vt[VT_DEV_Acquire];
    orig_gds  = (PFN_GetDeviceState)vt[VT_DEV_GetDeviceState];
    orig_gdd  = (PFN_GetDeviceData)vt[VT_DEV_GetDeviceData];
    orig_coop = (PFN_SetCoop)vt[VT_DEV_SetCoop];
    patch_ptr(&vt[VT_DEV_Acquire], (void *)hk_Acquire);
    patch_ptr(&vt[VT_DEV_GetDeviceState], (void *)hk_GetDeviceState);
    patch_ptr(&vt[VT_DEV_GetDeviceData], (void *)hk_GetDeviceData);
    patch_ptr(&vt[VT_DEV_SetCoop], (void *)hk_SetCoop);
    logf("keyboard device %08lx: vtable %08lx patched (Acquire/GetDeviceState/GetDeviceData/SetCooperativeLevel)",
         (unsigned long)(ULONG_PTR)dev, (unsigned long)(ULONG_PTR)vt);
}

/* ---- IDirectInput hooks ---- */
static HRESULT WINAPI hk_CreateDevice(void *self, const GUID *g, void **out, void *outer)
{
    HRESULT hr = orig_cd(self, g, out, outer);
    logf("CreateDevice(%s) -> %08lx dev=%08lx", guidstr(g), (unsigned long)hr, (unsigned long)(ULONG_PTR)(out && SUCCEEDED(hr) ? *out : NULL));
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(g, &KF_GUID_SysKeyboard)) register_kbd(*out);
    return hr;
}

static HRESULT WINAPI hk_CreateDevice7(void *self, const GUID *g, void **out, void *outer)
{
    HRESULT hr = orig_cd7(self, g, out, outer);
    logf("CreateDevice[7](%s) -> %08lx dev=%08lx", guidstr(g), (unsigned long)hr, (unsigned long)(ULONG_PTR)(out && SUCCEEDED(hr) ? *out : NULL));
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(g, &KF_GUID_SysKeyboard)) register_kbd(*out);
    return hr;
}

static HRESULT WINAPI hk_CreateDeviceEx(void *self, const GUID *g, const GUID *iid, void **out, void *outer)
{
    HRESULT hr = orig_cdex(self, g, iid, out, outer);
    logf("CreateDeviceEx(%s) -> %08lx dev=%08lx", guidstr(g), (unsigned long)hr, (unsigned long)(ULONG_PTR)(out && SUCCEEDED(hr) ? *out : NULL));
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(g, &KF_GUID_SysKeyboard)) register_kbd(*out);
    return hr;
}

static HRESULT WINAPI hk_DICreateA(HINSTANCE h, DWORD ver, void **out, void *outer)
{
    HRESULT hr = orig_create(h, ver, out, outer);
    logf("DirectInputCreateA(ver=%04lx) -> %08lx obj=%08lx", (unsigned long)ver, (unsigned long)hr, (unsigned long)(ULONG_PTR)(out && SUCCEEDED(hr) ? *out : NULL));
    if (SUCCEEDED(hr) && out && *out)
    {
        void **vt = *(void ***)*out; void *p7 = NULL;
        if (vt[VT_DI_CreateDevice] != (void *)hk_CreateDevice && vt[VT_DI_CreateDevice] != (void *)hk_CreateDevice7)
        {
            orig_cd = (PFN_CreateDevice)vt[VT_DI_CreateDevice];
            patch_ptr(&vt[VT_DI_CreateDevice], (void *)hk_CreateDevice);
            logf("IDirectInput vtable %08lx: CreateDevice patched", (unsigned long)(ULONG_PTR)vt);
        }
        if (SUCCEEDED(((PFN_QI)vt[0])(*out, &KF_IID_IDirectInput7A, &p7)) && p7)
        {
            void **vt7 = *(void ***)p7;
            if (vt7[VT_DI_CreateDeviceEx] != (void *)hk_CreateDeviceEx)
            {
                orig_cdex = (PFN_CreateDeviceEx)vt7[VT_DI_CreateDeviceEx];
                patch_ptr(&vt7[VT_DI_CreateDeviceEx], (void *)hk_CreateDeviceEx);
            }
            if (vt7 != vt && vt7[VT_DI_CreateDevice] != (void *)hk_CreateDevice && vt7[VT_DI_CreateDevice] != (void *)hk_CreateDevice7)
            {
                orig_cd7 = (PFN_CreateDevice)vt7[VT_DI_CreateDevice];
                patch_ptr(&vt7[VT_DI_CreateDevice], (void *)hk_CreateDevice7);
            }
            logf("IDirectInput7A vtable %08lx: CreateDeviceEx patched%s", (unsigned long)(ULONG_PTR)vt7, vt7 != vt ? " (separate vtable)" : "");
            ((PFN_Release)vt7[2])(p7);
        }
        else logf("IDirectInput7A not supported by this object");
        ensure_hooks();
    }
    return hr;
}

/* ---- IAT patch of the game executable ---- */
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

static void open_log(void)
{
    char path[MAX_PATH]; DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    lstrcpyA(path + n, "NFSHP2KeyFix.log");
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        void **slot; HMODULE exe = GetModuleHandleA(NULL);
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        open_log();
        logf("NFSHP2KeyFix loaded; exe=%08lx pid=%lu tid=%lu dinput.dll=%08lx", (unsigned long)(ULONG_PTR)exe,
             (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(),
             (unsigned long)(ULONG_PTR)GetModuleHandleA("dinput.dll"));
        slot = find_iat_entry(exe, "DINPUT.dll", "DirectInputCreateA");
        if (slot)
        {
            orig_create = (PFN_DICreateA)*slot;
            patch_ptr(slot, (void *)hk_DICreateA);
            logf("IAT DINPUT.dll!DirectInputCreateA hooked (was %08lx)", (unsigned long)(ULONG_PTR)orig_create);
        }
        else logf("IAT entry for DINPUT.dll!DirectInputCreateA NOT found - plugin inactive");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_msghook) UnhookWindowsHookEx(g_msghook);
        if (g_log != INVALID_HANDLE_VALUE) { logf("unloading"); CloseHandle(g_log); }
    }
    return TRUE;
}

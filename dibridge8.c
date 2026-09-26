/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Surmavanick
 * https://github.com/Surmavanick/autorun-dinput-keyfix
 *
 * dinput8.dll proxy for Autorun (wine-nx) on the Nintendo Switch.
 *
 * Drop it next to a DirectInput 8 game's executable. It forwards
 * DirectInput8Create to the system dinput8.dll and wraps the keyboard and
 * mouse devices the game creates, feeding them from the Win32 key state and
 * cursor position that Autorun's controller-to-keyboard/mouse injection does
 * reach (GetAsyncKeyState, GetKeyboardState, GetCursorPos), because the
 * injected events never reach DirectInput's own hooks on that runtime.
 *
 *   keyboard: DIK state overlay + synthesized buffered events
 *   mouse:    relative X/Y from cursor movement, buttons 0..2 from
 *             VK_LBUTTON/VK_RBUTTON/VK_MBUTTON, both as immediate state
 *             (DIMOUSESTATE/DIMOUSESTATE2) and as buffered events
 *
 * Log: dinput8-bridge.log next to the DLL.
 *
 * Build: zig cc -target x86-windows-gnu -shared -O2 -o dinput8.dll dibridge8.c -luser32 -lkernel32 -Wl,--kill-at
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define KF_DI_OK             ((HRESULT)0)
#define KF_DI_BUFFEROVERFLOW ((HRESULT)1)
#define KF_DIGDD_PEEK        1

typedef struct { DWORD dwOfs, dwData, dwTimeStamp, dwSequence; } KF_DIDOD;

static const GUID KF_GUID_SysKeyboard = {0x6F1D2B61,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID KF_GUID_SysMouse    = {0x6F1D2B60,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID KF_IID_IDirectInput8A = {0xBF798030,0x483A,0x4DA2,{0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};
static const GUID KF_IID_IDirectInput8W = {0xBF798031,0x483A,0x4DA2,{0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};

typedef HRESULT (WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, const GUID *, void **, void *);
typedef HRESULT (WINAPI *PFN_CreateDevice)(void *, const GUID *, void **, void *);
typedef HRESULT (WINAPI *PFN_Acquire)(void *);
typedef HRESULT (WINAPI *PFN_GetDeviceState)(void *, DWORD, void *);
typedef HRESULT (WINAPI *PFN_GetDeviceData)(void *, DWORD, KF_DIDOD *, DWORD *, DWORD);
typedef HRESULT (WINAPI *PFN_SetDataFormat)(void *, const void *);
typedef HRESULT (WINAPI *PFN_SetCoop)(void *, HWND, DWORD);
typedef HRESULT (WINAPI *PFN_Generic)(void);

#define VT_DI_CreateDevice    3
#define VT_DEV_Acquire        7
#define VT_DEV_GetDeviceState 9
#define VT_DEV_GetDeviceData  10
#define VT_DEV_SetDataFormat  11
#define VT_DEV_SetCoop        13

/* ---- forwarding to the real dinput8.dll ---- */
static HMODULE g_self, g_real;
static PFN_DI8Create real_DirectInput8Create;
static PFN_Generic   real_DllCanUnloadNow, real_DllGetClassObject, real_DllRegisterServer, real_DllUnregisterServer;

/* ---- patched vtables (A and W interfaces have separate ones) ---- */
typedef struct { void **vt; PFN_CreateDevice cd; } DIVT;
typedef struct { void **vt; PFN_Acquire acq; PFN_GetDeviceState gds; PFN_GetDeviceData gdd; PFN_SetDataFormat sdf; PFN_SetCoop coop; } DEVVT;
static DIVT g_divt[4]; static int g_ndivt;
static DEVVT g_devvt[4]; static int g_ndevvt;

/* ---- devices ---- */
#define KIND_KBD 1
#define KIND_MOUSE 2
typedef struct {
    void *dev; int kind;
    KF_DIDOD q[256]; DWORD qhead, qcount, qseq; int qoverflow;
    /* keyboard */
    BYTE prev[256];
    /* mouse */
    int have_pos; POINT last; LONG pend_x, pend_y; BYTE btn[3]; DWORD data_size;
} DEVICE;
static DEVICE g_devs[8]; static int g_ndevs;

static HANDLE g_log = INVALID_HANDLE_VALUE;
static HHOOK g_msghook; static int g_hooks_ready;
static BYTE vk2dik[256]; static volatile BYTE msgkeys[256];
static DWORD n_calls, n_data_calls, n_failed, n_msgev, n_src_msg, n_src_async, n_src_state, n_src_orig, n_mouse_moves, n_mouse_btn;
static HRESULT last_fail; static DWORD last_log_tick, last_logged_sum;
static BYTE synth[256];

static void logf(const char *fmt, ...)
{
    char buf[600]; int n; DWORD w; va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = wsprintfA(buf, "[%08lu] ", (unsigned long)GetTickCount());
    va_start(ap, fmt); n += wvsprintfA(buf + n, fmt, ap); va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';
    WriteFile(g_log, buf, n, &w, NULL); FlushFileBuffers(g_log);
}

static const char *guidstr(const GUID *g)
{
    static char s[64];
    if (!g) return "(null)";
    if (IsEqualGUID(g, &KF_GUID_SysKeyboard)) return "SysKeyboard";
    if (IsEqualGUID(g, &KF_GUID_SysMouse)) return "SysMouse";
    if (IsEqualGUID(g, &KF_IID_IDirectInput8A)) return "IDirectInput8A";
    if (IsEqualGUID(g, &KF_IID_IDirectInput8W)) return "IDirectInput8W";
    wsprintfA(s, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (unsigned long)g->Data1, g->Data2, g->Data3,
              g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return s;
}

static void patch_ptr(void **slot, void *val)
{
    DWORD old = 0;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *slot = val;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

static DEVICE *find_dev(void *dev) { int i; for (i = 0; i < g_ndevs; i++) if (g_devs[i].dev == dev) return &g_devs[i]; return NULL; }
static DEVVT *find_devvt(void **vt) { int i; for (i = 0; i < g_ndevvt; i++) if (g_devvt[i].vt == vt) return &g_devvt[i]; return NULL; }
static DIVT *find_divt(void **vt) { int i; for (i = 0; i < g_ndivt; i++) if (g_divt[i].vt == vt) return &g_divt[i]; return NULL; }

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
    UINT sc, vk; int n = 0, i;
    for (sc = 1; sc < 0x80; sc++) { vk = MapVirtualKeyA(sc, 3); if (vk && vk < 256 && !vk2dik[vk]) vk2dik[vk] = (BYTE)sc; }
    vk2dik[VK_UP] = 0xC8; vk2dik[VK_DOWN] = 0xD0; vk2dik[VK_LEFT] = 0xCB; vk2dik[VK_RIGHT] = 0xCD;
    vk2dik[VK_HOME] = 0xC7; vk2dik[VK_END] = 0xCF; vk2dik[VK_PRIOR] = 0xC9; vk2dik[VK_NEXT] = 0xD1;
    vk2dik[VK_INSERT] = 0xD2; vk2dik[VK_DELETE] = 0xD3; vk2dik[VK_DIVIDE] = 0xB5;
    vk2dik[VK_RCONTROL] = 0x9D; vk2dik[VK_RMENU] = 0xB8; vk2dik[VK_LWIN] = 0xDB; vk2dik[VK_RWIN] = 0xDC;
    vk2dik[VK_APPS] = 0xDD; vk2dik[VK_NUMLOCK] = 0x45; vk2dik[VK_PAUSE] = 0xC5; vk2dik[VK_SNAPSHOT] = 0xB7;
    vk2dik[VK_SHIFT] = 0x2A; vk2dik[VK_CONTROL] = 0x1D; vk2dik[VK_MENU] = 0x38;
    if (!vk2dik[VK_LSHIFT]) vk2dik[VK_LSHIFT] = 0x2A; if (!vk2dik[VK_RSHIFT]) vk2dik[VK_RSHIFT] = 0x36;
    if (!vk2dik[VK_LCONTROL]) vk2dik[VK_LCONTROL] = 0x1D; if (!vk2dik[VK_LMENU]) vk2dik[VK_LMENU] = 0x38;
    if (!vk2dik[VK_RETURN]) vk2dik[VK_RETURN] = 0x1C; if (!vk2dik[VK_ESCAPE]) vk2dik[VK_ESCAPE] = 0x01;
    if (!vk2dik[VK_SPACE]) vk2dik[VK_SPACE] = 0x39; if (!vk2dik[VK_TAB]) vk2dik[VK_TAB] = 0x0F;
    if (!vk2dik[VK_BACK]) vk2dik[VK_BACK] = 0x0E; if (!vk2dik[VK_OEM_COMMA]) vk2dik[VK_OEM_COMMA] = 0x33;
    if (!vk2dik[VK_OEM_PERIOD]) vk2dik[VK_OEM_PERIOD] = 0x34; if (!vk2dik[VK_OEM_MINUS]) vk2dik[VK_OEM_MINUS] = 0x0C;
    if (!vk2dik[VK_OEM_PLUS]) vk2dik[VK_OEM_PLUS] = 0x0D;
    /* mouse buttons are not keyboard keys */
    vk2dik[VK_LBUTTON] = vk2dik[VK_RBUTTON] = vk2dik[VK_MBUTTON] = vk2dik[VK_XBUTTON1] = vk2dik[VK_XBUTTON2] = 0;
    for (i = 0; i < 256; i++) if (vk2dik[i]) n++;
    logf("vk->dik table: %d entries (UP=%02x DOWN=%02x LEFT=%02x RIGHT=%02x SPACE=%02x)", n, vk2dik[VK_UP], vk2dik[VK_DOWN], vk2dik[VK_LEFT], vk2dik[VK_RIGHT], vk2dik[VK_SPACE]);
}

static void ensure_hooks(void)
{
    if (g_hooks_ready) return;
    g_hooks_ready = 1;
    build_vk_table();
    g_msghook = SetWindowsHookExA(WH_GETMESSAGE, getmsg_hook, g_self, GetCurrentThreadId());
    logf("WH_GETMESSAGE hook on thread %lu: %s (err %lu)", (unsigned long)GetCurrentThreadId(), g_msghook ? "installed" : "FAILED", (unsigned long)(g_msghook ? 0 : GetLastError()));
}

static void push_event(DEVICE *d, DWORD ofs, DWORD data)
{
    KF_DIDOD *e;
    if (d->qcount == 256) { d->qhead = (d->qhead + 1) & 255; d->qcount--; d->qoverflow = 1; }
    e = &d->q[(d->qhead + d->qcount) & 255];
    e->dwOfs = ofs; e->dwData = data; e->dwTimeStamp = GetTickCount(); e->dwSequence = ++d->qseq;
    d->qcount++;
}

/* keyboard: refresh synth[] from the sources and queue changes on the device */
static void update_keyboard(DEVICE *d)
{
    BYTE cur[256], kbstate[256]; int vk, i, any_msg = 0, any_async = 0, any_state = 0, have_state;
    ZeroMemory(cur, sizeof cur);
    have_state = GetKeyboardState(kbstate);
    for (vk = 1; vk < 256; vk++)
    {
        BYTE dik = vk2dik[vk]; int p = 0;
        if (!dik) continue;
        if (msgkeys[vk]) { p = 1; any_msg = 1; }
        if (GetAsyncKeyState(vk) & 0x8000) { p = 1; any_async = 1; }
        if (have_state && (kbstate[vk] & 0x80)) { p = 1; any_state = 1; }
        if (p) cur[dik] = 0x80;
    }
    if (any_msg) n_src_msg++; if (any_async) n_src_async++; if (any_state) n_src_state++;
    for (i = 0; i < 256; i++) if (cur[i] != d->prev[i]) push_event(d, (DWORD)i, cur[i]);
    CopyMemory(d->prev, cur, 256); CopyMemory(synth, cur, 256);
}

/* mouse: accumulate cursor deltas and button state, queue changes */
static void update_mouse(DEVICE *d)
{
    POINT p; BYTE b[3]; int i;
    if (GetCursorPos(&p))
    {
        if (d->have_pos) { LONG dx = p.x - d->last.x, dy = p.y - d->last.y; if (dx || dy) { d->pend_x += dx; d->pend_y += dy; n_mouse_moves++; } }
        d->last = p; d->have_pos = 1;
    }
    b[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 0x80 : 0;
    b[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 0x80 : 0;
    b[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 0x80 : 0;
    for (i = 0; i < 3; i++) if (b[i] != d->btn[i]) { push_event(d, 12 + i, b[i]); d->btn[i] = b[i]; n_mouse_btn++; }
}

static void flush_mouse_motion_events(DEVICE *d)
{
    if (d->pend_x) { push_event(d, 0, (DWORD)d->pend_x); d->pend_x = 0; }
    if (d->pend_y) { push_event(d, 4, (DWORD)d->pend_y); d->pend_y = 0; }
}

static void maybe_log_stats(void)
{
    DWORD now = GetTickCount(), sum;
    if (now - last_log_tick < 3000) return;
    last_log_tick = now;
    sum = n_calls + n_data_calls + n_failed + n_src_msg + n_src_async + n_src_state + n_src_orig + n_msgev + n_mouse_moves + n_mouse_btn;
    if (sum == last_logged_sum) return;
    last_logged_sum = sum;
    {
        char pressed[96]; int i, n = 0; pressed[0] = 0;
        for (i = 0; i < 256 && n < 80; i++) if (synth[i]) n += wsprintfA(pressed + n, "%02x ", i);
        logf("stats: state=%lu data=%lu fail=%lu(last %08lx) msgev=%lu | keys from msg=%lu async=%lu kbstate=%lu original=%lu | mouse moves=%lu btn=%lu | now=[%s]",
             (unsigned long)n_calls, (unsigned long)n_data_calls, (unsigned long)n_failed, (unsigned long)last_fail, (unsigned long)n_msgev,
             (unsigned long)n_src_msg, (unsigned long)n_src_async, (unsigned long)n_src_state, (unsigned long)n_src_orig,
             (unsigned long)n_mouse_moves, (unsigned long)n_mouse_btn, pressed);
    }
}

/* ---- device hooks ---- */
static HRESULT WINAPI hk_Acquire(void *self)
{
    DEVVT *v = find_devvt(*(void ***)self); DEVICE *d = find_dev(self); HRESULT hr;
    if (!v) return E_FAIL;
    hr = v->acq(self);
    if (!d) return hr;
    if (n_calls < 4 || FAILED(hr)) logf("Acquire(%s %08lx) -> %08lx%s", d->kind == KIND_KBD ? "kbd" : "mouse", (unsigned long)(ULONG_PTR)self, (unsigned long)hr, FAILED(hr) ? " (masked to DI_OK)" : "");
    return FAILED(hr) ? KF_DI_OK : hr;
}

static HRESULT WINAPI hk_SetDataFormat(void *self, const void *fmt)
{
    DEVVT *v = find_devvt(*(void ***)self); DEVICE *d = find_dev(self); HRESULT hr;
    if (!v) return E_FAIL;
    hr = v->sdf(self, fmt);
    if (d && fmt)
    {
        const DWORD *f = (const DWORD *)fmt; /* dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs */
        d->data_size = f[3];
        logf("SetDataFormat(%s %08lx) dataSize=%lu numObjs=%lu flags=%lx -> %08lx", d->kind == KIND_KBD ? "kbd" : "mouse", (unsigned long)(ULONG_PTR)self, (unsigned long)f[3], (unsigned long)f[4], (unsigned long)f[2], (unsigned long)hr);
    }
    return hr;
}

static HRESULT WINAPI hk_SetCoop(void *self, HWND hwnd, DWORD flags)
{
    DEVVT *v = find_devvt(*(void ***)self); DEVICE *d = find_dev(self); HRESULT hr;
    if (!v) return E_FAIL;
    hr = v->coop(self, hwnd, flags);
    if (d) logf("SetCooperativeLevel(%s %08lx, hwnd %08lx, flags %lx) -> %08lx", d->kind == KIND_KBD ? "kbd" : "mouse", (unsigned long)(ULONG_PTR)self, (unsigned long)(ULONG_PTR)hwnd, (unsigned long)flags, (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI hk_GetDeviceState(void *self, DWORD cb, void *data)
{
    DEVVT *v = find_devvt(*(void ***)self); DEVICE *d = find_dev(self); HRESULT hr; BYTE *p = (BYTE *)data; int i;
    if (!v) return E_FAIL;
    hr = v->gds(self, cb, data);
    if (!d || !data) return hr;
    if (n_calls == 0) logf("GetDeviceState(%s, cb=%lu) first call -> %08lx", d->kind == KIND_KBD ? "kbd" : "mouse", (unsigned long)cb, (unsigned long)hr);
    n_calls++;
    if (FAILED(hr)) { ZeroMemory(data, cb); n_failed++; last_fail = hr; }
    if (d->kind == KIND_KBD && cb >= 256)
    {
        if (SUCCEEDED(hr)) for (i = 0; i < 256; i++) if (p[i] & 0x80) { n_src_orig++; break; }
        update_keyboard(d);
        for (i = 0; i < 256; i++) if (synth[i]) p[i] = 0x80;
        d->qcount = 0; d->qhead = 0; /* immediate-mode readers do not want the queue to grow */
    }
    else if (d->kind == KIND_MOUSE && cb >= 16)
    {
        LONG *ax = (LONG *)data;
        update_mouse(d);
        ax[0] += d->pend_x; ax[1] += d->pend_y; d->pend_x = d->pend_y = 0;
        for (i = 0; i < 3; i++) if (d->btn[i]) p[12 + i] = 0x80;
        d->qcount = 0; d->qhead = 0;
    }
    maybe_log_stats();
    return KF_DI_OK;
}

static HRESULT WINAPI hk_GetDeviceData(void *self, DWORD cbobj, KF_DIDOD *rgdod, DWORD *inout, DWORD flags)
{
    DEVVT *v = find_devvt(*(void ***)self); DEVICE *d = find_dev(self); HRESULT hr; DWORD want, got = 0, i, idx;
    if (!v) return E_FAIL;
    if (!d || !inout) return v->gdd(self, cbobj, rgdod, inout, flags);
    if (n_data_calls == 0) logf("GetDeviceData(%s, cbobj=%lu, rgdod=%s, n=%lu, flags=%lx) first call", d->kind == KIND_KBD ? "kbd" : "mouse", (unsigned long)cbobj, rgdod ? "buf" : "NULL", (unsigned long)*inout, (unsigned long)flags);
    n_data_calls++;
    want = *inout;
    if (d->kind == KIND_KBD) update_keyboard(d); else { update_mouse(d); flush_mouse_motion_events(d); }
    hr = v->gdd(self, cbobj, rgdod, inout, flags);
    got = SUCCEEDED(hr) ? *inout : 0;
    if (!rgdod)
    {
        *inout = (want == INFINITE) ? got + d->qcount : (got + d->qcount > want ? want : got + d->qcount);
        if (!(flags & KF_DIGDD_PEEK)) { d->qcount = 0; d->qhead = 0; }
        return KF_DI_OK;
    }
    i = got; idx = 0;
    while (i < want && idx < d->qcount)
    {
        KF_DIDOD *src = &d->q[(d->qhead + idx) & 255]; BYTE *dst = (BYTE *)rgdod + i * cbobj;
        ZeroMemory(dst, cbobj);
        CopyMemory(dst, src, cbobj < sizeof(KF_DIDOD) ? cbobj : sizeof(KF_DIDOD));
        i++; idx++;
    }
    if (!(flags & KF_DIGDD_PEEK)) { d->qhead = (d->qhead + idx) & 255; d->qcount -= idx; }
    *inout = i;
    maybe_log_stats();
    if (d->qoverflow) { d->qoverflow = 0; return KF_DI_BUFFEROVERFLOW; }
    return KF_DI_OK;
}

static void register_device(void *dev, int kind)
{
    void **vt; DEVVT *v; DEVICE *d;
    if (!dev) return;
    if (!(d = find_dev(dev)))
    {
        if (g_ndevs >= 8) { logf("too many devices, %08lx not wrapped", (unsigned long)(ULONG_PTR)dev); return; }
        d = &g_devs[g_ndevs++]; ZeroMemory(d, sizeof *d); d->dev = dev; d->kind = kind;
    }
    vt = *(void ***)dev;
    if (find_devvt(vt)) return;
    if (g_ndevvt >= 4) { logf("too many device vtables, %08lx not patched", (unsigned long)(ULONG_PTR)vt); return; }
    v = &g_devvt[g_ndevvt++];
    v->vt = vt; v->acq = (PFN_Acquire)vt[VT_DEV_Acquire]; v->gds = (PFN_GetDeviceState)vt[VT_DEV_GetDeviceState];
    v->gdd = (PFN_GetDeviceData)vt[VT_DEV_GetDeviceData]; v->sdf = (PFN_SetDataFormat)vt[VT_DEV_SetDataFormat]; v->coop = (PFN_SetCoop)vt[VT_DEV_SetCoop];
    patch_ptr(&vt[VT_DEV_Acquire], (void *)hk_Acquire);
    patch_ptr(&vt[VT_DEV_GetDeviceState], (void *)hk_GetDeviceState);
    patch_ptr(&vt[VT_DEV_GetDeviceData], (void *)hk_GetDeviceData);
    patch_ptr(&vt[VT_DEV_SetDataFormat], (void *)hk_SetDataFormat);
    patch_ptr(&vt[VT_DEV_SetCoop], (void *)hk_SetCoop);
    logf("device vtable %08lx patched (Acquire/GetDeviceState/GetDeviceData/SetDataFormat/SetCooperativeLevel)", (unsigned long)(ULONG_PTR)vt);
}

static HRESULT WINAPI hk_CreateDevice(void *self, const GUID *g, void **out, void *outer)
{
    DIVT *v = find_divt(*(void ***)self); HRESULT hr;
    if (!v) return E_FAIL;
    hr = v->cd(self, g, out, outer);
    logf("CreateDevice(%s) -> %08lx dev=%08lx", guidstr(g), (unsigned long)hr, (unsigned long)(ULONG_PTR)(out && SUCCEEDED(hr) ? *out : NULL));
    if (SUCCEEDED(hr) && out && *out)
    {
        if (IsEqualGUID(g, &KF_GUID_SysKeyboard)) register_device(*out, KIND_KBD);
        else if (IsEqualGUID(g, &KF_GUID_SysMouse)) register_device(*out, KIND_MOUSE);
    }
    return hr;
}

static void wrap_dinput(void *obj)
{
    void **vt = *(void ***)obj; DIVT *v;
    if (find_divt(vt)) return;
    if (g_ndivt >= 4) return;
    v = &g_divt[g_ndivt++]; v->vt = vt; v->cd = (PFN_CreateDevice)vt[VT_DI_CreateDevice];
    patch_ptr(&vt[VT_DI_CreateDevice], (void *)hk_CreateDevice);
    logf("IDirectInput8 vtable %08lx: CreateDevice patched", (unsigned long)(ULONG_PTR)vt);
}

/* ---- exports ---- */
static int load_real(void)
{
    char path[MAX_PATH]; UINT n;
    if (g_real) return 1;
    n = GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n >= MAX_PATH - 12) return 0;
    lstrcpyA(path + n, "\\dinput8.dll");
    g_real = LoadLibraryA(path);
    logf("real dinput8: %s -> %08lx", path, (unsigned long)(ULONG_PTR)g_real);
    if (!g_real) return 0;
    real_DirectInput8Create   = (PFN_DI8Create)GetProcAddress(g_real, "DirectInput8Create");
    real_DllCanUnloadNow      = (PFN_Generic)GetProcAddress(g_real, "DllCanUnloadNow");
    real_DllGetClassObject    = (PFN_Generic)GetProcAddress(g_real, "DllGetClassObject");
    real_DllRegisterServer    = (PFN_Generic)GetProcAddress(g_real, "DllRegisterServer");
    real_DllUnregisterServer  = (PFN_Generic)GetProcAddress(g_real, "DllUnregisterServer");
    return real_DirectInput8Create != NULL;
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD ver, const GUID *iid, void **out, void *outer)
{
    HRESULT hr;
    if (!load_real()) return E_FAIL;
    hr = real_DirectInput8Create(h, ver, iid, out, outer);
    logf("DirectInput8Create(ver=%04lx, %s) -> %08lx obj=%08lx", (unsigned long)ver, guidstr(iid), (unsigned long)hr, (unsigned long)(ULONG_PTR)(out && SUCCEEDED(hr) ? *out : NULL));
    if (SUCCEEDED(hr) && out && *out) { wrap_dinput(*out); ensure_hooks(); }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return load_real() && real_DllCanUnloadNow ? real_DllCanUnloadNow() : S_FALSE; }
__declspec(dllexport) HRESULT WINAPI DllGetClassObject(const GUID *clsid, const GUID *iid, void **out)
{
    typedef HRESULT (WINAPI *PFN)(const GUID *, const GUID *, void **);
    if (!load_real() || !real_DllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
    return ((PFN)real_DllGetClassObject)(clsid, iid, out);
}
__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) { return load_real() && real_DllRegisterServer ? real_DllRegisterServer() : E_FAIL; }
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) { return load_real() && real_DllUnregisterServer ? real_DllUnregisterServer() : E_FAIL; }

static void open_log(void)
{
    char path[MAX_PATH]; DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    lstrcpyA(path + n, "dinput8-bridge.log");
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = inst; DisableThreadLibraryCalls(inst); open_log();
        logf("dinput8 bridge loaded; exe=%08lx pid=%lu tid=%lu", (unsigned long)(ULONG_PTR)GetModuleHandleA(NULL), (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_msghook) UnhookWindowsHookEx(g_msghook);
        if (g_log != INVALID_HANDLE_VALUE) { logf("unloading"); CloseHandle(g_log); }
    }
    return TRUE;
}

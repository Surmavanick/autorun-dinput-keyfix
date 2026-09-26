/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Surmavanick
 * https://github.com/Surmavanick/autorun-dinput-keyfix
 *
 * D3D8Fps.asi - on-screen FPS counter and frame-rate limiter for Direct3D 8
 * games, loaded by the Ultimate ASI Loader. Hooks Direct3DCreate8 through the
 * executable's import table, wraps IDirect3D8::CreateDevice and patches the
 * device vtable so that every Present() first draws the current frame rate
 * (7-segment digits, top left) with DrawPrimitiveUP and then paces the frame
 * to the configured limit. No D3DX, no fonts, no dependencies beyond
 * kernel32/user32/winmm. Written for Autorun (wine-nx, Nintendo Switch) where
 * WineD3D has neither a HUD nor a frame limiter, but it works anywhere.
 *
 * Configuration: D3D8Fps.ini next to the plugin
 *   [D3D8Fps]
 *   Limit=30        ; frames per second, 0 = no limit
 *   ShowCounter=1   ; 0 hides the digits (the limit still applies)
 *   NoVSync=1       ; with a limit, present immediately instead of waiting for vblank
 *
 * Log: D3D8Fps.log next to the plugin.
 *
 * Build: zig cc -target x86-windows-gnu -shared -O2 -o D3D8Fps.asi d3d8fps.c -luser32 -lkernel32 -lwinmm
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

/* IDirect3D8 vtable */
#define VT_D3D_CreateDevice 15
/* IDirect3DDevice8 vtable */
#define VT_DEV_Reset            14
#define VT_DEV_Present          15
#define VT_DEV_BeginScene       34
#define VT_DEV_EndScene         35
#define VT_DEV_SetRenderState   50
#define VT_DEV_BeginStateBlock  52
#define VT_DEV_EndStateBlock    53
#define VT_DEV_ApplyStateBlock  54
#define VT_DEV_CaptureStateBlock 55
#define VT_DEV_DeleteStateBlock 56
#define VT_DEV_SetTexture       61
#define VT_DEV_SetTextureStageState 63
#define VT_DEV_DrawPrimitiveUP  72
#define VT_DEV_SetVertexShader  76
#define VT_DEV_SetPixelShader   88

/* D3D8 constants */
#define D3DRS_ZENABLE 7
#define D3DRS_FILLMODE 8
#define D3DRS_ZWRITEENABLE 14
#define D3DRS_ALPHATESTENABLE 15
#define D3DRS_CULLMODE 22
#define D3DRS_ALPHABLENDENABLE 27
#define D3DRS_FOGENABLE 28
#define D3DRS_STENCILENABLE 52
#define D3DRS_CLIPPING 136
#define D3DRS_LIGHTING 137
#define D3DRS_COLORWRITEENABLE 168
#define D3DTSS_COLOROP 1
#define D3DTSS_COLORARG1 2
#define D3DTSS_ALPHAOP 4
#define D3DTOP_DISABLE 1
#define D3DTOP_SELECTARG1 2
#define D3DTA_DIFFUSE 0
#define D3DCULL_NONE 1
#define D3DFILL_SOLID 3
#define D3DFVF_XYZRHW_DIFFUSE 0x44
#define D3DPT_TRIANGLELIST 4

typedef void *(WINAPI *PFN_D3DCreate8)(UINT);
typedef HRESULT (WINAPI *PFN_CreateDevice)(void *, UINT, DWORD, HWND, DWORD, void *, void **);
typedef HRESULT (WINAPI *PFN_Present)(void *, const RECT *, const RECT *, HWND, const void *);
typedef HRESULT (WINAPI *PFN_Reset)(void *, void *);
typedef HRESULT (WINAPI *PFN_Void)(void *);
typedef HRESULT (WINAPI *PFN_SetRS)(void *, DWORD, DWORD);
typedef HRESULT (WINAPI *PFN_SetTSS)(void *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *PFN_SetTex)(void *, DWORD, void *);
typedef HRESULT (WINAPI *PFN_SetShader)(void *, DWORD);
typedef HRESULT (WINAPI *PFN_DrawUP)(void *, DWORD, UINT, const void *, UINT);
typedef HRESULT (WINAPI *PFN_EndSB)(void *, DWORD *);
typedef HRESULT (WINAPI *PFN_SB)(void *, DWORD);

typedef struct { float x, y, z, rhw; DWORD color; } VERT;

static HMODULE g_self; static HANDLE g_log = INVALID_HANDLE_VALUE;
static PFN_D3DCreate8 orig_create8; static PFN_CreateDevice orig_cd; static PFN_Present orig_present; static PFN_Reset orig_reset;
static void **g_d3d_vt, **g_dev_vt;
static DWORD g_sb; static int g_sb_broken;
static DWORD g_frames, g_last_tick, g_fps; static int g_first_present;
static UINT g_bb_w, g_bb_h;
static VERT g_verts[1024]; static UINT g_nverts;
/* configuration */
static int g_limit, g_show = 1, g_novsync = 1;
static DWORD g_resyncs, g_stat_tick, g_stat_frames, g_stat_wait;
/* limiter */
static LARGE_INTEGER g_qpf; static LONGLONG g_next; static DWORD g_waited_ms, g_limit_frames;

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

/* ---- frame limiter ---- */
static void limit_wait(void)
{
    LARGE_INTEGER now; LONGLONG interval, remaining; DWORD t0 = 0;
    if (g_limit <= 0 || !g_qpf.QuadPart) return;
    interval = g_qpf.QuadPart / g_limit;
    QueryPerformanceCounter(&now);
    /* first frame, or the game fell more than a frame behind: re-sync instead of catching up */
    if (!g_next || now.QuadPart > g_next + interval) { if (g_next) g_resyncs++; g_next = now.QuadPart; }
    if (now.QuadPart < g_next) t0 = GetTickCount();
    while (now.QuadPart < g_next)
    {
        remaining = g_next - now.QuadPart;
        if (remaining > g_qpf.QuadPart / 400) Sleep(1);      /* more than 2.5 ms left */
        else if (remaining > g_qpf.QuadPart / 4000) Sleep(0); /* more than 0.25 ms left */
        QueryPerformanceCounter(&now);
    }
    if (t0) { DWORD w = GetTickCount() - t0; g_waited_ms += w; g_stat_wait += w; }
    g_next += interval;
    g_limit_frames++;
}

/* ---- drawing ---- */
static void add_rect(float x0, float y0, float x1, float y1, DWORD c)
{
    VERT *v; int i;
    if (g_nverts + 6 > 1024) return;
    v = &g_verts[g_nverts];
    v[0].x = x0; v[0].y = y0; v[1].x = x1; v[1].y = y0; v[2].x = x0; v[2].y = y1;
    v[3].x = x1; v[3].y = y0; v[4].x = x1; v[4].y = y1; v[5].x = x0; v[5].y = y1;
    for (i = 0; i < 6; i++) { v[i].x -= 0.5f; v[i].y -= 0.5f; v[i].z = 0.0f; v[i].rhw = 1.0f; v[i].color = c; }
    g_nverts += 6;
}

/* 7-segment: bit0 top, bit1 top-right, bit2 bottom-right, bit3 bottom, bit4 bottom-left, bit5 top-left, bit6 middle */
static const BYTE seg7[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void add_digit(float x, float y, float w, float h, float t, int d, DWORD c)
{
    BYTE s = seg7[d % 10];
    float mid = y + h / 2;
    if (s & 0x01) add_rect(x + t, y, x + w - t, y + t, c);
    if (s & 0x02) add_rect(x + w - t, y + t, x + w, mid - t / 2, c);
    if (s & 0x04) add_rect(x + w - t, mid + t / 2, x + w, y + h - t, c);
    if (s & 0x08) add_rect(x + t, y + h - t, x + w - t, y + h, c);
    if (s & 0x10) add_rect(x, mid + t / 2, x + t, y + h - t, c);
    if (s & 0x20) add_rect(x, y + t, x + t, mid - t / 2, c);
    if (s & 0x40) add_rect(x + t, mid - t / 2, x + w - t, mid + t / 2, c);
}

static void build_overlay(DWORD fps)
{
    const float X = 10, Y = 10, W = 13, H = 22, T = 3, GAP = 5;
    int digits[4], n = 0, i; DWORD v = fps; float x;
    g_nverts = 0;
    if (!g_show) return;
    if (v > 9999) v = 9999;
    do { digits[n++] = (int)(v % 10); v /= 10; } while (v && n < 4);
    add_rect(X - 5, Y - 5, X + n * (W + GAP) - GAP + 5, Y + H + 5, 0xC0000000);
    x = X;
    for (i = n - 1; i >= 0; i--) { add_digit(x, Y, W, H, T, digits[i], 0xFFFFFF00); x += W + GAP; }
}

/* the device states the overlay touches; also used to record the state block */
static void set_overlay_states(void *dev)
{
    void **vt = g_dev_vt;
    ((PFN_SetShader)vt[VT_DEV_SetPixelShader])(dev, 0);
    ((PFN_SetShader)vt[VT_DEV_SetVertexShader])(dev, D3DFVF_XYZRHW_DIFFUSE);
    ((PFN_SetTex)vt[VT_DEV_SetTexture])(dev, 0, NULL);
    ((PFN_SetTSS)vt[VT_DEV_SetTextureStageState])(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    ((PFN_SetTSS)vt[VT_DEV_SetTextureStageState])(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    ((PFN_SetTSS)vt[VT_DEV_SetTextureStageState])(dev, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    ((PFN_SetTSS)vt[VT_DEV_SetTextureStageState])(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_ZENABLE, FALSE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_ZWRITEENABLE, FALSE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_ALPHATESTENABLE, FALSE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_FOGENABLE, FALSE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_STENCILENABLE, FALSE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_LIGHTING, FALSE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_CLIPPING, FALSE);
    ((PFN_SetRS)vt[VT_DEV_SetRenderState])(dev, D3DRS_COLORWRITEENABLE, 0x0F);
}

static void draw_overlay(void *dev)
{
    void **vt = g_dev_vt; HRESULT hr; int have_sb = 0;
    if (!g_nverts) return;
    if (!g_sb && !g_sb_broken)
    {
        /* record a state block that holds exactly the states set_overlay_states touches */
        if (SUCCEEDED(((PFN_Void)vt[VT_DEV_BeginStateBlock])(dev)))
        {
            set_overlay_states(dev);
            hr = ((PFN_EndSB)vt[VT_DEV_EndStateBlock])(dev, &g_sb);
            if (FAILED(hr) || !g_sb) { g_sb = 0; g_sb_broken = 1; logf("EndStateBlock failed %08lx; drawing without state save/restore", (unsigned long)hr); }
            else logf("state block %lu recorded", (unsigned long)g_sb);
        }
        else { g_sb_broken = 1; logf("BeginStateBlock failed; drawing without state save/restore"); }
    }
    if (g_sb) { hr = ((PFN_SB)vt[VT_DEV_CaptureStateBlock])(dev, g_sb); have_sb = SUCCEEDED(hr); }
    if (FAILED(((PFN_Void)vt[VT_DEV_BeginScene])(dev))) return;
    set_overlay_states(dev);
    ((PFN_DrawUP)vt[VT_DEV_DrawPrimitiveUP])(dev, D3DPT_TRIANGLELIST, g_nverts / 3, g_verts, sizeof(VERT));
    ((PFN_Void)vt[VT_DEV_EndScene])(dev);
    if (have_sb) ((PFN_SB)vt[VT_DEV_ApplyStateBlock])(dev, g_sb);
}

/* D3DPRESENT_PARAMETERS8: FullScreen_PresentationInterval is the 13th DWORD */
static void force_immediate(void *pp, const char *where)
{
    DWORD *p = (DWORD *)pp;
    if (!pp || !g_limit || !g_novsync) return;
    logf("%s: presentation interval %08lx -> IMMEDIATE (limiter active)", where, (unsigned long)p[12]);
    p[12] = 0x80000000; /* D3DPRESENT_INTERVAL_IMMEDIATE */
}

/* ---- device hooks ---- */
static HRESULT WINAPI hk_Present(void *dev, const RECT *src, const RECT *dst, HWND wnd, const void *dirty)
{
    DWORD now = GetTickCount();
    g_frames++;
    if (!g_last_tick) g_last_tick = now;
    if (now - g_last_tick >= 500)
    {
        g_fps = (g_frames * 1000 + (now - g_last_tick) / 2) / (now - g_last_tick);
        g_frames = 0; g_last_tick = now;
        build_overlay(g_fps);
    }
    if (!g_first_present)
    {
        g_first_present = 1; build_overlay(0);
        logf("first Present on device %08lx (limit %d fps, counter %s)", (unsigned long)(ULONG_PTR)dev, g_limit, g_show ? "on" : "off");
    }
    draw_overlay(dev);
    limit_wait();
    g_stat_frames++;
    if (!g_stat_tick) g_stat_tick = now;
    if (now - g_stat_tick >= 5000)
    {
        logf("5s: %lu frames (%lu fps shown), waited %lu ms, resyncs %lu", (unsigned long)g_stat_frames, (unsigned long)g_fps, (unsigned long)g_stat_wait, (unsigned long)g_resyncs);
        g_stat_frames = 0; g_stat_wait = 0; g_resyncs = 0; g_stat_tick = now;
    }
    return orig_present(dev, src, dst, wnd, dirty);
}

static HRESULT WINAPI hk_Reset(void *dev, void *pp)
{
    HRESULT hr;
    if (g_sb) { ((PFN_SB)g_dev_vt[VT_DEV_DeleteStateBlock])(dev, g_sb); g_sb = 0; }
    force_immediate(pp, "Reset");
    hr = orig_reset(dev, pp);
    if (pp) { g_bb_w = ((UINT *)pp)[0]; g_bb_h = ((UINT *)pp)[1]; }
    g_next = 0;
    logf("Reset -> %08lx (backbuffer %ux%u)", (unsigned long)hr, g_bb_w, g_bb_h);
    return hr;
}

static HRESULT WINAPI hk_CreateDevice(void *d3d, UINT adapter, DWORD type, HWND focus, DWORD flags, void *pp, void **out)
{
    HRESULT hr;
    force_immediate(pp, "CreateDevice");
    hr = orig_cd(d3d, adapter, type, focus, flags, pp, out);
    logf("CreateDevice(adapter %u, type %lu, flags %lx) -> %08lx dev=%08lx", adapter, (unsigned long)type, (unsigned long)flags, (unsigned long)hr, (unsigned long)(ULONG_PTR)(out && SUCCEEDED(hr) ? *out : NULL));
    if (SUCCEEDED(hr) && out && *out)
    {
        void **vt = *(void ***)*out;
        if (pp) { g_bb_w = ((UINT *)pp)[0]; g_bb_h = ((UINT *)pp)[1]; logf("backbuffer %ux%u", g_bb_w, g_bb_h); }
        g_sb = 0; g_sb_broken = 0; g_first_present = 0; g_next = 0;
        if (g_dev_vt != vt)
        {
            g_dev_vt = vt;
            orig_present = (PFN_Present)vt[VT_DEV_Present]; orig_reset = (PFN_Reset)vt[VT_DEV_Reset];
            patch_ptr(&vt[VT_DEV_Present], (void *)hk_Present);
            patch_ptr(&vt[VT_DEV_Reset], (void *)hk_Reset);
            logf("device vtable %08lx patched (Present, Reset)", (unsigned long)(ULONG_PTR)vt);
        }
    }
    return hr;
}

static void *WINAPI hk_Direct3DCreate8(UINT sdk)
{
    void *d3d = orig_create8(sdk);
    logf("Direct3DCreate8(%u) -> %08lx", sdk, (unsigned long)(ULONG_PTR)d3d);
    if (d3d)
    {
        void **vt = *(void ***)d3d;
        if (g_d3d_vt != vt)
        {
            g_d3d_vt = vt; orig_cd = (PFN_CreateDevice)vt[VT_D3D_CreateDevice];
            patch_ptr(&vt[VT_D3D_CreateDevice], (void *)hk_CreateDevice);
            logf("IDirect3D8 vtable %08lx: CreateDevice patched", (unsigned long)(ULONG_PTR)vt);
        }
    }
    return d3d;
}

/* ---- IAT patch ---- */
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
        void **slot; HMODULE exe = GetModuleHandleA(NULL); char path[MAX_PATH];
        g_self = inst; DisableThreadLibraryCalls(inst);
        plugin_path(path, "D3D8Fps.log");
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        plugin_path(path, "D3D8Fps.ini");
        g_limit = GetPrivateProfileIntA("D3D8Fps", "Limit", 0, path);
        g_show = GetPrivateProfileIntA("D3D8Fps", "ShowCounter", 1, path);
        g_novsync = GetPrivateProfileIntA("D3D8Fps", "NoVSync", 1, path);
        if (g_limit < 0 || g_limit > 1000) g_limit = 0;
        QueryPerformanceFrequency(&g_qpf);
        if (g_limit) timeBeginPeriod(1);
        logf("D3D8Fps loaded; exe=%08lx ini=%s limit=%d counter=%d novsync=%d qpf=%lu", (unsigned long)(ULONG_PTR)exe, path, g_limit, g_show, g_novsync, (unsigned long)g_qpf.QuadPart);
        slot = find_iat_entry(exe, "d3d8.dll", "Direct3DCreate8");
        if (slot) { orig_create8 = (PFN_D3DCreate8)*slot; patch_ptr(slot, (void *)hk_Direct3DCreate8); logf("IAT d3d8.dll!Direct3DCreate8 hooked (was %08lx)", (unsigned long)(ULONG_PTR)orig_create8); }
        else logf("IAT entry for d3d8.dll!Direct3DCreate8 NOT found - plugin inactive");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_limit) timeEndPeriod(1);
        if (g_log != INVALID_HANDLE_VALUE) { logf("unloading (last fps %lu, limited frames %lu, waited %lu ms)", (unsigned long)g_fps, (unsigned long)g_limit_frames, (unsigned long)g_waited_ms); CloseHandle(g_log); }
    }
    return TRUE;
}

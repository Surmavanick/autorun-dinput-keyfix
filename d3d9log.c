/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Surmavanick
 *
 * d3d9log.dll - a Direct3D 9 logging proxy. Rename it to d3d9.dll and put it next to a game:
 * it loads the system d3d9.dll, patches the IDirect3D9 and IDirect3DDevice9 vtables in place
 * and writes d3d9log.txt next to itself: adapter/caps, CreateDevice parameters, every failed
 * call (first 30 per method, with arguments) and a per-frame summary every 120 frames
 * (draw calls, primitives, render-target switches, clears, shader/texture sets).
 * Made to find out what a game does on Autorun (wine-nx) when its scene stays black.
 *
 * Build: zig cc -target x86-windows-gnu -shared -O2 -o d3d9log.dll d3d9log.c d3d9log.def -lkernel32 -luser32
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>

static HMODULE g_self, g_real; static HANDLE g_log = INVALID_HANDLE_VALUE;
static void logf(const char *fmt, ...)
{
    char buf[1024]; int n; DWORD w; va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = wsprintfA(buf, "[%08lu] ", (unsigned long)GetTickCount());
    va_start(ap, fmt); n += wvsprintfA(buf + n, fmt, ap); va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n'; WriteFile(g_log, buf, n, &w, NULL); FlushFileBuffers(g_log);
}
static const char *fmtname(D3DFORMAT f)
{
    static char b[16];
    switch ((int)f) {
    case 0: return "UNKNOWN"; case 20: return "R8G8B8"; case 21: return "A8R8G8B8"; case 22: return "X8R8G8B8"; case 23: return "R5G6B5";
    case 24: return "X1R5G5B5"; case 25: return "A1R5G5B5"; case 26: return "A4R4G4B4"; case 28: return "A8"; case 32: return "A2B10G10R10";
    case 41: return "P8"; case 50: return "L8"; case 51: return "A8L8"; case 60: return "V8U8"; case 63: return "Q8W8V8U8"; case 64: return "V16U16";
    case 70: return "D16_LOCKABLE"; case 71: return "D32"; case 73: return "D15S1"; case 75: return "D24S8"; case 77: return "D24X8"; case 79: return "D24X4S4"; case 80: return "D16";
    case 81: return "L16"; case 111: return "R16F"; case 112: return "G16R16F"; case 113: return "A16B16G16R16F"; case 114: return "R32F"; case 115: return "G32R32F"; case 116: return "A32B32G32R32F";
    case 34: return "G16R16"; case 36: return "A16B16G16R16"; case 61: return "L6V5U5"; case 62: return "X8L8V8U8";
    }
    if (f > 0x20202020) { b[0] = f & 255; b[1] = (f >> 8) & 255; b[2] = (f >> 16) & 255; b[3] = (f >> 24) & 255; b[4] = 0; return b; }
    wsprintfA(b, "fmt%d", (int)f); return b;
}
static void surfdesc(IDirect3DSurface9 *s, char *out)
{
    D3DSURFACE_DESC d;
    if (!s) { lstrcpyA(out, "NULL"); return; }
    if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d))) wsprintfA(out, "%p %ux%u %s usage=%lx pool=%d ms=%d", s, d.Width, d.Height, fmtname(d.Format), (unsigned long)d.Usage, d.Pool, d.MultiSampleType);
    else wsprintfA(out, "%p (GetDesc failed)", s);
}

/* ---- counters ---- */
typedef struct { const char *name; unsigned long calls, fails, logged; } STAT;
enum { S_CreateTexture, S_CreateCubeTexture, S_CreateVolumeTexture, S_CreateVertexBuffer, S_CreateIndexBuffer, S_CreateRenderTarget, S_CreateDepthStencilSurface,
       S_UpdateTexture, S_GetRenderTargetData, S_StretchRect, S_ColorFill, S_CreateOffscreenPlainSurface, S_SetRenderTarget, S_SetDepthStencilSurface,
       S_BeginScene, S_EndScene, S_Clear, S_SetTexture, S_DrawPrimitive, S_DrawIndexedPrimitive, S_DrawPrimitiveUP, S_DrawIndexedPrimitiveUP,
       S_CreateVertexDeclaration, S_SetVertexDeclaration, S_SetFVF, S_CreateVertexShader, S_SetVertexShader, S_CreatePixelShader, S_SetPixelShader,
       S_SetStreamSource, S_SetIndices, S_CreateQuery, S_CreateStateBlock, S_BeginStateBlock, S_EndStateBlock, S_ValidateDevice, S_Reset, S_TestCooperativeLevel,
       S_Present, S_SetRenderState, S_SetTextureStageState, S_SetSamplerState, S_SetTransform, S_SetLight, S_LightEnable, S_SetMaterial, S_SetViewport, S_SetScissorRect,
       S_SetVertexShaderConstantF, S_SetPixelShaderConstantF, S_UpdateSurface, S_COUNT };
static STAT st[S_COUNT] = {
    {"CreateTexture"},{"CreateCubeTexture"},{"CreateVolumeTexture"},{"CreateVertexBuffer"},{"CreateIndexBuffer"},{"CreateRenderTarget"},{"CreateDepthStencilSurface"},
    {"UpdateTexture"},{"GetRenderTargetData"},{"StretchRect"},{"ColorFill"},{"CreateOffscreenPlainSurface"},{"SetRenderTarget"},{"SetDepthStencilSurface"},
    {"BeginScene"},{"EndScene"},{"Clear"},{"SetTexture"},{"DrawPrimitive"},{"DrawIndexedPrimitive"},{"DrawPrimitiveUP"},{"DrawIndexedPrimitiveUP"},
    {"CreateVertexDeclaration"},{"SetVertexDeclaration"},{"SetFVF"},{"CreateVertexShader"},{"SetVertexShader"},{"CreatePixelShader"},{"SetPixelShader"},
    {"SetStreamSource"},{"SetIndices"},{"CreateQuery"},{"CreateStateBlock"},{"BeginStateBlock"},{"EndStateBlock"},{"ValidateDevice"},{"Reset"},{"TestCooperativeLevel"},
    {"Present"},{"SetRenderState"},{"SetTextureStageState"},{"SetSamplerState"},{"SetTransform"},{"SetLight"},{"LightEnable"},{"SetMaterial"},{"SetViewport"},{"SetScissorRect"},
    {"SetVertexShaderConstantF"},{"SetPixelShaderConstantF"},{"UpdateSurface"} };
static unsigned long f_draws, f_prims, f_rt_switch, f_rt_nonback, f_clears, f_vs_set, f_ps_set, f_vs_null, f_ps_null, f_tex_set, f_tex_null, f_frames;
static int g_rt_is_backbuffer = 1; static IDirect3DSurface9 *g_backbuffer;
#define TRACK(i, hr) do { st[i].calls++; if (FAILED(hr)) st[i].fails++; } while (0)
#define SHOULDLOG(i, hr) (FAILED(hr) && st[i].logged++ < 30)

static IDirect3DDevice9Vtbl orig; /* copy of the original device vtable */
static IDirect3D9Vtbl orig9;
static int g_dump_this_frame; static unsigned long g_draws_this_frame; static int g_nomsaa;
static void dump_state(IDirect3DDevice9 *d, const char *what, UINT count);

/* ---- device hooks ---- */
static HRESULT WINAPI h_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *sh)
{ HRESULT hr = orig.CreateTexture(d, w, h, lv, usage, f, pool, out, sh); TRACK(S_CreateTexture, hr); if (SHOULDLOG(S_CreateTexture, hr)) logf("FAIL CreateTexture %ux%u lv=%u usage=%lx %s pool=%d -> %08lx", w, h, lv, (unsigned long)usage, fmtname(f), pool, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateCubeTexture(IDirect3DDevice9 *d, UINT e, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *sh)
{ HRESULT hr = orig.CreateCubeTexture(d, e, lv, usage, f, pool, out, sh); TRACK(S_CreateCubeTexture, hr); if (SHOULDLOG(S_CreateCubeTexture, hr)) logf("FAIL CreateCubeTexture %u lv=%u usage=%lx %s pool=%d -> %08lx", e, lv, (unsigned long)usage, fmtname(f), pool, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateVolumeTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT dp, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *sh)
{ HRESULT hr = orig.CreateVolumeTexture(d, w, h, dp, lv, usage, f, pool, out, sh); TRACK(S_CreateVolumeTexture, hr); if (SHOULDLOG(S_CreateVolumeTexture, hr)) logf("FAIL CreateVolumeTexture %ux%ux%u %s pool=%d -> %08lx", w, h, dp, fmtname(f), pool, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateVertexBuffer(IDirect3DDevice9 *d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *sh)
{ HRESULT hr = orig.CreateVertexBuffer(d, len, usage, fvf, pool, out, sh); TRACK(S_CreateVertexBuffer, hr); if (SHOULDLOG(S_CreateVertexBuffer, hr)) logf("FAIL CreateVertexBuffer len=%u usage=%lx fvf=%lx pool=%d -> %08lx", len, (unsigned long)usage, (unsigned long)fvf, pool, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateIndexBuffer(IDirect3DDevice9 *d, UINT len, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *sh)
{ HRESULT hr = orig.CreateIndexBuffer(d, len, usage, f, pool, out, sh); TRACK(S_CreateIndexBuffer, hr); if (SHOULDLOG(S_CreateIndexBuffer, hr)) logf("FAIL CreateIndexBuffer len=%u usage=%lx %s pool=%d -> %08lx", len, (unsigned long)usage, fmtname(f), pool, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateRenderTarget(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lock, IDirect3DSurface9 **out, HANDLE *sh)
{ HRESULT hr = orig.CreateRenderTarget(d, w, h, f, ms, q, lock, out, sh); TRACK(S_CreateRenderTarget, hr); if (st[S_CreateRenderTarget].logged++ < 30) logf("%s CreateRenderTarget %ux%u %s ms=%d lockable=%d -> %08lx %p", FAILED(hr) ? "FAIL" : "ok", w, h, fmtname(f), ms, lock, (unsigned long)hr, out ? *out : NULL); return hr; }
static HRESULT WINAPI h_CreateDepthStencilSurface(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL disc, IDirect3DSurface9 **out, HANDLE *sh)
{ HRESULT hr = orig.CreateDepthStencilSurface(d, w, h, f, ms, q, disc, out, sh); TRACK(S_CreateDepthStencilSurface, hr); if (st[S_CreateDepthStencilSurface].logged++ < 30) logf("%s CreateDepthStencilSurface %ux%u %s ms=%d -> %08lx %p", FAILED(hr) ? "FAIL" : "ok", w, h, fmtname(f), ms, (unsigned long)hr, out ? *out : NULL); return hr; }
static HRESULT WINAPI h_UpdateTexture(IDirect3DDevice9 *d, IDirect3DBaseTexture9 *s, IDirect3DBaseTexture9 *t)
{ HRESULT hr = orig.UpdateTexture(d, s, t); TRACK(S_UpdateTexture, hr); if (SHOULDLOG(S_UpdateTexture, hr)) logf("FAIL UpdateTexture %p -> %p: %08lx", s, t, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_UpdateSurface(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *t, const POINT *pt)
{ HRESULT hr = orig.UpdateSurface(d, s, sr, t, pt); TRACK(S_UpdateSurface, hr); if (SHOULDLOG(S_UpdateSurface, hr)) { char a[96], b[96]; surfdesc(s, a); surfdesc(t, b); logf("FAIL UpdateSurface %s -> %s: %08lx", a, b, (unsigned long)hr); } return hr; }
static HRESULT WINAPI h_GetRenderTargetData(IDirect3DDevice9 *d, IDirect3DSurface9 *s, IDirect3DSurface9 *t)
{ HRESULT hr = orig.GetRenderTargetData(d, s, t); TRACK(S_GetRenderTargetData, hr); if (SHOULDLOG(S_GetRenderTargetData, hr)) logf("FAIL GetRenderTargetData -> %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_StretchRect(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *t, const RECT *dr, D3DTEXTUREFILTERTYPE flt)
{ HRESULT hr = orig.StretchRect(d, s, sr, t, dr, flt); TRACK(S_StretchRect, hr); if (st[S_StretchRect].logged++ < 30) { char a[96], b[96]; surfdesc(s, a); surfdesc(t, b); logf("%s StretchRect %s -> %s filter=%d: %08lx", FAILED(hr) ? "FAIL" : "ok", a, b, flt, (unsigned long)hr); } return hr; }
static HRESULT WINAPI h_ColorFill(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *r, D3DCOLOR c)
{ HRESULT hr = orig.ColorFill(d, s, r, c); TRACK(S_ColorFill, hr); if (SHOULDLOG(S_ColorFill, hr)) logf("FAIL ColorFill -> %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateOffscreenPlainSurface(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *sh)
{ HRESULT hr = orig.CreateOffscreenPlainSurface(d, w, h, f, pool, out, sh); TRACK(S_CreateOffscreenPlainSurface, hr); if (SHOULDLOG(S_CreateOffscreenPlainSurface, hr)) logf("FAIL CreateOffscreenPlainSurface %ux%u %s pool=%d -> %08lx", w, h, fmtname(f), pool, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetRenderTarget(IDirect3DDevice9 *d, DWORD idx, IDirect3DSurface9 *s)
{ HRESULT hr = orig.SetRenderTarget(d, idx, s); TRACK(S_SetRenderTarget, hr); f_rt_switch++; if (idx == 0) { g_rt_is_backbuffer = (s == g_backbuffer || s == NULL); if (!g_rt_is_backbuffer) f_rt_nonback++; }
  if (st[S_SetRenderTarget].logged++ < 40 || FAILED(hr)) { char a[96]; surfdesc(s, a); logf("%s SetRenderTarget %lu %s%s: %08lx", FAILED(hr) ? "FAIL" : "ok", (unsigned long)idx, a, s == g_backbuffer ? " (backbuffer)" : "", (unsigned long)hr); } return hr; }
static HRESULT WINAPI h_SetDepthStencilSurface(IDirect3DDevice9 *d, IDirect3DSurface9 *s)
{ HRESULT hr = orig.SetDepthStencilSurface(d, s); TRACK(S_SetDepthStencilSurface, hr); if (SHOULDLOG(S_SetDepthStencilSurface, hr)) { char a[96]; surfdesc(s, a); logf("FAIL SetDepthStencilSurface %s: %08lx", a, (unsigned long)hr); } return hr; }
static HRESULT WINAPI h_BeginScene(IDirect3DDevice9 *d) { HRESULT hr = orig.BeginScene(d); TRACK(S_BeginScene, hr); if (SHOULDLOG(S_BeginScene, hr)) logf("FAIL BeginScene %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_EndScene(IDirect3DDevice9 *d) { HRESULT hr = orig.EndScene(d); TRACK(S_EndScene, hr); if (SHOULDLOG(S_EndScene, hr)) logf("FAIL EndScene %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_Clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR c, float z, DWORD s)
{ HRESULT hr = orig.Clear(d, n, r, flags, c, z, s); TRACK(S_Clear, hr); f_clears++; if (SHOULDLOG(S_Clear, hr)) logf("FAIL Clear flags=%lx color=%08lx: %08lx", (unsigned long)flags, (unsigned long)c, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetTexture(IDirect3DDevice9 *d, DWORD stage, IDirect3DBaseTexture9 *t)
{ HRESULT hr = orig.SetTexture(d, stage, t); TRACK(S_SetTexture, hr); if (t) f_tex_set++; else f_tex_null++; if (SHOULDLOG(S_SetTexture, hr)) logf("FAIL SetTexture %lu %p: %08lx", (unsigned long)stage, t, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_DrawPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT sv, UINT pc)
{ HRESULT hr; if (g_dump_this_frame) dump_state(d, "DP", pc); hr = orig.DrawPrimitive(d, t, sv, pc); TRACK(S_DrawPrimitive, hr); f_draws++; f_prims += pc; if (SHOULDLOG(S_DrawPrimitive, hr)) logf("FAIL DrawPrimitive type=%d count=%u: %08lx", t, pc, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_DrawIndexedPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc)
{ HRESULT hr; if (g_dump_this_frame) dump_state(d, "DIP", pc); hr = orig.DrawIndexedPrimitive(d, t, bv, mi, nv, si, pc); TRACK(S_DrawIndexedPrimitive, hr); f_draws++; f_prims += pc; if (SHOULDLOG(S_DrawIndexedPrimitive, hr)) logf("FAIL DrawIndexedPrimitive type=%d nv=%u count=%u: %08lx", t, nv, pc, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_DrawPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT pc, const void *v, UINT stride)
{ HRESULT hr; if (g_dump_this_frame) { dump_state(d, "DPUP", pc); if (g_draws_this_frame <= 40 && v && stride >= 12) { const float *f = (const float *)v; logf("    v0=%d,%d,%d,%d stride=%u", (int)f[0], (int)f[1], (int)f[2], (int)f[3], stride); } } hr = orig.DrawPrimitiveUP(d, t, pc, v, stride); TRACK(S_DrawPrimitiveUP, hr); f_draws++; f_prims += pc; if (SHOULDLOG(S_DrawPrimitiveUP, hr)) logf("FAIL DrawPrimitiveUP type=%d count=%u: %08lx", t, pc, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_DrawIndexedPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void *idx, D3DFORMAT f, const void *v, UINT stride)
{ HRESULT hr; if (g_dump_this_frame) { dump_state(d, "DIPUP", pc); if (g_draws_this_frame <= 40 && v && stride >= 12) { const float *fv = (const float *)v; logf("    v0=%d,%d,%d,%d stride=%u", (int)fv[0], (int)fv[1], (int)fv[2], (int)fv[3], stride); } } hr = orig.DrawIndexedPrimitiveUP(d, t, mi, nv, pc, idx, f, v, stride); TRACK(S_DrawIndexedPrimitiveUP, hr); f_draws++; f_prims += pc; if (SHOULDLOG(S_DrawIndexedPrimitiveUP, hr)) logf("FAIL DrawIndexedPrimitiveUP count=%u: %08lx", pc, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateVertexDeclaration(IDirect3DDevice9 *d, const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **out)
{ HRESULT hr = orig.CreateVertexDeclaration(d, e, out); TRACK(S_CreateVertexDeclaration, hr); if (SHOULDLOG(S_CreateVertexDeclaration, hr)) logf("FAIL CreateVertexDeclaration %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetVertexDeclaration(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v) { HRESULT hr = orig.SetVertexDeclaration(d, v); TRACK(S_SetVertexDeclaration, hr); if (SHOULDLOG(S_SetVertexDeclaration, hr)) logf("FAIL SetVertexDeclaration %p %08lx", v, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetFVF(IDirect3DDevice9 *d, DWORD fvf) { HRESULT hr = orig.SetFVF(d, fvf); TRACK(S_SetFVF, hr); if (SHOULDLOG(S_SetFVF, hr)) logf("FAIL SetFVF %lx %08lx", (unsigned long)fvf, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateVertexShader(IDirect3DDevice9 *d, const DWORD *fn, IDirect3DVertexShader9 **out) { HRESULT hr = orig.CreateVertexShader(d, fn, out); TRACK(S_CreateVertexShader, hr); if (st[S_CreateVertexShader].logged++ < 30) logf("%s CreateVertexShader version=%08lx -> %08lx", FAILED(hr) ? "FAIL" : "ok", fn ? (unsigned long)fn[0] : 0UL, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetVertexShader(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) { HRESULT hr = orig.SetVertexShader(d, s); TRACK(S_SetVertexShader, hr); if (s) f_vs_set++; else f_vs_null++; if (SHOULDLOG(S_SetVertexShader, hr)) logf("FAIL SetVertexShader %p %08lx", s, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreatePixelShader(IDirect3DDevice9 *d, const DWORD *fn, IDirect3DPixelShader9 **out) { HRESULT hr = orig.CreatePixelShader(d, fn, out); TRACK(S_CreatePixelShader, hr); if (st[S_CreatePixelShader].logged++ < 30) logf("%s CreatePixelShader version=%08lx -> %08lx", FAILED(hr) ? "FAIL" : "ok", fn ? (unsigned long)fn[0] : 0UL, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetPixelShader(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s) { HRESULT hr = orig.SetPixelShader(d, s); TRACK(S_SetPixelShader, hr); if (s) f_ps_set++; else f_ps_null++; if (SHOULDLOG(S_SetPixelShader, hr)) logf("FAIL SetPixelShader %p %08lx", s, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetStreamSource(IDirect3DDevice9 *d, UINT n, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride) { HRESULT hr = orig.SetStreamSource(d, n, vb, off, stride); TRACK(S_SetStreamSource, hr); if (SHOULDLOG(S_SetStreamSource, hr)) logf("FAIL SetStreamSource %u %p off=%u stride=%u %08lx", n, vb, off, stride, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetIndices(IDirect3DDevice9 *d, IDirect3DIndexBuffer9 *ib) { HRESULT hr = orig.SetIndices(d, ib); TRACK(S_SetIndices, hr); if (SHOULDLOG(S_SetIndices, hr)) logf("FAIL SetIndices %p %08lx", ib, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateQuery(IDirect3DDevice9 *d, D3DQUERYTYPE t, IDirect3DQuery9 **out) { HRESULT hr = orig.CreateQuery(d, t, out); TRACK(S_CreateQuery, hr); if (st[S_CreateQuery].logged++ < 20) logf("%s CreateQuery type=%d -> %08lx", FAILED(hr) ? "FAIL" : "ok", t, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_CreateStateBlock(IDirect3DDevice9 *d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9 **out) { HRESULT hr = orig.CreateStateBlock(d, t, out); TRACK(S_CreateStateBlock, hr); if (SHOULDLOG(S_CreateStateBlock, hr)) logf("FAIL CreateStateBlock type=%d %08lx", t, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_BeginStateBlock(IDirect3DDevice9 *d) { HRESULT hr = orig.BeginStateBlock(d); TRACK(S_BeginStateBlock, hr); if (SHOULDLOG(S_BeginStateBlock, hr)) logf("FAIL BeginStateBlock %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_EndStateBlock(IDirect3DDevice9 *d, IDirect3DStateBlock9 **out) { HRESULT hr = orig.EndStateBlock(d, out); TRACK(S_EndStateBlock, hr); if (SHOULDLOG(S_EndStateBlock, hr)) logf("FAIL EndStateBlock %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_ValidateDevice(IDirect3DDevice9 *d, DWORD *passes) { HRESULT hr = orig.ValidateDevice(d, passes); TRACK(S_ValidateDevice, hr); if (st[S_ValidateDevice].logged++ < 10) logf("%s ValidateDevice passes=%lu -> %08lx", FAILED(hr) ? "FAIL" : "ok", passes ? (unsigned long)*passes : 0UL, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp) { HRESULT hr; if (g_nomsaa && pp->MultiSampleType) { logf("NoMSAA: Reset MultiSampleType %d -> 0", pp->MultiSampleType); pp->MultiSampleType = D3DMULTISAMPLE_NONE; pp->MultiSampleQuality = 0; } hr = orig.Reset(d, pp); TRACK(S_Reset, hr); logf("%s Reset %ux%u %s windowed=%d ms=%d autods=%d %s flags=%lx: %08lx", FAILED(hr) ? "FAIL" : "ok", pp->BackBufferWidth, pp->BackBufferHeight, fmtname(pp->BackBufferFormat), pp->Windowed, pp->MultiSampleType, pp->EnableAutoDepthStencil, fmtname(pp->AutoDepthStencilFormat), (unsigned long)pp->Flags, (unsigned long)hr); g_backbuffer = NULL; if (SUCCEEDED(hr)) orig.GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backbuffer); return hr; }
static HRESULT WINAPI h_TestCooperativeLevel(IDirect3DDevice9 *d) { HRESULT hr = orig.TestCooperativeLevel(d); TRACK(S_TestCooperativeLevel, hr); if (SHOULDLOG(S_TestCooperativeLevel, hr)) logf("TestCooperativeLevel -> %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) { HRESULT hr = orig.SetRenderState(d, s, v); TRACK(S_SetRenderState, hr); if (SHOULDLOG(S_SetRenderState, hr)) logf("FAIL SetRenderState %d=%lu %08lx", s, (unsigned long)v, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetTextureStageState(IDirect3DDevice9 *d, DWORD st_, D3DTEXTURESTAGESTATETYPE t, DWORD v) { HRESULT hr = orig.SetTextureStageState(d, st_, t, v); TRACK(S_SetTextureStageState, hr); if (SHOULDLOG(S_SetTextureStageState, hr)) logf("FAIL SetTextureStageState %lu %d=%lu %08lx", (unsigned long)st_, t, (unsigned long)v, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetSamplerState(IDirect3DDevice9 *d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) { HRESULT hr = orig.SetSamplerState(d, s, t, v); TRACK(S_SetSamplerState, hr); if (SHOULDLOG(S_SetSamplerState, hr)) logf("FAIL SetSamplerState %lu %d=%lu %08lx", (unsigned long)s, t, (unsigned long)v, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) { HRESULT hr = orig.SetTransform(d, t, m); TRACK(S_SetTransform, hr); if (SHOULDLOG(S_SetTransform, hr)) logf("FAIL SetTransform %d %08lx", t, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetLight(IDirect3DDevice9 *d, DWORD i, const D3DLIGHT9 *l) { HRESULT hr = orig.SetLight(d, i, l); TRACK(S_SetLight, hr); if (SHOULDLOG(S_SetLight, hr)) logf("FAIL SetLight %lu %08lx", (unsigned long)i, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_LightEnable(IDirect3DDevice9 *d, DWORD i, BOOL e) { HRESULT hr = orig.LightEnable(d, i, e); TRACK(S_LightEnable, hr); if (SHOULDLOG(S_LightEnable, hr)) logf("FAIL LightEnable %lu %08lx", (unsigned long)i, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetMaterial(IDirect3DDevice9 *d, const D3DMATERIAL9 *m) { HRESULT hr = orig.SetMaterial(d, m); TRACK(S_SetMaterial, hr); if (SHOULDLOG(S_SetMaterial, hr)) logf("FAIL SetMaterial %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) { HRESULT hr = orig.SetViewport(d, v); TRACK(S_SetViewport, hr); if (st[S_SetViewport].logged++ < 12 || FAILED(hr)) logf("%s SetViewport %lu,%lu %lux%lu z=%d..%d: %08lx", FAILED(hr) ? "FAIL" : "ok", (unsigned long)v->X, (unsigned long)v->Y, (unsigned long)v->Width, (unsigned long)v->Height, (int)(v->MinZ * 100), (int)(v->MaxZ * 100), (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetScissorRect(IDirect3DDevice9 *d, const RECT *r) { HRESULT hr = orig.SetScissorRect(d, r); TRACK(S_SetScissorRect, hr); if (SHOULDLOG(S_SetScissorRect, hr)) logf("FAIL SetScissorRect %08lx", (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetVertexShaderConstantF(IDirect3DDevice9 *d, UINT r, const float *c, UINT n) { HRESULT hr = orig.SetVertexShaderConstantF(d, r, c, n); TRACK(S_SetVertexShaderConstantF, hr); if (SHOULDLOG(S_SetVertexShaderConstantF, hr)) logf("FAIL SetVertexShaderConstantF reg=%u n=%u %08lx", r, n, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_SetPixelShaderConstantF(IDirect3DDevice9 *d, UINT r, const float *c, UINT n) { HRESULT hr = orig.SetPixelShaderConstantF(d, r, c, n); TRACK(S_SetPixelShaderConstantF, hr); if (SHOULDLOG(S_SetPixelShaderConstantF, hr)) logf("FAIL SetPixelShaderConstantF reg=%u n=%u %08lx", r, n, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_Present(IDirect3DDevice9 *d, const RECT *s, const RECT *t, HWND w, const RGNDATA *r)
{
    HRESULT hr = orig.Present(d, s, t, w, r); TRACK(S_Present, hr); f_frames++;
    g_dump_this_frame = (f_frames == 3 || f_frames % 600 == 300); g_draws_this_frame = 0; if (g_dump_this_frame) logf("--- state dump for frame %lu ---", f_frames + 1);
    if (FAILED(hr) && st[S_Present].logged++ < 30) logf("FAIL Present %08lx", (unsigned long)hr);
    if (f_frames % 120 == 1 || f_frames < 10)
    {
        int i; char line[512]; int n = 0;
        logf("frame %lu: draws=%lu prims=%lu clears=%lu rt_switch=%lu rt_offscreen=%lu vs_set=%lu/null=%lu ps_set=%lu/null=%lu tex_set=%lu/null=%lu", f_frames, f_draws, f_prims, f_clears, f_rt_switch, f_rt_nonback, f_vs_set, f_vs_null, f_ps_set, f_ps_null, f_tex_set, f_tex_null);
        for (i = 0; i < S_COUNT; i++) if (st[i].fails) n += wsprintfA(line + n, "%s:%lu/%lu ", st[i].name, st[i].fails, st[i].calls);
        if (n) logf("  failures so far: %s", line);
    }
    f_draws = f_prims = f_clears = f_rt_switch = f_rt_nonback = f_vs_set = f_ps_set = f_vs_null = f_ps_null = f_tex_set = f_tex_null = 0;
    return hr;
}


/* ---- per-draw state dump on selected frames (every 600th frame, first 40 draws) ---- */
static void dump_state(IDirect3DDevice9 *d, const char *what, UINT count)
{
    DWORD light = 0, amb = 0, ab = 0, sb = 0, db = 0, at = 0, ze = 0, cull = 0, cw = 0, fog = 0, tf = 0, cv = 0, zw = 0, shade = 0, aref = 0, ares = 0;
    DWORD cop = 0, ca1 = 0, ca2 = 0, aop = 0, aa1 = 0, cop1 = 0; DWORD fvf = 0; IDirect3DBaseTexture9 *tex = NULL; IDirect3DVertexShader9 *vs = NULL; IDirect3DPixelShader9 *ps = NULL;
    IDirect3DVertexDeclaration9 *decl = NULL; D3DVIEWPORT9 vp; D3DMATERIAL9 mat; D3DLIGHT9 l0; BOOL l0on = FALSE; char texs[80] = "none"; D3DMATRIX mw, mv, mp;
    if (g_draws_this_frame++ >= 40) return;
    orig.GetRenderState(d, D3DRS_LIGHTING, &light); orig.GetRenderState(d, D3DRS_AMBIENT, &amb); orig.GetRenderState(d, D3DRS_ALPHABLENDENABLE, &ab);
    orig.GetRenderState(d, D3DRS_SRCBLEND, &sb); orig.GetRenderState(d, D3DRS_DESTBLEND, &db); orig.GetRenderState(d, D3DRS_ALPHATESTENABLE, &at); orig.GetRenderState(d, D3DRS_ALPHAREF, &aref); orig.GetRenderState(d, D3DRS_ALPHAFUNC, &ares);
    orig.GetRenderState(d, D3DRS_ZENABLE, &ze); orig.GetRenderState(d, D3DRS_ZWRITEENABLE, &zw); orig.GetRenderState(d, D3DRS_CULLMODE, &cull); orig.GetRenderState(d, D3DRS_COLORWRITEENABLE, &cw);
    orig.GetRenderState(d, D3DRS_FOGENABLE, &fog); orig.GetRenderState(d, D3DRS_TEXTUREFACTOR, &tf); orig.GetRenderState(d, D3DRS_COLORVERTEX, &cv); orig.GetRenderState(d, D3DRS_SHADEMODE, &shade);
    orig.GetTextureStageState(d, 0, D3DTSS_COLOROP, &cop); orig.GetTextureStageState(d, 0, D3DTSS_COLORARG1, &ca1); orig.GetTextureStageState(d, 0, D3DTSS_COLORARG2, &ca2);
    orig.GetTextureStageState(d, 0, D3DTSS_ALPHAOP, &aop); orig.GetTextureStageState(d, 0, D3DTSS_ALPHAARG1, &aa1); orig.GetTextureStageState(d, 1, D3DTSS_COLOROP, &cop1);
    orig.GetFVF(d, &fvf); orig.GetVertexShader(d, &vs); orig.GetPixelShader(d, &ps); orig.GetVertexDeclaration(d, &decl); orig.GetTexture(d, 0, &tex);
    orig.GetViewport(d, &vp); orig.GetMaterial(d, &mat); orig.GetLightEnable(d, 0, &l0on); orig.GetLight(d, 0, &l0);
    orig.GetTransform(d, D3DTS_WORLD, &mw); orig.GetTransform(d, D3DTS_VIEW, &mv); orig.GetTransform(d, D3DTS_PROJECTION, &mp);
    if (tex) { D3DRESOURCETYPE rt = IDirect3DBaseTexture9_GetType(tex); if (rt == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC sd; if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)tex, 0, &sd))) wsprintfA(texs, "%p %ux%u %s pool=%d usage=%lx", tex, sd.Width, sd.Height, fmtname(sd.Format), sd.Pool, (unsigned long)sd.Usage); else wsprintfA(texs, "%p (no desc)", tex); } else wsprintfA(texs, "%p type=%d", tex, rt); IDirect3DBaseTexture9_Release(tex); }
    logf("  draw %lu %s n=%u: light=%lu amb=%08lx blend=%lu(%lu,%lu) atest=%lu(%lu>%lu) z=%lu/%lu cull=%lu cw=%lx fog=%lu tf=%08lx cv=%lu | ss0 op=%lu a1=%lu a2=%lu aop=%lu aa1=%lu ss1op=%lu | tex0=%s | vs=%p ps=%p fvf=%lx decl=%p | vp=%lu,%lu %lux%lu | mat d=%d,%d,%d,%d a=%d,%d,%d e=%d,%d,%d | l0 on=%d type=%d d=%d,%d,%d dir=%d,%d,%d | w=%d,%d,%d,%d v=%d,%d,%d p=%d,%d,%d,%d",
         g_draws_this_frame, what, count, (unsigned long)light, (unsigned long)amb, (unsigned long)ab, (unsigned long)sb, (unsigned long)db, (unsigned long)at, (unsigned long)ares, (unsigned long)aref, (unsigned long)ze, (unsigned long)zw, (unsigned long)cull, (unsigned long)cw, (unsigned long)fog, (unsigned long)tf, (unsigned long)cv,
         (unsigned long)cop, (unsigned long)ca1, (unsigned long)ca2, (unsigned long)aop, (unsigned long)aa1, (unsigned long)cop1, texs, vs, ps, (unsigned long)fvf, decl, (unsigned long)vp.X, (unsigned long)vp.Y, (unsigned long)vp.Width, (unsigned long)vp.Height,
         (int)(mat.Diffuse.r * 100), (int)(mat.Diffuse.g * 100), (int)(mat.Diffuse.b * 100), (int)(mat.Diffuse.a * 100), (int)(mat.Ambient.r * 100), (int)(mat.Ambient.g * 100), (int)(mat.Ambient.b * 100), (int)(mat.Emissive.r * 100), (int)(mat.Emissive.g * 100), (int)(mat.Emissive.b * 100),
         l0on, l0.Type, (int)(l0.Diffuse.r * 100), (int)(l0.Diffuse.g * 100), (int)(l0.Diffuse.b * 100), (int)(l0.Direction.x * 100), (int)(l0.Direction.y * 100), (int)(l0.Direction.z * 100),
         (int)(mw._11 * 100), (int)(mw._22 * 100), (int)(mw._41), (int)(mw._42), (int)(mv._11 * 100), (int)(mv._22 * 100), (int)(mv._43), (int)(mp._11 * 100), (int)(mp._22 * 100), (int)(mp._33 * 100), (int)(mp._43 * 100));
    if (vs) IDirect3DVertexShader9_Release(vs); if (ps) IDirect3DPixelShader9_Release(ps); if (decl) IDirect3DVertexDeclaration9_Release(decl);
}

static void patch(void **slot, void *fn) { DWORD old; VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old); *slot = fn; VirtualProtect(slot, sizeof(void *), old, &old); }
#define HOOK(name) patch((void **)&vt->name, (void *)h_##name)
static void hook_device(IDirect3DDevice9 *dev)
{
    static int done; IDirect3DDevice9Vtbl *vt = dev->lpVtbl;
    if (done) return; done = 1; orig = *vt;
    HOOK(CreateTexture); HOOK(CreateCubeTexture); HOOK(CreateVolumeTexture); HOOK(CreateVertexBuffer); HOOK(CreateIndexBuffer); HOOK(CreateRenderTarget); HOOK(CreateDepthStencilSurface);
    HOOK(UpdateTexture); HOOK(UpdateSurface); HOOK(GetRenderTargetData); HOOK(StretchRect); HOOK(ColorFill); HOOK(CreateOffscreenPlainSurface); HOOK(SetRenderTarget); HOOK(SetDepthStencilSurface);
    HOOK(BeginScene); HOOK(EndScene); HOOK(Clear); HOOK(SetTexture); HOOK(DrawPrimitive); HOOK(DrawIndexedPrimitive); HOOK(DrawPrimitiveUP); HOOK(DrawIndexedPrimitiveUP);
    HOOK(CreateVertexDeclaration); HOOK(SetVertexDeclaration); HOOK(SetFVF); HOOK(CreateVertexShader); HOOK(SetVertexShader); HOOK(CreatePixelShader); HOOK(SetPixelShader);
    HOOK(SetStreamSource); HOOK(SetIndices); HOOK(CreateQuery); HOOK(CreateStateBlock); HOOK(BeginStateBlock); HOOK(EndStateBlock); HOOK(ValidateDevice); HOOK(Reset); HOOK(TestCooperativeLevel);
    HOOK(Present); HOOK(SetRenderState); HOOK(SetTextureStageState); HOOK(SetSamplerState); HOOK(SetTransform); HOOK(SetLight); HOOK(LightEnable); HOOK(SetMaterial); HOOK(SetViewport); HOOK(SetScissorRect);
    HOOK(SetVertexShaderConstantF); HOOK(SetPixelShaderConstantF);
    logf("device vtable hooked (%d methods)", 51);
}

/* ---- IDirect3D9 hooks ---- */
static HRESULT WINAPI h9_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND hwnd, DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr;
    logf("CreateDevice adapter=%u type=%d hwnd=%p behavior=%lx: %ux%u %s count=%u windowed=%d swap=%d ms=%d/%lu autods=%d %s flags=%lx interval=%lx", adapter, type, hwnd, (unsigned long)flags, pp->BackBufferWidth, pp->BackBufferHeight, fmtname(pp->BackBufferFormat), pp->BackBufferCount, pp->Windowed, pp->SwapEffect, pp->MultiSampleType, (unsigned long)pp->MultiSampleQuality, pp->EnableAutoDepthStencil, fmtname(pp->AutoDepthStencilFormat), (unsigned long)pp->Flags, (unsigned long)pp->PresentationInterval);
    if (g_nomsaa && pp->MultiSampleType) { logf("NoMSAA: forcing MultiSampleType %d -> 0", pp->MultiSampleType); pp->MultiSampleType = D3DMULTISAMPLE_NONE; pp->MultiSampleQuality = 0; }
    hr = orig9.CreateDevice(d3d, adapter, type, hwnd, flags, pp, out);
    logf("CreateDevice -> %08lx device=%p", (unsigned long)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) { hook_device(*out); orig.GetBackBuffer(*out, 0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backbuffer); logf("backbuffer %p", g_backbuffer); }
    return hr;
}
static HRESULT WINAPI h9_CheckDeviceFormat(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT cf)
{ static int n; HRESULT hr = orig9.CheckDeviceFormat(d3d, a, t, af, usage, rt, cf); if (n++ < 200) logf("CheckDeviceFormat adapter=%s usage=%lx rtype=%d %s -> %08lx%s", fmtname(af), (unsigned long)usage, rt, fmtname(cf), (unsigned long)hr, FAILED(hr) ? " (NOT SUPPORTED)" : ""); return hr; }
static HRESULT WINAPI h9_GetDeviceCaps(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, D3DCAPS9 *c)
{
    static int n; HRESULT hr = orig9.GetDeviceCaps(d3d, a, t, c);
    if (n++ < 3 && SUCCEEDED(hr)) logf("GetDeviceCaps: vs=%08lx ps=%08lx maxtex=%lux%lu texcaps=%lx devcaps=%lx caps2=%lx rasters=%lx maxvsconst=%lu maxprims=%lu maxvidx=%lu numRTs=%lu maxstreams=%lu maxaniso=%lu vertexprocessing=%lx", (unsigned long)c->VertexShaderVersion, (unsigned long)c->PixelShaderVersion, (unsigned long)c->MaxTextureWidth, (unsigned long)c->MaxTextureHeight, (unsigned long)c->TextureCaps, (unsigned long)c->DevCaps, (unsigned long)c->Caps2, (unsigned long)c->RasterCaps, (unsigned long)c->MaxVertexShaderConst, (unsigned long)c->MaxPrimitiveCount, (unsigned long)c->MaxVertexIndex, (unsigned long)c->NumSimultaneousRTs, (unsigned long)c->MaxStreams, (unsigned long)c->MaxAnisotropy, (unsigned long)c->VertexProcessingCaps);
    return hr;
}
static HRESULT WINAPI h9_GetAdapterIdentifier(IDirect3D9 *d3d, UINT a, DWORD flags, D3DADAPTER_IDENTIFIER9 *id)
{ static int n; HRESULT hr = orig9.GetAdapterIdentifier(d3d, a, flags, id); if (n++ < 2 && SUCCEEDED(hr)) logf("GetAdapterIdentifier: driver=%s desc=%s vendor=%04lx device=%04lx driver=%08lx.%08lx", id->Driver, id->Description, (unsigned long)id->VendorId, (unsigned long)id->DeviceId, (unsigned long)id->DriverVersion.HighPart, (unsigned long)id->DriverVersion.LowPart); return hr; }
static HRESULT WINAPI h9_CheckDeviceMultiSampleType(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL windowed, D3DMULTISAMPLE_TYPE ms, DWORD *q)
{ static int n; HRESULT hr; if (g_nomsaa && ms != D3DMULTISAMPLE_NONE) { if (n++ < 10) logf("NoMSAA: CheckDeviceMultiSampleType %s ms=%d -> refused", fmtname(f), ms); if (q) *q = 0; return D3DERR_NOTAVAILABLE; } hr = orig9.CheckDeviceMultiSampleType(d3d, a, t, f, windowed, ms, q); if (n++ < 20) logf("CheckDeviceMultiSampleType %s ms=%d -> %08lx q=%lu", fmtname(f), ms, (unsigned long)hr, q ? (unsigned long)*q : 0UL); return hr; }
static void hook_d3d9(IDirect3D9 *d3d)
{
    static int done; IDirect3D9Vtbl *vt = d3d->lpVtbl;
    if (done) return; done = 1; orig9 = *vt;
    patch((void **)&vt->CreateDevice, (void *)h9_CreateDevice); patch((void **)&vt->CheckDeviceFormat, (void *)h9_CheckDeviceFormat);
    patch((void **)&vt->GetDeviceCaps, (void *)h9_GetDeviceCaps); patch((void **)&vt->CheckDeviceMultiSampleType, (void *)h9_CheckDeviceMultiSampleType); patch((void **)&vt->GetAdapterIdentifier, (void *)h9_GetAdapterIdentifier);
}

typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
static HMODULE load_real(void)
{
    char path[MAX_PATH]; UINT n;
    if (g_real) return g_real;
    n = GetModuleFileNameA(g_self, path, MAX_PATH); while (n && path[n - 1] != '\\') n--; lstrcpyA(path + n, "d3d9log.ini");
    GetPrivateProfileStringA("d3d9log", "Real", "", path + 0, 0, path); /* placeholder, see below */
    {
        char real[MAX_PATH]; char ini[MAX_PATH]; n = GetModuleFileNameA(g_self, ini, MAX_PATH); while (n && ini[n - 1] != '\\') n--; lstrcpyA(ini + n, "d3d9log.ini");
        GetPrivateProfileStringA("d3d9log", "Real", "", real, MAX_PATH, ini);
        if (!real[0]) { n = GetSystemDirectoryA(real, MAX_PATH); lstrcpyA(real + n, "\\d3d9.dll"); }
        g_real = LoadLibraryA(real);
        logf("real d3d9: %s -> %s", real, g_real ? "loaded" : "FAILED");
    }
    return g_real;
}
__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    PFN_Create9 fn; IDirect3D9 *d3d;
    if (!load_real()) return NULL;
    fn = (PFN_Create9)GetProcAddress(g_real, "Direct3DCreate9");
    d3d = fn ? fn(sdk) : NULL;
    logf("Direct3DCreate9(%u) -> %p", sdk, d3d);
    if (d3d) hook_d3d9(d3d);
    return d3d;
}
__declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { return 0; }
__declspec(dllexport) int WINAPI D3DPERF_EndEvent(void) { return 0; }
__declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) { }
__declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD o) { }
__declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }
__declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { return FALSE; }
__declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) { }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char path[MAX_PATH]; UINT n; HMODULE pin;
        g_self = inst; DisableThreadLibraryCalls(inst);
        n = GetModuleFileNameA(inst, path, MAX_PATH); while (n && path[n - 1] != '\\') n--; lstrcpyA(path + n, "d3d9log.txt");
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        lstrcpyA(path + n, "d3d9log.ini"); g_nomsaa = GetPrivateProfileIntA("d3d9log", "NoMSAA", 0, path);
        logf("d3d9log loaded in pid %lu (NoMSAA=%d)", (unsigned long)GetCurrentProcessId(), g_nomsaa);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)DllMain, &pin);
    }
    else if (reason == DLL_PROCESS_DETACH && g_log != INVALID_HANDLE_VALUE) { logf("unloading"); CloseHandle(g_log); }
    return TRUE;
}

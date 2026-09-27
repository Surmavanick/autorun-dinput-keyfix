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
static int g_dump_this_frame; static unsigned long g_draws_this_frame;
static int g_nomsaa, g_probe_enabled = 1, g_game_force, g_game_min_draws;
static void dump_state(IDirect3DDevice9 *d, const char *what, UINT count);

/* ---- visual resource-upload probe ---------------------------------------------------------
 *
 * The two-row matrix is deliberately made from six independent upload/draw paths:
 *
 *        red: DrawPrimitiveUP       green: MANAGED VB       blue: DEFAULT|DYNAMIC VB
 *     yellow: MANAGED texture     magenta: DYNAMIC texture   cyan: SYSTEMMEM -> DEFAULT
 *
 * It is drawn immediately before Present through the unhooked vtable copy, so it does not
 * contaminate the game's counters.  Every resource operation is logged on the first frame;
 * failures are always logged.  Dynamic resources are rewritten periodically so a single run
 * tests both initial creation and the recurring DISCARD/UpdateTexture upload paths.
 */
#define PROBE_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define PROBE_TEX_SIZE 8
typedef struct { float x, y, z, rhw; DWORD color; float u, v; } PROBEVERT;
typedef struct {
    int attempted, ready, first_draw;
    IDirect3DStateBlock9 *state;
    IDirect3DVertexBuffer9 *managed_vb, *dynamic_vb, *rename_vb;
    IDirect3DTexture9 *managed_tex, *dynamic_tex, *upload_src, *upload_dst;
    PROBEVERT up[6], managed[6], dynamic[6], tex[3][6], rename[2][6];
} PROBE;
static PROBE g_probe;

/* One-shot capture of the game's own fixed-function draw.  The stable black frame seen on
 * Switch is 32 DrawPrimitive calls / 64 primitives, all FVF 0x142.  We retain the largest
 * managed A8R8G8B8 draw plus the largest texture of any kind from one such frame. */
#define GAME_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define GAME_VB_COPY_MAX 512
#define GAME_TEX_HASH_MAX (4u * 1024u * 1024u)
typedef struct { float x, y, z; DWORD color; float u, v; } GAMEVERT;
typedef struct {
    int valid, finalized;
    IDirect3DTexture9 *tex;
    IDirect3DVertexBuffer9 *vb;
    IDirect3DTexture9 *replay_tex;
    IDirect3DVertexBuffer9 *replay_vb;
    D3DSURFACE_DESC td;
    D3DVERTEXBUFFER_DESC vd;
    D3DPRIMITIVETYPE type;
    UINT start_vertex, primitive_count, draw_number;
    UINT stream_offset, stride, stream_freq;
    DWORD fvf, state_ok;
    D3DMATRIX world, view, projection;
    D3DVIEWPORT9 viewport;
    RECT scissor;
    DWORD lighting, fog, alpha_blend, src_blend, dst_blend;
    DWORD alpha_test, alpha_ref, alpha_func, z_enable, z_write;
    DWORD cull, color_write, scissor_enable, clipping, clipplane_enable;
    DWORD texture_factor, srgb_write;
    DWORD color_op, color_arg1, color_arg2, alpha_op, alpha_arg1, alpha_arg2;
    DWORD texcoord_index, tex_transform;
    DWORD address_u, address_v, min_filter, mag_filter, mip_filter, srgb_texture;
    BYTE vb_bytes[GAME_VB_COPY_MAX];
    BYTE *tex_bytes;
    UINT vb_byte_count, vb_lock_offset, vertex_count, decoded_count;
    UINT tex_row_bytes, tex_rows, tex_snapshot_bytes;
    INT tex_pitch;
    int vb_snapshot_complete, tex_layout_known, tex_snapshot_complete;
    DWORD vb_hash;
    HRESULT vb_lock_hr, vb_unlock_hr, tex_lock_hr, tex_unlock_hr;
    HRESULT replay_vb_create_hr, replay_vb_lock_hr, replay_vb_unlock_hr;
    HRESULT replay_tex_create_hr, replay_tex_lock_hr, replay_tex_unlock_hr;
    DWORD tex_hash, tex_hashed_bytes, tex_rgb_nonzero, tex_alpha_nonzero;
    DWORD tex_samples[4];
    GAMEVERT vertices[6];
} GAMECAP;
typedef struct {
    int collecting, done, pending_finalize, just_finalized;
    UINT stable_frames, attempts, draw_fail_logs;
    GAMECAP managed, any;
} GAMEPROBE;
static GAMEPROBE g_game;

/* Run 8 deliberately captures a busy menu frame, renders retained ORIGINAL resources to an
 * offscreen target, forces completion with GetRenderTargetData, and only then read-locks and
 * clones them for an identical REPLAY pass.  This avoids relying on a photo and also avoids
 * run 7's ambiguity: its original commands were queued in the same scene before the locks. */
#define RUN8_CLEAR_RGB 0x00010203u
#define RUN8_VB_RGB    0x0017b35au
#define RUN8_CTRL_RGB  0x00d05cffu
#define RUN8_FIX_COPY_MAX (64u * 1024u)
typedef struct {
    HRESULT grtd_hr, lock_hr, unlock_hr;
    DWORD primary_hash, any_hash;
    UINT primary_nonclear, any_nonclear, vb_exact, control_exact;
    int valid;
} RUN8STATS;
typedef struct {
    int attempted, complete, texture_verdict, vb_verdict;
    int fix_vb, fix_texture;
    unsigned long fix_vb_attempts, fix_vb_success, fix_vb_fail;
    unsigned long fix_texture_substitutions;
    RUN8STATS original, replay;
} RUN8PROBE;
static RUN8PROBE g_run8;

static DWORD probe_hash_bytes(DWORD h, const void *data, UINT size)
{
    const BYTE *p = (const BYTE *)data;
    while (size--) { h ^= *p++; h *= 16777619u; }
    return h;
}
static DWORD probe_hash_rows(const D3DLOCKED_RECT *lr)
{
    DWORD h = 2166136261u; UINT y;
    for (y = 0; y < PROBE_TEX_SIZE; y++)
        h = probe_hash_bytes(h, (const BYTE *)lr->pBits + y * lr->Pitch, PROBE_TEX_SIZE * sizeof(DWORD));
    return h;
}
static void probe_quad(PROBEVERT *v, float x0, float y0, float x1, float y1, DWORD color)
{
    PROBEVERT a = { x0 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, color, 0.0f, 0.0f };
    PROBEVERT b = { x1 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, color, 1.0f, 0.0f };
    PROBEVERT c = { x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, color, 1.0f, 1.0f };
    PROBEVERT d = { x0 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, color, 0.0f, 1.0f };
    v[0] = a; v[1] = b; v[2] = c; v[3] = a; v[4] = c; v[5] = d;
}
static void probe_note(const char *what, HRESULT hr, int verbose)
{
    if (verbose || FAILED(hr)) logf("PROBE %s -> %08lx", what, (unsigned long)hr);
}
static HRESULT probe_fill_vb(IDirect3DVertexBuffer9 *vb, const PROBEVERT *src, DWORD flags,
                             const char *name, int verbose)
{
    void *bits = NULL; HRESULT hr, uhr; DWORD hash = 0;
    if (!vb) { if (verbose) logf("PROBE %s skipped (no resource)", name); return D3DERR_INVALIDCALL; }
    hr = IDirect3DVertexBuffer9_Lock(vb, 0, 6 * sizeof(*src), &bits, flags);
    probe_note(name, hr, verbose);
    if (FAILED(hr) || !bits) return hr;
    CopyMemory(bits, src, 6 * sizeof(*src));
    hash = probe_hash_bytes(2166136261u, bits, 6 * sizeof(*src));
    if (verbose) logf("PROBE %s write hash=%08lx sample=(%ld,%ld,%08lx)", name, (unsigned long)hash,
         (long)src[0].x, (long)src[0].y, (unsigned long)src[0].color);
    uhr = IDirect3DVertexBuffer9_Unlock(vb); probe_note("VB Unlock", uhr, verbose);
    return FAILED(uhr) ? uhr : hr;
}
static HRESULT probe_read_vb(IDirect3DVertexBuffer9 *vb, const char *name)
{
    void *bits = NULL; HRESULT hr, uhr; DWORD hash;
    if (!vb) return D3DERR_INVALIDCALL;
    hr = IDirect3DVertexBuffer9_Lock(vb, 0, 6 * sizeof(PROBEVERT), &bits, D3DLOCK_READONLY);
    probe_note(name, hr, 1);
    if (FAILED(hr) || !bits) return hr;
    hash = probe_hash_bytes(2166136261u, bits, 6 * sizeof(PROBEVERT));
    logf("PROBE %s readback hash=%08lx sample=%08lx", name, (unsigned long)hash,
         (unsigned long)((PROBEVERT *)bits)[0].color);
    uhr = IDirect3DVertexBuffer9_Unlock(vb); probe_note("VB readback Unlock", uhr, 1);
    return FAILED(uhr) ? uhr : hr;
}
static HRESULT probe_fill_tex(IDirect3DTexture9 *tex, DWORD bright, DWORD dark, DWORD flags,
                              const char *name, int verbose)
{
    D3DLOCKED_RECT lr; HRESULT hr, uhr; UINT x, y; DWORD hash, s0, s1;
    if (!tex) { if (verbose) logf("PROBE %s skipped (no resource)", name); return D3DERR_INVALIDCALL; }
    ZeroMemory(&lr, sizeof(lr));
    hr = IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, flags); probe_note(name, hr, verbose);
    if (FAILED(hr) || !lr.pBits) return hr;
    for (y = 0; y < PROBE_TEX_SIZE; y++) for (x = 0; x < PROBE_TEX_SIZE; x++)
        ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = (((x >> 1) ^ (y >> 1)) & 1) ? bright : dark;
    /* White/black opposing corners make rotation, stale data and channel swaps visible. */
    *(DWORD *)lr.pBits = 0xffffffffu;
    ((DWORD *)((BYTE *)lr.pBits + 7 * lr.Pitch))[7] = 0xff000000u;
    hash = probe_hash_rows(&lr); s0 = *(DWORD *)lr.pBits;
    s1 = ((DWORD *)((BYTE *)lr.pBits + 3 * lr.Pitch))[4];
    if (verbose) logf("PROBE %s write pitch=%d hash=%08lx samples=%08lx/%08lx", name, lr.Pitch,
         (unsigned long)hash, (unsigned long)s0, (unsigned long)s1);
    uhr = IDirect3DTexture9_UnlockRect(tex, 0); probe_note("texture UnlockRect", uhr, verbose);
    return FAILED(uhr) ? uhr : hr;
}
static HRESULT probe_read_tex(IDirect3DTexture9 *tex, const char *name)
{
    D3DLOCKED_RECT lr; HRESULT hr, uhr; DWORD hash, s0, s1;
    if (!tex) return D3DERR_INVALIDCALL;
    ZeroMemory(&lr, sizeof(lr));
    hr = IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, D3DLOCK_READONLY); probe_note(name, hr, 1);
    if (FAILED(hr) || !lr.pBits) return hr;
    hash = probe_hash_rows(&lr); s0 = *(DWORD *)lr.pBits;
    s1 = ((DWORD *)((BYTE *)lr.pBits + 3 * lr.Pitch))[4];
    logf("PROBE %s readback pitch=%d hash=%08lx samples=%08lx/%08lx", name, lr.Pitch,
         (unsigned long)hash, (unsigned long)s0, (unsigned long)s1);
    uhr = IDirect3DTexture9_UnlockRect(tex, 0); probe_note("texture readback UnlockRect", uhr, 1);
    return FAILED(uhr) ? uhr : hr;
}

enum {
    GAME_OK_WORLD = 0x0001, GAME_OK_VIEW = 0x0002, GAME_OK_PROJECTION = 0x0004,
    GAME_OK_VIEWPORT = 0x0008, GAME_OK_SCISSOR = 0x0010, GAME_OK_FREQUENCY = 0x0020,
    GAME_OK_RENDERSTATES = 0x0040, GAME_OK_TSS = 0x0080, GAME_OK_SAMPLER = 0x0100
};
static void game_snapshot_vb(GAMECAP *c);
static void game_snapshot_texture(GAMECAP *c);

static void gamecap_clear(GAMECAP *c)
{
    if (c->tex) IDirect3DTexture9_Release(c->tex);
    if (c->vb) IDirect3DVertexBuffer9_Release(c->vb);
    if (c->replay_tex) IDirect3DTexture9_Release(c->replay_tex);
    if (c->replay_vb) IDirect3DVertexBuffer9_Release(c->replay_vb);
    if (c->tex_bytes) HeapFree(GetProcessHeap(), 0, c->tex_bytes);
    ZeroMemory(c, sizeof(*c));
}

static void gamecap_move(GAMECAP *dst, GAMECAP *src)
{
    gamecap_clear(dst); *dst = *src; ZeroMemory(src, sizeof(*src));
}

static void gamecap_clone(GAMECAP *dst, const GAMECAP *src)
{
    BYTE *copy = NULL;
    if (src->tex_bytes && src->tex_snapshot_bytes)
        copy = (BYTE *)HeapAlloc(GetProcessHeap(), 0, src->tex_snapshot_bytes);
    gamecap_clear(dst); *dst = *src;
    if (dst->tex) IDirect3DTexture9_AddRef(dst->tex);
    if (dst->vb) IDirect3DVertexBuffer9_AddRef(dst->vb);
    if (dst->replay_tex) IDirect3DTexture9_AddRef(dst->replay_tex);
    if (dst->replay_vb) IDirect3DVertexBuffer9_AddRef(dst->replay_vb);
    dst->tex_bytes = copy;
    if (copy) CopyMemory(copy, src->tex_bytes, src->tex_snapshot_bytes);
    else if (src->tex_snapshot_bytes) dst->tex_snapshot_complete = 0;
}

static void game_release_all(void)
{
    gamecap_clear(&g_game.managed); gamecap_clear(&g_game.any);
    ZeroMemory(&g_game, sizeof(g_game));
}

static int gamecap_take(IDirect3DDevice9 *d, GAMECAP *c, IDirect3DTexture9 *tex,
                        const D3DSURFACE_DESC *td, D3DPRIMITIVETYPE type,
                        UINT start_vertex, UINT primitive_count, UINT draw_number)
{
    HRESULT hr, all; DWORD fvf = 0;
    ZeroMemory(c, sizeof(*c));
    c->vb_lock_hr = c->vb_unlock_hr = c->tex_lock_hr = c->tex_unlock_hr = E_FAIL;
    c->replay_vb_create_hr = c->replay_vb_lock_hr = c->replay_vb_unlock_hr = E_FAIL;
    c->replay_tex_create_hr = c->replay_tex_lock_hr = c->replay_tex_unlock_hr = E_FAIL;
    c->tex = tex; IDirect3DTexture9_AddRef(c->tex); c->td = *td;
    c->type = type; c->start_vertex = start_vertex; c->primitive_count = primitive_count;
    c->draw_number = draw_number;
    hr = orig.GetFVF(d, &fvf); if (FAILED(hr) || fvf != GAME_FVF) goto fail; c->fvf = fvf;
    hr = orig.GetStreamSource(d, 0, &c->vb, &c->stream_offset, &c->stride);
    if (FAILED(hr) || !c->vb || c->stride < sizeof(GAMEVERT)) goto fail;
    hr = IDirect3DVertexBuffer9_GetDesc(c->vb, &c->vd); if (FAILED(hr)) goto fail;
    if (SUCCEEDED(orig.GetStreamSourceFreq(d, 0, &c->stream_freq))) c->state_ok |= GAME_OK_FREQUENCY;
    if (SUCCEEDED(orig.GetTransform(d, D3DTS_WORLD, &c->world))) c->state_ok |= GAME_OK_WORLD;
    if (SUCCEEDED(orig.GetTransform(d, D3DTS_VIEW, &c->view))) c->state_ok |= GAME_OK_VIEW;
    if (SUCCEEDED(orig.GetTransform(d, D3DTS_PROJECTION, &c->projection))) c->state_ok |= GAME_OK_PROJECTION;
    if (SUCCEEDED(orig.GetViewport(d, &c->viewport))) c->state_ok |= GAME_OK_VIEWPORT;
    if (SUCCEEDED(orig.GetScissorRect(d, &c->scissor))) c->state_ok |= GAME_OK_SCISSOR;

    all = S_OK;
#define GAME_GET_RS(field, state) do { hr = orig.GetRenderState(d, (state), &c->field); if (FAILED(hr)) all = hr; } while (0)
    GAME_GET_RS(lighting, D3DRS_LIGHTING); GAME_GET_RS(fog, D3DRS_FOGENABLE);
    GAME_GET_RS(alpha_blend, D3DRS_ALPHABLENDENABLE); GAME_GET_RS(src_blend, D3DRS_SRCBLEND); GAME_GET_RS(dst_blend, D3DRS_DESTBLEND);
    GAME_GET_RS(alpha_test, D3DRS_ALPHATESTENABLE); GAME_GET_RS(alpha_ref, D3DRS_ALPHAREF); GAME_GET_RS(alpha_func, D3DRS_ALPHAFUNC);
    GAME_GET_RS(z_enable, D3DRS_ZENABLE); GAME_GET_RS(z_write, D3DRS_ZWRITEENABLE); GAME_GET_RS(cull, D3DRS_CULLMODE);
    GAME_GET_RS(color_write, D3DRS_COLORWRITEENABLE); GAME_GET_RS(scissor_enable, D3DRS_SCISSORTESTENABLE);
    GAME_GET_RS(clipping, D3DRS_CLIPPING); GAME_GET_RS(clipplane_enable, D3DRS_CLIPPLANEENABLE);
    GAME_GET_RS(texture_factor, D3DRS_TEXTUREFACTOR); GAME_GET_RS(srgb_write, D3DRS_SRGBWRITEENABLE);
#undef GAME_GET_RS
    if (SUCCEEDED(all)) c->state_ok |= GAME_OK_RENDERSTATES;

    all = S_OK;
#define GAME_GET_TSS(field, state) do { hr = orig.GetTextureStageState(d, 0, (state), &c->field); if (FAILED(hr)) all = hr; } while (0)
    GAME_GET_TSS(color_op, D3DTSS_COLOROP); GAME_GET_TSS(color_arg1, D3DTSS_COLORARG1); GAME_GET_TSS(color_arg2, D3DTSS_COLORARG2);
    GAME_GET_TSS(alpha_op, D3DTSS_ALPHAOP); GAME_GET_TSS(alpha_arg1, D3DTSS_ALPHAARG1); GAME_GET_TSS(alpha_arg2, D3DTSS_ALPHAARG2);
    GAME_GET_TSS(texcoord_index, D3DTSS_TEXCOORDINDEX); GAME_GET_TSS(tex_transform, D3DTSS_TEXTURETRANSFORMFLAGS);
#undef GAME_GET_TSS
    if (SUCCEEDED(all)) c->state_ok |= GAME_OK_TSS;

    all = S_OK;
#define GAME_GET_SS(field, state) do { hr = orig.GetSamplerState(d, 0, (state), &c->field); if (FAILED(hr)) all = hr; } while (0)
    GAME_GET_SS(address_u, D3DSAMP_ADDRESSU); GAME_GET_SS(address_v, D3DSAMP_ADDRESSV);
    GAME_GET_SS(min_filter, D3DSAMP_MINFILTER); GAME_GET_SS(mag_filter, D3DSAMP_MAGFILTER); GAME_GET_SS(mip_filter, D3DSAMP_MIPFILTER);
    GAME_GET_SS(srgb_texture, D3DSAMP_SRGBTEXTURE);
#undef GAME_GET_SS
    if (SUCCEEDED(all)) c->state_ok |= GAME_OK_SAMPLER;
    /* Do not read-lock either original resource here.  Run 8 first renders and reads back
       retained originals, then snapshots them and creates replay copies. */
    c->valid = 1; return 1;
fail:
    gamecap_clear(c); return 0;
}

static int gamecap_bigger(const D3DSURFACE_DESC *td, const GAMECAP *old, int prefer_dynamic)
{
    ULONGLONG area = (ULONGLONG)td->Width * (ULONGLONG)td->Height;
    ULONGLONG old_area;
    if (!old->valid) return 1;
    old_area = (ULONGLONG)old->td.Width * (ULONGLONG)old->td.Height;
    if (area != old_area) return area > old_area;
    if (prefer_dynamic && (td->Usage & D3DUSAGE_DYNAMIC) && !(old->td.Usage & D3DUSAGE_DYNAMIC)) return 1;
    return 0;
}

static void game_consider_draw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type,
                               UINT start_vertex, UINT primitive_count, UINT draw_number)
{
    IDirect3DBaseTexture9 *base = NULL; IDirect3DTexture9 *tex;
    IDirect3DVertexShader9 *vs = NULL; IDirect3DPixelShader9 *ps = NULL;
    D3DSURFACE_DESC td; GAMECAP tmp; DWORD fvf = 0; HRESULT hr;
    int want_managed, want_any;
    if (!g_game.collecting || primitive_count != 2) return;
    if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLEFAN) return;
    hr = orig.GetFVF(d, &fvf); if (FAILED(hr) || fvf != GAME_FVF) return;
    if (FAILED(orig.GetVertexShader(d, &vs)) || FAILED(orig.GetPixelShader(d, &ps))) {
        if (vs) IDirect3DVertexShader9_Release(vs); if (ps) IDirect3DPixelShader9_Release(ps); return;
    }
    if (vs || ps) { if (vs) IDirect3DVertexShader9_Release(vs); if (ps) IDirect3DPixelShader9_Release(ps); return; }
    hr = orig.GetTexture(d, 0, &base); if (FAILED(hr) || !base) return;
    if (IDirect3DBaseTexture9_GetType(base) != D3DRTYPE_TEXTURE) { IDirect3DBaseTexture9_Release(base); return; }
    tex = (IDirect3DTexture9 *)base; ZeroMemory(&td, sizeof(td));
    hr = IDirect3DTexture9_GetLevelDesc(tex, 0, &td);
    if (FAILED(hr)) { IDirect3DBaseTexture9_Release(base); return; }
    want_managed = td.Pool == D3DPOOL_MANAGED && td.Format == D3DFMT_A8R8G8B8 && gamecap_bigger(&td, &g_game.managed, 0);
    want_any = gamecap_bigger(&td, &g_game.any, 1);
    if ((want_managed || want_any) && gamecap_take(d, &tmp, tex, &td, type, start_vertex, primitive_count, draw_number)) {
        if (want_managed && want_any) { gamecap_move(&g_game.managed, &tmp); gamecap_clone(&g_game.any, &g_game.managed); }
        else if (want_managed) gamecap_move(&g_game.managed, &tmp);
        else gamecap_move(&g_game.any, &tmp);
    }
    IDirect3DBaseTexture9_Release(base);
}

static DWORD game_float_bits(float v)
{
    DWORD bits; CopyMemory(&bits, &v, sizeof(bits)); return bits;
}

static int game_float_finite(float v)
{
    return (game_float_bits(v) & 0x7f800000u) != 0x7f800000u;
}

static UINT game_vertex_count(D3DPRIMITIVETYPE type, UINT primitive_count)
{
    switch (type) {
    case D3DPT_TRIANGLELIST: return primitive_count * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return primitive_count + 2;
    case D3DPT_LINELIST: return primitive_count * 2;
    case D3DPT_LINESTRIP: return primitive_count + 1;
    case D3DPT_POINTLIST: return primitive_count;
    default: return 0;
    }
}

static void game_snapshot_vb(GAMECAP *c)
{
    void *bits = NULL; ULONGLONG offset64, bytes64, full_bytes64; UINT offset, bytes, i, count;
    if (!c->vb) return;
    count = game_vertex_count(c->type, c->primitive_count); c->vertex_count = count;
    offset64 = (ULONGLONG)c->stream_offset + (ULONGLONG)c->start_vertex * (ULONGLONG)c->stride;
    bytes64 = (ULONGLONG)count * (ULONGLONG)c->stride;
    if (!count || offset64 >= c->vd.Size || bytes64 > (ULONGLONG)c->vd.Size - offset64) {
        c->vb_lock_hr = D3DERR_INVALIDCALL; return;
    }
    full_bytes64 = bytes64;
    if (bytes64 > GAME_VB_COPY_MAX) bytes64 = GAME_VB_COPY_MAX;
    offset = (UINT)offset64; bytes = (UINT)bytes64; c->vb_lock_offset = offset;
    c->vb_lock_hr = IDirect3DVertexBuffer9_Lock(c->vb, offset, bytes, &bits, D3DLOCK_READONLY);
    if (FAILED(c->vb_lock_hr) || !bits) return;
    CopyMemory(c->vb_bytes, bits, bytes); c->vb_byte_count = bytes;
    c->vb_hash = probe_hash_bytes(2166136261u, c->vb_bytes, bytes);
    if (c->fvf == GAME_FVF && c->stride >= sizeof(GAMEVERT)) {
        c->decoded_count = count > 6 ? 6 : count;
        for (i = 0; i < c->decoded_count; i++) {
            if ((ULONGLONG)i * c->stride + sizeof(GAMEVERT) > bytes) { c->decoded_count = i; break; }
            CopyMemory(&c->vertices[i], c->vb_bytes + i * c->stride, sizeof(GAMEVERT));
        }
    }
    c->vb_unlock_hr = IDirect3DVertexBuffer9_Unlock(c->vb);
    c->vb_snapshot_complete = full_bytes64 == c->vb_byte_count && SUCCEEDED(c->vb_unlock_hr);
}

static int game_texture_layout(D3DFORMAT fmt, UINT width, UINT height,
                               UINT pitch, UINT *row_bytes, UINT *rows, int *argb32)
{
    ULONGLONG rb = 0; *argb32 = 0; *rows = height;
    switch (fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: rb = (ULONGLONG)width * 4; *argb32 = 1; break;
    case D3DFMT_R8G8B8: rb = (ULONGLONG)width * 3; break;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: case D3DFMT_L16:
    case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_R16F: rb = (ULONGLONG)width * 2; break;
    case D3DFMT_A8: case D3DFMT_L8: rb = width; break;
    case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16: case D3DFMT_X8L8V8U8: case D3DFMT_G16R16F:
    case D3DFMT_R32F: rb = (ULONGLONG)width * 4; break;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F: rb = (ULONGLONG)width * 8; break;
    case D3DFMT_A32B32G32R32F: rb = (ULONGLONG)width * 16; break;
    case D3DFMT_DXT1: rb = (ULONGLONG)((width + 3) / 4) * 8; *rows = (height + 3) / 4; break;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        rb = (ULONGLONG)((width + 3) / 4) * 16; *rows = (height + 3) / 4; break;
    default: *row_bytes = 0; *rows = 0; return 0;
    }
    if (rb > 0xffffffffu || rb > pitch) { *row_bytes = 0; *rows = 0; return 0; }
    *row_bytes = (UINT)rb; return 1;
}

static DWORD game_texel32(const D3DLOCKED_RECT *lr, UINT x, UINT y)
{
    DWORD p = 0; const BYTE *row = (const BYTE *)lr->pBits + (LONG)y * lr->Pitch;
    CopyMemory(&p, row + x * 4, sizeof(p)); return p;
}

static void game_snapshot_texture(GAMECAP *c)
{
    D3DLOCKED_RECT lr; UINT pitch, row_bytes, rows, y, x, n; DWORD remain;
    ULONGLONG total64; int argb32;
    if (!c->tex) return;
    ZeroMemory(&lr, sizeof(lr));
    c->tex_lock_hr = IDirect3DTexture9_LockRect(c->tex, 0, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(c->tex_lock_hr) || !lr.pBits) return;
    c->tex_pitch = lr.Pitch;
    pitch = lr.Pitch < 0 ? (UINT)(-lr.Pitch) : (UINT)lr.Pitch;
    c->tex_layout_known = game_texture_layout(c->td.Format, c->td.Width, c->td.Height,
                                               pitch, &row_bytes, &rows, &argb32);
    if (!c->tex_layout_known) { c->tex_unlock_hr = IDirect3DTexture9_UnlockRect(c->tex, 0); return; }
    c->tex_row_bytes = row_bytes; c->tex_rows = rows;
    total64 = (ULONGLONG)row_bytes * rows;
    remain = total64 > GAME_TEX_HASH_MAX ? GAME_TEX_HASH_MAX : (DWORD)total64;
    if (remain) c->tex_bytes = (BYTE *)HeapAlloc(GetProcessHeap(), 0, remain);
    if (c->tex_bytes) c->tex_snapshot_bytes = remain;
    c->tex_hash = 2166136261u; remain = GAME_TEX_HASH_MAX;
    for (y = 0; y < rows && remain; y++) {
        const BYTE *row = (const BYTE *)lr.pBits + (LONG)y * lr.Pitch;
        n = row_bytes < remain ? row_bytes : remain;
        c->tex_hash = probe_hash_bytes(c->tex_hash, row, n);
        if (c->tex_bytes && c->tex_hashed_bytes + n <= c->tex_snapshot_bytes)
            CopyMemory(c->tex_bytes + c->tex_hashed_bytes, row, n);
        c->tex_hashed_bytes += n; remain -= n;
        if (argb32) for (x = 0; x < n / 4; x++) {
            DWORD p; CopyMemory(&p, row + x * 4, sizeof(p));
            if (p & 0x00ffffffu) c->tex_rgb_nonzero++;
            if (p & 0xff000000u) c->tex_alpha_nonzero++;
        }
    }
    if (argb32 && c->td.Width && c->td.Height) {
        c->tex_samples[0] = game_texel32(&lr, 0, 0);
        c->tex_samples[1] = game_texel32(&lr, c->td.Width - 1, 0);
        c->tex_samples[2] = game_texel32(&lr, c->td.Width / 2, c->td.Height / 2);
        c->tex_samples[3] = game_texel32(&lr, c->td.Width - 1, c->td.Height - 1);
    }
    c->tex_snapshot_complete = c->tex_bytes && total64 == c->tex_snapshot_bytes;
    c->tex_unlock_hr = IDirect3DTexture9_UnlockRect(c->tex, 0);
}

static void game_log_matrix(const char *name, const char *which, const D3DMATRIX *m)
{
    const float *v = (const float *)m; UINT r;
    for (r = 0; r < 4; r++) logf("GAMECAP %s %s%u=%08lx/%08lx/%08lx/%08lx", name, which, r,
        (unsigned long)game_float_bits(v[r*4]), (unsigned long)game_float_bits(v[r*4+1]),
        (unsigned long)game_float_bits(v[r*4+2]), (unsigned long)game_float_bits(v[r*4+3]));
}

static void game_matrix_multiply(D3DMATRIX *out, const D3DMATRIX *a, const D3DMATRIX *b)
{
    D3DMATRIX t; UINT i, j, k;
    ZeroMemory(&t, sizeof(t));
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++)
        for (k = 0; k < 4; k++) t.m[i][j] += a->m[i][k] * b->m[k][j];
    *out = t;
}

static void game_log_vertices(const GAMECAP *c, const char *name)
{
    D3DMATRIX wv, wvp; UINT i;
    game_matrix_multiply(&wv, &c->world, &c->view); game_matrix_multiply(&wvp, &wv, &c->projection);
    for (i = 0; i < c->decoded_count; i++) {
        const GAMEVERT *v = &c->vertices[i]; float cx, cy, cz, cw, sx, sy;
        cx = v->x*wvp.m[0][0] + v->y*wvp.m[1][0] + v->z*wvp.m[2][0] + wvp.m[3][0];
        cy = v->x*wvp.m[0][1] + v->y*wvp.m[1][1] + v->z*wvp.m[2][1] + wvp.m[3][1];
        cz = v->x*wvp.m[0][2] + v->y*wvp.m[1][2] + v->z*wvp.m[2][2] + wvp.m[3][2];
        cw = v->x*wvp.m[0][3] + v->y*wvp.m[1][3] + v->z*wvp.m[2][3] + wvp.m[3][3];
        logf("GAMECAP %s V%u xyz=%08lx/%08lx/%08lx color=%08lx uv=%08lx/%08lx", name, i,
             (unsigned long)game_float_bits(v->x), (unsigned long)game_float_bits(v->y),
             (unsigned long)game_float_bits(v->z), (unsigned long)v->color,
             (unsigned long)game_float_bits(v->u), (unsigned long)game_float_bits(v->v));
        if (game_float_finite(cx) && game_float_finite(cy) && game_float_finite(cz) &&
            game_float_finite(cw) && cw != 0.0f && (c->state_ok & GAME_OK_VIEWPORT)) {
            sx = c->viewport.X + (cx / cw + 1.0f) * c->viewport.Width * 0.5f;
            sy = c->viewport.Y + (1.0f - cy / cw) * c->viewport.Height * 0.5f;
            if (game_float_finite(sx) && game_float_finite(sy) && sx > -1000000.0f && sx < 1000000.0f && sy > -1000000.0f && sy < 1000000.0f)
                logf("GAMECAP %s V%u clip=%08lx/%08lx/%08lx/%08lx screen100=%ld/%ld", name, i,
                     (unsigned long)game_float_bits(cx), (unsigned long)game_float_bits(cy),
                     (unsigned long)game_float_bits(cz), (unsigned long)game_float_bits(cw),
                     (long)(sx * 100.0f), (long)(sy * 100.0f));
            else logf("GAMECAP %s V%u clip=%08lx/%08lx/%08lx/%08lx screen=invalid", name, i,
                     (unsigned long)game_float_bits(cx), (unsigned long)game_float_bits(cy),
                     (unsigned long)game_float_bits(cz), (unsigned long)game_float_bits(cw));
        } else logf("GAMECAP %s V%u clip=%08lx/%08lx/%08lx/%08lx screen=invalid", name, i,
                   (unsigned long)game_float_bits(cx), (unsigned long)game_float_bits(cy),
                   (unsigned long)game_float_bits(cz), (unsigned long)game_float_bits(cw));
    }
}

static void game_make_replay_resources(IDirect3DDevice9 *d, GAMECAP *c)
{
    void *vb_bits = NULL; D3DLOCKED_RECT lr; UINT y, pitch, rb, rows; int argb;
    if (c->vb_snapshot_complete && c->vb_byte_count) {
        c->replay_vb_create_hr = orig.CreateVertexBuffer(d, c->vb_byte_count, 0, GAME_FVF,
                                                         D3DPOOL_MANAGED, &c->replay_vb, NULL);
        if (SUCCEEDED(c->replay_vb_create_hr)) {
            c->replay_vb_lock_hr = IDirect3DVertexBuffer9_Lock(c->replay_vb, 0,
                                                               c->vb_byte_count, &vb_bits, 0);
            if (SUCCEEDED(c->replay_vb_lock_hr) && vb_bits) {
                CopyMemory(vb_bits, c->vb_bytes, c->vb_byte_count);
                c->replay_vb_unlock_hr = IDirect3DVertexBuffer9_Unlock(c->replay_vb);
                if (FAILED(c->replay_vb_unlock_hr)) { IDirect3DVertexBuffer9_Release(c->replay_vb); c->replay_vb = NULL; }
            }
            else { IDirect3DVertexBuffer9_Release(c->replay_vb); c->replay_vb = NULL; }
        }
    }
    if (c->tex_snapshot_complete && c->tex_bytes) {
        c->replay_tex_create_hr = orig.CreateTexture(d, c->td.Width, c->td.Height, 1, 0,
                                                      c->td.Format, D3DPOOL_MANAGED,
                                                      &c->replay_tex, NULL);
        if (SUCCEEDED(c->replay_tex_create_hr)) {
            ZeroMemory(&lr, sizeof(lr));
            c->replay_tex_lock_hr = IDirect3DTexture9_LockRect(c->replay_tex, 0, &lr, NULL, 0);
            if (SUCCEEDED(c->replay_tex_lock_hr) && lr.pBits) {
                pitch = lr.Pitch < 0 ? (UINT)(-lr.Pitch) : (UINT)lr.Pitch;
                if (game_texture_layout(c->td.Format, c->td.Width, c->td.Height,
                                        pitch, &rb, &rows, &argb) &&
                    rb == c->tex_row_bytes && rows == c->tex_rows) {
                    for (y = 0; y < rows; y++)
                        CopyMemory((BYTE *)lr.pBits + (LONG)y * lr.Pitch,
                                   c->tex_bytes + y * rb, rb);
                    c->replay_tex_unlock_hr = IDirect3DTexture9_UnlockRect(c->replay_tex, 0);
                    if (FAILED(c->replay_tex_unlock_hr)) { IDirect3DTexture9_Release(c->replay_tex); c->replay_tex = NULL; }
                } else {
                    c->replay_tex_unlock_hr = IDirect3DTexture9_UnlockRect(c->replay_tex, 0);
                    c->replay_tex_lock_hr = D3DERR_INVALIDCALL;
                    IDirect3DTexture9_Release(c->replay_tex); c->replay_tex = NULL;
                }
            }
            else { IDirect3DTexture9_Release(c->replay_tex); c->replay_tex = NULL; }
        }
    }
}

static void game_finalize_slot(IDirect3DDevice9 *d, GAMECAP *c, const char *name)
{
    UINT original_levels; DWORD original_lod;
    if (!c->valid) return;
    /* Deliberately delayed until after the original-resource overlay for this Present. */
    game_snapshot_vb(c); game_snapshot_texture(c);
    logf("GAMECAP %s draw=%u type=%d sv=%u pc=%u fvf=%lx stateOK=%lx", name,
         c->draw_number, c->type, c->start_vertex, c->primitive_count,
         (unsigned long)c->fvf, (unsigned long)c->state_ok);
    logf("GAMECAP %s TEX=%p %ux%u %s usage=%lx pool=%d", name, c->tex,
         c->td.Width, c->td.Height, fmtname(c->td.Format), (unsigned long)c->td.Usage, c->td.Pool);
    original_levels = IDirect3DTexture9_GetLevelCount(c->tex);
    original_lod = IDirect3DTexture9_GetLOD(c->tex);
    logf("GAMECAP %s original TEX levels=%u lod=%lu", name, original_levels,
         (unsigned long)original_lod);
    logf("GAMECAP %s VB=%p size=%u usage=%lx pool=%d off=%u stride=%u freq=%lx", name, c->vb,
         c->vd.Size, (unsigned long)c->vd.Usage, c->vd.Pool, c->stream_offset, c->stride,
         (unsigned long)c->stream_freq);
    logf("GAMECAP %s VP=%lu,%lu %lux%lu z=%08lx/%08lx scissor=%ld,%ld,%ld,%ld", name,
         (unsigned long)c->viewport.X, (unsigned long)c->viewport.Y,
         (unsigned long)c->viewport.Width, (unsigned long)c->viewport.Height,
         (unsigned long)game_float_bits(c->viewport.MinZ), (unsigned long)game_float_bits(c->viewport.MaxZ),
         c->scissor.left, c->scissor.top, c->scissor.right, c->scissor.bottom);
    logf("GAMECAP %s RS light=%lu fog=%lu blend=%lu/%lu/%lu atest=%lu/%lu/%lu z=%lu/%lu cull=%lu cw=%lx", name,
         (unsigned long)c->lighting, (unsigned long)c->fog, (unsigned long)c->alpha_blend,
         (unsigned long)c->src_blend, (unsigned long)c->dst_blend, (unsigned long)c->alpha_test,
         (unsigned long)c->alpha_func, (unsigned long)c->alpha_ref, (unsigned long)c->z_enable,
         (unsigned long)c->z_write, (unsigned long)c->cull, (unsigned long)c->color_write);
    logf("GAMECAP %s RS scissor=%lu clip=%lu/%lx tf=%08lx srgbW=%lu", name,
         (unsigned long)c->scissor_enable, (unsigned long)c->clipping,
         (unsigned long)c->clipplane_enable, (unsigned long)c->texture_factor, (unsigned long)c->srgb_write);
    logf("GAMECAP %s TSS c=%lu/%lu/%lu a=%lu/%lu/%lu tc=%lx tt=%lx", name,
         (unsigned long)c->color_op, (unsigned long)c->color_arg1, (unsigned long)c->color_arg2,
         (unsigned long)c->alpha_op, (unsigned long)c->alpha_arg1, (unsigned long)c->alpha_arg2,
         (unsigned long)c->texcoord_index, (unsigned long)c->tex_transform);
    logf("GAMECAP %s SAMP addr=%lu/%lu filter=%lu/%lu/%lu srgb=%lu", name,
         (unsigned long)c->address_u, (unsigned long)c->address_v, (unsigned long)c->min_filter,
         (unsigned long)c->mag_filter, (unsigned long)c->mip_filter, (unsigned long)c->srgb_texture);
    logf("GAMECAP %s VB snapshot-at-draw off=%u bytes=%u complete=%d Lock=%08lx Unlock=%08lx%s", name,
         c->vb_lock_offset, c->vb_byte_count, c->vb_snapshot_complete, (unsigned long)c->vb_lock_hr,
         (unsigned long)c->vb_unlock_hr, (c->vd.Usage & D3DUSAGE_WRITEONLY) ? " WRITEONLY" : "");
    if (c->vb_byte_count) logf("GAMECAP %s VB hash=%08lx vertices=%u decoded=%u", name,
         (unsigned long)c->vb_hash, c->vertex_count, c->decoded_count);
    logf("GAMECAP %s TEX snapshot-at-draw Lock=%08lx Unlock=%08lx layout=%d pitch=%d", name,
         (unsigned long)c->tex_lock_hr, (unsigned long)c->tex_unlock_hr,
         c->tex_layout_known, c->tex_pitch);
    if (c->tex_hashed_bytes) logf("GAMECAP %s TEX bytes=%lu/%u hash=%08lx rgbNZ=%lu alphaNZ=%lu complete=%d", name,
         (unsigned long)c->tex_hashed_bytes, c->tex_snapshot_bytes, (unsigned long)c->tex_hash,
         (unsigned long)c->tex_rgb_nonzero, (unsigned long)c->tex_alpha_nonzero, c->tex_snapshot_complete);
    if (c->tex_layout_known && (c->td.Format == D3DFMT_A8R8G8B8 || c->td.Format == D3DFMT_X8R8G8B8))
        logf("GAMECAP %s TEX samples=%08lx/%08lx/%08lx/%08lx", name,
             (unsigned long)c->tex_samples[0], (unsigned long)c->tex_samples[1],
             (unsigned long)c->tex_samples[2], (unsigned long)c->tex_samples[3]);
    game_make_replay_resources(d, c);
    logf("GAMECAP %s private VB create/lock/unlock=%08lx/%08lx/%08lx ptr=%p", name,
         (unsigned long)c->replay_vb_create_hr, (unsigned long)c->replay_vb_lock_hr,
         (unsigned long)c->replay_vb_unlock_hr, c->replay_vb);
    logf("GAMECAP %s private TEX create/lock/unlock=%08lx/%08lx/%08lx ptr=%p", name,
         (unsigned long)c->replay_tex_create_hr, (unsigned long)c->replay_tex_lock_hr,
         (unsigned long)c->replay_tex_unlock_hr, c->replay_tex);
    if (c->replay_tex)
        logf("GAMECAP %s replay TEX levels=%u lod=%lu", name,
             IDirect3DTexture9_GetLevelCount(c->replay_tex),
             (unsigned long)IDirect3DTexture9_GetLOD(c->replay_tex));
    game_log_matrix(name, "W", &c->world); game_log_matrix(name, "V", &c->view); game_log_matrix(name, "P", &c->projection);
    game_log_vertices(c, name); c->finalized = 1;
}

static int gamecap_same_draw(const GAMECAP *a, const GAMECAP *b)
{
    return a->valid && b->valid && a->tex == b->tex && a->vb == b->vb &&
           a->type == b->type && a->start_vertex == b->start_vertex && a->primitive_count == b->primitive_count;
}

static void game_capture_present(IDirect3DDevice9 *d)
{
    int stable = g_game_min_draws > 0 ? f_draws >= (unsigned long)g_game_min_draws :
                 (g_game_force ? f_draws != 0 : (f_draws == 32 && f_prims == 64));
    (void)d;
    if (!g_probe_enabled || g_game.done) return;
    if (g_game.collecting) {
        if (stable && (g_game.managed.valid || g_game.any.valid)) {
            logf("GAMECAP stable frame accepted frame=%lu draws=%lu prims=%lu", f_frames + 1, f_draws, f_prims);
            g_game.collecting = 0; g_game.done = 1; g_game.pending_finalize = 1;
            logf("GAMECAP run8 pending: offscreen ORIGINAL pass/readback will finish before READONLY snapshots/replay creation");
            return;
        }
        gamecap_clear(&g_game.managed); gamecap_clear(&g_game.any); g_game.collecting = 0;
        if (!stable) logf("GAMECAP candidate frame rejected draws=%lu prims=%lu", f_draws, f_prims);
    }
    if (stable) g_game.stable_frames++; else g_game.stable_frames = 0;
    if (g_game.stable_frames >= 2 && g_game.attempts < 3) {
        g_game.collecting = 1; g_game.attempts++;
        if (g_game_min_draws > 0) logf("GAMECAP MENU armed after %u frames with >=%d draws; collecting next frame",
                                      g_game.stable_frames, g_game_min_draws);
        else if (g_game_force) logf("GAMECAP FORCE armed after %u nonempty frames; collecting next frame", g_game.stable_frames);
        else logf("GAMECAP armed after %u stable 32/64 frames; collecting next frame (attempt %u)",
                  g_game.stable_frames, g_game.attempts);
    }
}

static void game_finalize_pending(IDirect3DDevice9 *d)
{
    if (!g_game.pending_finalize) return;
    if (g_game.managed.valid) game_finalize_slot(d, &g_game.managed, "MANAGED");
    if (g_game.any.valid) {
        if (gamecap_same_draw(&g_game.managed, &g_game.any)) {
            g_game.any.finalized = 1; logf("GAMECAP ANY aliases MANAGED (same texture/VB/draw)");
        } else game_finalize_slot(d, &g_game.any, "ANY");
    }
    g_game.pending_finalize = 0; g_game.just_finalized = 1;
    logf("GAMECAP legacy overlay legend: red shifted shape=ORIGINAL VB; lime shape=REPLAY VB");
    logf("GAMECAP legacy overlay row3: replay primary full/UV/alpha | replay ANY full");
    logf("GAMECAP legacy overlay row4: red-strip ORIGINAL primary | green-strip REPLAY primary | red-strip ORIGINAL ANY | green-strip REPLAY ANY");
}

static void game_identity(D3DMATRIX *m)
{
    ZeroMemory(m, sizeof(*m)); m->_11 = m->_22 = m->_33 = m->_44 = 1.0f;
}

static void game_xyz_quad(GAMEVERT *v, float x0, float y0, float x1, float y1, DWORD color)
{
    GAMEVERT a = { x0, y0, 0.5f, color, 0.0f, 0.0f };
    GAMEVERT b = { x1, y0, 0.5f, color, 1.0f, 0.0f };
    GAMEVERT c = { x0, y1, 0.5f, color, 0.0f, 1.0f };
    GAMEVERT e = { x1, y1, 0.5f, color, 1.0f, 1.0f };
    v[0] = a; v[1] = b; v[2] = c; v[3] = e;
}

static void game_probe_quad_uv(PROBEVERT *v, float x0, float y0, float x1, float y1,
                               float u0, float v0, float u1, float v1)
{
    PROBEVERT a = { x0 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, 0xffffffffu, u0, v0 };
    PROBEVERT b = { x1 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, 0xffffffffu, u1, v0 };
    PROBEVERT c = { x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, 0xffffffffu, u1, v1 };
    PROBEVERT e = { x0 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, 0xffffffffu, u0, v1 };
    v[0] = a; v[1] = b; v[2] = c; v[3] = a; v[4] = c; v[5] = e;
}

static int game_uv_bounds(const GAMECAP *c, float *u0, float *v0, float *u1, float *v1)
{
    UINT i;
    if (!c->decoded_count || !game_float_finite(c->vertices[0].u) || !game_float_finite(c->vertices[0].v)) return 0;
    *u0 = *u1 = c->vertices[0].u; *v0 = *v1 = c->vertices[0].v;
    for (i = 1; i < c->decoded_count; i++) {
        float u = c->vertices[i].u, v = c->vertices[i].v;
        if (!game_float_finite(u) || !game_float_finite(v)) return 0;
        if (u < *u0) *u0 = u; if (u > *u1) *u1 = u;
        if (v < *v0) *v0 = v; if (v > *v1) *v1 = v;
    }
    return 1;
}

static void game_probe_set_vertex(PROBEVERT *v, float x, float y, float u, float t)
{
    v->x = x - 0.5f; v->y = y - 0.5f; v->z = 0.0f; v->rhw = 1.0f;
    v->color = 0xffffffffu; v->u = u; v->v = t;
}

static int game_probe_actual_uv(const GAMECAP *c, PROBEVERT *out,
                                float x0, float y0, float x1, float y1)
{
    static const BYTE strip_map[6] = { 0, 1, 2, 2, 1, 3 };
    const BYTE *map = NULL; UINT i;
    float px[6] = { x0, x1, x0, x0, x1, x1 };
    float py[6] = { y0, y0, y1, y1, y0, y1 };
    if (c->type == D3DPT_TRIANGLESTRIP && c->decoded_count >= 4) map = strip_map;
    if (map) {
        for (i = 0; i < 6; i++) {
            const GAMEVERT *v = &c->vertices[map[i]];
            if (!game_float_finite(v->u) || !game_float_finite(v->v)) return 0;
            game_probe_set_vertex(&out[i], px[i], py[i], v->u, v->v);
        }
        return 1;
    }
    if (c->type == D3DPT_TRIANGLELIST && c->decoded_count >= 6) {
        for (i = 0; i < 6; i++) {
            const GAMEVERT *v = &c->vertices[i];
            if (!game_float_finite(v->u) || !game_float_finite(v->v)) return 0;
            game_probe_set_vertex(&out[i], px[i], py[i], v->u, v->v);
        }
        return 1;
    }
    return 0;
}

static GAMECAP *game_primary(void)
{
    if (g_game.managed.valid) return &g_game.managed;
    if (g_game.any.valid) return &g_game.any;
    return NULL;
}

static void run8_rects(UINT width, UINT height, RECT *primary, RECT *any, RECT *control)
{
    LONG tile_w = width > 640 ? 256 : (LONG)width / 5;
    LONG tile_h = height > 320 ? 128 : (LONG)height / 5;
    LONG gap = 16;
    if (tile_w < 32) tile_w = 32;
    if (tile_h < 24) tile_h = 24;
    primary->left = gap; primary->top = gap;
    primary->right = primary->left + tile_w; primary->bottom = primary->top + tile_h;
    any->left = primary->right + gap; any->top = gap;
    any->right = any->left + tile_w; any->bottom = any->top + tile_h;
    control->right = (LONG)width - gap; control->left = control->right - 96;
    control->top = gap; control->bottom = control->top + 64;
    if (primary->right > (LONG)width) primary->right = width;
    if (primary->bottom > (LONG)height) primary->bottom = height;
    if (any->right > (LONG)width) any->right = width;
    if (any->bottom > (LONG)height) any->bottom = height;
    if (control->left < 0) control->left = 0;
    if (control->bottom > (LONG)height) control->bottom = height;
}

static void run8_hash_region(const D3DLOCKED_RECT *lr, const RECT *r, DWORD clear_rgb,
                             DWORD *hash, UINT *nonclear)
{
    LONG x, y; DWORD h = 2166136261u; UINT n = 0;
    for (y = r->top; y < r->bottom; y++) {
        const DWORD *row = (const DWORD *)((const BYTE *)lr->pBits + y * lr->Pitch);
        for (x = r->left; x < r->right; x++) {
            DWORD rgb = row[x] & 0x00ffffffu;
            h = probe_hash_bytes(h, &rgb, sizeof(rgb));
            if (rgb != clear_rgb) n++;
        }
    }
    *hash = h; *nonclear = n;
}

static int run8_read_surface(IDirect3DSurface9 *sys, RUN8STATS *out)
{
    D3DSURFACE_DESC sd; D3DLOCKED_RECT lr; RECT primary, any, control;
    LONG x, y; DWORD rgb; HRESULT grtd_hr = out->grtd_hr;
    ZeroMemory(out, sizeof(*out)); out->grtd_hr = grtd_hr;
    out->lock_hr = out->unlock_hr = E_FAIL;
    if (FAILED(IDirect3DSurface9_GetDesc(sys, &sd))) return 0;
    if (sd.Format != D3DFMT_X8R8G8B8 && sd.Format != D3DFMT_A8R8G8B8) return 0;
    run8_rects(sd.Width, sd.Height, &primary, &any, &control);
    ZeroMemory(&lr, sizeof(lr));
    out->lock_hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(out->lock_hr) || !lr.pBits) return 0;
    run8_hash_region(&lr, &primary, RUN8_CLEAR_RGB, &out->primary_hash, &out->primary_nonclear);
    run8_hash_region(&lr, &any, RUN8_CLEAR_RGB, &out->any_hash, &out->any_nonclear);
    for (y = 0; y < (LONG)sd.Height; y++) {
        const DWORD *row = (const DWORD *)((const BYTE *)lr.pBits + y * lr.Pitch);
        for (x = 0; x < (LONG)sd.Width; x++) {
            rgb = row[x] & 0x00ffffffu;
            if (rgb == RUN8_VB_RGB) out->vb_exact++;
            if (rgb == RUN8_CTRL_RGB) out->control_exact++;
        }
    }
    out->unlock_hr = IDirect3DSurface9_UnlockRect(sys);
    out->valid = SUCCEEDED(out->unlock_hr);
    return out->valid;
}

static int run8_set_common_state(IDirect3DDevice9 *d, const D3DSURFACE_DESC *sd)
{
    D3DVIEWPORT9 vp; HRESULT hr; int ok = 1;
#define RUN8_STATE(call) do { hr = (call); if (FAILED(hr)) ok = 0; } while (0)
    vp.X = vp.Y = 0; vp.Width = sd->Width; vp.Height = sd->Height; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    RUN8_STATE(orig.SetViewport(d, &vp));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_ZENABLE, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_ZWRITEENABLE, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_FOGENABLE, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_STENCILENABLE, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_LIGHTING, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_FILLMODE, D3DFILL_SOLID));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_COLORWRITEENABLE, 0x0f));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_CLIPPING, TRUE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_CLIPPLANEENABLE, 0));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_VERTEXBLEND, D3DVBF_DISABLE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_WRAP0, 0));
    RUN8_STATE(orig.SetVertexShader(d, NULL)); RUN8_STATE(orig.SetPixelShader(d, NULL));
    RUN8_STATE(orig.SetTextureStageState(d, 1, D3DTSS_COLOROP, D3DTOP_DISABLE));
    RUN8_STATE(orig.SetSamplerState(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    RUN8_STATE(orig.SetSamplerState(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    RUN8_STATE(orig.SetSamplerState(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    RUN8_STATE(orig.SetSamplerState(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    RUN8_STATE(orig.SetSamplerState(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    RUN8_STATE(orig.SetSamplerState(d, 0, D3DSAMP_SRGBTEXTURE, FALSE));
    RUN8_STATE(orig.SetRenderState(d, D3DRS_SRGBWRITEENABLE, FALSE));
    RUN8_STATE(orig.SetTextureStageState(d, 0, D3DTSS_TEXCOORDINDEX, 0));
    RUN8_STATE(orig.SetTextureStageState(d, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
#undef RUN8_STATE
    return ok;
}

static int run8_draw_pass(IDirect3DDevice9 *d, IDirect3DSurface9 *rt,
                          IDirect3DSurface9 *sys, const D3DSURFACE_DESC *sd,
                          GAMECAP *p, GAMECAP *any, int replay, RUN8STATS *stats)
{
    RECT rp, ra, rc; D3DRECT clear_rects[2];
    PROBEVERT qp[6], qa[6], qc[6]; HRESULT hr, end_hr;
    IDirect3DTexture9 *pt, *at; IDirect3DVertexBuffer9 *vb;
    D3DVIEWPORT9 full_vp;
    UINT vb_offset, vb_start; int begun = 0, any_distinct;
    run8_rects(sd->Width, sd->Height, &rp, &ra, &rc);
    game_probe_quad_uv(qp, (float)rp.left, (float)rp.top, (float)rp.right, (float)rp.bottom,
                       0.0f, 0.0f, 1.0f, 1.0f);
    game_probe_quad_uv(qa, (float)ra.left, (float)ra.top, (float)ra.right, (float)ra.bottom,
                       0.0f, 0.0f, 1.0f, 1.0f);
    probe_quad(qc, (float)rc.left, (float)rc.top, (float)rc.right, (float)rc.bottom,
               0xff000000u | RUN8_CTRL_RGB);
    pt = replay ? p->replay_tex : p->tex;
    at = replay ? any->replay_tex : any->tex;
    vb = replay ? p->replay_vb : p->vb;
    vb_offset = replay ? 0 : p->stream_offset;
    vb_start = replay ? 0 : p->start_vertex;
    any_distinct = any->valid && any->tex != p->tex;
    full_vp.X = full_vp.Y = 0; full_vp.Width = sd->Width; full_vp.Height = sd->Height;
    full_vp.MinZ = 0.0f; full_vp.MaxZ = 1.0f;
    clear_rects[0].x1 = rp.left; clear_rects[0].y1 = rp.top;
    clear_rects[0].x2 = rp.right; clear_rects[0].y2 = rp.bottom;
    clear_rects[1].x1 = ra.left; clear_rects[1].y1 = ra.top;
    clear_rects[1].x2 = ra.right; clear_rects[1].y2 = ra.bottom;

    hr = orig.SetRenderTarget(d, 0, rt); if (FAILED(hr)) goto done;
    orig.SetDepthStencilSurface(d, NULL);
    if (!run8_set_common_state(d, sd)) goto done;
    hr = orig.BeginScene(d); if (FAILED(hr)) goto done; begun = 1;
    hr = orig.Clear(d, 0, NULL, D3DCLEAR_TARGET, 0xff000000u | RUN8_CLEAR_RGB, 1.0f, 0);
    if (FAILED(hr)) goto done;

    /* Draw the retained/shared VB first. Texture tiles drawn later cannot be contaminated by it. */
    if (vb && (p->state_ok & (GAME_OK_WORLD|GAME_OK_VIEW|GAME_OK_PROJECTION)) ==
              (GAME_OK_WORLD|GAME_OK_VIEW|GAME_OK_PROJECTION)) {
        if (p->state_ok & GAME_OK_VIEWPORT) orig.SetViewport(d, &p->viewport);
        orig.SetFVF(d, GAME_FVF);
        orig.SetStreamSourceFreq(d, 0, (p->state_ok & GAME_OK_FREQUENCY) ? p->stream_freq : 1);
        orig.SetStreamSource(d, 0, vb, vb_offset, p->stride);
        orig.SetTransform(d, D3DTS_WORLD, &p->world); orig.SetTransform(d, D3DTS_VIEW, &p->view);
        orig.SetTransform(d, D3DTS_PROJECTION, &p->projection);
        orig.SetTexture(d, 0, NULL);
        orig.SetRenderState(d, D3DRS_TEXTUREFACTOR, 0xff000000u | RUN8_VB_RGB);
        orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        hr = orig.DrawPrimitive(d, p->type, vb_start, p->primitive_count);
        logf("RUN8 %s VB draw -> %08lx", replay ? "REPLAY" : "ORIGINAL", (unsigned long)hr);
    }

    orig.SetViewport(d, &full_vp);
    /* Remove any VB pixels below the texture tiles so a missing texture draw remains the
       exact clear color instead of being misclassified from unrelated geometry. */
    hr = orig.Clear(d, any_distinct ? 2 : 1, clear_rects, D3DCLEAR_TARGET,
                    0xff000000u | RUN8_CLEAR_RGB, 1.0f, 0);
    if (FAILED(hr)) {
        logf("RUN8 %s tile Clear failed -> %08lx",
             replay ? "REPLAY" : "ORIGINAL", (unsigned long)hr);
        goto done;
    }
    orig.SetFVF(d, PROBE_FVF);
    orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    if (pt) {
        orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)pt);
        hr = orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, qp, sizeof(PROBEVERT));
        logf("RUN8 %s primary texture draw -> %08lx", replay ? "REPLAY" : "ORIGINAL", (unsigned long)hr);
    }
    if (any_distinct && at) {
        orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)at);
        hr = orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, qa, sizeof(PROBEVERT));
        logf("RUN8 %s ANY texture draw -> %08lx", replay ? "REPLAY" : "ORIGINAL", (unsigned long)hr);
    }
    orig.SetTexture(d, 0, NULL);
    orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    hr = orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, qc, sizeof(PROBEVERT));
    logf("RUN8 %s control DPUP -> %08lx", replay ? "REPLAY" : "ORIGINAL", (unsigned long)hr);

done:
    end_hr = begun ? orig.EndScene(d) : E_FAIL;
    if (begun && FAILED(end_hr)) logf("RUN8 %s EndScene failed -> %08lx", replay ? "REPLAY" : "ORIGINAL", (unsigned long)end_hr);
    ZeroMemory(stats, sizeof(*stats)); stats->grtd_hr = E_FAIL;
    if (!begun || FAILED(end_hr)) return 0;
    stats->grtd_hr = orig.GetRenderTargetData(d, rt, sys);
    if (FAILED(stats->grtd_hr)) {
        logf("RUN8 %s GetRenderTargetData failed -> %08lx", replay ? "REPLAY" : "ORIGINAL", (unsigned long)stats->grtd_hr);
        return 0;
    }
    if (!run8_read_surface(sys, stats)) {
        logf("RUN8 %s systemmem readback failed Lock=%08lx Unlock=%08lx", replay ? "REPLAY" : "ORIGINAL",
             (unsigned long)stats->lock_hr, (unsigned long)stats->unlock_hr);
        return 0;
    }
    logf("RUN8 %s readback primary hash=%08lx nonclear=%u ANY hash=%08lx nonclear=%u vb_exact=%u control_exact=%u",
         replay ? "REPLAY" : "ORIGINAL", (unsigned long)stats->primary_hash, stats->primary_nonclear,
         (unsigned long)stats->any_hash, stats->any_nonclear, stats->vb_exact, stats->control_exact);
    return 1;
}

static const char *run8_verdict_name(int verdict)
{
    return verdict > 0 ? "OK" : verdict < 0 ? "BAD" : "INCONCLUSIVE";
}

static void run8_selftest(IDirect3DDevice9 *d, const D3DSURFACE_DESC *bb)
{
    IDirect3DSurface9 *rt = NULL, *sys = NULL; GAMECAP *p = game_primary(), *any = &g_game.any;
    D3DSURFACE_DESC sd; HRESULT hr;
    int original_ok, replay_ok, any_distinct, textures_comparable, texture_ok;
    if (g_run8.attempted || !g_game.pending_finalize || !p || !bb) return;
    g_run8.attempted = 1;
    ZeroMemory(&sd, sizeof(sd)); sd = *bb;
    if (sd.Format != D3DFMT_X8R8G8B8 && sd.Format != D3DFMT_A8R8G8B8) sd.Format = D3DFMT_X8R8G8B8;
    logf("RUN8 menu selftest begin frame=%lu draws=%lu prims=%lu size=%ux%u %s (ORIGINAL pass precedes all READONLY locks)",
         f_frames + 1, f_draws, f_prims, sd.Width, sd.Height, fmtname(sd.Format));
    hr = orig.CreateRenderTarget(d, sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    logf("RUN8 CreateRenderTarget -> %08lx %p", (unsigned long)hr, rt);
    if (FAILED(hr) || !rt) goto cleanup;
    hr = orig.CreateOffscreenPlainSurface(d, sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    logf("RUN8 CreateOffscreenPlainSurface -> %08lx %p", (unsigned long)hr, sys);
    if (FAILED(hr) || !sys) goto cleanup;

    original_ok = run8_draw_pass(d, rt, sys, &sd, p, any, 0, &g_run8.original);
    logf("RUN8 ORIGINAL pass complete=%d; forcing snapshot/replay only now", original_ok);
    game_finalize_pending(d);
    replay_ok = run8_draw_pass(d, rt, sys, &sd, p, any, 1, &g_run8.replay);
    any_distinct = any->valid && any->tex != p->tex;
    textures_comparable = p->replay_tex && (!any_distinct || any->replay_tex);

    if (original_ok && replay_ok && g_run8.original.control_exact && g_run8.replay.control_exact) {
        if (textures_comparable) {
            texture_ok = g_run8.replay.primary_nonclear &&
                         g_run8.original.primary_nonclear == g_run8.replay.primary_nonclear &&
                         g_run8.original.primary_hash == g_run8.replay.primary_hash;
            if (any_distinct)
                texture_ok = texture_ok && g_run8.replay.any_nonclear &&
                             g_run8.original.any_nonclear == g_run8.replay.any_nonclear &&
                             g_run8.original.any_hash == g_run8.replay.any_hash;
            if (texture_ok) g_run8.texture_verdict = 1;
            else if (g_run8.replay.primary_nonclear || (any_distinct && g_run8.replay.any_nonclear))
                g_run8.texture_verdict = -1;
        }
        if (g_run8.replay.vb_exact && g_run8.original.vb_exact == g_run8.replay.vb_exact)
            g_run8.vb_verdict = 1;
        else if (g_run8.replay.vb_exact && g_run8.original.vb_exact != g_run8.replay.vb_exact)
            g_run8.vb_verdict = -1;
    }
    g_run8.complete = original_ok && replay_ok;
    logf("RUN8 VERDICT texture=%s vb=%s original_control=%u replay_control=%u",
         run8_verdict_name(g_run8.texture_verdict), run8_verdict_name(g_run8.vb_verdict),
         g_run8.original.control_exact, g_run8.replay.control_exact);
    if (g_run8.texture_verdict < 0) {
        g_run8.fix_texture = 1;
        logf("RUN8 FIX enabling exact captured-texture replay substitution");
    } else if (g_run8.texture_verdict > 0 && g_run8.vb_verdict < 0) {
        g_run8.fix_vb = 1;
        logf("RUN8 FIX enabling diagnostic DrawPrimitive-to-DrawPrimitiveUP VB bypass");
    }

cleanup:
    orig.SetRenderTarget(d, 0, g_backbuffer);
    if (sys) IDirect3DSurface9_Release(sys);
    if (rt) IDirect3DSurface9_Release(rt);
    if (g_game.pending_finalize) game_finalize_pending(d);
}

static void game_draw_overlays(IDirect3DDevice9 *d, const D3DSURFACE_DESC *bb, int verbose)
{
    GAMECAP *p = game_primary(), *any = &g_game.any; HRESULT hr; int note, any_distinct, was_pending;
    IDirect3DVertexBuffer9 *draw_vb;
    IDirect3DTexture9 *draw_tex, *any_tex, *original_tex, *original_any_tex;
    UINT draw_offset, draw_start;
    GAMEVERT xyz[4]; PROBEVERT full[6], crop[6], alpha[6], any_full[6];
    PROBEVERT compare[4][6], marker[4][6];
    D3DMATRIX identity, ortho, shifted_world; D3DVIEWPORT9 full_vp;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (!g_game.done || !p || !bb->Width || !bb->Height) return;
    was_pending = g_game.pending_finalize;
    note = verbose || was_pending || g_game.just_finalized;
#define GAME_DRAW_CALL(label, call) do { hr = (call); if (note || (FAILED(hr) && g_game.draw_fail_logs++ < 16)) logf("PROBE %s -> %08lx", (label), (unsigned long)hr); } while (0)
    full_vp.X = full_vp.Y = 0; full_vp.Width = bb->Width; full_vp.Height = bb->Height;
    full_vp.MinZ = 0.0f; full_vp.MaxZ = 1.0f;
    original_tex = p->tex; original_any_tex = any->tex;
    any_distinct = any->valid && any->tex != p->tex;

    game_probe_quad_uv(compare[0], 18.0f, 153.0f, 94.0f, 195.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    game_probe_quad_uv(compare[1], 101.0f, 153.0f, 177.0f, 195.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    game_probe_quad_uv(compare[2], 184.0f, 153.0f, 260.0f, 195.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    game_probe_quad_uv(compare[3], 267.0f, 153.0f, 343.0f, 195.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    probe_quad(marker[0], 18.0f, 147.0f, 94.0f, 151.0f, 0xffff2040u);
    probe_quad(marker[1], 101.0f, 147.0f, 177.0f, 151.0f, 0xff20ff40u);
    probe_quad(marker[2], 184.0f, 147.0f, 260.0f, 151.0f, 0xffff2040u);
    probe_quad(marker[3], 267.0f, 147.0f, 343.0f, 151.0f, 0xff20ff40u);

    /* First submit the retained ORIGINAL objects, before either diagnostic READONLY lock.
       At Present the shared dynamic VB contains the last quad written by the game.  The
       immediately following snapshot copies those same current bytes into the replay VB. */
    if (was_pending) logf("GAMECAP legacy ORIGINAL pre-lock overlay begin");
    shifted_world = p->world; shifted_world._41 += 400.0f;
    if (p->vb && (p->state_ok & (GAME_OK_WORLD|GAME_OK_VIEW|GAME_OK_PROJECTION)) ==
                 (GAME_OK_WORLD|GAME_OK_VIEW|GAME_OK_PROJECTION)) {
        if (p->state_ok & GAME_OK_VIEWPORT) GAME_DRAW_CALL("GAME ORIGINAL PRELOCK SetViewport", orig.SetViewport(d, &p->viewport));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK FVF 0x142", orig.SetFVF(d, GAME_FVF));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK StreamFreq 1", orig.SetStreamSourceFreq(d, 0, 1));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK StreamSource", orig.SetStreamSource(d, 0, p->vb, p->stream_offset, p->stride));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK World shifted +400", orig.SetTransform(d, D3DTS_WORLD, &shifted_world));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK View", orig.SetTransform(d, D3DTS_VIEW, &p->view));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK Projection", orig.SetTransform(d, D3DTS_PROJECTION, &p->projection));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK texture NULL", orig.SetTexture(d, 0, NULL));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK factor red", orig.SetRenderState(d, D3DRS_TEXTUREFACTOR, 0xffff2040u));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK COLOROP", orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK COLORARG1", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK ALPHAOP", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK ALPHAARG1", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
        GAME_DRAW_CALL("GAME ORIGINAL PRELOCK VB red shifted", orig.DrawPrimitive(d, p->type, p->start_vertex, p->primitive_count));
    } else if (note) logf("PROBE GAME ORIGINAL PRELOCK VB skipped (object/WVP unavailable)");

    /* Row 4: red marker = ORIGINAL, green marker = replay.  Submit originals now;
       their matching replay tiles are submitted after snapshot/copy below. */
    GAME_DRAW_CALL("GAME compare SetViewport full", orig.SetViewport(d, &full_vp));
    GAME_DRAW_CALL("GAME compare FVF XYZRHW", orig.SetFVF(d, PROBE_FVF));
    GAME_DRAW_CALL("GAME compare marker texture NULL", orig.SetTexture(d, 0, NULL));
    GAME_DRAW_CALL("GAME compare marker COLOROP", orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    GAME_DRAW_CALL("GAME compare marker COLORARG1", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
    GAME_DRAW_CALL("GAME compare marker ALPHAOP", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    GAME_DRAW_CALL("GAME compare marker ALPHAARG1", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
    GAME_DRAW_CALL("GAME compare marker ORIGINAL primary red", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, marker[0], sizeof(PROBEVERT)));
    GAME_DRAW_CALL("GAME compare marker REPLAY primary green", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, marker[1], sizeof(PROBEVERT)));
    if (any_distinct) {
        GAME_DRAW_CALL("GAME compare marker ORIGINAL ANY red", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, marker[2], sizeof(PROBEVERT)));
        GAME_DRAW_CALL("GAME compare marker REPLAY ANY green", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, marker[3], sizeof(PROBEVERT)));
    }
    GAME_DRAW_CALL("GAME compare texture COLOROP", orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    GAME_DRAW_CALL("GAME compare texture COLORARG1", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    GAME_DRAW_CALL("GAME compare texture ALPHAOP", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    GAME_DRAW_CALL("GAME compare texture ALPHAARG1", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
    if (original_tex) {
        GAME_DRAW_CALL("GAME compare ORIGINAL primary SetTexture PRELOCK", orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)original_tex));
        GAME_DRAW_CALL("GAME compare ORIGINAL primary full PRELOCK", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, compare[0], sizeof(PROBEVERT)));
    }
    if (any_distinct && original_any_tex) {
        GAME_DRAW_CALL("GAME compare ORIGINAL ANY SetTexture PRELOCK", orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)original_any_tex));
        GAME_DRAW_CALL("GAME compare ORIGINAL ANY full PRELOCK", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, compare[2], sizeof(PROBEVERT)));
    }
    if (was_pending) logf("GAMECAP legacy ORIGINAL pre-lock overlay submitted; snapshots begin now");

    game_finalize_pending(d);
    draw_vb = p->replay_vb; draw_offset = 0; draw_start = 0;
    draw_tex = p->replay_tex; any_tex = any->replay_tex;

    /* Replay the exact game VB/WVP, but remove texture, alpha, depth and culling as causes. */
    if (draw_vb && (p->state_ok & (GAME_OK_WORLD|GAME_OK_VIEW|GAME_OK_PROJECTION)) ==
                 (GAME_OK_WORLD|GAME_OK_VIEW|GAME_OK_PROJECTION)) {
        if (p->state_ok & GAME_OK_VIEWPORT) GAME_DRAW_CALL("GAME actual SetViewport", orig.SetViewport(d, &p->viewport));
        GAME_DRAW_CALL("GAME actual FVF 0x142", orig.SetFVF(d, GAME_FVF));
        GAME_DRAW_CALL("GAME actual StreamFreq 1", orig.SetStreamSourceFreq(d, 0, 1));
        GAME_DRAW_CALL("GAME actual StreamSource", orig.SetStreamSource(d, 0, draw_vb, draw_offset, p->stride));
        GAME_DRAW_CALL("GAME actual World", orig.SetTransform(d, D3DTS_WORLD, &p->world));
        GAME_DRAW_CALL("GAME actual View", orig.SetTransform(d, D3DTS_VIEW, &p->view));
        GAME_DRAW_CALL("GAME actual Projection", orig.SetTransform(d, D3DTS_PROJECTION, &p->projection));
        GAME_DRAW_CALL("GAME actual texture NULL", orig.SetTexture(d, 0, NULL));
        GAME_DRAW_CALL("GAME actual texture factor lime", orig.SetRenderState(d, D3DRS_TEXTUREFACTOR, 0xff80ff20u));
        GAME_DRAW_CALL("GAME actual COLOROP", orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
        GAME_DRAW_CALL("GAME actual COLORARG1", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
        GAME_DRAW_CALL("GAME actual ALPHAOP", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
        GAME_DRAW_CALL("GAME actual ALPHAARG1", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
        GAME_DRAW_CALL("GAME actual CLIPPING TRUE", orig.SetRenderState(d, D3DRS_CLIPPING, TRUE));
        GAME_DRAW_CALL("GAME actual CLIPPLANES 0", orig.SetRenderState(d, D3DRS_CLIPPLANEENABLE, 0));
        GAME_DRAW_CALL("GAME actual VB forced lime", orig.DrawPrimitive(d, p->type, draw_start, p->primitive_count));
    } else if (note) logf("PROBE GAME actual VB replay skipped (private snapshot/WVP unavailable)");

    /* Known non-pretransformed path: orange proves XYZ + W/V/P works independently. */
    game_identity(&identity); ZeroMemory(&ortho, sizeof(ortho));
    ortho._11 = 2.0f / bb->Width; ortho._22 = -2.0f / bb->Height; ortho._33 = 1.0f;
    ortho._41 = -1.0f; ortho._42 = 1.0f; ortho._44 = 1.0f;
    game_xyz_quad(xyz, 267.0f, 18.0f, 343.0f, 52.0f, 0xffff8010u);
    GAME_DRAW_CALL("GAME known SetViewport full", orig.SetViewport(d, &full_vp));
    GAME_DRAW_CALL("GAME known FVF XYZ", orig.SetFVF(d, GAME_FVF));
    GAME_DRAW_CALL("GAME known World identity", orig.SetTransform(d, D3DTS_WORLD, &identity));
    GAME_DRAW_CALL("GAME known View identity", orig.SetTransform(d, D3DTS_VIEW, &identity));
    GAME_DRAW_CALL("GAME known Projection ortho", orig.SetTransform(d, D3DTS_PROJECTION, &ortho));
    GAME_DRAW_CALL("GAME known CLIPPING TRUE", orig.SetRenderState(d, D3DRS_CLIPPING, TRUE));
    GAME_DRAW_CALL("GAME known CLIPPLANES 0", orig.SetRenderState(d, D3DRS_CLIPPLANEENABLE, 0));
    GAME_DRAW_CALL("GAME known texture NULL", orig.SetTexture(d, 0, NULL));
    GAME_DRAW_CALL("GAME known factor orange", orig.SetRenderState(d, D3DRS_TEXTUREFACTOR, 0xffff8010u));
    GAME_DRAW_CALL("GAME known COLOROP", orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    GAME_DRAW_CALL("GAME known COLORARG1", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
    GAME_DRAW_CALL("GAME known ALPHAOP", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    GAME_DRAW_CALL("GAME known ALPHAARG1", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
    GAME_DRAW_CALL("GAME known XYZ ortho orange", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLESTRIP, 2, xyz, sizeof(GAMEVERT)));

    /* Texture content checks use screen-space vertices so transforms cannot affect them. */
    game_probe_quad_uv(full, 18.0f, 102.0f, 94.0f, 144.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    if (!game_probe_actual_uv(p, crop, 101.0f, 102.0f, 177.0f, 144.0f)) {
        if (!game_uv_bounds(p, &u0, &v0, &u1, &v1) && note)
            logf("PROBE GAME UV crop unavailable; using full 0..1 range");
        game_probe_quad_uv(crop, 101.0f, 102.0f, 177.0f, 144.0f, u0, v0, u1, v1);
    }
    CopyMemory(alpha, crop, sizeof(alpha));
    { UINT ai; for (ai = 0; ai < 6; ai++) alpha[ai].x += 83.0f; }
    game_probe_quad_uv(any_full, 267.0f, 102.0f, 343.0f, 144.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    if (draw_tex || (any_distinct && any_tex)) {
        GAME_DRAW_CALL("GAME texture FVF XYZRHW", orig.SetFVF(d, PROBE_FVF));
        GAME_DRAW_CALL("GAME texture COLOROP", orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
        GAME_DRAW_CALL("GAME texture COLORARG1", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
        GAME_DRAW_CALL("GAME texture ALPHAOP", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
        GAME_DRAW_CALL("GAME texture ALPHAARG1", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
    }
    if (draw_tex) {
        GAME_DRAW_CALL("GAME texture primary", orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)draw_tex));
        GAME_DRAW_CALL("GAME texture full UV", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, full, sizeof(PROBEVERT)));
        GAME_DRAW_CALL("GAME texture UV crop", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, crop, sizeof(PROBEVERT)));
        GAME_DRAW_CALL("GAME texture alpha replicate", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE));
        GAME_DRAW_CALL("GAME texture alpha-as-RGB", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, alpha, sizeof(PROBEVERT)));
        GAME_DRAW_CALL("GAME texture RGB restore", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    } else if (note) logf("PROBE GAME primary texture tiles skipped (private snapshot unavailable)");
    if (any_distinct) {
        if (any_tex) {
            GAME_DRAW_CALL("GAME texture ANY", orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)any_tex));
            GAME_DRAW_CALL("GAME texture ANY full UV", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, any_full, sizeof(PROBEVERT)));
        } else if (note) logf("PROBE GAME ANY texture tile skipped (private snapshot unavailable)");
    }

    /* Complete row 4 with the freshly snapshotted replay resources. */
    if (draw_tex) {
        GAME_DRAW_CALL("GAME compare REPLAY primary SetTexture", orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)draw_tex));
        GAME_DRAW_CALL("GAME compare REPLAY primary full", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, compare[1], sizeof(PROBEVERT)));
    }
    if (any_distinct && any_tex) {
        GAME_DRAW_CALL("GAME compare REPLAY ANY SetTexture", orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)any_tex));
        GAME_DRAW_CALL("GAME compare REPLAY ANY full", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, compare[3], sizeof(PROBEVERT)));
    }
    GAME_DRAW_CALL("GAME finish SetViewport full", orig.SetViewport(d, &full_vp));
#undef GAME_DRAW_CALL
}

static void probe_release(void)
{
    if (g_probe.state) IDirect3DStateBlock9_Release(g_probe.state);
    if (g_probe.managed_vb) IDirect3DVertexBuffer9_Release(g_probe.managed_vb);
    if (g_probe.dynamic_vb) IDirect3DVertexBuffer9_Release(g_probe.dynamic_vb);
    if (g_probe.rename_vb) IDirect3DVertexBuffer9_Release(g_probe.rename_vb);
    if (g_probe.managed_tex) IDirect3DTexture9_Release(g_probe.managed_tex);
    if (g_probe.dynamic_tex) IDirect3DTexture9_Release(g_probe.dynamic_tex);
    if (g_probe.upload_src) IDirect3DTexture9_Release(g_probe.upload_src);
    if (g_probe.upload_dst) IDirect3DTexture9_Release(g_probe.upload_dst);
    ZeroMemory(&g_probe, sizeof(g_probe));
    game_release_all();
    ZeroMemory(&g_run8, sizeof(g_run8));
}
static void probe_desc(IDirect3DTexture9 *tex, const char *name)
{
    D3DSURFACE_DESC sd; HRESULT hr;
    if (!tex) return;
    ZeroMemory(&sd, sizeof(sd)); hr = IDirect3DTexture9_GetLevelDesc(tex, 0, &sd);
    if (SUCCEEDED(hr)) logf("PROBE %s GetLevelDesc -> %08lx %ux%u %s usage=%lx pool=%d", name,
        (unsigned long)hr, sd.Width, sd.Height, fmtname(sd.Format), (unsigned long)sd.Usage, sd.Pool);
    else probe_note(name, hr, 1);
}
static void probe_init(IDirect3DDevice9 *d)
{
    HRESULT hr; const float x = 18.0f, y = 18.0f, cw = 76.0f, ch = 34.0f, gap = 7.0f;
    D3DVERTEXBUFFER_DESC vd;
    if (!g_probe_enabled || g_probe.attempted) return;
    g_probe.attempted = 1; g_probe.first_draw = 1;
    probe_quad(g_probe.up,      x,                y, x + cw,                y + ch, 0xffff2020u);
    probe_quad(g_probe.managed, x + cw + gap,     y, x + 2*cw + gap,       y + ch, 0xff20ff20u);
    probe_quad(g_probe.dynamic, x + 2*(cw + gap), y, x + 3*cw + 2*gap,     y + ch, 0xff2080ffu);
    probe_quad(g_probe.tex[0],  x,           y+ch+gap, x + cw,           y + 2*ch + gap, 0xffffffffu);
    probe_quad(g_probe.tex[1],  x+cw+gap,    y+ch+gap, x + 2*cw + gap,  y + 2*ch + gap, 0xffffffffu);
    probe_quad(g_probe.tex[2],  x+2*(cw+gap),y+ch+gap, x + 3*cw + 2*gap,y + 2*ch + gap, 0xffffffffu);
    probe_quad(g_probe.rename[0], 18.0f, 204.0f, 94.0f, 238.0f, 0xffff4040u);
    probe_quad(g_probe.rename[1], 101.0f, 204.0f, 177.0f, 238.0f, 0xff40ffffu);
    logf("PROBE legend top=red DPUP | green MANAGED-VB | blue DYNAMIC-VB; bottom=yellow MANAGED-TEX | magenta DYNAMIC-TEX | cyan UpdateTexture");
    logf("PROBE row5 rename: red A then cyan B, same DYNAMIC|WRITEONLY DEFAULT VB, two DISCARD locks in one scene");

    hr = orig.CreateStateBlock(d, D3DSBT_ALL, &g_probe.state); probe_note("CreateStateBlock ALL", hr, 1);
    hr = orig.CreateVertexBuffer(d, sizeof(g_probe.managed), 0, PROBE_FVF, D3DPOOL_MANAGED, &g_probe.managed_vb, NULL);
    probe_note("CreateVertexBuffer MANAGED", hr, 1);
    if (SUCCEEDED(hr)) {
        probe_fill_vb(g_probe.managed_vb, g_probe.managed, 0, "MANAGED-VB Lock", 1);
        ZeroMemory(&vd, sizeof(vd)); hr = IDirect3DVertexBuffer9_GetDesc(g_probe.managed_vb, &vd);
        if (SUCCEEDED(hr)) logf("PROBE MANAGED-VB GetDesc -> %08lx size=%u usage=%lx fvf=%lx pool=%d", (unsigned long)hr, vd.Size, (unsigned long)vd.Usage, (unsigned long)vd.FVF, vd.Pool);
        else probe_note("MANAGED-VB GetDesc", hr, 1);
        probe_read_vb(g_probe.managed_vb, "MANAGED-VB Lock READONLY");
    }
    hr = orig.CreateVertexBuffer(d, sizeof(g_probe.dynamic), D3DUSAGE_DYNAMIC, PROBE_FVF, D3DPOOL_DEFAULT, &g_probe.dynamic_vb, NULL);
    probe_note("CreateVertexBuffer DEFAULT|DYNAMIC", hr, 1);
    if (SUCCEEDED(hr)) {
        ZeroMemory(&vd, sizeof(vd)); hr = IDirect3DVertexBuffer9_GetDesc(g_probe.dynamic_vb, &vd);
        if (SUCCEEDED(hr)) logf("PROBE DYNAMIC-VB GetDesc -> %08lx size=%u usage=%lx fvf=%lx pool=%d", (unsigned long)hr, vd.Size, (unsigned long)vd.Usage, (unsigned long)vd.FVF, vd.Pool);
        else probe_note("DYNAMIC-VB GetDesc", hr, 1);
    }
    hr = orig.CreateVertexBuffer(d, sizeof(g_probe.rename[0]), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                 PROBE_FVF, D3DPOOL_DEFAULT, &g_probe.rename_vb, NULL);
    probe_note("CreateVertexBuffer RENAME DEFAULT|DYNAMIC|WRITEONLY", hr, 1);
    if (SUCCEEDED(hr)) {
        ZeroMemory(&vd, sizeof(vd)); hr = IDirect3DVertexBuffer9_GetDesc(g_probe.rename_vb, &vd);
        if (SUCCEEDED(hr)) logf("PROBE RENAME-VB GetDesc -> %08lx size=%u usage=%lx fvf=%lx pool=%d",
                                (unsigned long)hr, vd.Size, (unsigned long)vd.Usage,
                                (unsigned long)vd.FVF, vd.Pool);
        else probe_note("RENAME-VB GetDesc", hr, 1);
    }

    hr = orig.CreateTexture(d, PROBE_TEX_SIZE, PROBE_TEX_SIZE, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_probe.managed_tex, NULL);
    probe_note("CreateTexture MANAGED", hr, 1); probe_desc(g_probe.managed_tex, "MANAGED-TEX");
    if (SUCCEEDED(hr)) { probe_fill_tex(g_probe.managed_tex, 0xffffff00u, 0xff706000u, 0, "MANAGED-TEX LockRect", 1); probe_read_tex(g_probe.managed_tex, "MANAGED-TEX LockRect READONLY"); }
    hr = orig.CreateTexture(d, PROBE_TEX_SIZE, PROBE_TEX_SIZE, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_probe.dynamic_tex, NULL);
    probe_note("CreateTexture DEFAULT|DYNAMIC", hr, 1); probe_desc(g_probe.dynamic_tex, "DYNAMIC-TEX");
    hr = orig.CreateTexture(d, PROBE_TEX_SIZE, PROBE_TEX_SIZE, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_probe.upload_src, NULL);
    probe_note("CreateTexture SYSTEMMEM source", hr, 1); probe_desc(g_probe.upload_src, "UPLOAD-SRC");
    if (SUCCEEDED(hr)) { probe_fill_tex(g_probe.upload_src, 0xff20ffffu, 0xff006070u, 0, "UPLOAD-SRC LockRect", 1); probe_read_tex(g_probe.upload_src, "UPLOAD-SRC LockRect READONLY"); }
    hr = orig.CreateTexture(d, PROBE_TEX_SIZE, PROBE_TEX_SIZE, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_probe.upload_dst, NULL);
    probe_note("CreateTexture DEFAULT destination", hr, 1); probe_desc(g_probe.upload_dst, "UPLOAD-DST");
    /* This read lock is expected to fail on a native DEFAULT non-dynamic texture.  Logging it
       distinguishes 'not CPU-lockable by design' from a failed visual UpdateTexture result. */
    if (g_probe.upload_dst) probe_read_tex(g_probe.upload_dst, "UPLOAD-DST LockRect READONLY (expected unavailable)");
    g_probe.ready = g_probe.state != NULL;
    logf("PROBE init complete: state=%p mvb=%p dvb=%p rvb=%p mtex=%p dtex=%p src=%p dst=%p ready=%d",
         g_probe.state, g_probe.managed_vb, g_probe.dynamic_vb, g_probe.rename_vb, g_probe.managed_tex,
         g_probe.dynamic_tex, g_probe.upload_src, g_probe.upload_dst, g_probe.ready);
}
static void probe_dynamic_uploads(IDirect3DDevice9 *d, int verbose)
{
    HRESULT hr;
    if (g_probe.dynamic_vb) probe_fill_vb(g_probe.dynamic_vb, g_probe.dynamic, D3DLOCK_DISCARD, "DYNAMIC-VB Lock DISCARD", verbose);
    if (g_probe.dynamic_tex) probe_fill_tex(g_probe.dynamic_tex, 0xffff20ffu, 0xff700060u, D3DLOCK_DISCARD, "DYNAMIC-TEX LockRect DISCARD", verbose);
    if (verbose && g_probe.dynamic_vb) probe_read_vb(g_probe.dynamic_vb, "DYNAMIC-VB Lock READONLY diagnostic");
    if (verbose && g_probe.dynamic_tex) probe_read_tex(g_probe.dynamic_tex, "DYNAMIC-TEX LockRect READONLY diagnostic");
    if (g_probe.upload_src && g_probe.upload_dst) {
        hr = orig.UpdateTexture(d, (IDirect3DBaseTexture9 *)g_probe.upload_src, (IDirect3DBaseTexture9 *)g_probe.upload_dst);
        probe_note("UpdateTexture SYSTEMMEM->DEFAULT", hr, verbose);
    }
}

static HRESULT probe_rename_upload(IDirect3DVertexBuffer9 *vb, const PROBEVERT *src,
                                   const char *lock_name, const char *unlock_name, int verbose)
{
    void *bits = NULL; HRESULT hr, uhr; DWORD hash;
    hr = IDirect3DVertexBuffer9_Lock(vb, 0, 6 * sizeof(*src), &bits, D3DLOCK_DISCARD);
    probe_note(lock_name, hr, verbose);
    if (FAILED(hr) || !bits) return hr;
    CopyMemory(bits, src, 6 * sizeof(*src));
    hash = probe_hash_bytes(2166136261u, bits, 6 * sizeof(*src));
    if (verbose) logf("PROBE %s write hash=%08lx first=(%ld,%ld,%08lx)", lock_name,
                      (unsigned long)hash, (long)src[0].x, (long)src[0].y,
                      (unsigned long)src[0].color);
    uhr = IDirect3DVertexBuffer9_Unlock(vb); probe_note(unlock_name, uhr, verbose);
    return uhr;
}

static void probe_draw_rename(IDirect3DDevice9 *d, int verbose)
{
    HRESULT hr;
    if (!g_probe.rename_vb) return;
    hr = probe_rename_upload(g_probe.rename_vb, g_probe.rename[0],
                             "RENAME-A Lock DISCARD", "RENAME-A Unlock", verbose);
    if (SUCCEEDED(hr)) {
        hr = orig.SetStreamSource(d, 0, g_probe.rename_vb, 0, sizeof(PROBEVERT));
        probe_note("RENAME-A SetStreamSource", hr, verbose);
        if (SUCCEEDED(hr)) {
            hr = orig.DrawPrimitive(d, D3DPT_TRIANGLELIST, 0, 2);
            probe_note("RENAME-A draw red", hr, verbose);
        }
    }
    hr = probe_rename_upload(g_probe.rename_vb, g_probe.rename[1],
                             "RENAME-B Lock DISCARD", "RENAME-B Unlock", verbose);
    if (SUCCEEDED(hr)) {
        hr = orig.SetStreamSource(d, 0, g_probe.rename_vb, 0, sizeof(PROBEVERT));
        probe_note("RENAME-B SetStreamSource", hr, verbose);
        if (SUCCEEDED(hr)) {
            hr = orig.DrawPrimitive(d, D3DPT_TRIANGLELIST, 0, 2);
            probe_note("RENAME-B draw cyan", hr, verbose);
        }
    }
}

static void probe_draw(IDirect3DDevice9 *d)
{
    IDirect3DSurface9 *saved_rt = NULL, *saved_ds = NULL; D3DVIEWPORT9 saved_vp, vp;
    RECT saved_scissor; D3DSURFACE_DESC bb; HRESULT hr; int verbose, have_ds = 0, have_vp = 0, have_scissor = 0, bb_ok = 0, overlay_ok = 1;
#define PROBE_CALL(label, call) do { hr = (call); probe_note((label), hr, verbose); } while (0)
#define PROBE_REQUIRED(label, call) do { hr = (call); probe_note((label), hr, verbose); if (FAILED(hr)) overlay_ok = 0; } while (0)
    probe_init(d); if (!g_probe.ready || !g_backbuffer) return;
    verbose = g_probe.first_draw;
    /* Refresh again every 120 presented frames: enough to catch recurring upload failures
       without turning an unsupported DISCARD path into thousands of duplicate log lines. */
    if (verbose || (f_frames % 120) == 0) probe_dynamic_uploads(d, verbose);
    hr = IDirect3DStateBlock9_Capture(g_probe.state); probe_note("state Capture", hr, verbose); if (FAILED(hr)) return;
    hr = orig.GetRenderTarget(d, 0, &saved_rt); probe_note("save RenderTarget 0", hr, verbose); if (FAILED(hr)) goto restore;
    hr = orig.GetDepthStencilSurface(d, &saved_ds);
    if (hr == D3DERR_NOTFOUND) { have_ds = 1; if (verbose) logf("PROBE save DepthStencil -> %08lx (none)", (unsigned long)hr); }
    else { probe_note("save DepthStencil", hr, verbose); if (FAILED(hr)) goto restore; have_ds = 1; }
    hr = orig.GetViewport(d, &saved_vp); probe_note("save Viewport", hr, verbose); if (FAILED(hr)) goto restore; have_vp = 1;
    hr = orig.GetScissorRect(d, &saved_scissor); probe_note("save ScissorRect", hr, verbose); if (FAILED(hr)) goto restore; have_scissor = 1;
    /* Run 8's original pass must finish and be read back before game_finalize_pending()
       performs any diagnostic READONLY lock on the retained game resources. */
    if (g_game.pending_finalize) {
        D3DSURFACE_DESC run8_bb; ZeroMemory(&run8_bb, sizeof(run8_bb));
        hr = IDirect3DSurface9_GetDesc(g_backbuffer, &run8_bb);
        if (SUCCEEDED(hr)) run8_selftest(d, &run8_bb);
        else logf("RUN8 backbuffer GetDesc failed -> %08lx", (unsigned long)hr);
    }
    hr = orig.BeginScene(d); probe_note("BeginScene", hr, verbose); if (FAILED(hr)) goto restore;
    PROBE_REQUIRED("SetRenderTarget backbuffer", orig.SetRenderTarget(d, 0, g_backbuffer));
    PROBE_CALL("SetDepthStencil NULL", orig.SetDepthStencilSurface(d, NULL));
    ZeroMemory(&bb, sizeof(bb)); hr = IDirect3DSurface9_GetDesc(g_backbuffer, &bb); probe_note("backbuffer GetDesc", hr, verbose);
    if (SUCCEEDED(hr)) { bb_ok = 1; vp.X = vp.Y = 0; vp.Width = bb.Width; vp.Height = bb.Height; vp.MinZ = 0.0f; vp.MaxZ = 1.0f; PROBE_REQUIRED("SetViewport full", orig.SetViewport(d, &vp)); }
    else overlay_ok = 0;
    PROBE_CALL("SetScissorTest FALSE", orig.SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE));
    PROBE_REQUIRED("SetVertexShader NULL", orig.SetVertexShader(d, NULL));
    PROBE_REQUIRED("SetPixelShader NULL", orig.SetPixelShader(d, NULL));
    PROBE_CALL("SetFVF XYZRHW|DIFFUSE|TEX1", orig.SetFVF(d, PROBE_FVF));
    PROBE_CALL("ZENABLE FALSE", orig.SetRenderState(d, D3DRS_ZENABLE, FALSE));
    PROBE_CALL("ZWRITEENABLE FALSE", orig.SetRenderState(d, D3DRS_ZWRITEENABLE, FALSE));
    PROBE_CALL("ALPHATEST FALSE", orig.SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE));
    PROBE_CALL("ALPHABLEND FALSE", orig.SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE));
    PROBE_CALL("FOG FALSE", orig.SetRenderState(d, D3DRS_FOGENABLE, FALSE));
    PROBE_CALL("STENCIL FALSE", orig.SetRenderState(d, D3DRS_STENCILENABLE, FALSE));
    PROBE_CALL("LIGHTING FALSE", orig.SetRenderState(d, D3DRS_LIGHTING, FALSE));
    PROBE_CALL("CULL NONE", orig.SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE));
    PROBE_CALL("FILL SOLID", orig.SetRenderState(d, D3DRS_FILLMODE, D3DFILL_SOLID));
    PROBE_CALL("COLORWRITE RGBA", orig.SetRenderState(d, D3DRS_COLORWRITEENABLE, 0x0f));
    PROBE_CALL("VERTEXBLEND DISABLE", orig.SetRenderState(d, D3DRS_VERTEXBLEND, D3DVBF_DISABLE));
    PROBE_CALL("INDEXEDVERTEXBLEND FALSE", orig.SetRenderState(d, D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE));
    PROBE_CALL("WRAP0 0", orig.SetRenderState(d, D3DRS_WRAP0, 0));
    PROBE_CALL("stage1 DISABLE", orig.SetTextureStageState(d, 1, D3DTSS_COLOROP, D3DTOP_DISABLE));
    PROBE_CALL("sampler MIN POINT", orig.SetSamplerState(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    PROBE_CALL("sampler MAG POINT", orig.SetSamplerState(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    PROBE_CALL("sampler MIP NONE", orig.SetSamplerState(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    PROBE_CALL("sampler ADDRESSU CLAMP", orig.SetSamplerState(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    PROBE_CALL("sampler ADDRESSV CLAMP", orig.SetSamplerState(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    PROBE_CALL("sampler SRGBTEXTURE FALSE", orig.SetSamplerState(d, 0, D3DSAMP_SRGBTEXTURE, FALSE));
    PROBE_CALL("SRGBWRITE FALSE", orig.SetRenderState(d, D3DRS_SRGBWRITEENABLE, FALSE));
    PROBE_CALL("TEXCOORDINDEX 0", orig.SetTextureStageState(d, 0, D3DTSS_TEXCOORDINDEX, 0));
    PROBE_CALL("TEXTURETRANSFORM DISABLE", orig.SetTextureStageState(d, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
    PROBE_CALL("RESULTARG CURRENT", orig.SetTextureStageState(d, 0, D3DTSS_RESULTARG, D3DTA_CURRENT));

    if (bb_ok && overlay_ok) game_draw_overlays(d, &bb, verbose);

    PROBE_CALL("untextured SetTexture NULL", orig.SetTexture(d, 0, NULL));
    PROBE_CALL("untextured COLOROP", orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    PROBE_CALL("untextured COLORARG1", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
    PROBE_CALL("untextured ALPHAOP", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    PROBE_CALL("untextured ALPHAARG1", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
    PROBE_CALL("draw red DrawPrimitiveUP", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, g_probe.up, sizeof(PROBEVERT)));
    if (g_probe.managed_vb) { PROBE_CALL("SetStreamSource MANAGED-VB", orig.SetStreamSource(d, 0, g_probe.managed_vb, 0, sizeof(PROBEVERT))); PROBE_CALL("draw green MANAGED-VB", orig.DrawPrimitive(d, D3DPT_TRIANGLELIST, 0, 2)); }
    if (g_probe.dynamic_vb) { PROBE_CALL("SetStreamSource DYNAMIC-VB", orig.SetStreamSource(d, 0, g_probe.dynamic_vb, 0, sizeof(PROBEVERT))); PROBE_CALL("draw blue DYNAMIC-VB", orig.DrawPrimitive(d, D3DPT_TRIANGLELIST, 0, 2)); }
    probe_draw_rename(d, verbose);

    PROBE_CALL("textured COLOROP", orig.SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_MODULATE));
    PROBE_CALL("textured COLORARG1", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    PROBE_CALL("textured COLORARG2", orig.SetTextureStageState(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE));
    PROBE_CALL("textured ALPHAOP", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE));
    PROBE_CALL("textured ALPHAARG1", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
    PROBE_CALL("textured ALPHAARG2", orig.SetTextureStageState(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE));
    if (g_probe.managed_tex) { PROBE_CALL("SetTexture MANAGED", orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)g_probe.managed_tex)); PROBE_CALL("draw yellow MANAGED-TEX", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, g_probe.tex[0], sizeof(PROBEVERT))); }
    if (g_probe.dynamic_tex) { PROBE_CALL("SetTexture DYNAMIC", orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)g_probe.dynamic_tex)); PROBE_CALL("draw magenta DYNAMIC-TEX", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, g_probe.tex[1], sizeof(PROBEVERT))); }
    if (g_probe.upload_dst) { PROBE_CALL("SetTexture UPLOAD-DST", orig.SetTexture(d, 0, (IDirect3DBaseTexture9 *)g_probe.upload_dst)); PROBE_CALL("draw cyan UPDATE-TEX", orig.DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, 2, g_probe.tex[2], sizeof(PROBEVERT))); }
    hr = orig.EndScene(d); probe_note("EndScene", hr, verbose);
restore:
    hr = IDirect3DStateBlock9_Apply(g_probe.state); probe_note("state Apply", hr, verbose);
    if (saved_rt) { hr = orig.SetRenderTarget(d, 0, saved_rt); probe_note("restore RenderTarget 0", hr, verbose); }
    if (have_ds && saved_ds) { hr = orig.SetDepthStencilSurface(d, saved_ds); probe_note("restore DepthStencil", hr, verbose); }
    else if (have_ds) { hr = orig.SetDepthStencilSurface(d, NULL); probe_note("restore DepthStencil NULL", hr, verbose); }
    if (have_vp) { hr = orig.SetViewport(d, &saved_vp); probe_note("restore Viewport", hr, verbose); }
    if (have_scissor) { hr = orig.SetScissorRect(d, &saved_scissor); probe_note("restore ScissorRect", hr, verbose); }
    if (saved_rt) IDirect3DSurface9_Release(saved_rt); if (saved_ds) IDirect3DSurface9_Release(saved_ds);
    if (verbose) logf("PROBE first matrix draw complete before Present");
    g_game.just_finalized = 0;
    g_probe.first_draw = 0;
#undef PROBE_CALL
#undef PROBE_REQUIRED
}

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
static IDirect3DBaseTexture9 *run8_texture_replacement(DWORD stage, IDirect3DBaseTexture9 *t)
{
    IDirect3DTexture9 *replacement = NULL;
    if (!g_run8.fix_texture || stage != 0 || !t) return t;
    if (g_game.managed.valid && t == (IDirect3DBaseTexture9 *)g_game.managed.tex)
        replacement = g_game.managed.replay_tex;
    if (!replacement && g_game.any.valid && t == (IDirect3DBaseTexture9 *)g_game.any.tex)
        replacement = g_game.any.replay_tex;
    if (replacement) {
        g_run8.fix_texture_substitutions++;
        if (g_run8.fix_texture_substitutions <= 16)
            logf("RUN8 FIX texture substitution #%lu %p -> %p", g_run8.fix_texture_substitutions,
                 t, replacement);
        return (IDirect3DBaseTexture9 *)replacement;
    }
    return t;
}

static int run8_try_drawprimitive_up(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type,
                                     UINT start_vertex, UINT primitive_count, HRESULT *draw_hr)
{
    IDirect3DVertexBuffer9 *vb = NULL; IDirect3DVertexShader9 *vs = NULL;
    IDirect3DPixelShader9 *ps = NULL; D3DVERTEXBUFFER_DESC vd;
    DWORD fvf = 0; UINT stream_freq = 0, stream_offset = 0, stride = 0, vertex_count, bytes;
    ULONGLONG offset64, bytes64; void *locked = NULL, *copy = NULL;
    HRESULT hr, unlock_hr, restore_hr; int candidate = 0, handled = 0;
    if (!g_run8.fix_vb || !draw_hr) return 0;
    g_run8.fix_vb_attempts++;
    hr = orig.GetFVF(d, &fvf); if (FAILED(hr) || fvf != GAME_FVF) goto cleanup;
    hr = orig.GetVertexShader(d, &vs); if (FAILED(hr)) goto cleanup;
    hr = orig.GetPixelShader(d, &ps); if (FAILED(hr) || vs || ps) goto cleanup;
    hr = orig.GetStreamSource(d, 0, &vb, &stream_offset, &stride);
    if (FAILED(hr) || !vb || stride < sizeof(GAMEVERT)) goto cleanup;
    hr = orig.GetStreamSourceFreq(d, 0, &stream_freq);
    if (FAILED(hr) || stream_freq != 1) goto cleanup;
    ZeroMemory(&vd, sizeof(vd)); hr = IDirect3DVertexBuffer9_GetDesc(vb, &vd);
    if (FAILED(hr)) goto cleanup;
    vertex_count = game_vertex_count(type, primitive_count);
    offset64 = (ULONGLONG)stream_offset + (ULONGLONG)start_vertex * stride;
    bytes64 = (ULONGLONG)vertex_count * stride;
    if (!vertex_count || !bytes64 || bytes64 > RUN8_FIX_COPY_MAX ||
        offset64 >= vd.Size || bytes64 > (ULONGLONG)vd.Size - offset64) goto cleanup;
    candidate = 1; bytes = (UINT)bytes64;
    copy = HeapAlloc(GetProcessHeap(), 0, bytes); if (!copy) goto cleanup;
    hr = IDirect3DVertexBuffer9_Lock(vb, (UINT)offset64, bytes, &locked, D3DLOCK_READONLY);
    if (FAILED(hr) || !locked) goto cleanup;
    CopyMemory(copy, locked, bytes);
    unlock_hr = IDirect3DVertexBuffer9_Unlock(vb); locked = NULL;
    if (FAILED(unlock_hr)) goto cleanup;
    hr = orig.DrawPrimitiveUP(d, type, primitive_count, copy, stride);
    restore_hr = orig.SetStreamSource(d, 0, vb, stream_offset, stride);
    if (SUCCEEDED(hr)) {
        *draw_hr = hr; handled = 1; g_run8.fix_vb_success++;
        if (g_run8.fix_vb_success <= 16)
            logf("RUN8 FIX VB bypass #%lu type=%d sv=%u pc=%u bytes=%u off=%lu",
                 g_run8.fix_vb_success, type, start_vertex, primitive_count, bytes,
                 (unsigned long)offset64);
    } else if (FAILED(restore_hr)) {
        /* Stream 0 is cleared by DrawPrimitiveUP, so an original fallback would be invalid. */
        *draw_hr = hr; handled = 1;
    }
    if (FAILED(restore_hr)) {
        g_run8.fix_vb_fail++;
        if (g_run8.fix_vb_fail <= 16)
            logf("RUN8 FIX VB SetStreamSource restore failed #%lu -> %08lx",
                 g_run8.fix_vb_fail, (unsigned long)restore_hr);
    }

cleanup:
    if (locked && vb) IDirect3DVertexBuffer9_Unlock(vb);
    if (copy) HeapFree(GetProcessHeap(), 0, copy);
    if (vb) IDirect3DVertexBuffer9_Release(vb);
    if (vs) IDirect3DVertexShader9_Release(vs);
    if (ps) IDirect3DPixelShader9_Release(ps);
    if (candidate && !handled) {
        g_run8.fix_vb_fail++;
        if (g_run8.fix_vb_fail <= 16)
            logf("RUN8 FIX VB bypass failed #%lu; falling back to original DrawPrimitive",
                 g_run8.fix_vb_fail);
    }
    return handled;
}

static HRESULT WINAPI h_SetTexture(IDirect3DDevice9 *d, DWORD stage, IDirect3DBaseTexture9 *t)
{ IDirect3DBaseTexture9 *use = run8_texture_replacement(stage, t); HRESULT hr = orig.SetTexture(d, stage, use); TRACK(S_SetTexture, hr); if (t) f_tex_set++; else f_tex_null++; if (SHOULDLOG(S_SetTexture, hr)) logf("FAIL SetTexture %lu %p (use=%p): %08lx", (unsigned long)stage, t, use, (unsigned long)hr); return hr; }
static HRESULT WINAPI h_DrawPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT sv, UINT pc)
{ HRESULT hr = E_FAIL; int bypassed; if (g_dump_this_frame) dump_state(d, "DP", pc); bypassed = run8_try_drawprimitive_up(d, t, sv, pc, &hr); if (!bypassed) hr = orig.DrawPrimitive(d, t, sv, pc); if (SUCCEEDED(hr) && g_game.collecting) game_consider_draw(d, t, sv, pc, f_draws + 1); TRACK(S_DrawPrimitive, hr); f_draws++; f_prims += pc; if (SHOULDLOG(S_DrawPrimitive, hr)) logf("FAIL DrawPrimitive type=%d count=%u: %08lx", t, pc, (unsigned long)hr); return hr; }
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
static HRESULT WINAPI h_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp) { HRESULT hr, bhr; if (g_nomsaa && pp->MultiSampleType) { logf("NoMSAA: Reset MultiSampleType %d -> 0", pp->MultiSampleType); pp->MultiSampleType = D3DMULTISAMPLE_NONE; pp->MultiSampleQuality = 0; } probe_release(); if (g_backbuffer) { IDirect3DSurface9_Release(g_backbuffer); g_backbuffer = NULL; } hr = orig.Reset(d, pp); TRACK(S_Reset, hr); logf("%s Reset %ux%u %s windowed=%d ms=%d autods=%d %s flags=%lx: %08lx", FAILED(hr) ? "FAIL" : "ok", pp->BackBufferWidth, pp->BackBufferHeight, fmtname(pp->BackBufferFormat), pp->Windowed, pp->MultiSampleType, pp->EnableAutoDepthStencil, fmtname(pp->AutoDepthStencilFormat), (unsigned long)pp->Flags, (unsigned long)hr); if (SUCCEEDED(hr)) { bhr = orig.GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backbuffer); logf("PROBE Reset GetBackBuffer -> %08lx %p", (unsigned long)bhr, g_backbuffer); } return hr; }
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
    HRESULT hr; game_capture_present(d); probe_draw(d); hr = orig.Present(d, s, t, w, r); TRACK(S_Present, hr); f_frames++;
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
        char path[MAX_PATH], force[8]; UINT n; HMODULE pin;
        g_self = inst; DisableThreadLibraryCalls(inst);
        n = GetModuleFileNameA(inst, path, MAX_PATH); while (n && path[n - 1] != '\\') n--; lstrcpyA(path + n, "d3d9log.txt");
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        lstrcpyA(path + n, "d3d9log.ini"); g_nomsaa = GetPrivateProfileIntA("d3d9log", "NoMSAA", 0, path); g_probe_enabled = GetPrivateProfileIntA("d3d9log", "Probe", 1, path);
        g_game_force = GetPrivateProfileIntA("d3d9log", "GameForce", 0, path) != 0;
        g_game_min_draws = GetPrivateProfileIntA("d3d9log", "GameMinDraws", 0, path);
        if (g_game_min_draws < 0) g_game_min_draws = 0;
        force[0] = 0;
        if (GetEnvironmentVariableA("D3D9LOG_GAMECAP_FORCE", force, sizeof(force)))
            g_game_force = force[0] == '1';
        logf("d3d9log run8 loaded in pid %lu (NoMSAA=%d Probe=%d GameForce=%d GameMinDraws=%d)",
             (unsigned long)GetCurrentProcessId(), g_nomsaa, g_probe_enabled, g_game_force, g_game_min_draws);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)DllMain, &pin);
    }
    else if (reason == DLL_PROCESS_DETACH && g_log != INVALID_HANDLE_VALUE) { logf("unloading"); CloseHandle(g_log); }
    return TRUE;
}

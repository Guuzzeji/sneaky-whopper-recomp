/*
 * d3d9_hle.c: the game's Xbox D3D8 (XDK 5849) mapped onto host Direct3D 9.
 *
 * The XDK D3D library is statically linked and lifted with the rest of the
 * image. Left alone it writes NV2A pushbuffers. Here its entry points are
 * wrapped instead: each sub_X below is the address the game calls, and its
 * hle_* handler either forwards to the lifted body (sub_X_gen) or does the
 * work on a D3D9 device. Render and texture-stage state are not taken from
 * the setters: D3D keeps every state in guest arrays (D3D_g_RenderState and
 * friends, below), the game's inlined setters write them directly, and the
 * draw path reads them there.
 *
 * Arguments: [esp] is the return address, stack args follow. Most entry
 * points are __stdcall (the handler pops 4 + 4*n); the few register-argument
 * ones say so. Replacing a function means setting g_eax and g_esp exactly as
 * its `ret N` would.
 *
 * RECOMP_D3D_TRACE=N logs the first N calls (name, register and stack args).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"
#include "d3d8_swizzle.h"
#include "overlay.h"
#include "d3d_hle_shared.h"






/* -- Value translation: Xbox D3D uses the NV2A's (OpenGL) enum values ---- */

static DWORD tr_cmp(uint32_t v)          /* GL_NEVER 0x200 .. GL_ALWAYS 0x207 */
{
    return (v >= 0x200 && v <= 0x207) ? v - 0x200 + 1 : D3DCMP_ALWAYS;
}

static DWORD tr_blend(uint32_t v)
{
    switch (v) {
    case 0x000: return D3DBLEND_ZERO;
    case 0x001: return D3DBLEND_ONE;
    case 0x300: return D3DBLEND_SRCCOLOR;
    case 0x301: return D3DBLEND_INVSRCCOLOR;
    case 0x302: return D3DBLEND_SRCALPHA;
    case 0x303: return D3DBLEND_INVSRCALPHA;
    case 0x304: return D3DBLEND_DESTALPHA;
    case 0x305: return D3DBLEND_INVDESTALPHA;
    case 0x306: return D3DBLEND_DESTCOLOR;
    case 0x307: return D3DBLEND_INVDESTCOLOR;
    case 0x308: return D3DBLEND_SRCALPHASAT;
    /* ponytail: constant alpha uses the whole blend colour; exact only when
     * the title's BLENDCOLOR is grey. */
    case 0x8001: case 0x8003: return D3DBLEND_BLENDFACTOR;
    case 0x8002: case 0x8004: return D3DBLEND_INVBLENDFACTOR;
    }
    return D3DBLEND_ONE;
}

static DWORD tr_blendop(uint32_t v)
{
    switch (v) {
    case 0x800A: return D3DBLENDOP_SUBTRACT;
    case 0x800B: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN;
    case 0x8008: return D3DBLENDOP_MAX;
    }
    return D3DBLENDOP_ADD;
}

static DWORD tr_stencilop(uint32_t v)
{
    switch (v) {
    case 0x0000: return D3DSTENCILOP_ZERO;
    case 0x1E01: return D3DSTENCILOP_REPLACE;
    case 0x1E02: return D3DSTENCILOP_INCRSAT;
    case 0x1E03: return D3DSTENCILOP_DECRSAT;
    case 0x150A: return D3DSTENCILOP_INVERT;
    case 0x8507: return D3DSTENCILOP_INCR;
    case 0x8508: return D3DSTENCILOP_DECR;
    }
    return D3DSTENCILOP_KEEP;                /* 0x1E00 */
}

static DWORD tr_cull(uint32_t v)
{
    return v == 0x900 ? D3DCULL_CW : v == 0x901 ? D3DCULL_CCW : D3DCULL_NONE;
}

static DWORD tr_colorwrite(uint32_t v)
{
    return ((v >> 16) & 1 ? D3DCOLORWRITEENABLE_RED : 0)
         | ((v >> 8) & 1 ? D3DCOLORWRITEENABLE_GREEN : 0)
         | (v & 1 ? D3DCOLORWRITEENABLE_BLUE : 0)
         | ((v >> 24) & 1 ? D3DCOLORWRITEENABLE_ALPHA : 0);
}

static DWORD tr_wrap(uint32_t v)         /* Xbox U 0x10, V 0x1000, W 0x100000 */
{
    return ((v & 0x10) ? D3DWRAP_U : 0) | ((v & 0x1000) ? D3DWRAP_V : 0)
         | ((v & 0x100000) ? D3DWRAP_W : 0);
}

static DWORD tr_texop(uint32_t v)
{
    /* Xbox D3DTOP order differs from PC after BLENDTEXTUREALPHA. */
    static const DWORD map[] = {
        0, D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2,
        D3DTOP_MODULATE, D3DTOP_MODULATE2X, D3DTOP_MODULATE4X, D3DTOP_ADD,
        D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT, D3DTOP_ADDSMOOTH,
        D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDCURRENTALPHA,
        D3DTOP_BLENDTEXTUREALPHA, D3DTOP_BLENDFACTORALPHA,
        D3DTOP_BLENDTEXTUREALPHAPM, D3DTOP_PREMODULATE,
        D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTOP_MODULATECOLOR_ADDALPHA,
        D3DTOP_MODULATEINVALPHA_ADDCOLOR, D3DTOP_MODULATEINVCOLOR_ADDALPHA,
        D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP,
        D3DTOP_BUMPENVMAP, D3DTOP_BUMPENVMAPLUMINANCE,
    };
    return v < sizeof map / sizeof map[0] && v ? map[v] : D3DTOP_DISABLE;
}

static DWORD tr_filter(uint32_t v)       /* QUINCUNX 4, GAUSSIANCUBIC 5 */
{
    return v <= D3DTEXF_ANISOTROPIC ? v : D3DTEXF_LINEAR;
}

static DWORD tr_address(uint32_t v)      /* CLAMPTOEDGE 5 */
{
    return (v >= 1 && v <= 4) ? v : D3DTADDRESS_CLAMP;
}

/* -- Device and window ---------------------------------------------------- */

extern void *xbox_FramebufferWindowHwnd(void);
extern void xbox_FramebufferWindowExternalPresent(int on);

static CRITICAL_SECTION s_lock;
static IDirect3D9 *s_d3d;
static IDirect3DDevice9 *s_dev;
static IDirect3DSwapChain9 *s_sc;          /* sized to the window's client area */
static UINT s_sc_w, s_sc_h;
static int s_failed;
static uint32_t s_scale = 2;
static OverlayD3dStats s_st;                /* this frame's counters (overlay) */
static unsigned s_hd_hits;                  /* textures RECOMP_HD_TEX replaced */
static uint32_t s_frame;

static int dev_ready(void)
{
    HWND hwnd;
    D3DPRESENT_PARAMETERS pp;

    if (s_dev)
        return 1;
    if (s_failed || !(hwnd = (HWND)xbox_FramebufferWindowHwnd()))
        return 0;
    s_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 640 * s_scale;
    pp.BackBufferHeight = 480 * s_scale;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (!s_d3d || FAILED(IDirect3D9_CreateDevice(s_d3d, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED
            | D3DCREATE_FPU_PRESERVE, &pp, &s_dev))) {
        fprintf(stderr, "[D3D9] device creation failed\n");
        s_failed = 1;
        return 0;
    }
    xbox_FramebufferWindowExternalPresent(1);
    {
        /* ponytail: 640x480 a scale step, shrunk to fit the screen. Async:
         * the window's own thread moves it. */
        RECT wa, wr = { 0, 0, (LONG)(640 * s_scale), (LONG)(480 * s_scale) };
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        while (wr.right > (wa.right - wa.left) * 9 / 10
               || wr.bottom > (wa.bottom - wa.top) * 9 / 10) {
            wr.right = wr.right * 3 / 4;
            wr.bottom = wr.bottom * 3 / 4;
        }
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    IDirect3DDevice9_BeginScene(s_dev);
    fprintf(stderr, "[D3D9] device up, rendering at %ux\n", s_scale);
    return 1;
}

/* -- Render targets: one host target per guest surface address ------------ */

typedef struct {
    uint32_t data, w, h;            /* guest address and size (1x) */
    uint32_t xfmt, pitch;           /* guest colour format and row pitch */
    uint32_t ghash, synced;         /* guest memory as last seen, and when */
    int depth;
    IDirect3DTexture9 *tex;         /* colour targets: sampleable */
    IDirect3DSurface9 *surf;
} HostRt;

#define MAX_RT 32
static HostRt s_rt[MAX_RT];
static int s_nrt;
static HostRt *s_cur_rt, *s_cur_ds;


static HostRt *rt_find(uint32_t data)
{
    int i;
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i].data == data && !s_rt[i].depth)
            return &s_rt[i];
    return NULL;
}

static HostRt *rt_get(uint32_t res, int depth)
{
    uint32_t data = res_data(res), w, h, pitch;
    HostRt *r;
    int i;
    HRESULT hr;

    surf_dims(res, &w, &h, &pitch);
    for (i = 0; i < s_nrt; i++) {
        r = &s_rt[i];
        if (r->data == data && r->depth == depth && r->w == w && r->h == h)
            return r;
    }
    /* ponytail: no eviction; a title with more than MAX_RT targets reuses
     * the last slot. */
    r = &s_rt[s_nrt < MAX_RT ? s_nrt++ : MAX_RT - 1];
    if (r->surf) IDirect3DSurface9_Release(r->surf);
    if (r->tex) IDirect3DTexture9_Release(r->tex);
    memset(r, 0, sizeof *r);
    r->data = data; r->w = w; r->h = h; r->depth = depth;
    r->xfmt = (MEM32(res + 0xC) >> 8) & 0xFF;
    r->pitch = pitch ? pitch : w * 4;
    r->synced = (uint32_t)-1;
    if (depth) {
        hr = IDirect3DDevice9_CreateDepthStencilSurface(s_dev, w * s_scale, h * s_scale,
                D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &r->surf, NULL);
    } else {
        hr = IDirect3DDevice9_CreateTexture(s_dev, w * s_scale, h * s_scale, 1,
                D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &r->tex, NULL);
        if (SUCCEEDED(hr))
            IDirect3DTexture9_GetSurfaceLevel(r->tex, 0, &r->surf);
    }
    fprintf(stderr, "[D3D9] %s target %08X %ux%u%s\n", depth ? "depth" : "colour",
            data, w, h, FAILED(hr) ? " FAILED" : "");
    return r;
}

/* Guest memory of a colour target is what the CPU sees of it. Nothing on the
 * host draws there: Clear fills it (below), the CPU writes the rest (the XMV
 * player decodes movie frames straight into the back buffer). So when it has
 * changed since we last looked, the CPU wrote it, and the host target takes
 * the new pixels. Checked once per frame per target, and at present.
 *
 * ponytail: the sampled hash misses a CPU write of a few pixels; the movie
 * player rewrites the whole frame. GPU results are not copied back to guest
 * memory for CPU readers. */
static IDirect3DSurface9 *s_up_sys, *s_up_def;
static uint32_t s_up_w, s_up_h;

static uint32_t rt_guest_bytes(const HostRt *r) { return r->pitch * r->h; }

static void rt_upload(HostRt *r)
{
    D3DLOCKED_RECT lr;
    uint32_t x, y, bpp16 = r->xfmt == 0x11 || r->xfmt == 0x05;

    s_st.rt_uploads++;
    if (s_up_w != r->w || s_up_h != r->h) {
        if (s_up_sys) IDirect3DSurface9_Release(s_up_sys);
        if (s_up_def) IDirect3DSurface9_Release(s_up_def);
        s_up_sys = s_up_def = NULL;
        IDirect3DDevice9_CreateOffscreenPlainSurface(s_dev, r->w, r->h, D3DFMT_A8R8G8B8,
                                                     D3DPOOL_SYSTEMMEM, &s_up_sys, NULL);
        IDirect3DDevice9_CreateOffscreenPlainSurface(s_dev, r->w, r->h, D3DFMT_A8R8G8B8,
                                                     D3DPOOL_DEFAULT, &s_up_def, NULL);
        s_up_w = r->w;
        s_up_h = r->h;
    }
    if (!s_up_sys || !s_up_def
        || FAILED(IDirect3DSurface9_LockRect(s_up_sys, &lr, NULL, 0)))
        return;
    for (y = 0; y < r->h; y++) {
        const uint8_t *s = (const uint8_t *)gp(r->data) + (size_t)y * r->pitch;
        uint32_t *d = (uint32_t *)((uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
        if (!bpp16) {
            memcpy(d, s, (size_t)r->w * 4);
            continue;
        }
        for (x = 0; x < r->w; x++) {
            uint32_t p = ((const uint16_t *)s)[x];
            uint32_t R = (p >> 11) & 31, G = (p >> 5) & 63, B = p & 31;
            d[x] = 0xFF000000u | ((R << 3 | R >> 2) << 16) | ((G << 2 | G >> 4) << 8) | (B << 3 | B >> 2);
        }
    }
    IDirect3DSurface9_UnlockRect(s_up_sys);
    IDirect3DDevice9_UpdateSurface(s_dev, s_up_sys, NULL, s_up_def, NULL);
    IDirect3DDevice9_StretchRect(s_dev, s_up_def, NULL, r->surf, NULL, D3DTEXF_POINT);
}

static void rt_sync(HostRt *r, int force)
{
    uint32_t h;
    if (!r || r->depth || !r->surf || (!force && r->synced == s_frame))
        return;
    r->synced = s_frame;
    h = hash_bytes((const uint8_t *)gp(r->data), rt_guest_bytes(r));
    if (h != r->ghash) {
        r->ghash = h;
        rt_upload(r);
    }
}

/* Clear's effect on the CPU's view of a colour target. */
static void rt_fill(HostRt *r, const D3DRECT *rects, uint32_t n, uint32_t argb)
{
    D3DRECT full = { 0, 0, 0, 0 };
    uint32_t i, x, y, bpp16 = r->xfmt == 0x11 || r->xfmt == 0x05;
    uint16_t c16 = (uint16_t)(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F));

    full.x2 = (LONG)r->w;
    full.y2 = (LONG)r->h;
    if (!n) { rects = &full; n = 1; }
    for (i = 0; i < n; i++) {
        LONG x1 = rects[i].x1 < 0 ? 0 : rects[i].x1, y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
        LONG x2 = rects[i].x2 > (LONG)r->w ? (LONG)r->w : rects[i].x2;
        LONG y2 = rects[i].y2 > (LONG)r->h ? (LONG)r->h : rects[i].y2;
        for (y = (uint32_t)y1; (LONG)y < y2; y++) {
            uint8_t *row = (uint8_t *)gp(r->data) + (size_t)y * r->pitch;
            for (x = (uint32_t)x1; (LONG)x < x2; x++) {
                if (bpp16) ((uint16_t *)row)[x] = c16;
                else ((uint32_t *)row)[x] = argb;
            }
        }
    }
    r->ghash = hash_bytes((const uint8_t *)gp(r->data), rt_guest_bytes(r));
}

/* Viewport as the title last set it (D3DVIEWPORT8, 1x pixels). */
static D3DVIEWPORT9 s_vp = { 0, 0, 640, 480, 0.0f, 1.0f };

static void bind_targets(void)
{
    uint32_t dev = xb_device();
    uint32_t rt = MEM32(dev + 0x1A04), ds = MEM32(dev + 0x1A08);
    HostRt *c = rt ? rt_get(rt, 0) : NULL;
    HostRt *d = ds ? rt_get(ds, 1) : NULL;
    D3DVIEWPORT9 vp = s_vp;

    if (c && c->surf && c != s_cur_rt) {
        IDirect3DDevice9_SetRenderTarget(s_dev, 0, c->surf);
        s_cur_rt = c;
    }
    rt_sync(c, 0);
    if (d != s_cur_ds) {
        IDirect3DDevice9_SetDepthStencilSurface(s_dev, d ? d->surf : NULL);
        s_cur_ds = d;
    }
    vp.X *= s_scale; vp.Y *= s_scale; vp.Width *= s_scale; vp.Height *= s_scale;
    if (c) {
        /* D3D9 refuses a viewport outside the target. */
        if (vp.X + vp.Width > c->w * s_scale) vp.Width = c->w * s_scale - vp.X;
        if (vp.Y + vp.Height > c->h * s_scale) vp.Height = c->h * s_scale - vp.Y;
    }
    IDirect3DDevice9_SetViewport(s_dev, &vp);
}

/* -- Textures -------------------------------------------------------------- */

typedef struct {
    uint8_t xfmt;
    uint8_t bits;           /* per texel; DXT: per 4x4 block / 16 */
    uint8_t swizzled;
    uint8_t expand;         /* decode to A8R8G8B8 on the CPU */
    D3DFORMAT d3d;
} XFmt;

static const XFmt s_fmts[] = {
    { 0x00,  8, 1, 0, D3DFMT_L8 },       { 0x01,  8, 1, 1, D3DFMT_A8R8G8B8 }, /* AL8 */
    { 0x02, 16, 1, 0, D3DFMT_A1R5G5B5 }, { 0x03, 16, 1, 0, D3DFMT_X1R5G5B5 },
    { 0x04, 16, 1, 0, D3DFMT_A4R4G4B4 }, { 0x05, 16, 1, 0, D3DFMT_R5G6B5 },
    { 0x06, 32, 1, 0, D3DFMT_A8R8G8B8 }, { 0x07, 32, 1, 0, D3DFMT_X8R8G8B8 },
    { 0x0B,  8, 1, 1, D3DFMT_A8R8G8B8 }, /* P8 */
    { 0x0C,  4, 0, 0, D3DFMT_DXT1 },     { 0x0E,  8, 0, 0, D3DFMT_DXT3 },
    { 0x0F,  8, 0, 0, D3DFMT_DXT5 },
    { 0x10, 16, 0, 0, D3DFMT_A1R5G5B5 }, { 0x11, 16, 0, 0, D3DFMT_R5G6B5 },
    { 0x12, 32, 0, 0, D3DFMT_A8R8G8B8 }, { 0x13,  8, 0, 0, D3DFMT_L8 },
    { 0x19,  8, 1, 0, D3DFMT_A8 },       { 0x1A, 16, 1, 0, D3DFMT_A8L8 },
    { 0x1B,  8, 0, 1, D3DFMT_A8R8G8B8 }, /* LIN_AL8 */
    { 0x1C, 16, 0, 0, D3DFMT_X1R5G5B5 }, { 0x1D, 16, 0, 0, D3DFMT_A4R4G4B4 },
    { 0x1E, 32, 0, 0, D3DFMT_X8R8G8B8 }, { 0x1F,  8, 0, 0, D3DFMT_A8 },
    { 0x20, 16, 0, 0, D3DFMT_A8L8 },
    { 0x3A, 32, 1, 0, D3DFMT_A8B8G8R8 }, { 0x3F, 32, 0, 0, D3DFMT_A8B8G8R8 },
};

static const XFmt *fmt_find(uint32_t xfmt)
{
    size_t i;
    for (i = 0; i < sizeof s_fmts / sizeof s_fmts[0]; i++)
        if (s_fmts[i].xfmt == xfmt)
            return &s_fmts[i];
    return NULL;
}

static int fmt_is_dxt(const XFmt *f) { return f->d3d == D3DFMT_DXT1 || f->d3d == D3DFMT_DXT3 || f->d3d == D3DFMT_DXT5; }

static uint32_t level_bytes(const XFmt *f, uint32_t w, uint32_t h)
{
    if (fmt_is_dxt(f))
        return ((w + 3) / 4) * ((h + 3) / 4) * (f->bits * 2);
    return w * h * f->bits / 8;
}

typedef struct {
    uint32_t data, format, size, palette;   /* key */
    uint32_t hash, checked;
    uint32_t w, h;                           /* 1x texels */
    int linear;
    IDirect3DBaseTexture9 *tex;
} HostTex;

#define MAX_TEX 2048
static HostTex s_tex[MAX_TEX];
static int s_ntex;

/* One level, guest layout to a locked host rect. */
static void upload_level(const XFmt *f, const uint8_t *src, uint32_t w, uint32_t h,
                         uint32_t src_pitch, const uint32_t *pal, D3DLOCKED_RECT *lr)
{
    uint8_t *dst = (uint8_t *)lr->pBits;
    uint32_t bpp = f->bits / 8, y, x;
    uint8_t *tmp = NULL;

    if (fmt_is_dxt(f)) {
        uint32_t row = ((w + 3) / 4) * f->bits * 2, rows = (h + 3) / 4;
        for (y = 0; y < rows; y++)
            memcpy(dst + y * lr->Pitch, src + y * row, row);
        return;
    }
    if (f->swizzled) {
        tmp = (uint8_t *)malloc((size_t)w * h * bpp);
        xbox_unswizzle_rect(tmp, src, w, h, bpp);
        src = tmp;
        src_pitch = w * bpp;
    }
    for (y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * src_pitch;
        uint8_t *d = dst + (size_t)y * lr->Pitch;
        if (!f->expand) {
            memcpy(d, s, (size_t)w * bpp);
        } else if (f->xfmt == 0x0B) {                     /* P8 */
            for (x = 0; x < w; x++)
                ((uint32_t *)d)[x] = pal ? pal[s[x]] : 0xFFFFFFFFu;
        } else {                                          /* AL8 */
            for (x = 0; x < w; x++)
                ((uint32_t *)d)[x] = s[x] * 0x01010101u;
        }
    }
    free(tmp);
}

static IDirect3DBaseTexture9 *tex_create(const XFmt *f, uint32_t data, uint32_t format,
                                         uint32_t w, uint32_t h, uint32_t pitch,
                                         const uint32_t *pal)
{
    uint32_t levels = f->swizzled || fmt_is_dxt(f) ? (format >> 16) & 0xF : 1;
    int cube = (format >> 2) & 1, faces = cube ? 6 : 1, face, lv;
    const uint8_t *src = (const uint8_t *)gp(data);
    IDirect3DTexture9 *t2 = NULL;
    IDirect3DCubeTexture9 *tc = NULL;
    HRESULT hr;

    s_st.tex_creates++;
    if (!levels) levels = 1;
    if (cube)
        hr = IDirect3DDevice9_CreateCubeTexture(s_dev, w, levels, 0, f->d3d,
                                                D3DPOOL_MANAGED, &tc, NULL);
    else
        hr = IDirect3DDevice9_CreateTexture(s_dev, w, h, levels, 0, f->d3d,
                                            D3DPOOL_MANAGED, &t2, NULL);
    if (FAILED(hr))
        return NULL;
    for (face = 0; face < faces; face++) {
        uint32_t lw = w, lh = h, face_bytes = 0;
        for (lv = 0; lv < (int)levels; lv++) {
            D3DLOCKED_RECT lr;
            HRESULT lh_ok = cube
                ? IDirect3DCubeTexture9_LockRect(tc, (D3DCUBEMAP_FACES)face, lv, &lr, NULL, 0)
                : IDirect3DTexture9_LockRect(t2, lv, &lr, NULL, 0);
            if (SUCCEEDED(lh_ok)) {
                upload_level(f, src, lw, lh, pitch ? pitch : lw * f->bits / 8, pal, &lr);
                if (cube) IDirect3DCubeTexture9_UnlockRect(tc, (D3DCUBEMAP_FACES)face, lv);
                else IDirect3DTexture9_UnlockRect(t2, lv);
            }
            face_bytes += level_bytes(f, lw, lh);
            src += level_bytes(f, lw, lh);
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        /* Cube faces start on 128-byte boundaries. */
        if (cube)
            src += ((face_bytes + 127) & ~127u) - face_bytes;
    }
    return cube ? (IDirect3DBaseTexture9 *)tc : (IDirect3DBaseTexture9 *)t2;
}

/* RECOMP_HD_TEX=<dir>: a replacement for a guest texture, from
 * <dir>/<hash>.dds (DXT1/3/5, any size; tools/xen_tex.py makes them from the
 * 360 packages). The hash is the one tex_get keys the cache on, so a texture
 * is replaced exactly when its guest bytes match. Coordinates of swizzled
 * and DXT textures are normalized, so a bigger texture needs nothing else. */
static IDirect3DBaseTexture9 *hd_load(uint32_t hash)
{
    static const char *dir = (const char *)-1;
    char path[MAX_PATH];
    uint8_t hdr[128], *buf = NULL;
    IDirect3DTexture9 *t = NULL;
    uint32_t w, h, mips, fourcc, bpb, lv, off = 0;
    long n;
    D3DFORMAT fmt;
    FILE *f;

    if (dir == (const char *)-1)
        dir = getenv("RECOMP_HD_TEX");
    if (!dir || !*dir)
        return NULL;
    snprintf(path, sizeof path, "%s/%08x.dds", dir, hash);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f) - 128;
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || fread(hdr, 1, 128, f) != 128 || memcmp(hdr, "DDS ", 4)
        || !(buf = (uint8_t *)malloc((size_t)n)) || fread(buf, 1, (size_t)n, f) != (size_t)n)
        goto done;
    memcpy(&h, hdr + 12, 4);
    memcpy(&w, hdr + 16, 4);
    memcpy(&mips, hdr + 28, 4);
    memcpy(&fourcc, hdr + 84, 4);
    if (!mips) mips = 1;
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '1')) { fmt = D3DFMT_DXT1; bpb = 8; }
    else if (fourcc == MAKEFOURCC('D', 'X', 'T', '3')) { fmt = D3DFMT_DXT3; bpb = 16; }
    else if (fourcc == MAKEFOURCC('D', 'X', 'T', '5')) { fmt = D3DFMT_DXT5; bpb = 16; }
    else goto done;
    if (FAILED(IDirect3DDevice9_CreateTexture(s_dev, w, h, mips, 0, fmt, D3DPOOL_MANAGED,
                                              &t, NULL)))
        goto done;
    for (lv = 0; lv < mips; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        uint32_t row = (lw + 3) / 4 * bpb, rows = (lh + 3) / 4, y;
        D3DLOCKED_RECT lr;
        if (off + row * rows > (uint32_t)n)
            break;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(t, lv, &lr, NULL, 0))) {
            for (y = 0; y < rows; y++)
                memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, buf + off + y * row, row);
            IDirect3DTexture9_UnlockRect(t, lv);
        }
        off += row * rows;
    }
    if (s_hd_hits++ < 8)
        fprintf(stderr, "[HD] %08x -> %ux%u\n", hash, w, h);
done:
    free(buf);
    fclose(f);
    return (IDirect3DBaseTexture9 *)t;
}

/* The host texture for a guest texture (and palette), uploaded or refreshed
 * as needed. *lin_w/*lin_h are set for linear textures, whose coordinates the
 * title gives in texels. */
static IDirect3DBaseTexture9 *tex_get(uint32_t res, uint32_t pal_res,
                                      float *lin_w, float *lin_h)
{
    uint32_t data = res_data(res), format = MEM32(res + 0xC), size = MEM32(res + 0x10);
    uint32_t w, h, pitch, xf = (format >> 8) & 0xFF, bytes, hash, pal = 0;
    const XFmt *f;
    HostRt *rt;
    HostTex *t = NULL;
    int i;

    surf_dims(res, &w, &h, &pitch);
    *lin_w = *lin_h = 0;
    /* A texture over a render target samples the host target. */
    if ((rt = rt_find(data)) != NULL) {
        if (size) { *lin_w = (float)rt->w; *lin_h = (float)rt->h; }
        return (IDirect3DBaseTexture9 *)rt->tex;
    }
    f = fmt_find(xf);
    if (!f) {
        static uint32_t said[64];
        if (!said[xf & 63]++)
            fprintf(stderr, "[D3D9] texture format 0x%02X not handled\n", xf);
        return NULL;
    }
    if (xf == 0x0B && pal_res)
        pal = res_data(pal_res);
    if (!f->swizzled && !fmt_is_dxt(f)) {
        *lin_w = (float)w;
        *lin_h = (float)h;
    }
    for (i = 0; i < s_ntex; i++)
        if (s_tex[i].data == data && s_tex[i].format == format && s_tex[i].size == size
            && s_tex[i].palette == pal) {
            t = &s_tex[i];
            break;
        }
    if (t && t->checked == s_frame)
        return t->tex;
    bytes = pitch ? pitch * h : level_bytes(f, w, h);
    hash = hash_bytes((const uint8_t *)gp(data), bytes);
    if (pal)
        hash ^= hash_bytes((const uint8_t *)gp(pal), 1024) * 31u;
    if (t && t->hash == hash) {
        t->checked = s_frame;
        return t->tex;
    }
    if (!t) {
        /* ponytail: no eviction past MAX_TEX; the last slot is recycled. */
        t = &s_tex[s_ntex < MAX_TEX ? s_ntex++ : MAX_TEX - 1];
        if (t->tex) IDirect3DBaseTexture9_Release(t->tex);
        memset(t, 0, sizeof *t);
    } else if (t->tex) {
        IDirect3DBaseTexture9_Release(t->tex);
    }
    t->data = data; t->format = format; t->size = size; t->palette = pal;
    t->hash = hash; t->checked = s_frame; t->w = w; t->h = h;
    t->tex = !pal && !pitch && !((format >> 2) & 1) ? hd_load(hash) : NULL;
    if (!t->tex)
        t->tex = tex_create(f, data, format, w, h, pitch,
                            pal ? (const uint32_t *)gp(pal) : NULL);
    return t->tex;
}

/* -- Mirrored state --------------------------------------------------------- */

static uint32_t s_stage_tex[4], s_stage_pal[4];
static uint32_t s_vshader;                  /* FVF, or a shader handle | 1 */
static uint32_t s_pshader;
static uint32_t s_stream_vb[16], s_stream_stride[16];
static uint32_t s_ib, s_base_vertex;
static D3DMATRIX s_xf[10];
static uint32_t s_xf_dirty = 0x3FF;
static D3DLIGHT9 s_light[8];
static uint32_t s_light_dirty, s_light_on, s_light_on_dirty;
static D3DMATERIAL9 s_material;
static int s_material_dirty;
static float s_lin[4][2];                   /* 1/texel size of linear textures */

static DWORD s_rs_shadow[256];
static uint8_t s_rs_valid[256];

static void rs(D3DRENDERSTATETYPE st, DWORD v)
{
    if (s_rs_valid[st] && s_rs_shadow[st] == v)
        return;
    s_rs_valid[st] = 1;
    s_rs_shadow[st] = v;
    s_st.states++;
    IDirect3DDevice9_SetRenderState(s_dev, st, v);
}

static void apply_render_state(void)
{
    uint32_t zbias = xb_rs(XRS_ZBIAS);

    rs(D3DRS_ZENABLE, xb_rs(XRS_ZENABLE) && s_cur_ds ? xb_rs(XRS_ZENABLE) : D3DZB_FALSE);
    rs(D3DRS_ZFUNC, tr_cmp(xb_rs(XRS_ZFUNC)));
    rs(D3DRS_ZWRITEENABLE, xb_rs(XRS_ZWRITEENABLE) != 0);
    rs(D3DRS_ALPHATESTENABLE, xb_rs(XRS_ALPHATESTENABLE) != 0);
    rs(D3DRS_ALPHAFUNC, tr_cmp(xb_rs(XRS_ALPHAFUNC)));
    rs(D3DRS_ALPHAREF, xb_rs(XRS_ALPHAREF) & 0xFF);
    rs(D3DRS_ALPHABLENDENABLE, xb_rs(XRS_ALPHABLENDENABLE) != 0);
    rs(D3DRS_SRCBLEND, tr_blend(xb_rs(XRS_SRCBLEND)));
    rs(D3DRS_DESTBLEND, tr_blend(xb_rs(XRS_DESTBLEND)));
    rs(D3DRS_BLENDOP, tr_blendop(xb_rs(XRS_BLENDOP)));
    rs(D3DRS_BLENDFACTOR, xb_rs(XRS_BLENDCOLOR));
    rs(D3DRS_DITHERENABLE, xb_rs(XRS_DITHERENABLE) != 0);
    rs(D3DRS_SHADEMODE, xb_rs(XRS_SHADEMODE) == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
    rs(D3DRS_COLORWRITEENABLE, tr_colorwrite(xb_rs(XRS_COLORWRITEENABLE)));
    rs(D3DRS_STENCILENABLE, xb_rs(XRS_STENCILENABLE) && s_cur_ds);
    rs(D3DRS_STENCILFUNC, tr_cmp(xb_rs(XRS_STENCILFUNC)));
    rs(D3DRS_STENCILREF, xb_rs(XRS_STENCILREF));
    rs(D3DRS_STENCILMASK, xb_rs(XRS_STENCILMASK));
    rs(D3DRS_STENCILWRITEMASK, xb_rs(XRS_STENCILWRITEMASK));
    rs(D3DRS_STENCILFAIL, tr_stencilop(xb_rs(XRS_STENCILFAIL)));
    rs(D3DRS_STENCILZFAIL, tr_stencilop(xb_rs(XRS_STENCILZFAIL)));
    rs(D3DRS_STENCILPASS, tr_stencilop(xb_rs(XRS_STENCILPASS)));
    rs(D3DRS_CULLMODE, tr_cull(xb_rs(XRS_CULLMODE)));
    rs(D3DRS_FILLMODE, xb_rs(XRS_FILLMODE) == 0x1B00 ? D3DFILL_POINT
                     : xb_rs(XRS_FILLMODE) == 0x1B01 ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    rs(D3DRS_FOGENABLE, xb_rs(XRS_FOGENABLE) != 0);
    rs(D3DRS_FOGTABLEMODE, xb_rs(XRS_FOGTABLEMODE));
    rs(D3DRS_FOGSTART, xb_rs(XRS_FOGSTART));
    rs(D3DRS_FOGEND, xb_rs(XRS_FOGEND));
    rs(D3DRS_FOGDENSITY, xb_rs(XRS_FOGDENSITY));
    rs(D3DRS_RANGEFOGENABLE, xb_rs(XRS_RANGEFOGENABLE) != 0);
    rs(D3DRS_FOGCOLOR, xb_rs(XRS_FOGCOLOR));
    rs(D3DRS_WRAP0, tr_wrap(xb_rs(XRS_WRAP0)));
    rs(D3DRS_WRAP1, tr_wrap(xb_rs(XRS_WRAP1)));
    rs(D3DRS_WRAP2, tr_wrap(xb_rs(XRS_WRAP2)));
    rs(D3DRS_WRAP3, tr_wrap(xb_rs(XRS_WRAP3)));
    rs(D3DRS_LIGHTING, xb_rs(XRS_LIGHTING) != 0);
    rs(D3DRS_SPECULARENABLE, xb_rs(XRS_SPECULARENABLE) != 0);
    rs(D3DRS_LOCALVIEWER, xb_rs(XRS_LOCALVIEWER) != 0);
    rs(D3DRS_COLORVERTEX, xb_rs(XRS_COLORVERTEX) != 0);
    rs(D3DRS_SPECULARMATERIALSOURCE, xb_rs(XRS_SPECULARMATERIALSOURCE));
    rs(D3DRS_DIFFUSEMATERIALSOURCE, xb_rs(XRS_DIFFUSEMATERIALSOURCE));
    rs(D3DRS_AMBIENTMATERIALSOURCE, xb_rs(XRS_AMBIENTMATERIALSOURCE));
    rs(D3DRS_EMISSIVEMATERIALSOURCE, xb_rs(XRS_EMISSIVEMATERIALSOURCE));
    rs(D3DRS_AMBIENT, xb_rs(XRS_AMBIENT));
    rs(D3DRS_NORMALIZENORMALS, xb_rs(XRS_NORMALIZENORMALS) != 0);
    rs(D3DRS_TEXTUREFACTOR, xb_rs(XRS_TEXTUREFACTOR));
    rs(D3DRS_POINTSIZE, xb_rs(XRS_POINTSIZE));
    rs(D3DRS_POINTSIZE_MIN, xb_rs(XRS_POINTSIZE_MIN));
    rs(D3DRS_POINTSIZE_MAX, xb_rs(XRS_POINTSIZE_MAX));
    rs(D3DRS_POINTSPRITEENABLE, xb_rs(XRS_POINTSPRITEENABLE) != 0);
    rs(D3DRS_POINTSCALEENABLE, xb_rs(XRS_POINTSCALEENABLE) != 0);
    rs(D3DRS_POINTSCALE_A, xb_rs(XRS_POINTSCALE_A));
    rs(D3DRS_POINTSCALE_B, xb_rs(XRS_POINTSCALE_B));
    rs(D3DRS_POINTSCALE_C, xb_rs(XRS_POINTSCALE_C));
    /* Depth bias: the polygon offset states as GL has them, ZBIAS (0..16,
     * larger is nearer) on top.
     * ponytail: ZBIAS as 1/65536 of depth per step; tune against xemu. */
    {
        int solid = xb_rs(XRS_SOLIDOFFSETENABLE) != 0;
        float slope = solid ? f32(xb_rs(XRS_POLYGONOFFSETZSLOPESCALE)) : 0.0f;
        float units = solid ? f32(xb_rs(XRS_POLYGONOFFSETZOFFSET)) / 16777216.0f : 0.0f;
        units -= (float)zbias / 65536.0f;
        rs(D3DRS_SLOPESCALEDEPTHBIAS, u32f(slope));
        rs(D3DRS_DEPTHBIAS, u32f(units));
    }
}

static void apply_stages(void)
{
    int st;
    for (st = 0; st < 4; st++) {
        IDirect3DBaseTexture9 *t = NULL;
        float lw = 0, lh = 0;

        if (s_stage_tex[st])
            t = tex_get(s_stage_tex[st], s_stage_pal[st], &lw, &lh);
        s_lin[st][0] = lw ? 1.0f / lw : 0;
        s_lin[st][1] = lh ? 1.0f / lh : 0;
        IDirect3DDevice9_SetTexture(s_dev, st, t);
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_ADDRESSU, tr_address(xb_tss(st, XTSS_ADDRESSU)));
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_ADDRESSV, tr_address(xb_tss(st, XTSS_ADDRESSV)));
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_ADDRESSW, tr_address(xb_tss(st, XTSS_ADDRESSW)));
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_MAGFILTER, tr_filter(xb_tss(st, XTSS_MAGFILTER)));
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_MINFILTER, tr_filter(xb_tss(st, XTSS_MINFILTER)));
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_MIPFILTER, tr_filter(xb_tss(st, XTSS_MIPFILTER)));
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_MIPMAPLODBIAS, xb_tss(st, XTSS_MIPMAPLODBIAS));
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_MAXMIPLEVEL, xb_tss(st, XTSS_MAXMIPLEVEL));
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_MAXANISOTROPY, xb_tss(st, XTSS_MAXANISOTROPY) ? xb_tss(st, XTSS_MAXANISOTROPY) : 1);
        IDirect3DDevice9_SetSamplerState(s_dev, st, D3DSAMP_BORDERCOLOR, xb_tss(st, XTSS_BORDERCOLOR));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_COLOROP, tr_texop(xb_tss(st, XTSS_COLOROP)));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_COLORARG0, xb_tss(st, XTSS_COLORARG0));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_COLORARG1, xb_tss(st, XTSS_COLORARG1));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_COLORARG2, xb_tss(st, XTSS_COLORARG2));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_ALPHAOP, tr_texop(xb_tss(st, XTSS_ALPHAOP)));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_ALPHAARG0, xb_tss(st, XTSS_ALPHAARG0));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_ALPHAARG1, xb_tss(st, XTSS_ALPHAARG1));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_ALPHAARG2, xb_tss(st, XTSS_ALPHAARG2));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_RESULTARG, xb_tss(st, XTSS_RESULTARG));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_TEXCOORDINDEX, xb_tss(st, XTSS_TEXCOORDINDEX));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_TEXTURETRANSFORMFLAGS, xb_tss(st, XTSS_TEXTURETRANSFORMFLAGS));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_BUMPENVMAT00, xb_tss(st, XTSS_BUMPENVMAT00));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_BUMPENVMAT01, xb_tss(st, XTSS_BUMPENVMAT01));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_BUMPENVMAT10, xb_tss(st, XTSS_BUMPENVMAT10));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_BUMPENVMAT11, xb_tss(st, XTSS_BUMPENVMAT11));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_BUMPENVLSCALE, xb_tss(st, XTSS_BUMPENVLSCALE));
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_BUMPENVLOFFSET, xb_tss(st, XTSS_BUMPENVLOFFSET));
    }
}

static void apply_fixed_function(void)
{
    static const D3DTRANSFORMSTATETYPE xf_map[10] = {
        D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_TEXTURE0, D3DTS_TEXTURE1,
        D3DTS_TEXTURE2, D3DTS_TEXTURE3, D3DTS_WORLDMATRIX(0),
        D3DTS_WORLDMATRIX(1), D3DTS_WORLDMATRIX(2), D3DTS_WORLDMATRIX(3),
    };
    int i;
    for (i = 0; i < 10; i++) {
        if (!(s_xf_dirty & (1u << i)))
            continue;
        if (i == 1) {
            /* The title draws its movie quad exactly on the far plane with
             * ZFUNC LESS against a depth buffer cleared to 1.0. The NV2A's
             * integer depth rounds that below 1; D3D9 compares (and clips) in
             * float. Scaling projected z by a hair under 1 keeps the order of
             * everything else. */
            D3DMATRIX p = s_xf[1];
            const float k = 1.0f - 1.0f / 4194304.0f;
            p._13 *= k; p._23 *= k; p._33 *= k; p._43 *= k;
            IDirect3DDevice9_SetTransform(s_dev, D3DTS_PROJECTION, &p);
            continue;
        }
        IDirect3DDevice9_SetTransform(s_dev, xf_map[i], &s_xf[i]);
    }
    s_xf_dirty = 0;
    for (i = 0; i < 8; i++)
        if (s_light_dirty & (1u << i))
            IDirect3DDevice9_SetLight(s_dev, i, &s_light[i]);
    s_light_dirty = 0;
    for (i = 0; i < 8; i++)
        if (s_light_on_dirty & (1u << i))
            IDirect3DDevice9_LightEnable(s_dev, i, (s_light_on >> i) & 1);
    s_light_on_dirty = 0;
    if (s_material_dirty)
        IDirect3DDevice9_SetMaterial(s_dev, &s_material);
    s_material_dirty = 0;
}

/* -- Draws ----------------------------------------------------------------- */

static uint32_t fvf_stride(uint32_t fvf)
{
    uint32_t n = 0, i, tc = (fvf >> 8) & 0xF;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: n = 12; break;
    case D3DFVF_XYZRHW: n = 16; break;
    case D3DFVF_XYZB1: n = 16; break;
    case D3DFVF_XYZB2: n = 20; break;
    case D3DFVF_XYZB3: n = 24; break;
    case D3DFVF_XYZB4: n = 28; break;
    }
    if (fvf & D3DFVF_NORMAL) n += 12;
    if (fvf & D3DFVF_PSIZE) n += 4;
    if (fvf & D3DFVF_DIFFUSE) n += 4;
    if (fvf & D3DFVF_SPECULAR) n += 4;
    for (i = 0; i < tc; i++) {
        uint32_t sz = (fvf >> (16 + 2 * i)) & 3;
        n += sz == 0 ? 8 : sz == 1 ? 12 : sz == 2 ? 16 : 4;
    }
    return n;
}

/* Byte offset of texture coordinate set `set` in an FVF vertex. */
static uint32_t fvf_tc_offset(uint32_t fvf, uint32_t set)
{
    uint32_t n = fvf_stride(fvf & ~0xFFFF0F00u), i;     /* everything before the texcoords */
    for (i = 0; i < set; i++) {
        uint32_t sz = (fvf >> (16 + 2 * i)) & 3;
        n += sz == 0 ? 8 : sz == 1 ? 12 : sz == 2 ? 16 : 4;
    }
    return n;
}

static D3DPRIMITIVETYPE prim_count(uint32_t xprim, uint32_t n, UINT *count)
{
    switch (xprim) {
    case 1: *count = n; return D3DPT_POINTLIST;
    case 2: *count = n / 2; return D3DPT_LINELIST;
    case 4: *count = n ? n - 1 : 0; return D3DPT_LINESTRIP;
    case 5: *count = n / 3; return D3DPT_TRIANGLELIST;
    case 6: case 9: *count = n > 2 ? n - 2 : 0; return D3DPT_TRIANGLESTRIP;  /* quad strip: same order */
    case 7: case 10: *count = n > 2 ? n - 2 : 0; return D3DPT_TRIANGLEFAN;   /* polygon: convex */
    }
    *count = 0;
    return D3DPT_TRIANGLELIST;
}

static uint8_t *s_scratch;
static size_t s_scratch_cap;
static uint16_t *s_qidx;
static size_t s_qidx_cap;

static void *scratch(size_t n)
{
    if (n > s_scratch_cap) {
        free(s_scratch);
        s_scratch_cap = n * 2;
        s_scratch = (uint8_t *)malloc(s_scratch_cap);
    }
    return s_scratch;
}

/* Copies vertices that need patching: pre-transformed positions scale with
 * the render target, and linear textures are addressed in texels. */
static const void *fixup_vertices(const void *verts, uint32_t first, uint32_t nverts, uint32_t stride)
{
    uint32_t fvf = s_vshader, st, v;
    int rhw = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW && s_scale != 1;
    int lin = 0;
    uint8_t *out;

    for (st = 0; st < 4; st++)
        if (s_lin[st][0] && ((xb_tss(st, XTSS_TEXCOORDINDEX) & 0xFFFF) < ((fvf >> 8) & 0xF)))
            lin = 1;
    if (!rhw && !lin)
        return verts;
    out = (uint8_t *)scratch((size_t)(first + nverts) * stride);
    memcpy(out + (size_t)first * stride, (const uint8_t *)verts + (size_t)first * stride,
           (size_t)nverts * stride);
    for (v = first; v < first + nverts; v++) {
        uint8_t *p = out + (size_t)v * stride;
        if (rhw) {
            /* Pixel centres sit on integers in both spaces. */
            float *xy = (float *)p;
            xy[0] = (xy[0] + 0.5f) * s_scale - 0.5f;
            xy[1] = (xy[1] + 0.5f) * s_scale - 0.5f;
        }
        for (st = 0; st < 4 && lin; st++) {
            uint32_t set = xb_tss(st, XTSS_TEXCOORDINDEX) & 0xFFFF;
            float *tc;
            if (!s_lin[st][0] || set >= ((fvf >> 8) & 0xF))
                continue;
            tc = (float *)(p + fvf_tc_offset(fvf, set));
            tc[0] *= s_lin[st][0];
            if (((fvf >> (16 + 2 * set)) & 3) != 3)
                tc[1] *= s_lin[st][1];
        }
    }
    return out;
}

/* -- Programmable vertex shaders ------------------------------------------ */

/* The game assembles xvs.1.1 source at run time; CreateVertexShader gets
 * NV2A microcode and a D3DVSD declaration. The microcode goes through the
 * translator below to HLSL, compiled for vs_2_0 so the
 * texture stages can stay fixed function. The declaration's streams are
 * expanded to float4 per register on the CPU: several Xbox types
 * (NORMPACKED3, FLOAT2H, the PBYTEs) have no D3D9 equivalent. */

#define NV2A_VS_MAX_INSTRUCTIONS 136
#define NV2A_VS_MAX_CONSTANTS    192

typedef struct {
    uint32_t handle;
    uint32_t decl[96];
    uint32_t mc[NV2A_VS_MAX_INSTRUCTIONS * 4];
    int nmc;
    int tried;
    uint16_t inputs;                  /* registers the program reads */
    int uses_vp;                      /* applies D3D's viewport epilogue */
    IDirect3DVertexShader9 *vs;
} HostVs;

#define MAX_VS 128
static HostVs s_vs[MAX_VS];
static int s_nvs;
static float s_vsc[NV2A_VS_MAX_CONSTANTS][4];
static int s_vsc_dirty = 1;


static HostVs *vs_find(uint32_t handle)
{
    int i;
    for (i = 0; i < s_nvs; i++)
        if (s_vs[i].handle == handle)
            return &s_vs[i];
    return NULL;
}


/* A vs_2_0 HLSL program for `n` instructions (see vsh_body). */
static void vsh_to_hlsl(const uint32_t *mc, int n, Sb *s, uint16_t *inputs, int *uses_vp)
{
    size_t body;

    sb(s, "float4 c[192] : register(c0);\n"
          "float4 hle_ts[4] : register(c192);\n"
          "float4 hle_fog : register(c196);\n"   /* table mode, start, end, density */
          "float nv_fog(float d) { d = abs(d); return hle_fog.x == 3 ? (hle_fog.z - d) / (hle_fog.z - hle_fog.y)\n"
          "  : hle_fog.x == 1 ? exp(-d * hle_fog.w) : hle_fog.x == 2 ? exp(-(d * hle_fog.w) * (d * hle_fog.w)) : 1.0 - d; }\n"
          "float rcc(float x) { float f = 1.0 / x; float m = clamp(abs(f), 5.42101e-20, 1.884467e19); return f < 0 ? -m : m; }\n"
          "float4 nv_log(float x) { float a = abs(x); float e = floor(log2(a)); return a == 0 ? float4(-1e38, 1, -1e38, 1) : float4(e, a / exp2(e), log2(a), 1); }\n"
          "float4 nv_lit(float4 c) { float p = clamp(c.w, -127.9961, 127.9961); return float4(1, max(c.x, 0), c.x > 0 ? pow(max(c.y, 0), p) : 0, 1); }\n"
          "struct VS_OUT { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
          "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float fog : FOG; };\n"
          "VS_OUT main(");
    body = s->len;                       /* the parameter list goes here */
    sb(s, "@PARAMS@) {\n"
          "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0, R8 = 0, R9 = 0, R10 = 0, R11 = 0, R12 = 0;\n"
          "  float4 oD0 = float4(0, 0, 0, 1), oD1 = float4(0, 0, 0, 1), oFog = 0;\n"
          "  float4 oT0 = float4(0, 0, 0, 1), oT1 = float4(0, 0, 0, 1), oT2 = float4(0, 0, 0, 1), oT3 = float4(0, 0, 0, 1);\n"
          "  int a0 = 0;\n");
    (void)body;
    vsh_body(mc, n, s, inputs, uses_vp, 0);
    /* D3D's epilogue left x, y, z divided by w (c[58] and c[59] are identity,
     * set at draw time); multiply back to clip space. */
    sb(s, "  VS_OUT o;\n"
          "  o.pos = %s;\n"
          "  o.d0 = saturate(oD0); o.d1 = saturate(oD1); o.fog = saturate(nv_fog(oFog.x));\n"
          "  o.t0 = oT0 * hle_ts[0]; o.t1 = oT1 * hle_ts[1]; o.t2 = oT2 * hle_ts[2]; o.t3 = oT3 * hle_ts[3];\n"
          "  return o;\n}\n",
       *uses_vp ? "float4(R12.xyz * R12.w, R12.w)" : "R12");
    {
        /* Now the inputs are known: fill in the parameter list. */
        char params[1024];
        size_t pl = 0;
        int k;
        params[0] = 0;
        for (k = 0; k < 16; k++)
            if (*inputs & (1u << k))
                pl += (size_t)snprintf(params + pl, sizeof params - pl, "%sfloat4 v%d : TEXCOORD%d",
                                       pl ? ", " : "", k, k);
        if (!*inputs)
            pl += (size_t)snprintf(params + pl, sizeof params - pl, "float4 v0 : TEXCOORD0");
        str_replace(s->buf, s->cap, "@PARAMS@", params);
        s->len = strlen(s->buf);
    }
}

static char s_hlsl[256 * 1024];

static void vs_compile(HostVs *h)
{
    ID3DBlob *code = NULL, *err = NULL;
    const char *dump = getenv("RECOMP_D3D9_DUMP_VSH");
    Sb s = { s_hlsl, sizeof s_hlsl, 0 };

    s_st.vs_compiles++;
    h->tried = 1;
    s_hlsl[0] = 0;
    vsh_to_hlsl(h->mc, h->nmc, &s, &h->inputs, &h->uses_vp);
    if (dump) {
        char path[512];
        FILE *f;
        snprintf(path, sizeof path, "%s/vs_%08X.hlsl", dump, h->handle);
        if ((f = fopen(path, "w")) != NULL) { fputs(s_hlsl, f); fclose(f); }
    }
    if (FAILED(D3DCompile(s_hlsl, strlen(s_hlsl), "nv2a_vsh", NULL, NULL, "main", "vs_2_0",
                          0, 0, &code, &err))) {
        fprintf(stderr, "[D3D9] vertex shader %08X does not compile: %s\n", h->handle,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
    } else {
        IDirect3DDevice9_CreateVertexShader(s_dev, (const DWORD *)ID3D10Blob_GetBufferPointer(code), &h->vs);
    }
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
}

/* The program's inputs as one interleaved float4 stream, one slot per
 * register, in register order. */
#define MAX_VDECL 64
static IDirect3DVertexDeclaration9 *s_vdecl[MAX_VDECL];
static uint16_t s_vdecl_key[MAX_VDECL];
static int s_nvdecl;

static IDirect3DVertexDeclaration9 *vdecl_get(uint16_t regs)
{
    D3DVERTEXELEMENT9 el[17];
    D3DVERTEXELEMENT9 end = D3DDECL_END();
    int i, n = 0;
    for (i = 0; i < s_nvdecl; i++)
        if (s_vdecl_key[i] == regs)
            return s_vdecl[i];
    for (i = 0; i < 16; i++)
        if (regs & (1u << i)) {
            el[n].Stream = 0; el[n].Offset = (WORD)(16 * n); el[n].Type = D3DDECLTYPE_FLOAT4;
            el[n].Method = D3DDECLMETHOD_DEFAULT; el[n].Usage = D3DDECLUSAGE_TEXCOORD;
            el[n].UsageIndex = (BYTE)i;
            n++;
        }
    el[n] = end;
    if (s_nvdecl == MAX_VDECL)
        return NULL;
    s_vdecl_key[s_nvdecl] = regs;
    IDirect3DDevice9_CreateVertexDeclaration(s_dev, el, &s_vdecl[s_nvdecl]);
    return s_vdecl[s_nvdecl++];
}

static uint8_t *s_vsbuf;
static size_t s_vsbuf_cap;

/* Vertices base+[lo, hi] for a program, as float4 slots. Stream 0 is `up`
 * when the draw supplies it. Returns the buffer (index 0 = vertex base)
 * and its stride. Registers the program reads that no stream supplies get
 * (0,0,0,1). */
static const void *vs_vertices(HostVs *h, uint32_t base, uint32_t lo, uint32_t hi,
                               const uint8_t *up, uint32_t up_stride, uint32_t *stride_out)
{
    VsInput in[16];
    int n = vs_inputs(h->decl, in), slot[16], ns = 0, k;
    uint16_t regs = h->inputs;
    uint32_t i, stride;
    const uint8_t *src[16];
    uint32_t sstride[16];

    for (k = 0; k < n; k++)
        regs |= 1u << in[k].reg;
    for (k = 0; k < 16; k++)
        slot[k] = (regs & (1u << k)) ? ns++ : -1;
    if (!ns) {
        regs = 1;
        slot[0] = ns++;
    }
    stride = 16 * (uint32_t)ns;
    if ((size_t)(hi + 1) * stride > s_vsbuf_cap) {
        free(s_vsbuf);
        s_vsbuf_cap = (size_t)(hi + 1) * stride * 2;
        s_vsbuf = (uint8_t *)malloc(s_vsbuf_cap);
    }
    for (k = 0; k < 16; k++) {
        if (k == 0 && up) {
            src[k] = up; sstride[k] = up_stride;
        } else {
            src[k] = s_stream_vb[k] ? (const uint8_t *)gp(res_data(s_stream_vb[k])) : NULL;
            sstride[k] = s_stream_stride[k];
        }
    }
    for (i = lo; i <= hi; i++) {
        float *o = (float *)(s_vsbuf + (size_t)i * stride);
        for (k = 0; k < ns; k++) {
            o[k * 4] = o[k * 4 + 1] = o[k * 4 + 2] = 0.0f; o[k * 4 + 3] = 1.0f;
        }
        for (k = 0; k < n; k++) {
            const uint8_t *s = src[in[k].stream];
            if (s)
                vsdt_read(in[k].type, s + (size_t)(base + i) * sstride[in[k].stream] + in[k].offset,
                          o + slot[in[k].reg] * 4);
        }
    }
    IDirect3DDevice9_SetVertexDeclaration(s_dev, vdecl_get(regs));
    *stride_out = stride;
    return s_vsbuf;
}

static void vs_bind(HostVs *h)
{
    float ts[4][4];
    int st;
    if (s_vsc_dirty) {
        /* D3D loads the viewport into c[58] (scale) and c[59] (offset)
         * itself; identity leaves the epilogue's divide by w, which the
         * translated program undoes. */
        s_vsc[58][0] = s_vsc[58][1] = s_vsc[58][2] = s_vsc[58][3] = 1.0f;
        s_vsc[59][0] = s_vsc[59][1] = s_vsc[59][2] = s_vsc[59][3] = 0.0f;
        IDirect3DDevice9_SetVertexShaderConstantF(s_dev, 0, &s_vsc[0][0], NV2A_VS_MAX_CONSTANTS);
        s_vsc_dirty = 0;
    }
    for (st = 0; st < 4; st++) {
        ts[st][0] = s_lin[st][0] ? s_lin[st][0] : 1.0f;
        ts[st][1] = s_lin[st][1] ? s_lin[st][1] : 1.0f;
        ts[st][2] = ts[st][3] = 1.0f;
        /* Stage n of a program samples oTn. */
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_TEXCOORDINDEX, st);
        IDirect3DDevice9_SetTextureStageState(s_dev, st, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
    }
    IDirect3DDevice9_SetVertexShaderConstantF(s_dev, 192, &ts[0][0], 4);
    {
        /* A program's oFog is a fog coordinate that the NV2A puts through the
         * table mode; D3D9 takes a shader's fog output as the final factor.
         * So the program computes the factor and D3D9 only blends it. */
        float fog[4];
        fog[0] = (float)xb_rs(XRS_FOGTABLEMODE);
        fog[1] = f32(xb_rs(XRS_FOGSTART));
        fog[2] = f32(xb_rs(XRS_FOGEND));
        fog[3] = f32(xb_rs(XRS_FOGDENSITY));
        IDirect3DDevice9_SetVertexShaderConstantF(s_dev, 196, fog, 1);
        rs(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        rs(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    }
    IDirect3DDevice9_SetVertexShader(s_dev, h->vs);
}

static HostVs *s_draw_vs;           /* the program of the draw in flight */

static int prepare_draw(void)
{
    if (!dev_ready())
        return 0;
    s_draw_vs = NULL;
    if (s_vshader & 1) {
        HostVs *h = vs_find(s_vshader);
        if (h && !h->tried)
            vs_compile(h);
        if (!h || !h->vs)
            return 0;
        s_draw_vs = h;
    }
    if (s_pshader) {
        static int said;
        if (!said++)
            fprintf(stderr, "[D3D9] pixel shaders not done yet; using texture stages\n");
    }
    bind_targets();
    apply_render_state();
    apply_stages();
    apply_fixed_function();
    IDirect3DDevice9_SetPixelShader(s_dev, NULL);
    if (s_draw_vs) {
        vs_bind(s_draw_vs);
    } else {
        IDirect3DDevice9_SetVertexShader(s_dev, NULL);
        IDirect3DDevice9_SetFVF(s_dev, s_vshader);
    }
    return 1;
}

/* One draw. Vertices come from `up` (stream 0 given with the call) or the
 * bound streams, starting at vertex `base`; idx == NULL draws [0, n). */
static void draw(uint32_t xprim, uint32_t n, const uint16_t *idx,
                 const uint8_t *up, uint32_t up_stride, uint32_t base)
{
    UINT count;
    D3DPRIMITIVETYPE pt;
    uint32_t lo = 0, hi = n ? n - 1 : 0, i, stride;
    const void *verts;

    if (!n || !prepare_draw())
        return;
    if (idx) {
        lo = 0xFFFF; hi = 0;
        for (i = 0; i < n; i++) {
            if (idx[i] < lo) lo = idx[i];
            if (idx[i] > hi) hi = idx[i];
        }
    }
    if (s_draw_vs) {
        verts = vs_vertices(s_draw_vs, base, lo, hi, up, up_stride, &stride);
    } else {
        stride = up ? up_stride : s_stream_stride[0];
        if (up)
            verts = up;
        else if (s_stream_vb[0])
            verts = (const uint8_t *)gp(res_data(s_stream_vb[0])) + (size_t)base * stride;
        else
            return;
        if (!stride)
            return;
        verts = fixup_vertices(verts, lo, hi - lo + 1, stride);
    }
    if (xprim == 8 || xprim == 3) {
        /* Quads and line loops have no D3D9 primitive: index them. */
        uint32_t m = xprim == 8 ? n / 4 * 6 : n + 1, q;
        if (m > s_qidx_cap) {
            free(s_qidx);
            s_qidx_cap = m * 2;
            s_qidx = (uint16_t *)malloc(s_qidx_cap * sizeof *s_qidx);
        }
        if (xprim == 8) {
            for (q = 0; q < n / 4; q++) {
                uint16_t a = idx ? idx[q * 4] : (uint16_t)(q * 4);
                uint16_t b = idx ? idx[q * 4 + 1] : (uint16_t)(q * 4 + 1);
                uint16_t c = idx ? idx[q * 4 + 2] : (uint16_t)(q * 4 + 2);
                uint16_t d = idx ? idx[q * 4 + 3] : (uint16_t)(q * 4 + 3);
                uint16_t *o = s_qidx + q * 6;
                o[0] = a; o[1] = b; o[2] = c; o[3] = a; o[4] = c; o[5] = d;
            }
            pt = D3DPT_TRIANGLELIST;
            count = n / 4 * 2;
        } else {
            for (q = 0; q < n; q++)
                s_qidx[q] = idx ? idx[q] : (uint16_t)q;
            s_qidx[n] = s_qidx[0];
            pt = D3DPT_LINESTRIP;
            count = n;
        }
        IDirect3DDevice9_DrawIndexedPrimitiveUP(s_dev, pt, lo, hi - lo + 1, count,
                                                s_qidx, D3DFMT_INDEX16, verts, stride);
        s_st.draws++;
        s_st.prims += count;
        return;
    }
    pt = prim_count(xprim, n, &count);
    if (!count)
        return;
    if (idx)
        IDirect3DDevice9_DrawIndexedPrimitiveUP(s_dev, pt, lo, hi - lo + 1, count,
                                                idx, D3DFMT_INDEX16, verts, stride);
    else
        IDirect3DDevice9_DrawPrimitiveUP(s_dev, pt, count, verts, stride);
    s_st.draws++;
    s_st.prims += count;
}

/* -- Present ----------------------------------------------------------------- */

static int sc_ready(HWND hwnd)
{
    RECT cr;
    UINT w, h;

    if (!GetClientRect(hwnd, &cr))
        return 0;
    w = (UINT)(cr.right - cr.left);
    h = (UINT)(cr.bottom - cr.top);
    if (!w || !h)
        return 0;                                  /* minimised */
    if (s_sc && w == s_sc_w && h == s_sc_h)
        return 1;
    if (s_sc) IDirect3DSwapChain9_Release(s_sc);
    s_sc = NULL;
    {
        D3DPRESENT_PARAMETERS pp;
        memset(&pp, 0, sizeof pp);
        pp.BackBufferWidth = w;
        pp.BackBufferHeight = h;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = hwnd;
        pp.Windowed = TRUE;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        if (FAILED(IDirect3DDevice9_CreateAdditionalSwapChain(s_dev, &pp, &s_sc)))
            return 0;
    }
    s_sc_w = w;
    s_sc_h = h;
    return 1;
}

static void dump_bmp(HostRt *r)
{
    static const char *prefix = (const char *)-1;
    static int every, seq;
    IDirect3DSurface9 *sys = NULL;
    D3DLOCKED_RECT lr;
    uint32_t W = r->w, H = r->h, x, y, sx, sy;
    char path[512];
    FILE *f;

    if (prefix == (const char *)-1) {
        const char *e = getenv("RECOMP_D3D9_DUMP_EVERY");
        prefix = getenv("RECOMP_D3D9_DUMP");
        every = e ? atoi(e) : 600;
    }
    if (!prefix || !every || s_frame % every != 0)
        return;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(s_dev, W * s_scale, H * s_scale,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL)))
        return;
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(s_dev, r->surf, sys))
        && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        uint32_t row = W * 3, pad = (4 - (row & 3)) & 3, filesz = 54 + (row + pad) * H;
        uint8_t hdr[54] = { 'B', 'M' };
        memcpy(hdr + 2, &filesz, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &W, 4);
        memcpy(hdr + 22, &H, 4);
        hdr[26] = 1; hdr[28] = 24;
        snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
        if ((f = fopen(path, "wb")) != NULL) {
            fwrite(hdr, 1, sizeof hdr, f);
            /* Box-filter down to 1x, rows bottom-up. */
            for (y = H; y-- > 0; ) {
                for (x = 0; x < W; x++) {
                    uint32_t acc[3] = { 0, 0, 0 }, n = s_scale * s_scale;
                    uint8_t bgr[3];
                    for (sy = 0; sy < s_scale; sy++)
                        for (sx = 0; sx < s_scale; sx++) {
                            const uint8_t *p = (const uint8_t *)lr.pBits
                                + (size_t)(y * s_scale + sy) * lr.Pitch + (x * s_scale + sx) * 4;
                            acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
                        }
                    bgr[0] = (uint8_t)(acc[0] / n); bgr[1] = (uint8_t)(acc[1] / n); bgr[2] = (uint8_t)(acc[2] / n);
                    fwrite(bgr, 1, 3, f);
                }
                fwrite("\0\0\0", 1, pad, f);
            }
            fclose(f);
            fprintf(stderr, "[D3D9] frame dump %s\n", path);
        }
        IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
}

/* The buffer the title just finished (guest address), fitted into the window. */
static void present(uint32_t data)
{
    HWND hwnd = (HWND)xbox_FramebufferWindowHwnd();
    HostRt *r = data ? rt_find(data) : NULL;
    IDirect3DSurface9 *bb = NULL;

    if (!r)
        r = s_cur_rt;
    rt_sync(r, 1);
    s_frame++;
    {
        /* RECOMP_D3D9_STATS=1: presents per second, once a second. */
        static int stats = -1;
        static DWORD t0;
        static uint32_t f0;
        DWORD now = GetTickCount();
        if (stats < 0)
            stats = getenv("RECOMP_D3D9_STATS") && strcmp(getenv("RECOMP_D3D9_STATS"), "0") != 0;
        if (stats && now - t0 >= 1000) {
            if (t0)
                fprintf(stderr, "[D3D9] %.1f presents/s\n", (s_frame - f0) * 1000.0 / (now - t0));
            t0 = now;
            f0 = s_frame;
        }
    }
    IDirect3DDevice9_EndScene(s_dev);
    if (r && r->surf)
        dump_bmp(r);
    if (r && r->surf && hwnd && sc_ready(hwnd)
        && SUCCEEDED(IDirect3DSwapChain9_GetBackBuffer(s_sc, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        RECT fit;
        UINT fw = s_sc_w, fh = s_sc_w * r->h / r->w;
        if (fh > s_sc_h) { fh = s_sc_h; fw = s_sc_h * r->w / r->h; }
        fit.left = (LONG)(s_sc_w - fw) / 2;
        fit.top = (LONG)(s_sc_h - fh) / 2;
        fit.right = fit.left + (LONG)fw;
        fit.bottom = fit.top + (LONG)fh;
        IDirect3DDevice9_ColorFill(s_dev, bb, NULL, D3DCOLOR_XRGB(0, 0, 0));
        IDirect3DDevice9_StretchRect(s_dev, r->surf, NULL, bb, &fit, D3DTEXF_LINEAR);
        s_st.rts = s_nrt; s_st.rts_max = MAX_RT;
        s_st.texs = s_ntex; s_st.texs_max = MAX_TEX;
        s_st.vss = s_nvs; s_st.vss_max = MAX_VS;
        s_st.vdecls = s_nvdecl; s_st.vdecls_max = MAX_VDECL;
        s_st.scale = s_scale; s_st.sc_w = s_sc_w; s_st.sc_h = s_sc_h; s_st.frame = s_frame;
        s_st.hd_textures = s_hd_hits;
        {
            /* The pointer over the game's menus, in guest 640x480. */
            POINT pt;
            int gx = -1, gy = -1;
            if (GetForegroundWindow() == hwnd && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)
                && pt.x >= fit.left && pt.x < fit.right && pt.y >= fit.top && pt.y < fit.bottom) {
                gx = (int)((pt.x - fit.left) * 640 / (LONG)fw);
                gy = (int)((pt.y - fit.top) * 480 / (LONG)fh);
            }
            mkb_set_menu_cursor(menu_mouse_update(gx, gy));
        }
        overlay_present(s_dev, hwnd, bb, &s_st);
        IDirect3DSurface9_Release(bb);
        IDirect3DSwapChain9_Present(s_sc, NULL, NULL, NULL, NULL, 0);
    }
    IDirect3DDevice9_BeginScene(s_dev);
    memset(&s_st, 0, sizeof s_st);
}

/* -- Handlers ---------------------------------------------------------------- */

static void hle_Swap(void)                  /* Swap(Flags) */
{
    uint32_t flags = ARG(0);
    /* D3D flips by swapping Data between its buffer objects: take the back
     * buffer's address before the flip. */
    uint32_t bb = MEM32(xb_device() + 0x1A14), data = bb ? res_data(bb) : 0;

    sub_00200110_gen();                     /* D3D's own flip bookkeeping */
    if (flags == 0 || (flags & 4)) {
        /* A flip waits for vblank on the Xbox (presentation interval one).
         * Loading loops swap as fast as they can otherwise: at most one
         * present per 60 Hz frame. */
        static LARGE_INTEGER last, freq;
        LARGE_INTEGER now;
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        if (last.QuadPart) {
            LONGLONG due = last.QuadPart + freq.QuadPart / 60;
            while (now.QuadPart < due) {
                LONGLONG ms = (due - now.QuadPart) * 1000 / freq.QuadPart;
                Sleep(ms > 1 ? (DWORD)(ms - 1) : 0);
                QueryPerformanceCounter(&now);
            }
        }
        last = now;
        EnterCriticalSection(&s_lock);
        if (dev_ready())
            present(data);
        LeaveCriticalSection(&s_lock);
    }
}

/* Clear(Count, pRects, Flags, Color, Z, Stencil) */
static void hle_Clear(void)
{
    uint32_t count = ARG(0), rects = ARG(1), flags = ARG(2), color = ARG(3);
    float z = f32(ARG(4));
    uint32_t stencil = ARG(5);
    DWORD f = 0;
    D3DRECT r[16];
    uint32_t i;

    EnterCriticalSection(&s_lock);
    if (dev_ready()) {
        bind_targets();
        if (flags & 0xF0) f |= D3DCLEAR_TARGET;
        if (s_cur_ds && (flags & 1)) f |= D3DCLEAR_ZBUFFER;
        if (s_cur_ds && (flags & 2)) f |= D3DCLEAR_STENCIL;
        if (count > 16) count = 16;
        for (i = 0; i < count && rects; i++) {
            r[i].x1 = SMEM32(rects + 16 * i) * (LONG)s_scale;
            r[i].y1 = SMEM32(rects + 16 * i + 4) * (LONG)s_scale;
            r[i].x2 = SMEM32(rects + 16 * i + 8) * (LONG)s_scale;
            r[i].y2 = SMEM32(rects + 16 * i + 12) * (LONG)s_scale;
        }
        /* ponytail: a partial colour mask (one channel of 0xF0) clears all
         * four; not seen in this title. */
        if (f)
            IDirect3DDevice9_Clear(s_dev, rects ? count : 0, rects ? r : NULL, f,
                                   color, z, stencil);
        if ((f & D3DCLEAR_TARGET) && s_cur_rt) {
            for (i = 0; i < count && rects; i++) {
                r[i].x1 /= (LONG)s_scale; r[i].y1 /= (LONG)s_scale;
                r[i].x2 /= (LONG)s_scale; r[i].y2 /= (LONG)s_scale;
            }
            rt_fill(s_cur_rt, r, rects ? count : 0, color);
        }
    }
    LeaveCriticalSection(&s_lock);
    sub_001F91D0_gen();
}

static void hle_SetViewport(void)           /* SetViewport(pViewport) */
{
    uint32_t p = ARG(0);
    if (p) {
        s_vp.X = MEM32(p);
        s_vp.Y = MEM32(p + 4);
        s_vp.Width = MEM32(p + 8);
        s_vp.Height = MEM32(p + 12);
        s_vp.MinZ = f32(MEM32(p + 16));
        s_vp.MaxZ = f32(MEM32(p + 20));
    }
    sub_001F1590_gen();
}

static void hle_SetTransform(void)          /* SetTransform(State, pMatrix) */
{
    uint32_t st = ARG(0), p = ARG(1);
    if (st < 10 && p) {
        memcpy(&s_xf[st], gp(p), sizeof(D3DMATRIX));
        s_xf_dirty |= 1u << st;
    }
    sub_001EFB70_gen();
}

static void hle_SetLight(void)              /* SetLight(Index, pLight) */
{
    uint32_t i = ARG(0), p = ARG(1);
    if (i < 8 && p) {
        memcpy(&s_light[i], gp(p), sizeof(D3DLIGHT9));
        s_light_dirty |= 1u << i;
    }
    sub_001F17C0_gen();
}

static void hle_LightEnable(void)           /* LightEnable(Index, Enable) */
{
    uint32_t i = ARG(0);
    if (i < 8) {
        s_light_on = (s_light_on & ~(1u << i)) | ((ARG(1) != 0) << i);
        s_light_on_dirty |= 1u << i;
    }
    sub_001F1A80_gen();
}

static void hle_SetMaterial(void)           /* SetMaterial(pMaterial) */
{
    if (ARG(0)) {
        memcpy(&s_material, gp(ARG(0)), sizeof s_material);
        s_material_dirty = 1;
    }
    sub_001EFD60_gen();
}

static void hle_SetTexture(void)            /* SetTexture(Stage, pTexture) */
{
    if (ARG(0) < 4)
        s_stage_tex[ARG(0)] = ARG(1);
    sub_001F1C10_gen();
}

static void hle_SetPalette(void)            /* SetPalette(Stage, pPalette) */
{
    if (ARG(0) < 4)
        s_stage_pal[ARG(0)] = ARG(1);
    sub_001F1DF0_gen();
}

static void hle_SetStreamSource(void)       /* SetStreamSource(Stream, pVB, Stride) */
{
    if (ARG(0) < 16) {
        s_stream_vb[ARG(0)] = ARG(1);
        s_stream_stride[ARG(0)] = ARG(2);
    }
    sub_001F3AA0_gen();
}

static void hle_SetIndices(void)            /* SetIndices(pIB, BaseVertexIndex) */
{
    s_ib = ARG(0);
    s_base_vertex = ARG(1);
    sub_001F1E80_gen();
}

static void hle_SetVertexShader(void)       /* SetVertexShader(Handle) */
{
    s_vshader = ARG(0);
    sub_001F4000_gen();
}

static void hle_SetPixelShader(void)        /* SetPixelShader(Handle) */
{
    s_pshader = ARG(0);
    sub_00200AF0_gen();
}

/* DrawVerticesUP(PrimitiveType, VertexCount, pVertexStreamZeroData, Stride) */
static void hle_DrawVerticesUP(void)
{
    EnterCriticalSection(&s_lock);
    if (ARG(2))
        draw(ARG(0), ARG(1), NULL, (const uint8_t *)gp(ARG(2)), ARG(3), 0);
    LeaveCriticalSection(&s_lock);
    stdcall_ret(4, 0);
}

/* DrawIndexedVerticesUP(PrimitiveType, VertexCount, pIndexData,
 *                       pVertexStreamZeroData, Stride) */
static void hle_DrawIndexedVerticesUP(void)
{
    EnterCriticalSection(&s_lock);
    if (ARG(2) && ARG(3))
        draw(ARG(0), ARG(1), (const uint16_t *)gp(ARG(2)), (const uint8_t *)gp(ARG(3)), ARG(4), 0);
    LeaveCriticalSection(&s_lock);
    stdcall_ret(5, 0);
}

/* DrawVertices(PrimitiveType, StartVertex, VertexCount) */
static void hle_DrawVertices(void)
{
    EnterCriticalSection(&s_lock);
    draw(ARG(0), ARG(2), NULL, NULL, 0, ARG(1));
    LeaveCriticalSection(&s_lock);
    stdcall_ret(3, 0);
}

/* DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData): indices are
 * offset by SetIndices' BaseVertexIndex. */
static void hle_DrawIndexedVertices(void)
{
    EnterCriticalSection(&s_lock);
    if (ARG(2))
        draw(ARG(0), ARG(1), (const uint16_t *)gp(ARG(2)), NULL, 0, s_base_vertex);
    LeaveCriticalSection(&s_lock);
    stdcall_ret(3, 0);
}

/* CreateVertexShader(pDeclaration, pFunction, pHandle, Usage) */
static void hle_CreateVertexShader(void)
{
    uint32_t decl = ARG(0), func = ARG(1), ph = ARG(2);
    HostVs *h;
    int i;

    sub_001F34C0_gen();
    if (!ph || !MEM32(ph) || !func)
        return;                                 /* FVF-only: nothing to translate */
    EnterCriticalSection(&s_lock);
    h = vs_find(MEM32(ph));
    if (!h && s_nvs < MAX_VS)
        h = &s_vs[s_nvs++];
    if (h) {
        if (h->vs) IDirect3DVertexShader9_Release(h->vs);
        memset(h, 0, sizeof *h);
        h->handle = MEM32(ph);
        for (i = 0; i < 96; i++) {
            h->decl[i] = decl ? MEM32(decl + 4 * i) : 0xFFFFFFFFu;
            if (h->decl[i] == 0xFFFFFFFFu)
                break;
        }
        if (i == 96) h->decl[95] = 0xFFFFFFFFu;
        /* Header: program type in the low half, instruction count in the high. */
        h->nmc = (int)(MEM32(func) >> 16);
        if (h->nmc > NV2A_VS_MAX_INSTRUCTIONS) h->nmc = NV2A_VS_MAX_INSTRUCTIONS;
        memcpy(h->mc, gp(func + 4), (size_t)h->nmc * 16);
    }
    LeaveCriticalSection(&s_lock);
}

/* SetVertexShaderConstant1/4 take Register in ecx and pConstantData in edx;
 * NotInline adds a dword count on the stack. Register is already biased to
 * the hardware slot (0..191): the public API adds 96 before calling these. */
static void vsc_mirror(int32_t reg, uint32_t p, uint32_t count)
{
    int32_t i = reg;
    uint32_t k;
    for (k = 0; k < count && i + (int32_t)k < NV2A_VS_MAX_CONSTANTS && p; k++)
        if (i + (int32_t)k >= 0)
            memcpy(s_vsc[i + k], gp(p + 16 * k), 16);
    s_vsc_dirty = 1;
}

static void hle_SetVertexShaderConstant1(void)
{
    vsc_mirror((int32_t)g_ecx, g_edx, 1);
    sub_001F3620_gen();
}

static void hle_SetVertexShaderConstant4(void)
{
    vsc_mirror((int32_t)g_ecx, g_edx, 4);
    sub_001F36D0_gen();
}

static void hle_SetVertexShaderConstantNotInline(void)
{
    vsc_mirror((int32_t)g_ecx, g_edx, ARG(0) / 4);   /* the count is in dwords */
    sub_001F3860_gen();
}

/* -- Init -------------------------------------------------------------------- */

void d3d9_hle_init(void)
{
    const char *e = getenv("RECOMP_SCALE");
    int i;
    InitializeCriticalSection(&s_lock);
    for (i = 0; i < 10; i++) {          /* D3D's defaults: identity */
        memset(&s_xf[i], 0, sizeof s_xf[i]);
        s_xf[i]._11 = s_xf[i]._22 = s_xf[i]._33 = s_xf[i]._44 = 1.0f;
    }
    if (e && atoi(e) >= 1 && atoi(e) <= 8)
        s_scale = (uint32_t)atoi(e);
}

/* Not replaced yet: forward to the lifted body. */
#define hle_Direct3D_CreateDevice sub_001FF350_gen
#define hle_SetRenderTarget sub_001F0A90_gen
#define hle_GetBackBuffer2 sub_001F0E30_gen
#define hle_GetDepthStencilSurface2 sub_001F1570_gen
#define hle_CopyRects sub_001F1220_gen
#define hle_DeleteVertexShader sub_001F3EF0_gen
#define hle_CreatePixelShader sub_00200640_gen
#define hle_SetPixelShaderConstant sub_00200CF0_gen
#define hle_DeletePixelShader sub_00200690_gen
#define hle_SetShaderConstantMode sub_001F3D90_gen
#define hle_EnableOverlay sub_00201180_gen
#define hle_UpdateOverlay sub_00201000_gen
#define hle_GetOverlayUpdateStatus sub_00200FC0_gen
#define hle_MakeSpace sub_001EEF60_gen
#define hle_KickOff sub_001EEAD0_gen
#define hle_BlockOnFence sub_001EFF50_gen
#define hle_KickOffAndWaitForIdle sub_001EEFC0_gen
#define hle_InsertFence sub_001F0330_gen

void sub_001FF350(void) { hle_trace("Direct3D_CreateDevice", 6); hle_Direct3D_CreateDevice(); }
void sub_00200110(void) { hle_trace("Swap", 1); hle_Swap(); }
void sub_001F91D0(void) { hle_trace("Clear", 6); hle_Clear(); }
void sub_001F0A90(void) { hle_trace("SetRenderTarget", 2); hle_SetRenderTarget(); }
void sub_001F0E30(void) { hle_trace("GetBackBuffer2", 1); hle_GetBackBuffer2(); }
void sub_001F1570(void) { hle_trace("GetDepthStencilSurface2", 0); hle_GetDepthStencilSurface2(); }
void sub_001F1220(void) { hle_trace("CopyRects", 5); hle_CopyRects(); }
void sub_001F1590(void) { hle_trace("SetViewport", 1); hle_SetViewport(); }
void sub_001EFB70(void) { hle_trace("SetTransform", 2); hle_SetTransform(); }
void sub_001F17C0(void) { hle_trace("SetLight", 2); hle_SetLight(); }
void sub_001F1A80(void) { hle_trace("LightEnable", 2); hle_LightEnable(); }
void sub_001EFD60(void) { hle_trace("SetMaterial", 1); hle_SetMaterial(); }
void sub_001F1C10(void) { hle_trace("SetTexture", 2); hle_SetTexture(); }
void sub_001F1DF0(void) { hle_trace("SetPalette", 2); hle_SetPalette(); }
void sub_001F3AA0(void) { hle_trace("SetStreamSource", 3); hle_SetStreamSource(); }
void sub_001F1E80(void) { hle_trace("SetIndices", 2); hle_SetIndices(); }
void sub_001FEA80(void) { hle_trace("DrawVerticesUP", 4); hle_DrawVerticesUP(); }
void sub_001FEBF0(void) { hle_trace("DrawIndexedVerticesUP", 5); hle_DrawIndexedVerticesUP(); }
void sub_001FED30(void) { hle_trace("DrawVertices", 3); hle_DrawVertices(); }
void sub_001FEDD0(void) { hle_trace("DrawIndexedVertices", 3); hle_DrawIndexedVertices(); }
void sub_001F34C0(void) { hle_trace("CreateVertexShader", 4); hle_CreateVertexShader(); }
void sub_001F4000(void) { hle_trace("SetVertexShader", 1); hle_SetVertexShader(); }
void sub_001F3EF0(void) { hle_trace("DeleteVertexShader", 1); hle_DeleteVertexShader(); }
void sub_001F3620(void) { hle_trace("SetVertexShaderConstant1", 0); hle_SetVertexShaderConstant1(); }  /* ecx,edx */
void sub_001F36D0(void) { hle_trace("SetVertexShaderConstant4", 0); hle_SetVertexShaderConstant4(); }  /* ecx,edx */
void sub_001F3860(void) { hle_trace("SetVertexShaderConstantNotInline", 1); hle_SetVertexShaderConstantNotInline(); }  /* ecx,edx */
void sub_00200640(void) { hle_trace("CreatePixelShader", 2); hle_CreatePixelShader(); }
void sub_00200AF0(void) { hle_trace("SetPixelShader", 1); hle_SetPixelShader(); }
void sub_00200CF0(void) { hle_trace("SetPixelShaderConstant", 3); hle_SetPixelShaderConstant(); }
void sub_00200690(void) { hle_trace("DeletePixelShader", 1); hle_DeletePixelShader(); }
void sub_001F3D90(void) { hle_trace("SetShaderConstantMode", 1); hle_SetShaderConstantMode(); }
void sub_00201180(void) { hle_trace("EnableOverlay", 1); hle_EnableOverlay(); }
void sub_00201000(void) { hle_trace("UpdateOverlay", 5); hle_UpdateOverlay(); }
void sub_00200FC0(void) { hle_trace("GetOverlayUpdateStatus", 0); hle_GetOverlayUpdateStatus(); }
void sub_001EEF60(void) { hle_trace("MakeSpace", 0); hle_MakeSpace(); }
void sub_001EEAD0(void) { hle_trace("KickOff", 0); hle_KickOff(); }
void sub_001EFF50(void) { hle_trace("BlockOnFence", 1); hle_BlockOnFence(); }
void sub_001EEFC0(void) { hle_trace("KickOffAndWaitForIdle", 0); hle_KickOffAndWaitForIdle(); }
void sub_001F0330(void) { hle_trace("InsertFence", 0); hle_InsertFence(); }

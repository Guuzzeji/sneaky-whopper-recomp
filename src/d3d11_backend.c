/*
 * D3D11 render back end for the NV2A pushbuffer executor.
 *
 * The executor (xboxrecomp nv2a_pb_exec.c) decodes the title's pushbuffer and
 * transforms vertices on the CPU. By default it also rasterises every
 * triangle on the CPU, and on Sneak King that is what paces the game: the
 * executor thread sat at 90 % of a core and movies played at 15 fps instead of
 * 29.97, so their audio ran dry half the time. With this registered
 * (nv2a_backend_register) the executor hands the triangles here and the host
 * GPU fills them.
 *
 * Guest memory stays the source of truth. A colour surface is uploaded into a
 * render target the first time it is drawn to in a frame, and read back into
 * guest memory at the flip, or earlier when the title samples it as a
 * texture. So the framebuffer window, the frame dumps and anything else that
 * reads guest memory work as before; the cost is one 1.2 MB readback a frame.
 * Depth and stencil live only on the GPU.
 *
 * Everything runs on the NV2A poll thread, one call at a time, so there is no
 * locking. The device is offscreen: there is no swapchain.
 *
 * What the back end interface carries is what gets drawn: the register
 * combiners over all four texture stages, diffuse, specular and fog, then
 * blend, alpha test, depth and stencil (nv2a_backend.h).
 *
 * RECOMP_D3D11=0 turns it off and the software rasteriser draws as before.
 *
 * Higher resolution (RECOMP_SCALE=N, default 2): every render target and
 * depth buffer is N times the guest surface in each direction. Vertices
 * arrive in guest pixels and the viewport does the scaling, so nothing else
 * changes. Guest memory still gets the frame, box-filtered down to its own
 * size (rt_readback), and what the window shows comes straight from the
 * scaled target through a swap chain on the framebuffer window (present).
 *
 * The overlay (F1) is the D3D11 debugger: frame timings, what the back end
 * did this frame, its caches, every render target, and the debug layer's
 * messages (RECOMP_D3D11_DEBUG=1). F2 shows each render target in turn, F3
 * draws wireframe, F4 bypasses the register combiners (texel * diffuse).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "d3d8_swizzle.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xbox_FramebufferWindowHwnd(void);
extern void xbox_FramebufferWindowExternalPresent(int on);
extern int xbox_FramebufferKeyDown(int vk);

static uint32_t s_scale = 1;    /* render targets are this many times the guest's */

/* F1..F4, read at the flip. */
static struct { int overlay, view, wire, no_rc; } s_dbg;

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11DeviceContext1 *s_ctx1;          /* ClearView, for clip-rect clears */
static ID3D11VertexShader *s_vs;
static ID3D11InputLayout *s_layout;
static ID3D11Buffer *s_cb, *s_vb;
static UINT s_vb_bytes, s_vb_pos;

/* RECOMP_D3D11_STATS: what the executor thread spends its time on here.
 * Counts and QPC ticks, printed with the flip rate. */
typedef struct {
    uint32_t readbacks, uploads, decodes, hashes, draws;
    LONGLONG t_readback, t_upload, t_tex, t_draw, t_vb, t_cb;
} Stats;
static Stats s_st;

static LONGLONG qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)(xbox_GetMemoryOffset() + (ptrdiff_t)va);
}

/* ---- blits ------------------------------------------------------------- */

/* One textured triangle over the viewport: scaling a surface up after an
 * upload, down before a readback, onto the swap chain, and the overlay.
 * alpha < 0 keeps the texel's alpha (guest pixels); otherwise it is the
 * output alpha, blended over what is there when below 1. umax, vmax: how
 * much of the source to read (1, 1: all of it). */
static const char s_blit_hlsl[] =
"cbuffer B : register(b0) { float4 bk; };\n"
"Texture2D bt : register(t0); SamplerState bs : register(s0);\n"
"void vs(uint id : SV_VertexID, out float4 p : SV_Position, out float2 uv : TEXCOORD0) {\n"
"  uv = float2((id << 1) & 2, id & 2); p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
"float4 ps(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
"  float4 c = bt.Sample(bs, uv * bk.yz); return bk.x < 0 ? c : float4(c.rgb, bk.x); }\n";

static ID3D11VertexShader *s_blit_vs;
static ID3D11PixelShader *s_blit_ps;
static ID3D11Buffer *s_blit_cb;
static ID3D11SamplerState *s_point, *s_linear;
static ID3D11BlendState *s_over;

static void blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                 float x, float y, float w, float h, int linear, float alpha, float umax, float vmax)
{
    D3D11_VIEWPORT vp = { x, y, w, h, 0.0f, 1.0f };
    float k[4] = { alpha, umax, vmax, 0 };
    ID3D11ShaderResourceView *none = NULL;

    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_blit_cb, 0, NULL, k, 0, 0);
    /* The target first: it unbinds a surface the last draw left bound as a
     * target, which is what src may be. */
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &dst, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, alpha >= 0 && alpha < 1 ? s_over : NULL,
                                        NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(s_ctx, NULL);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_blit_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, linear ? &s_linear : &s_point);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
}

/* ---- colour surfaces --------------------------------------------------- */

/* tex is the surface at s_scale. With a scale above 1, lo is the same
 * surface at guest size: uploads land there and are scaled up into tex,
 * readbacks are scaled down into it and copied out through staging. */
typedef struct {
    uint32_t va, pitch, bpp, fmt, w, h;     /* w, h: guest pixels */
    ID3D11Texture2D *tex, *staging, *lo;
    ID3D11RenderTargetView *rtv, *lo_rtv;
    ID3D11ShaderResourceView *srv, *lo_srv;
    uint64_t hash;          /* guest memory as the last readback left it */
    int valid;              /* the GPU copy matches guest memory */
    int dirty;              /* the GPU copy is newer than guest memory */
} Rt;

#define MAX_RT 16
static Rt s_rt[MAX_RT];
static Rt *s_last_rt;       /* drawn into last: the frame a flip shows */

static void rt_release(Rt *r)
{
    if (r == s_last_rt) s_last_rt = NULL;
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    if (r->staging) ID3D11Texture2D_Release(r->staging);
    if (r->lo_rtv) ID3D11RenderTargetView_Release(r->lo_rtv);
    if (r->lo_srv) ID3D11ShaderResourceView_Release(r->lo_srv);
    if (r->lo) ID3D11Texture2D_Release(r->lo);
    memset(r, 0, sizeof *r);
}

static uint64_t hash_bytes(const uint8_t *p, uint32_t n);

/* 16-bit surface pixels to and from B8G8R8A8. Formats 1 and 2 are X1R5G5B5,
 * 3 and 0xA R5G6B5. */
static uint32_t px16_to_32(uint16_t p, uint32_t fmt)
{
    uint32_t r, g, b;
    if (fmt == 0x3 || fmt == 0xA) {
        r = (p >> 11) & 31; g = (p >> 5) & 63; b = p & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    } else {
        r = (p >> 10) & 31; g = (p >> 5) & 31; b = p & 31;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    }
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint16_t px32_to_16(uint32_t p, uint32_t fmt)
{
    uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    if (fmt == 0x3 || fmt == 0xA)
        return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

/* Guest memory into the GPU copy, unless it is still what the last readback
 * or upload left there: the GPU copy is then already right, and at a scale
 * above 1 it holds more than guest memory can, which a fresh upload would
 * throw away for a blurred copy of itself (a frame drawn without a colour
 * clear builds on the last one). */
static void rt_upload(Rt *r)
{
    const uint8_t *src = guest(r->va);
    ID3D11Texture2D *dst = r->lo ? r->lo : r->tex;
    LONGLONG t0 = qpc();
    uint64_t h = hash_bytes(src, r->pitch * r->h);

    if (h == r->hash) {
        r->valid = 1;
        r->dirty = 0;
        return;
    }
    r->hash = h;
    s_st.uploads++;
    if (r->bpp == 4) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)dst,
                                              0, NULL, src, r->pitch, 0);
    } else {
        uint32_t *tmp = (uint32_t *)malloc((size_t)r->w * r->h * 4), x, y;
        if (!tmp)
            return;
        for (y = 0; y < r->h; y++) {
            const uint16_t *row = (const uint16_t *)(src + (size_t)y * r->pitch);
            for (x = 0; x < r->w; x++)
                tmp[(size_t)y * r->w + x] = px16_to_32(row[x], r->fmt);
        }
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)dst,
                                              0, NULL, tmp, r->w * 4, 0);
        free(tmp);
    }
    if (r->lo)
        blit(r->lo_srv, r->rtv, 0, 0, (float)(r->w * s_scale), (float)(r->h * s_scale), 0, -1, 1, 1);
    r->valid = 1;
    r->dirty = 0;
    s_st.t_upload += qpc() - t0;
}

static void tex_touched(uint32_t va, uint32_t len);

static void rt_readback(Rt *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *dst = guest(r->va);
    uint32_t x, y;
    LONGLONG t0;

    if (!r->dirty)
        return;
    t0 = qpc();
    s_st.readbacks++;
    /* Down to guest size. Bilinear at the centre of each 2x2 block is the
     * box filter; at 3x and up it is the nearest few texels. */
    if (r->lo)
        blit(r->srv, r->lo_rtv, 0, 0, (float)r->w, (float)r->h, 1, -1, 1, 1);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                     (ID3D11Resource *)(r->lo ? r->lo : r->tex));
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return;
    for (y = 0; y < r->h; y++) {
        const uint32_t *s = (const uint32_t *)((const uint8_t *)m.pData
                                               + (size_t)y * m.RowPitch);
        uint8_t *d = dst + (size_t)y * r->pitch;
        if (r->bpp == 4)
            memcpy(d, s, (size_t)r->w * 4);
        else
            for (x = 0; x < r->w; x++)
                ((uint16_t *)d)[x] = px32_to_16(s[x], r->fmt);
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->staging, 0);
    r->dirty = 0;
    r->hash = hash_bytes(dst, r->pitch * r->h);
    tex_touched(r->va, r->pitch * r->h);
    s_st.t_readback += qpc() - t0;
}

/* The render target for a surface, created or grown as needed and holding
 * the guest's pixels. Sized to the whole surface (pitch wide, down to the
 * bottom of the clip), because vertices are in whole-surface pixels. */
static Rt *rt_get(const Nv2aSurface *s)
{
    uint32_t w, h, i;
    Rt *r = NULL, *free_slot = NULL;
    D3D11_TEXTURE2D_DESC td;

    if (!s->color_va || !s->pitch || (s->bytes_per_pixel != 2 && s->bytes_per_pixel != 4))
        return NULL;
    w = s->pitch / s->bytes_per_pixel;
    h = s->clip_y + s->height;
    if (!w || !h || w > 8192 || h > 8192)
        return NULL;

    for (i = 0; i < MAX_RT; i++) {
        if (s_rt[i].tex && s_rt[i].va == s->color_va && s_rt[i].pitch == s->pitch
            && s_rt[i].bpp == s->bytes_per_pixel) {
            r = &s_rt[i];
            break;
        }
        if (!s_rt[i].tex && !free_slot)
            free_slot = &s_rt[i];
    }
    if (r && r->h >= h) {
        r->fmt = s->format;
        if (!r->valid)
            rt_upload(r);
        return r;
    }
    if (r) {                                   /* taller now: start again */
        rt_readback(r);
        rt_release(r);
    } else if (free_slot) {
        r = free_slot;
    } else {
        /* ponytail: evict slot 0 when all 16 are in use. A title that renders
         * to more surfaces than that a frame wants an LRU here. */
        r = &s_rt[0];
        rt_readback(r);
        rt_release(r);
    }

    memset(&td, 0, sizeof td);
    td.Width = w * s_scale;
    td.Height = h * s_scale;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex)))
        return NULL;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv);
    td.Width = w;
    td.Height = h;
    if (s_scale > 1) {
        ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->lo);
        if (r->lo) {
            ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->lo, NULL, &r->lo_rtv);
            ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->lo, NULL, &r->lo_srv);
        }
    }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->staging);
    if (!r->staging || !r->rtv || !r->srv || (s_scale > 1 && (!r->lo_rtv || !r->lo_srv))) {
        rt_release(r);
        return NULL;
    }
    r->va = s->color_va;
    r->pitch = s->pitch;
    r->bpp = s->bytes_per_pixel;
    r->fmt = s->format;
    r->w = w;
    r->h = h;
    rt_upload(r);
    return r;
}

/* Before the CPU reads guest memory in [va, va+len): write back any render
 * target that overlaps it. A title that renders into a texture and then
 * samples it gets the pixels it drew. */
static void rt_flush_range(uint32_t va, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < MAX_RT; i++) {
        Rt *r = &s_rt[i];
        if (r->tex && r->dirty && va < r->va + r->pitch * r->h && r->va < va + len)
            rt_readback(r);
    }
}

/* ---- depth/stencil ----------------------------------------------------- */

/* D32 float + S8 rather than D24S8: depth arrives as the integer the NV2A
 * depth buffer would hold, and an integer below 2^24 divided by 2^24 is exact
 * in a float, so a quad at z 16777214 still passes LESS against a clear of
 * 16777215. D24 rounds the two together (the crosshatch over the movies in
 * P4 was the same rounding in the software path). */
typedef struct {
    uint32_t va, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
} Ds;

#define MAX_DS 8
static Ds s_ds[MAX_DS];

static Ds *ds_get(uint32_t va, uint32_t w, uint32_t h)
{
    uint32_t i;
    Ds *d = NULL;
    D3D11_TEXTURE2D_DESC td;

    if (!va)
        return NULL;
    for (i = 0; i < MAX_DS; i++)
        if (s_ds[i].tex && s_ds[i].va == va) {
            if (s_ds[i].w == w && s_ds[i].h == h)
                return &s_ds[i];
            d = &s_ds[i];
            break;
        }
    if (!d)
        for (i = 0; i < MAX_DS && !d; i++)
            if (!s_ds[i].tex)
                d = &s_ds[i];
    if (!d)
        d = &s_ds[0];                          /* ponytail: as rt_get */
    if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    memset(d, 0, sizeof *d);

    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->tex))
        || FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex,
                                                      NULL, &d->dsv))) {
        if (d->tex) ID3D11Texture2D_Release(d->tex);
        memset(d, 0, sizeof *d);
        return NULL;
    }
    d->va = va;
    d->w = w;
    d->h = h;
    /* Fresh depth reads as the far plane until the title clears it. */
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    return d;
}

/* ---- textures ---------------------------------------------------------- */

/* `checked`: the frame level 0 was last hashed in. Within a frame it is
 * trusted as is, and only a render-target readback over it (rt_readback)
 * makes it hash again. ponytail: a title that rewrites a texture with the CPU
 * between two draws of one frame keeps the first version until the next
 * frame; hash every lookup again if one does. */
typedef struct {
    uint32_t offset, w, h, pitch, color, bytes;
    uint64_t hash, hash_l0;
    uint32_t last_use, checked;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} Tex;

#define MAX_TEX 256
static Tex s_tex[MAX_TEX];
static uint32_t s_frame = 1;    /* 0 is never a frame: `checked` starts there */
static uint32_t s_tick;         /* lookups so far: the LRU clock */

/* Bytes one texel of an uncompressed format takes (as the executor's
 * tex_texel_bytes). */
static uint32_t texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return 2;
    default:
        return 4;
    }
}

/* Level 0's footprint in guest memory: what is hashed to see whether the
 * title changed it (an XMV frame rewrites its texture every frame). */
static uint32_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    if (block)
        return ((t->width + 3) / 4) * ((t->height + 3) / 4) * block;
    if (!d3d8_format_is_swizzled(t->color) && t->pitch)
        return t->pitch * t->height;
    return t->width * t->height * texel_bytes(t->color);
}

static uint64_t hash_bytes(const uint8_t *p, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* Guest texels to B8G8R8A8: linear A8R8G8B8 / X8R8G8B8 (the XMV frame) are
 * copied row by row; everything else goes through the executor's decoder,
 * which knows the swizzle and every format it can sample. */
static int tex_decode(const Nv2aTexture *t, uint32_t *out)
{
    if ((t->color == 0x12 || t->color == 0x1E) && t->pitch) {
        const uint8_t *src = guest(t->offset);
        uint32_t x, y;
        for (y = 0; y < t->height; y++) {
            const uint32_t *row = (const uint32_t *)(src + (size_t)y * t->pitch);
            uint32_t *d = out + (size_t)y * t->width;
            if (t->color == 0x12)
                memcpy(d, row, (size_t)t->width * 4);
            else
                for (x = 0; x < t->width; x++)
                    d[x] = row[x] | 0xFF000000u;
        }
        return 1;
    }
    return nv2a_backend_decode_texture(t, out);
}

/* Guest memory in [va, va+len) changed under the textures: hash them again. */
static void tex_touched(uint32_t va, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < MAX_TEX; i++)
        if (s_tex[i].tex && va < s_tex[i].offset + s_tex[i].bytes
            && s_tex[i].offset < va + len)
            s_tex[i].checked = 0;
}

/* The view of a texture, uploaded when new or changed. The LRU clock ticks
 * per lookup, so the slot a lookup evicts is never one an earlier stage of
 * the same draw was just handed. */
static ID3D11ShaderResourceView *tex_get(const Nv2aTexture *t)
{
    uint32_t bytes, i;
    uint64_t h, h0;
    Tex *e = NULL, *lru = &s_tex[0];
    uint32_t *argb;
    int fresh = 0;

    if (!t->offset || !t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    bytes = tex_bytes(t);
    rt_flush_range(t->offset, bytes);
    s_tick++;

    for (i = 0; i < MAX_TEX; i++) {
        Tex *c = &s_tex[i];
        if (c->tex && c->offset == t->offset && c->w == t->width && c->h == t->height
            && c->color == t->color && c->pitch == t->pitch) {
            e = c;
            break;
        }
        if (lru->tex && (!c->tex || c->last_use < lru->last_use))
            lru = c;
    }
    if (e && e->checked == s_frame) {
        h0 = e->hash_l0;
    } else {
        s_st.hashes++;
        h0 = hash_bytes(guest(t->offset), bytes);
    }
    h = h0;
    if (t->palette)              /* P8: the same indices under a new palette */
        h = h * 31 + hash_bytes(guest(t->palette), 256 * 4);
    if (e && e->hash == h) {
        e->hash_l0 = h0;
        e->checked = s_frame;
        e->last_use = s_tick;
        return e->srv;
    }
    if (!e) {
        D3D11_TEXTURE2D_DESC td;
        e = lru;
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        memset(e, 0, sizeof *e);
        memset(&td, 0, sizeof td);
        td.Width = t->width;
        td.Height = t->height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex)))
            return NULL;
        if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex,
                                                         NULL, &e->srv))) {
            ID3D11Texture2D_Release(e->tex);
            e->tex = NULL;
            return NULL;
        }
        e->offset = t->offset;
        e->w = t->width;
        e->h = t->height;
        e->color = t->color;
        e->pitch = t->pitch;
        e->bytes = bytes;
        fresh = 1;
    }
    argb = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    if (!argb)
        return NULL;
    s_st.decodes++;
    if (!tex_decode(t, argb)) {
        /* A format the decoder does not know (palettised): drop the entry so
         * the batch draws untextured, as the software path would. */
        free(argb);
        {
            static uint32_t seen[8];
            int i2;
            for (i2 = 0; i2 < 8 && seen[i2] && seen[i2] != t->color + 1; i2++)
                ;
            if (i2 < 8 && !seen[i2]) {
                seen[i2] = t->color + 1;
                fprintf(stderr, "[D3D11] texture format 0x%02X not decoded "
                        "(%ux%u at 0x%08X): drawn untextured\n",
                        t->color, t->width, t->height, t->offset);
            }
        }
        if (fresh) {
            ID3D11ShaderResourceView_Release(e->srv);
            ID3D11Texture2D_Release(e->tex);
            memset(e, 0, sizeof *e);
        }
        return NULL;
    }
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0, NULL,
                                          argb, t->width * 4, 0);
    free(argb);
    e->hash = h;
    e->hash_l0 = h0;
    e->checked = s_frame;
    e->last_use = s_tick;
    return e->srv;
}

/* ---- pipeline state, cached by description ----------------------------- */

#define STATE_CACHE(name, Desc, Iface, create, release)                      \
    static struct { Desc d; Iface *s; } name[64];                             \
    static Iface *name##_get(const Desc *d)                                   \
    {                                                                         \
        int i;                                                                \
        for (i = 0; i < 64 && name[i].s; i++)                                 \
            if (!memcmp(&name[i].d, d, sizeof *d))                            \
                return name[i].s;                                             \
        if (i == 64) { release(name[63].s); i = 63; }                         \
        name[i].d = *d;                                                       \
        name[i].s = NULL;                                                     \
        create(s_dev, d, &name[i].s);                                         \
        return name[i].s;                                                     \
    }

#define REL_BLEND(x) ID3D11BlendState_Release(x)
#define REL_DS(x) ID3D11DepthStencilState_Release(x)
#define REL_RS(x) ID3D11RasterizerState_Release(x)
#define REL_SS(x) ID3D11SamplerState_Release(x)
STATE_CACHE(s_blend, D3D11_BLEND_DESC, ID3D11BlendState,
            ID3D11Device_CreateBlendState, REL_BLEND)
STATE_CACHE(s_dss, D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState,
            ID3D11Device_CreateDepthStencilState, REL_DS)
STATE_CACHE(s_rs, D3D11_RASTERIZER_DESC, ID3D11RasterizerState,
            ID3D11Device_CreateRasterizerState, REL_RS)
STATE_CACHE(s_ss, D3D11_SAMPLER_DESC, ID3D11SamplerState,
            ID3D11Device_CreateSamplerState, REL_SS)

/* NV2A takes OpenGL enum values (nv2a_backend.h). */
static D3D11_BLEND map_blend(uint32_t f)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return D3D11_BLEND_SRC_COLOR;
    case 0x0301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return D3D11_BLEND_DEST_COLOR;
    case 0x0307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

/* The same factor where only alpha is allowed (no *_COLOR in an alpha slot). */
static D3D11_BLEND map_blend_alpha(uint32_t f)
{
    D3D11_BLEND b = map_blend(f);
    if (b == D3D11_BLEND_SRC_COLOR) return D3D11_BLEND_SRC_ALPHA;
    if (b == D3D11_BLEND_INV_SRC_COLOR) return D3D11_BLEND_INV_SRC_ALPHA;
    if (b == D3D11_BLEND_DEST_COLOR) return D3D11_BLEND_DEST_ALPHA;
    if (b == D3D11_BLEND_INV_DEST_COLOR) return D3D11_BLEND_INV_DEST_ALPHA;
    return b;
}

static D3D11_BLEND_OP map_blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC map_cmp(uint32_t f)
{
    /* GL_NEVER..GL_ALWAYS (0x200..0x207) line up with D3D11's 1..8. */
    return (f >= 0x200 && f <= 0x207) ? (D3D11_COMPARISON_FUNC)(f - 0x200 + 1)
                                      : D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP map_stencil_op(uint32_t o)
{
    switch (o) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static D3D11_TEXTURE_ADDRESS_MODE map_addr(uint32_t a)
{
    switch (a) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

/* ---- shaders ----------------------------------------------------------- */

typedef struct {
    float rt_w, rt_h, zscale, alpha_ref;
    float alpha_func, pad[3];
    float fogc[4];              /* fog colour, RGBA */
    uint32_t tb[4];             /* stage has a texture bound */
    /* Combiner constants, which a title changes per draw (fades): per stage
     * factor0, factor1; the final combiner's C0, C1. The program itself is
     * compiled into the pixel shader; see PsKey. */
    uint32_t fac[8][4], fcon[4];
} Consts;

/* The combiner program a pixel shader is compiled for: per stage colour ICW,
 * alpha ICW, colour OCW, alpha OCW; final CW0, CW1; control, stage program,
 * use_rc, clip-plane mode (0 unless a stage is CLIPPLANE). As compile-time constants the compiler folds the register-file
 * indexing away. Evaluated from a constant buffer, every R[b & 15] was an
 * indexed temp array, and the first level's frame took ~17 ms of GPU time:
 * 24 fps against 30 with the combiners off. */
typedef struct {
    uint32_t st[8][4], fin[2], ctl[4];
} PsKey;

/* Vertices arrive in surface pixels with the title's own integer depth and
 * 1/w. Depth is floored (the NV2A writes integers) and scaled by a power of
 * two so the comparison is exact; see ds_get. It is clamped at the far end
 * only: past the depth buffer's range a vertex draws at the far plane, as the
 * software path's float depth lets it, while z < 0 is cut by depth clipping. */
static const char s_hlsl[] =
"cbuffer C : register(b0) { float2 rt; float zscale; float aref;\n"
"                           float afunc; float3 pad; float4 fogc; uint4 tb;\n"
"                           uint4 fac[8]; uint4 fcon; };\n"
"struct VO { float4 p : SV_Position; float4 c : COLOR0; float4 c1 : COLOR1; float fog : FOG;\n"
"            float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2;\n"
"            float4 t3 : TEXCOORD3; };\n"
"VO vs(float4 p : POSITION, float4 c : COLOR0, float4 c1 : COLOR1, float fog : FOG,\n"
"      float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1, float4 t2 : TEXCOORD2,\n"
"      float4 t3 : TEXCOORD3) {\n"
"  VO o; float w = p.w > 0 ? 1.0 / p.w : 1.0;\n"
"  float2 n = float2(p.x / rt.x * 2 - 1, 1 - p.y / rt.y * 2);\n"
"  float z = min(floor(p.z) / zscale, 1);\n"
"  o.p = float4(n * w, z * w, w); o.c = c; o.c1 = c1; o.fog = fog;\n"
"  o.t0 = t0; o.t1 = t1; o.t2 = t2; o.t3 = t3; return o; }\n"
"Texture2D x0 : register(t0); Texture2D x1 : register(t1);\n"
"Texture2D x2 : register(t2); Texture2D x3 : register(t3);\n"
"SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
"SamplerState s2 : register(s2); SamplerState s3 : register(s3);\n"
"#ifdef RC_ST\n"
"static const uint4 st[8] = { RC_ST }; static const uint2 fin = RC_FIN;\n"
"static const uint4 ctl = RC_CTL;\n"
/* One texture stage by its SHADER_STAGE_PROGRAM mode, as the executor's
 * rc_stage_fetch: NONE, PROJECT2D/3D, CUBEMAP (here its first face),
 * PASSTHRU, CLIPPLANE (cp: the stage's 4 clip-plane mode bits). The bump-env
 * and dependent modes sample at their own coordinates, unbumped; the
 * dot-product modes read zero. An enabled stage without a texture reads
 * white. */
"float4 fetch(Texture2D x, SamplerState s, float4 c, uint m, uint b, uint cp) {\n"
"  if (m == 0) return float4(0, 0, 0, 1); if (m == 4) return saturate(c);\n"
"  if (m == 5) { [unroll] for (uint j = 0; j < 4; j++)\n"
"      if (((cp >> j) & 1) ? c[j] >= 0 : c[j] < 0) discard;\n"
"    return 0; }\n"
"  if (m > 3 && m != 6 && m != 7 && m != 15 && m != 16) return 0; if (!b) return 1;\n"
"  return x.SampleLevel(s, m == 3 ? c.xy : c.xy / (c.w != 0 ? c.w : 1), 0); }\n"
/* Register combiners: a line-for-line port of nv2a_rc_eval (the toolkit's
 * nv2a_combiner.c), which documents the encoding. Registers: 0 zero, 1 c0,
 * 2 c1, 3 fog, 4 v0, 5 v1, 8 t0, 12 r0, 13 r1, 14 v1+r0, 15 E*F. */
"float4 unpk(uint c) { return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0; }\n"
"float mapin(float x, uint b) { float p = max(x, 0); uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p; if (m == 1) return 1 - saturate(x); if (m == 2) return 2 * p - 1;\n"
"  if (m == 3) return -2 * p + 1; if (m == 4) return p - 0.5; if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x; return -x; }\n"
"float3 mapout(float3 x, uint m) { if (m == 0x08) x -= 0.5; else if (m == 0x10) x *= 2;\n"
"  else if (m == 0x18) x = (x - 0.5) * 2; else if (m == 0x20) x *= 4;\n"
"  else if (m == 0x30) x *= 0.5; return clamp(x, -1, 1); }\n"
"float3 rgb(float4 R[16], uint b) { float4 r = R[b & 15]; float3 v = (b & 16) ? r.aaa : r.rgb;\n"
"  return float3(mapin(v.r, b), mapin(v.g, b), mapin(v.b, b)); }\n"
"float alp(float4 R[16], uint b) { float4 r = R[b & 15]; return mapin((b & 16) ? r.a : r.b, b); }\n"
"float4 rc(float4 v0, float4 v1, float4 fog, float4 T[4]) {\n"
"  float4 R[16]; [unroll] for (int k = 0; k < 16; k++) R[k] = 0;\n"
"  R[3] = fog; R[4] = v0; R[5] = v1; R[8] = T[0]; R[9] = T[1]; R[10] = T[2]; R[11] = T[3];\n"
"  R[12].a = (ctl.y & 31) ? T[0].a : 1;\n"
"  uint n = min(ctl.x & 255, 8), fl = ctl.x >> 8;\n"
"  [unroll] for (uint s = 0; s < 8; s++) if (s < n) {\n"
"    uint icw = st[s].x, aicw = st[s].y, ocw = st[s].z, aocw = st[s].w;\n"
"    uint f = ocw >> 12, af = aocw >> 12, d;\n"
"    R[1] = unpk(fac[(fl & 0x10) ? s : 0].x); R[2] = unpk(fac[(fl & 0x100) ? s : 0].y);\n"
"    bool mux = (fl & 1) ? R[12].a >= 0.5 : (((uint)(R[12].a * 255)) & 1) != 0;\n"
"    float3 A = rgb(R, icw >> 24), B = rgb(R, icw >> 16), C = rgb(R, icw >> 8), D = rgb(R, icw);\n"
"    float aA = alp(R, aicw >> 24), aB = alp(R, aicw >> 16), aC = alp(R, aicw >> 8), aD = alp(R, aicw);\n"
"    float3 pab = (f & 2) ? dot(A, B).xxx : A * B, pcd = (f & 1) ? dot(C, D).xxx : C * D;\n"
"    float3 sum = (f & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    float3 ab = mapout(pab, f & 0x38), cd = mapout(pcd, f & 0x38), ms = mapout(sum, f & 0x38);\n"
"    float apab = aA * aB, apcd = aC * aD, asum = (af & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"    float aab = mapout(apab.xxx, af & 0x38).x, acd = mapout(apcd.xxx, af & 0x38).x,\n"
"          ams = mapout(asum.xxx, af & 0x38).x;\n"
"    d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (f & 0x80) R[d].a = ab.b; }\n"
"    d = ocw & 15;        if (d) { R[d].rgb = cd; if (f & 0x40) R[d].a = cd.b; }\n"
"    d = (ocw >> 8) & 15; if (d) R[d].rgb = ms;\n"
"    d = (aocw >> 4) & 15; if (d) R[d].a = aab;\n"
"    d = aocw & 15;        if (d) R[d].a = acd;\n"
"    d = (aocw >> 8) & 15; if (d) R[d].a = ams;\n"
"    R[0] = 0; }\n"
"  float4 o = R[12];\n"
"  if (fin.x | fin.y) {\n"
"    uint f0 = fin.x, f1 = fin.y, ff = f1 & 255;\n"
"    R[1] = unpk(fcon.x); R[2] = unpk(fcon.y);\n"
"    float3 a = (ff & 0x40) ? 1 - R[5].rgb : R[5].rgb, b = (ff & 0x20) ? 1 - R[12].rgb : R[12].rgb;\n"
"    R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0);\n"
"    R[15] = float4(rgb(R, f1 >> 24) * rgb(R, f1 >> 16), 0);\n"
"    float3 A = rgb(R, f0 >> 24), B = rgb(R, f0 >> 16), C = rgb(R, f0 >> 8), D = rgb(R, f0);\n"
"    o = float4(D + C * (1 - A) + B * A, alp(R, f1 >> 8)); }\n"
"  return saturate(o); }\n"
"float4 ps(VO i) : SV_Target {\n"
"  float4 c;\n"
"  if (ctl.z) { uint pg = ctl.y; float4 T[4];\n"
"    T[0] = fetch(x0, s0, i.t0, pg & 31, tb.x, ctl.w & 15);\n"
"    T[1] = fetch(x1, s1, i.t1, (pg >> 5) & 31, tb.y, (ctl.w >> 4) & 15);\n"
"    T[2] = fetch(x2, s2, i.t2, (pg >> 10) & 31, tb.z, (ctl.w >> 8) & 15);\n"
"    T[3] = fetch(x3, s3, i.t3, (pg >> 15) & 31, tb.w, (ctl.w >> 12) & 15);\n"
"    c = rc(i.c, i.c1, float4(fogc.rgb, i.fog), T);\n"
"  } else c = tb.x ? i.c * x0.SampleLevel(s0, i.t0.xy, 0) : i.c;\n"
"  float a = round(c.a * 255); int f = (int)afunc; bool ok = true;\n"
"  if (f == 0) ok = false; else if (f == 1) ok = a < aref;\n"
"  else if (f == 2) ok = a == aref; else if (f == 3) ok = a <= aref;\n"
"  else if (f == 4) ok = a > aref; else if (f == 5) ok = a != aref;\n"
"  else if (f == 6) ok = a >= aref;\n"
"  if (!ok) discard; return c; }\n"
"#endif\n";

static int compile_src(const char *src, size_t len, const char *entry, const char *target,
                       const D3D_SHADER_MACRO *defs, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = D3DCompile(src, len, "nv2a_backend", defs, NULL,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "compile failed");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    return 1;
}

static int compile(const char *entry, const char *target,
                   const D3D_SHADER_MACRO *defs, ID3DBlob **out)
{
    return compile_src(s_hlsl, sizeof s_hlsl - 1, entry, target, defs, out);
}

/* The blit shaders and their fixed state. */
static int blit_init(void)
{
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC cbd;
    D3D11_SAMPLER_DESC sd;
    D3D11_BLEND_DESC bd;

    if (!compile_src(s_blit_hlsl, sizeof s_blit_hlsl - 1, "vs", "vs_4_0", NULL, &b))
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                    ID3D10Blob_GetBufferSize(b), NULL, &s_blit_vs);
    ID3D10Blob_Release(b);
    if (!compile_src(s_blit_hlsl, sizeof s_blit_hlsl - 1, "ps", "ps_4_0", NULL, &b))
        return 0;
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), NULL, &s_blit_ps);
    ID3D10Blob_Release(b);
    memset(&cbd, 0, sizeof cbd);
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(s_dev, &cbd, NULL, &s_blit_cb);
    memset(&sd, 0, sizeof sd);
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(s_dev, &sd, &s_point);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(s_dev, &sd, &s_linear);
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(s_dev, &bd, &s_over);
    return s_blit_vs && s_blit_ps && s_blit_cb && s_point && s_linear && s_over;
}

/* The pixel shader for one combiner program, compiled on first use, found by
 * a hash of its key. When all MAX_PS slots are in use the least recently used
 * shader goes. */
#define MAX_PS 1024
static struct { PsKey k; uint64_t h; uint32_t last; ID3D11PixelShader *ps; } s_psv[MAX_PS];
static int s_npsv;
static uint32_t s_ps_tick;

/* Compiled pixel shaders persist across runs, so a combiner program the game
 * has used before costs nothing the next time it appears. Compiling one takes
 * tens of milliseconds, which is a visible hitch the first time a scene uses
 * it. ps_cache_load creates every cached shader at startup; ps_get appends
 * what it compiles, once per key (s_saved), so the file holds each program
 * once however often the LRU evicts it. The file holds a hash of the HLSL
 * source and is ignored when that changes. RECOMP_SHADER_CACHE=<path> moves
 * it; "0" turns it off.
 *
 *   "NV2APS1" NUL, uint64 source hash, then records of
 *   PsKey, uint32 size, size bytes of bytecode. */
static uint64_t *s_saved;
static uint32_t s_nsaved, s_cap_saved;

/* 1 when h is new to the file (and is now recorded), 0 when it is there. */
static int saved_add(uint64_t h)
{
    uint32_t i;
    for (i = 0; i < s_nsaved; i++)
        if (s_saved[i] == h)
            return 0;
    if (s_nsaved == s_cap_saved) {
        uint32_t cap = s_cap_saved ? s_cap_saved * 2 : 256;
        uint64_t *p = (uint64_t *)realloc(s_saved, cap * sizeof *p);
        if (!p)
            return 0;
        s_saved = p;
        s_cap_saved = cap;
    }
    s_saved[s_nsaved++] = h;
    return 1;
}

static const char *ps_cache_path(void)
{
    const char *e = getenv("RECOMP_SHADER_CACHE");
    if (e && !strcmp(e, "0"))
        return NULL;
    return e ? e : "nv2a_shaders.bin";
}

static uint64_t ps_source_hash(void)
{
    return hash_bytes((const uint8_t *)s_hlsl, (uint32_t)sizeof s_hlsl) ^ sizeof(PsKey);
}

static uint64_t ps_key_hash(const PsKey *k)
{
    return hash_bytes((const uint8_t *)k, (uint32_t)sizeof *k);
}

static ID3D11PixelShader *ps_find(const PsKey *k, uint64_t h)
{
    int i;
    for (i = 0; i < s_npsv; i++)
        if (s_psv[i].h == h && !memcmp(&s_psv[i].k, k, sizeof *k)) {
            s_psv[i].last = ++s_ps_tick;
            return s_psv[i].ps;
        }
    return NULL;
}

static ID3D11PixelShader *ps_create(const PsKey *k, uint64_t h, const void *code, uint32_t size)
{
    ID3D11PixelShader *ps = NULL;
    int i, v = 0;

    if (FAILED(ID3D11Device_CreatePixelShader(s_dev, code, size, NULL, &ps)) || !ps)
        return NULL;
    if (s_npsv < MAX_PS) {
        v = s_npsv++;
    } else {
        for (i = 1; i < MAX_PS; i++)
            if (s_psv[i].last < s_psv[v].last)
                v = i;
        ID3D11PixelShader_Release(s_psv[v].ps);
    }
    s_psv[v].k = *k;
    s_psv[v].h = h;
    s_psv[v].last = ++s_ps_tick;
    s_psv[v].ps = ps;
    return ps;
}

static void ps_cache_load(void)
{
    const char *path = ps_cache_path();
    FILE *f = path ? fopen(path, "rb") : NULL;
    char magic[8];
    uint64_t h;
    int n = 0;

    if (!f)
        return;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "NV2APS1", 8)
        || fread(&h, 8, 1, f) != 1 || h != ps_source_hash()) {
        fclose(f);
        remove(path);                      /* stale: start a new one */
        fprintf(stderr, "[D3D11] shader cache out of date; rebuilding it\n");
        return;
    }
    while (s_npsv < MAX_PS) {
        PsKey k;
        uint32_t size;
        uint64_t kh;
        void *code;
        if (fread(&k, sizeof k, 1, f) != 1 || fread(&size, 4, 1, f) != 1
            || size > (1u << 20) || !(code = malloc(size)))
            break;
        kh = ps_key_hash(&k);
        if (fread(code, 1, size, f) == size && saved_add(kh) && ps_create(&k, kh, code, size))
            n++;
        free(code);
    }
    fclose(f);
    fprintf(stderr, "[D3D11] %d pixel shaders loaded from %s\n", n, path);
}

static void ps_cache_append(const PsKey *k, uint64_t h, const void *code, uint32_t size)
{
    const char *path = ps_cache_path();
    FILE *f;
    int fresh;

    if (!path || !saved_add(h))
        return;
    f = fopen(path, "rb");
    fresh = !f;
    if (f)
        fclose(f);
    if (!(f = fopen(path, "ab")))
        return;
    if (fresh) {
        uint64_t h = ps_source_hash();
        fwrite("NV2APS1", 1, 8, f);
        fwrite(&h, 8, 1, f);
    }
    fwrite(k, sizeof *k, 1, f);
    fwrite(&size, 4, 1, f);
    fwrite(code, 1, size, f);
    fclose(f);
}

static ID3D11PixelShader *ps_get(const PsKey *k)
{
    char st[8 * 64], fin[48], ctl[64];
    D3D_SHADER_MACRO defs[] = { { "RC_ST", st }, { "RC_FIN", fin },
                                { "RC_CTL", ctl }, { NULL, NULL } };
    ID3DBlob *b = NULL;
    ID3D11PixelShader *ps;
    uint64_t h = ps_key_hash(k);
    int i, n = 0;

    if ((ps = ps_find(k, h)) != NULL)
        return ps;
    for (i = 0; i < 8; i++)
        n += snprintf(st + n, sizeof st - n, "%suint4(0x%X,0x%X,0x%X,0x%X)", i ? "," : "",
                      k->st[i][0], k->st[i][1], k->st[i][2], k->st[i][3]);
    snprintf(fin, sizeof fin, "uint2(0x%X,0x%X)", k->fin[0], k->fin[1]);
    snprintf(ctl, sizeof ctl, "uint4(0x%X,0x%X,0x%X,0x%X)",
             k->ctl[0], k->ctl[1], k->ctl[2], k->ctl[3]);
    if (!compile("ps", "ps_4_0", defs, &b))
        return NULL;
    ps = ps_create(k, h, ID3D10Blob_GetBufferPointer(b), (uint32_t)ID3D10Blob_GetBufferSize(b));
    if (ps)
        ps_cache_append(k, h, ID3D10Blob_GetBufferPointer(b),
                        (uint32_t)ID3D10Blob_GetBufferSize(b));
    ID3D10Blob_Release(b);
    return ps;
}

/* ---- the back end ------------------------------------------------------ */


/* Depth arrives as the integer the zeta surface holds: 16 or 24 bits. */
static float depth_scale(const Nv2aRenderState *rs)
{
    return rs->zeta_format == 1 ? 65536.0f : 16777216.0f;
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Rt *r = rt_get(s);
    if (!r)
        return;
    if (flags & 0xF0) {
        /* ponytail: a partial colour-channel clear clears all four. */
        float c[4] = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                       (argb & 0xFF) / 255.0f, (argb >> 24) / 255.0f };
        D3D11_RECT rc = { (LONG)(s->clip_x * s_scale), (LONG)(s->clip_y * s_scale),
                          (LONG)((s->clip_x + s->width) * s_scale),
                          (LONG)((s->clip_y + s->height) * s_scale) };
        if (s_ctx1)
            ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)r->rtv, c, &rc, 1);
        else
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, c);
        r->dirty = 1;
        s_last_rt = r;
    }
    if (flags & 0x3) {
        Ds *d = ds_get(rs->zeta_va, r->w * s_scale, r->h * s_scale);
        if (d) {
            /* Z24S8: depth in the top 24 bits; Z16: the low 16. */
            float z = rs->zeta_format == 1 ? (float)(zstencil & 0xFFFF) / 65536.0f
                                           : (float)(zstencil >> 8) / 16777216.0f;
            UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0)
                   | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv, f,
                                                      z, (UINT8)(zstencil & 0xFF));
        }
    }
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    Rt *r;
    Ds *d = NULL;
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    const Nv2aTexture *tx[4];
    int st;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BLEND_DESC bd;
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    Consts k;
    UINT stride = sizeof(Nv2aVertex), offset = 0, bytes, first;
    float bf[4];
    LONGLONG t0, t1;
    PsKey pk;
    ID3D11PixelShader *ps;

    if (!b->count)
        return;
    r = rt_get(s);
    if (!r)
        return;
    t0 = qpc();
    s_st.draws++;
    /* With combiners every enabled stage samples; without, stage 0 alone. */
    for (st = 0; st < 4; st++) {
        tx[st] = b->rc && !s_dbg.no_rc ? b->stages[st] : (st == 0 ? b->texture : NULL);
        if (tx[st])
            srv[st] = tex_get(tx[st]);    /* may read back a target: before binding */
    }
    s_st.t_tex += qpc() - t0;
    if (rs->zeta_va && (rs->depth_test_enable || rs->depth_write || rs->stencil_enable))
        d = ds_get(rs->zeta_va, r->w * s_scale, r->h * s_scale);

    /* Vertices: appended to a ring, NO_OVERWRITE until it wraps. A DISCARD
     * on every draw made the driver rename the buffer ~3,600 times a second
     * in the first level and cost half the executor's time. */
    t1 = qpc();
    bytes = b->count * stride;
    if (bytes > s_vb_bytes) {
        D3D11_BUFFER_DESC vbd;
        if (s_vb) ID3D11Buffer_Release(s_vb);
        s_vb = NULL;
        memset(&vbd, 0, sizeof vbd);
        vbd.ByteWidth = bytes * 2 > 8u << 20 ? bytes * 2 : 8u << 20;
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &vbd, NULL, &s_vb)))
            return;
        s_vb_bytes = vbd.ByteWidth;
        s_vb_pos = s_vb_bytes;                 /* first map discards */
    }
    s_vb_pos = (s_vb_pos + stride - 1) / stride * stride;
    if (s_vb_pos + bytes > s_vb_bytes)
        s_vb_pos = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                       s_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                : D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy((uint8_t *)m.pData + s_vb_pos, b->vertices, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    first = s_vb_pos / stride;
    s_vb_pos += bytes;

    s_st.t_vb += qpc() - t1;
    /* Constants */
    t1 = qpc();
    k.rt_w = (float)r->w;
    k.rt_h = (float)r->h;
    k.zscale = depth_scale(rs);
    k.alpha_ref = (float)(rs->alpha_ref & 0xFF);
    k.alpha_func = rs->alpha_test_enable && rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
                 ? (float)(rs->alpha_func - 0x200) : 7.0f;
    k.pad[0] = k.pad[1] = k.pad[2] = 0.0f;
    k.fogc[0] = ((b->fog_color >> 16) & 0xFF) / 255.0f;
    k.fogc[1] = ((b->fog_color >> 8) & 0xFF) / 255.0f;
    k.fogc[2] = (b->fog_color & 0xFF) / 255.0f;
    k.fogc[3] = (b->fog_color >> 24) / 255.0f;
    for (st = 0; st < 4; st++)
        k.tb[st] = srv[st] != NULL;
    memset(k.fac, 0, sizeof k - offsetof(Consts, fac));
    memset(&pk, 0, sizeof pk);
    if (b->rc && !s_dbg.no_rc) {
        const Nv2aCombiner *c = b->rc;
        int i, n = (int)(c->control & 0xFF) < 8 ? (int)(c->control & 0xFF) : 8;
        for (i = 0; i < 8; i++) {
            if (i < n) {             /* stages past the count do not run */
                pk.st[i][0] = c->color_icw[i]; pk.st[i][1] = c->alpha_icw[i];
                pk.st[i][2] = c->color_ocw[i]; pk.st[i][3] = c->alpha_ocw[i];
            }
            k.fac[i][0] = c->factor0[i];  k.fac[i][1] = c->factor1[i];
        }
        pk.fin[0] = c->final0; pk.fin[1] = c->final1;
        k.fcon[0] = c->final_c0; k.fcon[1] = c->final_c1;
        pk.ctl[0] = c->control; pk.ctl[1] = c->stage_program; pk.ctl[2] = 1;
        for (i = 0; i < 4; i++)
            if (((c->stage_program >> (i * 5)) & 0x1F) == 5)
                pk.ctl[3] = b->clip_plane_mode & 0xFFFF;
    }
    ps = ps_get(&pk);
    if (!ps)
        return;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_cb, 0, NULL, &k, 0, 0);
    s_st.t_cb += qpc() - t1;

    /* Blend */
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = rs->blend_enable ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = map_blend(rs->blend_src);
    bd.RenderTarget[0].DestBlend = map_blend(rs->blend_dst);
    bd.RenderTarget[0].BlendOp = map_blend_op(rs->blend_eq);
    bd.RenderTarget[0].SrcBlendAlpha = map_blend_alpha(rs->blend_src);
    bd.RenderTarget[0].DestBlendAlpha = map_blend_alpha(rs->blend_dst);
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask =
        (UINT8)(((rs->color_mask & 0x00010000) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
              | ((rs->color_mask & 0x00000100) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
              | ((rs->color_mask & 0x00000001) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
              | ((rs->color_mask & 0x01000000) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = (rs->blend_color >> 24) / 255.0f;
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004
        || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];     /* CONSTANT_ALPHA: the alpha in every slot */

    /* Depth and stencil */
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = d && (rs->depth_test_enable || rs->depth_write);
    dd.DepthWriteMask = rs->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = rs->depth_test_enable ? map_cmp(rs->depth_func) : D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = d && rs->stencil_enable;
    dd.StencilReadMask = (UINT8)rs->stencil_func_mask;
    dd.StencilWriteMask = (UINT8)rs->stencil_write_mask;
    dd.FrontFace.StencilFunc = map_cmp(rs->stencil_func);
    dd.FrontFace.StencilFailOp = map_stencil_op(rs->stencil_fail);
    dd.FrontFace.StencilDepthFailOp = map_stencil_op(rs->stencil_zfail);
    dd.FrontFace.StencilPassOp = map_stencil_op(rs->stencil_zpass);
    dd.BackFace = dd.FrontFace;

    /* Rasteriser. ponytail: culling off. Winding after the executor's
     * transform is unverified, and culling the wrong face loses geometry,
     * while drawing both only costs fill on closed meshes the depth test
     * hides anyway. Map cull_face/front_face once a 3D scene shows the
     * difference. */
    memset(&rd, 0, sizeof rd);
    rd.FillMode = s_dbg.wire ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    /* Depth clipping on, and the shader clamps z at 1 only: geometry nearer
     * than the near plane (z < 0) is cut away, as on the NV2A. Clamped to 0
     * it drew in front of everything instead -- a wall or a log beside the
     * camera flashed across the whole screen when the camera brushed it. */
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;

    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)(r->w * s_scale); vp.Height = (float)(r->h * s_scale);
    vp.MinDepth = 0; vp.MaxDepth = 1;
    sc.left = (LONG)(s->clip_x * s_scale); sc.top = (LONG)(s->clip_y * s_scale);
    sc.right = (LONG)((s->clip_x + s->width) * s_scale);
    sc.bottom = (LONG)((s->clip_y + s->height) * s_scale);

    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, d ? d->dsv : NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blend_get(&bd), bf, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_dss_get(&dd),
                                               rs->stencil_ref & 0xFF);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs_get(&rd));
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    for (st = 0; st < 4; st++) {
        D3D11_SAMPLER_DESC sd;
        uint32_t mag, min;
        ID3D11SamplerState *ss;
        if (!srv[st])
            continue;
        mag = (tx[st]->filter >> 24) & 0xF;
        min = (tx[st]->filter >> 16) & 0xFF;
        memset(&sd, 0, sizeof sd);
        sd.Filter = (mag == 2 || min == 2 || min == 4 || min == 6)
                  ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = map_addr(tx[st]->addr_u);
        sd.AddressV = map_addr(tx[st]->addr_v);
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ss = s_ss_get(&sd);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, (UINT)st, 1, &ss);
    }
    ID3D11DeviceContext_Draw(s_ctx, b->count, first);
    r->dirty = 1;
    s_last_rt = r;
    s_st.t_draw += qpc() - t0;
}

/* ---- the window: swap chain and overlay -------------------------------- */

static IDXGISwapChain1 *s_sc;
static ID3D11RenderTargetView *s_bb;
static UINT s_bb_w, s_bb_h;
static ID3D11InfoQueue *s_iq;   /* RECOMP_D3D11_DEBUG=1 */

/* The overlay: text drawn by GDI into a DIB, uploaded, and blended over the
 * frame. Plain GDI because it is already there, and the overlay is a debug
 * aid rather than something a frame should be built around. */
#define OV_W 720
#define OV_H 400
#define OV_LINE 15
static HDC s_ov_dc;
static int s_ov_wmax;       /* widest line this frame, pixels */
static uint32_t *s_ov_px;
static ID3D11Texture2D *s_ov_tex;
static ID3D11ShaderResourceView *s_ov_srv;

static struct {
    double ms;                  /* flip to flip, smoothed */
    LONGLONG t_last;
    uint32_t iq_total;          /* debug layer messages so far */
    char iq_last[2][128];       /* the latest two */
} s_ov;

/* What the frame just shown cost: the counters' change since the last flip,
 * when they were s_fr. */
static Stats s_fr, s_fd;

static int ov_init(void)
{
    BITMAPINFO bi;
    HBITMAP bmp;
    HFONT font;
    D3D11_TEXTURE2D_DESC td;

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = OV_W;
    bi.bmiHeader.biHeight = -OV_H;             /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    s_ov_dc = CreateCompatibleDC(NULL);
    bmp = s_ov_dc ? CreateDIBSection(s_ov_dc, &bi, DIB_RGB_COLORS, (void **)&s_ov_px, NULL, 0)
                  : NULL;
    if (!bmp)
        return 0;
    SelectObject(s_ov_dc, bmp);
    font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0,
                       NONANTIALIASED_QUALITY, FIXED_PITCH, "Consolas");
    if (font)
        SelectObject(s_ov_dc, font);
    SetBkMode(s_ov_dc, TRANSPARENT);
    memset(&td, 0, sizeof td);
    td.Width = OV_W;
    td.Height = OV_H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ov_tex))
        && SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_ov_tex,
                                                           NULL, &s_ov_srv));
}

static int ov_line(int y, COLORREF c, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    int n;

    if (y > OV_H - OV_LINE)
        return y;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    SetTextColor(s_ov_dc, c);
    TextOutA(s_ov_dc, 6, y, buf, n);
    {
        SIZE sz;
        if (GetTextExtentPoint32A(s_ov_dc, buf, n, &sz) && sz.cx + 12 > s_ov_wmax)
            s_ov_wmax = sz.cx + 12 < OV_W ? sz.cx + 12 : OV_W;
    }
    return y + OV_LINE;
}

/* The overlay's text for this frame, over the top left of the window. */
static void ov_draw(const Rt *shown)
{
    LARGE_INTEGER f;
    double ms;
    uint32_t i, ntex = 0, nds = 0, nrt = 0, k = 0;
    int y = 4;
    const COLORREF white = RGB(255, 255, 255), grey = RGB(170, 170, 170),
                   yellow = RGB(255, 220, 90);

    if (!s_ov_srv && !ov_init())
        return;
    QueryPerformanceFrequency(&f);
    ms = 1000.0 / (double)f.QuadPart;
    for (i = 0; i < MAX_TEX; i++) ntex += s_tex[i].tex != NULL;
    for (i = 0; i < MAX_DS; i++) nds += s_ds[i].tex != NULL;
    for (i = 0; i < MAX_RT; i++) nrt += s_rt[i].tex != NULL;

    memset(s_ov_px, 0, (size_t)OV_W * OV_H * 4);
    s_ov_wmax = 0;
    y = ov_line(y, yellow, "D3D11 debugger  F1 hide  F2 view  F3 wireframe  F4 combiners  F9 mouse");
    y = ov_line(y, white, "%.1f fps  %.2f ms/frame   scale %ux   window %ux%u",
                s_ov.ms > 0 ? 1000.0 / s_ov.ms : 0.0, s_ov.ms, s_scale, s_bb_w, s_bb_h);
    y = ov_line(y, white, "view: %s   wireframe %s   combiners %s",
                s_dbg.view ? "render target" : "final frame", s_dbg.wire ? "ON" : "off",
                s_dbg.no_rc ? "BYPASSED" : "on");
    y = ov_line(y, white, "draws %u  %.2f ms   textures %.2f ms: %u hashed, %u decoded",
                s_fd.draws, s_fd.t_draw * ms, s_fd.t_tex * ms, s_fd.hashes, s_fd.decodes);
    y = ov_line(y, white, "surface uploads %u  %.2f ms   readbacks %u  %.2f ms   vb %.2f  cb %.2f ms",
                s_fd.uploads, s_fd.t_upload * ms, s_fd.readbacks, s_fd.t_readback * ms,
                s_fd.t_vb * ms, s_fd.t_cb * ms);
    y = ov_line(y, white, "cached: pixel shaders %d/%d  textures %u/%d  targets %u/%d  depth %u/%d",
                s_npsv, MAX_PS, ntex, MAX_TEX, nrt, MAX_RT, nds, MAX_DS);
    y = ov_line(y, grey, "render targets: #, guest address, guest size, bpp, format");
    for (i = 0; i < MAX_RT; i++) {
        const Rt *r = &s_rt[i];
        if (!r->tex)
            continue;
        k++;
        y = ov_line(y, r == shown ? yellow : grey, " %c%2u  %08X  %4ux%-4u  %2u  0x%X%s",
                    r == shown ? '>' : ' ', k, r->va, r->w, r->h, r->bpp * 8, r->fmt,
                    r == s_last_rt ? "  last drawn" : "");
    }
    if (s_iq) {
        y = ov_line(y, s_ov.iq_total ? yellow : grey, "debug layer: %u messages",
                    s_ov.iq_total);
        for (i = 0; i < 2; i++)
            if (s_ov.iq_last[i][0])
                y = ov_line(y, grey, "  %.100s", s_ov.iq_last[i]);
    }
    GdiFlush();
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_ov_tex, 0, NULL,
                                          s_ov_px, OV_W * 4, 0);
    y += 4;
    blit(s_ov_srv, s_bb, 8, 8, (float)s_ov_wmax, (float)y, 0, 0.72f,
         (float)s_ov_wmax / OV_W, (float)y / OV_H);
}

/* The debug layer's messages since the last flip: printed (the first 200),
 * counted, and the latest two kept for the overlay. */
static void iq_drain(void)
{
    static uint32_t printed;
    UINT64 n, i;

    if (!s_iq)
        return;
    n = ID3D11InfoQueue_GetNumStoredMessages(s_iq);
    for (i = 0; i < n; i++) {
        SIZE_T len = 0;
        D3D11_MESSAGE *m;
        if (FAILED(ID3D11InfoQueue_GetMessage(s_iq, i, NULL, &len))
            || !(m = (D3D11_MESSAGE *)malloc(len)))
            continue;
        if (SUCCEEDED(ID3D11InfoQueue_GetMessage(s_iq, i, m, &len))) {
            if (printed++ < 200)
                fprintf(stderr, "[D3D11 debug] %.*s\n", (int)m->DescriptionByteLength,
                        m->pDescription);
            memcpy(s_ov.iq_last[0], s_ov.iq_last[1], sizeof s_ov.iq_last[0]);
            snprintf(s_ov.iq_last[1], sizeof s_ov.iq_last[1], "%.*s",
                     (int)m->DescriptionByteLength, m->pDescription);
            s_ov.iq_total++;
        }
        free(m);
    }
    ID3D11InfoQueue_ClearStoredMessages(s_iq);
}

/* The swap chain on the framebuffer window, made the first time the window
 * is there. The window is sized to the scaled frame (within the screen) and
 * stays resizable: the back buffer follows its client area, and the frame
 * is fitted inside at its own aspect. */
static int sc_ready(HWND hwnd)
{
    RECT cr;
    UINT w, h;

    if (!s_sc) {
        IDXGIDevice *xd = NULL;
        IDXGIAdapter *ad = NULL;
        IDXGIFactory2 *fac = NULL;
        DXGI_SWAP_CHAIN_DESC1 d;
        RECT wa, wr = { 0, 0, (LONG)(640 * s_scale), (LONG)(480 * s_scale) };

        ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&xd);
        if (xd) IDXGIDevice_GetAdapter(xd, &ad);
        if (ad) IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
        memset(&d, 0, sizeof d);
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (fac) {
            IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)s_dev, hwnd, &d, NULL, NULL,
                                                 &s_sc);
            IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory2_Release(fac);
        }
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
        if (!s_sc) {
            static int said;
            if (!said++)
                fprintf(stderr, "[D3D11] no swap chain: the window shows guest memory\n");
            return 0;
        }
        xbox_FramebufferWindowExternalPresent(1);
        /* ponytail: 640x480 a scale step, the size this title renders at,
         * shrunk to fit the screen. Async: the window's thread moves it. */
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        while (wr.right > (wa.right - wa.left) * 9 / 10
               || wr.bottom > (wa.bottom - wa.top) * 9 / 10) {
            wr.right = wr.right * 3 / 4;
            wr.bottom = wr.bottom * 3 / 4;
        }
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        fprintf(stderr, "[D3D11] presenting at %ux through a swap chain\n", s_scale);
    }
    if (!GetClientRect(hwnd, &cr))
        return 0;
    w = (UINT)(cr.right - cr.left);
    h = (UINT)(cr.bottom - cr.top);
    if (!w || !h)
        return 0;                              /* minimised */
    if (w != s_bb_w || h != s_bb_h) {
        if (s_bb) ID3D11RenderTargetView_Release(s_bb);
        s_bb = NULL;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain1_ResizeBuffers(s_sc, 0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
            return 0;
        s_bb_w = w;
        s_bb_h = h;
    }
    if (!s_bb) {
        ID3D11Texture2D *bb = NULL;
        IDXGISwapChain1_GetBuffer(s_sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
        if (bb) {
            ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_bb);
            ID3D11Texture2D_Release(bb);
        }
    }
    return s_bb != NULL;
}

/* The finished frame (or, with F2, the render target picked) onto the swap
 * chain, fitted to the window, and the overlay over it.
 *
 * ponytail: the frame shown is the target drawn into last. A title that
 * writes its front buffer with the CPU and draws nothing would show its last
 * drawn frame instead; present guest memory there if one does. */
static void present(void)
{
    static const float black[4] = { 0, 0, 0, 1 };
    HWND hwnd = (HWND)xbox_FramebufferWindowHwnd();
    const Rt *r = s_last_rt;

    if (!hwnd || !sc_ready(hwnd))
        return;
    if (s_dbg.view) {
        uint32_t i, k = 0;
        for (i = 0; i < MAX_RT; i++)
            if (s_rt[i].tex && ++k == (uint32_t)s_dbg.view)
                r = &s_rt[i];
        if (k < (uint32_t)s_dbg.view)
            s_dbg.view = 0;                    /* past the last: the frame again */
    }
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_bb, black);
    if (r) {
        float fw = (float)s_bb_w, fh = (float)s_bb_h;
        if (fw * r->h > fh * r->w)
            fw = fh * r->w / r->h;
        else
            fh = fw * r->h / r->w;
        blit(r->srv, s_bb, ((float)s_bb_w - fw) / 2, ((float)s_bb_h - fh) / 2, fw, fh,
             1, 1.0f, 1, 1);
    }
    if (s_dbg.overlay)
        ov_draw(r);
    IDXGISwapChain1_Present(s_sc, 0, 0);
}

/* F1..F4 on their press, from the window's key table. */
static void hotkeys(void)
{
    static int prev[4];
    int i;

    for (i = 0; i < 4; i++) {
        int down = xbox_FramebufferKeyDown(VK_F1 + i);
        if (down && !prev[i]) {
            switch (i) {
            case 0: s_dbg.overlay ^= 1; break;
            case 1: s_dbg.view++; break;
            case 2: s_dbg.wire ^= 1; break;
            case 3: s_dbg.no_rc ^= 1; break;
            }
        }
        prev[i] = down;
    }
}

/* The frame is done: show it, then write every target back to guest memory,
 * where the dumps and a title that reads its own frame find it, and treat
 * guest memory as the truth again next frame (the title may write a surface
 * with the CPU in between). */
static void be_flip(void)
{
    LONGLONG now = qpc();
    uint32_t i;

    hotkeys();
    iq_drain();
#define FD(x) s_fd.x = s_st.x - s_fr.x
    FD(readbacks); FD(uploads); FD(decodes); FD(hashes); FD(draws);
    FD(t_readback); FD(t_upload); FD(t_tex); FD(t_draw); FD(t_vb); FD(t_cb);
#undef FD
    s_fr = s_st;            /* the readbacks below count towards the next frame */
    if (s_ov.t_last) {
        LARGE_INTEGER f;
        double ms;
        QueryPerformanceFrequency(&f);
        ms = (double)(now - s_ov.t_last) * 1000.0 / (double)f.QuadPart;
        s_ov.ms = s_ov.ms > 0 ? s_ov.ms * 0.9 + ms * 0.1 : ms;
    }
    s_ov.t_last = now;
    present();

    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex) {
            rt_readback(&s_rt[i]);
            s_rt[i].valid = 0;
        }
    s_frame++;

    /* RECOMP_D3D11_STATS=1: flips a second, every five seconds. */
    {
        static int on = -1;
        static DWORD t0;
        static uint32_t f0;
        DWORD now = GetTickCount();
        if (on < 0)
            on = getenv("RECOMP_D3D11_STATS") != NULL;
        if (on && now - t0 >= 5000) {
            if (t0) {
                LARGE_INTEGER f;
                double ms, secs = (now - t0) / 1000.0;
                QueryPerformanceFrequency(&f);
                ms = 1000.0 / (double)f.QuadPart / secs;   /* ticks to ms a second */
                fprintf(stderr, "[D3D11] %.1f flips/s | a second: %.0f draws %.0f ms "
                        "(textures %.0f ms: %.0f lookups, %.0f decodes) | %.0f readbacks "
                        "%.0f ms | %.0f uploads %.0f ms | vb %.0f ms cb %.0f ms\n",
                        (s_frame - f0) / secs, s_st.draws / secs, s_st.t_draw * ms,
                        s_st.t_tex * ms, s_st.hashes / secs, s_st.decodes / secs,
                        s_st.readbacks / secs, s_st.t_readback * ms,
                        s_st.uploads / secs, s_st.t_upload * ms, s_st.t_vb * ms, s_st.t_cb * ms);
            }
            memset(&s_st, 0, sizeof s_st);
            memset(&s_fr, 0, sizeof s_fr);
            t0 = now;
            f0 = s_frame;
        }
    }
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip };

int d3d11_backend_init(void)
{
    static const D3D11_INPUT_ELEMENT_DESC il[] = {
#define EL(name, i, fmt, field) \
        { name, i, fmt, 0, (UINT)offsetof(Nv2aVertex, field), D3D11_INPUT_PER_VERTEX_DATA, 0 }
        EL("POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, x),
        EL("COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     diffuse),
        EL("COLOR",    1, DXGI_FORMAT_B8G8R8A8_UNORM,     specular),
        EL("FOG",      0, DXGI_FORMAT_R32_FLOAT,          fog),
        EL("TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, tc[0]),
        EL("TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, tc[1]),
        EL("TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, tc[2]),
        EL("TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, tc[3]),
#undef EL
    };
    D3D_FEATURE_LEVEL fl;
    ID3DBlob *vsb = NULL;
    PsKey plain;
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    {
        const char *e = getenv("RECOMP_SCALE");
        int n = e ? atoi(e) : 2;
        s_scale = n >= 1 && n <= 8 ? (uint32_t)n : 2;
    }
    /* RECOMP_D3D11_DEBUG=1: the debug layer, its messages on the overlay
     * and stderr. It needs the Graphics Tools optional feature; without it
     * the device is made as usual. */
    hr = E_FAIL;
    if (getenv("RECOMP_D3D11_DEBUG") && strcmp(getenv("RECOMP_D3D11_DEBUG"), "0")) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_DEBUG,
                               NULL, 0, D3D11_SDK_VERSION, &s_dev, &fl, &s_ctx);
        if (SUCCEEDED(hr)) {
            ID3D11Device_QueryInterface(s_dev, &IID_ID3D11InfoQueue, (void **)&s_iq);
            s_dbg.overlay = 1;
        } else {
            fprintf(stderr, "[D3D11] no debug layer (0x%08lX): install Graphics Tools\n",
                    (unsigned long)hr);
        }
    }
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &s_dev, &fl, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] no hardware device (0x%08lX); software rasteriser\n",
                (unsigned long)hr);
        return 0;
    }
    ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1, (void **)&s_ctx1);
    if (!compile("vs", "vs_4_0", NULL, &vsb))
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreateInputLayout(s_dev, il, (UINT)(sizeof il / sizeof il[0]), ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &s_layout);
    ID3D10Blob_Release(vsb);
    memset(&cbd, 0, sizeof cbd);
    cbd.ByteWidth = sizeof(Consts);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(s_dev, &cbd, NULL, &s_cb);
    ps_cache_load();
    memset(&plain, 0, sizeof plain);       /* texel * diffuse: proves the PS compiles */
    if (!s_vs || !s_layout || !s_cb || !ps_get(&plain) || !blit_init()) {
        fprintf(stderr, "[D3D11] pipeline setup failed; software rasteriser\n");
        return 0;
    }
    nv2a_backend_register(&s_backend);
    fprintf(stderr, "[D3D11] render back end up (feature level %X, %ux resolution)\n",
            (unsigned)fl, s_scale);
    return 1;
}

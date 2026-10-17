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
 * What the back end interface carries is what gets drawn: texture stage 0
 * times the diffuse colour, blend, alpha test, depth and stencil. Register
 * combiners and texture stages 1-3 are not passed on (nv2a_backend.h).
 *
 * RECOMP_D3D11=0 turns it off and the software rasteriser draws as before.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "d3d8_swizzle.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11DeviceContext1 *s_ctx1;          /* ClearView, for clip-rect clears */
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11InputLayout *s_layout;
static ID3D11Buffer *s_cb, *s_vb;
static UINT s_vb_bytes;

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)(xbox_GetMemoryOffset() + (ptrdiff_t)va);
}

/* ---- colour surfaces --------------------------------------------------- */

typedef struct {
    uint32_t va, pitch, bpp, fmt, w, h;
    ID3D11Texture2D *tex, *staging;
    ID3D11RenderTargetView *rtv;
    int valid;              /* the GPU copy matches guest memory */
    int dirty;              /* the GPU copy is newer than guest memory */
} Rt;

#define MAX_RT 16
static Rt s_rt[MAX_RT];

static void rt_release(Rt *r)
{
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    if (r->staging) ID3D11Texture2D_Release(r->staging);
    memset(r, 0, sizeof *r);
}

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

static void rt_upload(Rt *r)
{
    const uint8_t *src = guest(r->va);
    if (r->bpp == 4) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex,
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
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex,
                                              0, NULL, tmp, r->w * 4, 0);
        free(tmp);
    }
    r->valid = 1;
    r->dirty = 0;
}

static void rt_readback(Rt *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *dst = guest(r->va);
    uint32_t x, y;

    if (!r->dirty)
        return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                     (ID3D11Resource *)r->tex);
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
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex)))
        return NULL;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->staging);
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
    if (!r->staging || !r->rtv) {
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

typedef struct {
    uint32_t offset, w, h, pitch, color;
    uint64_t hash;
    uint32_t last_use;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} Tex;

#define MAX_TEX 256
static Tex s_tex[MAX_TEX];
static uint32_t s_frame;

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

static ID3D11ShaderResourceView *tex_get(const Nv2aTexture *t)
{
    uint32_t bytes, i;
    uint64_t h;
    Tex *e = NULL, *lru = &s_tex[0];
    uint32_t *argb;
    int fresh = 0;

    if (!t->offset || !t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    bytes = tex_bytes(t);
    rt_flush_range(t->offset, bytes);
    h = hash_bytes(guest(t->offset), bytes);
    if (t->palette)              /* P8: the same indices under a new palette */
        h = h * 31 + hash_bytes(guest(t->palette), 256 * 4);

    for (i = 0; i < MAX_TEX; i++) {
        Tex *c = &s_tex[i];
        if (c->tex && c->offset == t->offset && c->w == t->width && c->h == t->height
            && c->color == t->color && c->pitch == t->pitch) {
            e = c;
            break;
        }
        if (!c->tex || c->last_use < lru->last_use || (lru->tex && !c->tex))
            lru = c;
    }
    if (e && e->hash == h) {
        e->last_use = s_frame;
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
        fresh = 1;
    }
    argb = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    if (!argb)
        return NULL;
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
    e->last_use = s_frame;
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
    float rt_w, rt_h, zscale, textured;
    float tex_w, tex_h, alpha_ref, alpha_func;
} Consts;

/* Vertices arrive in surface pixels with the title's own integer depth and
 * 1/w. Depth is floored (the NV2A writes integers) and scaled by a power of
 * two so the comparison is exact; see ds_get. */
static const char s_hlsl[] =
"cbuffer C : register(b0) { float2 rt; float zscale; float textured;\n"
"                           float2 ts; float aref; float afunc; };\n"
"struct VI { float4 p : POSITION; float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"struct VO { float4 p : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"VO vs(VI i) {\n"
"  VO o; float w = i.p.w > 0 ? 1.0 / i.p.w : 1.0;\n"
"  float2 n = float2(i.p.x / rt.x * 2 - 1, 1 - i.p.y / rt.y * 2);\n"
"  float z = saturate(floor(i.p.z) / zscale);\n"
"  o.p = float4(n * w, z * w, w); o.c = i.c; o.uv = i.uv / ts; return o; }\n"
"Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
"float4 ps(VO i) : SV_Target {\n"
"  float4 c = i.c; if (textured > 0.5) c *= t0.Sample(s0, i.uv);\n"
"  float a = round(c.a * 255); int f = (int)afunc; bool ok = true;\n"
"  if (f == 0) ok = false; else if (f == 1) ok = a < aref;\n"
"  else if (f == 2) ok = a == aref; else if (f == 3) ok = a <= aref;\n"
"  else if (f == 4) ok = a > aref; else if (f == 5) ok = a != aref;\n"
"  else if (f == 6) ok = a >= aref;\n"
"  if (!ok) discard; return c; }\n";

static int compile(const char *entry, const char *target, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "nv2a_backend", NULL, NULL,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "compile failed");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    return 1;
}

/* ---- the back end ------------------------------------------------------ */


static float depth_scale(const Nv2aRenderState *rs)
{
    return rs->depth_max > 65536.0f ? 16777216.0f : 65536.0f;
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
        D3D11_RECT rc = { (LONG)s->clip_x, (LONG)s->clip_y,
                          (LONG)(s->clip_x + s->width), (LONG)(s->clip_y + s->height) };
        if (s_ctx1)
            ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)r->rtv, c, &rc, 1);
        else
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, c);
        r->dirty = 1;
    }
    if (flags & 0x3) {
        Ds *d = ds_get(rs->zeta_va, r->w, r->h);
        if (d) {
            /* Z24S8: depth in the top 24 bits. ponytail: Z16 surfaces read
             * the same word and lose their low bits. */
            float z = (float)(zstencil >> 8) / 16777216.0f;
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
    ID3D11ShaderResourceView *srv = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BLEND_DESC bd;
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    Consts k;
    UINT stride = sizeof(Nv2aVertex), offset = 0, bytes;
    float bf[4];

    if (!b->count)
        return;
    r = rt_get(s);
    if (!r)
        return;
    if (b->texture)
        srv = tex_get(b->texture);        /* may read back a target: before binding */
    if (rs->zeta_va && (rs->depth_test_enable || rs->depth_write || rs->stencil_enable))
        d = ds_get(rs->zeta_va, r->w, r->h);

    /* Vertices */
    bytes = b->count * stride;
    if (bytes > s_vb_bytes) {
        D3D11_BUFFER_DESC vbd;
        if (s_vb) ID3D11Buffer_Release(s_vb);
        s_vb = NULL;
        memset(&vbd, 0, sizeof vbd);
        vbd.ByteWidth = bytes * 2;
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &vbd, NULL, &s_vb)))
            return;
        s_vb_bytes = vbd.ByteWidth;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, b->vertices, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);

    /* Constants */
    k.rt_w = (float)r->w;
    k.rt_h = (float)r->h;
    k.zscale = depth_scale(rs);
    k.textured = srv ? 1.0f : 0.0f;
    k.tex_w = b->texture ? (float)b->texture->width : 1.0f;
    k.tex_h = b->texture ? (float)b->texture->height : 1.0f;
    k.alpha_ref = (float)(rs->alpha_ref & 0xFF);
    k.alpha_func = rs->alpha_test_enable && rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
                 ? (float)(rs->alpha_func - 0x200) : 7.0f;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_cb, 0, NULL, &k, 0, 0);

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
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;

    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)r->w; vp.Height = (float)r->h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    sc.left = (LONG)s->clip_x; sc.top = (LONG)s->clip_y;
    sc.right = (LONG)(s->clip_x + s->width); sc.bottom = (LONG)(s->clip_y + s->height);

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
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    if (srv) {
        D3D11_SAMPLER_DESC sd;
        uint32_t mag = (b->texture->filter >> 24) & 0xF;
        uint32_t min = (b->texture->filter >> 16) & 0xFF;
        ID3D11SamplerState *ss;
        memset(&sd, 0, sizeof sd);
        sd.Filter = (mag == 2 || min == 2 || min == 4 || min == 6)
                  ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = map_addr(b->texture->addr_u);
        sd.AddressV = map_addr(b->texture->addr_v);
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ss = s_ss_get(&sd);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &ss);
    }
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
    r->dirty = 1;
}

/* The frame is done: write every target back to guest memory, where the
 * window and the dumps read it, and treat guest memory as the truth again
 * next frame (the title may write a surface with the CPU in between). */
static void be_flip(void)
{
    uint32_t i;
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
            if (t0)
                fprintf(stderr, "[D3D11] %.1f flips/s\n",
                        (s_frame - f0) * 1000.0 / (now - t0));
            t0 = now;
            f0 = s_frame;
        }
    }
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip };

int d3d11_backend_init(void)
{
    static const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D_FEATURE_LEVEL fl;
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                           D3D11_SDK_VERSION, &s_dev, &fl, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] no hardware device (0x%08lX); software rasteriser\n",
                (unsigned long)hr);
        return 0;
    }
    ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1, (void **)&s_ctx1);
    if (!compile("vs", "vs_4_0", &vsb) || !compile("ps", "ps_4_0", &psb))
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    ID3D11Device_CreateInputLayout(s_dev, il, 3, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &s_layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    memset(&cbd, 0, sizeof cbd);
    cbd.ByteWidth = sizeof(Consts);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(s_dev, &cbd, NULL, &s_cb);
    if (!s_vs || !s_ps || !s_layout || !s_cb) {
        fprintf(stderr, "[D3D11] pipeline setup failed; software rasteriser\n");
        return 0;
    }
    nv2a_backend_register(&s_backend);
    fprintf(stderr, "[D3D11] render back end up (feature level %X)\n", (unsigned)fl);
    return 1;
}

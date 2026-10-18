/* Shooter mode: the gun in the King's hand, its muzzle flash and tracers.
 *
 * The gun is SHOOTER_GUN_DIR/<gun>/gun.obj, CoD4's third-person model
 * (tools/cod4_extract.py, from the user's own install, with a <material>.dds
 * per surface). recomp_manual.c works out where it is (the King's right
 * wrist and CoD4's grip) and submits the scene with the game's camera when
 * it leaves 3D for its HUD; d3d9_hle.c's present() then has it drawn into
 * the presented buffer against the scene's depth, before the stretch to the
 * window. Every state set here is put back with a state block. */
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"

IDirect3DTexture9 *d3d9_dds_load(const char *path);

#define GUN_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)
#define FX_FVF  (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
typedef struct { float p[3], n[3], uv[2]; } GunVert;
typedef struct { float p[3]; DWORD c; float uv[2]; } FxVert;
typedef struct { UINT first, tris; IDirect3DTexture9 *tex; } GunPart;

static struct {
    char name[64];
    IDirect3DVertexBuffer9 *vb;
    GunPart part[64];
    int nparts, failed;
} s_gun;

static IDirect3DStateBlock9 *s_saved;
static IDirect3DTexture9 *s_fx_flash, *s_fx_side, *s_fx_tracer, *s_fx_dust, *s_fx_spark;

static void gun_free(void)
{
    int i;
    for (i = 0; i < s_gun.nparts; i++)
        if (s_gun.part[i].tex)
            IDirect3DTexture9_Release(s_gun.part[i].tex);
    if (s_gun.vb)
        IDirect3DVertexBuffer9_Release(s_gun.vb);
    memset(&s_gun, 0, sizeof s_gun);
}

/* Grows *arr to hold n+1 elements of size sz. */
static void *grow(void *arr, size_t *cap, size_t n, size_t sz)
{
    if (n < *cap)
        return arr;
    *cap = *cap ? *cap * 2 : 1024;
    return realloc(arr, *cap * sz);
}

/* OBJ with v/vt/vn and triangular f a/b/c, a part per usemtl. */
static void gun_load(IDirect3DDevice9 *dev, const char *name)
{
    char path[MAX_PATH], line[512], mtl[128] = "";
    float (*pos)[3] = NULL, (*uv)[2] = NULL, (*nrm)[3] = NULL;
    GunVert *out = NULL;
    size_t np = 0, nt = 0, nn = 0, no = 0, cp = 0, ct = 0, cn = 0, co = 0;
    void *dst;
    FILE *f;

    gun_free();
    snprintf(s_gun.name, sizeof s_gun.name, "%s", name);
    snprintf(path, sizeof path, SHOOTER_GUN_DIR "/%s/gun.obj", name);
    if (!(f = fopen(path, "r"))) {
        fprintf(stderr, "[GUN] no %s\n", path);
        s_gun.failed = 1;
        return;
    }
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            pos = grow(pos, &cp, np, sizeof *pos);
            sscanf(line + 2, "%f %f %f", &pos[np][0], &pos[np][1], &pos[np][2]);
            np++;
        } else if (line[0] == 'v' && line[1] == 't') {
            uv = grow(uv, &ct, nt, sizeof *uv);
            sscanf(line + 3, "%f %f", &uv[nt][0], &uv[nt][1]);
            uv[nt][1] = 1.0f - uv[nt][1];
            nt++;
        } else if (line[0] == 'v' && line[1] == 'n') {
            nrm = grow(nrm, &cn, nn, sizeof *nrm);
            sscanf(line + 3, "%f %f %f", &nrm[nn][0], &nrm[nn][1], &nrm[nn][2]);
            nn++;
        } else if (!strncmp(line, "usemtl ", 7) && s_gun.nparts < 64) {
            GunPart *p = &s_gun.part[s_gun.nparts++];
            sscanf(line + 7, "%127s", mtl);
            snprintf(path, sizeof path, SHOOTER_GUN_DIR "/%s/%s.dds", name, mtl);
            p->first = (UINT)no;
            p->tex = d3d9_dds_load(path);
        } else if (line[0] == 'f' && line[1] == ' ' && s_gun.nparts) {
            char *s = line + 2;
            int k;
            for (k = 0; k < 3; k++) {
                long a = strtol(s, &s, 10), b = 0, c = 0;
                if (*s == '/') b = strtol(s + 1, &s, 10);
                if (*s == '/') c = strtol(s + 1, &s, 10);
                out = grow(out, &co, no, sizeof *out);
                memset(&out[no], 0, sizeof *out);
                if (a > 0 && (size_t)a <= np) memcpy(out[no].p, pos[a - 1], sizeof out[no].p);
                if (b > 0 && (size_t)b <= nt) memcpy(out[no].uv, uv[b - 1], sizeof out[no].uv);
                if (c > 0 && (size_t)c <= nn) memcpy(out[no].n, nrm[c - 1], sizeof out[no].n);
                no++;
            }
            s_gun.part[s_gun.nparts - 1].tris++;
        }
    }
    fclose(f);
    if (no && SUCCEEDED(IDirect3DDevice9_CreateVertexBuffer(dev, (UINT)(no * sizeof *out), 0,
                                                             GUN_FVF, D3DPOOL_MANAGED, &s_gun.vb, NULL))
        && SUCCEEDED(IDirect3DVertexBuffer9_Lock(s_gun.vb, 0, 0, &dst, 0))) {
        memcpy(dst, out, no * sizeof *out);
        IDirect3DVertexBuffer9_Unlock(s_gun.vb);
        fprintf(stderr, "[GUN] %s: %u triangles, %d parts\n", name, (unsigned)(no / 3), s_gun.nparts);
    } else {
        s_gun.failed = 1;
    }
    free(pos); free(uv); free(nrm); free(out);
}

static void fx_load(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    s_fx_flash = d3d9_dds_load(SHOOTER_FX_DIR "/muzflash.dds");
    s_fx_side = d3d9_dds_load(SHOOTER_FX_DIR "/muzflash_side.dds");
    s_fx_tracer = d3d9_dds_load(SHOOTER_FX_DIR "/tracer.dds");
    s_fx_dust = d3d9_dds_load(SHOOTER_FX_DIR "/impact_dust.dds");
    s_fx_spark = d3d9_dds_load(SHOOTER_FX_DIR "/impact_spark.dds");
}

static void v3sub(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static void v3cross(const float a[3], const float b[3], float o[3])
{
    float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(o, r, sizeof r);
}
static void v3norm(float v[3])
{
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-6f) { v[0] /= n; v[1] /= n; v[2] /= n; }
}

/* A quad around c spanned by half-vectors u and v, into four FxVerts (strip order). */
static void quad(FxVert q[4], const float c[3], const float u[3], const float v[3], DWORD col)
{
    static const float su[4] = { -1, 1, -1, 1 }, sv[4] = { -1, -1, 1, 1 };
    int i, k;
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 3; k++)
            q[i].p[k] = c[k] + u[k] * su[i] + v[k] * sv[i];
        q[i].c = col;
        q[i].uv[0] = su[i] * 0.5f + 0.5f;
        q[i].uv[1] = sv[i] * 0.5f + 0.5f;
    }
}

static void draw_fx(IDirect3DDevice9 *dev, const ShooterScene *sc)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float eye[3], right[3], up[3];
    FxVert q[4];
    int i;

    /* the camera: right and up are the view's first two columns; the eye
     * is -translation * transpose(rotation) */
    for (i = 0; i < 3; i++) {
        right[i] = sc->view[i * 4 + 0];
        up[i] = sc->view[i * 4 + 1];
        eye[i] = -(sc->view[12] * sc->view[i * 4 + 0] + sc->view[13] * sc->view[i * 4 + 1]
                   + sc->view[14] * sc->view[i * 4 + 2]);
    }
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, (const D3DMATRIX *)ident);
    IDirect3DDevice9_SetFVF(dev, FX_FVF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    /* tracers: a thin strip from the muzzle toward the hit, turned to face us */
    if (s_fx_tracer) {
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)s_fx_tracer);
        for (i = 0; i < SHOOTER_TRACERS; i++) {
            float d[3], to_eye[3], side[3], len, c[3], u[3], head = 0.0f;
            int a = (int)(sc->tracer[i].life / 0.06f * 255.0f), k;
            if (sc->tracer[i].life <= 0.0f)
                continue;
            v3sub(sc->tracer[i].b, sc->tracer[i].a, d);
            len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len < 1.0f)
                continue;
            /* a streak a third of the way along, moving out as it fades */
            head = 1.0f - sc->tracer[i].life / 0.06f;
            for (k = 0; k < 3; k++)
                c[k] = sc->tracer[i].a[k] + d[k] * (0.15f + 0.6f * head);
            v3sub(eye, c, to_eye);
            v3cross(d, to_eye, side);
            v3norm(side);
            for (k = 0; k < 3; k++) {
                side[k] *= 1.6f;
                u[k] = d[k] / len * fminf(len * 0.25f, 260.0f);
            }
            a = a < 0 ? 0 : a > 255 ? 255 : a;
            quad(q, c, side, u, D3DCOLOR_ARGB(255, a, a * 9 / 10, a * 6 / 10));
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
        }
    }
    /* muzzle flash: a star facing us and a flare along the barrel */
    if (sc->flash > 0.0f && sc->draw_gun) {
        float bar[3] = { sc->muzzle_dir[0], sc->muzzle_dir[1], sc->muzzle_dir[2] }, s = 18.0f;
        float u[3], v[3], ang = (float)(rand() % 628) / 100.0f, cs = cosf(ang), sn = sinf(ang);
        float side[3], to_eye[3], c[3];
        int k;
        v3norm(bar);
        if (s_fx_flash) {
            for (k = 0; k < 3; k++) {
                u[k] = (right[k] * cs + up[k] * sn) * s;
                v[k] = (-right[k] * sn + up[k] * cs) * s;
            }
            IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)s_fx_flash);
            quad(q, sc->muzzle, u, v, D3DCOLOR_ARGB(255, 255, 230, 180));
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
        }
        if (s_fx_side) {
            for (k = 0; k < 3; k++)
                c[k] = sc->muzzle[k] + bar[k] * s * 1.2f;
            v3sub(eye, c, to_eye);
            v3cross(bar, to_eye, side);
            v3norm(side);
            for (k = 0; k < 3; k++) {
                side[k] *= s * 0.6f;
                u[k] = bar[k] * s * 1.3f;
            }
            IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)s_fx_side);
            quad(q, c, side, u, D3DCOLOR_ARGB(255, 255, 220, 160));
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
        }
    }
    /* impacts: a spark for a moment, then a puff of dust that grows and fades */
    for (i = 0; i < SHOOTER_IMPACTS; i++) {
        float age = SHOOTER_IMPACT_LIFE - sc->impact[i].life, u[3], v[3], sz;
        int k, a;
        if (sc->impact[i].life <= 0.0f || !s_fx_spark || age > 0.07f)
            continue;
        sz = 7.0f + age * 120.0f;
        for (k = 0; k < 3; k++) { u[k] = right[k] * sz; v[k] = up[k] * sz; }
        a = (int)(255.0f * (1.0f - age / 0.07f));
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)s_fx_spark);
        quad(q, sc->impact[i].p, u, v, D3DCOLOR_ARGB(255, a, a * 8 / 10, a / 2));
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    }
    if (s_fx_dust) {
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)s_fx_dust);
        for (i = 0; i < SHOOTER_IMPACTS; i++) {
            float t = 1.0f - sc->impact[i].life / SHOOTER_IMPACT_LIFE, u[3], v[3], sz, ang = (float)i * 2.1f;
            float cs = cosf(ang), sn = sinf(ang);
            int k, a;
            if (sc->impact[i].life <= 0.0f)
                continue;
            sz = 10.0f + 26.0f * sqrtf(t);
            for (k = 0; k < 3; k++) {
                u[k] = (right[k] * cs + up[k] * sn) * sz;
                v[k] = (-right[k] * sn + up[k] * cs) * sz;
            }
            a = (int)(230.0f * (1.0f - t));
            quad(q, sc->impact[i].p, u, v, D3DCOLOR_ARGB(a, 200, 185, 160));
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
        }
    }
}

static ShooterScene s_pending;
static int s_have_pending;

/* The scene, kept for the end of the frame: the game's first switch from 3D
 * to 2D can come before the world is drawn, which would paint over us. */
void gun_world_submit(const ShooterScene *sc)
{
    s_pending = *sc;
    s_have_pending = 1;
}

/* -- Skate mode: the board ------------------------------------------------ */

/* Built from boxes in board space (inches, X along the board, Y up, Z across,
 * the ground at Y 0): a 32" deck with kicked nose and tail, two trucks, four
 * wheels. Vertex colours, lit like the gun. */
#define BOARD_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE)
#define BOARD_FILE "game/mods/skate3/board.bin"
typedef struct { float p[3], n[3]; DWORD c; } BoardVert;

static BoardVert s_box[36 * 16], *s_board = s_box;
static int s_board_n;
static struct { float view[16], proj[16], world[16]; int pending; } s_bd;

/* A box of half-size h at c, turned by `pitch` about Z, faces coloured top / sides / bottom. */
static void board_box(const float c[3], const float h[3], float pitch, DWORD top, DWORD side, DWORD bottom)
{
    static const int f[6][4] = { { 2, 3, 7, 6 }, { 0, 4, 5, 1 }, { 1, 5, 7, 3 }, { 0, 2, 6, 4 },
                                 { 4, 6, 7, 5 }, { 0, 1, 3, 2 } };   /* +Y -Y +X -X +Z -Z */
    static const float nrm[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 },
                                     { 0, 0, 1 }, { 0, 0, -1 } };
    float v[8][3], cs = cosf(pitch), sn = sinf(pitch);
    int i, k;
    for (i = 0; i < 8; i++) {
        float x = (i & 1 ? h[0] : -h[0]), y = (i & 2 ? h[1] : -h[1]), z = (i & 4 ? h[2] : -h[2]);
        v[i][0] = c[0] + x * cs - y * sn;
        v[i][1] = c[1] + x * sn + y * cs;
        v[i][2] = c[2] + z;
    }
    for (k = 0; k < 6; k++) {
        static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        DWORD col = k == 0 ? top : k == 1 ? bottom : side;
        float n[3] = { nrm[k][0] * cs - nrm[k][1] * sn, nrm[k][0] * sn + nrm[k][1] * cs, nrm[k][2] };
        for (i = 0; i < 6 && s_board_n < (int)(sizeof s_box / sizeof s_box[0]); i++) {
            BoardVert *bv = &s_board[s_board_n++];
            memcpy(bv->p, v[f[k][tri[i]]], sizeof bv->p);
            memcpy(bv->n, n, sizeof bv->n);
            bv->c = col;
        }
    }
}

static void board_build(void)
{
    const DWORD grip = D3DCOLOR_XRGB(28, 28, 30), ply = D3DCOLOR_XRGB(196, 150, 96);
    const DWORD art = D3DCOLOR_XRGB(205, 40, 35), metal = D3DCOLOR_XRGB(170, 172, 178);
    const DWORD wheel = D3DCOLOR_XRGB(240, 236, 220);
    float c[3], h[3];
    int s, w;
    FILE *f;
    unsigned n;
    if (s_board_n)
        return;
    /* Skate 3's own board, from tools/skate3_anim.py: u32 triangles, then
     * per vertex pos[3], normal[3] and a colour, laid out as BoardVert */
    if ((f = fopen(BOARD_FILE, "rb")) != NULL) {
        if (fread(&n, 4, 1, f) == 1 && n && n < 100000
            && (s_board = (BoardVert *)malloc(n * 3 * sizeof *s_board)) != NULL
            && fread(s_board, sizeof *s_board, n * 3, f) == n * 3) {
            s_board_n = (int)n * 3;
            fclose(f);
            fprintf(stderr, "[BOARD] %u triangles from %s\n", n, BOARD_FILE);
            return;
        }
        free(s_board == s_box ? NULL : s_board);
        s_board = s_box;
        fclose(f);
    }
    /* deck: flat middle, nose (+X) and tail kicked up 18 degrees */
    c[0] = 0; c[1] = 3.35f; c[2] = 0; h[0] = 11.5f; h[1] = 0.25f; h[2] = 4.0f;
    board_box(c, h, 0.0f, grip, ply, art);
    for (s = -1; s <= 1; s += 2) {
        float a = 0.314f * s;
        c[0] = s * (11.5f + 2.6f * cosf(a)); c[1] = 3.35f + 2.6f * fabsf(sinf(a)); c[2] = 0;
        h[0] = 2.7f; h[1] = 0.25f; h[2] = 3.8f;
        board_box(c, h, a, grip, ply, art);
    }
    /* trucks: baseplate and hanger, then the wheels at the hanger's ends */
    for (s = -1; s <= 1; s += 2) {
        c[0] = s * 7.0f; c[1] = 2.6f; c[2] = 0; h[0] = 1.3f; h[1] = 0.5f; h[2] = 1.2f;
        board_box(c, h, 0.0f, metal, metal, metal);
        c[1] = 1.7f; h[0] = 0.5f; h[1] = 0.45f; h[2] = 3.9f;
        board_box(c, h, 0.0f, metal, metal, metal);
        for (w = -1; w <= 1; w += 2) {
            c[0] = s * 7.0f; c[1] = 1.05f; c[2] = w * 3.6f; h[0] = 1.05f; h[1] = 1.05f; h[2] = 0.7f;
            board_box(c, h, 0.0f, wheel, wheel, wheel);
            board_box(c, h, 0.785f, wheel, wheel, wheel);   /* an octagon, roughly */
        }
    }
}

void board_world_submit(const float view[16], const float proj[16], const float world[16])
{
    memcpy(s_bd.view, view, sizeof s_bd.view);
    memcpy(s_bd.proj, proj, sizeof s_bd.proj);
    memcpy(s_bd.world, world, sizeof s_bd.world);
    s_bd.pending = 1;
}

static void board_draw(IDirect3DDevice9 *dev)
{
    D3DLIGHT9 light;
    D3DMATERIAL9 mat;
    int i;
    board_build();
    if (!s_saved && FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &s_saved)))
        return;
    IDirect3DStateBlock9_Capture(s_saved);
    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, (const D3DMATRIX *)s_bd.view);
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, (const D3DMATRIX *)s_bd.proj);
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, (const D3DMATRIX *)s_bd.world);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, BOARD_FVF);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SPECULARENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_NORMALIZENORMALS, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORVERTEX, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_AMBIENT, D3DCOLOR_XRGB(120, 120, 125));
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SLOPESCALEDEPTHBIAS, 0);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    memset(&light, 0, sizeof light);
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 0.7f;
    light.Direction.x = 0.3f; light.Direction.y = -0.9f; light.Direction.z = 0.3f;
    IDirect3DDevice9_SetLight(dev, 0, &light);
    IDirect3DDevice9_LightEnable(dev, 0, TRUE);
    for (i = 1; i < 8; i++)
        IDirect3DDevice9_LightEnable(dev, i, FALSE);
    memset(&mat, 0, sizeof mat);
    mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1.0f;
    mat.Ambient = mat.Diffuse;
    IDirect3DDevice9_SetMaterial(dev, &mat);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, s_board_n / 3, s_board, sizeof s_board[0]);
    if (getenv("RECOMP_SKATE_DEBUG")) {
        static DWORD last;
        if (GetTickCount() - last > 1000) {
            last = GetTickCount();
            fprintf(stderr, "[BOARD] %d verts at %.0f %.0f %.0f x-axis %.2f %.2f %.2f view row3 %.0f %.0f %.0f\n", s_board_n,
                    s_bd.world[12], s_bd.world[13], s_bd.world[14], s_bd.world[0], s_bd.world[1], s_bd.world[2],
                    s_bd.view[12], s_bd.view[13], s_bd.view[14]);
        }
    }
    IDirect3DStateBlock9_Apply(s_saved);
}

/* d3d9_hle.c's present(), before the frame is stretched to the window: the
 * guest's colour and depth targets still hold the finished 3D scene. */
void gun_world_flush(void *dev_)
{
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)dev_;
    if (!s_have_pending && !s_bd.pending)
        return;
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(dev))) {
        if (s_have_pending)
            gun_world_draw(dev, &s_pending);
        if (s_bd.pending)
            board_draw(dev);
        IDirect3DDevice9_EndScene(dev);
    }
    s_have_pending = 0;
    s_bd.pending = 0;
}

/* Into the targets present() bound: the presented guest buffer and its depth. */
void gun_world_draw(void *dev_, const ShooterScene *sc)
{
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)dev_;
    D3DLIGHT9 light;
    D3DMATERIAL9 mat;
    int i;

    if (sc->draw_gun && strcmp(sc->gun, s_gun.name) != 0)
        gun_load(dev, sc->gun);
    fx_load();
    if (!s_saved && FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &s_saved)))
        return;
    IDirect3DStateBlock9_Capture(s_saved);

    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, (const D3DMATRIX *)sc->view);
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, (const D3DMATRIX *)sc->proj);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SPECULARENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_NORMALIZENORMALS, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORVERTEX, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SLOPESCALEDEPTHBIAS, 0);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    if (sc->draw_gun && s_gun.vb && !s_gun.failed) {
        IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, (const D3DMATRIX *)sc->gun_world);
        IDirect3DDevice9_SetFVF(dev, GUN_FVF);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, TRUE);
        /* lit from above, with the muzzle flash warming it for a moment */
        IDirect3DDevice9_SetRenderState(dev, D3DRS_AMBIENT, sc->flash > 0.0f
                                        ? D3DCOLOR_XRGB(200, 170, 120) : D3DCOLOR_XRGB(110, 110, 115));
        memset(&light, 0, sizeof light);
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 0.75f;
        light.Direction.x = 0.3f; light.Direction.y = -0.9f; light.Direction.z = 0.3f;
        IDirect3DDevice9_SetLight(dev, 0, &light);
        IDirect3DDevice9_LightEnable(dev, 0, TRUE);
        for (i = 1; i < 8; i++)
            IDirect3DDevice9_LightEnable(dev, i, FALSE);
        memset(&mat, 0, sizeof mat);
        mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1.0f;
        mat.Ambient = mat.Diffuse;
        IDirect3DDevice9_SetMaterial(dev, &mat);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetStreamSource(dev, 0, s_gun.vb, 0, sizeof(GunVert));
        for (i = 0; i < s_gun.nparts; i++) {
            const GunPart *pt = &s_gun.part[i];
            if (!pt->tris)
                continue;
            IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)pt->tex);
            IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP,
                                                  pt->tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
            IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, pt->first, pt->tris);
        }
    }
    draw_fx(dev, sc);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DStateBlock9_Apply(s_saved);
}

/* Shooter mode: the gun in the King's hand, its muzzle flash and tracers.
 *
 * The gun is SHOOTER_GUN_DIR/<gun>/gun.obj, CoD4's third-person model
 * (tools/cod4_extract.py, from the user's own install, with a <material>.dds
 * per surface). recomp_manual.c works out where it is (the King's right
 * wrist and CoD4's grip) and submits the scene with the game's camera when
 * it leaves 3D for its HUD; the renderer's present() then has it drawn into
 * the presented buffer against the scene's depth, before the stretch to the
 * window, through GL ES 3 (gl_hle.c sets all of its own state at each draw,
 * so nothing needs putting back). */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"

#include <GLES3/gl3.h>
typedef GLuint MTex;
GLuint gl_dds_load(const char *path);
#define tex_load gl_dds_load
#define D3DCOLOR_ARGB(a, r, g, b) \
    ((DWORD)((((a) & 0xFF) << 24) | (((r) & 0xFF) << 16) | (((g) & 0xFF) << 8) | ((b) & 0xFF)))
#define D3DCOLOR_XRGB(r, g, b) D3DCOLOR_ARGB(0xFF, r, g, b)

typedef struct { float p[3], n[3], uv[2]; } GunVert;
typedef struct { float p[3]; DWORD c; float uv[2]; } FxVert;
typedef struct { UINT first, tris; MTex tex; } GunPart;

static struct {
    char name[64];
    GLuint vb;
    GunPart part[64];
    int nparts, failed;
} s_gun;

static MTex s_fx_flash, s_fx_side, s_fx_tracer, s_fx_dust, s_fx_spark;

static void gun_free(void)
{
    int i;
    for (i = 0; i < s_gun.nparts; i++)
        if (s_gun.part[i].tex) {
            glDeleteTextures(1, &s_gun.part[i].tex);
        }
    if (s_gun.vb) {
        glDeleteBuffers(1, &s_gun.vb);
    }
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
static void gun_load(void *dev, const char *name)
{
    char path[MAX_PATH], line[512], mtl[128] = "";
    float (*pos)[3] = NULL, (*uv)[2] = NULL, (*nrm)[3] = NULL;
    GunVert *out = NULL;
    size_t np = 0, nt = 0, nn = 0, no = 0, cp = 0, ct = 0, cn = 0, co = 0;
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
            p->tex = tex_load(path);
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
    (void)dev;
    if (no) {
        glGenBuffers(1, &s_gun.vb);
        glBindBuffer(GL_ARRAY_BUFFER, s_gun.vb);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(no * sizeof *out), out, GL_STATIC_DRAW);
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
    s_fx_flash = tex_load(SHOOTER_FX_DIR "/muzflash.dds");
    s_fx_side = tex_load(SHOOTER_FX_DIR "/muzflash_side.dds");
    s_fx_tracer = tex_load(SHOOTER_FX_DIR "/tracer.dds");
    s_fx_dust = tex_load(SHOOTER_FX_DIR "/impact_dust.dds");
    s_fx_spark = tex_load(SHOOTER_FX_DIR "/impact_spark.dds");
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

/* per back end, at the end of the file */
static void fx_begin(void *dev);
static void fx_quad(void *dev, MTex t, const FxVert q[4]);
static void fx_dust(void *dev);

static void draw_fx(void *dev, const ShooterScene *sc)
{
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
    fx_begin(dev);

    /* tracers: a thin strip from the muzzle toward the hit, turned to face us */
    if (s_fx_tracer) {
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
            fx_quad(dev, s_fx_tracer, q);
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
            quad(q, sc->muzzle, u, v, D3DCOLOR_ARGB(255, 255, 230, 180));
            fx_quad(dev, s_fx_flash, q);
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
            quad(q, c, side, u, D3DCOLOR_ARGB(255, 255, 220, 160));
            fx_quad(dev, s_fx_side, q);
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
        quad(q, sc->impact[i].p, u, v, D3DCOLOR_ARGB(255, a, a * 8 / 10, a / 2));
        fx_quad(dev, s_fx_spark, q);
    }
    if (s_fx_dust) {
        fx_dust(dev);
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
            fx_quad(dev, s_fx_dust, q);
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


/* -- Drawing --------------------------------------------------------------------
 * D3D9-style fixed function as one small program: a directional light and the
 * ambient over a material that is white or the vertex colour, times the
 * texture. Matrices are D3D's (row vectors), loaded as they are: GLSL reads
 * them transposed, so M * v is D3D's v * M. gl_hle.c's present() binds the
 * targets and viewport; clip y is negated and z remapped as gl_hle.c does. */

static const char *const s_mm_vs =
    "#version 300 es\n"
    "uniform mat4 u_wvp, u_world;\n"
    "uniform vec3 u_ambient, u_light;\n"
    "uniform int u_lit, u_colvert;\n"
    "in vec3 a_pos; in vec3 a_nrm; in vec4 a_col; in vec2 a_uv;\n"
    "out vec4 v_col; out vec2 v_uv;\n"
    "void main() {\n"
    "  vec4 p = u_wvp * vec4(a_pos, 1.0);\n"
    "  gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w);\n"
    "  vec4 c = u_colvert != 0 ? a_col.bgra : vec4(1.0);\n"   /* D3DCOLOR is BGRA in memory */
    "  if (u_lit != 0) {\n"
    "    vec3 n = normalize(mat3(u_world) * a_nrm);\n"
    "    c.rgb = clamp(c.rgb * (u_ambient + u_light * max(dot(n, -normalize(vec3(0.3, -0.9, 0.3))), 0.0)), 0.0, 1.0);\n"
    "  }\n"
    "  v_col = c; v_uv = a_uv;\n"
    "}\n";
static const char *const s_mm_fs =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_use_tex, u_tex_alpha;\n"
    "in vec4 v_col; in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "  vec4 c = v_col;\n"
    "  if (u_use_tex != 0) {\n"
    "    vec4 t = texture(u_tex, v_uv);\n"
    "    c.rgb *= t.rgb;\n"
    "    if (u_tex_alpha != 0) c.a *= t.a;\n"
    "  }\n"
    "  o_color = c;\n"
    "}\n";

static GLuint s_mm_prog, s_mm_vao, s_mm_vbo, s_mm_wrap, s_mm_clamp;
static GLint s_u_wvp, s_u_world, s_u_ambient, s_u_light, s_u_lit, s_u_colvert, s_u_use_tex, s_u_tex_alpha;
static float s_mm_vp[16];                  /* view * proj of the pass being drawn */

static GLuint mm_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[MODS] shader: %s\n", log);
    }
    return sh;
}

static int mm_init(void)
{
    static const char *const attr[] = { "a_pos", "a_nrm", "a_col", "a_uv" };
    static int failed;
    GLint ok = 0;
    GLuint i;
    if (s_mm_prog || failed)
        return s_mm_prog != 0;
    s_mm_prog = glCreateProgram();
    glAttachShader(s_mm_prog, mm_shader(GL_VERTEX_SHADER, s_mm_vs));
    glAttachShader(s_mm_prog, mm_shader(GL_FRAGMENT_SHADER, s_mm_fs));
    for (i = 0; i < 4; i++)
        glBindAttribLocation(s_mm_prog, i, attr[i]);
    glLinkProgram(s_mm_prog);
    glGetProgramiv(s_mm_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "[MODS] the mods' GL program does not link\n");
        glDeleteProgram(s_mm_prog);
        s_mm_prog = 0;
        failed = 1;
        return 0;
    }
    s_u_wvp = glGetUniformLocation(s_mm_prog, "u_wvp");
    s_u_world = glGetUniformLocation(s_mm_prog, "u_world");
    s_u_ambient = glGetUniformLocation(s_mm_prog, "u_ambient");
    s_u_light = glGetUniformLocation(s_mm_prog, "u_light");
    s_u_lit = glGetUniformLocation(s_mm_prog, "u_lit");
    s_u_colvert = glGetUniformLocation(s_mm_prog, "u_colvert");
    s_u_use_tex = glGetUniformLocation(s_mm_prog, "u_use_tex");
    s_u_tex_alpha = glGetUniformLocation(s_mm_prog, "u_tex_alpha");
    glGenVertexArrays(1, &s_mm_vao);
    glGenBuffers(1, &s_mm_vbo);
    glGenSamplers(1, &s_mm_wrap);
    glGenSamplers(1, &s_mm_clamp);
    glSamplerParameteri(s_mm_wrap, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(s_mm_wrap, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(s_mm_clamp, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(s_mm_clamp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(s_mm_clamp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(s_mm_clamp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUseProgram(s_mm_prog);
    glUniform1i(glGetUniformLocation(s_mm_prog, "u_tex"), 0);
    return 1;
}

/* o = a * b, D3D row-major */
static void mat_mul(const float a[16], const float b[16], float o[16])
{
    int r, c, k;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) {
            float v = 0.0f;
            for (k = 0; k < 4; k++)
                v += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = v;
        }
}

/* The program and depth state for a pass: opaque, depth tested and written. */
static void mm_begin(const float view[16], const float proj[16])
{
    mat_mul(view, proj, s_mm_vp);
    glUseProgram(s_mm_prog);
    glBindVertexArray(s_mm_vao);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUniform1i(s_u_tex_alpha, 0);
}

static void mm_world(const float world[16])
{
    float wvp[16];
    mat_mul(world, s_mm_vp, wvp);
    glUniformMatrix4fv(s_u_wvp, 1, GL_FALSE, wvp);
    glUniformMatrix4fv(s_u_world, 1, GL_FALSE, world);
}

static void mm_light(int lit, int colvert, DWORD ambient, float diffuse)
{
    float a[3] = { ((ambient >> 16) & 0xFF) / 255.0f, ((ambient >> 8) & 0xFF) / 255.0f, (ambient & 0xFF) / 255.0f };
    glUniform1i(s_u_lit, lit);
    glUniform1i(s_u_colvert, colvert);
    glUniform3fv(s_u_ambient, 1, a);
    glUniform3f(s_u_light, diffuse, diffuse, diffuse);
}

static void mm_tex(MTex t, GLuint sampler)
{
    glUniform1i(s_u_use_tex, t != 0);
    glBindTexture(GL_TEXTURE_2D, t);
    glBindSampler(0, sampler);
}

/* Attribute layout of the bound array buffer, for a vertex with (offsets,
 * or -1 for none) position, normal, colour and texture coordinate. */
static void mm_attribs(GLsizei stride, int nrm, int col, int uv)
{
    const int off[4] = { 0, nrm, col, uv };
    GLuint i;
    for (i = 0; i < 4; i++) {
        if (off[i] < 0) {
            glDisableVertexAttribArray(i);
            continue;
        }
        glEnableVertexAttribArray(i);
        if (i == 2)
            glVertexAttribPointer(i, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void *)(uintptr_t)off[i]);
        else
            glVertexAttribPointer(i, i == 3 ? 2 : 3, GL_FLOAT, GL_FALSE, stride, (const void *)(uintptr_t)off[i]);
    }
    glVertexAttrib4f(1, 0, 0, 0, 0);
    glVertexAttrib4f(2, 1, 1, 1, 1);
}

static void mm_stream(const void *v, size_t bytes)
{
    glBindBuffer(GL_ARRAY_BUFFER, s_mm_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, v, GL_STREAM_DRAW);
}

static void fx_begin(void *dev)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    (void)dev;
    mm_world(ident);
    mm_light(0, 1, 0, 0.0f);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);
}

static void fx_quad(void *dev, MTex t, const FxVert q[4])
{
    (void)dev;
    mm_tex(t, s_mm_clamp);
    mm_stream(q, 4 * sizeof *q);
    mm_attribs(sizeof *q, -1, 12, 16);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* the dust: alpha blended, with the texture's alpha */
static void fx_dust(void *dev)
{
    (void)dev;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1i(s_u_tex_alpha, 1);
}

static void board_draw(void)
{
    board_build();
    mm_begin(s_bd.view, s_bd.proj);
    mm_world(s_bd.world);
    mm_light(1, 1, D3DCOLOR_XRGB(120, 120, 125), 0.7f);
    mm_tex(0, s_mm_wrap);
    mm_stream(s_board, (size_t)s_board_n * sizeof *s_board);
    mm_attribs(sizeof *s_board, 12, 24, -1);
    glDrawArrays(GL_TRIANGLES, 0, s_board_n);
}

/* gl_hle.c's present(), before the frame is blitted to the window, with the
 * presented guest buffer and the scene's depth bound. */
void gun_world_flush(void *dev)
{
    if ((s_have_pending || s_bd.pending) && mm_init()) {
        if (s_have_pending)
            gun_world_draw(dev, &s_pending);
        if (s_bd.pending)
            board_draw();
        glBindSampler(0, 0);
    }
    s_have_pending = 0;
    s_bd.pending = 0;
}

void gun_world_draw(void *dev, const ShooterScene *sc)
{
    int i;
    if (sc->draw_gun && strcmp(sc->gun, s_gun.name) != 0)
        gun_load(dev, sc->gun);
    fx_load();
    if (!mm_init())
        return;
    mm_begin(sc->view, sc->proj);
    if (sc->draw_gun && s_gun.vb && !s_gun.failed) {
        mm_world(sc->gun_world);
        /* lit from above, with the muzzle flash warming it for a moment */
        mm_light(1, 0, sc->flash > 0.0f ? D3DCOLOR_XRGB(200, 170, 120) : D3DCOLOR_XRGB(110, 110, 115), 0.75f);
        glBindBuffer(GL_ARRAY_BUFFER, s_gun.vb);
        mm_attribs(sizeof(GunVert), 12, -1, 24);
        for (i = 0; i < s_gun.nparts; i++) {
            const GunPart *pt = &s_gun.part[i];
            if (!pt->tris)
                continue;
            mm_tex(pt->tex, s_mm_wrap);
            glDrawArrays(GL_TRIANGLES, (GLint)pt->first, (GLsizei)(pt->tris * 3));
        }
    }
    draw_fx(dev, sc);
}


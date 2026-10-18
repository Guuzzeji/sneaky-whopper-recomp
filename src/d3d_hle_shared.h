/*
 * d3d_hle_shared.h: the guest side of the D3D8 HLE, shared by the host back
 * ends (d3d9_hle.c on Direct3D 9, gl_hle.c on OpenGL ES 3 / WebGL2). Guest
 * state addresses and enums, argument access, resource decoding, vertex
 * declaration reading and the NV2A vertex program translator. Included once,
 * by the one back end a build compiles.
 */
#ifndef D3D_HLE_SHARED_H
#define D3D_HLE_SHARED_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"

/* XbSymbolDatabase (analysis/xbsymbol/xbsymbol_vars.json). */
#define XB_RENDER_STATE      0x0020D090u   /* D3D_g_RenderState[166] */
#define XB_TEXTURE_STATE     0x0020CE90u   /* D3D_g_DeferredTextureState[4][32] */
#define XB_STREAMS           0x0020F7F8u   /* D3D_g_Stream[16] */
#define XB_DEVICE_PTR        0x0020D328u   /* D3D_g_pDevice */

/* -- Call trace ----------------------------------------------------------- */

static long s_trace_left = -1;

static void hle_trace(const char *name, int nargs)
{
    int i;
    if (s_trace_left < 0) {
        const char *e = getenv("RECOMP_D3D_TRACE");
        s_trace_left = e ? atol(e) : 0;
    }
    if (s_trace_left == 0)
        return;
    {
        /* RECOMP_D3D_TRACE_ONLY=Name,Name: just those entry points. */
        static const char *only = (const char *)-1;
        if (only == (const char *)-1)
            only = getenv("RECOMP_D3D_TRACE_ONLY");
        if (only && !strstr(only, name))
            return;
    }
    s_trace_left--;
    fprintf(stderr, "[D3D] %-32s ecx=%08X edx=%08X", name, g_ecx, g_edx);
    for (i = 0; i < nargs; i++)
        fprintf(stderr, " %08X", MEM32(g_esp + 4 + 4 * i));
    fprintf(stderr, "  (ret %08X)\n", MEM32(g_esp));
}

/* -- Wrapped entry points ------------------------------------------------- */

extern void sub_001FF350_gen(void);
extern void sub_00200110_gen(void);
extern void sub_001F91D0_gen(void);
extern void sub_001F0A90_gen(void);
extern void sub_001F0E30_gen(void);
extern void sub_001F1570_gen(void);
extern void sub_001F1220_gen(void);
extern void sub_001F1590_gen(void);
extern void sub_001EFB70_gen(void);
extern void sub_001F17C0_gen(void);
extern void sub_001F1A80_gen(void);
extern void sub_001EFD60_gen(void);
extern void sub_001F1C10_gen(void);
extern void sub_001F1DF0_gen(void);
extern void sub_001F3AA0_gen(void);
extern void sub_001F1E80_gen(void);
extern void sub_001FEA80_gen(void);
extern void sub_001FEBF0_gen(void);
extern void sub_001FED30_gen(void);
extern void sub_001FEDD0_gen(void);
extern void sub_001F34C0_gen(void);
extern void sub_001F4000_gen(void);
extern void sub_001F3EF0_gen(void);
extern void sub_001F3620_gen(void);
extern void sub_001F36D0_gen(void);
extern void sub_001F3860_gen(void);
extern void sub_00200640_gen(void);
extern void sub_00200AF0_gen(void);
extern void sub_00200CF0_gen(void);
extern void sub_00200690_gen(void);
extern void sub_001F3D90_gen(void);
extern void sub_00201180_gen(void);
extern void sub_00201000_gen(void);
extern void sub_00200FC0_gen(void);
extern void sub_001EEF60_gen(void);
extern void sub_001EEAD0_gen(void);
void mkb_set_menu_cursor(int on);           /* toolkit src/input/mkb_input.h */
extern void sub_001EFF50_gen(void);
extern void sub_001EEFC0_gen(void);
extern void sub_001F0330_gen(void);

/* -- Guest memory --------------------------------------------------------- */

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

/* Pop the return address and n stack args, as `ret 4*n` would. */
static void stdcall_ret(int n, uint32_t eax_val)
{
    g_eax = eax_val;
    g_esp += 4 + 4 * n;
}

static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t u32f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void *gp(uint32_t va) { return (void *)XBOX_PTR(va); }

/* A resource's Data. D3D stores the GPU (physical) address for anything in
 * contiguous memory; that memory is mapped at 0x80000000 + phys. */
static uint32_t res_data(uint32_t res)
{
    uint32_t d = MEM32(res + 4);
    return d < 0x80000000u ? d | 0x80000000u : d;
}

/* ponytail: samples every 61st dword plus the first and last 256; a title that
 * rewrites a few texels in place between samples keeps the old upload. Full
 * hashing of every bound texture every frame costs more than the frame. */
static uint32_t hash_bytes(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u, i, step = 61 * 4;
    for (i = 0; i + 4 <= n && i < 256; i += 4)
        h = (h ^ *(const uint32_t *)(p + i)) * 16777619u;
    for (; i + 4 <= n; i += step)
        h = (h ^ *(const uint32_t *)(p + i)) * 16777619u;
    for (i = n > 256 ? n - 256 : 0; i + 4 <= n; i += 4)
        h = (h ^ *(const uint32_t *)(p + i)) * 16777619u;
    return h ^ n;
}

static uint32_t xb_rs(int i) { return MEM32(XB_RENDER_STATE + 4 * i); }
static uint32_t xb_tss(int stage, int i) { return MEM32(XB_TEXTURE_STATE + 0x80 * stage + 4 * i); }
static uint32_t xb_device(void) { return MEM32(XB_DEVICE_PTR); }

/* XDK 5849 D3DRENDERSTATETYPE: the index into D3D_g_RenderState. The block
 * boundaries are XbSymbolDatabase's (deferred 92, complex 136); the complex
 * slots were checked against the setter that writes each one. */
enum {
    XRS_PSALPHAINPUTS0 = 0, XRS_PSFINALCOMBINERINPUTSABCD = 8,
    XRS_PSFINALCOMBINERINPUTSEFG = 9, XRS_PSCONSTANT0_0 = 10,
    XRS_PSCONSTANT1_0 = 18, XRS_PSALPHAOUTPUTS0 = 26, XRS_PSRGBINPUTS0 = 34,
    XRS_PSCOMPAREMODE = 42, XRS_PSFINALCOMBINERCONSTANT0 = 43,
    XRS_PSFINALCOMBINERCONSTANT1 = 44, XRS_PSRGBOUTPUTS0 = 45,
    XRS_PSCOMBINERCOUNT = 53, XRS_PSDOTMAPPING = 55, XRS_PSINPUTTEXTURE = 56,
    XRS_ZFUNC = 57, XRS_ALPHAFUNC, XRS_ALPHABLENDENABLE, XRS_ALPHATESTENABLE,
    XRS_ALPHAREF, XRS_SRCBLEND, XRS_DESTBLEND, XRS_ZWRITEENABLE,
    XRS_DITHERENABLE, XRS_SHADEMODE, XRS_COLORWRITEENABLE, XRS_STENCILZFAIL,
    XRS_STENCILPASS, XRS_STENCILFUNC, XRS_STENCILREF, XRS_STENCILMASK,
    XRS_STENCILWRITEMASK, XRS_BLENDOP, XRS_BLENDCOLOR, XRS_SWATHWIDTH,
    XRS_POLYGONOFFSETZSLOPESCALE, XRS_POLYGONOFFSETZOFFSET,
    XRS_POINTOFFSETENABLE, XRS_WIREFRAMEOFFSETENABLE, XRS_SOLIDOFFSETENABLE,
    XRS_FOGENABLE = 92, XRS_FOGTABLEMODE, XRS_FOGSTART, XRS_FOGEND,
    XRS_FOGDENSITY, XRS_RANGEFOGENABLE, XRS_WRAP0, XRS_WRAP1, XRS_WRAP2,
    XRS_WRAP3, XRS_LIGHTING, XRS_SPECULARENABLE, XRS_LOCALVIEWER,
    XRS_COLORVERTEX, XRS_BACKSPECULARMATERIALSOURCE,
    XRS_BACKDIFFUSEMATERIALSOURCE, XRS_BACKAMBIENTMATERIALSOURCE,
    XRS_BACKEMISSIVEMATERIALSOURCE, XRS_SPECULARMATERIALSOURCE,
    XRS_DIFFUSEMATERIALSOURCE, XRS_AMBIENTMATERIALSOURCE,
    XRS_EMISSIVEMATERIALSOURCE, XRS_BACKAMBIENT, XRS_AMBIENT, XRS_POINTSIZE,
    XRS_POINTSIZE_MIN, XRS_POINTSPRITEENABLE, XRS_POINTSCALEENABLE,
    XRS_POINTSCALE_A, XRS_POINTSCALE_B, XRS_POINTSCALE_C, XRS_POINTSIZE_MAX,
    XRS_PSTEXTUREMODES = 136, XRS_VERTEXBLEND, XRS_FOGCOLOR, XRS_FILLMODE,
    XRS_BACKFILLMODE, XRS_TWOSIDEDLIGHTING, XRS_NORMALIZENORMALS, XRS_ZENABLE,
    XRS_STENCILENABLE, XRS_STENCILFAIL, XRS_FRONTFACE, XRS_CULLMODE,
    XRS_TEXTUREFACTOR, XRS_ZBIAS
};

/* XDK 5849 D3DTEXTURESTAGESTATETYPE: the index into a stage's 32 dwords. */
enum {
    XTSS_ADDRESSU = 0, XTSS_ADDRESSV, XTSS_ADDRESSW, XTSS_MAGFILTER,
    XTSS_MINFILTER, XTSS_MIPFILTER, XTSS_MIPMAPLODBIAS, XTSS_MAXMIPLEVEL,
    XTSS_MAXANISOTROPY, XTSS_COLORKEYOP, XTSS_COLORSIGN, XTSS_ALPHAKILL,
    XTSS_COLOROP, XTSS_COLORARG0, XTSS_COLORARG1, XTSS_COLORARG2,
    XTSS_ALPHAOP, XTSS_ALPHAARG0, XTSS_ALPHAARG1, XTSS_ALPHAARG2,
    XTSS_RESULTARG, XTSS_TEXTURETRANSFORMFLAGS, XTSS_BUMPENVMAT00,
    XTSS_BUMPENVMAT01, XTSS_BUMPENVMAT11, XTSS_BUMPENVMAT10,
    XTSS_BUMPENVLSCALE, XTSS_BUMPENVLOFFSET, XTSS_TEXCOORDINDEX,
    XTSS_BORDERCOLOR
};

static void surf_dims(uint32_t res, uint32_t *w, uint32_t *h, uint32_t *pitch)
{
    uint32_t f = MEM32(res + 0xC), sz = MEM32(res + 0x10);
    if (sz) {
        *w = (sz & 0xFFF) + 1;
        *h = ((sz >> 12) & 0xFFF) + 1;
        *pitch = (((sz >> 24) & 0xFF) + 1) * 64;
    } else {
        *w = 1u << ((f >> 20) & 0xF);
        *h = 1u << ((f >> 24) & 0xF);
        *pitch = 0;
    }
}

/* A declared register: where it comes from and how it is stored. */
typedef struct { int reg, stream; uint32_t offset, type; } VsInput;

static uint32_t vsdt_size(uint32_t t)
{
    switch (t) {
    case 0x12: return 4;  case 0x22: return 8;  case 0x32: return 12; case 0x42: return 16;
    case 0x40: return 4;  case 0x15: return 2;  case 0x25: return 4;  case 0x35: return 6;
    case 0x45: return 8;  case 0x11: return 2;  case 0x21: return 4;  case 0x31: return 6;
    case 0x41: return 8;  case 0x16: return 4;  case 0x14: return 1;  case 0x24: return 2;
    case 0x34: return 3;  case 0x44: return 4;  case 0x72: return 12;
    }
    return 0;                                         /* NONE */
}

static int vs_inputs(const uint32_t *decl, VsInput *in)
{
    int n = 0, stream = 0, i;
    uint32_t off[16] = { 0 };
    for (i = 0; i < 96 && decl[i] != 0xFFFFFFFFu; i++) {
        uint32_t tok = decl[i], kind = tok >> 29;
        if (kind == 1) {                                  /* STREAM */
            stream = tok & 0xF;
        } else if (kind == 2 && !(tok & 0x10000000)) {    /* STREAMDATA reg */
            uint32_t type = (tok >> 16) & 0xFF;
            if (n < 16 && type != 0x02) {
                in[n].reg = tok & 0x1F; in[n].stream = stream;
                in[n].offset = off[stream]; in[n].type = type;
                n++;
            }
            off[stream] += vsdt_size(type);
        } else if (kind == 2) {                           /* SKIP dwords / bytes */
            off[stream] += (tok & 0x08000000) ? (tok >> 16) & 0xFF : ((tok >> 16) & 0xF) * 4;
        } else if (kind == 4) {                           /* CONSTMEM: data follows */
            i += ((tok >> 25) & 0xF) * 4;
        }
    }
    return n;
}

static void vsdt_read(uint32_t type, const uint8_t *p, float *o)
{
    int k;
    o[0] = o[1] = o[2] = 0.0f; o[3] = 1.0f;
    switch (type) {
    case 0x12: case 0x22: case 0x32: case 0x42:
        memcpy(o, p, (type >> 4) * 4); break;
    case 0x72:                                        /* FLOAT2H: x, y, w */
        memcpy(o, p, 8); memcpy(&o[3], p + 8, 4); break;
    case 0x40: {                                      /* D3DCOLOR: ARGB -> RGBA */
        uint32_t c = *(const uint32_t *)p;
        o[0] = ((c >> 16) & 0xFF) / 255.0f; o[1] = ((c >> 8) & 0xFF) / 255.0f;
        o[2] = (c & 0xFF) / 255.0f; o[3] = (c >> 24) / 255.0f; break; }
    case 0x15: case 0x25: case 0x35: case 0x45:
        for (k = 0; k < (int)(type >> 4); k++) o[k] = (float)((const int16_t *)p)[k]; break;
    case 0x11: case 0x21: case 0x31: case 0x41:
        for (k = 0; k < (int)(type >> 4); k++) o[k] = ((const int16_t *)p)[k] / 32767.0f; break;
    case 0x14: case 0x24: case 0x34: case 0x44:
        for (k = 0; k < (int)(type >> 4); k++) o[k] = p[k] / 255.0f; break;
    case 0x16: {                                      /* NORMPACKED3: 11:11:10 signed */
        uint32_t v = *(const uint32_t *)p;
        o[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        o[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        o[2] = (float)((int32_t)v >> 22) / 511.0f; break; }
    }
}

/* Swap one piece of text for another throughout a NUL-terminated buffer. */
static void str_replace(char *buf, size_t cap, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to);
    char *p = buf;
    while ((p = strstr(p, from)) != NULL) {
        size_t rest = strlen(p + fl);
        if ((size_t)(p - buf) + tl + rest + 1 > cap)
            return;
        memmove(p + tl, p + fl, rest + 1);
        memcpy(p, to, tl);
        p += tl;
    }
}

/* NV2A microcode to HLSL, instruction for instruction. The decoding and the
 * semantics are the toolkit's CPU interpreter (src/kernel/nv2a_vsh_interp.c),
 * which the pushbuffer path drew every 3D scene with; this emits what it
 * executes. */

static uint32_t vfield(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

typedef struct { char *buf; size_t cap, len; } Sb;

static void sb(Sb *s, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (s->len >= s->cap)
        return;
    va_start(ap, fmt);
    n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        s->len += (size_t)n < s->cap - s->len ? (size_t)n : s->cap - s->len - 1;
}

/* Source operand `which` (A 0, B 1, C 2) of one instruction. */
static void emit_src(Sb *s, const uint32_t *ins, int which, uint16_t *inputs, int *uses_vp)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg, ci = vfield(ins, 1, 13, 8), rel = vfield(ins, 3, 1, 1);

    if (which == 0) {
        neg = vfield(ins, 1, 8, 1); swz = vfield(ins, 1, 0, 8);
        mux = vfield(ins, 2, 26, 2); reg = vfield(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = vfield(ins, 2, 25, 1); swz = vfield(ins, 2, 17, 8);
        mux = vfield(ins, 2, 11, 2); reg = vfield(ins, 2, 13, 4);
    } else {
        neg = vfield(ins, 2, 10, 1); swz = vfield(ins, 2, 2, 8);
        mux = vfield(ins, 3, 28, 2);
        reg = (vfield(ins, 2, 0, 2) << 2) | vfield(ins, 3, 30, 2);
    }
    sb(s, neg ? "-" : "");
    if (mux == 1) {
        sb(s, "R%u", reg <= 12 ? reg : 0);
    } else if (mux == 2) {
        uint32_t in = vfield(ins, 1, 9, 4);
        *inputs |= (uint16_t)(1u << in);
        sb(s, "v%u", in);
    } else if (rel) {
        sb(s, "c[a0 + %u]", ci);
    } else {
        if (ci == 58 || ci == 59)
            *uses_vp = 1;
        sb(s, "c[%u]", ci);
    }
    sb(s, ".%c%c%c%c", sw[(swz >> 6) & 3], sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
}

static void emit_mask(Sb *s, uint32_t m)
{
    sb(s, ".%s%s%s%s", (m & 8) ? "x" : "", (m & 4) ? "y" : "", (m & 2) ? "z" : "", (m & 1) ? "w" : "");
}

/* The instruction blocks of `n` instructions, in HLSL or (glsl) GLSL ES 3.00:
 * each reads its A, B and C sources and writes R0-R12, a0 and the o*
 * outputs, which the caller declares. Sets *inputs to the registers read and
 * *uses_vp when the program applies D3D's viewport (c[58], c[59]). */
static void vsh_body(const uint32_t *mc, int n, Sb *s, uint16_t *inputs, int *uses_vp, int glsl)
{
    static const char *const mac_hlsl[14] = {
        NULL, "A", "A * B", "A + C", "A * B + C", "dot(A.xyz, B.xyz).xxxx",
        "(dot(A.xyz, B.xyz) + B.w).xxxx", "dot(A, B).xxxx",
        "float4(1.0, A.y * B.y, A.z, B.w)", "min(A, B)", "max(A, B)",
        "float4(A < B)", "float4(A >= B)", NULL,
    };
    static const char *const ilu_hlsl[8] = {
        NULL, "C", "(1.0 / C.x).xxxx", "rcc(C.x).xxxx", "rsqrt(abs(C.x)).xxxx",
        "float4(exp2(floor(C.x)), C.x - floor(C.x), exp2(C.x), 1.0)",
        "nv_log(C.x)", "nv_lit(C)",
    };
    static const char *const mac_glsl[14] = {
        NULL, "A", "A * B", "A + C", "A * B + C", "vec4(dot(A.xyz, B.xyz))",
        "vec4(dot(A.xyz, B.xyz) + B.w)", "vec4(dot(A, B))",
        "vec4(1.0, A.y * B.y, A.z, B.w)", "min(A, B)", "max(A, B)",
        "vec4(lessThan(A, B))", "vec4(greaterThanEqual(A, B))", NULL,
    };
    static const char *const ilu_glsl[8] = {
        NULL, "C", "vec4(1.0 / C.x)", "vec4(rcc(C.x))", "vec4(inversesqrt(abs(C.x)))",
        "vec4(exp2(floor(C.x)), C.x - floor(C.x), exp2(C.x), 1.0)",
        "nv_log(C.x)", "nv_lit(C)",
    };
    const char *const *mac_expr = glsl ? mac_glsl : mac_hlsl;
    const char *const *ilu_expr = glsl ? ilu_glsl : ilu_hlsl;
    const char *v4 = glsl ? "vec4" : "float4", *zero = glsl ? "vec4(0.0)" : "0";
    int i;

    *inputs = 0;
    *uses_vp = 0;
    for (i = 0; i < n; i++) {
        const uint32_t *ins = mc + 4 * i;
        uint32_t mac = vfield(ins, 1, 21, 4), ilu = vfield(ins, 1, 25, 3);
        uint32_t mac_mask = vfield(ins, 3, 24, 4), ilu_mask = vfield(ins, 3, 16, 4);
        uint32_t tdst = vfield(ins, 3, 20, 4), omask = vfield(ins, 3, 12, 4);
        uint32_t oaddr = vfield(ins, 3, 3, 8), out_ilu = vfield(ins, 3, 2, 1);
        uint32_t out_reg = vfield(ins, 3, 11, 1);
        static const char *const oname[13] = {
            "R12", NULL, NULL, "oD0", "oD1", "oFog", NULL, NULL, NULL, "oT0", "oT1", "oT2", "oT3",
        };

        sb(s, "  { %s A = ", v4); emit_src(s, ins, 0, inputs, uses_vp);
        sb(s, "; %s B = ", v4); emit_src(s, ins, 1, inputs, uses_vp);
        sb(s, "; %s C = ", v4); emit_src(s, ins, 2, inputs, uses_vp);
        sb(s, ";\n    %s M = %s; %s I = %s;\n", v4,
           mac && mac < 13 && mac_expr[mac] ? mac_expr[mac] : zero, v4,
           ilu && ilu_expr[ilu] ? ilu_expr[ilu] : zero);
        if (mac == 13)
            sb(s, glsl ? "    a0 = int(floor(A.x + 0.001));\n" : "    a0 = (int)floor(A.x + 0.001);\n");
        if (mac && mac != 13 && mac_mask && tdst <= 12) {
            sb(s, "    R%u", tdst); emit_mask(s, mac_mask);
            sb(s, " = M"); emit_mask(s, mac_mask); sb(s, ";\n");
        }
        if (ilu && ilu_mask) {
            /* Beside a MAC op the ILU can only write R1. */
            uint32_t it = mac ? 1u : tdst;
            if (it <= 12) {
                sb(s, "    R%u", it); emit_mask(s, ilu_mask);
                sb(s, " = I"); emit_mask(s, ilu_mask); sb(s, ";\n");
            }
        }
        if (omask && out_reg && oaddr < 13 && oname[oaddr]) {
            sb(s, "    %s", oname[oaddr]); emit_mask(s, omask);
            sb(s, " = %s", out_ilu ? "I" : "M"); emit_mask(s, omask); sb(s, ";\n");
        }
        sb(s, "  }\n");
        if (vfield(ins, 3, 0, 1))
            break;
    }
}

#endif

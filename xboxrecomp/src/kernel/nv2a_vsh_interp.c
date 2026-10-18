/**
 * NV2A vertex program interpreter. See nv2a_vsh_interp.h for why it exists.
 *
 * Encoding. An instruction is four dwords; dword 0 is unused. Fields, as
 * (dword, lowest bit, width):
 *
 *   ILU op        1 25 3      MAC op        1 21 4
 *   const index   1 13 8      input index   1  9 4   (shared by A, B, C)
 *   A negate      1  8 1      A swizzle     1  0 8   (x y z w, 2 bits each,
 *   A temp        2 28 4      A mux         2 26 2    x highest)
 *   B negate      2 25 1      B swizzle     2 17 8
 *   B temp        2 13 4      B mux         2 11 2
 *   C negate      2 10 1      C swizzle     2  2 8
 *   C temp        2 0 2 (high) : 3 30 2 (low)          C mux 3 28 2
 *   MAC temp mask 3 24 4      temp dest     3 20 4
 *   ILU temp mask 3 16 4      out mask      3 12 4
 *   out to o[]    3 11 1      out address   3  3 8   (0: to c[])
 *   out from ILU  3  2 1      a0.x relative 3  1 1   final 3 0 1
 *
 * Masks are x=8 y=4 z=2 w=1. A source mux of 1 reads a temp, 2 an input,
 * 3 a constant. Temp 12 is oPos. When an instruction carries both a MAC and
 * an ILU op, the ILU's temp write goes to R1 whatever the temp field says,
 * and both units read their sources before either writes.
 *
 * The layout is checked against real programs rather than taken on trust:
 * RECOMP_VSH_DUMP disassembles each program the title uploads, and an Xbox
 * D3D program is recognisable -- DP4s against the matrix constants, ending in
 * the runtime's screen-space epilogue (RCC of R12.w, then a MAD into oPos).
 */
#include "nv2a_vsh_interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_program[NV2A_VSH_SLOTS][4];
static float    s_const[NV2A_VSH_CONSTANTS][4];
static uint32_t s_load_slot, s_load_word, s_start_slot;
static uint32_t s_const_load, s_const_word;
static uint32_t s_cxt_write;
static int      s_const_trace = -1;      /* RECOMP_VSH_DUMP: constant traffic */
static int      s_dec_valid;             /* s_dec matches s_program from s_start_slot */

static int vsh_dump_enabled(void)
{
    if (s_const_trace < 0)
        s_const_trace = getenv("RECOMP_VSH_DUMP") != NULL;
    return s_const_trace;
}

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* ---- uploads ------------------------------------------------------------ */

void nv2a_vsh_set_load_slot(uint32_t slot)
{
    s_load_slot = slot;
    s_load_word = 0;
}

void nv2a_vsh_program_word(uint32_t word)
{
    if (s_load_slot < NV2A_VSH_SLOTS)
        s_program[s_load_slot][s_load_word] = word;
    s_dec_valid = 0;
    if (++s_load_word == 4) {
        s_load_word = 0;
        s_load_slot++;
    }
}

void nv2a_vsh_set_start_slot(uint32_t slot)
{
    if (slot != s_start_slot)
        s_dec_valid = 0;
    s_start_slot = slot;
}
void nv2a_vsh_set_cxt_write(uint32_t enable) { s_cxt_write = enable; }
int nv2a_vsh_writes_constants(void) { return s_cxt_write != 0; }

void nv2a_vsh_set_constant_load(uint32_t index)
{
    s_const_load = index;
    s_const_word = 0;
    if (vsh_dump_enabled())
        fprintf(stderr, "  [VSH] constant load -> c[%u]\n", index);
}

void nv2a_vsh_constant_word(uint32_t word)
{
    if (s_const_load < NV2A_VSH_CONSTANTS)
        memcpy(&s_const[s_const_load][s_const_word], &word, 4);
    else if (vsh_dump_enabled()) {
        static int warned;
        if (warned++ < 8)
            fprintf(stderr, "  [VSH] constant write DROPPED (c[%u] >= %u)\n",
                    s_const_load, NV2A_VSH_CONSTANTS);
    }
    if (vsh_dump_enabled() && s_const_word == 3 && s_const_load >= 64) {
        static int shown;
        if (shown++ < 96)
            fprintf(stderr, "  [VSH] c[%u] = %.6g %.6g %.6g %.6g\n",
                    s_const_load, s_const[s_const_load][0],
                    s_const[s_const_load][1], s_const[s_const_load][2],
                    s_const[s_const_load][3]);
    }
    if (++s_const_word == 4) {
        s_const_word = 0;
        s_const_load++;
    }
}

void nv2a_vsh_set_constant(uint32_t index, const float v[4])
{
    if (index < NV2A_VSH_CONSTANTS)
        memcpy(s_const[index], v, sizeof s_const[index]);
}

void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4])
{
    if (slot < NV2A_VSH_SLOTS)
        memcpy(s_program[slot], words, sizeof s_program[slot]);
    s_dec_valid = 0;
}

/* ---- disassembly (RECOMP_VSH_DUMP) --------------------------------------- */

static const char *const k_mac[16] = {
    "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
    "dst", "min", "max", "slt", "sge", "arl", "mac14", "mac15"
};
static const char *const k_ilu[8] = {
    "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
};

static void dis_src(char *b, size_t n, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(b, n, "%sR%u", neg ? "-" : "", reg);
    else if (mux == 2)
        snprintf(b, n, "%sv%u", neg ? "-" : "", field(ins, 1, 9, 4));
    else
        snprintf(b, n, "%sc[%s%u]", neg ? "-" : "",
                 field(ins, 3, 1, 1) ? "a0.x+" : "", field(ins, 1, 13, 8));
    {
        size_t l = strlen(b);
        if (l + 6 < n)
            snprintf(b + l, n - l, ".%c%c%c%c", sw[(swz >> 6) & 3],
                     sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
    }
}

static void dump_program(uint32_t start)
{
    uint32_t s;
    fprintf(stderr, "  [VSH] program at slot %u:\n", start);
    for (s = start; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        char a[40], bb[40], c[40];
        dis_src(a, sizeof a, ins, 0);
        dis_src(bb, sizeof bb, ins, 1);
        dis_src(c, sizeof c, ins, 2);
        fprintf(stderr, "  [VSH] %3u %-4s %-4s A=%s B=%s C=%s  R%u mac%X ilu%X"
                " out%s%u m%X%s%s\n", s,
                k_mac[field(ins, 1, 21, 4)], k_ilu[field(ins, 1, 25, 3)],
                a, bb, c, field(ins, 3, 20, 4), field(ins, 3, 24, 4),
                field(ins, 3, 16, 4), field(ins, 3, 11, 1) ? "o" : "c",
                field(ins, 3, 3, 8), field(ins, 3, 12, 4),
                field(ins, 3, 2, 1) ? " ilu" : " mac",
                field(ins, 3, 0, 1) ? " FINAL" : "");
        if (field(ins, 3, 0, 1))
            break;
    }
    fflush(stderr);
}

const float *nv2a_vsh_constant(uint32_t index)
{
    return s_const[index < NV2A_VSH_CONSTANTS ? index : 0];
}

void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word)
{
    if (index < NV2A_VSH_CONSTANTS && comp < 4)
        memcpy(&s_const[index][comp], &word, 4);
}

/* ---- execution ----------------------------------------------------------- */

typedef struct { float v[4]; } vec4;

/* An instruction with its bit fields pulled out once per program rather than
 * once per vertex: the decode was most of the interpreter's time, and the
 * interpreter was the executor's biggest cost in a 3D scene. */
typedef struct {
    uint8_t mux, reg, neg, rel;
    uint8_t sw[4];
    uint16_t ci;
} DecSrc;

typedef struct {
    uint8_t mac, ilu, mac_mask, ilu_mask, tdst, omask, oaddr;
    uint8_t out_ilu, out_reg, final;
    uint8_t in;                       /* the one input register any source reads */
    DecSrc src[3];
} DecIns;

static DecIns s_dec[NV2A_VSH_SLOTS];

static void decode_src(const uint32_t *ins, int which, DecSrc *d)
{
    uint32_t swz;

    if (which == 0) {
        d->neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        d->mux = field(ins, 2, 26, 2); d->reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        d->neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        d->mux = field(ins, 2, 11, 2); d->reg = field(ins, 2, 13, 4);
    } else {
        d->neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        d->mux = field(ins, 3, 28, 2);
        d->reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    d->sw[0] = (swz >> 6) & 3; d->sw[1] = (swz >> 4) & 3;
    d->sw[2] = (swz >> 2) & 3; d->sw[3] = swz & 3;
    d->ci = (uint16_t)field(ins, 1, 13, 8);
    d->rel = field(ins, 3, 1, 1);
}

static void decode_program(void)
{
    uint32_t s;

    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        DecIns *d = &s_dec[s];
        int k;

        d->mac = field(ins, 1, 21, 4); d->ilu = field(ins, 1, 25, 3);
        d->mac_mask = field(ins, 3, 24, 4); d->ilu_mask = field(ins, 3, 16, 4);
        d->tdst = field(ins, 3, 20, 4);
        d->omask = field(ins, 3, 12, 4); d->oaddr = field(ins, 3, 3, 8);
        d->out_ilu = field(ins, 3, 2, 1); d->out_reg = field(ins, 3, 11, 1);
        d->final = field(ins, 3, 0, 1);
        d->in = field(ins, 1, 9, 4);
        for (k = 0; k < 3; k++)
            decode_src(ins, k, &d->src[k]);
        if (d->final)
            break;
    }
    s_dec_valid = 1;
}

static vec4 read_src(const DecSrc *d, uint32_t in_reg,
                     const float in[NV2A_VSH_INPUTS][4],
                     const vec4 *temp, const vec4 *opos, int a0)
{
    const float *s;
    vec4 r;

    if (d->mux == 1)
        s = (d->reg == 12) ? opos->v : (d->reg < 12 ? temp[d->reg].v : temp[0].v);
    else if (d->mux == 2)
        s = in[in_reg];
    else {
        int ci = (int)d->ci;
        if (d->rel)
            ci += a0;
        if (ci < 0 || ci >= NV2A_VSH_CONSTANTS)
            ci = 0;
        s = s_const[ci];
    }
    r.v[0] = s[d->sw[0]];
    r.v[1] = s[d->sw[1]];
    r.v[2] = s[d->sw[2]];
    r.v[3] = s[d->sw[3]];
    if (d->neg) {
        r.v[0] = -r.v[0]; r.v[1] = -r.v[1];
        r.v[2] = -r.v[2]; r.v[3] = -r.v[3];
    }
    return r;
}

static void write_masked(float *dst, const vec4 *src, uint32_t mask)
{
    if (mask & 8) dst[0] = src->v[0];
    if (mask & 4) dst[1] = src->v[1];
    if (mask & 2) dst[2] = src->v[2];
    if (mask & 1) dst[3] = src->v[3];
}

static vec4 splat(float f)
{
    vec4 r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = f;
    return r;
}

static vec4 run_mac(uint32_t op, vec4 a, vec4 b, vec4 c, int *a0)
{
    vec4 r = a;
    int i;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
    case 3: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + c.v[i]; break;
    case 4: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + c.v[i]; break;
    case 5: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]); break;
    case 6: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + b.v[3]); break;                        /* dph */
    case 7: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + a.v[3]*b.v[3]); break;
    case 8: r.v[0] = 1.0f; r.v[1] = a.v[1] * b.v[1];            /* dst */
            r.v[2] = a.v[2]; r.v[3] = b.v[3]; break;
    case 9: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
    case 10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? a.v[i] : b.v[i]; break;
    case 11: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; break;
    case 12: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
    case 13: *a0 = (int)floorf(a.v[0] + 0.001f); break;         /* arl */
    default: break;
    }
    return r;
}

static vec4 run_ilu(uint32_t op, vec4 c)
{
    float x = c.v[0];
    vec4 r = c;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: r = splat(x == 0.0f ? INFINITY : 1.0f / x); break;  /* rcp */
    case 3: {                                                  /* rcc */
        float f = 1.0f / x;
        if (fabsf(f) < 5.42101e-20f) f = f < 0 ? -5.42101e-20f : 5.42101e-20f;
        if (fabsf(f) > 1.884467e19f) f = f < 0 ? -1.884467e19f : 1.884467e19f;
        r = splat(f);
        break;
    }
    case 4: r = splat(1.0f / sqrtf(fabsf(x))); break;           /* rsq */
    case 5: {                                                  /* exp */
        float fl = floorf(x);
        r.v[0] = exp2f(fl); r.v[1] = x - fl; r.v[2] = exp2f(x); r.v[3] = 1.0f;
        break;
    }
    case 6: {                                                  /* log */
        float ax = fabsf(x);
        if (ax == 0.0f) {
            r.v[0] = r.v[2] = -INFINITY; r.v[1] = 1.0f;
        } else {
            float e = floorf(log2f(ax));
            r.v[0] = e; r.v[1] = ax / exp2f(e); r.v[2] = log2f(ax);
        }
        r.v[3] = 1.0f;
        break;
    }
    case 7: {                                                  /* lit */
        float nl = c.v[0] > 0.0f ? c.v[0] : 0.0f;
        float nh = c.v[1] > 0.0f ? c.v[1] : 0.0f;
        float p = c.v[3] < -127.9961f ? -127.9961f
                : (c.v[3] > 127.9961f ? 127.9961f : c.v[3]);
        r.v[0] = 1.0f; r.v[1] = nl;
        r.v[2] = c.v[0] > 0.0f ? powf(nh, p) : 0.0f; r.v[3] = 1.0f;
        break;
    }
    default: break;
    }
    return r;
}

int nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    static int dump = -1;
    static uint32_t dumped[16];
    static int ndumped;
    vec4 temp[12], opos;
    float outregs[13][4];
    int a0 = 0;
    uint32_t s;

    if (dump < 0)
        dump = getenv("RECOMP_VSH_DUMP") != NULL;
    if (dump && ndumped < 16) {
        /* By content: titles reload different programs into the same slot. */
        uint32_t h = 2166136261u, t;
        int i, seen = 0;
        for (t = s_start_slot; t < NV2A_VSH_SLOTS; t++) {
            for (i = 0; i < 4; i++)
                h = (h ^ s_program[t][i]) * 16777619u;
            if (field(s_program[t], 3, 0, 1))
                break;
        }
        for (i = 0; i < ndumped; i++)
            if (dumped[i] == h) seen = 1;
        if (!seen) {
            dumped[ndumped++] = h;
            dump_program(s_start_slot);
        }
    }

    memset(temp, 0, sizeof temp);
    memset(&opos, 0, sizeof opos);
    memset(outregs, 0, sizeof outregs);
    outregs[3][3] = outregs[4][3] = 1.0f;      /* colours default opaque */
    /* Unwritten texture coordinates are (0,0,0,1), as xemu initialises them:
     * a projective stage divides by q, and q = 0 would put every texel of an
     * unwritten stage at infinity. */
    outregs[9][3] = outregs[10][3] = outregs[11][3] = outregs[12][3] = 1.0f;

    if (!s_dec_valid)
        decode_program();
    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const DecIns *ins = &s_dec[s];
        uint32_t mac = ins->mac, ilu = ins->ilu;
        uint32_t mac_mask = ins->mac_mask, ilu_mask = ins->ilu_mask;
        uint32_t tdst = ins->tdst;
        uint32_t omask = ins->omask, oaddr = ins->oaddr;
        vec4 a = read_src(&ins->src[0], ins->in, in, temp, &opos, a0);
        vec4 b = read_src(&ins->src[1], ins->in, in, temp, &opos, a0);
        vec4 c = read_src(&ins->src[2], ins->in, in, temp, &opos, a0);
        vec4 mres = {{0, 0, 0, 0}}, ires = {{0, 0, 0, 0}};

        if (mac)
            mres = run_mac(mac, a, b, c, &a0);
        if (ilu)
            ires = run_ilu(ilu, c);

        if (mac && mac != 13 && mac_mask) {
            float *d = tdst == 12 ? opos.v : (tdst < 12 ? temp[tdst].v : NULL);
            if (d) write_masked(d, &mres, mac_mask);
        }
        if (ilu && ilu_mask) {
            /* Paired with any MAC op, the ILU can only write R1 -- even when
             * the MAC half writes no temp at all. The D3D epilogue is exactly
             * that: `mul o[0].xyz` beside `rcc R1.x`, then `mad` with R1.x.
             * Sending the rcc to the temp field (R7 there) left R1.x at 0 and
             * put every vertex of every 3D batch at c[59]. */
            uint32_t it = mac ? 1u : tdst;
            float *d = it == 12 ? opos.v : (it < 12 ? temp[it].v : NULL);
            if (d) write_masked(d, &ires, ilu_mask);
        }
        if (omask) {
            const vec4 *src = ins->out_ilu ? &ires : &mres;
            if (!ins->out_reg) {                          /* to c[] */
                if (s_cxt_write && oaddr < NV2A_VSH_CONSTANTS)
                    write_masked(s_const[oaddr], src, omask);
            } else if (oaddr == 0) {
                write_masked(opos.v, src, omask);
            } else if (oaddr < 13) {
                write_masked(outregs[oaddr], src, omask);
            }
        }
        if (ins->final) {
            memcpy(out->pos, opos.v, sizeof out->pos);
            memcpy(out->d0, outregs[3], sizeof out->d0);
            memcpy(out->d1, outregs[4], sizeof out->d1);
            memcpy(out->fog, outregs[5], sizeof out->fog);
            memcpy(out->tex[0], outregs[9], sizeof out->tex[0]);
            memcpy(out->tex[1], outregs[10], sizeof out->tex[1]);
            memcpy(out->tex[2], outregs[11], sizeof out->tex[2]);
            memcpy(out->tex[3], outregs[12], sizeof out->tex[3]);
            return 1;
        }
    }
    return 0;
}

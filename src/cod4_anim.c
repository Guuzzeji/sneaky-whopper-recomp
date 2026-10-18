/* CoD4 player animations retargeted onto the King (shooter mode).
 *
 * tools/cod4_anim.py writes game/mods/cod4/anims.bin from the user's own
 * CoD4 install; this samples and blends its clips into King poses. It knows
 * nothing about the guest: recomp_manual.c chooses the clips and writes the
 * pose into the King's bones. Quaternions are x, y, z, w in the engine's
 * convention, so a pose can be written to the bones as is. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cod4_anim.h"

typedef struct {
    char name[48];
    unsigned frames;                  /* keys; the last equals the first for loops */
    float fps;
    unsigned flags;
    float speed[2];                   /* root motion, King units/s: x, z */
    unsigned char mask[88];
    const float *data;
} Clip;

static Clip *s_clips;
static int s_nclips;
/* Each file loaded once; their clips go into one list (CoD4's and Skate 3's). */
static struct { char path[96]; int clips; } s_files[4];
static int s_nfiles;

#define FRAME_FLOATS (CANIM_BONES * 4 + 3 + 4 + 3)

/* Loads a file's clips (once) and returns how many it holds, 0 if missing. */
int canim_load(const char *path)
{
    FILE *f;
    long n;
    unsigned i, count, bones, off;
    unsigned char *file;
    const unsigned char *p;
    Clip *grown;
    int k;

    for (k = 0; k < s_nfiles; k++)
        if (!strcmp(s_files[k].path, path))
            return s_files[k].clips;
    if (s_nfiles == 4 || !(f = fopen(path, "rb")))
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = (unsigned char *)malloc((size_t)n);
    if (!file || fread(file, 1, (size_t)n, f) != (size_t)n || n < 12 || memcmp(file, "SKA1", 4)) {
        fclose(f);
        free(file);
        return 0;
    }
    fclose(f);
    memcpy(&count, file + 4, 4);
    memcpy(&bones, file + 8, 4);
    if (bones != CANIM_BONES || !(grown = (Clip *)realloc(s_clips, (s_nclips + count) * sizeof *s_clips))) {
        free(file);
        return 0;
    }
    s_clips = grown;
    for (i = 0, p = file + 12; i < count; i++, p += 160) {
        Clip *c = &s_clips[s_nclips + i];
        memset(c, 0, sizeof *c);
        memcpy(c->name, p, 48);
        c->name[47] = 0;
        memcpy(&c->frames, p + 48, 4);
        memcpy(&c->fps, p + 52, 4);
        memcpy(&c->flags, p + 56, 4);
        memcpy(c->speed, p + 60, 8);
        memcpy(c->mask, p + 68, 88);
        memcpy(&off, p + 156, 4);
        if (off + (size_t)c->frames * FRAME_FLOATS * 4 > (size_t)n || !c->frames)
            break;
        c->data = (const float *)(file + off);
    }
    s_nclips += (int)i;
    snprintf(s_files[s_nfiles].path, sizeof s_files[s_nfiles].path, "%s", path);
    s_files[s_nfiles++].clips = (int)i;
    fprintf(stderr, "[CANIM] %u clips from %s\n", i, path);
    return (int)i;
}

int canim_find(const char *name)
{
    int i;
    for (i = 0; i < s_nclips; i++)
        if (!strcmp(s_clips[i].name, name))
            return i;
    return -1;
}

float canim_length(int clip)
{
    const Clip *c = &s_clips[clip];
    return c->frames > 1 ? (c->frames - 1) / c->fps : 0.0f;
}

int canim_flags(int clip) { return (int)s_clips[clip].flags; }

float canim_speed(int clip)
{
    return sqrtf(s_clips[clip].speed[0] * s_clips[clip].speed[0]
                 + s_clips[clip].speed[1] * s_clips[clip].speed[1]);
}

static void qnorm(float q[4])
{
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    int i;
    if (n < 1e-6f) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    for (i = 0; i < 4; i++)
        q[i] /= n;
}

/* a += b * w, with b flipped into a's hemisphere */
static void qacc(float a[4], const float b[4], float w)
{
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    int i;
    if (d < 0.0f)
        w = -w;
    for (i = 0; i < 4; i++)
        a[i] += b[i] * w;
}

/* Normalised lerp from a to b by t, in place. */
static void qnlerp(float a[4], const float b[4], float t)
{
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0.0f ? -t : t;
    int i;
    for (i = 0; i < 4; i++)
        a[i] = a[i] * (1.0f - t) + b[i] * s;
    qnorm(a);
}

void canim_sample(int clip, float t, int loop, KingPose *out)
{
    const Clip *c = &s_clips[clip];
    float len = canim_length(clip), f, a;
    const float *k0, *k1;
    unsigned i0, i1;
    int b, i;

    if (len <= 0.0f) {
        t = 0.0f;
    } else if (loop) {
        t = fmodf(t, len);
        if (t < 0.0f)
            t += len;
    } else {
        t = t < 0.0f ? 0.0f : t > len ? len : t;
    }
    f = t * c->fps;
    i0 = (unsigned)f;
    if (i0 >= c->frames - 1)
        i0 = c->frames > 1 ? c->frames - 2 : 0;
    i1 = c->frames > 1 ? i0 + 1 : 0;
    a = f - (float)i0;
    a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
    k0 = c->data + (size_t)i0 * FRAME_FLOATS;
    k1 = c->data + (size_t)i1 * FRAME_FLOATS;
    for (b = 0; b < CANIM_BONES; b++) {
        out->has[b] = c->mask[b];
        if (!c->mask[b])
            continue;
        memcpy(out->q[b], k0 + 4 * b, 16);
        qnlerp(out->q[b], k1 + 4 * b, a);
    }
    k0 += CANIM_BONES * 4;
    k1 += CANIM_BONES * 4;
    for (i = 0; i < 3; i++)
        out->origin[i] = k0[i] + (k1[i] - k0[i]) * a;
    memcpy(out->grip_q, k0 + 3, 16);
    qnlerp(out->grip_q, k1 + 3, a);
    for (i = 0; i < 3; i++)
        out->grip_p[i] = k0[7 + i] + (k1[7 + i] - k0[7 + i]) * a;
}

void canim_clear(KingPose *p)
{
    memset(p, 0, sizeof *p);
}

void canim_add(KingPose *acc, const KingPose *p, float w)
{
    int b, i;
    if (w <= 0.0f)
        return;
    for (b = 0; b < CANIM_BONES; b++) {
        if (!p->has[b])
            continue;
        if (!acc->has[b]) {
            memset(acc->q[b], 0, 16);
            acc->has[b] = 1;
        }
        qacc(acc->q[b], p->q[b], w);
    }
    for (i = 0; i < 3; i++)
        acc->origin[i] += p->origin[i] * w;
    qacc(acc->grip_q, p->grip_q, w);
    for (i = 0; i < 3; i++)
        acc->grip_p[i] += p->grip_p[i] * w;
    acc->weight += w;
}

void canim_finish(KingPose *p)
{
    int b, i;
    for (b = 0; b < CANIM_BONES; b++)
        if (p->has[b])
            qnorm(p->q[b]);
    qnorm(p->grip_q);
    if (p->weight > 0.0f)
        for (i = 0; i < 3; i++) {
            p->origin[i] /= p->weight;
            p->grip_p[i] /= p->weight;
        }
    p->weight = 1.0f;
}

void canim_mix(KingPose *base, const KingPose *top, float w, int grip)
{
    int b, i;
    if (w <= 0.0f)
        return;
    if (w > 1.0f)
        w = 1.0f;
    for (b = 0; b < CANIM_BONES; b++) {
        if (!top->has[b])
            continue;
        if (!base->has[b]) {
            memcpy(base->q[b], top->q[b], 16);
            base->has[b] = 1;
        } else {
            qnlerp(base->q[b], top->q[b], w);
        }
    }
    if (grip) {
        qnlerp(base->grip_q, top->grip_q, w);
        for (i = 0; i < 3; i++)
            base->grip_p[i] += (top->grip_p[i] - base->grip_p[i]) * w;
    }
}

void canim_mix_origin(KingPose *base, const KingPose *top, float w)
{
    int i;
    for (i = 0; i < 3; i++)
        base->origin[i] += (top->origin[i] - base->origin[i]) * w;
}

/* Skate mode (Skate 3 style): the King on a skateboard.
 *
 * A cheat (F1 > Cheats). Push with W, brake with S, carve with A / D, G to
 * step off and walk with the board in hand (and back on). Space ollies; in
 * the air a quick flick of the mouse is a trick, its direction picking which
 * (flick_tick, docs/MODS.md). On the ground the mouse looks around. Gesture names and the gesture -> trick mapping follow
 * Skate 3's own state graph (docs/SKATE3_NOTES.md); the numbers are tuned by
 * eye until Skate 3's physics attributes are named.
 *
 * Sneak King side (docs/ROADMAP.md P9):
 *   The King is a kinematic physics actor. [King+0x190] is his controller
 *     block; +0x30 is a velocity (units/s, x z on the ground) the physics
 *     step moves him by, with the game's own collision: he stops at walls
 *     and slides along them, and follows the ground. Nothing else writes it
 *     while he stands in KingIdle, so the board's velocity goes there every
 *     game frame and his own stick is muted (toolkit mkb_set_mute).
 *   His heading, King+0x30C / +0x1A4 (King::Update 0x1CD20), turns him side
 *     on to the board. The ollie lifts his Origin bone before the bone build
 *     (0x151650); the board is drawn by mods_mesh.c at present. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"
#include "overlay.h"
#include "cod4_anim.h"

#define GA_KING   0x2E2EB8u
#define GA_DT     0x32A29Cu
#define GA_CLOCK  0x32A2A4u
#define GA_VIEW   0x30F9F0u
#define GA_PROJ   0x30FBF0u
#define GA_ZOFS   0x30FCA8u

#define PI 3.14159265f

int game_in_play(void);
int game_is_paused(void);
int mkb_take_mouse_delta(long *dx, long *dy);
void mkb_set_mute(int move, unsigned analog_mask);
int mkb_mouse_buttons(int grab, int *clicked);
void *xbox_FramebufferWindowHwnd(void);

/* Tuning, King units (about 4 mm: his walk is 175 a second, his board 32 inches
 * at 5.5 units an inch) and seconds. */
#define PUSH_GAIN     380.0f      /* speed one push adds at rest */
#define PUSH_TOP      2000.0f     /* pushing fades out toward this (~30 km/h) */
#define PUSH_GAP      0.55f       /* seconds between pushes */
#define ROLL_DRAG     18.0f       /* rolling resistance, units/s^2 */
#define AIR_DRAG      0.03f      /* per second, of the speed */
#define BRAKE         750.0f
#define CARVE         2.6f        /* rad/s at a crawl, falling with speed */
#define GRAVITY       2450.0f     /* 9.8 m/s^2 at about 4 mm a unit */
#define POP_V         900.0f      /* an ollie about 70 cm high, 0.73 s in the air */
#define TRICK_TIME    0.5f        /* a flip or shove-it takes this long to come round */
#define HARD_LAND     2300.0f     /* falling faster than this (about 4 m) is a bail */
#define BAIL_TIME     2.4f
#define GETUP_TIME    0.6f        /* the last part of a bail: back on the board */
#define BOARD_UNIT    board_unit() /* King units per inch of board */
#define DECK_TOP      3.6f        /* inches: grip tape above the ground */

/* ponytail: scale tuned by eye; RECOMP_SKATE_BOARD overrides it */
static float board_unit(void)
{
    static float u;
    if (!u)
        u = getenv("RECOMP_SKATE_BOARD") ? (float)atof(getenv("RECOMP_SKATE_BOARD")) : 5.5f;
    return u;
}

enum { Z_C, Z_U, Z_UR, Z_R, Z_DR, Z_D, Z_DL, Z_L, Z_UL };

typedef struct { const char *name; float roll, yaw; int score; } Trick;

/* Skate 3's flick-it set (ActionGraph_OnBoard: the Ollie, Nollie,
 * L_F/L_B Kickflip and Heelflip, L_FS/L_BS Shuvit gestures; varials are a
 * shove-it into a flip). roll: flips about the board's length, yaw: turns. */
enum { T_OLLIE, T_NOLLIE, T_KICKFLIP, T_HEELFLIP, T_BS_SHUV, T_FS_SHUV, T_VARIAL_KICK,
       T_VARIAL_HEEL, T_N_KICKFLIP, T_N_HEELFLIP, T_360FLIP, T_COUNT };
static const Trick s_tricks[T_COUNT] = {
    { "Ollie", 0, 0, 50 },
    { "Nollie", 0, 0, 75 },
    { "Kickflip", 1, 0, 250 },
    { "Heelflip", -1, 0, 250 },
    { "BS Pop Shove-it", 0, 0.5f, 200 },
    { "FS Pop Shove-it", 0, -0.5f, 200 },
    { "Varial Kickflip", 1, 0.5f, 450 },
    { "Varial Heelflip", -1, -0.5f, 450 },
    { "Nollie Kickflip", 1, 0, 400 },
    { "Nollie Heelflip", -1, 0, 400 },
    { "360 Flip", 1, 1.0f, 700 },
};

static struct {
    int on, live;
    uint32_t clock;
    /* the board */
    float heading, speed, cmd_speed, push_cd, push_t, act_avg;
    int blocked;
    float last_pos[3], pos[3];
    int have_pos;
    /* air: his height (vis_y) flies a parabola over the ground (pos[1]) */
    int air, trick;                     /* trick -1: rolled off something, no trick */
    float air_t, lift, vis_y, vy, gvy;
    float trick_t;                      /* since the trick began (a flick in the air) */
    /* manual: 1 on the tail, -1 on the nose; balance past +-1 drops it */
    int manual;
    /* grind: the side the ledge is on (+1 / -1), its height above his node,
     * and the drawn offset onto it (world) */
    int grind;
    float grind_t, grind_h, grind_off[3], grind_reach;
    float man_t, bal, bal_v, man_tilt;
    /* bail */
    float bail_t, bail_dir[3];
    /* flick-it: the mouse's recent motion, 1 = a flick; armed once it settles */
    float stick[2];
    int armed;
    /* off the board, walking with it in his hand (G) */
    int walk;
    float look_hold;                    /* the mouse looked around: no camera follow yet */
    int ground_ahead;                   /* there is ground past an invisible wall here */
    /* the lean into a carve */
    float lean, turn;
    float clock_t, land_t;              /* time skating; since the last landing */
    /* HUD */
    char shown[40];
    float show_t;
    int score, last_score, tricks_done;
    /* a line: tricks strung together before he rolls a second on plain
     * wheels; banked as their points times how many there were */
    int combo_n;
    float combo_pts, combo_t;
    char combo_line[64];
    /* grabs, in the air: 1 Melon (left button), 2 Method (right), 3 Mute (both) */
    int grab, grab_kind;
    float grab_t, grab_w;
    float land_kick, stuck_t;
    float board_world[16];
    int draw_board;
} s_sk;

void skate_set(int on);
static void roll_off(void);
static void mat_mul(const float a[16], const float b[16], float o[16]);

/* Skate 3's clips on the King (see clip_pose) */
#define SKATE_ANIM_FILE "game/mods/skate3/anims.bin"
#define BOARD_MODEL_INCH 3.162f        /* King model units an inch, at Skate 3's scale */

static struct {
    int tried, ok;
    int idle, lean_l, lean_r, crouch, push, land, manual, nose, grind, grabs[4];
    int g[T_COUNT], a[T_COUNT];         /* a trick's pop and air parts */
    float board[16];                    /* board mesh -> world, from the last pose */
    int have_board;
} s_ck;

static uint32_t s_pose_P;               /* the King's animation instance (skate_pose) */


/* RECOMP_SKATE=1: on from the start. */
int skate_on(void)
{
    static int env = -1;
    if (env < 0) {
        const char *e = getenv("RECOMP_SKATE");
        env = e && *e && *e != '0';
        if (env)
            skate_set(1);
    }
    return s_sk.on;
}

/* Invisible walls let him through only where there is ground beyond them:
 * the ones round the level's edge stand over nothing (recomp_manual.c,
 * king_contacts). */
int skate_walls_open(void)
{
    return skate_live() && s_sk.ground_ahead;
}

/* The mouse flicks tricks: only in the air. Otherwise it looks around. */
int skate_mouse_tricks(void)
{
    return skate_live() && s_sk.air;
}

int skate_live(void)
{
    return s_sk.on && !s_sk.walk && game_in_play() && !game_is_paused();
}

/* Skate 3's animations and board, from the user's own disc
 * (tools/skate3_anim.py). Without them there is no skate mode: a build from
 * source with no Skate 3 is plain Sneak King. */
int skate_available(void)
{
    static int have = -1;
    if (have < 0) {
        have = GetFileAttributesA(SKATE_ANIM_FILE) != INVALID_FILE_ATTRIBUTES
               && GetFileAttributesA("game/mods/skate3/board.bin") != INVALID_FILE_ATTRIBUTES;
        if (!have)
            fprintf(stderr, "[SKATE] no Skate 3 files (%s): skate mode is off (docs/MODS.md)\n", SKATE_ANIM_FILE);
    }
    return have;
}

void skate_set(int on)
{
    if (on && !skate_available())
        return;
    if (on && !s_sk.on) {
        memset(&s_sk, 0, sizeof s_sk);
        s_sk.heading = -100.0f;          /* taken from the King on the first frame */
        s_sk.land_t = 10.0f;
    }
    s_sk.on = on;
    if (!on) {
        mkb_set_mute(0, 0);
        mkb_mouse_buttons(0, NULL);
        roll_off();
    }
}

static int key(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0
           && GetForegroundWindow() == (HWND)xbox_FramebufferWindowHwnd();
}

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float wrap(float a) { return fmodf(a + 3.0f * PI, 2.0f * PI) - PI; }

static void show(const char *name, int pts)
{
    if (pts)
        snprintf(s_sk.shown, sizeof s_sk.shown, "%s  +%d", name, pts);
    else
        snprintf(s_sk.shown, sizeof s_sk.shown, "%s", name);
    s_sk.show_t = 2.0f;
}

/* A trick done: into the line, and up on the HUD. */
static void award(const char *name, int pts)
{
    size_t n = strlen(s_sk.combo_line);
    s_sk.combo_pts += (float)pts;
    s_sk.combo_n++;
    s_sk.combo_t = 0.0f;
    s_sk.tricks_done++;
    if (n + strlen(name) + 4 >= sizeof s_sk.combo_line)     /* keep the newest */
        s_sk.combo_line[0] = 0, n = 0;
    snprintf(s_sk.combo_line + n, sizeof s_sk.combo_line - n, "%s%s", n ? " + " : "", name);
    show(name, pts);
}

/* The line ends: its points times its length go to the score. */
static void combo_bank(void)
{
    if (s_sk.combo_n) {
        int total = (int)(s_sk.combo_pts * (float)s_sk.combo_n);
        s_sk.score += total;
        s_sk.last_score = total;
        if (s_sk.combo_n > 1) {
            snprintf(s_sk.shown, sizeof s_sk.shown, "Combo x%d  +%d", s_sk.combo_n, total);
            s_sk.show_t = 2.0f;
        }
    }
    s_sk.combo_n = 0;
    s_sk.combo_pts = 0.0f;
    s_sk.combo_line[0] = 0;
}

/* -- Sounds ----------------------------------------------------------------
 * Skate 3's own, when tools/skate3_extract.py has pulled them out of the
 * user's disc into SKATE_SND_DIR (16-bit mono WAVs at SND_PCM_RATE); else
 * stand-ins made here: wheels on tarmac, the pop, the landing. The push is
 * always made here (Skate 3's foot sounds aren't picked out yet). */

#define SKATE_SND_DIR "game/mods/skate3/sounds"
enum { S_ROLL, S_POP, S_LAND, S_FLIP, S_SCRAPE, S_GRIND, S_PUSH, S_KINDS };
typedef struct { short *p; int n; } Pcm;
static Pcm s_snd[S_KINDS][4];
static int s_nsnd[S_KINDS];

static float noise(void)
{
    static unsigned x = 12345u;
    x = x * 1664525u + 1013904223u;
    return (float)(int)(x >> 8 & 0xFFFF) / 32768.0f - 1.0f;
}

static short pcm16(float v) { return (short)(clampf(v, -1.0f, 1.0f) * 32000.0f); }

/* The data chunk of a WAV the extractor wrote (already the right format). */
static void wav_load(int kind, const char *name)
{
    char path[MAX_PATH];
    FILE *f;
    unsigned char h[8];
    snprintf(path, sizeof path, "%s/%s.wav", SKATE_SND_DIR, name);
    if (s_nsnd[kind] >= 4 || !(f = fopen(path, "rb")))
        return;
    fseek(f, 12, SEEK_SET);
    while (fread(h, 1, 8, f) == 8) {
        long n = h[4] | h[5] << 8 | h[6] << 16 | (long)h[7] << 24;
        if (!memcmp(h, "data", 4)) {
            Pcm *p = &s_snd[kind][s_nsnd[kind]];
            if ((p->p = (short *)malloc(n)) && fread(p->p, 1, n, f) == (size_t)n) {
                p->n = (int)(n / 2);
                s_nsnd[kind]++;
            }
            break;
        }
        fseek(f, n + (n & 1), SEEK_CUR);
    }
    fclose(f);
}

static Pcm synth(int n)
{
    Pcm p = { (short *)calloc(n, sizeof(short)), n };
    return p;
}

static void sounds_make(void)
{
    static int done;
    static const char *names[] = { "roll", "pop1", "pop2", "pop3", "land1", "land2", "land3", "flip1",
                                   "flip2", "flip3", "flip4", "scrape1", "scrape2", "grind" };
    static const int kinds[] = { S_ROLL, S_POP, S_POP, S_POP, S_LAND, S_LAND, S_LAND, S_FLIP,
                                 S_FLIP, S_FLIP, S_FLIP, S_SCRAPE, S_SCRAPE, S_GRIND };
    float lp = 0.0f, lp2 = 0.0f, hp = 0.0f, prev = 0.0f;
    Pcm p;
    int i;
    if (done)
        return;
    done = 1;
    for (i = 0; i < (int)(sizeof names / sizeof *names); i++)
        wav_load(kinds[i], names[i]);
    if (s_nsnd[S_ROLL])
        fprintf(stderr, "[SKATE] Skate 3 sounds from %s\n", SKATE_SND_DIR);
    if (!s_nsnd[S_ROLL]) {          /* rolling: low rumble, a clack every joint in the pavement */
        p = synth(SND_PCM_RATE);
        for (i = 0; i < p.n; i++) {
            float t = (float)i / SND_PCM_RATE, n = noise(), joint = fmodf(t, 0.25f);
            lp += (n - lp) * 0.08f;
            lp2 += (lp - lp2) * 0.15f;
            p.p[i] = pcm16(lp2 * 2.2f + noise() * 0.035f
                           + (joint < 0.012f ? noise() * 0.5f * (1.0f - joint / 0.012f) : 0.0f));
        }
        s_snd[S_ROLL][s_nsnd[S_ROLL]++] = p;
    }
    if (!s_nsnd[S_POP]) {           /* pop: the tail snapping the ground, a crack over a knock */
        p = synth(SND_PCM_RATE / 6);
        for (i = 0; i < p.n; i++) {
            float t = (float)i / SND_PCM_RATE, n = noise();
            hp = n - prev;
            prev = n;
            p.p[i] = pcm16(hp * 0.8f * expf(-t / 0.008f) + sinf(t * 2.0f * PI * 170.0f) * 0.7f * expf(-t / 0.035f));
        }
        s_snd[S_POP][s_nsnd[S_POP]++] = p;
    }
    if (!s_nsnd[S_LAND]) {          /* landing: a wooden thud, all four wheels */
        p = synth(SND_PCM_RATE / 3);
        lp = 0.0f;
        for (i = 0; i < p.n; i++) {
            float t = (float)i / SND_PCM_RATE;
            lp += (noise() - lp) * 0.3f;
            p.p[i] = pcm16(sinf(t * 2.0f * PI * (95.0f - 40.0f * t)) * 0.9f * expf(-t / 0.07f)
                           + lp * 0.9f * expf(-t / 0.03f));
        }
        s_snd[S_LAND][s_nsnd[S_LAND]++] = p;
    }
    p = synth(SND_PCM_RATE / 4);    /* push: a shoe scuffing the ground */
    lp = 0.0f;
    for (i = 0; i < p.n; i++) {
        float t = (float)i / SND_PCM_RATE, env = sinf(t / 0.25f * PI);
        lp += (noise() - lp) * 0.25f;
        p.p[i] = pcm16(lp * 0.9f * env * env);
    }
    s_snd[S_PUSH][s_nsnd[S_PUSH]++] = p;
}

/* One of a kind's takes, picked at random, a little off pitch each time. */
static void play(int kind, float vol, float pitch)
{
    const Pcm *p;
    if (!s_nsnd[kind])
        return;
    p = &s_snd[kind][(unsigned)(noise() * 1000.0f + 1000.0f) % s_nsnd[kind]];
    snd_pcm_play(p->p, p->n, vol, pitch * (1.0f + 0.05f * noise()));
}

static void loop(int kind, float vol, float pitch)
{
    if (s_nsnd[kind])
        snd_pcm_loop(s_snd[kind][0].p, s_snd[kind][0].n, vol, pitch);
}

static void roll_off(void) { loop(S_ROLL, 0.0f, 1.0f); loop(S_GRIND, 0.0f, 1.0f); }

/* -- Flick-it ------------------------------------------------------------- */

static int zone(const float v[2])
{
    float m = sqrtf(v[0] * v[0] + v[1] * v[1]), a;
    if (m < 0.5f)
        return Z_C;
    a = atan2f(v[0], v[1]);             /* 0 up, clockwise */
    return Z_U + ((int)floorf((a + PI / 8.0f) / (PI / 4.0f)) + 8) % 8;
}

static void pop(int trick);
static void grind_end(void);

static void air_trick(int trick);

/* In the air (Space pops, or off a ledge), one quick flick of the mouse is a
 * trick, picked by its direction; on the ground the mouse looks around.
 * Shift turns the ollie family into nollies. RECOMP_SKATE_FLICK sets how
 * many mouse counts make a flick (default 150). */
static void flick_tick(float dt, long dx, long dy)
{
    static float counts;
    static const int trick[9] = { -1, T_OLLIE, T_HEELFLIP, T_FS_SHUV, T_VARIAL_HEEL,
                                  T_360FLIP, T_VARIAL_KICK, T_BS_SHUV, T_KICKFLIP };
    float k = expf(-dt / 0.08f), m;
    int t;
    if (!counts)
        counts = getenv("RECOMP_SKATE_FLICK") ? (float)atof(getenv("RECOMP_SKATE_FLICK")) : 150.0f;
    s_sk.stick[0] = s_sk.stick[0] * k + dx / counts;
    s_sk.stick[1] = s_sk.stick[1] * k - dy / counts;
    m = sqrtf(s_sk.stick[0] * s_sk.stick[0] + s_sk.stick[1] * s_sk.stick[1]);
    if (m < 0.35f)
        s_sk.armed = 1;
    else if (m >= 1.0f && s_sk.armed) {
        s_sk.armed = 0;
        t = trick[zone(s_sk.stick)];
        if (key(VK_SHIFT))
            t = t == T_OLLIE ? T_NOLLIE : t == T_KICKFLIP ? T_N_KICKFLIP : t == T_HEELFLIP ? T_N_HEELFLIP : t;
        air_trick(t);
    }
    if (m > 1.0f) {                     /* the HUD's stick stays on its ring */
        s_sk.stick[0] /= m;
        s_sk.stick[1] /= m;
    }
}

/* -- Tricks ---------------------------------------------------------------- */

static void manual_end(void);

/* -- Grinds ------------------------------------------------------------------
 * The game holds his node to the ground, so a grind is drawn: coming down
 * beside a ledge or kerb (the collision ray finds ground 12-110 units higher
 * 25-65 units to one side), his node keeps rolling along at its foot while he
 * and the board are drawn on top of it, until the ledge ends, he slows down
 * or pops off. */

/* The height of a ledge on side sgn (+1 / -1) of travel dir, or 0; probed
 * at a few distances out, the nearest that finds one wins (s_sk.grind_reach). */
static float ledge(const float dir[3], int sgn)
{
    static const float reach[3] = { 25.0f, 45.0f, 65.0f };
    int i;
    for (i = 0; i < 3; i++) {
        float q[3] = { s_sk.pos[0] - dir[2] * sgn * reach[i], s_sk.pos[1] + 200.0f,
                       s_sk.pos[2] + dir[0] * sgn * reach[i] };
        float h = 200.0f - game_ground_below(q, 320.0f);
        if (h > 12.0f && h < 110.0f) {
            s_sk.grind_reach = reach[i];
            return h;
        }
    }
    return 0.0f;
}

static void grind_end(void)
{
    if (s_sk.grind && s_sk.grind_t > 0.3f)
        award("50-50 Grind", (int)(s_sk.grind_t * 250.0f * (1.0f + s_sk.speed / 800.0f)));
    s_sk.grind = 0;
}

/* Off the ground at upward speed vy from height y. */
static void launch(int trick, float vy, float y)
{
    manual_end();
    grind_end();
    s_sk.air = 1;
    s_sk.air_t = s_sk.trick_t = 0.0f;
    s_sk.trick = trick;
    s_sk.stick[0] = s_sk.stick[1] = 0.0f;   /* the mouse was looking: start the flick afresh */
    s_sk.armed = 1;
    s_sk.grab = s_sk.grab_kind = 0;
    s_sk.grab_t = 0.0f;
    s_sk.vy = vy;
    s_sk.vis_y = y;
}

/* One trick a jump, over a plain ollie or a roll off a ledge, and not once
 * he has grabbed. Too late in the air and it won't come round: a bail. */
static void air_trick(int trick)
{
    if (!s_sk.air || s_sk.bail_t > 0.0f || s_sk.grab_kind
        || (s_sk.trick >= 0 && s_sk.trick != T_OLLIE && s_sk.trick != T_NOLLIE))
        return;
    s_sk.trick = trick;
    s_sk.trick_t = 0.0f;
    if (s_tricks[trick].roll || s_tricks[trick].yaw)
        play(S_FLIP, 0.6f, 1.0f);
}

static void pop(int trick)
{
    if (s_sk.air || s_sk.bail_t > 0.0f)
        return;
    grind_end();
    launch(trick, POP_V + (s_sk.gvy > 0.0f ? s_sk.gvy : 0.0f), s_sk.pos[1] + s_sk.lift);
    play(S_POP, 0.9f, 1.0f);
    if (s_tricks[trick].roll || s_tricks[trick].yaw)
        play(S_FLIP, 0.6f, 1.0f);
}

static void bail(const float dir[3], const char *why)
{
    memcpy(s_sk.bail_dir, dir, sizeof s_sk.bail_dir);
    manual_end();
    s_sk.bail_t = BAIL_TIME;
    s_sk.combo_n = 0;                   /* the line is lost */
    s_sk.combo_pts = 0.0f;
    s_sk.combo_line[0] = 0;
    s_sk.air = 0;
    s_sk.lift = 0.0f;
    s_sk.speed *= 0.4f;                 /* he slides on a little */
    s_sk.cmd_speed = s_sk.act_avg = 0.0f;
    show(why, 0);
    play(S_LAND, 1.0f, 0.7f);
    play(S_SCRAPE, 0.9f, 0.9f);
}

/* Back on the ground: was the board round in time, and was the drop too big? */
static void land(const float dir[3])
{
    const Trick *t = s_sk.trick >= 0 ? &s_tricks[s_sk.trick] : NULL;
    int pts;
    s_sk.air = 0;
    s_sk.lift = 0.0f;
    s_sk.land_t = 0.0f;
    if (t && (t->roll || t->yaw) && s_sk.trick_t < TRICK_TIME * 0.85f) {
        bail(dir, "BAIL!  Too early");
        return;
    }
    if (-s_sk.vy > HARD_LAND) {
        bail(dir, "BAIL!  Too high");
        return;
    }
    if (s_sk.air_t > 0.12f)
        play(S_LAND, clampf(-s_sk.vy / 900.0f, 0.4f, 1.0f), 1.0f);
    s_sk.land_kick = clampf(-s_sk.vy / 900.0f, 0.2f, 1.0f);
    if (!t && s_sk.air_t < 0.8f && !(s_sk.grab_kind && s_sk.grab_t > 0.12f))   /* a hop off a kerb */
        return;
    pts = (int)((t ? t->score : 0) * (1.0f + s_sk.speed / 800.0f) + (s_sk.air_t > 0.8f ? 150.0f * s_sk.air_t : 0.0f));
    if (s_sk.grab_kind && s_sk.grab_t > 0.12f) {         /* a grab on the way */
        static const char *grabs[4] = { "", "Melon", "Method", "Mute" };
        char name[40];
        snprintf(name, sizeof name, "%s%s%s", t ? t->name : "", t ? " + " : "", grabs[s_sk.grab_kind]);
        award(name, pts + (int)(100.0f + 400.0f * s_sk.grab_t));
    } else {
        award(t ? t->name : "Air", pts);
    }
}

/* Manuals: hold the left mouse button (right for a nose manual) while
 * rolling, and keep him balanced with W / S. */
static void manual_end(void)
{
    if (s_sk.manual && s_sk.man_t > 0.6f)
        award(s_sk.manual > 0 ? "Manual" : "Nose Manual", (int)(s_sk.man_t * 120.0f * (1.0f + s_sk.speed / 800.0f)));
    if (s_sk.manual)
        play(S_LAND, 0.5f, 1.2f);       /* the wheels slap back down */
    s_sk.manual = 0;
}

static void manual_tick(float dt, int btn)
{
    int want = btn & 1 ? 1 : btn & 2 ? -1 : 0;
    if (s_sk.air || s_sk.bail_t > 0.0f || s_sk.speed < 150.0f)
        want = 0;
    if (want != s_sk.manual) {
        manual_end();
        if (want) {
            s_sk.manual = want;
            s_sk.man_t = 0.0f;
            s_sk.bal = 0.0f;
            s_sk.bal_v = 0.25f * noise();
        }
    }
    if (s_sk.manual) {
        /* the balance tips away on its own, faster the further it has gone */
        s_sk.bal_v += (2.2f * s_sk.bal + 1.5f * noise() + 2.8f * (float)(key('S') - key('W'))) * dt;
        s_sk.bal_v *= expf(-dt * 1.5f);
        s_sk.bal += s_sk.bal_v * dt;
        s_sk.man_t += dt;
        if (fabsf(s_sk.bal) > 1.0f)
            manual_end();
    }
    s_sk.man_tilt += ((float)s_sk.manual * (1.0f + 0.35f * s_sk.bal * (float)s_sk.manual) - s_sk.man_tilt)
                     * clampf(dt * 12.0f, 0.0f, 1.0f);
}

/* -- Debug autopilot -----------------------------------------------------------
 * RECOMP_SKATE_GOTO="x,z;x,z!;..." steers through waypoints (world x, z),
 * pushing up to about 600 units/s; RECOMP_SKATE_AUTOGRIND=1 ollies whenever
 * a ledge is beside him, to find grinds; '!' ollies on reaching that one. For
 * testing ledges and citizens without steering blind. Returns 1 while it
 * drives, with the turn (-1..1) and whether to push. */
static int autopilot(float *turn, int *push)
{
    static float wp[16][2];
    static int pop_at[16], n = -1, cur;
    const char *e = getenv("RECOMP_SKATE_GOTO");
    float vx, vz, err;
    if (n < 0) {
        n = 0;
        while (e && *e && n < 16) {
            if (sscanf(e, "%f,%f", &wp[n][0], &wp[n][1]) != 2)
                break;
            while (*e && *e != ';' && *e != '!')
                e++;
            pop_at[n++] = *e == '!';
            while (*e == '!' || *e == ';')
                e++;
        }
    }
    if (cur >= n)
        return 0;
    vx = wp[cur][0] - s_sk.pos[0];
    vz = wp[cur][1] - s_sk.pos[2];
    if (vx * vx + vz * vz < 120.0f * 120.0f) {
        if (pop_at[cur])
            pop(T_OLLIE);
        if (getenv("RECOMP_SKATE_DEBUG"))
            fprintf(stderr, "[SKATE] waypoint %d reached\n", cur);
        cur++;
        return cur < n;
    }
    err = wrap(atan2f(vx, -vz) - s_sk.heading);
    *turn = clampf(-err * 3.0f, -1.0f, 1.0f);
    *push = s_sk.speed < 600.0f;
    return 1;
}

/* G steps off the board, to walk with it in his hand, and back on. */
static void walk_toggle(uint32_t king)
{
    static int was;
    uint32_t body;
    int g = key('G');
    if (g && !was && game_in_play() && !game_is_paused() && !s_sk.air && !s_sk.grind && s_sk.bail_t <= 0.0f) {
        s_sk.walk = !s_sk.walk;
        manual_end();
        if (s_sk.combo_n)
            combo_bank();
        s_sk.speed = s_sk.cmd_speed = s_sk.act_avg = 0.0f;
        s_sk.heading = -100.0f;          /* back on: along the way he faces */
        s_sk.have_pos = 0;
        if (s_sk.walk && king && (body = MEM32(king + 0x190)) != 0)
            MEMF(body + 0x30) = MEMF(body + 0x38) = 0.0f;
        play(s_sk.walk ? S_LAND : S_PUSH, 0.5f, 1.1f);
    }
    was = g;
}

/* -- The board, once a game frame from King::Update ------------------------- */

void skate_king_tick(void)
{
    uint32_t king = MEM32(GA_KING), body, node;
    float dt = MEMF(GA_DT), dir[3], actual = 0.0f, gv = 0.0f, ground;
    long dx = 0, dy = 0;
    int live = skate_live(), i, btn;

    if (s_sk.on)
        walk_toggle(king);
    live = skate_live();
    mkb_set_mute(live, live ? 0x81u : 0u);      /* A (Space, click) and RT (Shift) */
    btn = mkb_mouse_buttons(live ? 3 : 0, NULL);
    s_sk.live = live;
    sounds_make();
    if (!live)
        roll_off();
    if (!live || !king || !(body = MEM32(king + 0x190)) || !(node = MEM32(king + 0x64)))
        return;
    if (!(dt > 0.0f) || dt > 0.25f)
        dt = 1.0f / 30.0f;
    if (MEM32(GA_CLOCK) == s_sk.clock)
        return;
    s_sk.clock = MEM32(GA_CLOCK);

    for (i = 0; i < 3; i++)
        s_sk.pos[i] = MEMF(node + 4 + 4 * i);
    if (s_sk.heading < -10.0f)          /* along the way he faces */
        s_sk.heading = MEMF(king + 0x30C);
    dir[0] = sinf(s_sk.heading); dir[1] = 0.0f; dir[2] = -cosf(s_sk.heading);

    /* how far the physics really took him along the board last frame */
    if (s_sk.have_pos) {
        float d[3] = { s_sk.pos[0] - s_sk.last_pos[0], 0.0f, s_sk.pos[2] - s_sk.last_pos[2] };
        actual = (d[0] * dir[0] + d[2] * dir[2]) / dt;
        gv = (s_sk.pos[1] - s_sk.last_pos[1]) / dt;
        if (!s_sk.air)                  /* up or down a slope: the distance along it */
            actual = (actual < 0.0f ? -1.0f : 1.0f) * sqrtf(actual * actual + gv * gv);
        if (d[0] * d[0] + d[2] * d[2] > 300.0f * 300.0f) {  /* a warp or respawn */
            s_sk.cmd_speed = s_sk.speed = actual = gv = 0.0f;
            s_sk.air = 0;
            s_sk.vis_y = s_sk.pos[1];
        }
    } else {
        s_sk.vis_y = s_sk.pos[1];
    }
    memcpy(s_sk.last_pos, s_sk.pos, sizeof s_sk.pos);
    s_sk.have_pos = 1;

    if (s_sk.air)
        mkb_take_mouse_delta(&dx, &dy);
    s_sk.act_avg += (actual - s_sk.act_avg) * 0.4f;
    if (s_sk.bail_t > 0.0f) {
        s_sk.bail_t -= dt;
        s_sk.speed -= BRAKE * 1.5f * dt;
        s_sk.armed = 0;
    } else {
        /* a wall: hit at speed, he bails; held back for a few frames
         * (brushing it, or stuck), he slows to what he really makes. One
         * short physics step is not a wall. */
        s_sk.blocked = s_sk.cmd_speed > 60.0f && actual < s_sk.cmd_speed * 0.7f ? s_sk.blocked + 1 : 0;
        if (s_sk.grind || s_sk.air)
            ;                           /* rubbing along the ledge's side is the grind; in the air
                                         * he is drawn over what stops his node */
        else if (s_sk.act_avg > 350.0f && actual < s_sk.cmd_speed * 0.3f)
            bail(dir, "BAIL!");
        else if (s_sk.blocked >= 4)
            s_sk.speed = actual > 0.0f ? actual : 0.0f;
        flick_tick(dt, dx, dy);
        {
            static int space_was;
            int sp = key(VK_SPACE);
            if (sp && !space_was)
                pop(T_OLLIE);
            space_was = sp;
        }
    }
    manual_tick(dt, btn);

    /* grabs: the mouse buttons in the air, once the flip has come round */
    s_sk.grab = s_sk.air && s_sk.trick_t > (s_sk.trick >= 0 && (s_tricks[s_sk.trick].roll || s_tricks[s_sk.trick].yaw)
                                         ? TRICK_TIME : 0.15f)
                ? ((btn & 3) == 3 ? 3 : btn & 1 ? 1 : btn & 2 ? 2 : 0) : 0;
    if (s_sk.grab) {
        s_sk.grab_kind = s_sk.grab;
        s_sk.grab_t += dt;
    }
    s_sk.grab_w += ((s_sk.grab ? 1.0f : 0.0f) - s_sk.grab_w) * clampf(dt * 12.0f, 0.0f, 1.0f);

    /* the line: banked after a second on plain wheels */
    if (s_sk.air || s_sk.manual || s_sk.grind || s_sk.bail_t > 0.0f)
        s_sk.combo_t = 0.0f;
    else if ((s_sk.combo_t += dt) > 1.0f && s_sk.combo_n)
        combo_bank();
    s_sk.land_kick *= expf(-dt * 6.0f);

    /* push, brake, roll; in a manual W and S balance him instead */
    s_sk.push_cd -= dt;
    s_sk.push_t = s_sk.push_t > dt ? s_sk.push_t - dt : 0.0f;
    if (s_sk.bail_t <= 0.0f && !s_sk.air && !s_sk.manual && !s_sk.grind) {
        float ap_turn = 0.0f;
        int ap_push = 0;
        autopilot(&ap_turn, &ap_push);
        if ((key('W') || ap_push) && s_sk.push_cd <= 0.0f) {
            s_sk.speed += PUSH_GAIN * clampf(1.0f - s_sk.speed / PUSH_TOP, 0.0f, 1.0f);
            s_sk.push_cd = PUSH_GAP;
            s_sk.push_t = PUSH_GAP;
            play(S_PUSH, 0.6f, 0.95f);
        }
        if (key('S'))
            s_sk.speed -= BRAKE * dt;
    }
    s_sk.speed -= (ROLL_DRAG + AIR_DRAG * s_sk.speed) * dt;
    /* downhill speeds him up, uphill slows him: g times the slope */
    if (!s_sk.air && s_sk.bail_t <= 0.0f && s_sk.speed > 30.0f)
        s_sk.speed -= GRAVITY * clampf(s_sk.gvy / s_sk.speed, -0.5f, 0.5f) * dt;
    if (s_sk.speed < 0.0f)
        s_sk.speed = 0.0f;

    /* carve: slower to turn the faster he goes; a little in the air */
    s_sk.turn = (float)(key('D') - key('A'));
    {
        float ap_turn = 0.0f;
        int ap_push = 0;
        if (autopilot(&ap_turn, &ap_push))
            s_sk.turn = ap_turn;
    }
    if (s_sk.bail_t <= 0.0f) {
        float rate = CARVE / (1.0f + s_sk.speed / 650.0f) * (s_sk.air ? 0.5f : s_sk.grind ? 0.0f : 1.0f);
        s_sk.heading = wrap(s_sk.heading - s_sk.turn * rate * dt);   /* heading grows to the left */
        s_sk.speed -= fabsf(s_sk.turn) * s_sk.speed * 0.08f * dt;
    }
    s_sk.lean += (s_sk.turn * clampf(s_sk.speed / 600.0f, 0.0f, 1.0f) - s_sk.lean) * clampf(dt * 6.0f, 0.0f, 1.0f);
    dir[0] = sinf(s_sk.heading); dir[2] = -cosf(s_sk.heading);

    /* Air. The game keeps his node on the ground, or drops it off a ledge
     * with its own (gentler) gravity: the ground under him, by the game's
     * collision ray, says which. His drawn height (vis_y) flies its own
     * parabola and never goes below the node. Off a ledge, or off the lip
     * of a ramp (the ground stops rising under him), he is in the air too. */
    ground = game_ground_below(s_sk.pos, 120.0f);
    {   /* ground somewhere under the spot 250 units ahead, hill or hollow */
        float ahead[3] = { s_sk.pos[0] + dir[0] * 250.0f, s_sk.pos[1] + 1500.0f, s_sk.pos[2] + dir[2] * 250.0f };
        s_sk.ground_ahead = game_ground_below(ahead, 4000.0f) < 3999.0f;
    }
    if (!s_sk.air) {
        float was = s_sk.gvy;
        if (s_sk.bail_t <= 0.0f && s_sk.speed > 150.0f
            && (ground > 25.0f || (was > 150.0f && gv < was - GRAVITY * dt * 1.5f))) {
            launch(-1, ground > 25.0f ? gv : was, s_sk.vis_y);
        } else {
            s_sk.vis_y = s_sk.pos[1];
            s_sk.gvy += (gv - s_sk.gvy) * 0.5f;
        }
    }
    if (getenv("RECOMP_SKATE_AUTOGRIND")) {     /* debug: where the ledges are */
        static DWORD last;
        float l = ledge(dir, -1), r = ledge(dir, 1);
        if ((l > 0.0f || r > 0.0f) && GetTickCount() - last > 150) {
            last = GetTickCount();
            fprintf(stderr, "[SKATE] ledge at %.0f %.0f hd %.2f: left %.0f right %.0f reach %.0f speed %.0f\n",
                    s_sk.pos[0], s_sk.pos[2], s_sk.heading, l, r, s_sk.grind_reach, s_sk.speed);
        }
    }
    if (getenv("RECOMP_SKATE_AUTOGRIND") && !s_sk.air && !s_sk.grind && s_sk.bail_t <= 0.0f
        && s_sk.speed > 150.0f && (ledge(dir, -1) > 0.0f || ledge(dir, 1) > 0.0f))
        pop(T_OLLIE);                   /* debug: ollie at any ledge beside him */
    if (s_sk.air && s_sk.speed > 150.0f && s_sk.bail_t <= 0.0f && s_sk.air_t > 0.1f) {
        int sgn;
        for (sgn = -1; sgn <= 1; sgn += 2) {         /* coming down onto a ledge: grind it */
            float h = ledge(dir, sgn), above = s_sk.vis_y - s_sk.pos[1] - h;
            if (h > 0.0f && above > -15.0f && above < 60.0f) {   /* the board is up to its top */
                land(dir);
                if (s_sk.bail_t > 0.0f)
                    break;
                s_sk.grind = sgn;
                s_sk.grind_t = 0.0f;
                s_sk.grind_h = h;
                play(S_LAND, 0.8f, 1.3f);
                break;
            }
        }
    }
    if (s_sk.grind) {
        float h = ledge(dir, s_sk.grind), k = clampf(dt * 10.0f, 0.0f, 1.0f);
        s_sk.grind_t += dt;
        s_sk.speed -= 120.0f * dt;
        s_sk.stuck_t = actual < 80.0f ? s_sk.stuck_t + dt : 0.0f;
        if (!h || s_sk.speed < 150.0f || s_sk.bail_t > 0.0f || key('S') || s_sk.stuck_t > 0.4f) {   /* off the end */
            float y = s_sk.pos[1] + s_sk.grind_h;
            grind_end();
            launch(-1, 0.0f, y);
        } else {
            s_sk.grind_h += (h - s_sk.grind_h) * k;
            s_sk.lift = s_sk.grind_h;
            s_sk.grind_off[0] += (-dir[2] * s_sk.grind * (s_sk.grind_reach + 10.0f) - s_sk.grind_off[0]) * k;
            s_sk.grind_off[2] += (dir[0] * s_sk.grind * (s_sk.grind_reach + 10.0f) - s_sk.grind_off[2]) * k;
        }
    }
    if (!s_sk.grind) {
        float k = clampf(dt * 8.0f, 0.0f, 1.0f);
        s_sk.grind_off[0] -= s_sk.grind_off[0] * k;
        s_sk.grind_off[2] -= s_sk.grind_off[2] * k;
    }
    if (s_sk.air) {
        s_sk.air_t += dt;
        s_sk.trick_t += dt;
        s_sk.vy -= GRAVITY * dt;
        s_sk.vis_y += s_sk.vy * dt;
        if (s_sk.vis_y <= s_sk.pos[1]) {
            s_sk.vis_y = s_sk.pos[1];
            if (ground > 12.0f && s_sk.vy < gv)  /* the game is still dropping him */
                s_sk.vy = gv;
            else {
                s_sk.gvy = 0.0f;
                land(dir);
            }
        }
        s_sk.lift = s_sk.air ? s_sk.vis_y - s_sk.pos[1] : 0.0f;
    }

    /* close calls: citizens he flies past jump out of the way */
    if (s_sk.bail_t <= 0.0f && s_sk.speed > 600.0f) {
        int n = game_citizens_startle(s_sk.pos, getenv("RECOMP_SKATE_STARTLE")   /* debug: a wider reach */
                                                ? (float)atof(getenv("RECOMP_SKATE_STARTLE"))
                                                : 140.0f + s_sk.speed * 0.08f);
        if (n) {
            int pts = 100 * n;
            s_sk.score += pts;
            show(n > 1 ? "Close Calls" : "Close Call", pts);
            if (getenv("RECOMP_SKATE_DEBUG"))
                fprintf(stderr, "[SKATE] close call: %d citizens\n", n);
        }
    }

    loop(S_GRIND, s_sk.grind ? 0.8f : 0.0f, 0.8f + s_sk.speed / 3000.0f);
    loop(S_ROLL, s_sk.air || s_sk.grind || s_sk.bail_t > 0.0f ? 0.0f : 0.75f * clampf(s_sk.speed / 500.0f, 0.0f, 1.0f),
         s_nsnd[S_ROLL] && s_snd[S_ROLL][0].n > SND_PCM_RATE * 2 ? 0.75f + s_sk.speed / 2500.0f
                                                                  : 0.55f + s_sk.speed / 1400.0f);

    /* the physics step moves him by this, with the game's own collision */
    s_sk.cmd_speed = s_sk.speed;
    MEMF(body + 0x30) = dir[0] * s_sk.speed;
    MEMF(body + 0x34) = 0.0f;           /* the game keeps him on the ground whatever goes here */
    MEMF(body + 0x38) = dir[2] * s_sk.speed;

    /* Skate 3's clips turn him side on themselves; the stance made here
     * needs him turned: chest to the right of travel */
    MEMF(king + 0x30C) = s_ck.ok ? s_sk.heading : wrap(s_sk.heading + PI * 0.5f);
    MEMF(king + 0x1A4) = MEMF(king + 0x30C);

    s_sk.show_t = s_sk.show_t > dt ? s_sk.show_t - dt : 0.0f;
    s_sk.look_hold -= dt;
    s_sk.clock_t += dt;
    s_sk.land_t += dt;
    if (getenv("RECOMP_SKATE_DEBUG")) {
        static DWORD last;
        if (GetTickCount() - last > 400) {
            last = GetTickCount();
            fprintf(stderr, "[SKATE] pos %.0f %.0f %.0f hd %.2f speed %.0f actual %.0f gvy %.0f gnd %.0f ledge %.0f/%.0f grind %d air %d lift %.0f vy %.0f "
                    "man %d bal %.2f bail %.2f stick %.2f %.2f\n",
                    s_sk.pos[0], s_sk.pos[1], s_sk.pos[2], s_sk.heading, s_sk.speed, actual, s_sk.gvy, ground, ledge(dir, -1), ledge(dir, 1), s_sk.grind, s_sk.air,
                    s_sk.lift, s_sk.vy, s_sk.manual, s_sk.bal, s_sk.bail_t, s_sk.stick[0], s_sk.stick[1]);
        }
    }
}

/* -- Skate 3's own animations on the King ------------------------------------
 * tools/skate3_anim.py retargets Skate 3's skater clips (OnBoard.abin from the
 * user's disc) onto the King into SKATE_ANIM_FILE; cod4_anim.c plays them.
 * Each frame's prop is the board in his model space, so flips and shove-its
 * are Skate 3's own. Without the file he keeps the procedural stance below. */

static void clips_find(void)
{
    static const char *trick[T_COUNT] = { "ollie", "nollie", "kickflip", "heelflip", "shuvit", "shuvit",
                                          "varialkick", "varialheel", "kickflip", "heelflip", "360flip" };
    char n[32];
    int t;
    s_ck.tried = 1;
    if (!canim_load(SKATE_ANIM_FILE))
        return;
    s_ck.idle = canim_find("idle");
    s_ck.lean_l = canim_find("lean_l");
    s_ck.lean_r = canim_find("lean_r");
    s_ck.crouch = canim_find("crouch");
    s_ck.push = canim_find("push");
    s_ck.land = canim_find("land");
    s_ck.manual = canim_find("manual");
    s_ck.nose = canim_find("nosemanual");
    s_ck.grind = canim_find("grind");
    s_ck.grabs[0] = -1;
    s_ck.grabs[1] = canim_find("melon");
    s_ck.grabs[2] = canim_find("method");
    s_ck.grabs[3] = canim_find("mute");
    for (t = 0; t < T_COUNT; t++) {
        snprintf(n, sizeof n, "%s_g", trick[t]);
        s_ck.g[t] = canim_find(n);
        snprintf(n, sizeof n, "%s_a", trick[t]);
        s_ck.a[t] = canim_find(n);
    }
    s_ck.ok = s_ck.idle >= 0;
}

static void layer(KingPose *base, int clip, float t, int loop, float w)
{
    KingPose top;
    if (clip < 0 || w <= 0.0f)
        return;
    canim_sample(clip, t, loop, &top);
    canim_mix(base, &top, w, 1);
    canim_mix_origin(base, &top, w);
}

/* A trick in the air: the pop part, then the air part, stretched so the
 * pair takes TRICK_TIME; then he rides out the rest of the air. */
static void trick_layer(KingPose *p, int trick, float t)
{
    int g = s_ck.g[trick], a = s_ck.a[trick];
    float lg = g >= 0 ? canim_length(g) : 0.0f, la = a >= 0 ? canim_length(a) : 0.0f, k;
    if (lg + la <= 0.0f)
        return;
    k = (lg + la) / TRICK_TIME;                 /* playback rate */
    if (t * k < lg)
        layer(p, g, t * k, 0, 1.0f);
    else
        layer(p, a, t * k - lg, 0, 1.0f);
}

/* Writes his bones from the clips; 0 when there are none. */
static int clip_pose(uint32_t P)
{
    uint32_t bones = MEM32(P + 0x1A4), n = MEM8(MEM32(P + 0x1A0) + 0xF1), i;
    KingPose p;
    float m[3][3], x, y, z, w, bm[16], model[16], off[3];
    int r;

    if (!s_ck.tried)
        clips_find();
    if (!s_ck.ok)
        return 0;
    canim_sample(s_ck.idle, s_sk.clock_t, 1, &p);
    layer(&p, s_sk.lean < 0.0f ? s_ck.lean_l : s_ck.lean_r, s_sk.clock_t, 1, fabsf(s_sk.lean) * 0.8f);
    if (s_sk.push_t > 0.0f && s_ck.push >= 0)   /* one stroke a push */
        layer(&p, s_ck.push, (1.0f - s_sk.push_t / PUSH_GAP) * canim_length(s_ck.push), 0,
              clampf(sinf(s_sk.push_t / PUSH_GAP * PI) * 2.0f, 0.0f, 1.0f));
    if (!s_sk.air && s_sk.stick[1] < -0.3f)     /* winding up a flick */
        layer(&p, s_ck.crouch, 0.25f, 0, clampf((-s_sk.stick[1] - 0.3f) * 2.0f, 0.0f, 1.0f));
    if (s_sk.manual || fabsf(s_sk.man_tilt) > 0.05f)
        layer(&p, s_sk.man_tilt > 0.0f ? s_ck.manual : s_ck.nose, s_sk.clock_t, 1,
              clampf(fabsf(s_sk.man_tilt), 0.0f, 1.0f));
    if (s_sk.air)
        trick_layer(&p, s_sk.trick >= 0 ? s_sk.trick : T_OLLIE, s_sk.trick >= 0 ? s_sk.trick_t : TRICK_TIME * 0.6f);
    else if (s_sk.land_t < 0.6f && s_ck.land >= 0)
        layer(&p, s_ck.land, s_sk.land_t, 0, clampf((0.6f - s_sk.land_t) / 0.3f, 0.0f, 1.0f));
    if (s_sk.grab_kind && s_sk.grab_w > 0.01f)
        layer(&p, s_ck.grabs[s_sk.grab_kind], s_sk.grab_t, 1, s_sk.grab_w);
    if (s_sk.grind)
        layer(&p, s_ck.grind, s_sk.grind_t, 1, clampf(s_sk.grind_t * 6.0f, 0.0f, 1.0f));
    for (r = 0; r < 16; r++)
        model[r] = MEMF(P + 4 * r);
    /* onto the ledge: the grind's world offset in his model axes (rows 0-2) */
    for (r = 0; r < 3; r++) {
        const float *ax = &model[4 * r];
        float n2 = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
        off[r] = n2 > 0.0f ? (s_sk.grind_off[0] * ax[0] + s_sk.grind_off[2] * ax[2]) / n2 : 0.0f;
    }
    for (r = 0; r < 3; r++)
        p.origin[r] += off[r];
    p.origin[1] += s_sk.lift / 0.88f;

    for (i = 0; i < n; i++) {
        uint32_t b = bones + 0x180 * i, def = MEM32(b + 0x174), pal = def ? MEM32(def + 0xC0) : 0xFFFF;
        if (pal >= CANIM_BONES || !p.has[pal])
            continue;
        MEMF(b + 0x30) = p.q[pal][0];
        MEMF(b + 0x34) = p.q[pal][1];
        MEMF(b + 0x38) = p.q[pal][2];
        MEMF(b + 0x3C) = p.q[pal][3];
        if (pal == 1) {
            MEMF(b + 0x10) = p.origin[0];
            MEMF(b + 0x14) = p.origin[1];
            MEMF(b + 0x18) = p.origin[2];
        }
    }

    /* the board: rows are where the mesh's axes go in his model space (nose
     * along Skate's +Z, which is the King's -Z; up; across), the mesh's
     * ground at the board root, then his model -> world (P+0) */
    x = p.grip_q[0]; y = p.grip_q[1]; z = p.grip_q[2]; w = p.grip_q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w); m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w); m[2][1] = 2 * (y * z + x * w); m[2][2] = 1 - 2 * (x * x + y * y);
    memset(bm, 0, sizeof bm);
    for (r = 0; r < 3; r++) {
        bm[0 + r] = -m[r][2] * BOARD_MODEL_INCH;   /* mesh X (nose) -> M (0,0,-1) */
        bm[4 + r] = m[r][1] * BOARD_MODEL_INCH;    /* mesh Y (up)   -> M (0,1,0) */
        bm[8 + r] = m[r][0] * BOARD_MODEL_INCH;    /* mesh Z        -> M (1,0,0) */
        bm[12 + r] = p.grip_p[r] + off[r];         /* the root is where the wheels touch */
    }
    bm[13] += s_sk.lift / 0.88f;
    bm[15] = 1.0f;
    mat_mul(bm, model, s_ck.board);
    s_ck.have_board = 1;
    return 1;
}

/* -- His bones: the lift, the lean, the fall ------------------------------- */

static void qmul(const float a[4], const float b[4], float o[4])
{
    float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                   a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                   a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                   a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
    memcpy(o, r, sizeof r);
}

/* Bones of an animation instance before the build: local rotations in the
 * engine's convention (conjugates) at +0x30, translation at +0x10, the
 * parent bone at +0x160, the palette index at [+0x174]+0xC0. */
#define SK_BONES 96
static struct { uint32_t addr; int parent, pal; float q[4]; } s_b[SK_BONES];
static int s_nb;

static void conj(const float a[4], float o[4]) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }

/* A bone's rotation in model space (standard convention). */
static void model_q(int i, float o[4])
{
    float l[4], p[4];
    conj(s_b[i].q, l);
    if (s_b[i].parent < 0) {
        memcpy(o, l, sizeof l);
        return;
    }
    model_q(s_b[i].parent, p);
    qmul(p, l, o);
}

/* Turn a bone by `deg` about a model-space axis (X his left-right, Y up, Z
 * back: the model faces -Z), keeping its children with it. */
static void turn(int pal, float x, float y, float z, float deg)
{
    float h = deg * 0.0174533f * 0.5f, r[4] = { x * sinf(h), y * sinf(h), z * sinf(h), cosf(h) };
    float mp[4] = { 0, 0, 0, 1 }, mpi[4], l[4], t[4];
    int i;
    for (i = 0; i < s_nb && s_b[i].pal != pal; i++)
        ;
    if (i == s_nb || !deg)
        return;
    if (s_b[i].parent >= 0)
        model_q(s_b[i].parent, mp);
    conj(mp, mpi);
    conj(s_b[i].q, l);
    /* l' = mp^-1 * r * mp * l */
    qmul(mpi, r, t);
    qmul(t, mp, t);
    qmul(t, l, l);
    conj(l, s_b[i].q);
    MEMF(s_b[i].addr + 0x30) = s_b[i].q[0];
    MEMF(s_b[i].addr + 0x34) = s_b[i].q[1];
    MEMF(s_b[i].addr + 0x38) = s_b[i].q[2];
    MEMF(s_b[i].addr + 0x3C) = s_b[i].q[3];
}

/* ponytail: angles found by eye; RECOMP_SKATE_POSE=hip,knee,ankle,arm,spine,drop
 * (degrees) overrides them while tuning. */
static float s_pose_deg[6] = { 38.0f, -70.0f, 32.0f, 55.0f, 12.0f, 18.0f };

/* Before the bone build (0x151650) for the King's animation instance P: a
 * skater's stance over his own animation. Knees bent, arms out along the
 * board for balance, the Origin raised onto the deck and through the ollie
 * (tucking up in the air), tipped into a carve, and over in a bail. */
void skate_pose(uint32_t P)
{
    static int env_done;
    uint32_t bones = MEM32(P + 0x1A4), n = MEM8(MEM32(P + 0x1A0) + 0xF1), i, j;
    float up = (DECK_TOP * BOARD_UNIT + s_sk.lift) / 0.88f, tilt, tuck, crouch, fall, k, *d = s_pose_deg;
    int clipped;
    s_pose_P = P;
    if (!s_sk.live || ((clipped = clip_pose(P)) && s_sk.bail_t <= 0.0f))
        return;
    if (!env_done) {
        env_done = 1;
        if (getenv("RECOMP_SKATE_POSE"))
            sscanf(getenv("RECOMP_SKATE_POSE"), "%f,%f,%f,%f,%f,%f", &d[0], &d[1], &d[2], &d[3], &d[4], &d[5]);
    }
    s_nb = n < SK_BONES ? (int)n : SK_BONES;
    for (i = 0; i < (uint32_t)s_nb; i++) {
        uint32_t b = bones + 0x180 * i, def = MEM32(b + 0x174), par = MEM32(b + 0x160);
        s_b[i].addr = b;
        s_b[i].pal = def ? (int)MEM32(def + 0xC0) : -1;
        s_b[i].parent = -1;
        for (j = 0; j < (uint32_t)s_nb; j++)
            if (par == bones + 0x180 * j && j != i)
                s_b[i].parent = (int)j;
        for (j = 0; j < 4; j++)
            s_b[i].q[j] = MEMF(b + 0x30 + 4 * j);
    }
    /* a bail: over onto the ground (fall), then back up onto the board */
    fall = s_sk.bail_t > 0.0f ? clampf((BAIL_TIME - s_sk.bail_t) / 0.35f, 0.0f, 1.0f)
                                * clampf(s_sk.bail_t / GETUP_TIME, 0.0f, 1.0f) : 0.0f;
    fall = fall * fall * (3.0f - 2.0f * fall);
    if (clipped) {          /* Skate 3's wipeouts are ragdoll: over he goes by hand */
        for (i = 0; i < (uint32_t)s_nb; i++)
            if (s_b[i].pal == 1)
                MEMF(s_b[i].addr + 0x14) -= 150.0f * fall;
        turn(1, 1, 0, 0, -80.0f * fall);
        return;
    }
    /* a deeper crouch winding up a push and tucked in the air */
    tuck = s_sk.air ? clampf(s_sk.lift / 110.0f, 0.0f, 1.0f) : 0.0f;
    crouch = 1.0f + 0.35f * tuck + 0.25f * (s_sk.push_t > 0.0f ? sinf(s_sk.push_t / PUSH_GAP * PI) : 0.0f);
    for (i = 0; i < (uint32_t)s_nb; i++)
        if (s_b[i].pal == 1)    /* bent knees lower the hips onto the deck */
            MEMF(s_b[i].addr + 0x14) += (up - d[5] * crouch * (1.0f - 0.8f * tuck)
                                         + 14.0f * fabsf(s_sk.man_tilt)) * (1.0f - fall) - 150.0f * fall;

    tilt = 13.0f * s_sk.lean * (1.0f - fall) + 80.0f * fall;
    turn(1, 1, 0, 0, -tilt);
    turn(1, 0, 0, 1, 9.0f * s_sk.man_tilt * (1.0f - fall));   /* weight over the wheels he's on */
    k = 1.0f - fall;
    turn(8, 1, 0, 0, d[4] * crouch * k);
    turn(75, 1, 0, 0, d[0] * crouch * k);
    turn(79, 1, 0, 0, d[0] * crouch * k);
    turn(76, 1, 0, 0, d[1] * crouch * k);
    turn(80, 1, 0, 0, d[1] * crouch * k);
    turn(77, 1, 0, 0, d[2] * crouch * k);
    turn(81, 1, 0, 0, d[2] * crouch * k);
    turn(12, 0, 0, 1, -d[3] * (1.0f + 0.4f * tuck) * k);
    turn(33, 0, 0, 1, d[3] * (1.0f + 0.4f * tuck) * k);
}

/* -- The camera follows the board ------------------------------------------ */

/* After KingCam's own target update: its yaw (cam+0x128 now, +0x130 wanted,
 * radians; more turns the view right) takes the mouse's look (radians), and
 * a moment after the mouse stops, eases round to look along the board. */
void skate_camera(uint32_t cam, float look)
{
    float right[3], fwd[3], d[2], e, nf, nr;
    if (look != 0.0f) {
        MEMF(cam + 0x128) += look;
        MEMF(cam + 0x130) += look;
        s_sk.look_hold = 1.5f;
    }
    if (s_sk.live && getenv("RECOMP_SKATE_ORBIT")) {   /* debug: circle him to look at the pose */
        e = (float)atof(getenv("RECOMP_SKATE_ORBIT")) * 0.0174533f * MEMF(GA_DT);
        MEMF(cam + 0x128) += e;
        MEMF(cam + 0x130) += e;
        return;
    }
    if (!s_sk.live || s_sk.speed < 40.0f || s_sk.look_hold > 0.0f)
        return;
    for (int i = 0; i < 3; i++) {
        right[i] = MEMF(GA_VIEW + 4 * (4 * i + 0));
        fwd[i] = MEMF(GA_VIEW + 4 * (4 * i + 2));
    }
    nf = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    nr = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (nf < 1e-3f || nr < 1e-3f)
        return;
    d[0] = sinf(s_sk.heading); d[1] = -cosf(s_sk.heading);
    e = atan2f((d[0] * right[0] + d[1] * right[2]) / nr, (d[0] * fwd[0] + d[1] * fwd[2]) / nf);
    e *= clampf(MEMF(GA_DT) * 2.5f, 0.0f, 1.0f) * clampf(s_sk.speed / 300.0f, 0.0f, 1.0f);
    MEMF(cam + 0x128) += e;
    MEMF(cam + 0x130) += e;
}

/* A wider view the faster he rolls (the FOV setter, 0x12CA70). */
float skate_fov(float fx)
{
    if (!s_sk.live)
        return fx;
    return fx * (1.0f + 0.14f * clampf(s_sk.speed / 1000.0f, 0.0f, 1.2f)
                 + 0.05f * clampf(s_sk.lift / 120.0f, 0.0f, 1.0f) - 0.05f * s_sk.land_kick);
}

/* -- The board in the world ---------------------------------------------------- */

static void mat_mul(const float a[16], const float b[16], float o[16])
{
    float r[16];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            r[i * 4 + j] = 0.0f;
            for (k = 0; k < 4; k++)
                r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        }
    memcpy(o, r, sizeof r);
}

static void mat_rot(float m[16], int axis, float a)
{
    float c = cosf(a), s = sinf(a);
    int u = (axis + 1) % 3, v = (axis + 2) % 3;
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[u * 4 + u] = c; m[u * 4 + v] = s;
    m[v * 4 + u] = -s; m[v * 4 + v] = c;
}

/* Board space: X along the board (nose +X), Y up, Z across; inches. Row
 * vectors, D3D style: scale, the trick's turn, the heading, the position. */
static void board_matrix(void)
{
    float m[16], r[16], f[3] = { sinf(s_sk.heading), 0.0f, -cosf(s_sk.heading) };
    float flip = 0.0f, spin = 0.0f, off[3] = { 0, 0, 0 }, tumble = 0.0f, pitch;
    int i;

    if (s_ck.have_board && s_sk.bail_t <= 0.0f) {   /* where Skate 3's clip has it */
        memcpy(s_sk.board_world, s_ck.board, sizeof s_ck.board);
        return;
    }
    if (s_sk.air && s_sk.trick >= 0) {
        float u = clampf(s_sk.trick_t / TRICK_TIME, 0.0f, 1.0f);
        u = u * u * (3.0f - 2.0f * u);
        flip = s_tricks[s_sk.trick].roll * 2.0f * PI * u;
        spin = s_tricks[s_sk.trick].yaw * 2.0f * PI * u;
    }
    if (s_sk.bail_t > 0.0f) {           /* the board shoots off and tumbles, then he fetches it */
        float t = BAIL_TIME - s_sk.bail_t, back = clampf(s_sk.bail_t / GETUP_TIME, 0.0f, 1.0f);
        for (i = 0; i < 3; i++)
            off[i] = s_sk.bail_dir[i] * 260.0f * (1.0f - expf(-t * 2.5f)) * back;
        off[1] = fmaxf(0.0f, 60.0f * t - 90.0f * t * t);
        tumble = fminf(t, 0.9f) * 9.0f * back;
        spin = fminf(t, 0.9f) * 4.0f * back;
    }
    /* a manual: up on the back wheels (nose up), or the front ones */
    pitch = 0.17f * s_sk.man_tilt;
    off[1] += 7.0f * fabsf(sinf(pitch)) * BOARD_UNIT;
    memset(m, 0, sizeof m);
    m[0] = m[5] = m[10] = s_ck.ok ? BOARD_MODEL_INCH * 0.88f : BOARD_UNIT;
    m[15] = 1.0f;
    mat_rot(r, 0, flip + tumble); mat_mul(m, r, m);
    mat_rot(r, 2, pitch);         mat_mul(m, r, m);
    mat_rot(r, 1, spin);          mat_mul(m, r, m);
    /* heading: board X -> f, Y -> up, Z -> f x up (to the board's right) */
    memset(r, 0, sizeof r);
    r[0] = f[0]; r[1] = 0.0f; r[2] = f[2];
    r[5] = 1.0f;
    r[8] = -f[2]; r[9] = 0.0f; r[10] = f[0];
    r[15] = 1.0f;
    mat_mul(m, r, m);
    m[12] += s_sk.pos[0] + off[0];
    m[13] += s_sk.pos[1] + off[1] + s_sk.lift * (s_sk.air ? 1.05f : 0.0f);
    m[14] += s_sk.pos[2] + off[2];
    memcpy(s_sk.board_world, m, sizeof m);
}

/* Off the board: in his right hand at his side, nose forward, grip tape to
 * his leg, the hand on its top edge (King model space: -Z ahead, +X his
 * left, Y up). */
static int carry_matrix(void)
{
    uint32_t P = s_pose_P, bones, n, i;
    float bm[16], model[16], s = s_ck.ok ? BOARD_MODEL_INCH : BOARD_UNIT / 0.88f;
    if (!P)
        return 0;
    bones = MEM32(P + 0x1A4);
    n = MEM8(MEM32(P + 0x1A0) + 0xF1);
    for (i = 0; i < n; i++) {
        uint32_t b = bones + 0x180 * i, def = MEM32(b + 0x174);
        if (!def || MEM32(def + 0xC0) != KB_RWRIST)
            continue;
        memset(bm, 0, sizeof bm);
        bm[2] = -s;                     /* nose -> ahead */
        bm[4] = s;                      /* deck up -> his left */
        bm[9] = -s;                     /* across -> down */
        bm[12] = MEMF(b + 0x70 + 48) - (DECK_TOP + 1.0f) * s;   /* the deck just outside his hand */
        bm[13] = MEMF(b + 0x70 + 52) - 4.0f * s;                /* half the board's width below it */
        bm[14] = MEMF(b + 0x70 + 56);
        bm[15] = 1.0f;
        for (i = 0; i < 16; i++)
            model[i] = MEMF(P + 4 * i);
        mat_mul(bm, model, s_sk.board_world);
        if (getenv("RECOMP_SKATE_DEBUG")) {
            static DWORD last;
            if (GetTickCount() - last > 400) {
                last = GetTickCount();
                fprintf(stderr, "[SKATE] carry at %.0f %.0f %.0f wrist %.1f %.1f %.1f\n", s_sk.board_world[12],
                        s_sk.board_world[13], s_sk.board_world[14], bm[12], bm[13], bm[14]);
            }
        }
        return 1;
    }
    return 0;
}

/* When the game leaves 3D for its HUD (0x122C50), once a game frame. */
void skate_scene(void)
{
    static uint32_t drawn;
    float view[16], proj[16];
    int i, carry = s_sk.on && s_sk.walk && game_in_play() && !game_is_paused();
    if (!(skate_live() || carry) || MEM32(GA_CLOCK) == drawn)
        return;
    if (!carry)
        board_matrix();
    else if (!carry_matrix())
        return;
    drawn = MEM32(GA_CLOCK);
    for (i = 0; i < 16; i++) {
        view[i] = MEMF(GA_VIEW + 4 * i);
        proj[i] = MEMF(GA_PROJ + 4 * i);
    }
    proj[14] -= MEMF(GA_ZOFS);
    board_world_submit(view, proj, s_sk.board_world);
}

void skate_hud(SkateHud *h)
{
    memset(h, 0, sizeof *h);
    h->on = s_sk.on;
    h->live = s_sk.live;
    h->walk = s_sk.walk;
    h->speed = s_sk.speed;
    h->score = s_sk.score;
    h->tricks = s_sk.tricks_done;
    h->show_t = s_sk.show_t;
    h->stick[0] = s_sk.stick[0];
    h->stick[1] = s_sk.stick[1];
    h->air = s_sk.air;
    h->bail = s_sk.bail_t > 0.0f;
    h->manual = s_sk.manual;
    h->combo_n = s_sk.combo_n;
    h->combo_pts = (int)s_sk.combo_pts;
    snprintf(h->combo_line, sizeof h->combo_line, "%s", s_sk.combo_line);
    h->balance = s_sk.bal;
    snprintf(h->shown, sizeof h->shown, "%s", s_sk.shown);
}

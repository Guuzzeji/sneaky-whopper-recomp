/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>   /* atof, rand */
#include <string.h>   /* memcpy */
#include <math.h>     /* atan, tan */
#include <windows.h>  /* GetTickCount */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
/* recomp_types.h declares them, plus recomp_func_t and the register file.
 * It declares the registers RECOMP_TLS: a plain `extern uint32_t g_eax;`
 * would not resolve to the calling thread's copy (see main.c). */
#include "recomp/gen/recomp_types.h"
#include "overlay.h"          /* skate_*, shooter types */

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* CMissionPickup::Update (vtable 0x2B8F70 slot 7, only called through it):
 * when the King's node enters the pickup's box and the "inside" byte +0x13A
 * is clear, it sets the byte and has CMode_Pause (0x11AE0) open the mission
 * offer. While skating the byte is held set, so rolling through a pickup
 * offers nothing; it clears again once he leaves the box on foot.
 * RECOMP_SKATE_MISSIONS=1 lets the offers through. */
extern void sub_00061F50(void);
static void skate_pickup_update(void)
{
    if (skate_live() && !getenv("RECOMP_SKATE_MISSIONS"))
        MEM8(g_ecx + 0x13A) = 1;
    if (getenv("RECOMP_SKATE_DEBUG")) {          /* where the pickups are, once each */
        static uint32_t seen[32];
        int i;
        for (i = 0; i < 32 && seen[i] && seen[i] != g_ecx; i++)
            ;
        if (i < 32 && !seen[i] && MEM32(g_ecx + 0x64)) {
            seen[i] = g_ecx;
            fprintf(stderr, "[SKATE] pickup %08X at %.0f %.0f %.0f\n", g_ecx, MEMF(MEM32(g_ecx + 0x64) + 4),
                    MEMF(MEM32(g_ecx + 0x64) + 8), MEMF(MEM32(g_ecx + 0x64) + 0xC));
        }
    }
    sub_00061F50();
}

static void citizen_idle_update(void);   /* skate mode: the live citizens (below) */
static void citizen_mobile_update(void);
static void citizen_alert_update(void);

/* Skate mode: no invisible walls. A physics controller's contact callback
 * (0x16F650, installed at body+0x258; cdecl space, self body, other body,
 * first contact, count; returning 0 drops the pair). Contacts are 0x100-byte
 * slots chained through +0x8C, the count kept in the first's +0x84, and are
 * freed by walking that chain (0xE8410); the caller (0xE9090) marks them
 * active (+0x80 |= 2) after the callbacks. Geometry is in +0x00..+0x7F:
 * position +0x10, normal +0x20 (toward self when +0x70 is self), surface
 * type of side A at +0x5C and of side B at +0x6C.
 *
 * Surface type 1 is "CameraInvisible" (the names in the King's debug print,
 * 0x1B0E0): collision with nothing drawn. The level's static world is one
 * body, so its invisible walls come in the same chain as the ground. While
 * skating, each invisible wall contact (normal no more than 60 degrees up,
 * the game's own ground test, 0x2B03A0) takes a copy of a solid contact's
 * geometry from its chain, so it holds nothing back; a chain of nothing but
 * invisible walls is dropped. Invisible floors still hold him up, and so do
 * the walls round the level's edge, over nothing (skate_walls_open).
 * RECOMP_SKATE_WALLS=1 logs the contacts it takes out. */
#define SURF_CAMERA_INVISIBLE 1u
extern void sub_0016F650(void);
static void king_contacts(void)
{
    uint32_t self = MEM32(g_esp + 8), other = MEM32(g_esp + 12), first = MEM32(g_esp + 16), owner, c, solid = 0;
    uint32_t walls[16];
    int n = (int)MEM32(g_esp + 20), i, nw = 0;
    static int probe = -1;
    if (probe < 0)
        probe = getenv("RECOMP_SKATE_WALLS") != NULL;
    if (!skate_walls_open() || !self || !other || n <= 0) {
        sub_0016F650();
        return;
    }
    owner = MEM32(self + 0x23C) ? MEM32(MEM32(self + 0x23C) + 4) : MEM32(self + 4);
    if (owner != MEM32(0x2E2EB8)) {
        sub_0016F650();
        return;
    }
    for (i = 0, c = first; i < n && c; i++, c = MEM32(c + 0x8C)) {
        int mine = MEM32(c + 0x70) == self;
        float ny = MEMF(c + 0x24) * (mine ? 1.0f : -1.0f);
        if (MEM32(c + (mine ? 0x6C : 0x5C)) == SURF_CAMERA_INVISIBLE && ny <= MEMF(0x2B03A0)) {
            if (nw < 16)
                walls[nw++] = c;
        } else if (!solid) {
            solid = c;
        }
    }
    if (probe && nw) {
        static DWORD last;
        if (GetTickCount() - last > 250) {
            last = GetTickCount();
            fprintf(stderr, "[WALL] invisible wall at %.0f %.0f %.0f: %d of %d contacts%s\n", MEMF(walls[0] + 0x10),
                    MEMF(walls[0] + 0x14), MEMF(walls[0] + 0x18), nw, n, solid ? "" : ", pair dropped");
        }
    }
    if (nw && !solid) {
        g_eax = 0;
        g_esp += 4;                                   /* ret */
        return;
    }
    for (i = 0; i < nw; i++)
        memcpy((void *)XBOX_PTR(walls[i]), (const void *)XBOX_PTR(solid), 0x80);
    sub_0016F650();
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x0016F650u)
        return king_contacts;
    if (xbox_va == 0x00061F50u)
        return skate_pickup_update;
    if (xbox_va == 0x000237B0u)
        return citizen_idle_update;
    if (xbox_va == 0x00023D20u)
        return citizen_mobile_update;
    if (xbox_va == 0x00023530u)
        return citizen_alert_update;
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Overrides ────────────────────────────────────────────────────────── */

/* XAPI startup kernel patch (no args, plain ret). It rewrites one dword of
 * kernel code at 0x8003xxxx chosen by XboxKrnlVersion, under cli/sti, then
 * tail-jumps to 0x641FE, which installs a GDT descriptor with sgdt + a far
 * jmp (0x6A1B8). There is no guest kernel or GDT here, so neither applies. */
void sub_00064262(void)
{
    g_esp += 4; /* ret */
}

/* ── Mouse camera ─────────────────────────────────────────────────────── */

/* All game input reads an analog control through InputManager::GetAnalog
 * (0x5F480, thiscall (control, filtered, yflag), ret 0xC, float in st0).
 * Controls 2 and 3 are the camera stick. The cameras turn by
 * value/32768 * sensitivity * dt * 180 degrees a second (0x2C370), so
 * returning counts * scale / dt for those two controls turns the camera by
 * a fixed angle per mouse count, with no deadzone or full-throw cap. The
 * game's own Invert Camera and sensitivity settings still apply, because
 * the value goes through its filters (0x5F320, 0x5F3F0).
 *
 * Signs: the stick orbits the camera round the King (stick right moves the
 * camera right, so the view turns left; stick up raises it to look down).
 * A mouse should turn the view itself, so both axes are negated: mouse
 * right looks right, mouse up looks up. RECOMP_MOUSE_DEG (default 0.08) is
 * the degrees per count at the game's middle sensitivity. */
extern void sub_0005F480_gen(void);
extern void sub_0005F290(void);
extern void sub_0005F320(void);
extern void sub_0005F3F0(void);
int mkb_take_mouse_delta(long *dx, long *dy);

static long s_mouse_dx, s_mouse_dy;   /* counts in the current game frame */
static DWORD s_cam_read_ms;           /* last camera-stick read: the King is in play */

/* The game reads the camera stick several times a frame (a test, then the
 * value) and updates at 30 Hz, half the present rate. So the mouse is taken
 * once per game frame: when the game clock (0x32A2A4, advanced each frame
 * by the timer update 0x1A4680) has moved since the last read. */
static void mouse_frame(void)
{
    static uint32_t last_clock;
    static int trace = -1;
    uint32_t clock = MEM32(0x32A2A4);
    if (clock == last_clock)
        return;
    last_clock = clock;
    if (skate_mouse_tricks()) {       /* in the air the mouse flicks tricks (skate.c) */
        s_mouse_dx = s_mouse_dy = 0;
        return;
    }
    if (!mkb_take_mouse_delta(&s_mouse_dx, &s_mouse_dy))
        s_mouse_dx = s_mouse_dy = 0;
    if (trace < 0)
        trace = getenv("RECOMP_MOUSE_TRACE") != NULL;
    if (trace && (s_mouse_dx || s_mouse_dy))
        fprintf(stderr, "[MOUSE] frame %ld %ld clock=%f dt=%f\n", s_mouse_dx, s_mouse_dy,
                MEMF(0x32A2A4), MEMF(0x32A29C));
}

/* The King is under control: the camera stick was read just now. Menus,
 * loading and the title screens do not read it, so the pointer is free
 * there (d3d9_hle.c present). */
int game_in_play(void)
{
    return GetTickCount() - s_cam_read_ms < 500;
}

static float mouse_fp_pop(void)
{
    float v = (float)g_fp_stack[g_fp_top];
    g_fp_top = (g_fp_top + 1u) & 7u;
    return v;
}

static int shooter_owns_pitch(void);  /* shooter mode pitches the camera itself */

void sub_0005F480(void)
{
    static double scale = -1.0;
    uint32_t self = g_ecx, ctl = MEM32(g_esp + 4), filt = MEM32(g_esp + 8);
    uint32_t yflag = MEM32(g_esp + 12), phys, bits;
    long d;
    float dt = MEMF(0x32A29C), v;      /* the game's frame time, seconds */

    if (ctl == 2 || ctl == 3) {
        s_cam_read_ms = GetTickCount();
        mouse_frame();
    }
    d = ctl == 2 ? s_mouse_dx : ctl == 3 && !shooter_owns_pitch() ? s_mouse_dy : 0;

    if (scale < 0.0) {
        const char *e = getenv("RECOMP_MOUSE_DEG");
        double deg = e ? atof(e) : 0.08;
        scale = (deg > 0.0 ? deg : 0.08) * 32768.0 / 180.0;
    }
    if (!d || !(dt > 0.0f)) {
        sub_0005F480_gen();
        return;
    }
    g_esp += 16;                                   /* ret 0xC */
    v = (float)(-d * scale / dt);
    if (!filt) {
        g_fp_top = (g_fp_top + 7u) & 7u;
        g_fp_stack[g_fp_top] = v;
        return;
    }
    PUSH32(g_esp, ctl);                            /* remap for swapped sticks */
    PUSH32(g_esp, 0x0005F4A2u);
    sub_0005F290();
    phys = g_eax;
    memcpy(&bits, &v, 4);
    PUSH32(g_esp, yflag);                          /* invert */
    PUSH32(g_esp, bits);
    PUSH32(g_esp, phys);
    PUSH32(g_esp, 0x0005F563u);
    g_ecx = self;
    sub_0005F320();
    v = mouse_fp_pop();
    memcpy(&bits, &v, 4);
    PUSH32(g_esp, bits);                           /* sensitivity; st0 = result */
    PUSH32(g_esp, phys);
    PUSH32(g_esp, 0x0005F574u);
    g_ecx = self;
    sub_0005F3F0();
}

/* ── Widescreen (RECOMP_WIDESCREEN, on by default) ────────────────────── */

/* The title is 4:3 only: XGetVideoFlags (0x1BD963) is read for 480p and
 * nothing else. 16:9 is done the way a widescreen TV did it, anamorphic:
 * the guest still draws 640x480, the 3D squeezed to 3/4 width, and the
 * present stretches it back out to 16:9 (d3d9_hle.c).
 *
 * 3D: the camera's FOV setter (0x12CA70, cdecl (fov_x, fov_y); fov_y equal
 * to the sentinel at 0x2B039C means "from fov_x and the 4:3 aspect at
 * 0x30FC98") is given a horizontal FOV whose tangent is 4/3 larger and the
 * vertical FOV the game would have derived itself: hor+. The projection
 * (0x122460) and the culling frustum read the same cot/sin/cos it writes,
 * so the sides are not culled. The stored fov_x (0x30F968) is put back to
 * the 4:3 value, because callers read it to restore a camera later.
 *
 * 2D: the HUD and menus are drawn through the ortho at 0x30FB70 (built by
 * 0x1223A0, M11 = 1/half-width); its x is squeezed by 3/4 so they come
 * out 4:3 and centred. Pixel-space draws that skip it (the movies) stretch. */
#define WIDE_X  (4.0 / 3.0)       /* 16:9 over 4:3 */

extern void sub_0012CA70_gen(void);
extern void sub_001223A0_gen(void);

int game_widescreen(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_WIDESCREEN");
        on = e && *e && *e != '0';
    }
    return on;
}

static float shooter_fov(float fx);

/* Also the shooter's ADS zoom: the game sets the FOV every frame. */
void sub_0012CA70(void)
{
    float fx = MEMF(g_esp + 4), fy = MEMF(g_esp + 8), k = MEMF(0x2B03A0);
    float zx = skate_fov(shooter_fov(fx));

    if (zx != fx) {
        if (fy != MEMF(0x2B039C))
            fy = (float)(atan(tan(fy * k) * tan(zx * k) / tan(fx * k)) / k);
        MEMF(g_esp + 4) = zx;
        MEMF(g_esp + 8) = fy;
    }
    if (!game_widescreen()) {
        sub_0012CA70_gen();
        MEMF(0x30F968) = fx;
        return;
    }
    if (fy == MEMF(0x2B039C))
        fy = (float)(2.0 * atan(tan(zx * k) / MEMF(0x30FC98)));
    MEMF(g_esp + 4) = (float)(atan(tan(zx * k) * WIDE_X) / k);
    MEMF(g_esp + 8) = fy;
    sub_0012CA70_gen();
    MEMF(0x30F968) = fx;
}

void sub_001223A0(void)
{
    sub_001223A0_gen();
    if (game_widescreen())
        MEMF(0x30FB70) *= (float)(1.0 / WIDE_X);
}

/* ── Debug overlay: game state and cheats (src/overlay.cpp) ───────────── */

#include "overlay.h"

/* Globals found with REA (docs/ROADMAP.md, P8). */
#define GA_GAMESTATE  0x2E2CCCu   /* GameState*: unlocks, missions, options  */
#define GA_PAUSE      0x2E2CC0u   /* CMode_Pause*                            */
#define GA_WORLD      0x2E2CD0u   /* CMode_World*                            */
#define GA_FRONTEND   0x2E2CD4u   /* the front end's menu owner              */
#define GA_LEVELS     0x2E2FFCu   /* g_levels[5], filled by RegisterLevel    */

extern void sub_00012710(void);   /* World::RequestLevel(level), thiscall, ret 4 */
extern void sub_00061A90(void);   /* GameState: unlock everything (unused dev code) */

/* The overlay runs inside hle_Swap, in the middle of the guest's call: a call
 * into lifted code from there must leave every guest register as it was. */
typedef struct { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; int fp; } GuestRegs;

static void regs_save(GuestRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx;
    r->esp = g_esp; r->ebp = g_ebp; r->esi = g_esi; r->edi = g_edi;
    r->fp = g_fp_top;
}

static void regs_restore(const GuestRegs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx;
    g_esp = r->esp; g_ebp = r->ebp; g_esi = r->esi; g_edi = r->edi;
    g_fp_top = r->fp;
}

static int game_paused(void)
{
    uint32_t p = MEM32(GA_PAUSE);
    return p && MEM8(p + 0x1A69);
}

int game_is_paused(void) { return game_paused(); }

void game_info(OverlayGameInfo *gi)
{
    uint32_t gs = MEM32(GA_GAMESTATE), w = MEM32(GA_WORLD);
    int i;

    memset(gi, 0, sizeof *gi);
    gi->gamestate = gs;
    if (gs) {
        gi->flags = MEM32(gs + 0xA4);
        for (i = 0; i < 4; i++)
            gi->missions[i] = MEM32(gs + 0x20 + i * 0x20);
    }
    gi->world = w;
    if (w) {
        gi->world_state = MEM32(w + 0x1A68);
        gi->level = MEM32(w + 0x1A64);
    }
    for (i = 0; i < OVERLAY_LEVELS; i++) {
        gi->levels[i] = MEM32(GA_LEVELS + 4 * i);
        gi->level_type[i] = gi->levels[i] && MEM32(gi->levels[i] + 0xD0)
                         ? MEM32(MEM32(gi->levels[i] + 0xD0) + 0x50) : 0xFFFFFFFFu;
    }
    gi->paused = game_paused();
    gi->in_play = GetTickCount() - s_cam_read_ms < 500;
}

void game_cheat_unlock_levels(void)
{
    uint32_t gs = MEM32(GA_GAMESTATE);
    if (gs)
        MEM32(gs + 0xA4) |= 0xF;
}

void game_cheat_unlock_all(void)
{
    GuestRegs r;
    uint32_t gs = MEM32(GA_GAMESTATE);
    if (!gs)
        return;
    regs_save(&r);
    PUSH32(g_esp, 0x00061AFDu);
    g_ecx = gs;
    sub_00061A90();
    regs_restore(&r);
}

/* The same request TeleportToLevel (0x28490) makes, without closing a pause
 * menu first: the overlay offers it only during play. */
void game_cheat_warp(int slot)
{
    GuestRegs r;
    uint32_t w = MEM32(GA_WORLD), lvl;
    if (slot < 0 || slot >= OVERLAY_LEVELS || !w || game_paused()
        || GetTickCount() - s_cam_read_ms >= 500)
        return;
    lvl = MEM32(GA_LEVELS + 4 * slot);
    if (!lvl)
        return;
    regs_save(&r);
    PUSH32(g_esp, lvl);
    PUSH32(g_esp, 0x000284C7u);
    g_ecx = w;
    sub_00012710();
    regs_restore(&r);
}

/* ── Mouse over the game's menus ──────────────────────────────────────── */

/* A menu page (MenuPage, ctor 0x29240) is a scene node whose children are
 * MenuOptions (ctor 0x29010). An option has no position: what is drawn is
 * its 2D-element children (node type 0x15, ctor 0x184100), each with a box
 * (W, H, anchor) and a world matrix. The screen rect below is the engine's
 * own, from its scissor code (0x16ABA0). Hovering an option focuses it the
 * way the d-pad does (0x282A0, which also plays the move sound); the click
 * is already bound to A in mkb.ini. */
extern void sub_000282A0(void);   /* focus item on the active page, cdecl */

#define NODE_CHILD(n)   MEM32((n) + 0x38)
#define NODE_NEXT(n)    MEM32((n) + 0x2C)
#define NODE_TYPE(n)    MEM8((n) + 0x94)

typedef struct { float l, t, r, b; int any; } MenuRect;

static int menu_is_option(uint32_t n)
{
    uint32_t vt = MEM32(n);
    return vt == 0x2B20D0u || vt == 0x2B2750u || vt == 0x2B2898u;
}

/* An element's box in the guest's screen pixels. */
static void menu_elem_rect(uint32_t e, MenuRect *u)
{
    uint32_t disp = MEM32(0x32A224);
    float w = MEMF(e + 0x100), h = MEMF(e + 0x104);
    float ax = MEMF(e + 0x108), ay = MEMF(e + 0x10C);
    float x[2], y[2], hw, hh, mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
    int i;

    if (!disp || !(MEM32(e + 0x40) & 0x200) || !(w >= 1.0f) || !(h >= 1.0f))
        return;
    hw = MEMF(disp + 0x7C);
    hh = MEMF(disp + 0x80);
    x[0] = -ax * w; x[1] = (1.0f - ax) * w;
    y[0] = (ay - 1.0f) * h; y[1] = ay * h;
    for (i = 0; i < 4; i++) {
        float cx = x[i & 1], cy = y[i >> 1];
        float X = cx * MEMF(e + 0xC0) + cy * MEMF(e + 0xD0) + MEMF(e + 0xF0);
        float Y = cx * MEMF(e + 0xC4) + cy * MEMF(e + 0xD4) + MEMF(e + 0xF4);
        if (X < mnx) mnx = X;
        if (X > mxx) mxx = X;
        if (Y < mny) mny = Y;
        if (Y > mxy) mxy = Y;
    }
    mnx += hw; mxx += hw;
    { float t = hh - mxy, b = hh - mny; mny = t; mxy = b; }
    if (!u->any || mnx < u->l) u->l = mnx;
    if (!u->any || mxx > u->r) u->r = mxx;
    if (!u->any || mny < u->t) u->t = mny;
    if (!u->any || mxy > u->b) u->b = mxy;
    u->any = 1;
}

/* The union of an option's visible 2D elements, two levels down. */
static void menu_option_rect(uint32_t n, MenuRect *u, int depth)
{
    uint32_t first = NODE_CHILD(n), c = first;
    int guard = 0;
    while (c && guard++ < 64) {
        if (NODE_TYPE(c) == 0x15)
            menu_elem_rect(c, u);
        if (depth < 2)
            menu_option_rect(c, u, depth + 1);
        c = NODE_NEXT(c);
        if (c == first)
            break;
    }
}

/* The option under (x, y) in guest pixels, searching the page's subtree. */
static uint32_t menu_hit(uint32_t n, float x, float y, int depth)
{
    uint32_t first = NODE_CHILD(n), c = first, hit;
    int guard = 0;
    while (c && guard++ < 128) {
        if (menu_is_option(c)) {
            MenuRect r = { 0 };
            menu_option_rect(c, &r, 0);
            if (r.any && x >= r.l && x < r.r && y >= r.t && y < r.b)
                return c;
        } else if (depth < 4 && (hit = menu_hit(c, x, y, depth + 1)) != 0) {
            return hit;
        }
        c = NODE_NEXT(c);
        if (c == first)
            break;
    }
    return 0;
}

static uint32_t menu_active_page(void)
{
    uint32_t p = MEM32(GA_PAUSE), fe = MEM32(GA_FRONTEND), page = 0;
    if (p && MEM8(p + 0x1A69))
        page = MEM32(p + 0x1A70);
    else if (fe)
        page = MEM32(fe + 0xE8);
    return page && MEM8(page + 0x2A4) ? page : 0;
}

int menu_mouse_update(int x, int y)
{
    static int last_x = -1, last_y = -1;
    uint32_t page = menu_active_page(), disp = MEM32(0x32A224), item;
    float gx, gy;
    GuestRegs r;

    if (!page || !disp)
        return 0;
    /* Only a moving pointer takes the focus, so the keys still work while
     * it rests over an option. */
    if (x < 0 || (x == last_x && y == last_y))
        return 1;
    last_x = x;
    last_y = y;
    gx = x * (float)(int32_t)MEM32(disp + 0x5C) / 640.0f;
    gy = y * (float)(int32_t)MEM32(disp + 0x60) / 480.0f;
    item = menu_hit(page, gx, gy, 0);
    if (item && !MEM8(item + 0xE4)) {
        regs_save(&r);
        PUSH32(g_esp, item);
        PUSH32(g_esp, 0x00028B90u);
        sub_000282A0();
        regs_restore(&r);
    }
    return 1;
}

/* ── Shooter mode: CoD4 in third person ───────────────────────────────── */

/* A cheat (F1 > Cheats): the King carries two CoD4 guns from the user's own
 * MW1 install (tools/cod4_extract.py), moves with CoD4's own multiplayer
 * animations retargeted onto him (tools/cod4_anim.py, src/cod4_anim.c), and
 * the camera goes over his shoulder. A shot citizen passes out. Guns handle
 * the way CoD4 computes it, decompiled from iw3mp.exe with REA
 * (docs/MW1_NOTES.md): fire rate, spread, view kick and its recovery, ADS.
 *
 * Sneak King side (REA on default_flat.exe; docs/ROADMAP.md P9):
 *   Bones: the King's actor [King+0xF8], its animation instance [actor+0xB8]
 *     = P. 0x151650(P) (cdecl) builds every bone's matrix from its local
 *     rotation (+0x30, quat, engine convention) and translation (+0x10)
 *     after the game's own animation has set them; bones at [P+0x1A4],
 *     stride 0x180, count byte [[P+0x1A0]+0xF1], palette index at
 *     [[bone+0x174]+0xC0]; bone->model at bone+0x70, model->world at P+0.
 *   King::Update 0x1CD20 writes his node's orientation ([King+0x64]+0x14).
 *   KingCam (0x2E3024): Update 0x2DED0 takes its distance from
 *     settings ([cam+0x98]) +0x5C/+0x60/+0x64; 0x2D800 and 0x2D940 place the
 *     eye and the aim point around the look target cam+0x14C.
 *   The camera: view 0x30F9F0 and projection 0x30FBF0 (row vectors), eye
 *     0x30F8D0. 0x122C50(mode, force) switches 3D (1) and 2D; the current
 *     mode is at 0x30FC9C.
 *   Raycast 0x15FD50: thiscall on [cam+0x58] (start4, delta4, rec, 0, 0,
 *     [[cam+0x98]+0x30]), ret 0x18; rec[0]=0x143, rec[1]=4,
 *     rec[4]=rec+0x1C; hit point rec+0x2C, rec+0x88 == 1 is a miss.
 *   AIManager::Register 0x19140: citizens at +0xFC, count +0x160. A
 *     citizen's position is [cit+0x64]+4, state machine cit+0x1A0, state 4
 *     (cit+0x290) CitizenPassedOut; a state request is SM+0xC = state. */
#include "cod4_anim.h"

#define GA_KING       0x2E2EB8u
#define GA_DT         0x32A29Cu   /* the game's frame time, seconds */
#define GA_KINGCAM    0x2E3024u
#define GA_VIEW       0x30F9F0u
#define GA_PROJ       0x30FBF0u
#define GA_ZOFS       0x30FCA8u   /* per-object depth offset added to PROJ._43 */
#define GA_GFX_MODE   0x30FC9Cu

extern void sub_0002C670_gen(void);
extern void sub_00019140_gen(void);
extern void sub_00151650_gen(void);
extern void sub_0001CD20_gen(void);
extern void sub_0002DED0_gen(void);
extern void sub_0002D800_gen(void);
extern void sub_0002D940_gen(void);
extern void sub_00122C50_gen(void);
extern void sub_0015FD50(void);
int mkb_mouse_buttons(int grab, int *clicked);
void mkb_set_mute(int move, unsigned analog_mask);
void *xbox_FramebufferWindowHwnd(void);

typedef struct {
    char dir[64], name[32];
    float fire_time, reload_time, ads_time, ads_fov, flash[3];
    /* reloads (CoD4's names): the empty one, when the rounds go in; shell by
     * shell (segmented) a start and an end round the loop; the bolt or pump
     * after each shot (rechamber) */
    float reload_empty_time, reload_add_time, reload_start_time, reload_start_add_time, reload_end_time;
    float rechamber_time, rechamber_bolt_time, ads_reload_trans_time;
    int segmented, reload_ammo_add, reload_start_add, bolt_action, burst;
    float hip_min, hip_max, fire_add, decay, ads_spread;
    float kick[2][4], center[2];      /* [hip, ads]: pitch min/max, yaw min/max */
    int clip, autofire, pistol, pellets;
} Weapon;

typedef struct { Weapon w; int ammo; } Slot;

static struct {
    int on, cur, want;                /* slot in hand, slot asked for */
    Slot slot[2];                     /* primary, sidearm */
    int knockouts;
    /* gun handling */
    float clock, next_fire, ads, scale, switch_t;
    /* the reload: its step (RL_*), time left in it and its length, time to
     * the rounds going in (-1 done), stop after this shell; the bolt's sound;
     * the burst's rounds still to fire */
    int rstate, rstop, rwas_empty, burst_left;
    float rtime, rlen, radd, bolt_at;
    float kick[2], kick_vel[2];       /* view kick: pitch, yaw (degrees) */
    float hit, flash;
    /* body */
    float phase, idle_t, fire_t, move, run, crouch, sprint, yaw, last_pos[3];
    int crouch_on, crouched, sprinting;   /* C toggles, Ctrl holds; Shift going forward */
    float pitch;                      /* the camera's, degrees down (shoulder()) */
    float vel[2];                     /* his ground velocity, world x z */
    int have_pos;
    KingPose pose;
    uint32_t pose_clock;
    float gun_world[16], muzzle[3];
    int gun_ok;
    ShooterScene scene;
} s_sh;

static int shooter_owns_pitch(void) { return s_sh.on; }

static float ini_f(const char *txt, const char *key, float def)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof pat, "\n%s=", key);
    p = strstr(txt, pat);
    return p ? (float)atof(p + strlen(pat)) : def;
}

static int ini_has(const char *txt, const char *key, const char *val)
{
    char pat[96];
    const char *p;
    snprintf(pat, sizeof pat, "\n%s=%s", key, val);
    p = strstr(txt, pat);
    return p && (p[strlen(pat)] == '\r' || p[strlen(pat)] == '\n' || !p[strlen(pat)]);
}

/* weapon.ini is the raw CoD4 weapon file as key=value: seconds and degrees
 * (the extractor joins multi-word values with ','). */
static void weapon_load(Weapon *w, const char *gun)
{
    static const char *const set[2] = { "hip", "ads" };
    static const char *const part[4] = { "PitchMin", "PitchMax", "YawMin", "YawMax" };
    char path[MAX_PATH], k[48], txt[16384] = "\n";
    const char *dn;
    size_t n = 0;
    FILE *f;
    int i, j;

    memset(w, 0, sizeof *w);
    snprintf(w->dir, sizeof w->dir, "%s", gun);
    snprintf(path, sizeof path, SHOOTER_GUN_DIR "/%s/weapon.ini", gun);
    if ((f = fopen(path, "rb")) != NULL) {
        n = fread(txt + 1, 1, sizeof txt - 2, f);
        fclose(f);
    }
    txt[n + 1] = 0;
    w->fire_time = ini_f(txt, "fireTime", 0.1f);
    w->reload_time = ini_f(txt, "reloadTime", 2.0f);
    w->reload_empty_time = ini_f(txt, "reloadEmptyTime", w->reload_time);
    w->reload_add_time = ini_f(txt, "reloadAddTime", w->reload_time * 0.6f);
    w->reload_start_time = ini_f(txt, "reloadStartTime", 0.0f);
    w->reload_start_add_time = ini_f(txt, "reloadStartAddTime", 0.0f);
    w->reload_end_time = ini_f(txt, "reloadEndTime", 0.0f);
    w->reload_ammo_add = (int)ini_f(txt, "reloadAmmoAdd", 0.0f);
    w->reload_start_add = (int)ini_f(txt, "reloadStartAdd", 0.0f);
    w->segmented = ini_f(txt, "segmentedReload", 0.0f) != 0.0f;
    w->bolt_action = ini_f(txt, "boltAction", 0.0f) != 0.0f;
    w->rechamber_time = ini_f(txt, "rechamberTime", 0.0f);
    w->rechamber_bolt_time = ini_f(txt, "rechamberBoltTime", 0.0f);
    w->burst = ini_has(txt, "fireType", "3-Round,Burst") ? 3 : ini_has(txt, "fireType", "2-Round,Burst") ? 2
             : ini_has(txt, "fireType", "4-Round,Burst") ? 4 : 0;
    w->ads_time = ini_f(txt, "adsTransInTime", 0.2f);
    w->ads_reload_trans_time = ini_f(txt, "adsReloadTransTime", 0.3f);
    w->ads_fov = ini_f(txt, "adsZoomFov", 50.0f);
    w->hip_min = ini_f(txt, "hipSpreadStandMin", 2.0f);
    w->hip_max = ini_f(txt, "hipSpreadMax", 6.0f);
    w->fire_add = ini_f(txt, "hipSpreadFireAdd", 0.5f);
    w->decay = ini_f(txt, "hipSpreadDecayRate", 4.0f);
    w->ads_spread = ini_f(txt, "adsSpread", 0.0f);
    w->clip = (int)ini_f(txt, "clipSize", 30.0f);
    w->pellets = (int)ini_f(txt, "shotCount", 1.0f);
    w->autofire = ini_has(txt, "fireType", "Full,Auto");
    w->pistol = ini_has(txt, "playerAnimType", "pistol");
    if ((dn = strstr(txt, "\nx_flash=")) != NULL)
        sscanf(dn + 9, "%f,%f,%f", &w->flash[0], &w->flash[1], &w->flash[2]);
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 4; j++) {
            snprintf(k, sizeof k, "%sViewKick%s", set[i], part[j]);
            w->kick[i][j] = ini_f(txt, k, j < 2 ? 30.0f : 0.0f);
        }
        snprintf(k, sizeof k, "%sViewKickCenterSpeed", set[i]);
        w->center[i] = ini_f(txt, k, 1500.0f);
    }
    /* "WEAPON_DESERTEAGLE" -> "DESERTEAGLE": the HUD's name */
    snprintf(w->name, sizeof w->name, "%s", gun);
    if ((dn = strstr(txt, "\ndisplayName=")) != NULL) {
        dn += 13;
        if (!strncmp(dn, "WEAPON_", 7))
            dn += 7;
        for (i = 0; i < (int)sizeof w->name - 1 && dn[i] && dn[i] != '\n' && dn[i] != '\r'; i++)
            w->name[i] = dn[i] == '_' ? ' ' : dn[i];
        w->name[i] = 0;
    }
    if (w->clip < 1) w->clip = 1;
    if (w->pellets < 1) w->pellets = 1;
    if (w->fire_time < 0.03f) w->fire_time = 0.03f;
    fprintf(stderr, "[SHOOTER] %s (%s): clip %d, %.0f rpm %s, reload %.1fs, ads fov %.0f%s\n", gun,
            w->name, w->clip, 60.0f / w->fire_time, w->autofire ? "auto" : "semi", w->reload_time,
            w->ads_fov, n ? "" : " (no weapon.ini: defaults)");
}

/* The loadout and whether the mode is on, kept between runs. */
#define SHOOTER_INI "shooter.ini"

/* CoD4's animations and at least one gun, extracted from the user's own
 * install (tools/cod4_extract.py, tools/cod4_anim.py). Without them there is
 * no shooter mode: a build from source with no CoD4 is plain Sneak King. */
int shooter_available(void)
{
    static int have = -1;
    if (have < 0) {
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA(SHOOTER_GUN_DIR "/*", &fd);
        char path[MAX_PATH];
        int gun = 0;
        if (f != INVALID_HANDLE_VALUE) {
            do {
                snprintf(path, sizeof path, SHOOTER_GUN_DIR "/%s/gun.obj", fd.cFileName);
                gun |= fd.cFileName[0] != '.' && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
            } while (!gun && FindNextFileA(f, &fd));
            FindClose(f);
        }
        have = gun && GetFileAttributesA(CANIM_FILE) != INVALID_FILE_ATTRIBUTES;
        if (!have)
            fprintf(stderr, "[SHOOTER] no CoD4 files (%s, %s): shooter mode is off (docs/MODS.md)\n",
                    SHOOTER_GUN_DIR, CANIM_FILE);
    }
    return have;
}

void game_set_shooter(int on, const char *primary, const char *sidearm)
{
    const char *gun[2] = { primary, sidearm };
    int i, was = s_sh.on;
    char old[2][64];
    FILE *f;

    for (i = 0; i < 2; i++)
        snprintf(old[i], sizeof old[i], "%s", s_sh.slot[i].w.dir);
    for (i = 0; i < 2; i++) {
        if (gun[i] && *gun[i] && strcmp(gun[i], s_sh.slot[i].w.dir) != 0) {
            weapon_load(&s_sh.slot[i].w, gun[i]);
            s_sh.slot[i].ammo = s_sh.slot[i].w.clip;
            if (i == s_sh.cur)
                s_sh.rstate = 0;
        }
    }
    if (on && !s_sh.on && !canim_load(CANIM_FILE))
        fprintf(stderr, "[SHOOTER] no %s: run tools/cod4_anim.py\n", CANIM_FILE);
    s_sh.on = on && s_sh.slot[0].w.dir[0] && shooter_available();
    if (s_sh.on && !was)
        s_sh.pitch = 8.0f;            /* the game's resting pitch */
    if (s_sh.on)
        skate_set(0);
    if (!s_sh.on) {
        mkb_mouse_buttons(0, NULL);
        if (was) {                    /* hand his legs back to the game */
            uint32_t king = MEM32(GA_KING), body = king ? MEM32(king + 0x190) : 0;
            mkb_set_mute(0, 0);
            if (body)
                MEMF(body + 0x30) = MEMF(body + 0x38) = 0.0f;
            s_sh.vel[0] = s_sh.vel[1] = 0.0f;
        }
    }
    if ((was != s_sh.on || strcmp(old[0], s_sh.slot[0].w.dir) || strcmp(old[1], s_sh.slot[1].w.dir))
        && (f = fopen(SHOOTER_INI, "w")) != NULL) {
        fprintf(f, "on=%d\nprimary=%s\nsidearm=%s\n", s_sh.on, s_sh.slot[0].w.dir, s_sh.slot[1].w.dir);
        fclose(f);
    }
}

/* At the first bone build: RECOMP_SHOOTER=<primary>[,<sidearm>] turns the
 * mode on; otherwise the last run's loadout comes back. */
static void shooter_startup(void)
{
    const char *e = getenv("RECOMP_SHOOTER");
    char a[64] = "", b[64] = "", line[160];
    int on = 1;
    FILE *f;

    if (e && *e) {
        sscanf(e, "%63[^,],%63s", a, b);
    } else if ((f = fopen(SHOOTER_INI, "r")) != NULL) {
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "on=%d", &on);
            sscanf(line, "primary=%63s", a);
            sscanf(line, "sidearm=%63s", b);
        }
        fclose(f);
        if (skate_on())               /* RECOMP_SKATE wins over a saved "on" */
            on = 0;
    }
    if (a[0])
        game_set_shooter(on, a, b);
}

static int shooter_live(void)
{
    return s_sh.on && game_in_play() && !game_paused();
}

static const Weapon *cur_weapon(void) { return &s_sh.slot[s_sh.cur].w; }

static float cur_spread(void)
{
    const Weapon *w = cur_weapon();
    float sp = w->hip_min + (w->hip_max - w->hip_min) * s_sh.scale / 255.0f;
    return sp + (w->ads_spread - sp) * s_sh.ads;
}

static float reload_frac(const Slot *sl);

void shooter_hud(ShooterHud *h)
{
    const Weapon *w = cur_weapon();
    memset(h, 0, sizeof *h);
    h->on = s_sh.on;
    h->live = shooter_live() && s_sh.gun_ok;
    snprintf(h->name, sizeof h->name, "%s", w->name);
    snprintf(h->primary, sizeof h->primary, "%s", s_sh.slot[0].w.dir);
    snprintf(h->sidearm, sizeof h->sidearm, "%s", s_sh.slot[1].w.dir);
    h->ammo = s_sh.slot[s_sh.cur].ammo;
    h->clip = w->clip;
    h->reload = reload_frac(&s_sh.slot[s_sh.cur]);
    h->ads = s_sh.ads;
    h->spread = cur_spread();
    h->fov = shooter_fov(MEMF(0x30F968)) * 57.29578f;   /* stored in radians */
    h->hit = s_sh.hit;
    h->knockouts = s_sh.knockouts;
    h->sprint = s_sh.sprint;
}

/* AIManager::Register. Its list is not the walking citizens (see
 * citizen_live), so nothing is taken from it; the override stays only
 * because the generated code was built without this function. */
void sub_00019140(void)
{
    sub_00019140_gen();
}

static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static int imin(int a, int b) { return a < b ? a : b; }
static float approach(float v, float to, float rate) { return v + (to - v) * clampf(rate, 0.0f, 1.0f); }

/* The camera's axes in the world: rows of the view matrix's 3x3 columns. */
static void cam_axes(float right[3], float up[3], float fwd[3])
{
    int i;
    for (i = 0; i < 3; i++) {
        right[i] = MEMF(GA_VIEW + 4 * (4 * i + 0));
        up[i] = MEMF(GA_VIEW + 4 * (4 * i + 1));
        fwd[i] = MEMF(GA_VIEW + 4 * (4 * i + 2));
    }
}

/* The eye of the view the frame is drawn with (the crosshair is its centre):
 * a row-vector view matrix is [R 0; t 1] with t = -eye R, so eye = -t R^T.
 * 0x30F8D0 (the game's eye) is not it: it lags, and the King's crouch camera places it apart. */
static void cam_eye(float eye[3])
{
    int i;
    for (i = 0; i < 3; i++)
        eye[i] = -(MEMF(GA_VIEW + 48) * MEMF(GA_VIEW + 4 * (4 * i + 0))
                   + MEMF(GA_VIEW + 52) * MEMF(GA_VIEW + 4 * (4 * i + 1))
                   + MEMF(GA_VIEW + 56) * MEMF(GA_VIEW + 4 * (4 * i + 2)));
}

/* The game's own collision ray, cast through KingCam as its occlusion test
 * (0x2DB10) does: the fraction of from..to before a hit, 1 when nothing is
 * in the way. Guest code runs; the registers are kept for the caller. */
static float guest_raycast(const float from[3], const float to[3], float hit[3])
{
    GuestRegs r;
    uint32_t node = MEM32(GA_KINGCAM), world, base, rec, i;
    float frac = 1.0f;

    if (!node || !(world = MEM32(node + 0x58)) || !MEM32(node + 0x98))
        return 1.0f;
    regs_save(&r);
    base = g_esp = (g_esp - 0x400) & ~15u;
    rec = base + 0x20;
    for (i = 0; i < 0x300; i += 4)
        MEM32(rec + i) = 0;
    MEM32(rec) = 0x143;
    MEM32(rec + 4) = 4;
    MEM32(rec + 0x10) = rec + 0x1C;
    for (i = 0; i < 3; i++) {
        MEMF(base + 4 * i) = from[i];
        MEMF(base + 0x10 + 4 * i) = to[i] - from[i];
    }
    MEMF(base + 0xC) = 1.0f;
    MEMF(base + 0x1C) = 1.0f;
    PUSH32(g_esp, MEM32(MEM32(node + 0x98) + 0x30));
    PUSH32(g_esp, 0);
    PUSH32(g_esp, 0);
    PUSH32(g_esp, rec);
    PUSH32(g_esp, base + 0x10);
    PUSH32(g_esp, base);
    PUSH32(g_esp, 0x0002DC57u);
    g_ecx = world;
    sub_0015FD50();
    if (g_eax && MEM32(rec + 0x88) != 1) {
        float d2 = 0.0f, h2 = 0.0f;
        for (i = 0; i < 3; i++) {
            float d = to[i] - from[i], h = MEMF(rec + 0x2C + 4 * i) - from[i];
            d2 += d * d;
            h2 += h * h;
            hit[i] = MEMF(rec + 0x2C + 4 * i);
        }
        frac = d2 > 0.0f ? sqrtf(h2 / d2) : 0.0f;
    }
    regs_restore(&r);
    return frac;
}

static uint32_t citizen_live(int i);
#define CITIZENS_LIVE 32

/* The citizen the ray hits first, before `limit` (0..1 of from..to), or 0.
 * Citizens are upright cylinders a little taller than their own scale of
 * the King. */
static uint32_t ray_citizen(const float from[3], const float to[3], float limit, float *at)
{
    uint32_t best = 0, i;
    float d[3], len2, best_t = limit, h = 190.0f, rad = 32.0f;

    for (i = 0; i < 3; i++)
        d[i] = to[i] - from[i];
    len2 = d[0] * d[0] + d[2] * d[2];
    for (i = 0; i < CITIZENS_LIVE; i++) {
        uint32_t c = citizen_live(i), node;
        float p[3], t, x, y, z;
        if (!c || !(node = MEM32(c + 0x64)))
            continue;
        p[0] = MEMF(node + 4); p[1] = MEMF(node + 8); p[2] = MEMF(node + 0xC);
        /* closest approach in the ground plane, then the height there */
        t = len2 > 0.0f ? ((p[0] - from[0]) * d[0] + (p[2] - from[2]) * d[2]) / len2 : 0.0f;
        if (t < 0.0f || t >= best_t)
            continue;
        x = from[0] + d[0] * t - p[0];
        y = from[1] + d[1] * t - p[1];
        z = from[2] + d[2] * t - p[2];
        if (x * x + z * z <= rad * rad && y >= 0.0f && y <= h) {
            best = c;
            best_t = t;
        }
    }
    *at = best_t;
    return best;
}

/* The citizen's state machine (a pointer at +0x1A0, as the King's): +4 the
 * current state, +0xC the one asked for next; +0x290 is his
 * CitizenPassedOut state (id 4). */
static void knock_out(uint32_t cit)
{
    uint32_t sm = MEM32(cit + 0x1A0), cur = sm ? MEM32(sm + 4) : 0;
    if (!sm || (cur && MEM32(cur + 8) == 4))
        return;
    MEM32(sm + 0xC) = MEM32(cit + 0x290);
    MEM32(sm + 0x10) = 0;
    s_sh.knockouts++;
}

/* Skate mode: how far the ground is below `at`, by the game's own collision
 * ray, up to `range` (range when nothing is hit). */
float game_ground_below(const float at[3], float range)
{
    float from[3] = { at[0], at[1] + 20.0f, at[2] }, to[3] = { at[0], at[1] - range, at[2] }, hit[3];
    float f = guest_raycast(from, to, hit);
    return f >= 1.0f ? range : f * (range + 20.0f) - 20.0f;
}

/* Skate mode: citizens within `radius` of a fast-rolling King jump back.
 * AIManager's registered list (+0xFC) turned out not to be the walking
 * citizens (its entries never get a state, and it keeps the first level's),
 * so the live ones are collected from their own state updates: CitizenIdle
 * (0x237B0), CitizenMobile (0x23D20) and CitizenAudioAlert (0x23530), each
 * called through its state's vtable with the citizen as the argument.
 * The state machine is [cit+0x1A0]: current state at +4 (id at +8), a request
 * at +0xC/+0x10 (what 0x1AB20 writes). CitizenSurprised (state 3, cit+0x28C;
 * Enter 0x24620 plays the reaction and turns them to face the King, Update
 * 0x24510 sends them back to idle once its timer runs out, since the King
 * isn't in KingSurprise). */
static struct { uint32_t cit, clock; } s_live[32];   /* clock: GetTickCount of the last update */

static void live_citizen(void)
{
    uint32_t c = MEM32(g_esp + 4), clock = GetTickCount();
    int i, free_i = -1;
    for (i = 0; i < 32; i++) {
        if (s_live[i].cit == c) {
            s_live[i].clock = clock;
            return;
        }
        if (free_i < 0 && (!s_live[i].cit || clock - s_live[i].clock > 2000))
            free_i = i;
    }
    if (free_i >= 0) {
        s_live[free_i].cit = c;
        s_live[free_i].clock = clock;
    }
}

extern void sub_000237B0(void);
extern void sub_00023D20(void);
extern void sub_00023530(void);
static void citizen_idle_update(void) { live_citizen(); sub_000237B0(); }
static void citizen_mobile_update(void) { live_citizen(); sub_00023D20(); }
static void citizen_alert_update(void) { live_citizen(); sub_00023530(); }

/* Slot i of the live citizens, or 0 when empty or not updated lately. */
static uint32_t citizen_live(int i)
{
    return GetTickCount() - s_live[i].clock <= 300 ? s_live[i].cit : 0;
}

int game_citizens_startle(const float at[3], float radius)
{
    uint32_t clock = GetTickCount();
    int i, k = 0;
    for (i = 0; i < 32; i++) {
        uint32_t c = s_live[i].cit, node, sm, cur;
        float dx, dz;
        if (!c || clock - s_live[i].clock > 300 || !(node = MEM32(c + 0x64)) || !(sm = MEM32(c + 0x1A0))
            || !MEM32(c + 0x28C))
            continue;
        dx = MEMF(node + 4) - at[0];
        dz = MEMF(node + 0xC) - at[2];
        cur = MEM32(sm + 4);
        if (dx * dx + dz * dz > radius * radius || !cur || MEM32(cur + 8) > 2 || MEM32(sm + 0xC))
            continue;
        MEM32(sm + 0xC) = MEM32(c + 0x28C);
        MEM32(sm + 0x10) = 0;
        k++;
    }
    return k;
}

static void add_tracer(const float a[3], const float b[3])
{
    int i, best = 0;
    for (i = 0; i < SHOOTER_TRACERS; i++)
        if (s_sh.scene.tracer[i].life < s_sh.scene.tracer[best].life)
            best = i;
    memcpy(s_sh.scene.tracer[best].a, a, 12);
    memcpy(s_sh.scene.tracer[best].b, b, 12);
    s_sh.scene.tracer[best].life = 0.06f;
}

/* The impact sounds sit with the sprites, SHOOTER_FX_DIR, which snd_play
 * reaches from SHOOTER_GUN_DIR. */
#define SHOOTER_FX_SND "../cod4/fx"

/* Dust where a shot hits the world, pulled a little toward the shooter so
 * the puff isn't half inside the wall. */
static void add_impact(const float at[3], const float from[3])
{
    int i, best = 0;
    float d[3], n;
    for (i = 0; i < SHOOTER_IMPACTS; i++)
        if (s_sh.scene.impact[i].life < s_sh.scene.impact[best].life)
            best = i;
    for (i = 0; i < 3; i++)
        d[i] = from[i] - at[i];
    n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (i = 0; i < 3; i++)
        s_sh.scene.impact[best].p[i] = at[i] + (n > 1.0f ? d[i] / n * 6.0f : 0.0f);
    s_sh.scene.impact[best].life = SHOOTER_IMPACT_LIFE;
}

/* One trigger pull: rays from the camera through the crosshair (third-
 * person CoD aims from the eye), each pellet inside the spread cone. */
static void fire(void)
{
    Slot *sl = &s_sh.slot[s_sh.cur];
    const Weapon *w = &sl->w;
    int ads = s_sh.ads >= 1.0f, p, i;
    float right[3], up[3], fwd[3], eye[3], spread = cur_spread();

    sl->ammo--;
    s_sh.next_fire = s_sh.clock + w->fire_time;
    s_sh.flash = 0.05f;
    s_sh.fire_t = 0.0f;
    snd_play(w->dir, "fire");
    /* the cone widens with each shot unless fully aimed (0x418210) */
    if (!ads && (s_sh.scale += w->fire_add * 255.0f) > 255.0f)
        s_sh.scale = 255.0f;
    /* view kick (0x419410): a random angular velocity, pitch up */
    s_sh.kick_vel[0] += w->kick[ads][0] + (w->kick[ads][1] - w->kick[ads][0]) * frand();
    s_sh.kick_vel[1] += w->kick[ads][2] + (w->kick[ads][3] - w->kick[ads][2]) * frand();

    cam_axes(right, up, fwd);
    cam_eye(eye);
    for (p = 0; p < w->pellets; p++) {
        float a = frand() * 6.2831853f, r = tanf(spread * 0.5f * 0.0174533f) * sqrtf(frand());
        float dir[3], to[3], hit[3], wall, ct;
        uint32_t cit;
        for (i = 0; i < 3; i++) {
            dir[i] = fwd[i] + right[i] * cosf(a) * r + up[i] * sinf(a) * r;
            to[i] = eye[i] + dir[i] * 6000.0f;
        }
        wall = guest_raycast(eye, to, hit);
        cit = ray_citizen(eye, to, wall, &ct);
        if (cit) {
            knock_out(cit);
            s_sh.hit = 0.25f;
            if (p == 0)
                snd_play(SHOOTER_FX_SND, "flesh");
            for (i = 0; i < 3; i++)
                hit[i] = eye[i] + (to[i] - eye[i]) * ct;
        } else if (wall >= 1.0f) {
            memcpy(hit, to, 12);
        } else {
            add_impact(hit, eye);
            if (p == 0 && wall < 0.25f)       /* only the near ones are heard */
                snd_play(SHOOTER_FX_SND, "impact");
        }
        if (s_sh.gun_ok)
            add_tracer(s_sh.muzzle, hit);
        if (getenv("RECOMP_SHOOT_TRACE")) {   /* where the hit lands on screen: 0 0 is the crosshair */
            float v[4], c[4];
            int r, k;
            for (k = 0; k < 4; k++) {
                v[k] = MEMF(GA_VIEW + 48 + 4 * k);
                for (r = 0; r < 3; r++)
                    v[k] += hit[r] * MEMF(GA_VIEW + 4 * (4 * r + k));
            }
            for (k = 0; k < 4; k++) {
                c[k] = 0.0f;
                for (r = 0; r < 4; r++)
                    c[k] += v[r] * MEMF(GA_PROJ + 4 * (4 * r + k));
            }
            fprintf(stderr, "[SHOOTER] eye %.0f %.0f %.0f fwd %.2f %.2f %.2f wall %.3f cit %08X screen %.3f %.3f\n",
                    eye[0], eye[1], eye[2], fwd[0], fwd[1], fwd[2], wall, cit,
                    c[3] ? c[0] / c[3] : 0.0f, c[3] ? c[1] / c[3] : 0.0f);
        }
    }
}

/* View kick recovery (0x44CBF0): in steps of at most 5 ms each angle is
 * pulled back at the centre speed, much more gently while still moving
 * away, and stops at zero; never more than 10 degrees. */
static void kick_update(float dt)
{
    int ads = s_sh.ads > 0.5f, i;
    while (dt > 0.0f) {
        float step = dt > 0.005f ? 0.005f : dt;
        dt -= step;
        for (i = 0; i < 2; i++) {
            float *ang = &s_sh.kick[i], *vel = &s_sh.kick_vel[i], d, n;
            if (*ang == 0.0f && *vel == 0.0f)
                continue;
            if (*ang != 0.0f)
                *vel += cur_weapon()->center[ads] * (*ang > 0.0f ? -1.0f : 1.0f) * step;
            d = *vel * step;
            if (d * *ang < 0.0f)
                d *= 0.06f;
            n = *ang + d;
            if (*ang * n < 0.0f) {
                *ang = *vel = 0.0f;
            } else if (fabsf(n) > 10.0f) {
                *ang = n > 0.0f ? 10.0f : -10.0f;
                *vel = 0.0f;
            } else {
                *ang = n;
            }
        }
    }
}

static int key_edge(int vk, int *was)
{
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0
               && GetForegroundWindow() == (HWND)xbox_FramebufferWindowHwnd();
    int edge = down && !*was;
    *was = down;
    return edge;
}

/* -- Reloads, the bolt, bursts: CoD4's weapon states (docs/MW1_NOTES.md) ----
 * A reload is a run of steps, each a time and, inside it, the moment the
 * rounds go in (the "add"; a step ends once both have passed):
 *   magazine: one step, reloadEmptyTime from empty, else reloadTime; the
 *     magazine goes in at reloadAddTime;
 *   shell by shell (segmentedReload): reloadStartTime (reloadStartAdd at
 *     reloadStartAddTime), then a reloadTime step a shell (reloadAmmoAdd at
 *     reloadAddTime) until full, then reloadEndTime. Fire pressed during it
 *     stops it after the shell in hand (in the start, once 40% is through).
 * Sprinting or switching guns drops a reload; rounds not yet in are lost.
 * Aiming is held off while reloading, until the last adsReloadTransTime of a
 * magazine reload or of the end step. An empty magazine reloads itself as
 * soon as the shot's fireTime is over. A bolt action or pump works its bolt
 * after each shot: fireTime, then rechamberTime, the sound at
 * rechamberBoltTime. Bursts fire their rounds fireTime apart however short
 * the click, then rest 0.2 s (player_burstFireCooldown) and need a new click. */
enum { RL_NONE, RL_MAG, RL_START, RL_SHELL, RL_END };

static const char *const s_rl_snd[5] = { "", "clipout", "start", "loop", "end" };

static void reload_step(const Weapon *w, int st, int ammo)
{
    float t = 0.0f, add = -1.0f;
    switch (st) {
    case RL_MAG:
        t = ammo == 0 ? w->reload_empty_time : w->reload_time;
        add = w->reload_add_time > 0.0f ? fminf(w->reload_add_time, t) : t;
        break;
    case RL_START:
        t = w->reload_start_time;
        if (w->reload_start_add > 0 && w->reload_start_add_time > 0.0f)
            add = fminf(w->reload_start_add_time, t);
        break;
    case RL_SHELL:
        t = ammo == 0 ? w->reload_empty_time : w->reload_time;
        add = w->reload_add_time > 0.0f ? fminf(w->reload_add_time, w->reload_time) : t;
        break;
    case RL_END:
        t = w->reload_end_time;
        break;
    }
    s_sh.rstate = st;
    s_sh.rtime = s_sh.rlen = fmaxf(t, add);
    s_sh.radd = add;
    s_sh.rwas_empty = st == RL_MAG && ammo == 0;
    if (getenv("RECOMP_SHOOT_DEBUG"))
        fprintf(stderr, "[GUN] %.2f reload step %d, %d rounds, %.2fs, rounds in at %.2fs\n", s_sh.clock, st, ammo,
                s_sh.rlen, add);
    if (st == RL_END && !snd_play(w->dir, "end"))
        snd_play(w->dir, "rechamber");              /* the W1200's end is its pump */
    else if (st != RL_END)
        snd_play(w->dir, s_rl_snd[st]);
}

static void reload_begin(const Weapon *w, int ammo)
{
    s_sh.rstop = 0;
    s_sh.bolt_at = 0.0f;                           /* the bolt is worked in the reload */
    if (w->segmented)
        reload_step(w, w->reload_start_time > 0.0f ? RL_START : RL_SHELL, ammo);
    else
        reload_step(w, RL_MAG, ammo);
}

static void reload_cancel(void)
{
    s_sh.rstate = RL_NONE;
}

static void reload_tick(Slot *sl, float dt)
{
    const Weapon *w = &sl->w;
    int needs;
    if (!s_sh.rstate)
        return;
    s_sh.rtime -= dt;
    if (s_sh.radd >= 0.0f && (s_sh.radd -= dt) <= 0.0f) {   /* the rounds go in */
        s_sh.radd = -1.0f;
        if (s_sh.rstate == RL_MAG) {
            sl->ammo = w->clip;
            snd_play(w->dir, "clipin");
        } else if (s_sh.rstate == RL_START) {
            sl->ammo = imin(sl->ammo + w->reload_start_add, w->clip);
        } else if (s_sh.rstate == RL_SHELL) {
            sl->ammo = imin(sl->ammo + (w->reload_ammo_add > 0 ? w->reload_ammo_add : 1), w->clip);
        }
    }
    /* from empty, the bolt goes forward after the magazine is in */
    if (s_sh.rstate == RL_MAG && s_sh.rwas_empty && s_sh.radd < 0.0f && s_sh.rtime <= s_sh.rlen * 0.25f) {
        s_sh.rwas_empty = 0;
        snd_play(w->dir, "chamber");
    }
    if (s_sh.rtime > 0.0f || s_sh.radd >= 0.0f)
        return;
    needs = sl->ammo < w->clip;
    switch (s_sh.rstate) {
    case RL_START:
    case RL_SHELL:
        if ((s_sh.rstop && sl->ammo > 0) || !needs) {
            if (w->reload_end_time > 0.0f)
                reload_step(w, RL_END, sl->ammo);
            else
                reload_cancel();
        } else {
            reload_step(w, RL_SHELL, sl->ammo);
        }
        break;
    default:
        reload_cancel();
    }
}

/* Aiming down the sights is held off while reloading, but for the tail. */
static int reload_blocks_ads(const Weapon *w)
{
    float trans = w->ads_reload_trans_time;
    if (!s_sh.rstate)
        return 0;
    return !((s_sh.rstate == RL_MAG || s_sh.rstate == RL_END) && s_sh.rtime <= trans);
}

/* 0..1 through the reload, for the HUD and the torso clip; a shell-by-shell
 * reload counts its shells. */
static float reload_frac(const Slot *sl)
{
    if (!s_sh.rstate)
        return 0.0f;
    if (s_sh.rstate == RL_MAG)
        return clampf(1.0f - s_sh.rtime / fmaxf(s_sh.rlen, 0.01f), 0.01f, 1.0f);
    return clampf((float)sl->ammo / (float)sl->w.clip, 0.01f, 1.0f);
}

/* Once a game frame: switch, ADS, reload, fire, recoil. */
static void gun_tick(float dt)
{
    static int r_was, k1_was, k2_was;
    Slot *sl = &s_sh.slot[s_sh.cur];
    const Weapon *w = &sl->w;
    int clicked, btn, live = shooter_live(), can_fire, want_fire;

    btn = live ? mkb_mouse_buttons(3, &clicked) : (mkb_mouse_buttons(0, &clicked), 0);
    if (!live)
        clicked = 0;
    s_sh.clock += dt;

    /* weapon switch: 1 / 2, or the wheel's other slot; lowered then raised */
    if (live && s_sh.slot[1].w.dir[0]) {
        if (key_edge('1', &k1_was) && s_sh.cur != 0) s_sh.want = 0;
        if (key_edge('2', &k2_was) && s_sh.cur != 1) s_sh.want = 1;
    }
    if (s_sh.want != s_sh.cur && s_sh.switch_t <= 0.0f) {
        s_sh.switch_t = 0.6f;
        reload_cancel();
        s_sh.burst_left = 0;
        s_sh.bolt_at = 0.0f;
    }
    if (s_sh.switch_t > 0.0f) {
        float before = s_sh.switch_t;
        s_sh.switch_t -= dt;
        if (before > 0.3f && s_sh.switch_t <= 0.3f) {
            s_sh.cur = s_sh.want;
            snd_play(s_sh.slot[s_sh.cur].w.dir, "raise");
        }
        if (s_sh.switch_t < 0.0f)
            s_sh.switch_t = 0.0f;
        sl = &s_sh.slot[s_sh.cur];
        w = &sl->w;
    }
    if (s_sh.sprint > 0.3f) {         /* starting a sprint drops the reload */
        reload_cancel();
        s_sh.burst_left = 0;
    }

    /* sprinting lowers the gun: no ADS, no firing (CoD4) */
    can_fire = live && s_sh.sprint < 0.3f && s_sh.switch_t <= 0.0f;
    s_sh.ads += ((btn & 2) && can_fire && !reload_blocks_ads(w) ? dt : -dt)
                / (w->ads_time > 0.01f ? w->ads_time : 0.01f);
    s_sh.ads = clampf(s_sh.ads, 0.0f, 1.0f);
    if ((s_sh.scale -= w->decay * dt * 255.0f) < 0.0f)        /* 0x417580 */
        s_sh.scale = 0.0f;

    reload_tick(sl, dt);
    if (s_sh.bolt_at > 0.0f && s_sh.clock >= s_sh.bolt_at) {
        s_sh.bolt_at = 0.0f;
        if (!snd_play(w->dir, "rechamber"))
            snd_play(w->dir, "chamber");
    }
    if (live && key_edge('R', &r_was) && sl->ammo < w->clip && !s_sh.rstate && s_sh.switch_t <= 0.0f
        && s_sh.sprint < 0.3f)
        reload_begin(w, sl->ammo);
    /* a fresh click stops a shell-by-shell reload after the shell in hand */
    if (can_fire && (clicked & 1) && (s_sh.rstate == RL_SHELL
                                      || (s_sh.rstate == RL_START && s_sh.rtime < s_sh.rlen * 0.6f)))
        s_sh.rstop = 1;

    want_fire = w->burst ? s_sh.burst_left > 0 || (clicked & 1)
              : w->autofire ? (btn & 1) : (clicked & 1);
    if (can_fire && !s_sh.rstate && s_sh.clock >= s_sh.next_fire && want_fire && sl->ammo > 0) {
        if (w->burst && s_sh.burst_left <= 0)
            s_sh.burst_left = w->burst;
        fire();
        if (getenv("RECOMP_SHOOT_DEBUG"))
            fprintf(stderr, "[GUN] %.2f shot, %d left\n", s_sh.clock, sl->ammo);
        if (w->burst && --s_sh.burst_left <= 0)
            s_sh.next_fire += 0.2f;                 /* player_burstFireCooldown */
        if (w->bolt_action) {                      /* fireTime, then the bolt */
            s_sh.next_fire += w->rechamber_time;
            s_sh.bolt_at = s_sh.clock + w->fire_time
                           + (w->rechamber_bolt_time > 0.0f && w->rechamber_bolt_time < w->rechamber_time
                              ? w->rechamber_bolt_time : 0.0f);
        }
    }
    /* empty: reload as soon as the shot is over */
    if (live && sl->ammo == 0 && !s_sh.rstate && s_sh.clock >= s_sh.next_fire - (w->bolt_action ? w->rechamber_time : 0.0f)
        && s_sh.switch_t <= 0.0f && s_sh.sprint < 0.3f) {
        s_sh.burst_left = 0;
        reload_begin(w, 0);
    }
    kick_update(dt);
    s_sh.flash = s_sh.flash > dt ? s_sh.flash - dt : 0.0f;
    s_sh.hit = s_sh.hit > dt ? s_sh.hit - dt : 0.0f;
    if (s_sh.fire_t >= 0.0f)
        s_sh.fire_t += dt;
}

/* -- The body: CoD4 clips by stance, weapon class and motion -------------- */

typedef struct { int idle, ads, mv[2][4], sprint, shoot, shoot_ads, reload; } ClipSet;
enum { MV_F, MV_B, MV_L, MV_R };
static ClipSet s_set[2][2];           /* [pistol][crouch] */

static void clip_sets(void)
{
    static int done;
    static const char *const dirs[4] = { "forward", "back", "left", "right" };
    static const char *const fmt[2][2][2] = {      /* [pistol][crouch][walk, run] */
        { { "pb_stand_shoot_walk_%s", "pb_combatrun_%s_loop" },
          { "pb_crouch_shoot_run_%s", "pb_crouch_shoot_run_%s" } },
        { { "pb_combatwalk_%s_loop_pistol", "pb_combatrun_%s_loop_pistol" },
          { "pb_crouch_walk_%s_pistol", "pb_crouch_walk_%s_pistol" } } };
    static const char *const one[2][2][6] = {      /* idle, ads, sprint, shoot, shoot ads, reload */
        { { "pb_stand_alert", "pb_stand_ads", "pb_sprint", "pt_stand_shoot", "pt_stand_shoot_ads",
            "pt_reload_stand_auto" },
          { "pb_crouch_alert", "pb_crouch_ads", "pb_sprint", "pt_crouch_shoot", "pt_crouch_shoot_ads",
            "pt_reload_crouch_rifle" } },
        { { "pb_stand_alert_pistol", "pb_stand_ads_pistol", "pb_sprint_pistol", "pt_stand_shoot_pistol",
            "pt_stand_shoot_pistol", "pt_reload_stand_pistol" },
          { "pb_crouch_alert_pistol", "pb_crouch_ads_pistol", "pb_sprint_pistol",
            "pt_crouch_shoot_pistol", "pt_crouch_shoot_pistol", "pt_reload_crouch_pistol" } } };
    char n[64];
    int p, c, g, d;

    if (done)
        return;
    done = 1;
    for (p = 0; p < 2; p++)
        for (c = 0; c < 2; c++) {
            ClipSet *cs = &s_set[p][c];
            for (g = 0; g < 2; g++)
                for (d = 0; d < 4; d++) {
                    snprintf(n, sizeof n, fmt[p][c][g], dirs[d]);
                    cs->mv[g][d] = canim_find(n);
                    if (cs->mv[g][d] < 0 && p)        /* no pistol forward run: the rifle's */
                        cs->mv[g][d] = s_set[0][c].mv[g][d];
                }
            cs->idle = canim_find(one[p][c][0]);
            cs->ads = canim_find(one[p][c][1]);
            cs->sprint = canim_find(one[p][c][2]);
            cs->shoot = canim_find(one[p][c][3]);
            cs->shoot_ads = canim_find(one[p][c][4]);
            cs->reload = canim_find(one[p][c][5]);
        }
}

/* The legs and pelvis of a pose dropped, so it can lie over the walk. */
static void torso_only(KingPose *p)
{
    static const int legs[] = { 75, 76, 77, 78, 79, 80, 81, 82, KB_ORIGIN };
    unsigned i;
    for (i = 0; i < sizeof legs / sizeof legs[0]; i++)
        p->has[legs[i]] = 0;
}

static void qmul(const float a[4], const float b[4], float o[4])
{
    float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                   a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                   a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                   a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
    memcpy(o, r, 16);
}

/* Bend the spine to the camera's pitch, a third at each spine bone, about
 * the model's X (his left-right) axis. Bone quats are in the engine's
 * (conjugate) convention, so the work is done on their conjugates. */
static void aim_pitch(KingPose *p, float deg)
{
    static const int spine[3] = { KB_SPINE1, KB_SPINE2, KB_SPINE3 };
    float s = sinf(deg / 3.0f * 0.5f * 0.0174533f), c = cosf(deg / 3.0f * 0.5f * 0.0174533f);
    float world[4], r[4] = { s, 0.0f, 0.0f, c };
    int i;

    if (!p->has[KB_ORIGIN])
        return;
    world[0] = -p->q[KB_ORIGIN][0]; world[1] = -p->q[KB_ORIGIN][1];
    world[2] = -p->q[KB_ORIGIN][2]; world[3] = p->q[KB_ORIGIN][3];
    for (i = 0; i < 3; i++) {
        float l[4], wi[4] = { -world[0], -world[1], -world[2], world[3] }, t[4];
        float *q = p->q[spine[i]];
        l[0] = -q[0]; l[1] = -q[1]; l[2] = -q[2]; l[3] = q[3];
        /* l' = conj(world) * r * world * l */
        qmul(wi, r, t);
        qmul(t, world, t);
        qmul(t, l, l);
        q[0] = -l[0]; q[1] = -l[1]; q[2] = -l[2]; q[3] = l[3];
        qmul(world, l, world);
    }
}

static void sample_mix(KingPose *acc, int clip, float t, int loop, float w)
{
    KingPose tmp;
    if (clip < 0 || w <= 0.001f)
        return;
    canim_sample(clip, t, loop, &tmp);
    canim_add(acc, &tmp, w);
}

/* The CoD4 pose for this game frame, from the King's motion and the gun. */
static void body_tick(uint32_t king, float dt)
{
    uint32_t kn = MEM32(king + 0x64);
    const Weapon *w = cur_weapon();
    float right[3], up[3], fwd[3], v[2] = { 0, 0 }, speed, vf, vr, wd[4], lsum, loco_speed = 0, loco_len = 0;
    float runs[2], rate, pitch;
    int pistol = w->pistol, crouch, d, g;
    KingPose base, move, top;
    ClipSet *cs;

    clip_sets();
    if (kn) {
        float p[3] = { MEMF(kn + 4), MEMF(kn + 8), MEMF(kn + 0xC) };
        if (s_sh.have_pos) {
            v[0] = (p[0] - s_sh.last_pos[0]) / dt;
            v[1] = (p[2] - s_sh.last_pos[2]) / dt;
        }
        memcpy(s_sh.last_pos, p, 12);
        s_sh.have_pos = 1;
    }
    speed = sqrtf(v[0] * v[0] + v[1] * v[1]);
    if (speed > 2000.0f)              /* a warp or respawn, not a step */
        speed = v[0] = v[1] = 0.0f;
    cam_axes(right, up, fwd);
    {   /* the camera's ground-plane axes */
        float nf = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]), nr = sqrtf(right[0] * right[0] + right[2] * right[2]);
        vf = nf > 0.01f ? (v[0] * fwd[0] + v[1] * fwd[2]) / nf : 0.0f;
        vr = nr > 0.01f ? (v[0] * right[0] + v[1] * right[2]) / nr : 0.0f;
    }
    /* crouch and sprint are shooter_move's, not the King's own states */
    crouch = s_sh.crouched;
    s_sh.crouch = approach(s_sh.crouch, (float)crouch, dt * 8.0f);
    s_sh.move = approach(s_sh.move, speed > 25.0f ? 1.0f : 0.0f, dt * 10.0f);
    s_sh.sprint = approach(s_sh.sprint, s_sh.sprinting && vf > speed * 0.7f && speed > 150.0f ? 1.0f : 0.0f,
                           dt * 6.0f);

    cs = &s_set[pistol][crouch];
    /* direction weights, then walk or run by speed (clip root speeds) */
    wd[MV_F] = vf > 0 ? vf : 0; wd[MV_B] = vf < 0 ? -vf : 0;
    wd[MV_R] = vr > 0 ? vr : 0; wd[MV_L] = vr < 0 ? -vr : 0;
    lsum = wd[0] + wd[1] + wd[2] + wd[3];
    if (lsum < 1e-3f) { wd[MV_F] = 1.0f; lsum = 1.0f; }
    for (g = 0; g < 2; g++) {
        runs[g] = 0.0f;
        for (d = 0; d < 4; d++)
            if (cs->mv[g][d] >= 0)
                runs[g] += wd[d] / lsum * canim_speed(cs->mv[g][d]);
    }
    s_sh.run = approach(s_sh.run, clampf((speed - runs[0]) / (runs[1] - runs[0] + 1e-3f), 0.0f, 1.0f),
                        dt * 6.0f);
    for (g = 0; g < 2; g++) {
        float gw = g ? s_sh.run : 1.0f - s_sh.run;
        for (d = 0; d < 4; d++)
            if (cs->mv[g][d] >= 0) {
                loco_speed += gw * wd[d] / lsum * canim_speed(cs->mv[g][d]);
                loco_len += gw * wd[d] / lsum * canim_length(cs->mv[g][d]);
            }
    }
    /* one phase for every moving clip, advanced at the speed the King
     * really moves over the speed the blend would carry him */
    rate = loco_speed > 1.0f && loco_len > 0.01f ? clampf(speed / loco_speed, 0.6f, 1.6f) / loco_len : 0.0f;
    s_sh.phase = fmodf(s_sh.phase + dt * rate, 1.0f);
    s_sh.idle_t += dt;

    canim_clear(&move);
    for (g = 0; g < 2; g++)
        for (d = 0; d < 4; d++)
            if (cs->mv[g][d] >= 0)
                sample_mix(&move, cs->mv[g][d], s_sh.phase * canim_length(cs->mv[g][d]), 1,
                           (g ? s_sh.run : 1.0f - s_sh.run) * wd[d] / lsum);
    if (cs->sprint >= 0 && s_sh.sprint > 0.0f) {
        KingPose sp;
        canim_clear(&sp);
        sample_mix(&sp, cs->sprint, s_sh.phase * canim_length(cs->sprint), 1, 1.0f);
        canim_finish(&sp);
        canim_finish(&move);
        canim_mix(&move, &sp, s_sh.sprint, 1);
        canim_mix_origin(&move, &sp, s_sh.sprint);
    } else {
        canim_finish(&move);
    }

    canim_clear(&base);
    sample_mix(&base, s_sh.ads > 0.5f ? cs->ads : cs->idle, s_sh.idle_t, 1, 1.0f);
    canim_finish(&base);
    if (move.weight > 0.0f && s_sh.move > 0.0f) {
        canim_mix(&base, &move, s_sh.move, 1);
        canim_mix_origin(&base, &move, s_sh.move);
    }
    /* the ADS torso over the legs: fully when aiming, and partly at all
     * times, since CoD4's alert stance reads as a hunch on the King's robe */
    {
        static float k = -1.0f;
        float tw;
        if (k < 0.0f)
            k = getenv("RECOMP_TPS_IDLE_ADS") ? (float)atof(getenv("RECOMP_TPS_IDLE_ADS")) : 0.6f;
        tw = fmaxf(s_sh.ads, k) * (1.0f - s_sh.sprint);
        if (tw > 0.0f && cs->ads >= 0) {
            canim_sample(cs->ads, s_sh.idle_t, 1, &top);
            torso_only(&top);
            canim_mix(&base, &top, tw, 1);
        }
    }
    /* torso layers: the shot, the reload, the weapon switch */
    if (s_sh.fire_t >= 0.0f) {
        int c = s_sh.ads > 0.5f ? cs->shoot_ads : cs->shoot;
        float len = c >= 0 ? canim_length(c) : 0.0f;
        if (c >= 0 && s_sh.fire_t < len) {
            canim_sample(c, s_sh.fire_t, 0, &top);
            torso_only(&top);
            canim_mix(&base, &top, clampf((len - s_sh.fire_t) / 0.1f, 0.0f, 1.0f), 1);
        } else {
            s_sh.fire_t = -1.0f;
        }
    }
    if (s_sh.rstate && cs->reload >= 0) {
        /* shell by shell: the clip's start, a stretch looped a shell, its end */
        float t = s_sh.rstate == RL_MAG ? reload_frac(&s_sh.slot[s_sh.cur])
                : s_sh.rstate == RL_START ? 0.3f * (1.0f - s_sh.rtime / fmaxf(s_sh.rlen, 0.01f))
                : s_sh.rstate == RL_SHELL ? 0.3f + 0.4f * (1.0f - s_sh.rtime / fmaxf(s_sh.rlen, 0.01f))
                : 0.7f + 0.3f * (1.0f - s_sh.rtime / fmaxf(s_sh.rlen, 0.01f));
        canim_sample(cs->reload, t * canim_length(cs->reload), 0, &top);
        torso_only(&top);
        canim_mix(&base, &top, clampf(fminf(t, 1.0f - t) / 0.08f, 0.0f, 1.0f), 1);
    }
    if (s_sh.switch_t > 0.0f) {       /* dip the gun out of sight and back */
        float k = sinf((0.6f - s_sh.switch_t) / 0.6f * 3.14159f);
        canim_clear(&top);
        sample_mix(&top, cs->reload, canim_length(cs->reload) * 0.3f, 0, 1.0f);
        canim_finish(&top);
        torso_only(&top);
        canim_mix(&base, &top, k * 0.8f, 1);
    }
    /* the spine follows the camera's pitch (CoD has no aim up/down clips) */
    pitch = asinf(clampf(fwd[1], -1.0f, 1.0f)) * 57.29578f;
    aim_pitch(&base, clampf(pitch, -60.0f, 60.0f) * (1.0f - s_sh.sprint));
    s_sh.pose = base;
}

/* InstanceAnim build (cdecl P): after the game evaluated his own animation,
 * the King takes the CoD4 pose; afterwards his right wrist carries the gun. */
void sub_00151650(void)
{
    uint32_t P = MEM32(g_esp + 4), king = MEM32(GA_KING), actor = king ? MEM32(king + 0xF8) : 0;
    uint32_t bones, n, i, clock;
    int mine;
    static int env_done;

    if (!env_done) {
        env_done = 1;
        shooter_startup();
    }
    if (skate_on() && actor && P == MEM32(actor + 0xB8)) {
        skate_pose(P);
        sub_00151650_gen();
        skate_scene();                /* the board, while his camera is the game's (once a frame) */
        return;
    }
    mine = s_sh.on && actor && P == MEM32(actor + 0xB8) && s_sh.slot[0].w.dir[0];
    if (getenv("RECOMP_CIT_DUMP")) {    /* ponytail: temporary skeleton survey */
        static uint32_t seen[64];
        static int nseen;
        int k, known = 0;
        for (k = 0; k < nseen; k++)
            known |= seen[k] == P;
        if (!known && nseen < 64) {
            uint32_t bb = MEM32(P + 0x1A4), nb = MEM8(MEM32(P + 0x1A0) + 0xF1), q;
            seen[nseen++] = P;
            fprintf(stderr, "[CIT] P %08X king %d bones %u\n", P, actor && P == MEM32(actor + 0xB8), nb);
            for (q = 0; q < nb; q++) {
                uint32_t d = MEM32(bb + 0x180 * q + 0x174);
                if (d)
                    fprintf(stderr, "[CIT]   %u pal %u ib %.1f %.1f %.1f\n", q, MEM32(d + 0xC0),
                            MEMF(d + 0xB0), MEMF(d + 0xB4), MEMF(d + 0xB8));
            }
        }
    }
    if (!mine) {
        sub_00151650_gen();
        return;
    }
    clock = MEM32(0x32A2A4);
    if (clock != s_sh.pose_clock) {   /* once a game frame */
        float dt = MEMF(GA_DT);
        if (!(dt > 0.0f) || dt > 0.25f)
            dt = 1.0f / 30.0f;
        s_sh.pose_clock = clock;
        gun_tick(dt);
        body_tick(king, dt);
    }
    bones = MEM32(P + 0x1A4);
    n = MEM8(MEM32(P + 0x1A0) + 0xF1);
    for (i = 0; i < n; i++) {
        uint32_t b = bones + 0x180 * i, def = MEM32(b + 0x174), pal = def ? MEM32(def + 0xC0) : 0xFFFF;
        if (pal >= CANIM_BONES || !s_sh.pose.has[pal])
            continue;
        MEMF(b + 0x30) = s_sh.pose.q[pal][0];
        MEMF(b + 0x34) = s_sh.pose.q[pal][1];
        MEMF(b + 0x38) = s_sh.pose.q[pal][2];
        MEMF(b + 0x3C) = s_sh.pose.q[pal][3];
        if (pal == KB_ORIGIN) {
            MEMF(b + 0x10) = s_sh.pose.origin[0];
            MEMF(b + 0x14) = s_sh.pose.origin[1];
            MEMF(b + 0x18) = s_sh.pose.origin[2];
        }
    }
    sub_00151650_gen();
    /* the gun: grip (King axes) <- CoD4 gun space (X fwd, Y left, Z up,
     * inches, scaled like the body), then the wrist and the model */
    s_sh.gun_ok = 0;
    for (i = 0; i < n; i++) {
        uint32_t b = bones + 0x180 * i, def = MEM32(b + 0x174);
        float wrist[16], model[16], grip[16], gw[16], m[16], x, y, z, qw;
        const float s = 3.08f, *gq = s_sh.pose.grip_q, *gp = s_sh.pose.grip_p;
        int r, c, k;
        if (!def || MEM32(def + 0xC0) != KB_RWRIST)
            continue;
        for (k = 0; k < 16; k++) {
            wrist[k] = MEMF(b + 0x70 + 4 * k);
            model[k] = MEMF(P + 4 * k);
        }
        /* grip as a row matrix: row r is where CoD's axis r goes, through
         * C (the mirror tools/cod4_anim.py uses: X -> -Z, Y -> +X, Z -> +Y),
         * then the grip rotation, scaled like the body */
        x = gq[0]; y = gq[1]; z = gq[2]; qw = gq[3];
        {
            float R[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * qw), 2 * (x * z + y * qw) },
                              { 2 * (x * y + z * qw), 1 - 2 * (x * x + z * z), 2 * (y * z - x * qw) },
                              { 2 * (x * z - y * qw), 2 * (y * z + x * qw), 1 - 2 * (x * x + y * y) } };
            static const float Cm[3][3] = { { 0, 1, 0 }, { 0, 0, 1 }, { -1, 0, 0 } };
            memset(grip, 0, sizeof grip);
            for (r = 0; r < 3; r++)
                for (c = 0; c < 3; c++)
                    for (k = 0; k < 3; k++)
                        grip[r * 4 + c] += R[c][k] * Cm[k][r] * s;
            grip[12] = gp[0]; grip[13] = gp[1]; grip[14] = gp[2]; grip[15] = 1.0f;
        }
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++) {
                m[r * 4 + c] = 0.0f;
                for (k = 0; k < 4; k++)
                    m[r * 4 + c] += grip[r * 4 + k] * wrist[k * 4 + c];
            }
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++) {
                gw[r * 4 + c] = 0.0f;
                for (k = 0; k < 4; k++)
                    gw[r * 4 + c] += m[r * 4 + k] * model[k * 4 + c];
            }
        memcpy(s_sh.gun_world, gw, sizeof gw);
        for (c = 0; c < 3; c++) {
            const float *f = cur_weapon()->flash;
            s_sh.muzzle[c] = f[0] * gw[c] + f[1] * gw[4 + c] + f[2] * gw[8 + c] + gw[12 + c];
        }
        s_sh.gun_ok = 1;
        break;
    }
}

static int key_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0 && GetForegroundWindow() == (HWND)xbox_FramebufferWindowHwnd();
}

/* Movement, a third-person CoD: WASD relative to the camera while he keeps
 * facing the aim, so S backpedals and A / D strafe. The game walks the King
 * by root motion along his facing, which shooter mode holds on the aim, so
 * its stick gets no keys (toolkit mkb_set_mute) and his physics velocity,
 * [King+0x190]+0x30 (as skate.c moves him, with the game's collision), is
 * set here instead. The speeds are the CoD4 clips' own (model units; his
 * world scale is 0.88), so the feet don't slide: the run clips by default,
 * the walk clips while aiming down the sights, the crouch clips crouched
 * (C toggles, Ctrl holds), the sprint clip on Shift going forward. LT and RT
 * are muted too: the game's own crouch and run would change his camera and
 * stance under the CoD pose. */
static void shooter_move(uint32_t king, float dt)
{
    static int c_was;
    uint32_t body = MEM32(king + 0x190);
    float right[3], up[3], fwd[3], nf, nr, f, r, m, spd = 0.0f, want[2] = { 0, 0 }, k;
    int live = shooter_live(), d, g;
    const ClipSet *cs;

    mkb_set_mute(live, live ? 0xC0u : 0u);           /* LT 6, RT 7 */
    if (!live || !body)
        return;
    if (key_edge('C', &c_was))
        s_sh.crouch_on = !s_sh.crouch_on;
    f = (float)(key_down('W') - key_down('S'));
    r = (float)(key_down('D') - key_down('A'));
    s_sh.sprinting = key_down(VK_SHIFT) && f > 0.0f && s_sh.ads < 0.5f;
    if (s_sh.sprinting)
        s_sh.crouch_on = 0;                           /* CoD: sprinting stands him up */
    s_sh.crouched = (s_sh.crouch_on || key_down(VK_CONTROL)) && !s_sh.sprinting;
    clip_sets();
    cs = &s_set[cur_weapon()->pistol][s_sh.crouched];
    m = sqrtf(f * f + r * r);
    if (m > 0.0f) {
        float wd[4];
        f /= m;
        r /= m;
        wd[MV_F] = f > 0 ? f : 0; wd[MV_B] = f < 0 ? -f : 0;
        wd[MV_L] = r < 0 ? -r : 0; wd[MV_R] = r > 0 ? r : 0;
        g = s_sh.ads > 0.5f ? 0 : 1;
        for (d = 0; d < 4; d++)
            if (cs->mv[g][d] >= 0)
                spd += wd[d] / (wd[0] + wd[1] + wd[2] + wd[3]) * canim_speed(cs->mv[g][d]);
        if (s_sh.sprinting && cs->sprint >= 0)
            spd = canim_speed(cs->sprint);
        spd *= 0.88f;
        if (spd < 60.0f)                              /* no clips: a jog */
            spd = s_sh.sprinting ? 520.0f : 300.0f;
    }
    cam_axes(right, up, fwd);
    nf = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    nr = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (nf > 1e-3f && nr > 1e-3f) {
        want[0] = (fwd[0] / nf * f + right[0] / nr * r) * spd;
        want[1] = (fwd[2] / nf * f + right[2] / nr * r) * spd;
    }
    k = clampf(dt * 12.0f, 0.0f, 1.0f);              /* quick to start and stop, as CoD */
    s_sh.vel[0] += (want[0] - s_sh.vel[0]) * k;
    s_sh.vel[1] += (want[1] - s_sh.vel[1]) * k;
    MEMF(body + 0x30) = s_sh.vel[0];
    MEMF(body + 0x34) = 0.0f;
    MEMF(body + 0x38) = s_sh.vel[1];
    if (getenv("RECOMP_SHOOT_DEBUG")) {
        static DWORD last;
        if (GetTickCount() - last > 500) {
            last = GetTickCount();
            fprintf(stderr, "[MOVE] keys %.0f %.0f speed %.0f crouch %d sprint %d vel %.0f %.0f\n", f, r, spd,
                    s_sh.crouched, s_sh.sprinting, s_sh.vel[0], s_sh.vel[1]);
        }
    }
}

/* King::Update: in shooter mode he turns, quickly, to face where the camera
 * looks, through the game's own heading: +0x30C (wanted) and +0x1A4 (eased),
 * from which the next update builds his orientation. The model faces its -Z,
 * and a heading a puts that along (sin a, 0, -cos a). */
void sub_0001CD20(void)
{
    float right[3], up[3], fwd[3], a, d, dt;

    sub_0001CD20_gen();
    {   /* RECOMP_WARP=<slot>: warp there once, two seconds into play (testing) */
        static int frames;
        if (getenv("RECOMP_WARP") && game_in_play() && frames >= 0 && ++frames == 60) {
            OverlayGameInfo gi;
            game_info(&gi);
            fprintf(stderr, "[WARP] slot %s; level types %u %u %u %u (0 logging, 1 suburbia, 2 construction, 3 urban)\n",
                    getenv("RECOMP_WARP"), gi.level_type[0], gi.level_type[1], gi.level_type[2], gi.level_type[3]);
            game_cheat_warp(atoi(getenv("RECOMP_WARP")));
            frames = -1;
        }
    }
    if (skate_on()) {
        skate_king_tick();
        return;
    }
    if (!s_sh.on || !game_in_play() || !MEM32(GA_KING))
        return;
    cam_axes(right, up, fwd);
    if (fwd[0] * fwd[0] + fwd[2] * fwd[2] < 1e-4f)
        return;
    a = atan2f(fwd[0], -fwd[2]);
    dt = MEMF(GA_DT);
    d = fmodf(a - s_sh.yaw + 3.0f * 3.14159265f, 2.0f * 3.14159265f) - 3.14159265f;
    s_sh.yaw += d * clampf(dt * 14.0f, 0.0f, 1.0f);
    s_sh.yaw = fmodf(s_sh.yaw + 3.0f * 3.14159265f, 2.0f * 3.14159265f) - 3.14159265f;
    MEMF(MEM32(GA_KING) + 0x30C) = s_sh.yaw;
    MEMF(MEM32(GA_KING) + 0x1A4) = s_sh.yaw;
    shooter_move(MEM32(GA_KING), dt > 0.0f && dt < 0.25f ? dt : 1.0f / 30.0f);
    {   /* and his orientation now, as the update builds it from +0x1A4
         * (0x11BC50: a yaw quaternion), or the next update eases the
         * heading back toward where he moves and strafing turns him */
        uint32_t o = MEM32(MEM32(GA_KING) + 0x68);
        float h = s_sh.yaw * MEMF(0x2B03A0);
        if (o) {
            MEMF(o + 0x14) = 0.0f;
            MEMF(o + 0x18) = sinf(h);
            MEMF(o + 0x1C) = 0.0f;
            MEMF(o + 0x20) = cosf(h);
            MEM32(o + 0x38) |= 0x800;
        }
    }
}

/* -- The camera: over the right shoulder, closer when aiming --------------- */

/* KingCam's update. Its distance (+0x138) also sets the look target's
 * height (0x2D5E0: the closer than the zoom level's distance, the lower),
 * so shooter mode scales the distance only around the eye's placement
 * (shoulder() below), never the zoom levels themselves. */
void sub_0002DED0(void)
{
    uint32_t cam = g_ecx, set = MEM32(cam + 0x98);
    float lo = 0.0f, hi = 0.0f;
    /* the camera's pitch (degrees, down positive) is held between set+0x58
     * (8 down) and set+0x50, which lets it swing almost overhead; a shooter
     * looks up and down about as far as CoD's third person does */
    if (s_sh.on && set) {
        lo = MEMF(set + 0x58);
        hi = MEMF(set + 0x50);
        MEMF(set + 0x58) = -45.0f;
        MEMF(set + 0x50) = 55.0f;
    }
    sub_0002DED0_gen();
    if (s_sh.on && set) {
        MEMF(set + 0x58) = lo;
        MEMF(set + 0x50) = hi;
    }
    if (s_sh.on && getenv("RECOMP_SHOOT_TRACE")) {
        static DWORD last;
        if (GetTickCount() - last > 250) {
            last = GetTickCount();
            fprintf(stderr, "[CAM] ads %.2f pitch %.1f dist %.0f view.t %.0f %.0f %.0f target %.0f %.0f %.0f\n",
                    s_sh.ads, s_sh.pitch, MEMF(cam + 0x138), MEMF(GA_VIEW + 48), MEMF(GA_VIEW + 52),
                    MEMF(GA_VIEW + 56), MEMF(cam + 0x14C), MEMF(cam + 0x150), MEMF(cam + 0x154));
        }
    }
}

/* The look target shifted right and up, so the eye and the aim point move
 * together and the screen centre stays the aim; the occlusion test between
 * these two calls still runs from his head. The camera also comes in when
 * aiming and backs off when sprinting. */
static void shoulder(uint32_t cam, int on)
{
    static float saved[3], saved_dist, saved_pitch;
    float right[3], up[3], fwd[3], n, side = 60.0f, rise = 22.0f - 45.0f * s_sh.crouch;
    int i;
    if (!on) {
        for (i = 0; i < 3; i++)
            MEMF(cam + 0x14C + 4 * i) = saved[i];
        MEMF(cam + 0x138) = saved_dist;
        MEMF(cam + 0x12C) = saved_pitch;
        return;
    }
    /* the pitch is the mouse's alone: the game's own pulls it toward its
     * limits whenever the view from his head is blocked (0x2DB10) */
    saved_pitch = MEMF(cam + 0x12C);
    MEMF(cam + 0x12C) = s_sh.pitch;
    /* a fixed orbit round the shoulder: the game's own distance follows the
     * pitch (in close looking up, far and overhead looking down), which
     * suits sneaking, not aiming. RECOMP_TPS_DIST sets it (hip, King units) */
    {
        static float dist;
        if (!dist)
            dist = getenv("RECOMP_TPS_DIST") ? (float)atof(getenv("RECOMP_TPS_DIST")) : 270.0f;
        saved_dist = MEMF(cam + 0x138);
        MEMF(cam + 0x138) = dist * (1.0f - 0.45f * s_sh.ads) * (1.0f + 0.2f * s_sh.sprint) * (1.0f - 0.15f * s_sh.crouch);
    }
    cam_axes(right, up, fwd);
    n = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (n < 1e-3f)
        n = 1.0f;
    side *= 1.0f + 0.3f * s_sh.ads;
    for (i = 0; i < 3; i++)
        saved[i] = MEMF(cam + 0x14C + 4 * i);
    MEMF(cam + 0x14C) += right[0] / n * side;
    MEMF(cam + 0x150) += rise;
    MEMF(cam + 0x154) += right[2] / n * side;
    {   /* in front of anything between the shoulder and the eye (a wall
         * behind him, the ground when looking up), by the game's own ray */
        float p = s_sh.pitch * 0.0174533f, nf = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]), d = MEMF(cam + 0x138);
        float from[3], to[3], hit[3], fr;
        if (nf > 1e-3f) {
            for (i = 0; i < 3; i++)
                from[i] = MEMF(cam + 0x14C + 4 * i);
            to[0] = from[0] - fwd[0] / nf * cosf(p) * (d + 15.0f);
            to[1] = from[1] + sinf(p) * (d + 15.0f);
            to[2] = from[2] - fwd[2] / nf * cosf(p) * (d + 15.0f);
            fr = guest_raycast(from, to, hit);
            if (fr < 1.0f)
                MEMF(cam + 0x138) = fmaxf(fr * (d + 15.0f) - 15.0f, 20.0f);
        }
    }
}

/* KingCam's target update: it also turns the camera toward where the King
 * faces and moves, which in shooter mode (he faces the camera's way) would
 * chase itself round. Only the mouse turns it then. */
static float mouse_deg(void)
{
    static float deg;
    if (!deg)
        deg = getenv("RECOMP_MOUSE_DEG") ? (float)atof(getenv("RECOMP_MOUSE_DEG")) : 0.08f;
    return deg;
}

extern void sub_0002D5E0_gen(void);
void sub_0002D5E0(void)
{
    uint32_t cam = g_ecx;
    float y0, y1;
    if (skate_on()) {                 /* the board steers it, not his facing */
        y0 = MEMF(cam + 0x128);
        y1 = MEMF(cam + 0x130);
        sub_0002D5E0_gen();
        if (skate_live()) {
            static uint32_t last_clock;
            float look = 0.0f;
            if (MEM32(0x32A2A4) != last_clock) {  /* the mouse's share, once a game frame */
                last_clock = MEM32(0x32A2A4);
                look = s_mouse_dx * mouse_deg() * 0.0174533f;
            }
            MEMF(cam + 0x128) = y0;
            MEMF(cam + 0x130) = y1;
            skate_camera(cam, look);
        }
        return;
    }
    if (!s_sh.on) { sub_0002D5E0_gen(); return; }
    y0 = MEMF(cam + 0x128);
    y1 = MEMF(cam + 0x130);
    sub_0002D5E0_gen();
    {   /* the mouse's share: RECOMP_MOUSE_DEG degrees a count, as everywhere */
        static uint32_t last_clock;
        float d = 0.0f;
        if (MEM32(0x32A2A4) != last_clock) {      /* once a game frame */
            last_clock = MEM32(0x32A2A4);
            d = s_mouse_dx * mouse_deg() * 0.0174533f * (1.0f - 0.5f * s_sh.ads);
            s_sh.pitch = clampf(s_sh.pitch + s_mouse_dy * mouse_deg() * (1.0f - 0.5f * s_sh.ads), -45.0f, 55.0f);
        }
        MEMF(cam + 0x128) = y0 + d;
        MEMF(cam + 0x130) = y1 + d;
    }
}

void sub_0002D800(void)
{
    uint32_t cam = g_ecx;
    if (!s_sh.on) { sub_0002D800_gen(); return; }
    shoulder(cam, 1);
    sub_0002D800_gen();
    shoulder(cam, 0);
}

void sub_0002D940(void)
{
    uint32_t cam = g_ecx;
    if (!s_sh.on) { sub_0002D940_gen(); return; }
    shoulder(cam, 1);
    sub_0002D940_gen();
    shoulder(cam, 0);
}

/* The view kick turns the camera itself: added to KingCam's current and
 * desired yaw (radians) and pitch (degrees); the recovery pulls it back. */
static void apply_kick_to_cam(void)
{
    static float applied[2];
    uint32_t cam = MEM32(GA_KINGCAM);
    if (!cam)
        return;
    /* pitch is in degrees, the eye above the target when positive; it is
     * shooter mode's own (shoulder()) */
    s_sh.pitch = clampf(s_sh.pitch - (s_sh.kick[0] - applied[0]), -45.0f, 55.0f);
    MEMF(cam + 0x130) += (s_sh.kick[1] - applied[1]) * 0.0174533f;
    MEMF(cam + 0x128) += (s_sh.kick[1] - applied[1]) * 0.0174533f;
    applied[0] = s_sh.kick[0];
    applied[1] = s_sh.kick[1];
}

/* FirstPersonCamera::Update: shooter mode is third person, so no FP. */
void sub_0002C670(void)
{
    if (s_sh.on) {
        g_esp += 4;                                   /* ret */
        return;
    }
    sub_0002C670_gen();
}

/* -- Drawing: when the game leaves 3D for its HUD ------------------------- */

void sub_00122C50(void)
{
    uint32_t mode = MEM32(g_esp + 4);
    static uint32_t drawn_clock;
    if (skate_on() && MEM32(GA_GFX_MODE) == 1 && mode != 1 && mode != 2)
        skate_scene();
    if (s_sh.on && MEM32(GA_GFX_MODE) == 1 && mode != 1 && mode != 2
        && MEM32(0x32A2A4) != drawn_clock && shooter_live()) {
        int i;
        ShooterScene *sc = &s_sh.scene;
        drawn_clock = MEM32(0x32A2A4);
        apply_kick_to_cam();
        for (i = 0; i < 16; i++) {
            sc->view[i] = MEMF(GA_VIEW + 4 * i);
            sc->proj[i] = MEMF(GA_PROJ + 4 * i);
        }
        sc->proj[14] -= MEMF(GA_ZOFS);
        snprintf(sc->gun, sizeof sc->gun, "%s", cur_weapon()->dir);
        sc->draw_gun = s_sh.gun_ok;
        memcpy(sc->gun_world, s_sh.gun_world, sizeof sc->gun_world);
        memcpy(sc->muzzle, s_sh.muzzle, sizeof sc->muzzle);
        for (i = 0; i < 3; i++)
            sc->muzzle_dir[i] = s_sh.gun_world[i];   /* CoD X: the barrel */
        sc->flash = s_sh.flash;
        gun_world_submit(sc);
        for (i = 0; i < SHOOTER_TRACERS; i++)
            sc->tracer[i].life -= MEMF(GA_DT);
        for (i = 0; i < SHOOTER_IMPACTS; i++)
            sc->impact[i].life -= MEMF(GA_DT);
    }
    sub_00122C50_gen();
}

/* ADS zoom, applied by the FOV setter (sub_0012CA70). */
static float shooter_fov(float fx)
{
    if (!s_sh.on || !shooter_live())
        return fx;
    return fx + (cur_weapon()->ads_fov * fx / 65.0f - fx) * s_sh.ads;
}

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
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
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
    d = ctl == 2 ? s_mouse_dx : ctl == 3 ? s_mouse_dy : 0;

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

void sub_0012CA70(void)
{
    float fx = MEMF(g_esp + 4), fy = MEMF(g_esp + 8), k = MEMF(0x2B03A0);

    if (!game_widescreen()) {
        sub_0012CA70_gen();
        return;
    }
    if (fy == MEMF(0x2B039C))
        fy = (float)(2.0 * atan(tan(fx * k) / MEMF(0x30FC98)));
    MEMF(g_esp + 4) = (float)(atan(tan(fx * k) * WIDE_X) / k);
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

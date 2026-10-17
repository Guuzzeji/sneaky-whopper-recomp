/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * A hidden window on its own thread registers for Raw Input with
 * RIDEV_INPUTSINK, so mouse deltas and key presses arrive whether or
 * not the game's own window is the one with focus, and no matter which
 * window the runtime happens to present through (the GDI debug window,
 * the FMV window, a D3D11 swap chain -- the input layer should not
 * have to care).
 *
 * The poll side drains the accumulated deltas and turns them into a
 * stick deflection that is proportional to the movement since the last
 * poll: the title integrates deflection over its own frame, so the
 * total rotation comes out proportional to total mouse travel at any
 * frame rate, which is the property mouse aim has to keep.
 *
 * Small movements get a floor of RECOMP_MKB_DZ of full throw, because
 * every console title applies its own dead zone to the stick, and a
 * mouse correction smaller than it would be silently eaten.
 */

/* Raw input needs a modern _WIN32_WINNT; the toolkit's platform header
 * includes windows.h first, so these have to land before anything else. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define _CRT_SECURE_NO_WARNINGS

#include "mkb_input.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ---- configuration ----------------------------------------------------- */

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    long   full;        /* mouse counts for full throw, per poll     */
    int    invert_y;    /* invert the vertical aim axis              */
    int    swap_sticks; /* mouse aims with the left stick instead    */
} MkbConfig;

static MkbConfig s_cfg = { 1.0, 0.18, 1.0, 8, 0, 0 };

/* What each pad control is bound to: up to MKB_BIND_MAX host inputs, any of
 * which presses it. Targets 0..7 are the analog buttons in XBOX_BUTTON_*
 * order, 8..15 the digital bits in wButtons order (d-pad, START, BACK,
 * thumb clicks), then the four movement directions of the move stick.
 *
 * A host input is a virtual-key code (0..255), a mouse button
 * (MKB_MOUSE + 0..4: left, right, middle, X1, X2) or a wheel direction. The
 * defaults are the FPS layout this layer was written for (Black); a title
 * rebinds any target from mkb.ini with "bind.<TARGET>=<INPUT>,<INPUT>...".
 * No spaces: the ini reader ends a value at the first one. */
enum {
    MKB_T_UP = 8, MKB_T_MOVE_UP = 16, MKB_T_MOVE_DOWN, MKB_T_MOVE_LEFT,
    MKB_T_MOVE_RIGHT, MKB_T_COUNT
};
#define MKB_BIND_MAX   4
#define MKB_MOUSE      256
#define MKB_WHEEL_UP   (MKB_MOUSE + 5)
#define MKB_WHEEL_DOWN (MKB_MOUSE + 6)

static const char *const s_target_name[MKB_T_COUNT] = {
    "A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT",
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "START", "BACK", "LS", "RS",
    "MOVE_UP", "MOVE_DOWN", "MOVE_LEFT", "MOVE_RIGHT",
};

static unsigned short s_bind[MKB_T_COUNT][MKB_BIND_MAX] = {
    { VK_SPACE }, { 'E' }, { 'R' }, { MKB_MOUSE + 2, MKB_WHEEL_UP, MKB_WHEEL_DOWN },
    { 'F', MKB_MOUSE + 4 }, { 'Q', MKB_MOUSE + 3 }, { MKB_MOUSE + 1 }, { MKB_MOUSE + 0 },
    { VK_UP }, { VK_DOWN }, { VK_LEFT }, { VK_RIGHT }, { VK_RETURN, VK_ESCAPE }, { VK_TAB },
    { VK_SHIFT }, { VK_CONTROL },
    { 'W' }, { 'S' }, { 'A' }, { 'D' },
};

static const struct { const char *name; unsigned short code; } s_input_name[] = {
    { "SPACE", VK_SPACE }, { "ENTER", VK_RETURN }, { "ESCAPE", VK_ESCAPE },
    { "TAB", VK_TAB }, { "BACKSPACE", VK_BACK }, { "SHIFT", VK_SHIFT },
    { "CTRL", VK_CONTROL }, { "ALT", VK_MENU }, { "UP", VK_UP }, { "DOWN", VK_DOWN },
    { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT }, { "MOUSE1", MKB_MOUSE + 0 },
    { "MOUSE2", MKB_MOUSE + 1 }, { "MOUSE3", MKB_MOUSE + 2 }, { "MOUSE4", MKB_MOUSE + 3 },
    { "MOUSE5", MKB_MOUSE + 4 }, { "WHEELUP", MKB_WHEEL_UP }, { "WHEELDOWN", MKB_WHEEL_DOWN },
};

/* One host input by name: a listed name, a single letter or digit, or a
 * virtual-key number (0x..). 0 when unknown. */
static unsigned short mkb_parse_input(const char *s)
{
    size_t i;
    for (i = 0; i < sizeof s_input_name / sizeof s_input_name[0]; i++)
        if (!_stricmp(s, s_input_name[i].name))
            return s_input_name[i].code;
    if (s[0] && !s[1] && isalnum((unsigned char)s[0]))
        return (unsigned short)toupper((unsigned char)s[0]);   /* VK of a letter or digit */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        long v = strtol(s, NULL, 16);
        return v > 0 && v < 256 ? (unsigned short)v : 0;
    }
    return 0;
}

/* "bind.<TARGET>" = comma-separated inputs; NONE unbinds the target. */
static int mkb_apply_bind(const char *target, const char *val)
{
    char buf[128], *tok, *ctx = NULL;
    int t, n = 0;

    for (t = 0; t < MKB_T_COUNT && _stricmp(target, s_target_name[t]); t++)
        ;
    if (t == MKB_T_COUNT)
        return 0;
    memset(s_bind[t], 0, sizeof s_bind[t]);
    snprintf(buf, sizeof buf, "%s", val);
    for (tok = strtok_s(buf, ",", &ctx); tok && n < MKB_BIND_MAX;
         tok = strtok_s(NULL, ",", &ctx)) {
        unsigned short c = mkb_parse_input(tok);
        if (c)
            s_bind[t][n++] = c;
        else if (_stricmp(tok, "NONE"))
            fprintf(stderr, "  [MKB] bind.%s: unknown input \"%s\"\n", target, tok);
    }
    return 1;
}

/* One "key=value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;

    if (!_strnicmp(key, "bind.", 5)) {
        return mkb_apply_bind(key + 5, val);
    } else if (!strcmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!strcmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!strcmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!strcmp(key, "full")) {
        l = strtol(val, NULL, 0); if (l > 0 && l < 1000) s_cfg.full = l;
    } else if (!strcmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "swap_sticks")) {
        s_cfg.swap_sticks = atoi(val) != 0;
    } else {
        return 0;
    }
    return 1;
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL", "full",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        "RECOMP_MKB_SWAP_STICKS", "swap_sticks",
        NULL
    };
    char path[MAX_PATH];
    const char *ini;
    DWORD n;
    FILE *f;
    int i;

    for (i = 0; kv[i]; i += 2) {
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }

    ini = getenv("RECOMP_MKB_INI");
    if (ini && *ini) {
        snprintf(path, sizeof path, "%s", ini);
    } else {
        n = GetModuleFileNameA(NULL, path, sizeof path - 12);
        if (!n || n >= sizeof path - 12)
            return;
        snprintf(path + n, 12, "\\..\\mkb.ini");   /* beside the .exe */
    }
    f = fopen(path, "r");
    if (!f)
        return;
    {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            char *k, *v;
            if (!eq) continue;
            *eq = 0;
            k = line; v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            k[strcspn(k, " \t\r\n")] = 0;
            v[strcspn(v, " \t\r\n")] = 0;
            if (*k && *v)
                mkb_apply_kv(k, v);
        }
    }
    fclose(f);
}

/* ---- raw input thread -------------------------------------------------- */

/* Accumulators, written by the input thread and drained by the poll.
 * InterlockedExchange is the whole protocol: the input thread adds
 * with InterlockedAdd, the poll takes everything with Exchange(0). */
static volatile LONG s_dx, s_dy;
static volatile LONG s_wheel;        /* wheel steps since last poll   */
static volatile LONG s_mouse_btn;    /* held: 1 L, 2 R, 4 M, 8 X1, 16 X2 */
static volatile LONG s_mouse_click;  /* went down since the last poll */
static volatile unsigned char s_vk_down[256];

static volatile LONG s_active = 1;   /* F9 toggles this               */
static volatile LONG s_direct;       /* the title takes the mouse deltas itself */
static volatile LONG s_suspended;    /* a host overlay owns mouse and keys */
static volatile LONG s_menu_cursor;  /* show the pointer for a menu       */
static volatile LONG s_thread_up;

/* F9 is read on the input thread, so the toggle works even while the
 * game is stalled on the poll side. */
#define MKB_TOGGLE_VK  VK_F9

static void mkb_handle_raw(HRAWINPUT h)
{
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    RAWINPUT *ri = (RAWINPUT *)buf;

    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER))
            == (UINT)-1)
        return;

    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &ri->data.mouse;

        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (m->lLastX) InterlockedAdd(&s_dx, m->lLastX);
            if (m->lLastY) InterlockedAdd(&s_dy, m->lLastY);
        }
        /* Buttons 0..4 have their DOWN flag at bit 2i and UP at 2i+1. A
         * click also latches, so one that starts and ends between two polls
         * is still seen once. */
        for (int i = 0; i < 5; i++) {
            if (m->usButtonFlags & (1u << (2 * i))) {
                InterlockedOr(&s_mouse_btn, 1 << i);
                InterlockedOr(&s_mouse_click, 1 << i);
            }
            if (m->usButtonFlags & (2u << (2 * i)))
                InterlockedAnd(&s_mouse_btn, ~(1 << i));
        }
        if (m->usButtonFlags & RI_MOUSE_WHEEL) {
            SHORT d = (SHORT)m->usButtonData;
            InterlockedAdd(&s_wheel, d / WHEEL_DELTA);
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        UINT vk = k->VKey;

        if (vk == MKB_TOGGLE_VK) {
            if (k->Message == WM_KEYDOWN && !s_vk_down[vk & 0xFF]) {
                LONG now = !s_active;
                InterlockedExchange(&s_active, now);
                fprintf(stderr, "  [MKB] %s\n", now ? "captured"
                                                    : "released (F9)");
                fflush(stderr);
            }
        }
        if (vk && vk < 256) {
            if (k->Message == WM_KEYDOWN || k->Message == WM_SYSKEYDOWN)
                s_vk_down[vk] = 1;
            else
                s_vk_down[vk] = 0;
        }
    }
}

static LRESULT CALLBACK mkb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        mkb_handle_raw((HRAWINPUT)l);
        return 0;
    }
    if (m == WM_CLOSE || m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI mkb_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND hwnd;
    RAWINPUTDEVICE rid[2];
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = mkb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "XboxRecompMKB";
    RegisterClassA(&wc);
    /* Not visible: it exists to own the WM_INPUT delivery, nothing else. */
    hwnd = CreateWindowExA(0, "XboxRecompMKB", "mkb", WS_OVERLAPPED,
                           0, 0, 0, 0, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        fprintf(stderr, "  [MKB] window creation failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;  rid[0].usUsage = 0x02;  /* mouse     */
    rid[1].usUsagePage = 0x01;  rid[1].usUsage = 0x06;  /* keyboard  */
    rid[0].dwFlags = RIDEV_INPUTSINK;   /* deliver without focus */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "  [MKB] RegisterRawInputDevices failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    InterlockedExchange(&s_thread_up, 1);
    fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
            "sens=%.2f dz=%.2f full=%ld)\n",
            s_cfg.sens, s_cfg.dz, s_cfg.full);
    fflush(stderr);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    InterlockedExchange(&s_thread_up, 0);
    return 0;
}

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        /* Opt-in, like RECOMP_KEYBOARD: a keyboard silently acting as
         * player 1 is surprising when a real pad is what you meant to use.
         * RECOMP_MKB=1 turns the layer on. */
        const char *v = getenv("RECOMP_MKB");
        on = v && *v && *v != '0';
        if (on) {
            mkb_load_config();
            if (InterlockedCompareExchange(&s_thread_up, 0, 0) == 0) {
                static LONG started = 0;
                if (InterlockedCompareExchange(&started, 1, 0) == 0)
                    CloseHandle(CreateThread(NULL, 0, mkb_thread,
                                             NULL, 0, NULL));
            }
        }
    }
    return on;
}

/* The recompiled game's process is the foreground one. Raw input with
 * INPUTSINK keeps arriving while the user is in another app, and the
 * mouse must go on behaving normally there. */
static int game_is_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Hide and confine the pointer while captured. Aiming with raw deltas
 * does not move the pointer by itself, but Windows still moves it from
 * the same physical movement, so it would wander across the screen and
 * end up clicking other apps' title bars. */
static void mkb_manage_cursor(int capture)
{
    static int hidden;          /* how many ShowCursor(FALSE) we owe  */
    static unsigned long last;

    unsigned long now = GetTickCount();
    if (now - last < 500 && (capture ? hidden : !hidden) == 0)
        return;
    last = now;

    if (capture) {
        RECT r;
        HWND fg = GetForegroundWindow();
        if (fg && GetWindowRect(fg, &r))
            ClipCursor(&r);
        if (hidden < 5) { ShowCursor(FALSE); hidden++; }
    } else {
        if (hidden > 0) { ShowCursor(TRUE); hidden--; }
        ClipCursor(NULL);
    }
}

/* Radial deadzone compensation: an input radius in [0..1] comes out as
 * [dz..1], so the smallest mouse correction still clears the title's
 * own deadzone, and the response stays linear above it. */
static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    double r = sqrt(ax * ax + ay * ay);
    double len, fx, fy;
    long v;

    if (r <= 0.0) { *out_x = 0; *out_y = 0; return; }
    if (r > 1.0) r = 1.0;
    len = s_cfg.dz + (1.0 - s_cfg.dz) * pow(r, s_cfg.acc);
    len *= s_cfg.sens;
    if (len > 1.0) len = 1.0;
    fx = ax * len / sqrt(ax * ax + ay * ay);
    fy = ay * len / sqrt(ax * ax + ay * ay);
    v = (long)(fx * 32767.0);
    *out_x = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = (long)(fy * 32767.0);
    *out_y = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Whether a bound host input is down now. btn holds the mouse buttons
 * (held, plus clicks since the last poll); wheel the wheel's direction this
 * poll. */
static int mkb_input_down(unsigned code, LONG btn, int wheel)
{
    if (code == MKB_WHEEL_UP)   return wheel > 0;
    if (code == MKB_WHEEL_DOWN) return wheel < 0;
    if (code >= MKB_MOUSE)      return (btn >> (code - MKB_MOUSE)) & 1;
    return code && code < 256 && s_vk_down[code];
}

static int mkb_target_down(int t, LONG btn, int wheel)
{
    for (int i = 0; i < MKB_BIND_MAX; i++)
        if (s_bind[t][i] && mkb_input_down(s_bind[t][i], btn, wheel))
            return 1;
    return 0;
}

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SHORT aim_x = 0, aim_y = 0, mov_x = 0, mov_y = 0;
    WORD b = 0;
    BYTE analog[8];
    LONG btn;
    int any = 0, i, wheel = 0;
    static DWORD packet;
    /* Mouse aim is a rate: counts per second over the last stretch of at
     * least AIM_MS, held until the next. The title may poll faster than it
     * reads (the OHCI model completes reports on its own clock), and
     * draining the deltas on every report left the title seeing only the
     * movement of the last few milliseconds. */
    enum { AIM_MS = 15, WHEEL_MS = 100 };
    static DWORD aim_t0, wheel_until;
    static int wheel_dir;
    static SHORT held_x, held_y;
    DWORD now;

    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;
    if (!s_thread_up)
        return 0;

    if (!s_active || s_suspended) {
        mkb_manage_cursor(0);
        return 0;
    }

    /* Foreground gate: while the user is elsewhere the mouse belongs to
     * whoever has it, and the game should read a pad at rest. */
    if (!game_is_foreground()) {
        mkb_manage_cursor(0);
        InterlockedExchange(&s_dx, 0);
        InterlockedExchange(&s_dy, 0);
        held_x = held_y = 0;
        return 0;
    }
    mkb_manage_cursor(!s_menu_cursor);

    now = GetTickCount();
    btn = s_mouse_btn | InterlockedExchange(&s_mouse_click, 0);
    /* A wheel step is held for WHEEL_MS so a title that samples at its own
     * frame rate cannot miss it. */
    {
        LONG w = InterlockedExchange(&s_wheel, 0);
        if (w) {
            wheel_dir = w > 0 ? 1 : -1;
            wheel_until = now + WHEEL_MS;
        }
        if ((LONG)(wheel_until - now) > 0)
            wheel = wheel_dir;
    }

    memset(analog, 0, sizeof analog);
    for (i = 0; i < 8; i++)
        if (mkb_target_down(i, btn, wheel))
            analog[i] = 255;
    for (i = 0; i < 8; i++)
        if (mkb_target_down(MKB_T_UP + i, btn, wheel))
            b |= (WORD)(1 << i);

    /* ---- movement keys: the move stick ---- */
    {
        int mx = mkb_target_down(MKB_T_MOVE_RIGHT, btn, wheel)
               - mkb_target_down(MKB_T_MOVE_LEFT, btn, wheel);
        int my = mkb_target_down(MKB_T_MOVE_UP, btn, wheel)
               - mkb_target_down(MKB_T_MOVE_DOWN, btn, wheel);
        if (mx || my) {
            double n = (mx && my) ? 0.7071 : 1.0;
            mov_x = (SHORT)(mx * n * 32767.0);
            mov_y = (SHORT)(my * n * 32767.0);
        }
    }

    /* ---- mouse: aim. `full` counts in 1/60 s is full throw ---- */
    if (!s_direct && now - aim_t0 >= AIM_MS) {
        LONG dx = InterlockedExchange(&s_dx, 0);
        LONG dy = InterlockedExchange(&s_dy, 0);
        double per = (double)s_cfg.full * (aim_t0 && now - aim_t0 < 200
                                           ? (now - aim_t0) / (1000.0 / 60.0) : 1.0);
        held_x = held_y = 0;
        if (dx || dy) {
            double ax = (double)dx / per, ay = -(double)dy / per;
            if (s_cfg.invert_y) ay = -ay;
            mkb_aim_vector(ax, ay, &held_x, &held_y);
        }
        aim_t0 = now;
    }
    aim_x = held_x;
    aim_y = held_y;

    /* ---- merge on top of whatever the host pad produced ---- */
    if (b)        { pState->Gamepad.wButtons |= b; any = 1; }
    for (i = 0; i < 8; i++) {
        if (analog[i]) {
            if (analog[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = analog[i];
            any = 1;
        }
    }
    if (aim_x || aim_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbLX = aim_x;
            pState->Gamepad.sThumbLY = aim_y;
        } else {
            pState->Gamepad.sThumbRX = aim_x;
            pState->Gamepad.sThumbRY = aim_y;
        }
        any = 1;
    }
    if (mov_x || mov_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbRX = mov_x;
            pState->Gamepad.sThumbRY = mov_y;
        } else {
            pState->Gamepad.sThumbLX = mov_x;
            pState->Gamepad.sThumbLY = mov_y;
        }
        any = 1;
    }

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move. */
    if (any)
        pState->dwPacketNumber = ++packet;
    return any;
}

int mkb_take_mouse_delta(long *dx, long *dy)
{
    s_direct = 1;
    *dx = InterlockedExchange(&s_dx, 0);
    *dy = InterlockedExchange(&s_dy, 0);
    if (!mkb_enabled() || !s_thread_up || !s_active || s_suspended
            || !game_is_foreground()) {
        *dx = *dy = 0;
        return 0;
    }
    return 1;
}

void mkb_set_suspended(int on)   { InterlockedExchange(&s_suspended, on != 0); }
void mkb_set_menu_cursor(int on) { InterlockedExchange(&s_menu_cursor, on != 0); }

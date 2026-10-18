/*
 * web_host.c: the browser side of the host window and input (Emscripten).
 *
 * Natively these come from the toolkit's Win32 framebuffer window and raw
 * input (fb_present.c, mkb_input.c). In the page there is the canvas: the
 * GL back end draws to it itself, and keyboard and mouse events arrive
 * through the html5 API. The callbacks run on the browser's main thread and
 * only write shared state, which the game threads read.
 *
 * Controls are mkb.ini's (the native keyboard and mouse layout): s_bind maps
 * each host key or mouse button onto a key of the toolkit's keyboard pad
 * (keyboard.c, RECOMP_KEYBOARD), whose fixed map turns it into the pad
 * control. DOM keyCode values are Windows virtual-key codes, as natively.
 * As natively, the pointer is free in menus (it points at options, gl_hle.c)
 * and locked in play, where the mouse turns the camera. The browser locks it
 * only on a user event: the next key or click after play starts. Escape
 * releases it, a click takes it back.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>

#include "keyboard.h"
#include "overlay.h"

static atomic_long s_mouse_dx, s_mouse_dy;
static atomic_int s_locked;
static atomic_int s_menu = 1;               /* pointer free: not in play */
static atomic_int s_px = -1, s_py = -1;     /* pointer over the picture, guest 640x480 */

/* Host inputs: virtual keys 0..255, then mouse buttons (DOM numbering: left,
 * middle, right, back, forward) and the wheel. */
enum { M_LEFT = 256, M_MIDDLE, M_RIGHT, M_BACK, M_FORWARD, M_WHEEL, SRC_COUNT };

/* mkb.ini's bindings, as {host input, keyboard.c key for that pad control}.
 * ponytail: fixed here; serve and parse mkb.ini if web players need rebinding. */
static const unsigned short s_bind[][2] = {
    { 'W', 'W' }, { 'S', 'S' }, { 'A', 'A' }, { 'D', 'D' },        /* move (left stick) */
    { 16, '3' },                                                   /* Shift: RT, run */
    { ' ', 'Z' }, { 13, 'Z' }, { M_LEFT, 'Z' },                    /* A */
    { M_RIGHT, 'X' }, { 'Q', 'X' }, { 8, 'X' }, { M_BACK, 'X' },   /* B */
    { 'E', 'V' }, { M_MIDDLE, 'V' },                               /* Y */
    { 'R', 'C' },                                                  /* X */
    { 17, '1' },                                                   /* Ctrl: LT */
    { 'F', 16 },                                                   /* LS click: first person */
    { M_WHEEL, 17 }, { 'Z', 17 },                                  /* RS click: zoom */
    { 38, 38 }, { 40, 40 }, { 37, 37 }, { 39, 39 },                /* d-pad */
    { 27, 13 }, { 'P', 13 },                                       /* START */
    { 9, 8 },                                                      /* BACK */
    { '1', 'Q' }, { '2', 'E' },                                    /* WHITE, BLACK */
};
#define NBIND (sizeof s_bind / sizeof s_bind[0])

static unsigned char s_src[SRC_COUNT];      /* browser thread only */

/* Each pad key is down while any host input bound to it is. */
static void remap(void)
{
    unsigned char down[256] = { 0 };
    size_t i;
    for (i = 0; i < NBIND; i++)
        down[s_bind[i][1]] |= s_src[s_bind[i][0]];
    for (i = 0; i < NBIND; i++)
        xbox_InputKeySet(s_bind[i][1], down[s_bind[i][1]]);
}

static int bound(int src)
{
    size_t i;
    for (i = 0; i < NBIND; i++)
        if (s_bind[i][0] == src)
            return 1;
    return 0;
}

/* fb_present.c: the GL back end presents into the canvas itself. */
void xbox_FramebufferWindowExternalPresent(int on) { (void)on; }

/* mkb_input.c: the mouse counts since the last call, for the camera. */
int mkb_take_mouse_delta(long *dx, long *dy)
{
    *dx = atomic_exchange(&s_mouse_dx, 0);
    *dy = atomic_exchange(&s_mouse_dy, 0);
    return atomic_load(&s_locked);
}

void mkb_set_suspended(int on) { (void)on; }

/* gl_hle.c, each present: whether the game is out of play. Leaving play asks
 * for the pointer lock, which the browser grants at the next key or click;
 * a menu gives the pointer back. */
void mkb_set_menu_cursor(int on)
{
    on = on != 0;
    if (atomic_exchange(&s_menu, on) == on)
        return;
    if (on)
        emscripten_exit_pointerlock();
    else
        emscripten_request_pointerlock("#screen", 1);
}

/* gl_hle.c: the pointer over the picture in guest 640x480, or -1. */
void web_pointer(int *gx, int *gy)
{
    *gx = atomic_load(&s_px);
    *gy = atomic_load(&s_py);
}

/* Shooter mode's hooks (recomp_manual.c). The mode is off in the browser
 * (mods_stub.c), so these are never live. */
void *xbox_FramebufferWindowHwnd(void) { return NULL; }
int mkb_mouse_buttons(int grab, int *clicked) { (void)grab; if (clicked) *clicked = 0; return 0; }
void mkb_set_mute(int move, unsigned analog_mask) { (void)move; (void)analog_mask; }

/* Cheats: the page's buttons (web/index.html) call web_cheat() on the
 * browser thread; the request runs on the game thread at the next present
 * (gl_hle.c), where the F1 overlay runs its cheats natively, because they
 * call into lifted code. 0 unlock levels, 1 unlock all, 2+N warp to slot N. */
static atomic_int s_cheat = -1;

EMSCRIPTEN_KEEPALIVE void web_cheat(int code) { atomic_store(&s_cheat, code); }

void web_cheats_run(void)
{
    int c = atomic_exchange(&s_cheat, -1);
    if (c >= 0)
        fprintf(stderr, "[CHEAT] %d\n", c);
    if (c == 0)
        game_cheat_unlock_levels();
    else if (c == 1)
        game_cheat_unlock_all();
    else if (c >= 2)
        game_cheat_warp(c - 2);           /* ignored outside play, as natively */
}

static bool on_key(int type, const EmscriptenKeyboardEvent *e, void *user)
{
    int vk = (int)e->keyCode;
    (void)user;
    if (!vk || vk == 116 /* F5 */ || vk == 122 /* F11 */ || vk == 123 /* F12 */)
        return false;                       /* leave reload and dev tools alone */
    if (vk > 255 || !bound(vk))
        return false;
    s_src[vk] = type == EMSCRIPTEN_EVENT_KEYDOWN;
    remap();
    return true;                            /* no scrolling, no Tab focus moves */
}

static bool on_mouse_move(int type, const EmscriptenMouseEvent *e, void *user)
{
    double w, h;
    (void)user;
    if (atomic_load(&s_locked)) {
        atomic_fetch_add(&s_mouse_dx, e->movementX);
        atomic_fetch_add(&s_mouse_dy, e->movementY);
    } else if (type == EMSCRIPTEN_EVENT_MOUSEMOVE
               && emscripten_get_element_css_size("#screen", &w, &h) == EMSCRIPTEN_RESULT_SUCCESS
               && w > 0 && h > 0) {
        atomic_store(&s_px, (int)(e->targetX * 640 / w));
        atomic_store(&s_py, (int)(e->targetY * 480 / h));
        return false;
    }
    atomic_store(&s_px, -1);                /* left the picture, or locked */
    return false;
}

static bool on_mouse_button(int type, const EmscriptenMouseEvent *e, void *user)
{
    int down = type == EMSCRIPTEN_EVENT_MOUSEDOWN;
    (void)user;
    if (e->button > 4)
        return false;
    if (down && !atomic_load(&s_locked) && !atomic_load(&s_menu)) {
        emscripten_request_pointerlock("#screen", 0);   /* in play: take the mouse back */
        return true;
    }
    s_src[M_LEFT + e->button] = down;
    remap();
    return true;                            /* no back/forward navigation on X1/X2 */
}

static void wheel_release(void *user)
{
    (void)user;
    s_src[M_WHEEL] = 0;
    remap();
}

/* A wheel notch is a short press: long enough for the game to poll it. */
static bool on_wheel(int type, const EmscriptenWheelEvent *e, void *user)
{
    (void)type; (void)e; (void)user;
    if (!s_src[M_WHEEL]) {
        s_src[M_WHEEL] = 1;
        remap();
        emscripten_set_timeout(wheel_release, 100, NULL);
    }
    return true;
}

static bool on_lock(int type, const EmscriptenPointerlockChangeEvent *e, void *user)
{
    (void)type; (void)user;
    atomic_store(&s_locked, e->isActive);
    return false;
}

static bool on_blur(int type, const EmscriptenFocusEvent *e, void *user)
{
    (void)type; (void)e; (void)user;
    memset(s_src, 0, sizeof s_src);         /* no keys stuck down after alt-tab */
    xbox_InputKeysClear();
    return false;
}

void web_host_init(void)
{
    const pthread_t ui = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD;
    emscripten_set_keydown_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_key, ui);
    emscripten_set_keyup_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_key, ui);
    emscripten_set_mousemove_callback_on_thread("#screen", NULL, 1, on_mouse_move, ui);
    emscripten_set_mouseleave_callback_on_thread("#screen", NULL, 1, on_mouse_move, ui);
    emscripten_set_mousedown_callback_on_thread("#screen", NULL, 1, on_mouse_button, ui);
    emscripten_set_mouseup_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, on_mouse_button, ui);
    emscripten_set_wheel_callback_on_thread("#screen", NULL, 1, on_wheel, ui);
    emscripten_set_pointerlockchange_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, on_lock, ui);
    emscripten_set_blur_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_blur, ui);
}

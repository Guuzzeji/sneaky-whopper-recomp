/*
 * web_host.c: the browser side of the host window and input (Emscripten).
 *
 * Natively these come from the toolkit's Win32 framebuffer window and raw
 * input (fb_present.c, mkb_input.c). In the page there is the canvas: the
 * GL back end draws to it itself, and keyboard and mouse events arrive
 * through the html5 API. The callbacks run on the browser's main thread and
 * only write shared state, which the game threads read.
 *
 * Keys go to the toolkit's keyboard pad (keyboard.c, RECOMP_KEYBOARD): DOM
 * keyCode values are the Windows virtual-key codes it is written against.
 * The mouse turns the camera once the canvas has the pointer (click it);
 * Escape gives the pointer back, so START is Enter here.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "keyboard.h"
#include "overlay.h"

static atomic_long s_mouse_dx, s_mouse_dy;
static atomic_int s_locked;

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
void mkb_set_menu_cursor(int on) { (void)on; }

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
    xbox_InputKeySet(vk, type == EMSCRIPTEN_EVENT_KEYDOWN);
    return true;                            /* no scrolling on arrows and space */
}

static bool on_mouse_move(int type, const EmscriptenMouseEvent *e, void *user)
{
    (void)type; (void)user;
    if (atomic_load(&s_locked)) {
        atomic_fetch_add(&s_mouse_dx, e->movementX);
        atomic_fetch_add(&s_mouse_dy, e->movementY);
    }
    return false;
}

static bool on_click(int type, const EmscriptenMouseEvent *e, void *user)
{
    (void)type; (void)e; (void)user;
    if (!atomic_load(&s_locked))
        emscripten_request_pointerlock("#screen", 0);
    return false;
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
    xbox_InputKeysClear();                  /* no keys stuck down after alt-tab */
    return false;
}

void web_host_init(void)
{
    const pthread_t ui = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD;
    emscripten_set_keydown_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_key, ui);
    emscripten_set_keyup_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_key, ui);
    emscripten_set_mousemove_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, on_mouse_move, ui);
    emscripten_set_click_callback_on_thread("#screen", NULL, 1, on_click, ui);
    emscripten_set_pointerlockchange_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, on_lock, ui);
    emscripten_set_blur_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_blur, ui);
}

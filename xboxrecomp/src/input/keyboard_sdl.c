/*
 * The SDL window's keys into the key table (keyboard.c), on POSIX hosts.
 *
 * The presenter's main loop (src/video/fb_present_sdl.c) hands every event
 * here and has no key logic of its own, so tests/input_keyboard_sdl drives
 * these rules with synthetic events without linking the presenter.
 *
 * Keycodes, not scancodes: a Windows VK for a letter follows the keyboard
 * layout (the key labelled Z is 'Z' on AZERTY too), and so does an SDL
 * keycode, so the one map means the same keys on both.
 */
#include "keyboard_sdl.h"
#include "keyboard.h"
#include <stdio.h>
#include <stdlib.h>

/* SDL2's keycodes are ABI-stable; the letter and digit arithmetic below
 * relies on these. */
_Static_assert(SDLK_a == 'a' && SDLK_z == 'z' && SDLK_0 == '0' && SDLK_9 == '9',
               "SDL letter and digit keycodes are their ASCII codes");
_Static_assert(SDLK_RETURN == '\r' && SDLK_BACKSPACE == '\b',
               "SDL Return and Backspace keycodes are their ASCII codes");

int xbox_InputSdlKeyToVk(int key)
{
    if (key >= SDLK_a && key <= SDLK_z)
        return key - 'a' + 'A';
    if (key >= SDLK_0 && key <= SDLK_9)
        return key;
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return XBOX_VK_RETURN;   /* both, as Win32 reports */
    case SDLK_BACKSPACE: return XBOX_VK_BACK;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:    return XBOX_VK_SHIFT;
    case SDLK_LCTRL:
    case SDLK_RCTRL:     return XBOX_VK_CONTROL;
    case SDLK_UP:        return XBOX_VK_UP;
    case SDLK_DOWN:      return XBOX_VK_DOWN;
    case SDLK_LEFT:      return XBOX_VK_LEFT;
    case SDLK_RIGHT:     return XBOX_VK_RIGHT;
    case SDLK_KP_2:      return XBOX_VK_NUMPAD2;
    case SDLK_KP_4:      return XBOX_VK_NUMPAD4;
    case SDLK_KP_6:      return XBOX_VK_NUMPAD6;
    case SDLK_KP_8:      return XBOX_VK_NUMPAD8;
    }
    return 0;
}

void xbox_InputSdlInit(void)
{
    /* SDL2 starts text input on desktop at init. The game takes no text,
     * and on macOS text input lets the press-and-hold accent popover take a
     * held letter (W held to walk would stop walking). */
    SDL_StopTextInput();
}

/* RECOMP_KEY_TRACE: each key-down as it arrives, the first 40, the same
 * line the GDI window prints. */
static void trace_down(int vk)
{
    static unsigned n;
    if (getenv("RECOMP_KEY_TRACE") && n++ < 40) {
        fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)vk);
        fflush(stderr);
    }
}

void xbox_InputSdlEvent(const SDL_Event *ev)
{
    int vk;

    switch (ev->type) {
    case SDL_KEYDOWN:
        /* Auto-repeat would only re-set a held key; dropping it keeps the
         * trace to real edges. */
        if (ev->key.repeat)
            return;
        /* Cmd+key is an app shortcut (Cmd-Tab, Cmd-H, Cmd-Q), not a game
         * key. Cmd itself releases whatever is held, which is what the
         * shortcut that follows expects; key-ups still apply below. */
        if (ev->key.keysym.sym == SDLK_LGUI || ev->key.keysym.sym == SDLK_RGUI) {
            xbox_InputKeysClear();
            return;
        }
        if (ev->key.keysym.mod & KMOD_GUI)
            return;
        vk = xbox_InputSdlKeyToVk(ev->key.keysym.sym);
        if (vk) {
            xbox_InputKeySet(vk, 1);
            trace_down(vk);
        }
        return;
    case SDL_KEYUP:
        vk = xbox_InputSdlKeyToVk(ev->key.keysym.sym);
        if (vk)
            xbox_InputKeySet(vk, 0);
        return;
    case SDL_WINDOWEVENT:
        /* SDL sends keys only to the focused window, so there is no focus
         * check on the way in; on the way out a held key would stay held
         * for ever (Cmd-Tab, Mission Control, Cmd-H, Cmd-M). */
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST
            || ev->window.event == SDL_WINDOWEVENT_HIDDEN
            || ev->window.event == SDL_WINDOWEVENT_MINIMIZED)
            xbox_InputKeysClear();
        return;
    }
}

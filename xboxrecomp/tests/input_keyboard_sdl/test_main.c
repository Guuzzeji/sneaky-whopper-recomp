/*
 * input_keyboard_sdl test: see CMakeLists.txt.
 *
 * Synthetic SDL events through xbox_InputSdlEvent(), checked in the key
 * table: down, auto-repeat, up, Cmd+key, Cmd going down with a key held,
 * focus lost, hidden, minimized, an unmapped key; and the keycode -> VK
 * rows, written out here independently of keyboard_sdl.c.
 */
#include "keyboard.h"
#include "keyboard_sdl.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static void key(Uint32 type, SDL_Keycode sym, Uint16 mod, Uint8 repeat)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    ev.key.repeat = repeat;
    ev.key.keysym.sym = sym;
    ev.key.keysym.mod = mod;
    xbox_InputSdlEvent(&ev);
}

static void window(Uint8 what)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_WINDOWEVENT;
    ev.window.event = what;
    xbox_InputSdlEvent(&ev);
}

static int held_count(void)
{
    int n = 0;
    for (int vk = 0; vk < 256; vk++)
        n += xbox_FramebufferKeyDown(vk);
    return n;
}

static void test_keycodes(void)
{
    static const struct { SDL_Keycode sym; int vk; } rows[] = {
        { SDLK_a, 'A' }, { SDLK_c, 'C' }, { SDLK_d, 'D' }, { SDLK_e, 'E' },
        { SDLK_i, 'I' }, { SDLK_j, 'J' }, { SDLK_k, 'K' }, { SDLK_l, 'L' },
        { SDLK_q, 'Q' }, { SDLK_s, 'S' }, { SDLK_v, 'V' }, { SDLK_w, 'W' },
        { SDLK_x, 'X' }, { SDLK_z, 'Z' }, { SDLK_1, '1' }, { SDLK_3, '3' },
        { SDLK_RETURN, 0x0D }, { SDLK_KP_ENTER, 0x0D }, { SDLK_BACKSPACE, 0x08 },
        { SDLK_LSHIFT, 0x10 }, { SDLK_RSHIFT, 0x10 },
        { SDLK_LCTRL, 0x11 },  { SDLK_RCTRL, 0x11 },
        { SDLK_LEFT, 0x25 }, { SDLK_UP, 0x26 }, { SDLK_RIGHT, 0x27 }, { SDLK_DOWN, 0x28 },
        { SDLK_KP_2, 0x62 }, { SDLK_KP_4, 0x64 }, { SDLK_KP_6, 0x66 }, { SDLK_KP_8, 0x68 },
        { SDLK_F1, 0 }, { SDLK_ESCAPE, 0 }, { SDLK_LGUI, 0 }, { SDLK_KP_5, 0 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        CHECK(xbox_InputSdlKeyToVk(rows[i].sym) == rows[i].vk,
              "keycode 0x%X -> 0x%02X, want 0x%02X", (unsigned)rows[i].sym,
              xbox_InputSdlKeyToVk(rows[i].sym), rows[i].vk);
    /* Every VK the map reads can be reached from some SDL key. */
    for (size_t r = 0; r < xbox_key_map_count; r++)
        for (int k = 0; k < 2; k++) {
            int vk = xbox_key_map[r].vk[k], hit = 0;
            if (!vk) continue;
            for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
                hit |= rows[i].vk == vk;
            CHECK(hit, "map key vk 0x%02X (%s) has no SDL key here", vk, xbox_key_map[r].keys);
        }
}

static void test_events(void)
{
    xbox_InputKeysClear();

    /* Down, repeat, up. */
    key(SDL_KEYDOWN, SDLK_RETURN, 0, 0);
    CHECK(xbox_FramebufferKeyDown(0x0D), "Return down not held");
    key(SDL_KEYDOWN, SDLK_RETURN, 0, 1);
    CHECK(xbox_FramebufferKeyDown(0x0D) && held_count() == 1, "repeat changed the table");
    key(SDL_KEYUP, SDLK_RETURN, 0, 0);
    CHECK(!xbox_FramebufferKeyDown(0x0D), "Return up still held");

    /* A repeat on its own (its down was lost) presses nothing. */
    key(SDL_KEYDOWN, SDLK_z, 0, 1);
    CHECK(held_count() == 0, "repeat-only down pressed a key");

    /* Shift held is a key, and Shift+letter is still the letter. */
    key(SDL_KEYDOWN, SDLK_LSHIFT, KMOD_LSHIFT, 0);
    key(SDL_KEYDOWN, SDLK_w, KMOD_LSHIFT, 0);
    CHECK(xbox_FramebufferKeyDown(0x10) && xbox_FramebufferKeyDown('W'), "Shift+W");
    key(SDL_KEYUP, SDLK_w, KMOD_LSHIFT, 0);
    key(SDL_KEYUP, SDLK_LSHIFT, 0, 0);
    CHECK(held_count() == 0, "Shift+W released");

    /* Cmd+key is a shortcut: ignored. Its key-up still applies. */
    key(SDL_KEYDOWN, SDLK_q, KMOD_LGUI, 0);
    CHECK(!xbox_FramebufferKeyDown('Q'), "Cmd+Q pressed Q");

    /* Cmd going down releases what is held. */
    key(SDL_KEYDOWN, SDLK_d, 0, 0);
    key(SDL_KEYDOWN, SDLK_RGUI, 0, 0);
    CHECK(held_count() == 0, "Cmd down left %d keys held", held_count());
    key(SDL_KEYDOWN, SDLK_d, 0, 0);
    key(SDL_KEYUP, SDLK_d, KMOD_LGUI, 0);
    CHECK(!xbox_FramebufferKeyDown('D'), "key-up with Cmd held not applied");

    /* Unmapped keys do nothing. */
    key(SDL_KEYDOWN, SDLK_F5, 0, 0);
    key(SDL_KEYDOWN, SDLK_ESCAPE, 0, 0);
    CHECK(held_count() == 0, "unmapped key held");

    /* Focus lost, hidden, minimized each release everything. */
    static const Uint8 kinds[] = { SDL_WINDOWEVENT_FOCUS_LOST, SDL_WINDOWEVENT_HIDDEN,
                                   SDL_WINDOWEVENT_MINIMIZED };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        key(SDL_KEYDOWN, SDLK_UP, 0, 0);
        key(SDL_KEYDOWN, SDLK_KP_8, 0, 0);
        CHECK(held_count() == 2, "setup for window event %d", kinds[i]);
        window(kinds[i]);
        CHECK(held_count() == 0, "window event %d left keys held", kinds[i]);
    }

    /* Other window events leave keys alone. */
    key(SDL_KEYDOWN, SDLK_UP, 0, 0);
    window(SDL_WINDOWEVENT_FOCUS_GAINED);
    window(SDL_WINDOWEVENT_EXPOSED);
    CHECK(xbox_FramebufferKeyDown(0x26), "focus gained / exposed released a key");
    xbox_InputKeysClear();
}

int main(void)
{
    /* The dummy video driver (set by ctest) is enough for SDL_StopTextInput. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    xbox_InputSdlInit();
    CHECK(!SDL_IsTextInputActive(), "text input still active after init");
    test_keycodes();
    test_events();
    SDL_Quit();
    printf("input_keyboard_sdl: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

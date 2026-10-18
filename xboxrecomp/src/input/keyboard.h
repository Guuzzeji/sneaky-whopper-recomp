/**
 * xbox_input keyboard -- the host keyboard as a port-0 pad.
 *
 * One key table and one key map for every host. The table is indexed by
 * Windows virtual-key codes, because that is what the Win32 windows already
 * receive; the SDL window (keyboard_sdl.c) turns SDL keycodes into the same
 * codes. Writers: the GDI window (fb_present.c) and the SDL main loop
 * (fb_present_sdl.c). Reader: xbox_InputGetState() for port 0, through
 * xbox_InputKeyboardPoll().
 *
 * No window or SDL header here, so tests/input_keyboard checks the map on
 * every host.
 */
#ifndef XBOX_INPUT_KEYBOARD_H
#define XBOX_INPUT_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Windows VK codes the map uses (winuser.h). */
#define XBOX_VK_BACK      0x08
#define XBOX_VK_RETURN    0x0D
#define XBOX_VK_SHIFT     0x10
#define XBOX_VK_CONTROL   0x11
#define XBOX_VK_LEFT      0x25
#define XBOX_VK_UP        0x26
#define XBOX_VK_RIGHT     0x27
#define XBOX_VK_DOWN      0x28
#define XBOX_VK_NUMPAD2   0x62
#define XBOX_VK_NUMPAD4   0x64
#define XBOX_VK_NUMPAD6   0x66
#define XBOX_VK_NUMPAD8   0x68
/* Letters and digits are their uppercase ASCII codes, as in Win32. */

/* Record a key as held (down != 0) or released. vk outside 0..255 is
 * ignored. */
void xbox_InputKeySet(int vk, int down);
/* Release every key (focus lost). */
void xbox_InputKeysClear(void);
/* Non-zero while that virtual key is held in the game's window. Always zero
 * when there is no window, which is the right answer: with nothing to focus
 * there is nothing to type into. Kept under its old name for usb_gamepad.c's
 * diagnostic. */
int  xbox_FramebufferKeyDown(int vk);

/* The pad the held keys make, through the one map. Moves the packet number
 * on every call, as the title looks for edges. */
void xbox_InputKeyboardState(XBOX_INPUT_STATE *out);
/* 0 when RECOMP_KEYBOARD is off, else 1 with *out filled by
 * xbox_InputKeyboardState(). */
int  xbox_InputKeyboardPoll(XBOX_INPUT_STATE *out);

/* The map, row by row (tests, and the table in README.md). */
enum { XBOX_KEY_BUTTON, XBOX_KEY_ANALOG, XBOX_KEY_AXIS };
enum { XBOX_KEY_LX, XBOX_KEY_LY, XBOX_KEY_RX, XBOX_KEY_RY };
typedef struct xbox_key_row {
    const char *keys;          /* as the README names them */
    uint8_t     vk[2];         /* 0 = no second key */
    uint8_t     kind;          /* XBOX_KEY_* */
    uint16_t    target;        /* wButtons bit, bAnalogButtons index, or axis */
    int8_t      sign;          /* XBOX_KEY_AXIS: -1 or +1 */
    const char *guest;         /* the guest control, as the README names it */
} xbox_key_row;

extern const xbox_key_row xbox_key_map[];
extern const size_t       xbox_key_map_count;

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_KEYBOARD_H */

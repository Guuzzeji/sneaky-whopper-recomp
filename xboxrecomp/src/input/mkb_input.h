/**
 * mkb_input.h -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * The goal: let a recompiled title be played like a native PC FPS --
 * raw mouse deltas drive the aim stick, WASD drives movement, mouse
 * buttons drive the triggers -- regardless of which path the title
 * takes to read its pad (the emulated USB device or the kernel's
 * XInput ordinals), because the merge happens in xbox_InputGetState,
 * which both go through.
 *
 * All knobs are env vars (and an optional mkb.ini), so no rebuild is
 * needed to tune the feel:
 *
 *   RECOMP_MKB=0|1        master switch            (default off, opt-in)
 *   RECOMP_MKB_SENS=f     mouse sensitivity        (default 1.0)
 *   RECOMP_MKB_FULL=n     mouse counts in 1/60 s   (default 8)
 *                         for a full stick throw
 *   RECOMP_MKB_DZ=f       assumed game deadzone    (default 0.18)
 *   RECOMP_MKB_ACC=f      aim acceleration         (default 1.0 = off)
 *   RECOMP_MKB_INVERT_Y=1 invert vertical aim      (default off)
 *   RECOMP_MKB_INI=path   config file (same keys, one per line)
 *
 * The ini also rebinds any pad control: bind.<TARGET>=<INPUT>,<INPUT>
 * (no spaces). TARGET is A B X Y BLACK WHITE LT RT DPAD_UP DPAD_DOWN
 * DPAD_LEFT DPAD_RIGHT START BACK LS RS MOVE_UP MOVE_DOWN MOVE_LEFT
 * MOVE_RIGHT. INPUT is a letter or digit, SPACE ENTER ESCAPE TAB BACKSPACE
 * SHIFT CTRL ALT UP DOWN LEFT RIGHT, MOUSE1..MOUSE5 (left, right, middle,
 * X1, X2), WHEELUP, WHEELDOWN, a virtual-key number (0x..), or NONE.
 * The defaults are an FPS layout: WASD move, mouse aims the right stick,
 * MOUSE1 RT, MOUSE2 LT, SPACE A, E B, R X, MOUSE3/wheel Y.
 *
 * F9 toggles mouse/keyboard capture at runtime. Capture is only
 * applied while the recompiled game's process has the foreground
 * window, so the mouse keeps working normally in other apps.
 */

#ifndef XBOXRECOMP_MKB_INPUT_H
#define XBOXRECOMP_MKB_INPUT_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the layer is enabled (RECOMP_MKB not "0"). Cheap; cached. */
int mkb_enabled(void);

/*
 * Merge the mouse+keyboard virtual pad on top of *pState, in place.
 * Buttons OR in, analog buttons take the larger value, and the aim
 * stick is taken over by the mouse whenever the mouse moved since the
 * last poll. Bumps dwPacketNumber when the virtual pad contributed
 * anything, so a title watching for edges sees the presses.
 *
 * Returns 1 when the virtual pad is active and produced a usable
 * state (so a caller treating "no host pad" as an error can still
 * report a connected controller), 0 otherwise.
 */
int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);

/*
 * Take the raw mouse counts since the last call (Windows convention: +x
 * right, +y down) for a title that turns its camera with them directly.
 * The first call stops the mouse from driving the aim stick. Returns 0,
 * with zero deltas, while the mouse is released (F9) or the game is not
 * in the foreground.
 */
int mkb_take_mouse_delta(long *dx, long *dy);

/*
 * The mouse buttons for the host (1 L, 2 R, 4 M, 8 X1, 16 X2): returns those
 * held, and in *clicked those pressed since the last call. Buttons in `grab`
 * stop driving their pad bindings until a later call leaves them out.
 */
int mkb_mouse_buttons(int grab, int *clicked);

/* A host overlay that wants the mouse and keyboard: while suspended the
 * virtual pad is at rest and the pointer is free. */
void mkb_set_suspended(int on);

/* Show and free the pointer while the title is not in play (a menu, a
 * loading or title screen); keys and buttons still merge. */
void mkb_set_menu_cursor(int on);
/* Keep the move keys off the move stick, and the given bAnalogButtons
 * (bit i = index i) off the pad: a host-side mode reads those keys itself. */
void mkb_set_mute(int move, unsigned analog_mask);

/* Whether the window should hide the pointer (its WM_SETCURSOR): captured,
 * no overlay, no menu pointer. F9 releases; a click captures. */
int mkb_cursor_hidden(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_MKB_INPUT_H */

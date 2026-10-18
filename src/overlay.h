/* Debug overlay (Dear ImGui, src/overlay.cpp): F1 opens the panels, F2 the
 * frame-rate line. Drawn by d3d9_hle.c's present() on the game thread. */
#ifndef SNEAKKING_OVERLAY_H
#define SNEAKKING_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* d3d9_hle.c: per-frame counters (the last presented frame) and caches. */
typedef struct {
    unsigned draws, prims, states, tex_creates, vs_compiles, rt_uploads;
    unsigned rts, rts_max, texs, texs_max, vss, vss_max, vdecls, vdecls_max;
    unsigned scale, sc_w, sc_h, frame;
    unsigned hd_textures;             /* RECOMP_HD_TEX replacements so far */
} OverlayD3dStats;

/* recomp_manual.c: what the game is doing, read from guest memory. */
#define OVERLAY_LEVELS 5
typedef struct {
    uint32_t gamestate;               /* GameState singleton, 0 before boot */
    uint32_t flags;                   /* GameState+0xA4: bits 0-3 unlocked levels */
    uint32_t missions[4];             /* GameState+0x20+L*0x20: mission bits */
    uint32_t world;                   /* CMode_World */
    uint32_t world_state;             /* its state machine (+0x1A68) */
    uint32_t level;                   /* the current level object (+0x1A64) */
    uint32_t levels[OVERLAY_LEVELS];  /* g_levels[], filled by RegisterLevel */
    uint32_t level_type[OVERLAY_LEVELS]; /* [[level+0xD0]+0x50]: 0 logging, 1 suburbia, 2 construction, 3 urban */
    int paused;
    int in_play;                      /* the gameplay camera is reading the stick */
} OverlayGameInfo;

void game_info(OverlayGameInfo *gi);
void game_cheat_unlock_levels(void);
void game_cheat_unlock_all(void);
void game_cheat_warp(int slot);       /* index into g_levels[] */

/* Mouse over the game's menus: the cursor in guest 640x480 coordinates, or
 * x < 0 when it is outside the picture. Returns 1 while a menu is up. */
int menu_mouse_update(int x, int y);

/* The King is under control (the camera stick is being read). */
int game_in_play(void);

/* RECOMP_WIDESCREEN: the guest's 640x480 is anamorphic 16:9 (recomp_manual.c). */
int game_widescreen(void);

void overlay_present(void *dev, void *hwnd, void *backbuffer, const OverlayD3dStats *st);

#ifdef __cplusplus
}
#endif

#endif

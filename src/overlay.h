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

/* Shooter mode (recomp_manual.c): CoD4 in third person. Guns are folders
 * made by tools/cod4_extract.py under SHOOTER_GUN_DIR. */
#define SHOOTER_GUN_DIR "game/mods/guns"
#define SHOOTER_FX_DIR  "game/mods/cod4/fx"
typedef struct {
    int on;                           /* the cheat is enabled */
    int live;                         /* ...the King is in play with a gun in hand */
    char name[32];                    /* the gun in hand, e.g. "AK47" */
    char primary[64], sidearm[64];    /* the loadout: gun folders */
    int ammo, clip;
    float reload;                     /* 0..1 while reloading, else 0 */
    float ads;                        /* 0 hip .. 1 aimed down the sights */
    float spread;                     /* current cone, degrees */
    float fov;                        /* the game's horizontal FOV, degrees */
    float hit;                        /* hit marker, seconds left */
    float sprint;                     /* 0..1 */
    int knockouts;
} ShooterHud;

void game_set_shooter(int on, const char *primary, const char *sidearm);
void shooter_hud(ShooterHud *h);

/* What the world pass draws this frame (row-major D3D matrices, row vectors). */
#define SHOOTER_TRACERS 12
#define SHOOTER_IMPACTS 16
#define SHOOTER_IMPACT_LIFE 0.6f
typedef struct {
    float view[16], proj[16];         /* the game's camera */
    char gun[64];                     /* the gun's folder */
    int draw_gun;
    float gun_world[16];              /* gun.obj (CoD4 inches, X fwd, Z up) -> world */
    float muzzle[3], muzzle_dir[3];   /* world; the barrel's direction (unnormalised) */
    float flash;                      /* muzzle flash, seconds left */
    struct { float a[3], b[3], life; } tracer[SHOOTER_TRACERS];
    struct { float p[3], life; } impact[SHOOTER_IMPACTS];   /* dust where shots hit */
} ShooterScene;

/* mods_mesh.c: the gun, muzzle flash and tracers, in the game's 3D scene:
 * submitted during the frame, drawn by present() before the HUD... */
void gun_world_submit(const ShooterScene *s);
void gun_world_flush(void *dev);
void gun_world_draw(void *dev, const ShooterScene *s);

/* Skate mode (skate.c): the King on a skateboard, Skate 3 style. */
typedef struct {
    int on, live, air, bail;
    int walk;                         /* off the board, carrying it */
    float speed;                      /* King units a second */
    int score, tricks;
    char shown[40];                   /* the last trick (or BAIL!) and its points */
    float show_t;                     /* seconds it stays up */
    float stick[2];                   /* the flick-it stick the mouse drives */
    int manual;                       /* 1 manual, -1 nose manual */
    float balance;                    /* -1..1 while in a manual */
    int combo_n, combo_pts;           /* the line so far: tricks, points */
    char combo_line[64];              /* its tricks' names */
} SkateHud;

void skate_set(int on);
int skate_on(void);
int skate_live(void);
void skate_hud(SkateHud *h);
void skate_king_tick(void);           /* King::Update */
void skate_pose(uint32_t P);          /* before the King's bone build */
void skate_camera(uint32_t cam, float look);   /* after KingCam's target update */
int skate_mouse_tricks(void);         /* the mouse is flicking tricks, not looking */
int skate_walls_open(void);           /* invisible walls let him through */
float skate_fov(float fx);
void skate_scene(void);               /* when the game leaves 3D */
/* recomp_manual.c: citizens near `at` jump back (CitizenSurprised); how many. */
int game_citizens_startle(const float at[3], float radius);
/* recomp_manual.c: the ground's distance below `at` (collision ray), at most range. */
float game_ground_below(const float at[3], float range);

/* mods_mesh.c: the board, drawn at present like the gun (inches, X along
 * the board, Y up). */
void board_world_submit(const float view[16], const float proj[16], const float world[16]);

/* cod4_sound.c: a gun's sound (SHOOTER_GUN_DIR/<gun>/<name>.wav), fire and forget. */
int snd_play(const char *gun, const char *name);   /* 0 when there is no such sound */

/* cod4_sound.c: 16-bit mono PCM made in code (skate mode). */
#define SND_PCM_RATE 22050
void snd_pcm_play(const short *pcm, int n, float vol, float pitch);
void snd_pcm_loop(const short *pcm, int n, float vol, float pitch);

#ifdef __cplusplus
}
#endif

#endif

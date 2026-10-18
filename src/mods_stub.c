/*
 * mods_stub.c: the joke modes' host side for the gl renderer (native GL and
 * the web build). The gun, effects and board are drawn by mods_mesh.c on
 * Direct3D 9 and the sounds play through XAudio2 (cod4_sound.c), neither of
 * which exists here, so shooter and skate mode report themselves unavailable
 * (shooter_available in recomp_manual.c, skate_available below) and these
 * entry points do nothing. The game-side cheats (unlocks, warp) still work.
 *
 * ponytail: modes off on gl; port mods_mesh.c to GL and the sounds to the
 * APU or Web Audio when they are wanted in the browser.
 */
#include <stdint.h>
#include "overlay.h"

void gun_world_submit(const ShooterScene *s) { (void)s; }
void board_world_submit(const float view[16], const float proj[16], const float world[16])
{
    (void)view; (void)proj; (void)world;
}

int snd_play(const char *gun, const char *name) { (void)gun; (void)name; return 0; }
void snd_pcm_play(const short *pcm, int n, float vol, float pitch) { (void)pcm; (void)n; (void)vol; (void)pitch; }
void snd_pcm_loop(const short *pcm, int n, float vol, float pitch) { (void)pcm; (void)n; (void)vol; (void)pitch; }

void skate_set(int on) { (void)on; }
int skate_available(void) { return 0; }
int skate_on(void) { return 0; }
int skate_live(void) { return 0; }
void skate_hud(SkateHud *h) { (void)h; }
void skate_king_tick(void) {}
void skate_pose(uint32_t P) { (void)P; }
void skate_camera(uint32_t cam, float look) { (void)cam; (void)look; }
int skate_mouse_tricks(void) { return 0; }
int skate_walls_open(void) { return 0; }
float skate_fov(float fx) { return fx; }
void skate_scene(void) {}

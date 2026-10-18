/* CoD4 player animations on the King (src/cod4_anim.c). */
#ifndef SNEAKKING_COD4_ANIM_H
#define SNEAKKING_COD4_ANIM_H

#define CANIM_BONES 85                /* the King's skeleton */
#define CANIM_FILE  "game/mods/cod4/anims.bin"

/* Bones of the King's skeleton (palette order) the modes use. */
enum { KB_ORIGIN = 1, KB_SPINE1 = 8, KB_SPINE2 = 9, KB_SPINE3 = 10, KB_RWRIST = 14,
       KB_NECK = 30, KB_HEAD = 31, KB_LWRIST = 35 };

enum { CANIM_LOOP = 1, CANIM_TORSO = 2 };

typedef struct {
    float q[CANIM_BONES][4];          /* local rotations, engine convention */
    unsigned char has[CANIM_BONES];   /* which bones the pose sets */
    float origin[3];                  /* the Origin (pelvis) bone, model space */
    float grip_q[4], grip_p[3];       /* the gun relative to the right wrist */
    float weight;                     /* accumulated by canim_add */
} KingPose;

int canim_load(const char *path);     /* clip count, 0 if missing */
int canim_find(const char *name);     /* clip index, or -1 */
float canim_length(int clip);         /* seconds */
int canim_flags(int clip);
float canim_speed(int clip);          /* root motion, King units per second */

void canim_sample(int clip, float t, int loop, KingPose *out);
void canim_clear(KingPose *p);
void canim_add(KingPose *acc, const KingPose *p, float w);   /* weighted sum... */
void canim_finish(KingPose *p);                               /* ...then normalise */
void canim_mix(KingPose *base, const KingPose *top, float w, int grip);  /* top over base */
void canim_mix_origin(KingPose *base, const KingPose *top, float w);

#endif

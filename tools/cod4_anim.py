"""Retarget CoD4 (MW1, PC) player animations onto the Burger King.

    python tools/cod4_anim.py [--mw1 mw1] [--game game] [--out game/mods/cod4/anims.bin]
                              [--png DIR clip:frame ...]

Reads the player animations from the user's own install (common_mp.ff), the
CoD4 body skeleton they were made for (body_mp_usmc_assault in mp_backlot.ff)
and the King's skeleton (his bone definitions in L_KingMe_World.xbp), and
writes every clip as King bone rotations, ready for the shooter mode's
animation player (src/recomp_manual.c). Output stays under game/, which is
gitignored. --png draws stick figures of some frames to check a retarget.

How the retarget works (docs/MW1_NOTES.md has the formats; the retarget step
itself is shared with Skate 3 in tools/king_retarget.py):
  * Both skeletons go into the King's model space: Y up, facing -Z, right
    hand at -X (a mirror of CoD4's axes), units about 1 cm. CoD4 is X
    forward, Y left, Z up, inches.
  * The King's bind pose is a T-pose and CoD4's an A-pose, so each mapped
    King bone's rest is first turned (shortest arc) to point the way the
    CoD4 bone points at rest. A frame is then CoD4's rotation from its rest,
    applied to that aligned King rest, in world space.
  * The pelvis moves by CoD4's pelvis offset scaled to the King's height;
    root motion (the delta part) is kept only as each clip's speed, so the
    player can match playback to how fast the King really moves.
"""
import argparse, math, os, re, struct, sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cod4_extract as ce          # noqa: E402
import cod4_xanim as xa            # noqa: E402
from king_retarget import (KING_BONES, KING_PARENT_OF_MAPPED, engine_quat, fk,  # noqa: E402,F401
                           m2q, stick_png, write)
import king_retarget as kr         # noqa: E402

# CoD4 body bone -> King bone (index in the King's 85-bone skeleton).
BONE_MAP = {
    'pelvis': 1, 'j_spinelower': 8, 'j_spineupper': 9, 'j_spine4': 10,
    'j_neck': 30, 'j_head': 31,
    'j_clavicle_ri': 11, 'j_shoulder_ri': 12, 'j_elbow_ri': 13, 'j_wrist_ri': 14,
    'j_clavicle_le': 32, 'j_shoulder_le': 33, 'j_elbow_le': 34, 'j_wrist_le': 35,
    'j_hip_ri': 75, 'j_knee_ri': 76, 'j_ankle_ri': 77, 'j_ball_ri': 78,
    'j_hip_le': 79, 'j_knee_le': 80, 'j_ankle_le': 81, 'j_ball_le': 82,
}
# Which child each mapped bone points at, to align the rests: CoD4 name, King index.
AIM = {
    'pelvis': ('j_spinelower', 8), 'j_spinelower': ('j_spineupper', 9),
    'j_spineupper': ('j_spine4', 10), 'j_spine4': ('j_neck', 30), 'j_neck': ('j_head', 31),
    'j_clavicle_ri': ('j_shoulder_ri', 12), 'j_shoulder_ri': ('j_elbow_ri', 13),
    'j_elbow_ri': ('j_wrist_ri', 14),
    'j_clavicle_le': ('j_shoulder_le', 33), 'j_shoulder_le': ('j_elbow_le', 34),
    'j_elbow_le': ('j_wrist_le', 35),
    'j_hip_ri': ('j_knee_ri', 76), 'j_knee_ri': ('j_ankle_ri', 77), 'j_ankle_ri': ('j_ball_ri', 78),
    'j_hip_le': ('j_knee_le', 80), 'j_knee_le': ('j_ankle_le', 81), 'j_ankle_le': ('j_ball_le', 82),
}

# CoD4 axes to the King's: v_king = C @ v_cod (forward X->-Z, left Y->+X, up Z->+Y).
# The King's model space is the mirror of CoD4's (D3D is left-handed), so C is
# a reflection; C R C^T of a rotation is still a rotation.
C = np.array([[0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [-1.0, 0.0, 0.0]])

CLIPS = [
    # rifle
    'pb_stand_alert', 'pb_stand_ads',
    'pb_stand_shoot_walk_forward', 'pb_stand_shoot_walk_back',
    'pb_stand_shoot_walk_left', 'pb_stand_shoot_walk_right',
    'pb_combatrun_forward_loop', 'pb_combatrun_back_loop',
    'pb_combatrun_left_loop', 'pb_combatrun_right_loop', 'pb_sprint',
    'pb_crouch_alert', 'pb_crouch_ads',
    'pb_crouch_shoot_run_forward', 'pb_crouch_shoot_run_back',
    'pb_crouch_shoot_run_left', 'pb_crouch_shoot_run_right',
    'pt_stand_shoot', 'pt_stand_shoot_ads', 'pt_crouch_shoot', 'pt_crouch_shoot_ads',
    'pt_reload_stand_auto', 'pt_reload_stand_rifle', 'pt_reload_crouch_rifle',
    'pt_stand_pullout_pose',
    # pistol
    'pb_stand_alert_pistol', 'pb_stand_ads_pistol',
    'pb_combatwalk_forward_loop_pistol', 'pb_combatwalk_back_loop_pistol',
    'pb_combatwalk_left_loop_pistol', 'pb_combatwalk_right_loop_pistol',
    'pb_combatrun_forward_loop_pistol', 'pb_combatrun_back_loop_pistol',
    'pb_combatrun_left_loop_pistol', 'pb_combatrun_right_loop_pistol', 'pb_sprint_pistol',
    'pb_crouch_alert_pistol', 'pb_crouch_ads_pistol',
    'pb_crouch_walk_forward_pistol', 'pb_crouch_walk_back_pistol',
    'pb_crouch_walk_left_pistol', 'pb_crouch_walk_right_pistol',
    'pt_stand_shoot_pistol', 'pt_crouch_shoot_pistol', 'pt_reload_stand_pistol',
    'pt_reload_crouch_pistol',
    # deaths, for citizens
    'pb_stand_death_frontspin', 'pb_stand_death_head_collapse', 'pb_stand_death_headchest_topple',
    'pb_stand_death_legs', 'pb_stand_death_lowerback', 'pb_stand_death_neckdeath',
    'pb_stand_death_nervedeath', 'pb_death_run_forward_crumple', 'pb_death_run_onfront',
]


def cod_skeleton(z, name):
    """A CoD4 body model's bones: names, parents, rest local quats and positions."""
    strs = ce.zone_strings(z)
    for m in re.finditer(re.escape(name.encode() + b'\0'), z):
        i = m.start()
        s = i - ce.XMODEL_SIZE
        if s >= 0 and z[s:s + 4] == b'\xff' * 4 and z[s + 8:s + 12] == b'\xff' * 4:
            break
    else:
        sys.exit(f'{name} not found')
    nb, nr = z[s + 4], z[s + 5]
    n = nb - nr
    p = i + len(name) + 1
    names = [strs[k] for k in struct.unpack_from(f'<{nb}H', z, p)]
    p += 2 * nb
    par = list(z[p:p + n])
    p += n
    quats = [xa.nrm(tuple(c / 32767 for c in struct.unpack_from('<4h', z, p + 8 * k))) for k in range(n)]
    p += 8 * n
    trans = [struct.unpack_from('<3f', z, p + 12 * k) for k in range(n)]
    parents = [-1] * nr + [nr + k - par[k] for k in range(n)]
    lq = [(0.0, 0.0, 0.0, 1.0)] * nr + quats
    lt = [(0.0, 0.0, 0.0)] * nr + trans
    return names, parents, lq, lt


# -- retarget ------------------------------------------------------------------

class Retarget(kr.Retarget):
    """The shared retarget (tools/king_retarget.py) fed from the CoD4 body
    skeleton; frame() also gives the gun grip."""

    def __init__(self, z_body, game):
        self.names, self.parents, self.lq, self.lt = cod_skeleton(z_body, 'body_mp_usmc_assault')
        self.idx = {n: i for i, n in enumerate(self.names)}
        missing = [b for b in BONE_MAP if b not in self.idx]
        if missing:
            sys.exit(f'CoD4 body skeleton lacks {missing}')
        if 'tag_weapon_right' not in self.idx:
            sys.exit('CoD4 body skeleton lacks tag_weapon_right')
        wr, wp = fk(self.parents, self.lq, self.lt)
        super().__init__(game, BONE_MAP, AIM,
                         {b: C @ wr[self.idx[b]] @ C.T for b in BONE_MAP},
                         {b: C @ wp[self.idx[b]] for b in BONE_MAP}, 'pelvis')

    def frame(self, anim, f):
        """King local rotations {bone: 3x3}, the Origin position and the grip for frame f."""
        lq, lt = list(self.lq), list(self.lt)
        for bi, bone in enumerate(anim['bones']):
            i = self.idx.get(bone)
            if i is None:
                continue
            if anim['rot'][bi]:
                lq[i] = xa.nrm(xa.sample(anim['rot'][bi], f))
            if anim['trans'][bi]:
                d = xa.sample(anim['trans'][bi], f)
                lt[i] = tuple(r + v for r, v in zip(self.lt[i], d))
        wr, wp = fk(self.parents, lq, lt)
        local, origin, world = self.pose({b: C @ wr[self.idx[b]] @ C.T for b in BONE_MAP},
                                         {b: C @ wp[self.idx[b]] for b in BONE_MAP})
        # the gun: CoD4's tag_weapon_right relative to the right wrist, in the
        # King's wrist frame (gun meshes are turned to King axes and scaled)
        t, w = self.idx['tag_weapon_right'], self.idx['j_wrist_ri']
        grip_r = world[14].T @ (C @ wr[t] @ C.T)
        grip_p = world[14].T @ (C @ (wp[t] - wp[w])) * self.scale
        return local, origin, (m2q(grip_r), tuple(grip_p))


def clip_speed(anim, scale):
    """Root motion over the clip, King units per second, in the King's (x, z)."""
    dp = anim['deltapart'] or {}
    if 'trans' not in dp or len(dp['trans'][1]) < 2 or not anim['frames']:
        return 0.0, 0.0
    a, b = np.array(dp['trans'][1][0]), np.array(dp['trans'][1][-1])
    d = C @ (b - a) * scale
    secs = anim['frames'] / anim['rate']
    return float(d[0] / secs), float(d[2] / secs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mw1', default='mw1')
    ap.add_argument('--game', default='game')
    ap.add_argument('--out', default='game/mods/cod4/anims.bin')
    ap.add_argument('--png', nargs='+', metavar=('DIR', 'CLIP:FRAME'))
    a = ap.parse_args()

    zone = os.path.join(a.mw1, 'zone', 'english')
    print('loading mp_backlot.ff (body skeleton) ...')
    rt = Retarget(ce.load_zone(os.path.join(zone, 'mp_backlot.ff')), a.game)
    print(f'  CoD4 -> King scale {rt.scale:.3f}')
    print('loading common_mp.ff (animations) ...')
    z = ce.load_zone(os.path.join(zone, 'common_mp.ff'))
    strs = xa.zone_strings(z)
    anims = {}
    want = set(CLIPS)
    for s in xa.find(z):
        try:
            an = xa.parse(z, s, strs)
        except (ValueError, struct.error, IndexError):
            continue
        if an['name'] in want:
            anims[an['name']] = an

    if a.png:
        os.makedirs(a.png[0], exist_ok=True)
        by = {}
        for spec in a.png[1:]:
            name, _, f = spec.partition(':')
            by.setdefault(name, []).append(int(f or 0))
        for name, frames in by.items():
            path = os.path.join(a.png[0], f'{name}.bmp')
            stick_png(path, rt, [rt.frame(anims[name], f)[:2] for f in frames])
            print('wrote', path)
        return

    clips = []
    for name in CLIPS:
        an = anims.get(name)
        if not an:
            print(f'  {name}: not in common_mp.ff, skipped')
            continue
        driven = {BONE_MAP[b] for b in an['bones'] if b in BONE_MAP}
        torso = name.startswith('pt_')
        if not torso:
            driven |= set(BONE_MAP.values())
        frames = []
        for f in range(an['frames'] + 1):
            local, origin, grip = rt.frame(an, f)
            frames.append(({k: engine_quat(m) for k, m in local.items() if k in driven}, tuple(origin), grip))
        clips.append(dict(name=name, frames=frames, fps=an['rate'],
                          flags=(1 if an['loop'] else 0) | (2 if torso else 0),
                          speed=clip_speed(an, rt.scale), bones=driven))
        print(f'  {name}: {len(frames)} keys at {an["rate"]:.0f} fps, {len(driven)} bones'
              + (f', speed {math.hypot(*clips[-1]["speed"]):.0f}/s' if any(clips[-1]['speed']) else ''))
    size = write(a.out, clips)
    print(f'wrote {a.out}: {len(clips)} clips, {size / 1e6:.1f} MB')


if __name__ == '__main__':
    main()

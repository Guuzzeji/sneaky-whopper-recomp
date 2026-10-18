"""Retarget Skate 3's skater animations onto the Burger King (skate mode).

    .venv\\Scripts\\python.exe tools\\skate3_anim.py [--skate3 skate3] [--game game]
        [--toolkit <skate-3-trick-toolkit>\\skate3_anim\\scripts]
        [--out game/mods/skate3/anims.bin] [--png DIR clip:frame ...]

Reads OnBoard.abin from the user's own disc (data/big/miscload.big), decodes
the clips below and writes them as King bone rotations for src/skate.c, the
same 'SKA1' file CoD4's clips use (tools/king_retarget.py does the retarget).
It also writes the board's mesh (board.bin, from a pro's board in
marquee.big). Output stays under game/, which is gitignored.

The .abin decoder (EA ANT "VBR" and "RAW" codecs) is not ours: it is
abin_importer.py from chasmlol/skate-3-trick-toolkit, which the user clones
next to this repo; --toolkit points at its scripts folder. That repository has
no licence, so it is used from the user's checkout, not copied here.
docs/SKATE3_NOTES.md has what the format looks like.

How a Skate 3 frame becomes a King pose:
  * The skeleton is the compact 36-channel on-board one (TRAJECTORY, HIPS,
    SPINE..HEAD, arms, legs, SKATEBOARD_ROOT, trucks, wheels, two toe
    targets). RIG_TPOSE gives each bone's reference rotation and offset.
  * A clip channel is a delta on that reference (row vectors in Skate:
    local = delta * reference), so in column form R = R_ref R_delta and
    t = R_ref t_delta + t_ref. Missing channels stay at the reference.
  * TRAJECTORY carries the clip's root motion and sits where the wheels meet
    the ground (SKATEBOARD_ROOT is on it); every bone is taken relative to it,
    since the King moves by physics.
  * Skate is right-handed (Y up, the rider facing +Z, right hand at -X); the
    King is left-handed (facing -Z, right hand at -X): C flips Z. The board
    runs along Skate's Z, its nose (TRUCK_FRONT) at +Z, so the King's forward
    is the board's travel and the clip turns his body side-on to it.
  * The board (SKATEBOARD_ROOT) goes into each frame's prop slot, in King
    model space, scaled like the body: skate.c draws the board there, so
    flips and shove-its are Skate 3's own.
"""
import argparse
import contextlib
import io
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import king_retarget as kr  # noqa: E402
import skate3_big           # noqa: E402

TOOLKIT = r'C:\repos\_research\skate-3-trick-toolkit\skate3_anim\scripts'
MISCLOAD = os.path.join('data', 'big', 'miscload.big')
MARQUEE = os.path.join('data', 'content', 'marquee.big')
BOARD = 'content/marquee/model/mike_carroll/SkateBoard/0x000007ba03e38811.rx2'


def board_mesh(skate3, toolkit, out):
    """A pro's board from marquee.big (RenderWare 4, read by the toolkit's
    rx2_skeleton.py) as board.bin for src/mods_mesh.c, little endian:
    u32 triangle count, then 3 vertices a triangle, each f32 pos[3] (inches,
    X along the board to the nose, Y up from where the wheels touch, Z
    across), f32 normal[3], u32 colour (D3DCOLOR). The textures aren't decoded,
    so the parts are coloured: grip on top, the graphic underneath, wood at
    the edges, metal trucks, white wheels."""
    import struct
    sys.path.insert(0, toolkit)
    with contextlib.redirect_stdout(io.StringIO()):
        import rx2_skeleton
    big = os.path.join(skate3, MARQUEE)
    for name, off, packed, size in skate3_big.entries(big):
        if name == BOARD:
            break
    else:
        sys.exit(f'{BOARD} not in {big}')
    tmp = out + '.rx2'
    open(tmp, 'wb').write(skate3_big.read(big, off, packed, size))
    with contextlib.redirect_stdout(io.StringIO()):
        m = rx2_skeleton.parse_rx2_meshes(tmp)[0]
    os.remove(tmp)
    pos, nrm, idx = m['positions'], m['normals'], m['indices']
    inch = 1 / 0.0254
    data = b''
    for t in range(0, len(idx) - 2, 3):
        tri = idx[t:t + 3]
        cy = sum(pos[i][1] for i in tri) / 3
        cx = max(abs(pos[i][0]) for i in tri)
        ny = sum(nrm[i][1] for i in tri) / 3
        if cy < 0.06:                  # under the deck: wheels out wide, trucks in the middle
            col = 0xFFF0ECDC if cx > 0.065 else 0xFFAAACB2
        else:
            col = 0xFF1C1C1E if ny > 0.5 else 0xFFCD2823 if ny < -0.5 else 0xFFC49660
        for i in tri:
            p, n = pos[i], nrm[i]
            data += struct.pack('<6fI', p[2] * inch, p[1] * inch, p[0] * inch, n[2], n[1], n[0], col)
    open(out, 'wb').write(struct.pack('<I', len(idx) // 3) + data)
    print(f'wrote {out}: {len(idx) // 3} triangles')

# Skate 3 bone (channel index in the on-board skeleton) -> King bone.
NAMES = ['TRAJECTORY', 'HIPS', 'SPINE', 'SPINE1', 'SPINE2', 'SPINE3', 'NECK', 'NECK1', 'HEAD',
         'RIGHTSHOULDER', 'RIGHTARM', 'RIGHTFOREARM', 'RIGHTHAND',
         'LEFTSHOULDER', 'LEFTARM', 'LEFTFOREARM', 'LEFTHAND',
         'RIGHTUPLEG', 'RIGHTLEG', 'RIGHTFOOT', 'RIGHTTOEBASE',
         'LEFTUPLEG', 'LEFTLEG', 'LEFTFOOT', 'LEFTTOEBASE', 'SKATEBOARD_ROOT']
BONE_MAP = {
    'HIPS': 1, 'SPINE': 8, 'SPINE2': 9, 'SPINE3': 10, 'NECK': 30, 'HEAD': 31,
    'RIGHTSHOULDER': 11, 'RIGHTARM': 12, 'RIGHTFOREARM': 13, 'RIGHTHAND': 14,
    'LEFTSHOULDER': 32, 'LEFTARM': 33, 'LEFTFOREARM': 34, 'LEFTHAND': 35,
    'RIGHTUPLEG': 75, 'RIGHTLEG': 76, 'RIGHTFOOT': 77, 'RIGHTTOEBASE': 78,
    'LEFTUPLEG': 79, 'LEFTLEG': 80, 'LEFTFOOT': 81, 'LEFTTOEBASE': 82,
}
AIM = {
    'HIPS': ('SPINE', 8), 'SPINE': ('SPINE2', 9), 'SPINE2': ('SPINE3', 10), 'SPINE3': ('NECK', 30),
    'NECK': ('HEAD', 31),
    'RIGHTSHOULDER': ('RIGHTARM', 12), 'RIGHTARM': ('RIGHTFOREARM', 13), 'RIGHTFOREARM': ('RIGHTHAND', 14),
    'LEFTSHOULDER': ('LEFTARM', 33), 'LEFTARM': ('LEFTFOREARM', 34), 'LEFTFOREARM': ('LEFTHAND', 35),
    'RIGHTUPLEG': ('RIGHTLEG', 76), 'RIGHTLEG': ('RIGHTFOOT', 77), 'RIGHTFOOT': ('RIGHTTOEBASE', 78),
    'LEFTUPLEG': ('LEFTLEG', 80), 'LEFTLEG': ('LEFTFOOT', 81), 'LEFTFOOT': ('LEFTTOEBASE', 82),
}
C = np.diag([1.0, 1.0, -1.0])          # Skate (right-handed) -> King (left-handed)

# Our name -> (Skate 3 clip, flags). Flag 1 loops. A trick is its ground
# part (the pop, _G) then its air part (_A), played back to back.
CLIPS = {
    'idle': ('R_IDLE_RIDE_N_0_CYC', 1),
    'lean_l': ('R_IDLE_HCOM_N100', 1),
    'lean_r': ('R_IDLE_HCOM_P100', 1),
    'crouch': ('R_ANTIC_OLLIE_N_0_CYC', 1),
    'push': ('R_PUSHLSP_LSTR_LEFT_0_CYC2', 1),
    'land': ('L_LAND_HIGH_LOOSE_1_N', 0),
    'manual': ('M_IDLE_N_0_CYC', 1),
    'nosemanual': ('M_NOSEIDLE_N_0_CYC', 1),
    'grind': ('G_5050_FS_LOW_0_CYC', 1),
    'melon': ('GR_MELON_N_BS_0_CYC', 1),
    'method': ('GR_METHOD_N_BS_0_CYC', 1),
    'mute': ('GR_MUTEGRAB_N_0_CYC', 1),
}
for ours, theirs in (('ollie', 'OLLIE_LOW'), ('nollie', 'NOLLIE_LOW'), ('kickflip', 'KICKFLIP_IN_LOW'),
                     ('heelflip', 'HEELFLIP_IN_LOW'), ('shuvit', 'POPSHUVIT_LOW'),
                     ('varialkick', 'VARIALKICKFLIP_LOW'), ('varialheel', 'VARIALHEELFLIP_D_LOW'),
                     ('360flip', '360FLIP_D_LOW')):
    CLIPS[ours + '_g'] = (theirs + '_G', 0)
    CLIPS[ours + '_a'] = (theirs + '_A', 0)


class Source:
    def __init__(self, abin_path, toolkit):
        sys.path.insert(0, toolkit)
        with contextlib.redirect_stdout(io.StringIO()):
            import abin_importer       # noqa: E402  (the user's checkout)
        self.A = abin_importer
        self.data = open(abin_path, 'rb').read()
        with contextlib.redirect_stdout(io.StringIO()):
            self.abin = abin_importer.AbinFile(self.data)
        self.hier = self.abin.hierarchy
        self.parents = list(self.hier.parents)
        tpose = next(p for p in self.abin.poses if p.header.name == 'RIG_TPOSE')
        self.ref = abin_importer.decode_pose_frame(self.data, tpose, self.hier)
        self.ref_q = [self.ref[i].quat if i in self.ref else (0, 0, 0, 1) for i in range(len(self.parents))]
        self.ref_t = [self.ref[i].trans if i in self.ref else (0, 0, 0) for i in range(len(self.parents))]
        self.clips = {c.header.name: c for c in self.abin.clips}

    def world(self, frame):
        """World rotations and positions (King axes, metres) of every channel."""
        lq, lt = [], []
        for i in range(len(self.parents)):
            r_ref = kr.q2m(self.ref_q[i])
            s = frame.get(i)
            if s is None:
                lq.append(self.ref_q[i])
                lt.append(self.ref_t[i])
            else:
                lq.append(kr.m2q(r_ref @ kr.q2m(s.quat)))
                lt.append(tuple(r_ref @ np.array(s.trans) + np.array(self.ref_t[i])))
        wr, wp = kr.fk(self.parents, lq, lt)
        # relative to the trajectory: the clip's root motion, and the point
        # where the wheels meet the ground (SKATEBOARD_ROOT sits on it)
        tr, tp = wr[0].T, np.array(wp[0])
        return ({NAMES[i]: C @ tr @ wr[i] @ C for i in range(len(NAMES))},
                {NAMES[i]: C @ tr @ (wp[i] - tp) for i in range(len(NAMES))})

    def frames(self, name):
        clip = self.clips[name]
        dec = {}
        for f in range(clip.num_frames):
            with contextlib.redirect_stdout(io.StringIO()):
                fr = self.A.decode_clip_frame(self.data, clip, dec, f, self.hier)
            yield self.world(fr)
        self.fps = clip.fps

    def rest(self):
        return self.world({})


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--skate3', default='skate3')
    ap.add_argument('--game', default='game')
    ap.add_argument('--toolkit', default=TOOLKIT)
    ap.add_argument('--out', default=os.path.join('game', 'mods', 'skate3', 'anims.bin'))
    ap.add_argument('--board', default=os.path.join('game', 'mods', 'skate3', 'board.bin'))
    ap.add_argument('--png', nargs='+', metavar=('DIR', 'CLIP:FRAME'))
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.toolkit, 'abin_importer.py')):
        sys.exit(f'no abin_importer.py in {a.toolkit}: clone chasmlol/skate-3-trick-toolkit and pass --toolkit')

    raw = os.path.join(os.path.dirname(a.out), 'raw', 'OnBoard.abin')
    if not os.path.isfile(raw):
        big = os.path.join(a.skate3, MISCLOAD)
        for name, off, packed, size in skate3_big.entries(big):
            if name.lower().endswith('/anim/onboard.abin'):
                os.makedirs(os.path.dirname(raw), exist_ok=True)
                open(raw, 'wb').write(skate3_big.read(big, off, packed, size))
                break
        else:
            sys.exit(f'OnBoard.abin not in {big}')
    print('decoding', raw, '...')
    src = Source(raw, a.toolkit)
    rot, pos = src.rest()
    rt = kr.Retarget(a.game, BONE_MAP, AIM, rot, pos, 'HIPS')
    print(f'  Skate 3 -> King scale {rt.scale:.1f}')

    def king(world):
        wr, wp = world
        local, origin, _ = rt.pose(wr, wp)
        board = (kr.m2q(wr['SKATEBOARD_ROOT']), tuple(wp['SKATEBOARD_ROOT'] * rt.scale))
        return local, origin, board

    if a.png:
        os.makedirs(a.png[0], exist_ok=True)
        by = {}
        for spec in a.png[1:]:
            name, _, f = spec.partition(':')
            by.setdefault(name, []).append(int(f or 0))
        for name, frames in by.items():
            fr = list(src.frames(CLIPS.get(name, (name,))[0]))
            path = os.path.join(a.png[0], f'{name}.bmp')
            kr.stick_png(path, rt, [king(fr[min(f, len(fr) - 1)])[:2] for f in frames])
            print('wrote', path)
        return

    clips = []
    for ours, (theirs, flags) in CLIPS.items():
        if theirs not in src.clips:
            print(f'  {ours}: {theirs} not in OnBoard.abin, skipped')
            continue
        frames = []
        for world in src.frames(theirs):
            local, origin, board = king(world)
            frames.append(({k: kr.engine_quat(m) for k, m in local.items()}, tuple(origin), board))
        if flags & 1:                  # loops end on their first key
            frames.append(frames[0])
        clips.append(dict(name=ours, frames=frames, fps=src.fps, flags=flags, speed=(0.0, 0.0),
                          bones=set(BONE_MAP.values())))
        print(f'  {ours:11} <- {theirs}: {len(frames)} keys at {src.fps:.0f} fps')
    size = kr.write(a.out, clips)
    print(f'wrote {a.out}: {len(clips)} clips, {size / 1e6:.1f} MB')
    board_mesh(a.skate3, a.toolkit, a.board)


if __name__ == '__main__':
    main()

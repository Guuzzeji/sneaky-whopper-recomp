"""Retarget another game's skeletal animation onto the Burger King (shared step).

The per-game readers (tools/cod4_anim.py for CoD4, tools/skate3_anim.py for
Skate 3) turn a source frame into world rotations and positions of named
source bones, already in the King's model axes (Y up, facing -Z, right hand
at -X). This module does the rest the same way for every source:

  * The King's skeleton comes from his bone definitions in L_KingMe_World.xbp.
  * Each mapped King bone's rest is turned (shortest arc) to point the way the
    source bone points at rest, so T-pose, A-pose or anything else lines up.
  * A frame is the source's world rotation from its rest, applied to that
    aligned King rest; locals follow from the King's own parents.
  * The pelvis moves by the source pelvis offset scaled to the King's height.

write() stores clips in the 'SKA1' file that src/cod4_anim.c plays.
"""
import math
import os
import struct
import sys

import numpy as np

KING_BONES = 85
# The King bones a source can drive and their parents in that subset.
KING_PARENT_OF_MAPPED = {1: -1, 8: 1, 9: 8, 10: 9, 30: 10, 31: 30, 11: 10, 12: 11, 13: 12, 14: 13,
                         32: 10, 33: 32, 34: 33, 35: 34, 75: 1, 76: 75, 77: 76, 78: 77,
                         79: 1, 80: 79, 81: 80, 82: 81}


# -- quaternions (x, y, z, w), column-vector Hamilton convention ---------------

def q2m(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def m2q(m):
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = ((m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s)
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = (0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s)
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = ((m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s)
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = ((m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s)
    n = math.sqrt(sum(c * c for c in q))
    return tuple(c / n for c in q)


def arc(a, b):
    """Shortest rotation (matrix) taking direction a onto direction b."""
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if c < -0.9999:
        axis = np.cross(a, [1.0, 0.0, 0.0])
        if np.linalg.norm(axis) < 1e-3:
            axis = np.cross(a, [0.0, 1.0, 0.0])
        axis /= np.linalg.norm(axis)
        return 2 * np.outer(axis, axis) - np.eye(3)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def fk(parents, lq, lt):
    """World rotation matrices (column) and positions from local quats and positions."""
    wr, wp = [], []
    for i, p in enumerate(parents):
        r = q2m(lq[i])
        if p < 0:
            wr.append(r)
            wp.append(np.array(lt[i], dtype=float))
        else:
            wr.append(wr[p] @ r)
            wp.append(wp[p] + wr[p] @ np.array(lt[i], dtype=float))
    return wr, wp


# -- the King --------------------------------------------------------------------

def king_skeleton(game):
    """The King's 85 bones in model space: world rotations (column) and positions,
    in palette order, from his bone defs (walked from Motion)."""
    b = open(os.path.join(game, 'Packages', 'L_KingMe_World.xbp'), 'rb').read()
    reloc = 0x306100                   # package pointer -> file offset
    root = 0x8267B0                    # the Motion bone's def
    rot, pos = [None] * KING_BONES, [None] * KING_BONES

    def visit(o, first):
        while o:
            pal, = struct.unpack_from('<I', b, o + 0xC0)
            m = np.array(struct.unpack_from('<16f', b, o + 0x80)).reshape(4, 4)
            w = np.linalg.inv(m)       # bone -> model, row vectors
            rot[pal] = w[:3, :3].T
            pos[pal] = w[3, :3].copy()
            nxt, _, _, child = struct.unpack_from('<4I', b, o + 0x110)
            if child:
                visit(child + reloc, child + reloc)
            o = nxt + reloc if nxt and nxt + reloc != first else 0
            if first == root:
                break
    visit(root, root)
    if any(r is None for r in rot):
        sys.exit('King skeleton: not all 85 bones found (different game build?)')
    return rot, pos


class Retarget:
    """bone_map: source bone -> King bone. aim: source bone -> (source child,
    King child) used to line the rests up. rest_rot / rest_pos: the source's
    rest pose, world, King axes. pelvis: the source bone that maps to King 1."""

    def __init__(self, game, bone_map, aim, rest_rot, rest_pos, pelvis):
        self.bone_map, self.pelvis = bone_map, pelvis
        self.k_rot, self.k_pos = king_skeleton(game)
        self.c_rest = {b: rest_rot[b] for b in bone_map}
        self.c_pos = {b: rest_pos[b] for b in bone_map}
        self.scale = self.k_pos[1][1] / self.c_pos[pelvis][1]
        self.k_aligned = {}
        for b, k in bone_map.items():
            if b in aim:
                cb, ck = aim[b]
                a = arc(self.k_pos[ck] - self.k_pos[k], rest_pos[cb] - rest_pos[b])
            else:                      # no child to aim at: keep the parent's turn
                pk = KING_PARENT_OF_MAPPED[k]
                pb = next(n for n, i in bone_map.items() if i == pk)
                a = self.k_aligned[pb] @ self.k_rot[pk].T
            self.k_aligned[b] = a @ self.k_rot[k]

    def pose(self, world_rot, world_pos):
        """King local rotations {bone: 3x3}, the Origin position and the King
        world rotations {bone: 3x3}, from source world rotations/positions."""
        world = {k: world_rot[b] @ self.c_rest[b].T @ self.k_aligned[b] for b, k in self.bone_map.items()}
        local = {}
        for b, k in self.bone_map.items():
            pk = KING_PARENT_OF_MAPPED[k]
            local[k] = world[k] if pk < 0 else world[pk].T @ world[k]
        origin = self.k_pos[1] + (world_pos[self.pelvis] - self.c_pos[self.pelvis]) * self.scale
        return local, origin, world


# -- output --------------------------------------------------------------------

def engine_quat(m):
    """The King's engine builds its bone matrix from q as m[0][1] = 2xy - 2zw
    and uses it with row vectors, so it turns by conj(q): store the conjugate
    of the column rotation we want."""
    x, y, z, w = m2q(m)
    return (-x, -y, -z, w)


def write(path, clips):
    """anims.bin, little endian:
         'SKA1', u32 clip count, u32 bone count (85)
         per clip: char name[48], u32 frames (keys: last == first for loops),
                   f32 fps, u32 flags (1 loop, 2 torso layer), f32 speed_x, f32 speed_z,
                   u8 mask[88] (1 = the clip drives that King bone), u32 data offset
         data: per frame, per King bone, f32 quat[4] (engine convention; identity
               where the mask is 0), f32 origin[3], then a prop: f32 quat[4]
               (column convention), f32 pos[3]. The prop is the gun relative to
               the right wrist for CoD4, the board in model space for Skate 3."""
    head = struct.pack('<4sII', b'SKA1', len(clips), KING_BONES)
    rec = 48 + 4 * 5 + 88 + 4
    off = len(head) + rec * len(clips)
    table, data = b'', b''
    for c in clips:
        mask = bytes(1 if k in c['bones'] else 0 for k in range(KING_BONES)).ljust(88, b'\0')
        table += struct.pack('<48sIfIff', c['name'].encode(), len(c['frames']), c['fps'], c['flags'],
                             c['speed'][0], c['speed'][1]) + mask + struct.pack('<I', off + len(data))
        for quats, origin, (gq, gp) in c['frames']:
            for k in range(KING_BONES):
                data += struct.pack('<4f', *quats.get(k, (0.0, 0.0, 0.0, 1.0)))
            data += struct.pack('<3f4f3f', *origin, *gq, *gp)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(head + table + data)
    return len(head) + len(table) + len(data)


def stick_png(path, rt, poses):
    """Front and side stick figures of the King for some (local, origin) poses, as a BMP."""
    W, H = 340 * len(poses), 560
    img = np.full((H, W, 3), 30, np.uint8)

    def line(a, b, col):
        n = int(max(abs(b[0] - a[0]), abs(b[1] - a[1]), 1))
        for t in np.linspace(0, 1, n + 1):
            x, y = int(a[0] + (b[0] - a[0]) * t), int(a[1] + (b[1] - a[1]) * t)
            if 0 <= x < W and 0 <= y < H:
                img[y, x] = col

    for fi, (local, origin) in enumerate(poses):
        wr, wp = {}, {}
        for k, pk in KING_PARENT_OF_MAPPED.items():
            if pk < 0:
                wr[k], wp[k] = local[k], origin
            else:
                wr[k] = wr[pk] @ local[k]
                rest_off = rt.k_rot[pk].T @ (rt.k_pos[k] - rt.k_pos[pk])
                wp[k] = wp[pk] + wr[pk] @ rest_off
        for x0, axis in ((fi * 340 + 85, 0), (fi * 340 + 255, 2)):
            for k, pk in KING_PARENT_OF_MAPPED.items():
                if pk < 0:
                    continue
                col = (80, 200, 255) if k in (12, 13, 14, 76, 77, 78, 11) else (255, 220, 80) \
                    if k in (33, 34, 35, 80, 81, 82, 32) else (230, 230, 230)
                a = (x0 + (-1 if axis == 0 else 1) * wp[pk][axis] * 2.2, 530 - wp[pk][1] * 2.2)
                b = (x0 + (-1 if axis == 0 else 1) * wp[k][axis] * 2.2, 530 - wp[k][1] * 2.2)
                line(a, b, col)
    row = (W * 3 + 3) & ~3
    with open(path, 'wb') as fh:
        fh.write(b'BM' + struct.pack('<IHHI', 54 + row * H, 0, 0, 54)
                 + struct.pack('<IiiHHIIiiII', 40, W, H, 1, 24, 0, row * H, 0, 0, 0, 0))
        for y in range(H - 1, -1, -1):
            fh.write(img[y, :, ::-1].tobytes() + b'\0' * (row - W * 3))

"""Decode CoD4 (IW3 PC) XAnimParts from a decompressed fastfile zone.

    python -I xanim.py <zone or .ff> [anim_name ...]

Layout from the iw3mp.exe raw-xanim builder (0x585d90) plus zone bytes.
Run with no names: decodes every inline xanim and checks that each one's
cursors consume exactly the header counts (the self-check).
"""
import math, re, struct, sys, zlib

FF = 0xFFFFFFFF
HDR = 0x58


def load(path):
    b = open(path, 'rb').read()
    return zlib.decompress(b[12:]) if b[:8] == b'IWffu100' else b


def zone_strings(z):
    n, = struct.unpack_from('<i', z, 44)
    o, out = 60 + 4 * n, []
    for p in struct.unpack_from(f'<{n}I', z, 60):
        if p != FF:
            out.append(None); continue
        e = z.index(b'\0', o); out.append(z[o:e].decode('latin1')); o = e + 1
    return out


class R:
    """Sequential cursor over one inline array."""
    def __init__(s, z, o, fmt, n):
        s.v = struct.unpack_from(f'<{n}{fmt}', z, o) if n else ()
        s.i = 0
    def take(s, k=1):
        r = s.v[s.i:s.i + k]; s.i += k
        if len(r) != k:
            raise ValueError('cursor overrun')
        return r
    def done(s):
        return s.i == len(s.v)


def parse(z, s, strs):
    h = z[s:s + HDR]
    (nbyte, nshort, nint, nrbyte, nrint, nframes) = struct.unpack_from('<6H', h, 4)
    loop, delta = h[0x10], h[0x11]
    bc = list(h[0x12:0x1c])
    nnotify = h[0x1c]
    nrshort, nidx = struct.unpack_from('<2I', h, 0x20)
    rate, freq = struct.unpack_from('<2f', h, 0x28)
    ptr = dict(zip(('names', 'byte', 'short', 'int', 'rshort', 'rbyte', 'rint', 'idx', 'notify', 'delta'),
                   struct.unpack_from('<10I', h, 0x30)))
    e = z.index(b'\0', s + HDR)
    name = z[s + HDR:e].decode('latin1')
    p = e + 1
    nb = bc[9]
    bones = []
    if ptr['names'] == FF:
        bones = [strs[i] for i in struct.unpack_from(f'<{nb}H', z, p)]; p += 2 * nb
    notify = []
    if ptr['notify'] == FF:
        for k in range(nnotify):
            sid, t = struct.unpack_from('<H2xf', z, p + 8 * k)
            notify.append((strs[sid], t))
        p += 8 * nnotify
    small = nframes < 256                    # key frame indices are u8, else u16
    ib, isz = ('B', 1) if small else ('H', 2)
    dpart = None
    if ptr['delta'] == FF:
        tptr, qptr = struct.unpack_from('<2I', z, p); p += 8
        dpart = {}
        if tptr == FF:                       # XAnimPartTrans
            size, smt = struct.unpack_from('<HB', z, p); p += 4
            if size == 0:
                dpart['trans'] = ([0], [struct.unpack_from('<3f', z, p)]); p += 12
            else:
                mins = struct.unpack_from('<3f', z, p); sc = struct.unpack_from('<3f', z, p + 12)
                fptr, = struct.unpack_from('<I', z, p + 24); p += 28
                keys = list(struct.unpack_from(f'<{size + 1}{ib}', z, p)); p += isz * (size + 1)
                fmt, w = ('B', 1) if smt else ('H', 2)
                fr = struct.unpack_from(f'<{3 * (size + 1)}{fmt}', z, p); p += 3 * w * (size + 1)
                dpart['trans'] = (keys, [tuple(mins[j] + sc[j] * fr[3 * k + j] for j in range(3))
                                         for k in range(size + 1)])
        if qptr == FF:                       # XAnimDeltaPartQuat: yaw only (z, w)
            size, = struct.unpack_from('<H', z, p); p += 4
            if size == 0:
                dpart['quat'] = ([0], [struct.unpack_from('<2h', z, p)]); p += 4
            else:
                p += 4                       # frames pointer (-1)
                keys = list(struct.unpack_from(f'<{size + 1}{ib}', z, p)); p += isz * (size + 1)
                fr = struct.unpack_from(f'<{2 * (size + 1)}h', z, p); p += 4 * (size + 1)
                dpart['quat'] = (keys, [fr[2 * k:2 * k + 2] for k in range(size + 1)])
    arrs = {}
    for key, fmt, n in (('byte', 'B', nbyte), ('short', 'H', nshort), ('int', 'f', nint),
                        ('rshort', 'h', nrshort), ('rbyte', 'B', nrbyte), ('rint', 'i', nrint),
                        ('idx', 'H' if not small else 'B', nidx)):
        if ptr[key] == FF:
            arrs[key] = R(z, p, fmt, n); p += struct.calcsize(fmt) * n
        else:
            arrs[key] = R(z, p, fmt, 0)
    B, S, I, RS, RB, IX = (arrs[k] for k in ('byte', 'short', 'int', 'rshort', 'rbyte', 'idx'))

    def keys(size):
        if small:
            return list(B.take(size + 1))
        if size < 64:
            return list(S.take(size + 1))
        S.take(((size - 1) >> 8) + 2)        # every 256th key + the last: a search table
        return list(IX.take(size + 1))

    rot = [None] * nb                        # bone -> (keys, [quat xyzw])
    b = bc[0]                                # type 0: no rotation data
    for t in (1, 2):                         # keyed: 1 = simple (z,w), 2 = full
        for _ in range(bc[t]):
            size, = S.take(); k = keys(size)
            q = RS.take((2 if t == 1 else 4) * (size + 1))
            rot[b] = (k, [simple_q(q[2 * i:2 * i + 2]) if t == 1 else full_q(q[4 * i:4 * i + 4])
                          for i in range(size + 1)]); b += 1
    for t in (3, 4):                         # one frame, stored in dataShort
        for _ in range(bc[t]):
            v = struct.unpack(f'<{2 if t == 3 else 4}h', struct.pack(f'<{2 if t == 3 else 4}H', *S.take(2 if t == 3 else 4)))
            rot[b] = ([0], [simple_q(v) if t == 3 else full_q(v)]); b += 1
    trans = [None] * nb
    for t in (5, 6, 7, 8):                   # 5 = u8x3, 6 = u16x3, 7 = one vec3, 8 = none
        for _ in range(bc[t]):
            bone, = B.take()
            if t in (5, 6):
                size, = S.take(); mins = I.take(3); sc = I.take(3); k = keys(size)
                fr = (RB if t == 5 else RS).take(3 * (size + 1))
                if t == 6:
                    fr = [v & 0xFFFF for v in fr]
                trans[bone] = (k, [tuple(mins[j] + sc[j] * fr[3 * i + j] for j in range(3))
                                   for i in range(size + 1)])
            elif t == 7:
                trans[bone] = ([0], [I.take(3)])
    left = {k: len(a.v) - a.i for k, a in arrs.items() if not a.done()}
    return dict(name=name, frames=nframes, rate=rate, loop=bool(loop), delta=bool(delta),
                counts=bc, bones=bones, rot=rot, trans=trans, deltapart=dpart, notify=notify,
                end=p, unconsumed=left)


def simple_q(zw):
    return (0.0, 0.0, zw[0] / 32767.0, zw[1] / 32767.0)


def full_q(v):
    return tuple(c / 32767.0 for c in v)


def sample(track, f):
    """Value of a (keys, values) track at frame f (lerp, then nlerp for quats)."""
    k, v = track
    if len(k) == 1 or f <= k[0]:
        return v[0]
    for i in range(1, len(k)):
        if f <= k[i]:
            a = (f - k[i - 1]) / (k[i] - k[i - 1])
            return tuple(x + (y - x) * a for x, y in zip(v[i - 1], v[i]))
    return v[-1]


def nrm(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


def find(z):
    """Inline XAnimParts: name pointer -1, rate 1..120, plausible pointers, name after."""
    for m in re.finditer(rb'(?=\xff\xff\xff\xff)', z):   # overlapping
        s = m.start()
        if s + HDR + 4 > len(z):
            break
        rate, = struct.unpack_from('<f', z, s + 0x28)
        if not 1.0 <= rate <= 120.0 or z[s + 0x10] > 1 or z[s + 0x11] > 1:
            continue
        bc = z[s + 0x12:s + 0x1c]
        if sum(bc[0:5]) != bc[9] or sum(bc[5:9]) != bc[9]:
            continue
        if any(p not in (0, FF) for p in struct.unpack_from('<10I', z, s + 0x30)):
            continue
        if re.match(rb'[a-z0-9_@\-]{2,63}\x00', z[s + HDR:s + HDR + 64], re.I):
            yield s


def main():
    z = load(sys.argv[1]); strs = zone_strings(z)
    anims = {}
    bad = 0
    for s in find(z):
        try:
            a = parse(z, s, strs)
        except (ValueError, struct.error, IndexError) as e:
            bad += 1; print('FAIL', hex(s), e); continue
        if a['unconsumed']:
            bad += 1; print('LEFTOVER', a['name'], a['unconsumed'])
        anims[a['name']] = a
    print(f'{len(anims)} xanims decoded, {bad} with problems')
    for n in sys.argv[2:]:
        a = anims[n]
        print(f"{n}: frames={a['frames']} rate={a['rate']} loop={a['loop']} delta={a['delta']} "
              f"counts={a['counts']} notify={a['notify']}")
        worst_norm, worst_jump = 0.0, 0.0
        for bi, bone in enumerate(a['bones']):
            r = a['rot'][bi]
            if r:
                for q in r[1]:
                    worst_norm = max(worst_norm, abs(math.sqrt(sum(c * c for c in q)) - 1))
                prev = None
                for f in range(a['frames'] + 1):
                    q = nrm(sample(r, f))
                    if prev:
                        d = abs(sum(x * y for x, y in zip(q, prev)))
                        worst_jump = max(worst_jump, 2 * math.degrees(math.acos(min(1.0, d))))
                    prev = q
        print(f'  max |q|-1 over keys: {worst_norm:.4f}; max per-frame rotation step: {worst_jump:.2f} deg')
        for bone in ('j_mainroot', 'j_spinelower', 'j_knee_le', 'j_elbow_ri', 'tag_weapon_right'):
            if bone in a['bones']:
                bi = a['bones'].index(bone)
                for f in (0, a['frames'] // 2):
                    q = nrm(sample(a['rot'][bi], f)) if a['rot'][bi] else None
                    t = sample(a['trans'][bi], f) if a['trans'][bi] else None
                    print(f'  {bone:18s} f{f:<4d} q={tuple(round(c, 4) for c in q) if q else None} '
                          f't={tuple(round(c, 3) for c in t) if t else None}')
        if a['deltapart']:
            for k, (keys, vals) in a['deltapart'].items():
                print(f'  delta {k}: keys {keys[:6]}.. first {vals[0]} last {vals[-1]}')


if __name__ == '__main__':
    main()

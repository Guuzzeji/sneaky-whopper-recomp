"""Extract the Xbox 360 build's textures as DDS keyed by the D3D9 HLE hash of the OG texture.

    python tools/xen_tex.py <pkg.xbp> <pkg.xen> -o game/hd [--check] [--dump DIR]

Package layout (BlitzTech .xbp little-endian, .xen big-endian), verified on Urban:
  header 16 dwords: [1] unit (0x80 / 0x1000), [3] resource count, [4] directory
  offset, [10] name table offset (offsets in units).
  directory: count x 8 dwords {chunk offset (units), name hash, chunk bytes,
  name table offset, ...}.
  OG texture chunk:  +0x20 {u32 0, u32 ptr, u32 texels, u16 fmt, w, h, 0x10, 0,
                     u8 levels, 1}, pixels at +0x100, linear DXT, mips packed.
  360 texture chunk: +0x20 {u16 w, h, fmt, 0x10, u32 0, u8 levels, 1}, pixels at
                     +0x1000, 8-in-16 endian, linear (not tiled) with the pitch
                     padded to 32 blocks; base level then mips,
                     each level padded to 32x32 blocks, levels <= 16 texels packed
                     into one tile.
  Package fmt 0x18 = DXT1 (D3D8 0x0C), 0x1A = DXT3 (D3D8 0x0E); other formats
  (0x2F lightmaps, ...) are skipped.
"""
import argparse
import os
import struct
import sys

import numpy as np

FMTS = {0x18: (8, b"DXT1", 0x0C), 0x1A: (16, b"DXT3", 0x0E)}   # pkg fmt -> block bytes, FourCC, D3D8 fmt


def hash_bytes(p, n):
    """src/d3d9_hle.c hash_bytes: FNV-1a over a sample of the dwords."""
    u = lambda i: int.from_bytes(p[i:i + 4], "little")
    h, i = 2166136261, 0
    while i + 4 <= n and i < 256:
        h = ((h ^ u(i)) * 16777619) & 0xFFFFFFFF
        i += 4
    while i + 4 <= n:
        h = ((h ^ u(i)) * 16777619) & 0xFFFFFFFF
        i += 61 * 4
    i = n - 256 if n > 256 else 0
    while i + 4 <= n:
        h = ((h ^ u(i)) * 16777619) & 0xFFFFFFFF
        i += 4
    return h ^ n


def load_pkg(path):
    """name -> (fmt, w, h, levels, pixel bytes) for every DXT texture in the package."""
    b = open(path, "rb").read()
    xen = path.lower().endswith(".xen")
    e = ">" if xen else "<"
    hd = struct.unpack(e + "16I", b[:64])
    unit, n, dirofs, names = hd[1], hd[3], hd[4] * hd[1], hd[10] * hd[1]
    out, other = {}, {}
    for i in range(n):
        r = struct.unpack_from(e + "8I", b, dirofs + 32 * i)
        nm = b[names + r[3]:b.index(b"\0", names + r[3])].decode("latin1")
        c, csize = r[0] * unit, r[2]
        if xen:
            w, h, fmt, _, _, lv, one = struct.unpack_from(">HHHHIBB", b, c + 0x20)
            data = c + 0x1000
        else:
            _, _, _, fmt, w, h, _, _, lv, one = struct.unpack_from("<IIIHHHHHBB", b, c + 0x20)
            data = c + 0x100
        if one != 1 or not w or not h or w & (w - 1) or h & (h - 1) or not 1 <= lv <= 13:
            continue            # not a texture chunk
        if fmt not in FMTS:
            other[nm] = fmt
            continue
        out[nm] = (fmt, w, h, lv, b[data:c + csize])
    return out, other


def dims(w, h, lv):
    return [(max(1, w >> i), max(1, h >> i)) for i in range(lv)]


def og_levels(fmt, w, h, lv, raw):
    bb, o, res = FMTS[fmt][0], 0, []
    for lw, lh in dims(w, h, lv):
        n = ((lw + 3) // 4) * ((lh + 3) // 4) * bb
        res.append(raw[o:o + n])
        o += n
    return res


def packed_offset(w, h, mip):
    """Xenia GetPackedMipOffset for 2D: (x, y) texel offset in the packed tile, or None."""
    lw, lh = (w - 1).bit_length(), (h - 1).bit_length()
    ls = min(lw, lh)
    if ls > 4 + mip:
        return None
    base = ls - 4 if ls > 4 else 0
    pm = mip - base
    if pm < 3:
        return (0, 16 >> pm) if lw > lh else (16 >> pm, 0)
    if lw > lh:
        return ((1 << (lw - base)) >> (pm - 2), 0)
    return (0, (1 << (lh - base)) >> (pm - 2))


def linear_blocks(raw, bw, bh, pitch, bb, ox=0, oy=0):
    """Gather a bw x bh block level at block (ox, oy) of a linear surface `pitch` blocks wide."""
    y, x = np.mgrid[0:bh, 0:bw]
    off = (((y + oy) * pitch + x + ox) * bb).ravel()
    return np.frombuffer(raw, np.uint8)[off[:, None] + np.arange(bb)].tobytes()


def xen_levels(fmt, w, h, lv, raw):
    """Linear little-endian DXT bytes per level from a 360 texture."""
    bb = FMTS[fmt][0]
    raw = np.frombuffer(raw[:len(raw) & ~1], np.uint16).byteswap().tobytes()   # 8-in-16
    al = lambda v: (v + 31) & ~31
    res, mipofs, tail = [], None, None
    for i, (lw, lh) in enumerate(dims(w, h, lv)):
        bw, bh = (lw + 3) // 4, (lh + 3) // 4
        pk = packed_offset(w, h, i)
        if i == 0:
            base = 0
            size = al(bw) * al(bh) * bb
            mipofs = (size + 0xFFF) & ~0xFFF
        elif pk is not None and tail is not None:
            base = tail
        else:
            base = mipofs
            mipofs += al(bw) * al(bh) * bb
            if pk is not None:
                tail = base
        ox, oy = (pk[0] // 4, pk[1] // 4) if pk else (0, 0)
        pitch = al(bw) if pk is None else 32
        res.append(linear_blocks(raw[base:], bw, bh, pitch, bb, ox, oy))
    return res


def write_dds(path, fmt, w, h, levels):
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000 | 0x20000      # caps|height|width|pixfmt|linearsize|mipcount
    caps = 0x1000 | (0x400008 if len(levels) > 1 else 0)      # texture [|complex|mipmap]
    hdr = struct.pack("<4s7I44x2I4s5I5I", b"DDS ", 124, flags, h, w, len(levels[0]), 0, len(levels),
                      32, 0x4, FMTS[fmt][1], 0, 0, 0, 0, 0, caps, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + b"".join(levels))


def decode(fmt, w, h, data):
    """DXT colour -> float RGB (h, w, 3); alpha ignored. For checks only."""
    bb = FMTS[fmt][0]
    bw, bh = max(1, w // 4), max(1, h // 4)
    blk = np.frombuffer(data[:bw * bh * bb], np.uint8).reshape(bh * bw, bb)[:, bb - 8:]
    c = blk[:, 0:4].view("<u2").astype(np.int32)
    rgb = np.stack([(c >> 11 & 31) * 255 / 31, (c >> 5 & 63) * 255 / 63, (c & 31) * 255 / 31], -1)
    c0, c1 = rgb[:, 0], rgb[:, 1]
    four = (c[:, 0] > c[:, 1]) | (bb == 16)
    pal = np.stack([c0, c1, np.where(four[:, None], (2 * c0 + c1) / 3, (c0 + c1) / 2),
                    np.where(four[:, None], (c0 + 2 * c1) / 3, 0)], 1)
    idx = blk[:, 4:8].copy().view("<u4")[:, 0]
    sel = (idx[:, None] >> (2 * np.arange(16))) & 3
    px = np.take_along_axis(pal, sel[:, :, None], 1)                  # (blocks, 16, 3)
    return px.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3)[:h, :w]


def corr(a, b):
    """Correlation of OG image a with b box-filtered down to a's size."""
    fy, fx = b.shape[0] // a.shape[0], b.shape[1] // a.shape[1]
    if fy < 1 or fx < 1 or b.shape[0] % a.shape[0] or b.shape[1] % a.shape[1]:
        return float("nan")
    b = b.reshape(a.shape[0], fy, a.shape[1], fx, 3).mean((1, 3))
    a, b = a.ravel() - a.mean(), b.ravel() - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d else (1.0 if not a.any() and not b.any() else 0.0)


def write_bmp(path, img):
    h, w, _ = img.shape
    row = (w * 3 + 3) & ~3
    px = np.zeros((h, row), np.uint8)
    px[:, :w * 3] = np.clip(img[::-1, :, ::-1], 0, 255).astype(np.uint8).reshape(h, w * 3)
    with open(path, "wb") as f:
        f.write(struct.pack("<2sIHHIIiiHHIIiiII", b"BM", 54 + px.size, 0, 0, 54, 40, w, h, 1, 24, 0,
                            px.size, 0, 0, 0, 0) + px.tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("xbp")
    ap.add_argument("xen")
    ap.add_argument("-o", "--out", default="game/hd")
    ap.add_argument("--check", action="store_true", help="print pairing stats, write nothing")
    ap.add_argument("--dump", help="write OG/360 decoded BMPs here")
    ap.add_argument("--min-corr", type=float, default=0.5, help="skip pairs that correlate less")
    a = ap.parse_args()

    og, og_other = load_pkg(a.xbp)
    xe, _ = load_pkg(a.xen)
    skip = {}
    for nm, f in og_other.items():
        skip[nm] = "OG fmt 0x%02X" % f
    if not a.check:
        os.makedirs(a.out, exist_ok=True)
    index, cors, clash = {}, [], []
    for nm in sorted(og):
        fo, wo, ho, lo, rawo = og[nm]
        if nm.startswith("lm_"):
            skip[nm] = "lightmap"; continue
        if nm not in xe:
            skip[nm] = "no 360 texture"; continue
        fx, wx, hx, lx, rawx = xe[nm]
        if wx * ho != hx * wo:
            skip[nm] = "aspect %dx%d vs %dx%d" % (wo, ho, wx, hx); continue
        if wx <= wo:
            skip[nm] = "360 not larger (%dx%d)" % (wx, hx); continue
        top = og_levels(fo, wo, ho, lo, rawo)[0]
        try:
            lvx = xen_levels(fx, wx, hx, lx, rawx)
        except IndexError:
            skip[nm] = "360 data short"; continue
        r = corr(decode(fo, wo, ho, top), decode(fx, wx, hx, lvx[0]))
        cors.append((r, nm))
        if a.dump:
            os.makedirs(a.dump, exist_ok=True)
            write_bmp(os.path.join(a.dump, nm + "_og.bmp"), decode(fo, wo, ho, top))
            write_bmp(os.path.join(a.dump, nm + "_360.bmp"), decode(fx, wx, hx, lvx[0]))
        if not r >= a.min_corr:
            skip[nm] = "low correlation"; continue
        hsh = hash_bytes(top, len(top))
        key = "%08x" % hsh
        if key in index:
            clash.append((key, index[key].split()[1], nm))   # identical OG pixels: first name wins
            continue
        if not a.check:
            write_dds(os.path.join(a.out, key + ".dds"), fx, wx, hx, lvx)
        index[key] = "%s %-40s OG %s %dx%d -> 360 %s %dx%d %d mips corr %.3f" % (
            key, nm, FMTS[fo][1].decode(), wo, ho, FMTS[fx][1].decode(), wx, hx, lx, r)
    if not a.check:
        # Merge with what earlier packages wrote, keyed by hash.
        path = os.path.join(a.out, "index.txt")
        old = dict(l.split(" ", 1) for l in open(path).read().splitlines() if l) if os.path.exists(path) else {}
        old.update((k, v.split(" ", 1)[1]) for k, v in index.items())
        with open(path, "w") as f:
            f.writelines("%s %s\n" % kv for kv in sorted(old.items()))
    print("OG DXT textures %d, 360 DXT textures %d, converted %d, skipped %d, same-hash duplicates %d" % (
        len(og), len(xe), len(index), len(skip), len(clash)))
    for c in clash:
        print("  duplicate %s: %s == %s" % c)
    reasons = {}
    for nm, why in skip.items():
        reasons.setdefault(why.split(" (")[0], []).append(nm)
    for why, nms in sorted(reasons.items(), key=lambda kv: -len(kv[1])):
        print("  skip %-28s %4d  e.g. %s" % (why, len(nms), ", ".join(nms[:3])))
    if cors:
        v = np.array([c for c, _ in cors])
        print("correlation OG vs 360 (box-filtered): median %.3f, <0.5: %d, <0.8: %d of %d" % (
            np.nanmedian(v), (v < 0.5).sum(), (v < 0.8).sum(), len(v)))
        for c, nm in sorted(cors)[:8]:
            print("  low %.3f %s" % (c, nm))


if __name__ == "__main__":
    sys.exit(main())

"""Dump a class from Skate 3's AttribSys vaults (db.big: skaterschema.* and
skatercollections.*, extracted with tools/skate3_big.py).

    py tools\\skate3_attrib.py game\\mods\\skate3\\raw physics_jump

Prints the class's fields and, for every collection of it, the values in its
layout. Names are 64-bit Jenkins lookup8 hashes (level 0xABCDEF0011223344);
only names found as strings in the vault .bins (or passed with -n) resolve.
Format notes: docs/SKATE3_NOTES.md.
"""
import argparse
import os
import re
import struct

M = (1 << 64) - 1


def _mix(a, b, c):
    for sa, sb, sc in ((43, 9, 8), (38, 23, 5), (35, 49, 11), (12, 18, 22)):
        a = (a - b - c) & M; a ^= c >> sa
        b = (b - c - a) & M; b ^= (a << sb) & M
        c = (c - a - b) & M; c ^= b >> sc
    return a, b, c


def h8(s, level=0xABCDEF0011223344):
    """Bob Jenkins' lookup8 hash() as Attrib uses it."""
    k = s.encode() if isinstance(s, str) else s
    a = b = level
    c = 0x9E3779B97F4A7C13
    p = 0
    while len(k) - p >= 24:
        a = (a + int.from_bytes(k[p:p + 8], "little")) & M
        b = (b + int.from_bytes(k[p + 8:p + 16], "little")) & M
        c = (c + int.from_bytes(k[p + 16:p + 24], "little")) & M
        a, b, c = _mix(a, b, c)
        p += 24
    c = (c + len(k)) & M
    t = k[p:]
    for i in range(len(t) - 1, -1, -1):
        if i >= 16:
            c = (c + (t[i] << (8 * (i - 15)))) & M
        elif i >= 8:
            b = (b + (t[i] << (8 * (i - 8)))) & M
        else:
            a = (a + (t[i] << (8 * i))) & M
    return _mix(a, b, c)[2]


def chunks(v):
    out, p = {}, 0
    while p < len(v):
        tag, size = v[p:p + 4], struct.unpack_from(">I", v, p + 4)[0]
        out[tag] = (p, size)
        if tag == b"EndC" or not size:
            break
        p += size
    return out


def pointers(v, c):
    """PtrN: {u32 fixup (DatN payload offset), u16 kind, u16 dep, u64 target}; kind 3 = pointer."""
    p, size = c[b"PtrN"]
    out = {}
    for o in range(p + 8, p + size, 16):
        fx, kind, _dep, target = struct.unpack_from(">IHHQ", v, o)
        if kind == 3:
            out[fx] = target
    return out


def dump(raw_dir, cls_name, extra_names=()):
    rd = lambda f: open(os.path.join(raw_dir, f), "rb").read()
    sv, sb, cv, cb = rd("skaterschema.vlt"), rd("skaterschema.bin"), rd("skatercollections.vlt"), rd("skatercollections.bin")
    names = {h8(s): s for blob in (sb, cb) for s in (m.decode() for m in re.findall(rb"[ -~]{2,64}", blob))}
    names.update({h8(s): s for s in (cls_name, "default", *extra_names)})
    name = lambda k: names.get(k, f"{k:#018x}")

    sc = chunks(sv)
    sdat = sc[b"DatN"][0] + 8
    ep = sc[b"ExpN"][0]
    key = h8(cls_name)
    for i in range(struct.unpack_from(">Q", sv, ep + 8)[0]):
        k, _t, _size, off = struct.unpack_from(">QQII", sv, ep + 16 + 24 * i)
        if k == key:
            break
    else:
        raise SystemExit(f"class {cls_name} not in schema")
    nfields = struct.unpack_from(">I", sv, sdat + off + 12)[0]
    fa = pointers(sv, sc)[off + 0x10]
    # field: {u64 key, u64 type, u16 offset, u16 size, u16 count, u8 flags, u8 align}; flags 0x2 = in layout
    fields = [struct.unpack_from(">QQHHHBB", sb, fa + 24 * i) for i in range(nfields)]
    print(f"class {cls_name}: {nfields} fields")
    for f in fields:
        print(f"  {name(f[0]):40} {name(f[1]):32} off {f[2]:#06x} size {f[3]} flags {f[5]:#x}")

    cc = chunks(cv)
    cdat = cc[b"DatN"][0] + 8
    for fx, layout in sorted(pointers(cv, cc).items()):
        if struct.unpack_from(">Q", cv, cdat + fx - 8)[0] != key:
            continue
        print(f"collection {name(struct.unpack_from('>Q', cv, cdat + fx - 16)[0])} (layout {layout:#x})")
        for f in fields:
            if not f[5] & 2:
                continue
            raw = cb[layout + f[2]:layout + f[2] + f[3]]
            tname = name(f[1])
            if "Float" in tname and f[3] == 4:
                val = f"{struct.unpack('>f', raw)[0]:g}"
            elif "Graph" in tname or "Vector" in tname:
                val = " ".join(f"{x:g}" for x in struct.unpack(f">{f[3] // 4}f", raw))
            else:
                val = raw.hex()
            print(f"  {name(f[0]):40} {val}")


if __name__ == "__main__":
    assert h8("physics_jump") == 0xC2E0A8B6C13990FD and h8("default") == 0xD7EDBD362D7D2152
    ap = argparse.ArgumentParser(description="Skate 3 AttribSys class dump")
    ap.add_argument("raw_dir")
    ap.add_argument("cls")
    ap.add_argument("-n", "--names", nargs="*", default=[], help="extra candidate names to hash")
    a = ap.parse_args()
    dump(a.raw_dir, a.cls, a.names)

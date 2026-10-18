"""List and extract Skate 3's EA "EB" v3 BIG archives (data/**/*.big).

    py tools\\skate3_big.py ls skate3\\data\\big\\db.big [regex]
    py tools\\skate3_big.py x  skate3\\data\\big\\db.big <regex> -o game\\mods\\skate3\\raw

Layout (big-endian), worked out from the files:
  0x00 'EB' u16 3, u32 count, u16 flags, u8 offset shift, u8 0,
  0x0C u32 name table offset, u32 name table size, u8 name record len,
  u8 dir record len, u16 dir count, u32 0, u32 archive size
  0x30 count x {u32 offset >> shift, u32 packed size (0 = stored), u32 size, u32 hash}
  name table: count x {u16 dir index, name}, then 12 bytes, then dir records.
Packed entries are RefPack (0x10FB), or 'chunkref' (chunks of RefPack).
"""
import argparse
import os
import re
import struct


def refpack(src):
    flags = src[0]
    assert src[1] == 0xFB, "not RefPack"
    n = 4 if flags & 0x80 else 3
    p = 2 + (n if flags & 0x01 else 0)               # skip packed size if present
    size = int.from_bytes(src[p:p + n], "big")
    p += n
    out = bytearray()
    while True:
        b0 = src[p]
        if b0 < 0x80:
            b1 = src[p + 1]; p += 2
            plain, cnt, off = b0 & 3, ((b0 >> 2) & 7) + 3, ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = src[p + 1], src[p + 2]; p += 3
            plain, cnt, off = b1 >> 6, (b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = src[p + 1], src[p + 2], src[p + 3]; p += 4
            plain, cnt = b0 & 3, ((b0 & 0x0C) << 6) + b3 + 5
            off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            p += 1
            plain, cnt = ((b0 & 0x1F) << 2) + 4, 0
        else:
            p += 1
            out += src[p:p + (b0 & 3)]
            break
        out += src[p:p + plain]
        p += plain
        for _ in range(cnt):                         # overlapping copy, byte by byte
            out.append(out[-off])
    assert len(out) == size, f"RefPack size {len(out)} != {size}"
    return bytes(out)


def entries(path):
    """Yield (name, offset, packed_size, size) for each file in the archive."""
    with open(path, "rb") as f:
        hdr = f.read(0x30)
        if hdr[:4] != b"EB\0\3":
            raise ValueError(f"{path}: not an EB v3 BIG")
        count = struct.unpack_from(">I", hdr, 4)[0]
        shift = hdr[10]
        noff = struct.unpack_from(">I", hdr, 12)[0]
        nlen, dlen = hdr[0x14], hdr[0x15]
        ndirs = struct.unpack_from(">H", hdr, 0x16)[0]
        table = f.read(16 * count)
        f.seek(noff)
        names = f.read(count * nlen + 12 + ndirs * dlen)
    dbase = count * nlen + 12
    dirs = [names[dbase + i * dlen:dbase + (i + 1) * dlen].split(b"\0")[0].decode("latin-1").strip()
            for i in range(ndirs)]
    for i in range(count):
        off, packed, size, _hash = struct.unpack_from(">IIII", table, 16 * i)
        rec = names[i * nlen:(i + 1) * nlen]
        d = dirs[struct.unpack_from(">H", rec)[0]]
        name = rec[2:].split(b"\0")[0].decode("latin-1").strip()
        yield f"{d}/{name}", off << shift, packed, size


def chunkref(src):
    """'chunkref' containers (marquee.big models): u32 version 2, u32 size,
    u32 chunk size, u32 chunk count, u32 alignment, then from 0x28 each chunk
    as {u32 packed size, u32 kind (2 = RefPack)} and its data, 16-aligned."""
    size, count = struct.unpack_from(">I", src, 12)[0], struct.unpack_from(">I", src, 20)[0]
    out, p = bytearray(), 0x28
    for _ in range(count):
        n, kind = struct.unpack_from(">II", src, p)
        chunk = src[p + 8:p + 8 + n]
        out += refpack(chunk) if kind == 2 else chunk
        p = (p + 8 + n + 15) & ~15
    assert len(out) == size, f"chunkref size {len(out)} != {size}"
    return bytes(out)


def read(path, off, packed, size):
    with open(path, "rb") as f:
        f.seek(off)
        data = f.read(packed or size)
    if data[:8] == b"chunkref":
        return chunkref(data)
    return refpack(data) if packed else data


def main():
    ap = argparse.ArgumentParser(description="Skate 3 BIG archives")
    ap.add_argument("cmd", choices=["ls", "x"])
    ap.add_argument("big")
    ap.add_argument("pattern", nargs="?", default="")
    ap.add_argument("-o", "--out", default=".")
    a = ap.parse_args()
    rx = re.compile(a.pattern, re.I)
    for name, off, packed, size in entries(a.big):
        if not rx.search(name):
            continue
        if a.cmd == "ls":
            print(f"{size:10} {'P' if packed else ' '} {name}")
        else:
            dst = os.path.join(a.out, name.lstrip("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(read(a.big, off, packed, size))
            print(dst)


if __name__ == "__main__":
    main()

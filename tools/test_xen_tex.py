"""Self-checks for xen_tex.py: python tools/test_xen_tex.py (or pytest)."""
import os
import struct
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xen_tex import hash_bytes, packed_offset, write_dds, xen_levels  # noqa: E402

P, M = 16777619, 0xFFFFFFFF


def test_hash():
    # n = 4: the head loop and the tail loop each see the one dword.
    d = 0x12345678
    h = ((((2166136261 ^ d) * P) & M ^ d) * P) & M
    assert hash_bytes(struct.pack("<I", d), 4) == h ^ 4
    # n = 1024: head covers 0..255, the stride samples 256, 500, 744, ..., tail 768..1023.
    buf = bytearray(1024)
    base = hash_bytes(bytes(buf), 1024)
    buf[300] = 1
    assert hash_bytes(bytes(buf), 1024) == base        # between samples
    buf[500] = 1
    assert hash_bytes(bytes(buf), 1024) != base        # on a sample


def test_packed_offset():
    assert packed_offset(512, 512, 4) is None           # 32 texels: own tile
    assert packed_offset(512, 512, 5) == (16, 0)        # 16 texels: first in the tail
    assert packed_offset(32, 32, 1) == (16, 0)
    assert packed_offset(32, 32, 2) == (8, 0)
    assert packed_offset(64, 32, 1) == (0, 16)          # wider than tall: stacked vertically


def test_xen_levels():
    # 32x32 DXT1, 3 levels: base 8x8 blocks in a 32x32-block pitch-padded surface
    # (0x2000), mips 1 and 2 packed into one 32x32-block tile at 0x2000.
    bb, pitch = 8, 32
    surf = np.zeros(0x4000, np.uint8)
    blocks = lambda n, seed: np.random.default_rng(seed).integers(0, 256, (n, bb), np.uint8)
    lv0, lv1, lv2 = blocks(64, 0), blocks(16, 1), blocks(4, 2)
    for i, blk in enumerate(lv0):
        o = ((i // 8) * pitch + i % 8) * bb
        surf[o:o + bb] = blk
    for x0, bw, lv in ((4, 4, lv1), (2, 2, lv2)):      # packed at texel (16, 0) and (8, 0)
        for i, blk in enumerate(lv):
            o = 0x2000 + ((i // bw) * pitch + x0 + i % bw) * bb
            surf[o:o + bb] = blk
    xen = surf.view(np.uint16).byteswap().tobytes()    # 8-in-16 on disk
    got = xen_levels(0x18, 32, 32, 3, xen)
    assert got == [lv0.tobytes(), lv1.tobytes(), lv2.tobytes()]


def test_dds_header():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "t.dds")
        write_dds(p, 0x1A, 8, 4, [bytes(32), bytes(16)])
        b = open(p, "rb").read()
    assert len(b) == 128 + 48 and b[:4] == b"DDS " and b[84:88] == b"DXT3"
    assert struct.unpack_from("<6I", b, 4) == (124, 0xA1007, 4, 8, 32, 0)
    assert struct.unpack_from("<I", b, 28)[0] == 2


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("ok")

import struct
from cod4_extract import iwi_to_dds, ref_block, unpack_unit


def test_iwi_to_dds_reverses_mips():
    # 8x8 DXT1: mips 8x8 (32 B), 4x4 (8), 2x2 (8), 1x1 (8), stored smallest first
    mips = [bytes([n]) * s for n, s in ((3, 8), (2, 8), (1, 8), (0, 32))]
    iwi = b'IWi\x06' + bytes([0x0B, 0]) + struct.pack('<HHH', 8, 8, 1) + bytes(16) + b''.join(mips)
    dds = iwi_to_dds(iwi)
    assert dds[:4] == b'DDS ' and len(dds) == 128 + 56
    assert struct.unpack_from('<II', dds, 12) == (8, 8) and dds[84:88] == b'DXT1'
    assert struct.unpack_from('<I', dds, 28)[0] == 4
    assert dds[128:160] == bytes(32) and dds[160:168] == b'\x01' * 8   # largest level first


def test_refs_and_normals():
    assert ref_block(((8 << 28) | 0x19c10) + 1) == (8, 0x19c10)
    x, y, z = unpack_unit(int.from_bytes(bytes([127, 127, 254, 63]), 'little'))
    assert abs(x) < 1e-6 and abs(y) < 1e-6 and 0.9 < z < 1.1

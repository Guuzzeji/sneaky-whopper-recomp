from skate3_big import refpack


def test_refpack_literal_and_copies():
    # 4 literals "abcd", copy 4 from 4 back, then 3 literals "xyz" + an overlapping copy of 3 from 1 back
    src = bytes([0x10, 0xFB, 0x00, 0x00, 0x0E,
                 0xE0]) + b"abcd" + bytes([0x04, 0x03,
                 0x03 | (0 << 2), 0x00]) + b"xyz" + bytes([0xFC])
    assert refpack(src) == b"abcdabcdxyzzzz"


def test_chunkref_stored_chunks():
    import struct
    from skate3_big import chunkref
    a, b = b"hello world!", b"abc"
    blob = b"chunkref" + struct.pack(">IIIII", 2, len(a) + len(b), 0x20000, 2, 16) + bytes(0x28 - 28)
    blob += struct.pack(">II", len(a), 0) + a
    blob += bytes((-len(blob)) % 16) + struct.pack(">II", len(b), 0) + b
    assert chunkref(blob) == a + b

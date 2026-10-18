"""Turn a retail Xbox 360 XEX2 into its plain PE image so REA (Ghidra's PE
loader, PowerPC big-endian) can open it.

    .venv\\Scripts\\python.exe tools\\xex_to_pe.py skate3\\default.xex -o analysis\\rea\\skate3_default.exe

Handles what retail discs use: AES-normal encryption and LZX-normal
compression (plus the trivial none/basic cases). LZX goes through
build\\lzx_cli.exe (libmspack, see tools/lzx_cli.c). Title-update patches
(.xexp) are not applied.
"""
import argparse
import ctypes
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
LZX_CLI = os.path.join(os.path.dirname(__file__), "..", "build", "lzx_cli.exe")


def aes_decrypt(key, data, cbc):
    """AES-128 decrypt (ECB, or CBC with a zero IV) through Windows bcrypt."""
    bc = ctypes.WinDLL("bcrypt")
    alg, hkey = ctypes.c_void_p(), ctypes.c_void_p()
    assert bc.BCryptOpenAlgorithmProvider(ctypes.byref(alg), "AES", None, 0) == 0
    mode = ctypes.create_unicode_buffer("ChainingModeCBC" if cbc else "ChainingModeECB")
    assert bc.BCryptSetProperty(alg, "ChainingMode", mode, ctypes.sizeof(mode), 0) == 0
    assert bc.BCryptGenerateSymmetricKey(alg, ctypes.byref(hkey), None, 0, key, len(key), 0) == 0
    n = len(data) & ~15
    out = ctypes.create_string_buffer(n)
    got = ctypes.c_ulong()
    iv = ctypes.create_string_buffer(16) if cbc else None
    assert bc.BCryptDecrypt(hkey, data[:n], n, None, iv, 16 if cbc else 0,
                            out, n, ctypes.byref(got), 0) == 0
    bc.BCryptDestroyKey(hkey)
    bc.BCryptCloseAlgorithmProvider(alg, 0)
    return out.raw + data[n:]


def xex_to_pe(xex):
    magic, _flags, pe_off, _res, sec_off, n = struct.unpack_from(">4sIIIII", xex)
    if magic != b"XEX2":
        raise ValueError("not a XEX2")
    hdr = dict(struct.unpack_from(">II", xex, 24 + 8 * i) for i in range(n))
    image_size = struct.unpack_from(">I", xex, sec_off + 4)[0]
    file_key = xex[sec_off + 0x150:sec_off + 0x160]

    ff = hdr[0x3FF]
    info_size, enc, comp = struct.unpack_from(">IHH", xex, ff)
    data = xex[pe_off:]
    if enc == 1:
        session = aes_decrypt(RETAIL_KEY, file_key, cbc=False)
        data = aes_decrypt(session, data, cbc=True)

    if comp == 0:                                    # none
        return data[:image_size]
    if comp == 1:                                    # basic: (data, zero) runs
        out, p = bytearray(), 0
        for i in range((info_size - 8) // 8):
            dsz, zsz = struct.unpack_from(">II", xex, ff + 8 + 8 * i)
            out += data[p:p + dsz] + bytes(zsz)
            p += dsz
        return bytes(out)
    if comp != 2:
        raise ValueError(f"compression {comp} not supported")

    # normal: hash-chained blocks of (u16 len, bytes) chunks, then LZX
    window, block_size = struct.unpack_from(">II", xex, ff + 8)
    block_hash = xex[ff + 16:ff + 36]
    stream, p = bytearray(), 0
    while block_size:
        block = data[p:p + block_size]
        if hashlib.sha1(block).digest() != block_hash:
            raise ValueError("block hash mismatch (wrong key?)")
        next_size, next_hash = struct.unpack_from(">I", block)[0], block[4:24]
        q = 24
        while True:
            (csz,) = struct.unpack_from(">H", block, q)
            q += 2
            if not csz:
                break
            stream += block[q:q + csz]
            q += csz
        p += block_size
        block_size, block_hash = next_size, next_hash

    with tempfile.TemporaryDirectory(dir=os.path.dirname(LZX_CLI)) as tmp:
        src, dst = os.path.join(tmp, "in.lzx"), os.path.join(tmp, "out.bin")
        open(src, "wb").write(stream)
        subprocess.run([LZX_CLI, src, dst, str(image_size), str(window)], check=True)
        return open(dst, "rb").read()


def flatten_sections(pe):
    """The XEX loads the image as mapped memory but keeps the linker's file
    offsets in the section headers. Point each section's raw data at its VA."""
    pe = bytearray(pe)
    e = struct.unpack_from("<I", pe, 0x3C)[0]
    nsec, opt_size = struct.unpack_from("<H", pe, e + 6)[0], struct.unpack_from("<H", pe, e + 20)[0]
    for i in range(nsec):
        s = e + 24 + opt_size + 40 * i
        vsize, va = struct.unpack_from("<II", pe, s + 8)
        struct.pack_into("<II", pe, s + 16, min(vsize, len(pe) - va), va)
    return bytes(pe)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("xex")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    pe = xex_to_pe(open(a.xex, "rb").read())
    if pe[:2] != b"MZ":
        sys.exit("output is not a PE (bad decrypt?)")
    pe = flatten_sections(pe)
    open(a.out, "wb").write(pe)
    print(f"wrote {a.out}: {len(pe):#x} bytes")


if __name__ == "__main__":
    main()

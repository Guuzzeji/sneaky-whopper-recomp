"""Wrap the XBE's flat image in a minimal PE32 so PE-only tools (REA's Windows
Ghidra provider) load it at the XBE's own addresses.

One RWX section spans VA 0x11000..end; image base is the XBE base 0x10000, so
every VA matches functions.json. Output is game-derived: keep it in analysis/.

    .venv\\Scripts\\python.exe tools\\xbe_to_pe.py [out.exe]
"""
import json, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
info = json.load(open(ROOT / "game/default_analysis.json"))
xbe = (ROOT / "game/default.xbe").read_bytes()
out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "analysis/rea/default_flat.exe"

BASE, ENTRY, ALIGN = int(info["base_address"], 16), int(info["entry_point"], 16), 0x1000
end = max(int(s["virtual_addr"], 16) + s["virtual_size"] for s in info["sections"])
img = bytearray((end - BASE + ALIGN - 1) // ALIGN * ALIGN)
for s in info["sections"]:
    va, ra, rs = int(s["virtual_addr"], 16), int(s["raw_addr"], 16), s["raw_size"]
    img[va - BASE:va - BASE + rs] = xbe[ra:ra + rs]

body = bytes(img[ALIGN:])                     # RVA 0x1000 .. end
coff = struct.pack("<HHIIIHH", 0x14C, 1, 0, 0, 0, 0xE0, 0x0102)  # i386, 1 section, EXE|32BIT
opt = struct.pack("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
                  0x10B, 0, 0, len(body), 0, 0, ENTRY - BASE, ALIGN, ALIGN, BASE,
                  ALIGN, ALIGN, 4, 0, 0, 0, 4, 0, 0, len(img), ALIGN, 0, 2, 0,
                  0x100000, 0x1000, 0x100000, 0x1000, 0, 16) + b"\0" * 128
sect = struct.pack("<8sIIIIIIHHI", b".xbe", len(body), ALIGN, len(body), ALIGN,
                   0, 0, 0, 0, 0xE0000060)     # code|data|RWX
hdr = b"MZ" + b"\0" * 58 + struct.pack("<I", 0x40) + b"PE\0\0" + coff + opt + sect
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(hdr.ljust(ALIGN, b"\0") + body)
print(f"{out}: base 0x{BASE:X}, entry 0x{ENTRY:X}, {len(img):#x} bytes")

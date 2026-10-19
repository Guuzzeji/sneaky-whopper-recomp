"""Headless Ghidra on the Skate 3 XEX, for the research REA can't do (REA
rejects PowerPC PEs). Input is tools/xex_to_pe.py's flat image.

    py tools\\skate3_ghidra.py build                 # import + .pdata seeds + analysis (long)
    py tools\\skate3_ghidra.py decomp 0x82123456 ...  # pseudo-C of the containing functions
    py tools\\skate3_ghidra.py xrefs 0x82085290 ...   # who references an address
    py tools\\skate3_ghidra.py str kickflip ...        # where a string is used

The project lives in analysis/ghidra_skate3/ (gitignored).
"""
import os
import struct
import sys

import pyghidra

ROOT = os.path.join(os.path.dirname(__file__), "..")
IMAGE = os.path.abspath(os.path.join(ROOT, "analysis", "rea", "skate3_default.exe"))
PROJ_DIR = os.path.abspath(os.path.join(ROOT, "analysis", "ghidra_skate3"))
GHIDRA = os.environ.get("GHIDRA_INSTALL_DIR", "ghidra")
BASE = 0x82000000
LANG = "PowerPC:BE:64:A2ALT-32addr"


def pdata_starts(img):
    """Function starts from .pdata: (BeginAddress, packed lengths) pairs."""
    e = struct.unpack_from("<I", img, 0x3C)[0]
    nsec, opt = struct.unpack_from("<H", img, e + 6)[0], struct.unpack_from("<H", img, e + 20)[0]
    for i in range(nsec):
        s = e + 24 + opt + 40 * i
        if img[s:s + 8].rstrip(b"\0") == b".pdata":
            vsize, va = struct.unpack_from("<II", img, s + 8)
            for o in range(va, va + vsize, 8):
                begin, packed = struct.unpack_from(">II", img, o)
                if begin:
                    yield begin, ((packed >> 8) & 0x3FFFFF) * 4
    return


def build():
    img = open(IMAGE, "rb").read()
    with pyghidra.open_program(IMAGE, project_location=PROJ_DIR, project_name="skate3",
                               analyze=False, language=LANG,
                               loader="ghidra.app.util.opinion.BinaryLoader") as api:
        prog = api.getCurrentProgram()
        prog.setImageBase(api.toAddr(hex(BASE)), True)
        n = 0
        for begin, _size in pdata_starts(img):
            a = api.toAddr(hex(begin))
            api.disassemble(a)
            if api.createFunction(a, None):
                n += 1
        print(f"seeded {n} functions from .pdata")
        api.analyzeAll(prog)
        print("functions after analysis:", prog.getFunctionManager().getFunctionCount())


def query(cmd, args):
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor
    with pyghidra.open_program(IMAGE, project_location=PROJ_DIR, project_name="skate3",
                               analyze=False) as api:
        prog = api.getCurrentProgram()
        fm, rm = prog.getFunctionManager(), prog.getReferenceManager()

        def where(addr):
            f = fm.getFunctionContaining(addr)
            return f"{addr} in {f.getName()}@{f.getEntryPoint()}" if f else f"{addr} (no function)"

        def refs(addr):
            return [r.getFromAddress() for r in rm.getReferencesTo(addr)]

        if cmd == "decomp":
            dec = DecompInterface()
            dec.openProgram(prog)
            for a in args:
                f = fm.getFunctionContaining(api.toAddr(a))
                if not f:
                    print(f"// {a}: no function")
                    continue
                r = dec.decompileFunction(f, 120, ConsoleTaskMonitor())
                print(f"// ---- {f.getName()} @ {f.getEntryPoint()}")
                print(r.getDecompiledFunction().getC() if r.decompileCompleted() else r.getErrorMessage())
        elif cmd == "xrefs":
            for a in args:
                print(f"== {a}")
                for f in refs(api.toAddr(a)):
                    print("  ", where(f))
        elif cmd == "str":
            mem = prog.getMemory()
            for s in args:
                print(f"== {s!r}")
                a = mem.findBytes(prog.getMinAddress(), s.encode() + b"\0", None, True, ConsoleTaskMonitor())
                while a:
                    hit = refs(a)
                    print(f"  string @ {a}: {len(hit)} refs")
                    for f in hit:
                        print("    ", where(f))
                    a = mem.findBytes(a.add(1), s.encode() + b"\0", None, True, ConsoleTaskMonitor())


if __name__ == "__main__":
    pyghidra.start(install_dir=GHIDRA)
    cmd = sys.argv[1]
    build() if cmd == "build" else query(cmd, sys.argv[2:])

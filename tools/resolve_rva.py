r"""Name the RVAs in a RECOMP_SAMPLE_SECS report from the linker map.

    .venv\Scripts\python.exe tools\resolve_rva.py stderr.txt [build\sneakking_recomp.map]
"""
import bisect
import re
import sys

log = sys.argv[1]
map_path = sys.argv[2] if len(sys.argv) > 2 else "build/sneakking_recomp.map"

syms = []
base = 0x140000000
for line in open(map_path, errors="replace"):
    m = re.match(r"\s*Preferred load address is ([0-9a-f]+)", line)
    if m:
        base = int(m[1], 16)
    m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s", line)
    if m:
        syms.append((int(m[2], 16) - base, m[1]))
syms.sort()
starts = [a for a, _ in syms]


def name(rva):
    i = bisect.bisect_right(starts, rva) - 1
    return syms[i][1] if i >= 0 else "?"


for line in open(log, errors="replace"):
    m = re.match(r"\s+([0-9.]+%)\s+(.*)", line)
    if m:
        frames = [name(int(x, 16)) for x in m[2].split(" < ")
                  if re.fullmatch("[0-9A-F]+", x)]
        print(f"{m[1]:>7}  " + " < ".join(frames))

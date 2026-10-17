"""P1 analysis survey: coverage gaps, data-table code pointers, vtables, jump
tables, indirect calls, D3D usage, thread creation.

Reads game/default.xbe + analysis/disasm, writes analysis/p1/*.json and prints
a summary. Output is derived from game code: keep it in analysis/ (gitignored).

    .venv\\Scripts\\python.exe tools\\p1_survey.py
"""
import collections, json, re, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
A = ROOT / "analysis"
OUT = A / "p1"
OUT.mkdir(exist_ok=True)

info = json.load(open(ROOT / "game/default_analysis.json"))
xbe = (ROOT / "game/default.xbe").read_bytes()
BASE = int(info["base_address"], 16)


def h(s):
    return int(s, 16)


# ---- image: VA -> bytes (header parsing via the section table already in info)
secs = []
for s in info["sections"]:
    va, vs = h(s["virtual_addr"]), s["virtual_size"]
    ra, rs = h(s["raw_addr"]), s["raw_size"]
    secs.append(dict(name=s["name"], va=va, end=va + vs, raw=xbe[ra:ra + rs],
                     x="X" in s["flags"].replace(" ", "").split(",")))
img = bytearray(max(s["end"] for s in secs) - BASE)
for s in secs:
    img[s["va"] - BASE:s["va"] - BASE + len(s["raw"])] = s["raw"]


def sec_of(va):
    for s in secs:
        if s["va"] <= va < s["end"]:
            return s["name"]
    return None


CODE = {".text", "D3D", "XGRPH", "DSOUND", "WMADEC", "XMV", "XPP"}
DATA = {".rdata", ".data", ".data1"}

funcs = json.load(open(A / "disasm/functions.json"))
starts = {h(f["start"]) for f in funcs}
ranges = sorted((h(f["start"]), h(f["end"])) for f in funcs)
fn_at = {h(f["start"]): f for f in funcs}


def containing(va):
    import bisect
    i = bisect.bisect_right(ranges, (va, 1 << 40)) - 1
    if i >= 0 and ranges[i][0] <= va < ranges[i][1]:
        return ranges[i][0]
    return None


# ---- 1. coverage gaps in code sections
PAD = {0x00, 0x90, 0xCC}
gaps = []
for s in secs:
    if s["name"] not in CODE:
        continue
    cov = bytearray(s["end"] - s["va"])
    for a, b in ranges:
        if a < s["end"] and b > s["va"]:
            lo, hi = max(a, s["va"]) - s["va"], min(b, s["end"]) - s["va"]
            cov[lo:hi] = b"\x01" * (hi - lo)
    i, n = 0, len(cov)
    while i < n:
        if cov[i]:
            i += 1
            continue
        j = i
        while j < n and not cov[j]:
            j += 1
        body = img[s["va"] - BASE + i:s["va"] - BASE + j]
        real = [k for k, c in enumerate(body) if c not in PAD]
        if real:
            gaps.append(dict(section=s["name"], start=s["va"] + i + real[0],
                             end=s["va"] + i + real[-1] + 1))
        i = j
gap_bytes = collections.Counter()
for g in gaps:
    gap_bytes[g["section"]] += g["end"] - g["start"]

# ---- 2. code pointers in data sections; 3. vtables = runs of >= 2
code_secs = [s for s in secs if s["name"] in CODE]


def is_code(v):
    return any(s["va"] <= v < s["end"] for s in code_secs)


ptrs = []
for s in secs:
    if s["name"] not in DATA:
        continue
    raw = s["raw"]
    for off in range(0, len(raw) - 3, 4):
        v = struct.unpack_from("<I", raw, off)[0]
        if is_code(v):
            ptrs.append((s["va"] + off, v))
ptr_kind = collections.Counter()
missed = collections.Counter()   # pointer target not a function start
for slot, v in ptrs:
    if v in starts:
        ptr_kind["func_start"] += 1
    elif containing(v) is not None:
        ptr_kind["mid_function"] += 1
        missed[v] += 1
    else:
        ptr_kind["in_gap"] += 1
        missed[v] += 1

vtables, run = [], []
for slot, v in ptrs + [(None, None)]:
    if run and slot == run[-1][0] + 4 and v in starts:
        run.append((slot, v))
        continue
    if len(run) >= 2:
        vtables.append(dict(addr=hex(run[0][0]), slots=len(run),
                            section=sec_of(run[0][0])))
    run = [(slot, v)] if slot is not None and v in starts else []

# ---- asm scan: jump tables, indirect calls, D3D-section calls, kernel calls
LINE = re.compile(r"^\s+0x([0-9A-F]{8})\s+[0-9a-f]+\s+(\S+)\s+(.*?)\s*(?:;.*)?$")
jtabs, icalls, d3d_calls, xgrph_calls = [], collections.Counter(), collections.Counter(), collections.Counter()
thread_sites = []
icall_kinds = collections.Counter()
for asm in sorted((A / "disasm/asm").glob("*.asm")):
    sec = asm.stem if asm.stem != "text" else ".text"
    if sec not in CODE:
        continue
    for line in open(asm, encoding="utf-8", errors="replace"):
        m = LINE.match(line)
        if not m:
            continue
        va, mn, op = int(m.group(1), 16), m.group(2), m.group(3)
        if mn == "jmp" and "*4" in op.replace(" ", "") and "[" in op:
            t = re.search(r"0x([0-9a-f]+)\]", op)
            jtabs.append(dict(site=hex(va), section=sec, table=hex(int(t.group(1), 16)) if t else None))
        elif mn == "call":
            o = op.replace(" ", "")
            if re.fullmatch(r"0x[0-9a-f]+", o):
                tgt = int(o, 16)
                ts = sec_of(tgt)
                if sec == ".text" and ts == "D3D":
                    d3d_calls[tgt] += 1
                elif sec == ".text" and ts == "XGRPH":
                    xgrph_calls[tgt] += 1
            elif re.fullmatch(r"dwordptr\[0x2af[0-9a-f]{3}\]", o):
                if o == "dwordptr[0x2afbc8]":
                    thread_sites.append(hex(va))
            else:
                k = ("reg" if "[" not in o else
                     "mem_abs" if re.fullmatch(r"dwordptr\[0x[0-9a-f]+\]", o) else
                     "mem_reg_disp")
                icall_kinds[(sec, k)] += 1

# thread creation: callers of the function holding the PsCreateSystemThreadEx call
xrefs = json.load(open(A / "disasm/xrefs.json"))
calls_to = collections.defaultdict(list)
for x in xrefs:
    if x["type"] in ("call", "jump"):
        calls_to[h(x["to"])].append(h(x["from"]))
threads = []
for site in thread_sites:
    wrapper = containing(int(site, 16))
    lvl1 = sorted({containing(c) for c in calls_to.get(wrapper, []) if containing(c)})
    threads.append(dict(site=site, wrapper=hex(wrapper) if wrapper else None,
                        callers=[hex(c) for c in lvl1],
                        callers_of_callers={hex(c): sorted({hex(containing(x)) for x in calls_to.get(c, []) if containing(x)})
                                            for c in lvl1}))

res = dict(
    coverage=dict(functions=len(funcs), nonpad_gaps=len(gaps), gap_bytes=dict(gap_bytes)),
    data_code_pointers=dict(total=len(ptrs), **ptr_kind, distinct_missed_targets=len(missed)),
    vtables=dict(count=len(vtables), slots=sum(v["slots"] for v in vtables)),
    jump_tables=dict(count=len(jtabs), by_section=dict(collections.Counter(j["section"] for j in jtabs))),
    indirect_calls={f"{s}:{k}": n for (s, k), n in sorted(icall_kinds.items())},
    text_to_d3d=dict(distinct_targets=len(d3d_calls), call_sites=sum(d3d_calls.values())),
    text_to_xgrph=dict(distinct_targets=len(xgrph_calls), call_sites=sum(xgrph_calls.values())),
    threads=threads,
)
json.dump(res, open(OUT / "summary.json", "w"), indent=2)
json.dump([dict(g, start=hex(g["start"]), end=hex(g["end"])) for g in gaps], open(OUT / "gaps.json", "w"), indent=1)
json.dump(vtables, open(OUT / "vtables.json", "w"), indent=1)
json.dump(jtabs, open(OUT / "jump_tables.json", "w"), indent=1)
json.dump(sorted(hex(v) for v in missed), open(OUT / "missed_ptr_targets.json", "w"), indent=1)
json.dump({hex(k): v for k, v in d3d_calls.most_common()}, open(OUT / "text_to_d3d.json", "w"), indent=1)
json.dump({hex(k): v for k, v in xgrph_calls.most_common()}, open(OUT / "text_to_xgrph.json", "w"), indent=1)
print(json.dumps(res, indent=2))

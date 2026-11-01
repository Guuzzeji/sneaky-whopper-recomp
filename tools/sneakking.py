"""One command from your Sneak King disc image to a playable Windows build.

    sneakking build <disc.iso> [--out dist\\SneakKing] [--force] [--angle-from-source]
    sneakking settings [--out ...]     ask the setting questions again
    sneakking check                    check the build tools only

Run it through sneakking.cmd in the repo root, which sets up Python first.
The steps are the pipeline in CLAUDE.md; each one is skipped while its
inputs are older than its last successful run, so running build again
after a failure picks up where it stopped.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLKIT = ROOT / "xboxrecomp"
ANALYSIS = ROOT / "analysis"
STAMPS = ANALYSIS / ".stamps"
ANGLE_DIR = ROOT / "third_party" / "angle"
DEFAULT_OUT = ROOT / "dist" / "SneakKing"

XBE_SHA1 = "0B174AD67451D29FDC39DA1A5E6DBA35FF0F0C13"   # NA retail, title 4D5300D3
ANGLE_URL = ("https://github.com/Guuzzeji/sneaky-whopper-recomp/releases/download/"
             "angle-feebf60/angle-feebf60-win-x64.zip")
ANGLE_SHA256 = "ca8967062266827c5852db174919a46e31d690723b59313e0d71408aad84c5d7"
ANGLE_FILES = ["bin/libEGL.dll", "bin/libGLESv2.dll", "bin/d3dcompiler_47.dll",
               "bin/vulkan-1.dll", "lib/libEGL.dll.lib", "lib/libGLESv2.dll.lib"]
VSWHERE = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
               "Microsoft Visual Studio", "Installer", "vswhere.exe")
VS_COMPONENTS = ["Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "Microsoft.VisualStudio.Component.VC.CMake.Project"]
FREE_GB = 5

# (key, question, choices or None for a number). Keys are settings.ini's.
QUESTIONS = [
    ("GFX", "Graphics back end (d3d11 works almost everywhere)", ["d3d11", "vulkan", "gl"]),
    ("SCALE", "Resolution scale, times 640x480 (2 = 1280x960)", [str(i) for i in range(1, 9)]),
    ("WIDESCREEN", "Widescreen 16:9 (1) or the original 4:3 (0)", ["1", "0"]),
    ("HD_TEX", "HD textures from the Xbox 360 half of the disc (y/n)", ["y", "n"]),
    ("MOUSE_DEG", "Mouse sensitivity, camera degrees per mouse count", None),
    ("VOLUME", "Master volume, 0-100", [str(i) for i in range(101)]),
    ("VSYNC", "Vsync: wait for the screen's refresh, no tearing (1) or off (0)", ["1", "0"]),
]


# ── Settings ─────────────────────────────────────────────────

def read_settings(text):
    """KEY=value pairs, as main.c's load_settings_ini reads them."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if line and line[0] not in "#[" and "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def apply_settings(text, values):
    """Rewrite the KEY= lines of a settings.ini, keeping its comments."""
    for k, v in values.items():
        text, n = re.subn(rf"(?m)^{re.escape(k)}=.*$", lambda m: f"{k}={v}", text)
        if not n:
            text = text.rstrip("\n") + f"\n{k}={v}\n"
    return text


def ask(question, default, choices):
    while True:
        a = input(f"  {question} [{default}]: ").strip().lower() or default
        if choices is None:
            try:
                float(a)
                return a
            except ValueError:
                pass
        elif a in choices:
            return a
        if not choices:
            shown = "a number"
        elif len(choices) > 10:
            shown = f"{choices[0]} to {choices[-1]}"
        else:
            shown = ", ".join(choices)
        print(f"    Please answer {shown}.")


def configure(out):
    """Ask the setting questions and write <out>/settings.ini."""
    template = (ROOT / "settings.ini").read_text(encoding="utf-8")
    path = out / "settings.ini"
    current = read_settings(path.read_text(encoding="utf-8") if path.exists() else template)
    print("\nSettings (Enter keeps the value in brackets; change them later in "
          f"{path} or with 'sneakking settings'):")
    values = {}
    for key, q, choices in QUESTIONS:
        if key == "HD_TEX":
            on = ask(q, "n" if current.get(key) == "0" else "y", choices) == "y"
            values[key] = "game/hd" if on else "0"
        else:
            values[key] = ask(q, current.get(key, ""), choices)
    out.mkdir(parents=True, exist_ok=True)
    path.write_text(apply_settings(template, values), encoding="utf-8")
    return values


# ── Running steps ────────────────────────────────────────────

class Build:
    def __init__(self, out, force):
        self.out, self.force = out, force
        out.mkdir(parents=True, exist_ok=True)
        self.log_path = out / "build.log"
        self.log = open(self.log_path, "a", encoding="utf-8")
        self.log.write(f"\n==== {time.ctime()} ====\n")
        self.env = dict(os.environ, PYTHONIOENCODING="utf-8")

    def banner(self, n, total, title):
        print(f"[{n}/{total}] {title}", flush=True)
        self.log.write(f"\n---- {title}\n")
        self.log.flush()

    def run(self, cmd, cwd=ROOT, hint=None, shell=False):
        """Run cmd with its output in build.log, showing the time taken."""
        self.log.write(f"$ {cmd if shell else subprocess.list2cmdline(map(str, cmd))}\n")
        self.log.flush()
        t0 = time.time()
        p = subprocess.Popen(cmd if shell else [str(c) for c in cmd], cwd=cwd, env=self.env,
                             stdout=self.log, stderr=subprocess.STDOUT, shell=shell)
        while True:
            try:
                p.wait(timeout=1)
                break
            except subprocess.TimeoutExpired:
                s = int(time.time() - t0)
                print(f"\r      {s // 60}:{s % 60:02d}", end="", flush=True)
        print("\r" + " " * 16 + "\r", end="")
        if p.returncode:
            self.fail(hint or f"Step failed (exit code {p.returncode}).")

    def fail(self, msg):
        self.log.close()
        tail = self.log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-30:]
        print("\n".join("    " + l for l in tail))
        sys.exit(f"\nERROR: {msg}\nThe full log is {self.log_path}")

    def fresh(self, name, inputs):
        return not self.force and fresh_stamp(STAMPS / name, inputs)

    def done(self, name):
        STAMPS.mkdir(parents=True, exist_ok=True)
        (STAMPS / name).touch()
        return STAMPS / name


def fresh_stamp(stamp, inputs):
    """True when the stamp is newer than every input (all must exist)."""
    if not stamp.exists():
        return False
    t = stamp.stat().st_mtime
    return all(Path(i).exists() and Path(i).stat().st_mtime <= t for i in inputs)


# ── Prerequisites ────────────────────────────────────────────

def yes(question):
    return input(f"{question} [Y/n]: ").strip().lower() in ("", "y", "yes")


def winget_install(name, package_id, *extra):
    """Offer to install with winget; True if it ran and succeeded."""
    if not shutil.which("winget"):
        return False
    if not yes(f"{name} is missing. Install it with winget now?"):
        return False
    cmd = ["winget", "install", "-e", "--id", package_id,
           "--accept-package-agreements", "--accept-source-agreements", *extra]
    print("  " + subprocess.list2cmdline(cmd))
    return subprocess.run(cmd).returncode == 0


def find_vs():
    """Path of a VS 2022 install with the C++ tools and CMake, or None."""
    if not VSWHERE.exists():
        return None
    r = subprocess.run([str(VSWHERE), "-latest", "-products", "*", "-version", "[17.0,18.0)",
                        "-requires", *VS_COMPONENTS, "-property", "installationPath"],
                       capture_output=True, text=True)
    path = r.stdout.strip().splitlines()
    return Path(path[0]) if path else None


def check_tools(out, install=True):
    """Make sure the build tools are there; returns vcvars64.bat."""
    vs = find_vs()
    if not vs and install:
        winget_install("Visual Studio 2022 Build Tools (C++)",
                       "Microsoft.VisualStudio.2022.BuildTools", "--override",
                       "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools "
                       "--includeRecommended")
        vs = find_vs()
    if not vs:
        sys.exit("ERROR: Visual Studio 2022 with 'Desktop development with C++' is needed.\n"
                 "  Get the free Build Tools at https://visualstudio.microsoft.com/downloads/\n"
                 "  (Tools for Visual Studio > Build Tools for Visual Studio 2022), tick\n"
                 "  'Desktop development with C++', install, then run this again.")
    print(f"  Visual Studio: {vs}")

    out.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(out).free / 2**30
    print(f"  Free disk space: {free:.1f} GB")
    if free < FREE_GB:
        print(f"  WARNING: the build needs about {FREE_GB} GB free.")
    return vs / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"


def angle_ok():
    return all((ANGLE_DIR / f).exists() for f in ANGLE_FILES)


def get_angle(b, from_source):
    if from_source:
        b.run(["cmd", "/c", ROOT / "tools" / "build_angle.cmd"],
              hint="Building ANGLE failed; see docs/BUILDING.md.")
        return
    zpath = Path(tempfile.gettempdir()) / "sneakking-angle.zip"
    print(f"      downloading {ANGLE_URL}")
    try:
        urllib.request.urlretrieve(ANGLE_URL, zpath)
    except OSError as e:
        b.fail(f"Couldn't download ANGLE ({e}). Check your connection, or build it "
               "with --angle-from-source.")
    digest = hashlib.sha256(zpath.read_bytes()).hexdigest()
    if digest != ANGLE_SHA256:
        zpath.unlink()
        b.fail(f"The ANGLE download is damaged (SHA-256 {digest}). Run build again.")
    with zipfile.ZipFile(zpath) as z:
        z.extractall(ANGLE_DIR)
    zpath.unlink()


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


# ── Commands ─────────────────────────────────────────────────

def cmd_check(args):
    check_tools(Path(args.out).resolve(), install=False)
    print(f"  ANGLE: {'ready' if angle_ok() else 'not downloaded yet (build gets it)'}")


def cmd_settings(args):
    out = Path(args.out).resolve()
    v = configure(out)
    if v["HD_TEX"] != "0" and not (out / "game" / "hd").exists():
        print("Run 'sneakking build' again to make the HD textures.")


def cmd_build(args):
    out = Path(args.out).resolve()
    game = out / "game"
    xbe = game / "default.xbe"
    unpacked = game / ".unpacked"
    if not args.iso and not unpacked.exists():
        sys.exit("ERROR: give the path to your disc image: sneakking build <disc.iso>")
    if args.iso and not Path(args.iso).is_file():
        sys.exit(f"ERROR: {args.iso} not found.")

    # Questions first, so the rest runs without anyone at the keyboard.
    if not (out / "settings.ini").exists():
        configure(out)
    settings = read_settings((out / "settings.ini").read_text(encoding="utf-8"))

    b = Build(out, args.force)
    py = sys.executable
    seeds = sorted((ROOT / "seeds").glob("*.json"))
    mains = [ROOT / "src" / f for f in ("recomp_manual.c", "gl_hle.c", "d3d_hle_shared.h", "main.c")]
    hd = settings.get("HD_TEX", "0") not in ("", "0")
    total = 10 + hd
    n = iter(range(1, total + 1))

    b.banner(next(n), total, "Checking the build tools")
    vcvars = check_tools(out)

    b.banner(next(n), total, "Getting ANGLE (graphics libraries)")
    if angle_ok() and not args.angle_from_source:
        print("      already there")
    else:
        get_angle(b, args.angle_from_source)
        if not angle_ok():
            b.fail(f"ANGLE is incomplete in {ANGLE_DIR}.")

    b.banner(next(n), total, "Unpacking the disc (about 1.6 GB)")
    if unpacked.exists() and not args.force:
        print("      already unpacked")
    else:
        iso = Path(args.iso).resolve()
        not_xbox = "This image doesn't look like an original Xbox disc."
        game.mkdir(parents=True, exist_ok=True)
        b.run([py, "-m", "tools.xiso", "get", iso, "default.xbe", "-o", xbe],
              cwd=TOOLKIT, hint=not_xbox)
        if sha1(xbe) != XBE_SHA1:
            b.fail("This isn't the North American retail disc of Burger King: Sneak King "
                   f"(default.xbe SHA-1 {sha1(xbe)}, expected {XBE_SHA1}).")
        b.run([py, "-m", "tools.xiso", "unpack", iso, "-o", game], cwd=TOOLKIT, hint=not_xbox)
        unpacked.touch()

    lift = [
        ("xbe", "Reading the game's header", [xbe],
         [py, "-m", "tools.xbe_parser", xbe, "--json", game / "default_analysis.json"]),
        ("disasm", "Disassembling (about 15 minutes)", seeds,
         [py, "-m", "tools.disasm", xbe, "-o", ANALYSIS / "disasm", "--force"]
         + [a for s in seeds for a in ("--seed-functions", s)]),
        ("func_id", "Identifying library functions", [],
         [py, "-m", "tools.func_id", xbe, "--functions", ANALYSIS / "disasm" / "functions.json",
          "--strings", ANALYSIS / "disasm" / "strings.json",
          "--xrefs", ANALYSIS / "disasm" / "xrefs.json", "-o", ANALYSIS / "func_id"]),
        ("abi", "Working out calling conventions", [],
         [py, "-m", "tools.abi_analysis", xbe, "--disasm-dir", ANALYSIS / "disasm",
          "--func-id-dir", ANALYSIS / "func_id", "--output-dir", ANALYSIS / "abi"]),
        ("recomp", "Translating to C (a few minutes)", mains,
         [py, "-m", "tools.recomp", xbe, "--all", "--split", "250",
          "--disasm-dir", ANALYSIS / "disasm", "--func-id-dir", ANALYSIS / "func_id",
          "--abi-dir", ANALYSIS / "abi", "--gen-dir", ROOT / "src" / "recomp" / "gen",
          "--game-name", "Sneak King"]
         + [a for m in mains for a in ("--exclude-manual", m)]
         + [a for r in ("1EC5C0-20FB40", "23CB60-262FEC", "2A65C0-2AFB10")
            for a in ("--mmio-range", r)]),
    ]
    names = ANALYSIS / "names_combined.json"
    prev = unpacked
    for key, title, inputs, cmd in lift:
        b.banner(next(n), total, title)
        if b.fresh(key, [prev, *inputs]):
            print("      up to date")
        else:
            b.run(cmd, cwd=TOOLKIT)
            # Developers with the XbSymbolDatabase/FidDb names get them applied;
            # nothing needs them (main.c calls the entry point by its sub_ name).
            if key == "disasm" and names.exists():
                b.run([py, Path("tools") / "ghidra_naming" / "merge_names.py",
                       "--names-json", names, "--functions-json",
                       ANALYSIS / "disasm" / "functions.json", "--apply"], cwd=TOOLKIT)
            b.done(key)
        prev = STAMPS / key

    b.banner(next(n), total, "Compiling (about 10 minutes the first time)")
    b.run(f'call "{vcvars}" >nul && cmake --preset release && cmake --build --preset release',
          shell=True, hint="The compile failed.")

    if hd:
        b.banner(next(n), total, "Converting the HD textures")
        hd_dir = game / "hd"
        pairs = [(x, x.with_suffix(".xen")) for x in sorted((game / "Packages").glob("*.xbp"))
                 if x.with_suffix(".xen").exists()]
        if (hd_dir / ".done").exists() and not args.force:
            print("      already converted")
        else:
            for xbp, xen in pairs:
                b.run([py, ROOT / "tools" / "xen_tex.py", xbp, xen, "-o", hd_dir])
            (hd_dir / ".done").touch()

    b.banner(next(n), total, "Putting the game together")
    shutil.copy2(ROOT / "build" / "sneakking_recomp.exe", out / "SneakKing.exe")
    for dll in ("libEGL.dll", "libGLESv2.dll", "d3dcompiler_47.dll", "vulkan-1.dll"):
        shutil.copy2(ROOT / "build" / dll, out / dll)
    if not (out / "mkb.ini").exists():
        shutil.copy2(ROOT / "mkb.ini", out / "mkb.ini")

    print(f"\nDone. Play: {out / 'SneakKing.exe'}\n"
          f"Settings: {out / 'settings.ini'}   Keys: {out / 'mkb.ini'}\n"
          f"Bug reports: attach {out / 'logs' / 'sneakking.log'} (written each time you play)")


def main(argv=None):
    p = argparse.ArgumentParser(prog="sneakking", description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="build the game from your disc image")
    b.add_argument("iso", nargs="?", help="your Sneak King disc image (.iso)")
    b.add_argument("--force", action="store_true", help="redo every step")
    b.add_argument("--angle-from-source", action="store_true",
                   help="build ANGLE instead of downloading it (about 9 GB, slow)")
    s = sub.add_parser("settings", help="change the game's settings")
    c = sub.add_parser("check", help="check the build tools")
    for x in (b, s, c):
        x.add_argument("--out", default=str(DEFAULT_OUT),
                       help=f"game folder (default {DEFAULT_OUT.relative_to(ROOT)})")
    args = p.parse_args(argv)
    try:
        {"build": cmd_build, "settings": cmd_settings, "check": cmd_check}[args.cmd](args)
    except KeyboardInterrupt:
        sys.exit("\nStopped. Run the same command again to carry on.")


if __name__ == "__main__":
    main()

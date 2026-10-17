# CLAUDE.md: Sneak King static recompilation

This project statically recompiles the **original Xbox** `default.xbe` of *Burger King: Sneak King* (2006, Blitz Games, BlitzTech engine) into a native Windows executable. There is no emulator.

The work uses the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit.

- Background: `docs/RESEARCH.md`
- Phases and progress: `docs/ROADMAP.md`. Tick boxes there as work lands.

## Hard rules
- **Never commit or upload game data.** That means the ISO, XBE, XEX, `game/`, `analysis/` (it contains disassembly) and `src/recomp/gen/` (lifted game code). `.gitignore` covers all of these; keep it that way. The user owns the physical disc.
- **Target the OG Xbox build only.** The disc is hybrid and also holds a 360 `default.xex`. We decided against XenonRecomp: its CPU lifter is proven, but the runtime would be mostly DIY. Use the XEX only as a cross-reference.
- **Never hand-edit `src/recomp/gen/`.** It is regenerated. Fixes go in:
  - `src/recomp_manual.c` (overrides)
  - JSON config passed to `tools.recomp` (seeds, coalesce, icall sites, trace)
  - upstream xboxrecomp

## Layout
| Path | What |
|---|---|
| `sneak-king-usa-dump-10-16-2026.iso` | User's redump. The XDVDFS game partition starts at offset `0x18300000`. |
| `game/` | Extracted disc: 143 files, 1.55 GB, including `default.xbe` and `default_analysis.json`. |
| `analysis/{disasm,func_id,abi}/` | Pipeline output (functions, xrefs, labels, strings, identified functions, ABI). |
| `src/main.c`, `src/recomp_manual.c` | Host entry point and manual overrides. Made from the xboxrecomp `templates/new-game`. |
| `src/recomp/gen/` | Generated lifted C (Phase 2 onward). |
| `CMakeLists.txt` | Calls `add_subdirectory(${XBOXRECOMP_DIR})` and links `xboxrecomp`. |
| `C:\repos\xboxrecomp` | Toolkit, a sibling clone at v0.14.0. `XBOXRECOMP_DIR` defaults to `../xboxrecomp`. |

## XBE facts
- **Identity:** retail build, title ID `0x4D5300D3`, NA region. SHA-1 `0B174AD67451D29FDC39DA1A5E6DBA35FF0F0C13`.
- **Addresses:** base `0x00010000`, image size `0x330600`, entry `0x000637AA`, kernel thunks at `0x002AFB20`.
- **Code sections:** `.text`, D3D, XGRPH, DSOUND, WMADEC, XMV, XPP. Code status of DOLBY is unknown.
- **Data sections:** `.rdata`, `.data`, `.data1`.
- **XDK:** build 5849 for all libraries (XAPILIB, D3D8, XBOXKRNL, LIBCMT, XGRAPHC, DSOUND, XVOICE, XMV).
- **Kernel imports:** 160, and the xboxrecomp kernel bridge routes all of them.
- **Analysis counts:**
  - 13,318 functions (10,554 in `.text`)
  - 2,345 thiscall functions (C++)
  - 698 function pointers in data tables
- D3D8 is statically linked, so the NV2A pushbuffer path is likely. This is still to be confirmed in P1.

## Environment
- **Compiler:** MSVC, from VS 2022 Community at `C:\Program Files\Microsoft Visual Studio\2022\Community`. Plain shells don't have `cl`/`ninja` on PATH. Wrap commands:
  `cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && <command>'`
- **Python:** use `C:\repos\sneak-king-recomp\.venv\Scripts\python.exe` (packages: capstone, pytest, pefile, numpy). The system Python is uv-managed, so `pip install` is blocked.
- **Running toolkit tools:** run `tools.*` with the toolkit as the working directory (`C:\repos\xboxrecomp`). Set `PYTHONIOENCODING=utf-8` so report printing doesn't crash.
- **Ghidra:** 12.1.4 at `C:\Users\SandGames\Desktop\ghidra_12.1.4_PUBLIC_20260921`. ghidra-mcp is also available.
- **xemu:** installed through winget as the reference emulator. It needs the user's own BIOS/MCPX dumps.
- **Known environment issue:** the toolkit conformance "corpus" stage fails with `PermissionError: [WinError 5]` when it launches `corpus_harness.exe` from `%TEMP%`. The core conformance run passes: 6,351 vectors, 0 mismatches.

## Pipeline commands
Run these from `C:\repos\xboxrecomp`, with `$py` set to the venv Python, `$x` to `C:\repos\sneak-king-recomp\game\default.xbe` and `$a` to `C:\repos\sneak-king-recomp\analysis`.

```powershell
& $py -m tools.xiso unpack <iso> -o C:\repos\sneak-king-recomp\game
& $py -m tools.xbe_parser $x --json C:\repos\sneak-king-recomp\game\default_analysis.json
& $py -m tools.disasm $x -o $a\disasm                       # ~8 min, all executable sections
& $py -m tools.func_id $x --functions $a\disasm\functions.json --strings $a\disasm\strings.json --xrefs $a\disasm\xrefs.json -o $a\func_id
& $py -m tools.abi_analysis $x --disasm-dir $a\disasm --func-id-dir $a\func_id --output-dir $a\abi
& $py -m tools.recomp $x --all --split 250 --disasm-dir $a\disasm --func-id-dir $a\func_id --abi-dir $a\abi --gen-dir C:\repos\sneak-king-recomp\src\recomp\gen --game-name "Sneak King"
```

Build from the repo root inside the vcvars wrapper. Keep parallelism low, because the generated files are huge:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 2
```

Run the exe from the repo root, because `main.c` uses the relative paths `game\default.xbe` and `game`.

## Debug loop (P3+)
- **Indirect calls:**
  1. Build with `RECOMP_ICALL_FEEDBACK`.
  2. Run the game.
  3. Run `tools.recomp.icall_feedback merge`, then `seeds --out icall_seeds.json --align 16`.
  4. Run `tools.disasm --seed-functions icall_seeds.json`.
  5. Lift again.
- **Function boundaries:**
  - `--coalesce-functions` for functions that were wrongly split.
  - `--trace-functions` for entry traces.
  - `--force-return ADDR=VALUE` to make a function return a constant.
- **Compare against reference:**
  - Compare behaviour with xemu.
  - Use RenderDoc on the host D3D11 output.
  - Use Ghidra for naming.
- **Upstream fixes:** keep toolkit fixes small, and send them upstream to xboxrecomp when they are generic.

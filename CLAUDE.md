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
| `tools/p1_survey.py` | P1 survey (gaps, data pointers, vtables, jump tables, D3D usage, threads). Writes to `analysis/p1/`. |
| `seeds/` | Seed lists for `tools.disasm --seed-functions`. They hold addresses only. |
| `CMakeLists.txt` | Calls `add_subdirectory(${XBOXRECOMP_DIR})` and links `xboxrecomp`. |
| `C:\repos\xboxrecomp` | Toolkit, a sibling clone at v0.14.0. `XBOXRECOMP_DIR` defaults to `../xboxrecomp`. |

## XBE facts
- **Identity:** retail build, title ID `0x4D5300D3`, NA region. SHA-1 `0B174AD67451D29FDC39DA1A5E6DBA35FF0F0C13`.
- **Addresses:** base `0x00010000`, image size `0x330600`, entry `0x000637AA`, kernel thunks at `0x002AFB20`.
- **Code sections:** `.text`, D3D, XGRPH, DSOUND, WMADEC, XMV, XPP. DOLBY is flagged X but holds MCPX APU DSP microcode (the Dolby encoder). DSOUND references it as data. Never lift it.
- **Data sections:** `.rdata`, `.data`, `.data1`.
- **XDK:** build 5849 for all libraries (XAPILIB, D3D8, XBOXKRNL, LIBCMT, XGRAPHC, DSOUND, XVOICE, XMV).
- **Kernel imports:** 160, and the xboxrecomp kernel bridge routes all of them.
- **Analysis counts:**
  - 13,650 functions after the P2 seeds (13,318 in P1; 10,554 in `.text`)
  - 2,345 thiscall functions (C++)
  - 698 function pointers in data tables
- D3D8 (non-LTCG) is statically linked. **Graphics path: NV2A pushbuffer executor.** This was decided in P1; see `docs/P1_FINDINGS.md`. Override only the D3D entry points that wait on hardware.
- Applied function names carry a prefix: `xdk_` for XbSymbolDatabase names and `crt_` for Ghidra FidDb names. Without it, XDK names collide with toolkit runtime symbols.

## Environment
- **Compiler:** MSVC, from VS 2022 Community at `C:\Program Files\Microsoft Visual Studio\2022\Community`. Plain shells don't have `cl`/`ninja` on PATH. Wrap commands:
  `cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && <command>'`
- **Python:** use `C:\repos\sneak-king-recomp\.venv\Scripts\python.exe` (packages: capstone, pytest, pefile, numpy). The system Python is uv-managed, so `pip install` is blocked.
- **Running toolkit tools:** run `tools.*` with the toolkit as the working directory (`C:\repos\xboxrecomp`). Set `PYTHONIOENCODING=utf-8` so report printing doesn't crash.
- **Ghidra:** 12.1.4 at `C:\Users\SandGames\Desktop\ghidra_12.1.4_PUBLIC_20260921\ghidra_12.1.4_PUBLIC` (note the nested folder). ghidra-mcp is also available.
  - Ghidra 12 has no Jython. Run the toolkit's `.py` Ghidra scripts under PyGhidra, which is installed in the venv, with `@runtime PyGhidra`. The command is in `docs/P1_FINDINGS.md`.
  - The analyzed project is in `C:\repos\xboxrecomp\tools\ghidra_naming\work\`. That repo's `.git/info/exclude` keeps it out of git.
- **XbSymbolDatabase:** run the CLI as `toolkit\xbsymbol\win_x64\bin\XbSymbolDatabaseCLI.exe <xbe> -e`.
- **REA: always use it** (the `mcp__rea__*` tools) for reverse-engineering questions: decompiling, reading a function's assembly, xrefs, callers/callees, strings, and checking a lifted function against the original. Prefer it over raw capstone dumps or grepping `analysis/disasm/asm`.
  - Open `analysis/rea/default_flat.exe` with `open_binary`. That file is the XBE flat image wrapped in a PE32, because REA's Windows Ghidra provider accepts only PE. Its addresses match the XBE VAs and `functions.json` 1:1. Regenerate it with `.venv\Scripts\python.exe tools\xbe_to_pe.py`.
  - The provider is Ghidra 12.1.4. The first query waits for auto-analysis to finish, which takes a few minutes.
  - The session database is temporary. Names and comments set through REA are lost when it closes, so put lasting names in `analysis/names_combined.json` instead.
- **xemu:** the reference emulator, at `toolkit\xemu\xemu.exe` (gitignored). The `config\` folder holds the user's BIOS, MCPX and HDD image. Settings live in `%APPDATA%\xemu\xemu\xemu.toml`, which uses absolute paths, so update it whenever the folder moves.
- **Known environment issue:** this machine blocks executing freshly compiled exes from `%TEMP%` (`PermissionError: [WinError 5]`). Run the toolkit tests with TEMP redirected; then all pass (805 passed, 7 skipped):
  `cmd /c 'set TMP=C:\repos\xboxrecomp\build\conftmp&& set TEMP=C:\repos\xboxrecomp\build\conftmp&& "<vcvars64.bat>" >nul 2>&1 && cd /d C:\repos\xboxrecomp && <venv python> -m pytest tools/ -q'`

## Pipeline commands
Run these from `C:\repos\xboxrecomp`, with `$py` set to the venv Python, `$x` to `C:\repos\sneak-king-recomp\game\default.xbe` and `$a` to `C:\repos\sneak-king-recomp\analysis`.

```powershell
& $py -m tools.xiso unpack <iso> -o C:\repos\sneak-king-recomp\game
& $py -m tools.xbe_parser $x --json C:\repos\sneak-king-recomp\game\default_analysis.json
& $py -m tools.disasm $x -o $a\disasm                       # ~8 min, all executable sections
& $py -m tools.func_id $x --functions $a\disasm\functions.json --strings $a\disasm\strings.json --xrefs $a\disasm\xrefs.json -o $a\func_id
& $py -m tools.abi_analysis $x --disasm-dir $a\disasm --func-id-dir $a\func_id --output-dir $a\abi
& $py -m tools.recomp $x --all --split 250 --disasm-dir $a\disasm --func-id-dir $a\func_id --abi-dir $a\abi --gen-dir C:\repos\sneak-king-recomp\src\recomp\gen --game-name "Sneak King" --exclude-manual C:\repos\sneak-king-recomp\src\recomp_manual.c
```

Build from the repo root inside the vcvars wrapper. Keep parallelism low, because the generated files are huge:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 2
```

Run the exe from the repo root, because `main.c` uses the relative paths `game\default.xbe` and `game`.

Run it with `RECOMP_AC97_READY=1`, `RECOMP_PB_EXEC=1` and `RECOMP_FB_WINDOW=1`:
- Without the first, DirectSound init fails and the game shuts itself down.
- Without the second, nothing writes the D3D fence semaphore.
- Without the third, nothing is visible.

Other useful switches:
- `RECOMP_WATCHDOG_SECS=N`: dumps the guest stack of the boot thread after N seconds.
- `RECOMP_KERNEL_LOG_BUDGET=N`: logs the first N kernel calls (the default is 200).
- `RECOMP_SAMPLE_SECS=N`: profiles the main thread. Resolve its output with `tools/resolve_rva.py`.
- `RECOMP_PEEK=0xVA,...`: reads guest dwords.

The toolkit needs local patches that are not upstream yet. They are on the xboxrecomp branch `sneak-king-p3-boot`, stacked on `lifter-cpu-probe`: the cpuid/pushfd lifter fix (P2), plus the contiguous-arena top-down fallback, `nv2a_pb_set_semaphore_device_ptr` and the APU EP heartbeat (P3), plus the P4 fixes: read-only D:, guest APCs (`NtQueueApcThread`, queued I/O completion), the QPC vblank tick and IRQ hold, and the fixed-function transform of inline arrays with integer depth. `main.c` turns `RECOMP_VBLANK` on by default, because the XMV player needs it.

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
  - Use REA to decompile and inspect the original function. Use Ghidra headless (`ghidra_naming`) for bulk naming.
- **Upstream fixes:** keep toolkit fixes small, and send them upstream to xboxrecomp when they are generic.

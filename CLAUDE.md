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
| `src/d3d9_hle.c` | The game's Xbox D3D8 mapped onto host Direct3D 9: wrapped entry points, state translation, textures, render targets, vertex programs, present. |
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
- D3D8 (non-LTCG) is statically linked. **Graphics path: D3D8 HLE on Direct3D 9** (`src/d3d9_hle.c`). It replaced the NV2A pushbuffer executor, the D3D11 back end and the software rasteriser; see `docs/P1_FINDINGS.md` §3. D3D9 is the only renderer: don't reintroduce another one.
  - The game's D3D entry points are wrapped (`sub_X` → `hle_*`). Setters run the lifted body and mirror into D3D9; draws, Clear and the present are replaced.
  - Render and texture-stage state are read from D3D's guest arrays at each draw: `D3D_g_RenderState` `0x20D090` (XDK 5849 indices) and `D3D_g_DeferredTextureState` `0x20CE90`. The current render target and depth surface are at `device+0x1A04` and `device+0x1A08`.
  - Vertex programs: NV2A microcode is translated to `vs_2_0` HLSL, decoded the way the toolkit's `nv2a_vsh_interp.c` does. `SetVertexShaderConstant1/4/NotInline` take the hardware register (0..191), and NotInline's count is in dwords.
  - The lifted D3D still writes a pushbuffer. Nothing executes it: the toolkit's NV2A model moves DMA_GET to PUT, and `xbox_Nv2aMirrorFence` keeps D3D's semaphore at its time stamp, so fence waits return.
  - Pixel shaders (register combiners) are not translated yet. The title creates two; none was bound in the menus or the first level.
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
& $py -m tools.disasm $x -o $a\disasm --force --seed-functions C:\repos\sneak-king-recomp\seeds\p1_static.json --seed-functions C:\repos\sneak-king-recomp\seeds\p3_runtime.json --seed-functions C:\repos\sneak-king-recomp\seeds\p5_xpp.json   # ~13 min
& $py tools\ghidra_naming\merge_names.py --names-json $a\names_combined.json --functions-json $a\disasm\functions.json --apply   # after disasm, before recomp
& $py -m tools.func_id $x --functions $a\disasm\functions.json --strings $a\disasm\strings.json --xrefs $a\disasm\xrefs.json -o $a\func_id
& $py -m tools.abi_analysis $x --disasm-dir $a\disasm --func-id-dir $a\func_id --output-dir $a\abi
& $py -m tools.recomp $x --all --split 250 --disasm-dir $a\disasm --func-id-dir $a\func_id --abi-dir $a\abi --gen-dir C:\repos\sneak-king-recomp\src\recomp\gen --game-name "Sneak King" --exclude-manual C:\repos\sneak-king-recomp\src\recomp_manual.c --exclude-manual C:\repos\sneak-king-recomp\src\d3d9_hle.c
```

Build from the repo root inside the vcvars wrapper. Keep parallelism low, because the generated files are huge. `CMakePresets.json` holds the settings (Ninja, 2 jobs): `release` builds `build\sneakking_recomp.exe`, `relwithdebinfo` builds an optimised exe with a PDB in `out\build\relwithdebinfo` (an unoptimised build of the generated code is too slow to play):

```
cmake --preset release && cmake --build --preset release
```

**IDEs.** Visual Studio 2022: File > Open > Folder on the repo root, pick a preset in the toolbar, then F5. VS Code: needs the CMake Tools and C/C++ extensions; `.vscode/settings.json` makes CMake Tools use the presets, and the "Sneak King" launch configuration in `.vscode/launch.json` debugs the selected build (set `RECOMP_*` switches in its `environment`). Started without redirected output, the exe opens a console for its log.

The exe finds the repo root itself: when `game\default.xbe` is not in the working directory, `main.c` walks up from the exe's folder to the one that holds it and works from there (`game\` and `mkb.ini` are relative to it).

`main.c` turns on everything a normal run needs, so the exe runs with no switches set:
- `RECOMP_AC97_READY`: without it, DirectSound init fails and the game shuts itself down.
- `RECOMP_FB_WINDOW`: the window D3D9 presents into. Without it, nothing is visible.

It also turns these on. Set any switch to `0` to turn it off:
- `RECOMP_VBLANK`: the XMV player times frames by vblank.
- `RECOMP_USB` and `RECOMP_MKB`: the pad on the emulated OHCI bus, and keyboard and mouse as a pad on port 0 (raw input, toolkit `src/input/mkb_input.c`). The bindings are in `mkb.ini` at the repo root (`RECOMP_MKB_INI` moves it): WASD move, mouse camera, Shift run, left click or Space A, right click or Q B, E Y, wheel zoom, F first person, Escape START, arrows and Enter for menus. F9 releases the mouse. `RECOMP_MKB=0` falls back to the keyboard-only `RECOMP_KEYBOARD` pad.
- `RECOMP_HEAP_RECLAIM`: reuse freed contiguous memory. Without it the movies run the arena dry.
- `RECOMP_SCALE=N` (1 to 8, default 2): D3D9 render targets at N times the guest's 640x480, shown through a swap chain on the framebuffer window.

`main.c` also calls `timeBeginPeriod(1)`. Without it the APU clock oversleeps and audio underruns.

Other useful switches:
- `RECOMP_WATCHDOG_SECS=N`: dumps the guest stack of the boot thread after N seconds.
- `RECOMP_KERNEL_LOG_BUDGET=N`: logs the first N kernel calls (the default is 200).
- `RECOMP_SAMPLE_SECS=N`: profiles the main thread; `RECOMP_SAMPLE_DELAY=N` starts it later. `RECOMP_SAMPLE_ALL=1` samples every thread and prints each thread's CPU time and start address. Resolve the output with `tools/resolve_rva.py`.
- `RECOMP_PEEK=0xVA,...`: reads guest dwords.
- `RECOMP_PAD_LIVE=<file>`: each line appended to the file is a pad press (`start:200`, `down`, `a`, `b`). `RECOMP_PAD_SCRIPT=<ms>:<button>,...` is the same on a timeline. With `RECOMP_D3D9_DUMP=<prefix>` and `RECOMP_D3D9_DUMP_EVERY=<presents>` (BMPs box-filtered to 640x480), this drives the menus with no one at the keyboard. START skips the movies.
- `RECOMP_APU_TRACE=1`: per-voice audio stats once a second. `RECOMP_AUDIO_WAV=<path>` records the output.
- `RECOMP_D3D9_STATS=1`: presents per second.
- `RECOMP_D3D_TRACE=N`: logs the first N D3D calls with their register and stack arguments; `RECOMP_D3D_TRACE_ONLY=Name,Name` keeps just those.
- `RECOMP_D3D9_DUMP_VSH=<dir>`: writes each translated vertex program as HLSL.

The toolkit needs local patches that are not upstream yet. They are on the xboxrecomp branch `sneak-king-p3-boot`, stacked on `lifter-cpu-probe`: the cpuid/pushfd lifter fix (P2), plus the contiguous-arena top-down fallback, `nv2a_pb_set_semaphore_device_ptr` and the APU EP heartbeat (P3), plus the P4 fixes: read-only D:, guest APCs (`NtQueueApcThread`, queued I/O completion), the QPC vblank tick and IRQ hold, and the fixed-function transform of inline arrays with integer depth, plus the P5 fixes: the per-kick `DMA_PUT` trace gated on `RECOMP_NV2A_TRACE`, and for render back ends, `clip_x`, `clip_y` and `format` in `Nv2aSurface`, `palette` in `Nv2aTexture`, and `flip()` called before the window copies the frame, plus the P5.4 fix: `Nv2aBatch.rc`, which passes the register-combiner program to render back ends, plus the P6 fix: `Nv2aBatch.stages` and `fog_color` and `Nv2aVertex` `specular`, `fog` and `tc`, which pass every texture stage the combiners read, plus `Nv2aRenderState.zeta_format` and `Nv2aBatch.clip_plane_mode`, per-vertex transform failure, and one shared worker pool for transform and raster, plus the P7 fixes: `mkb_input.c` bindings from the ini (`bind.<PAD>=...`), held mouse buttons, generic Shift/Ctrl, and poll-rate-independent mouse aim, and `xbox_FramebufferWindowHwnd` / `xbox_FramebufferWindowExternalPresent` so a back end can present into the framebuffer window, plus the D3D9 fix: `tools.recomp --exclude-manual` can be given more than once.

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
  - RenderDoc and PIX don't capture D3D9. Run the exe with DXVK's `d3d9.dll` beside it (D3D9 on Vulkan) and capture that, or use `RECOMP_D3D_TRACE`.
  - Reference frames from the old D3D11 path are in `analysis/golden_d3d11/`. `tools/bmp2png.py` converts dumps for viewing.
  - Use REA to decompile and inspect the original function. Use Ghidra headless (`ghidra_naming`) for bulk naming.
- **Upstream fixes:** keep toolkit fixes small, and send them upstream to xboxrecomp when they are generic.

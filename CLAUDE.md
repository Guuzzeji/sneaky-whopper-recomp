# CLAUDE.md: Sneak King static recompilation

This project statically recompiles the **original Xbox** `default.xbe` of *Burger King: Sneak King* (2006, Blitz Games, BlitzTech engine) into a native Windows executable. There is no emulator.

The work uses the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit.

- Background: `docs/RESEARCH.md`
- Phases and progress: `docs/ROADMAP.md`. Tick boxes there as work lands.

## Hard rules
- **Never commit or upload game data.** That means the ISO, XBE, XEX, `game/`, `analysis/` (it contains disassembly) and `src/recomp/gen/` (lifted game code). `.gitignore` covers all of these; keep it that way. The user owns the physical disc. The same applies to the reference games for the joke modes: `mw1/` (CoD4 PC install) and `skate3/` (unpacked from `skate-3-usa-x360-dump-10-17-26.iso` in the root), and anything extracted from them into `game/mods/`.
- **Target the OG Xbox build only.** The disc is hybrid and also holds a 360 `default.xex`. We decided against XenonRecomp: its CPU lifter is proven, but the runtime would be mostly DIY. Use the XEX only as a cross-reference.
- **Never hand-edit `src/recomp/gen/`.** It is regenerated. Fixes go in:
  - `src/recomp_manual.c` (overrides)
  - JSON config passed to `tools.recomp` (seeds, coalesce, icall sites, trace)
  - the vendored toolkit in `xboxrecomp/` (ours to patch; send generic fixes upstream)

## Layout
| Path | What |
|---|---|
| `sneak-king-usa-dump-10-16-2026.iso` | User's redump. The XDVDFS game partition starts at offset `0x18300000`. |
| `game/` | Extracted disc: 143 files, 1.55 GB, including `default.xbe` and `default_analysis.json`. |
| `mw1/` | The user's CoD4: Modern Warfare PC install (`iw3sp.exe`, `zone/english/*.ff`, `main/iw_*.iwd`). Reference for the shooter joke mode; gitignored. |
| `analysis/{disasm,func_id,abi}/` | Pipeline output (functions, xrefs, labels, strings, identified functions, ABI). |
| `src/main.c`, `src/recomp_manual.c` | Host entry point and manual overrides. Made from the xboxrecomp `templates/new-game`. |
| `src/d3d9_hle.c` | The game's Xbox D3D8 mapped onto host Direct3D 9: wrapped entry points, state translation, textures, render targets, vertex programs, present. |
| `src/overlay.cpp`, `third_party/imgui/` | F1 debug overlay (D3D9, health, cheats) on vendored Dear ImGui 1.91.8. Game-side helpers (cheats, mouse camera, menu hover) are in `recomp_manual.c`. |
| `tools/cod4_extract.py`, `tools/cod4_anim.py`, `src/cod4_anim.c`, `src/mods_mesh.c`, `src/cod4_sound.c` | Shooter joke mode (P9), CoD4 in third person: guns (OBJ, DDS, `weapon.ini`, WAVs) and player animations retargeted onto the King, extracted from `mw1/` into `game/mods/`; the animation player, the world-space gun/flash/tracer pass and the gun sounds. The gameplay hooks are in `recomp_manual.c`; see `docs/MODS.md`, `docs/MW1_NOTES.md`. |
| `src/skate.c` | Skate joke mode: board physics on the King's physics controller, flick-it on the mouse, tricks, bails, the stance on his bones, camera follow, synthesised sounds. The board is drawn by `mods_mesh.c`. See `docs/MODS.md`. |
| `skate3/`, `tools/xex_to_pe.py`, `tools/lzx_cli.c`, `tools/skate3_ghidra.py`, `tools/skate3_big.py`, `tools/skate3_attrib.py`, `tools/skate3_extract.py`, `tools/skate3_anim.py` | Skate joke mode research and assets: the unpacked 360 disc; XEX to PE (AES + LZX through libmspack); headless Ghidra queries; EB BIG archives with RefPack; the AttribSys tuning DB; Skate 3 sounds (through vgmstream) and animations (through the user's checkout of chasmlol/skate-3-trick-toolkit) into `game/mods/skate3/`. The retarget step shared with CoD4 is `tools/king_retarget.py`. See `docs/SKATE3_NOTES.md`. |
| `tools/xen_tex.py` | Converts the 360 `.xen` package textures to `game/hd/<hash>.dds` for `RECOMP_HD_TEX`. |
| `src/recomp/gen/` | Generated lifted C (Phase 2 onward). |
| `tools/p1_survey.py` | P1 survey (gaps, data pointers, vtables, jump tables, D3D usage, threads). Writes to `analysis/p1/`. |
| `seeds/` | Seed lists for `tools.disasm --seed-functions`. They hold addresses only. |
| `CMakeLists.txt` | Calls `add_subdirectory(${XBOXRECOMP_DIR})` and links `xboxrecomp`. |
| `xboxrecomp/` | The toolkit: upstream v0.14.0 plus our patches, vendored as a git subtree with its history. `CMakeLists.txt` points `XBOXRECOMP_DIR` here. Edit it like any other code in this repo. |

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
- **Running toolkit tools:** run `tools.*` with the toolkit as the working directory (`C:\repos\sneak-king-recomp\xboxrecomp`). Set `PYTHONIOENCODING=utf-8` so report printing doesn't crash.
- **Ghidra:** 12.1.4 at `C:\Users\SandGames\Desktop\ghidra_12.1.4_PUBLIC_20260921\ghidra_12.1.4_PUBLIC` (note the nested folder). ghidra-mcp is also available.
  - Ghidra 12 has no Jython. Run the toolkit's `.py` Ghidra scripts under PyGhidra, which is installed in the venv, with `@runtime PyGhidra`. The command is in `docs/P1_FINDINGS.md`.
  - The analyzed project is in `xboxrecomp\tools\ghidra_naming\work\`. The toolkit's `.gitignore` keeps it out of git.
- **XbSymbolDatabase:** run the CLI as `toolkit\xbsymbol\win_x64\bin\XbSymbolDatabaseCLI.exe <xbe> -e`.
- **REA: always use it** (the `mcp__rea__*` tools) for reverse-engineering questions: decompiling, reading a function's assembly, xrefs, callers/callees, strings, and checking a lifted function against the original. Prefer it over raw capstone dumps or grepping `analysis/disasm/asm`.
  - Use it on the reference games too when writing the joke modes: open `mw1/iw3sp.exe` (a plain PE) to learn how CoD4's weapons work (WeaponDef fields, spread, recoil, ADS, fastfiles). REA rejects the Skate 3 XEX (PowerPC): convert it with `tools/xex_to_pe.py` and query headless Ghidra with `tools/skate3_ghidra.py` (`docs/SKATE3_NOTES.md`). Follow the original's logic; record findings in docs, never copy its code.
  - Open `analysis/rea/default_flat.exe` with `open_binary`. That file is the XBE flat image wrapped in a PE32, because REA's Windows Ghidra provider accepts only PE. Its addresses match the XBE VAs and `functions.json` 1:1. Regenerate it with `.venv\Scripts\python.exe tools\xbe_to_pe.py`.
  - The provider is Ghidra 12.1.4. The first query waits for auto-analysis to finish, which takes a few minutes.
  - The session database is temporary. Names and comments set through REA are lost when it closes, so put lasting names in `analysis/names_combined.json` instead.
- **xemu:** the reference emulator, at `toolkit\xemu\xemu.exe` (gitignored). The `config\` folder holds the user's BIOS, MCPX and HDD image. Settings live in `%APPDATA%\xemu\xemu\xemu.toml`, which uses absolute paths, so update it whenever the folder moves.
- **Known environment issue:** this machine blocks executing freshly compiled exes from `%TEMP%` (`PermissionError: [WinError 5]`). Run the toolkit tests with TEMP redirected; then all pass (825 passed, 7 skipped):
  `cmd /c 'set TMP=C:\repos\sneak-king-recomp\xboxrecomp\build\conftmp&& set TEMP=C:\repos\sneak-king-recomp\xboxrecomp\build\conftmp&& "<vcvars64.bat>" >nul 2>&1 && cd /d C:\repos\sneak-king-recomp\xboxrecomp && <venv python> -m pytest tools/ -q'`

## Pipeline commands
Run these from `C:\repos\sneak-king-recomp\xboxrecomp`, with `$py` set to the venv Python, `$x` to `C:\repos\sneak-king-recomp\game\default.xbe` and `$a` to `C:\repos\sneak-king-recomp\analysis`.

```powershell
& $py -m tools.xiso unpack <iso> -o C:\repos\sneak-king-recomp\game
& $py -m tools.xbe_parser $x --json C:\repos\sneak-king-recomp\game\default_analysis.json
& $py -m tools.disasm $x -o $a\disasm --force --seed-functions C:\repos\sneak-king-recomp\seeds\p1_static.json --seed-functions C:\repos\sneak-king-recomp\seeds\p3_runtime.json --seed-functions C:\repos\sneak-king-recomp\seeds\p5_xpp.json   # ~13 min
& $py tools\ghidra_naming\merge_names.py --names-json $a\names_combined.json --functions-json $a\disasm\functions.json --apply   # after disasm, before recomp
& $py -m tools.func_id $x --functions $a\disasm\functions.json --strings $a\disasm\strings.json --xrefs $a\disasm\xrefs.json -o $a\func_id
& $py -m tools.abi_analysis $x --disasm-dir $a\disasm --func-id-dir $a\func_id --output-dir $a\abi
& $py -m tools.recomp $x --all --split 250 --disasm-dir $a\disasm --func-id-dir $a\func_id --abi-dir $a\abi --gen-dir C:\repos\sneak-king-recomp\src\recomp\gen --game-name "Sneak King" --exclude-manual C:\repos\sneak-king-recomp\src\recomp_manual.c --exclude-manual C:\repos\sneak-king-recomp\src\d3d9_hle.c --exclude-manual C:\repos\sneak-king-recomp\src\d3d_hle_shared.h --mmio-range 1EC5C0-20FB40 --mmio-range 23CB60-262FEC --mmio-range 2A65C0-2AFB10
```

`d3d_hle_shared.h` must be an `--exclude-manual` too: it holds the `extern void sub_X_gen(void);` declarations that tell the lifter which D3D bodies to emit as `sub_X_gen`. Without it the link fails with about 35 unresolved `sub_X_gen`. The three `--mmio-range` sections (D3D, DSOUND, XPP) reach hardware registers through explicit calls instead of page traps, which the web port needs (`docs/WEB_PORT_RESEARCH.md`). `-DRECOMP_COMPACT_MEM=ON` builds the web memory layout natively.

Build from the repo root inside the vcvars wrapper. Keep parallelism low, because the generated files are huge. One `src/recomp/gen/` serves every target. Pick the target with a `CMakePresets.json` preset (`docs/BUILDING.md` covers each one):
- `release`: the Windows game on D3D9, in `build\sneakking_recomp.exe`. This is the main build.
- `relwithdebinfo`: the same build, optimised, with a PDB, in `out\build\relwithdebinfo`. An unoptimised build of the generated code is too slow to play.
- `gl`: the web port's GL renderer and compact memory on Windows, in `build-gl\`, for debugging the web path natively.
- `web`: WebAssembly, in `build-web\`. It needs emsdk (`EMSDK`, default `C:\repos\emsdk`); `tools\web_build.cmd` wraps it.

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
- `RECOMP_USB` and `RECOMP_MKB`: the pad on the emulated OHCI bus, and keyboard and mouse as a pad on port 0 (raw input, toolkit `src/input/mkb_input.c`). The bindings are in `mkb.ini` at the repo root (`RECOMP_MKB_INI` moves it): WASD move, mouse camera, Shift run, left click or Space A, right click or Q B, E Y, wheel zoom, F first person, Escape or P START (pause), arrows and Enter for menus. The mouse is locked (hidden, kept inside the window) only while the King is in play, which is when the game reads the camera stick (`game_in_play`); menus, the pause menu, loading and the title screens free it. F9 releases it in play, a click takes it back. Closing the window ends the process. `RECOMP_MKB=0` falls back to the keyboard-only `RECOMP_KEYBOARD` pad.
- `RECOMP_HEAP_RECLAIM`: reuse freed contiguous memory. Without it the movies run the arena dry.
- `RECOMP_SCALE=N` (1 to 8, default 2): D3D9 render targets at N times the guest's 640x480, shown through a swap chain on the framebuffer window.
- `RECOMP_WIDESCREEN`: 16:9, anamorphic. The guest still draws 640x480: the camera FOV setter (`0x12CA70`) gets a hor+ FOV and the 2D ortho (`0x30FB70`, from `0x1223A0`) is squeezed by 3/4, so the HUD stays 4:3 and centred; the present stretches to 16:9. Movies stretch.

Keys: F1 opens the debug overlay (D3D9 stats, health, cheats: unlock levels, warp to a level), F2 shows a frame-rate line, F9 releases the mouse. The mouse turns the camera directly and points at menu options.

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
- `RECOMP_MOUSE_DEG=f`: camera degrees per mouse count (default 0.08). `RECOMP_MOUSE_TRACE=1` logs the mouse counts taken each game frame.
- `RECOMP_SHOOTER=<primary>,<sidearm>`, `RECOMP_SHOOTER_VOL`, `RECOMP_TPS_DIST`, `RECOMP_SHOOT_TRACE=1`, `RECOMP_SHOOT_DEBUG=1`: shooter mode (`docs/MODS.md`).
- `RECOMP_SKATE=1`, `RECOMP_SKATE_DEBUG=1`, `RECOMP_SKATE_BOARD=f`, `RECOMP_SKATE_POSE=...`, `RECOMP_SKATE_ORBIT=deg/s`, `RECOMP_SKATE_MISSIONS=1`, `RECOMP_SKATE_STARTLE=r`: skate mode (`docs/MODS.md`).
- `RECOMP_WARP=<slot>`: warp to level slot (0 sawmill, 1 suburbia, 2 construction, 3 urban) two seconds into play, for testing.
- `RECOMP_HD_TEX=<dir>`: replace textures with `<dir>/<hash>.dds` (360 HD textures from `tools/xen_tex.py <pkg.xbp> <pkg.xen> -o game/hd`). On by default with `game/hd`; `=0` keeps the Xbox textures.

The toolkit in `xboxrecomp/` carries local patches that are not upstream: the cpuid/pushfd lifter fix (P2), plus the contiguous-arena top-down fallback, `nv2a_pb_set_semaphore_device_ptr` and the APU EP heartbeat (P3), plus the P4 fixes: read-only D:, guest APCs (`NtQueueApcThread`, queued I/O completion), the QPC vblank tick and IRQ hold, and the fixed-function transform of inline arrays with integer depth, plus the P5 fixes: the per-kick `DMA_PUT` trace gated on `RECOMP_NV2A_TRACE`, and for render back ends, `clip_x`, `clip_y` and `format` in `Nv2aSurface`, `palette` in `Nv2aTexture`, and `flip()` called before the window copies the frame, plus the P5.4 fix: `Nv2aBatch.rc`, which passes the register-combiner program to render back ends, plus the P6 fix: `Nv2aBatch.stages` and `fog_color` and `Nv2aVertex` `specular`, `fog` and `tc`, which pass every texture stage the combiners read, plus `Nv2aRenderState.zeta_format` and `Nv2aBatch.clip_plane_mode`, per-vertex transform failure, and one shared worker pool for transform and raster, plus the P7 fixes: `mkb_input.c` bindings from the ini (`bind.<PAD>=...`), held mouse buttons, generic Shift/Ctrl, and poll-rate-independent mouse aim, and `xbox_FramebufferWindowHwnd` / `xbox_FramebufferWindowExternalPresent` so a back end can present into the framebuffer window, plus the D3D9 fix: `tools.recomp --exclude-manual` can be given more than once, plus the P8 additions: `mkb_take_mouse_delta`, `mkb_set_suspended` and `mkb_set_menu_cursor` in `mkb_input.c`, and `xbox_FramebufferWindowSetMsgHook` in `fb_present.c`, plus the P9 additions: `mkb_mouse_buttons` and `mkb_set_mute` in `mkb_input.c`.

## Web build (experimental)
The same game in Chrome as WebAssembly: `docs/WEB_PORT_RESEARCH.md` (research, decisions, results) and the W phases in `docs/ROADMAP.md`. Getting started: `docs/BUILDING.md`.
- Emscripten SDK at `C:\repos\emsdk`. `tools\web_build.cmd` (the `web` preset) builds `build-web\sneakking_recomp.{js,wasm}`; `.venv\Scripts\python.exe tools\serve.py` serves `web/index.html`, the build and `game/` on http://localhost:8080/ (COOP/COEP, range requests, generated `game/manifest.txt`). Local only: the build contains lifted game code, so it is never hosted.
- `src/main_web.c` (boot, WasmFS fetch mount), `src/web_host.c` (`mkb.ini`'s keyboard and mouse bindings, menu pointer, pointer lock in play), `src/gl_hle.c` (the GL ES 3 / WebGL2 renderer; natively `-DRECOMP_RENDERER=gl`), `src/d3d_hle_shared.h` (guest side shared with `d3d9_hle.c`). Toolkit: `apu_web.c`, compact memory (`XBOX_COMPACT_MEM`, `xbox_GuestPtr`), explicit MMIO (`xbox_MmioRead/Write`).
- The page sets `RECOMP_WIDESCREEN` (on unless `?RECOMP_WIDESCREEN=0`) and sizes both canvases to 16:9 or 4:3 before the game takes `#canvas`. The log panel can be hidden (remembered in `localStorage`).
- Cheats: the page's buttons call `web_cheat()` (`web_host.c`). It runs unlock and warp on the game thread at the next present (`gl_hle.c`), as the F1 overlay does natively. The `gl` renderer has no overlay. Shooter and skate mode are D3D9/XAudio2 only. On `gl` the build links `src/mods_stub.c` instead of `mods_mesh.c`, `skate.c` and `cod4_sound.c`, and both modes report themselves unavailable. `RECOMP_MODS` is defined only for the d3d9 renderer.
- `node tools/web_drive.mjs <url> <secs> <outdir> [Key@sec|PAUSE@sec ...]` drives Chrome headlessly enough for tests: console, screenshots, key presses, every thread's stack.

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
- **Upstream fixes:** keep toolkit fixes small, and send them upstream to xboxrecomp when they are generic. Commit toolkit changes apart from game changes: `git subtree split --prefix=xboxrecomp -b <branch>` then gives a branch to send upstream, and `git subtree pull --prefix=xboxrecomp https://github.com/sp00nznet/xboxrecomp main` takes in new upstream releases.

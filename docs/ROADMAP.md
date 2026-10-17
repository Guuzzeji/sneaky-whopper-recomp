# Sneak King Recomp — Roadmap

Goal: turn the original Xbox `default.xbe` of *Burger King: Sneak King* (2006) into a native PC executable with no emulator, using [xboxrecomp](https://github.com/sp00nznet/xboxrecomp).

Rules for every phase:
- Each phase has an **exit criterion**. Tick a box only after its result is verified.
- Background and the reasoning behind these phases: [RESEARCH.md](RESEARCH.md).

---

## P0 — Setup and lay of the land ✅

- [x] Make the repo a git repository. Add a `.gitignore` that excludes all game data, analysis dumps and generated C.
- [x] Install Visual Studio 2022 Community with the C++ workload. It includes MSVC, the Windows SDK, CMake and Ninja.
- [x] Create a Python `.venv` with capstone, pytest, pefile and numpy. The system Python is uv-managed, so pip cannot install into it.
- [x] Clone xboxrecomp to `C:\repos\xboxrecomp` (v0.14.0, `eb0aa1d`).
- [x] Run the toolkit test suite. The only failure is the corpus harness, an environment issue described in RESEARCH.md.
- [x] Build the toolkit runtime libraries in Release.
- [x] Extract the game partition to `game/` (143 files, 1.55 GB).
- [x] Run the analysis tools: `xbe_parser`, `disasm`, `func_id` and `abi_analysis`. Output is in `analysis/`.
- [x] Copy `templates/new-game` into the repo and fill in the entry point, paths and name.
- [x] Write RESEARCH.md, ROADMAP.md and CLAUDE.md.
- [x] Ghidra 12.1.4 is already installed. Install xemu as the reference emulator; it needs the user's own BIOS/MCPX dumps.

**Exit:** tools installed, toolkit tests green, game extracted, analysis stats recorded.

## P1 — Analysis

- [ ] Review function discovery coverage.
  - 13,318 functions found.
  - Spot-check gaps in `.text`.
  - Decide whether `DOLBY` holds code; it has 0 functions and is not marked executable. Check its xrefs.
- [ ] Load `default.xbe` into Ghidra.
  - Try ghidra-xbe or XbSymbolDatabase to name the XDK functions (D3D, DSound, XAPI, XMV).
  - Feed the names back with `tools/ghidra_naming`.
- [ ] Inventory the indirect control flow:
  - jump tables
  - vtables (2,345 thiscall functions)
  - the 698 function addresses found in data tables
- [ ] **Choose the graphics path.**
  - Option A: D3D8 HLE.
  - Option B: the NV2A pushbuffer executor.
  - Check whether the game calls `D3DDevice_*` through the linked D3D section, and whether it writes pushbuffers directly.
- [ ] Map the threads: find where the game calls `PsCreateSystemThreadEx`.
- [ ] Study [blinx2-recomp](https://github.com/apexJCL/blinx2-recomp), the most complete OG Xbox recomp. Note which patterns to copy.

**Exit:** a function map, a list of indirect flow, a recorded graphics-path decision and a thread map.

## P2 — First lift and link

- [ ] Run `tools.recomp --all --split 250 --gen-dir src/recomp/gen`.
- [ ] Fix every unhandled-instruction comment (`/* mnemonic */`) in the generated code. Fix them in the lifter config or upstream, never in `gen/`.
- [ ] Add the overrides MSVC needs: the negative-index jump tables in the CRT `memcpy`/`memmove`.
- [ ] Get a clean compile and link of `sneakking_recomp.exe`. Build with `--parallel 2`, because the generated files are huge.

**Exit:** the executable builds and links with no unresolved externals.

## P3 — Boot

- [ ] Get through the CRT/XAPI init, TLS, heap and stack.
- [ ] Make `D:\` file I/O work against `game/`.
- [ ] Run the indirect-call feedback loop:
  1. Build with `RECOMP_ICALL_FEEDBACK`.
  2. Merge the observed targets into `icall_seeds.json`.
  3. Run `disasm --seed-functions`.
  4. Lift again.
- [ ] Fix boundaries using `--coalesce-functions`, `--trace-functions` and `recomp_manual.c` overrides.
- [ ] Bring up the threads and sync objects without deadlocks.

**Exit:** the game reaches its main loop and the first `Present` or pushbuffer kick happens.

## P4 — First pixels

- [ ] Get the boot logos and splash screen on screen.
- [ ] Render the main menu correctly: textures, swizzle, blending and register combiners.
- [ ] Compare frames with xemu. Use RenderDoc to inspect the host D3D11 output.

**Exit:** the main menu renders and matches xemu visually.

## P5 — Input, audio, video

- [ ] Input: XInput controller plus keyboard and mouse. Menu navigation works.
- [ ] Audio:
  - [ ] DirectSound/APU output.
  - [ ] WMA music through the WMADEC section.
  - [ ] Check that the Dolby section isn't needed, or is handled.
- [ ] FMV: XMV movies play, through `tools/xmv` and Media Foundation.

**Exit:** the menu is fully navigable with sound and the intro movies play.

## P6 — Gameplay

- [ ] Levels load and the Sneak King missions run.
- [ ] Saves work. `U:` maps to the save dir. Stub signature checks if needed.
- [ ] Threading and timing are stable, with frame pacing at 60 Hz.
- [ ] Add golden-frame regression tests in the BLiNX 2 style: replay inputs, then compare frames.

**Exit:** one full level is playable from start to finish.

## P7 — Fun playable and polish

- [ ] A full playthrough completes with no crashes.
- [ ] Performance target is a locked 60 fps. Profile the hot generated functions.
- [ ] Quality of life:
  - [ ] Windowed and fullscreen.
  - [ ] Resolution scaling.
  - [ ] Controller remapping.
- [ ] Optional: the Linux/macOS build through xboxrecomp's OpenGL 3.3 + SDL2 path.
- [ ] Distribution model: ship code only. The player supplies their own disc dump, and game data is never bundled.

**Exit:** the game can be completed natively and is fun to play.

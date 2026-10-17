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
- [x] Run the toolkit test suite: 805 passed, 7 skipped. It needs TEMP redirected, as described in CLAUDE.md.
- [x] Build the toolkit runtime libraries in Release.
- [x] Extract the game partition to `game/` (143 files, 1.55 GB).
- [x] Run the analysis tools: `xbe_parser`, `disasm`, `func_id` and `abi_analysis`. Output is in `analysis/`.
- [x] Copy `templates/new-game` into the repo and fill in the entry point, paths and name.
- [x] Write RESEARCH.md, ROADMAP.md and CLAUDE.md.
- [x] Ghidra 12.1.4 is already installed. Install xemu as the reference emulator; it needs the user's own BIOS/MCPX dumps.

**Exit:** tools installed, toolkit tests green, game extracted, analysis stats recorded.

## P1 — Analysis ✅

- [x] Review function discovery coverage.
  - 13,318 functions found.
  - Spot-check gaps in `.text`.
  - Decide whether `DOLBY` holds code; it has 0 functions and is not marked executable. Check its xrefs.
- [x] Load `default.xbe` into Ghidra.
  - Try ghidra-xbe or XbSymbolDatabase to name the XDK functions (D3D, DSound, XAPI, XMV).
  - Feed the names back with `tools/ghidra_naming`.
- [x] Inventory the indirect control flow:
  - jump tables
  - vtables (2,345 thiscall functions)
  - the 698 function addresses found in data tables
- [x] **Choose the graphics path.**
  - Option A: D3D8 HLE.
  - Option B: the NV2A pushbuffer executor.
  - Check whether the game calls `D3DDevice_*` through the linked D3D section, and whether it writes pushbuffers directly.
- [x] Map the threads: find where the game calls `PsCreateSystemThreadEx`.
- [x] Study [blinx2-recomp](https://github.com/apexJCL/blinx2-recomp), the most complete OG Xbox recomp. Note which patterns to copy.

**Exit:** a function map, a list of indirect flow, a recorded graphics-path decision and a thread map.

**Result:** see [P1_FINDINGS.md](P1_FINDINGS.md). Graphics path: **B (NV2A pushbuffer executor)**, overriding only the hardware-wait entry points. DOLBY is APU DSP microcode, not x86. 690 XDK/CRT names applied; 288 seeds in `seeds/p1_static.json`.

## P2 — First lift and link ✅

- [x] Re-run `tools.disasm --seed-functions seeds/p1_static.json`, then `func_id`/`abi_analysis`, and re-apply `analysis/names_combined.json` via `merge_names.py --names-json ... --apply`.
- [x] Run `tools.recomp --all --split 250 --gen-dir src/recomp/gen --exclude-manual src/recomp_manual.c`.
- [x] Fix every unhandled instruction (`RECOMP_UNIMPL`) in **real** code. Fix them in the lifter config or upstream, never in `gen/`.
- [x] ~~Add the overrides MSVC needs: the negative-index jump tables in the CRT `memcpy`/`memmove`.~~ Not needed: toolkit v0.13.1+ lifts them as switches (checked in `crt_memmove_001B2450`).
- [x] Get a clean compile and link of `sneakking_recomp.exe`. Build with `--parallel 2`, because the generated files are huge.

**Exit:** the executable builds and links with no unresolved externals.

**Result (2026-10-16):** 13,650 functions detected, 13,715 lifted (1.52M lines of C in 55 files, 0 failed, 26 unresolved call targets stubbed), and `build/sneakking_recomp.exe` (30.7 MB) links cleanly. What changed:

- **P1 seeds and names were polluted.** `names_combined.json` held 113 Ghidra *data* labels (`crt_s_*` strings, `crt_u_*` unicode), and 49 "data-table pointer" seeds pointed into data tables. Seeding them lifted data as code: 3,353 unimplemented instructions and 106 stubs. I removed them (880 → 767 names, 288 → 221 seeds); the old map is in `analysis/names_combined.p1.json`. REA confirmed the targets are data.
- **Upstream lifter patch** (`xboxrecomp/tools/recomp/lifter.py`, test `test_lifter_cpu_probe.py`): `cpuid` returns the Xbox CPU (family 6 model 8, SSE, no SSE2); `pushfd`/`popfd` keep the stack balanced; `cli`/`sti`/`wbinvd`/`invd` are no-ops. It still needs to be sent upstream.
- **Override** `sub_00064262`: XAPI's startup kernel patch, which writes kernel code at `0x8003xxxx` and installs a GDT descriptor (`0x641FE` → `0x6A1B8`). Stubbed to a bare `ret`.
- **The remaining 1,441 `RECOMP_UNIMPL` are all data decoded as code**, never reached by real control flow: inline jump tables inside functions (memmove `0x1B26CC`, `0x13C7C5`, `0xBA4A8`, `0x1A7051`) and detector false positives over XGRPH's vertex-shader assembler tables (`0x2349xx–0x2377xx`), XMV codec tables, and D3D/DSOUND constant blocks. They compile to logged no-ops. If one is ever reached at runtime, `RECOMP_UNIMPL_TRAP=1` stops on it. The toolkit has no "data range" input to prune these; that would be an upstream feature.

## P3 — Boot ✅

- [x] Get through the CRT/XAPI init, TLS, heap and stack.
- [x] Make `D:\` file I/O work against `game/`. Packages load through the APC loader thread (P4, below).
- [ ] Run the indirect-call feedback loop:
  1. Build with `RECOMP_ICALL_FEEDBACK`.
  2. Merge the observed targets into `icall_seeds.json`.
  3. Run `disasm --seed-functions`.
  4. Lift again.
  So far the two unresolved targets were seeded by hand (`seeds/p3_runtime.json`); no ICALL failures remain on the boot path.
- [ ] Fix boundaries using `--coalesce-functions`, `--trace-functions` and `recomp_manual.c` overrides. None needed yet.
- [x] Bring up the threads and sync objects without deadlocks: XAPI main, an APC worker (`0x71260`) and a job worker (`0x1286E0`).

**Exit:** the game reaches its main loop and the first `Present` or pushbuffer kick happens.

**Result (2026-10-16):** the game boots to its main loop and presents at a steady 30 fps (`D3DDevice_Swap`), with DirectSound and the emulated APU running, in a 640×480 framebuffer window. The frames are clears only, so nothing is drawn yet. Run it as:

```
set RECOMP_AC97_READY=1 & set RECOMP_PB_EXEC=1 & set RECOMP_FB_WINDOW=1 & build\sneakking_recomp.exe
```

What it took:

- **50 MB `XPhysicalAlloc` at startup** (BlitzTech's heap, `0x6D10F`). The toolkit's contiguous arena fills bottom-up and holds a 1 MB reserve at 15–16 MB, so no run fit. Upstream: first-fit now falls back to top-down placement, as the console kernel allocates (`xbox_ContiguousAllocEx`).
- **Two code entries the sweep missed**: `0x1BB49E` (follows a noreturn call) and `0x2A7085` in XPP (follows `ret 8`). Seeded in `seeds/p3_runtime.json`.
- **DirectSound init failed without the AC'97 codec**, and the game tore itself down. `RECOMP_AC97_READY=1` brings up the emulated APU.
- **D3D fence wait** (`D3D_BlockOnTime`, `0x1EECB0`) spun on `*(device+0x30)`, which nothing wrote. Upstream: `nv2a_pb_set_semaphore_device_ptr()` names the device-pointer global, and the executor resolves the semaphore at the first `0x1D70` release. `main.c` registers `0x0020D328`.
- **DSP reset every DoWork**: DSOUND checks the EP heartbeat word `0xFE85A018` for `0xCCCCCC`, which the Dolby microcode writes on hardware. Without it, DoWork reset both DSPs with a 10 ms stall on every call, taking most of each frame. Upstream: the APU frame clock writes the heartbeat.
- **Diagnostics added**: `RECOMP_SAMPLE_SECS=N` samples the main thread's host stack (`main.c`), and `tools/resolve_rva.py` names the RVAs from `build/sneakking_recomp.map`.

**Next (start of P4):** the boot mode (`CMode_GameBoot`, world `packages/w_GameBoot`) checks `w_gameboot_fetm.xbp` (open, size, close) and then idles. Solved in P4: the package read was an APC that the toolkit dropped.

## P4 — First pixels

- [x] Get the boot logos and splash screen on screen: the copyright splash, the King Games and Blitz idents, the intro movie and "Press START".
- [ ] Render the main menu correctly: textures, swizzle, blending and register combiners.
- [ ] Compare frames with xemu. Use RenderDoc to inspect the host D3D11 output.

**Exit:** the main menu renders and matches xemu visually.

**Status:** paused after the boot sequence. The main menu sits behind "Press START", so it needs input from P5. The two unticked items above, and the open items below, move to **P5.4**.

**Progress (2026-10-16):** the boot loads 14 packages, draws the copyright splash, plays the idents and the intro movie, and reaches "Press START". The toolkit fixes are on branch `sneak-king-p3-boot`:

- **D: is read-only.** At startup (`0x77070`) the game creates `d:\_T_E_S_T.___`. If that succeeds, it believes it runs from a writable dev drive and turns off its DVD file cache. `bridge_create_file_open` now refuses write access and create dispositions on `\Device\CdRom0` (`xbox_path_is_dvd`).
- **APCs.** BlitzTech gives all package I/O to its loader thread (`0x71260`), which only waits alertably and receives work through `QueueUserAPC` (`0x776C0` queues `0x76730`). `NtQueueApcThread` was a stub that dropped the APC, so no package was ever read. Now it queues a host APC that runs the guest routine at the thread's next alertable wait. I/O completion APCs (`ReadFileEx`) are queued the same way and no longer run inline. Inline delivery ran the completion (`0x74080`) before the issuer set its in-flight flag (`0x2F2FB8`), which stalled the second package. `RECOMP_IO_APC_INLINE=1` restores the old behaviour.
- **Inline-array batches go through the fixed-function transform.** The splash quads arrive as `INLINE_ARRAY` in object space (±270, ±202). The executor drew them as screen space, which filled the top-left corner with a magnified texture.

- **XMV timing is driven by vblank.** The player (`GetNextFrame`, `0x27F0C2`) times video by D3D's vblank count (`D3DDevice_GetDisplayFieldStatus`) and syncs it to the audio stream callback (`0x27EA78`). The count never moved, and the movie stopped after two packets. Three problems:
  - Vblank was off. `main.c` now turns `RECOMP_VBLANK` on by default; `RECOMP_VBLANK=0` turns it off.
  - The vblank tick ran at about 32 Hz on the 15.6 ms system timer. It is now on a `QueryPerformanceCounter` schedule and drains DPCs after each vblank.
  - About a third of the vblanks went uncounted: the ack thread cleared `PMC_INTR_0` between D3D's ISR (`0x1F9670`) and its DPC (`0x1F9870`). A 0.5 ms hold (`g_nv2a_irq_hold_until`) now keeps the bits set for the DPC.
- **Depth precision in the fixed-function path.** The movie quad sits at the far plane: z is 16777214.67 against a Z24 clear value of 16777215, with `LESS`. Float rounding (in an 8e12 clip z, in the barycentric sum, and in the final cast) flipped pixels to failing, which drew a crosshatch over every movie. Now the transform runs in double, z is truncated to the integer the depth buffer holds, and interpolation uses the delta form.

`RECOMP_FRAME_TRACE` now also prints the raw position, UV and transformed position of the first four vertices of each fixed-function batch. `main.c` adds `RECOMP_SAMPLE_DELAY=<s>`, which starts the sampler later.

**Open:**
- The intro movie plays at about half speed, so the menu arrives after about 2 minutes. In the profile, 44% is GPU fence waits on the software PB executor, and 32% is the lifted MMX YUV→RGB copy (`sub_00280085`).
- The Press START background video reads one packet and stalls. The wrapper's `GetNextFrame` thunk `0x12BA50` is never called.
- A faint box shows around the loading icon.
- Reaching the main menu needs input (P5).

These are tracked in P5.4.

## P5 — Input, audio, video

- [ ] Input: XInput controller plus keyboard and mouse. Menu navigation works.
- [ ] Audio:
  - [ ] DirectSound/APU output.
  - [ ] WMA music through the WMADEC section.
  - [ ] Check that the Dolby section isn't needed, or is handled.
- [ ] FMV: XMV movies play, through `tools/xmv` and Media Foundation.

**Exit:** the menu is fully navigable with sound and the intro movies play.

## P5.4 — Finish P4: full rendering

Starts once P5 is complete, because input is what gets the game past "Press START" and into the menus. It finishes the rest of P4 so that rendering is complete.

- [ ] Render the main menu correctly: textures, swizzle, blending and register combiners.
- [ ] Compare frames with xemu. Use RenderDoc to inspect the host D3D11 output.
- [ ] Get the Press START background video playing. It reads one packet and stalls; the wrapper's `GetNextFrame` thunk `0x12BA50` is never called.
- [ ] Get movies to full speed. The intro plays at about 0.5×: GPU fence waits on the software PB executor take 44%, and the lifted MMX YUV→RGB copy (`sub_00280085`) takes 32%.
- [ ] Remove the faint box around the loading icon.

**Exit:** every screen from boot through the main menu and its submenus renders and matches xemu visually, and movies play at full speed.

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

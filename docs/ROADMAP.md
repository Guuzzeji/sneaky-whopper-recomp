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
- [x] Clone xboxrecomp (v0.14.0, `eb0aa1d`). Since vendored as `xboxrecomp/`.
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

- [x] Input: an Xbox controller on the emulated USB bus, fed by XInput, plus the keyboard as a pad on port 0. Menu navigation works.
- [ ] Audio:
  - [x] DirectSound/APU output.
  - [ ] WMA music through the WMADEC section. WMADEC serves the XDK's custom-soundtrack path (`TDATA\FFFE0000\MUSIC\*.WMA`); the game's own audio is in the `.xbp` packages and the movies. Compare the menus with xemu to see whether any music is missing.
  - [x] Check that the Dolby section isn't needed, or is handled. Handled: the toolkit stubs the EP DSP and writes its heartbeat (P3).
- [x] FMV: XMV movies play at full speed, with sound. They play through the game's own lifted XMV decoder, not through `tools/xmv` and Media Foundation.

**Exit:** the menu is fully navigable with sound and the intro movies play.

**Result (2026-10-17):** START skips the movies. The title screen, main menu, Help & Options and Audio Options all navigate with the pad, and UI sounds play on every press. Movies run at 29.97 fps with continuous audio. Rendering is on the host GPU (D3D11). What it took:

- **The pad was never enumerated.** The toolkit's OHCI model starts only when the title calls `xbox_OhciInit()`, and `main.c` never did. `main.c` now calls it, with `RECOMP_USB` and `RECOMP_KEYBOARD` on by default (`=0` turns either off).
- **Enumeration stopped after SET_CONFIGURATION.** The USB completion callback `0x2ABB39` was never lifted: it follows a `leave; ret 8` with no padding, like `0x2A7085` in P3. An ICALL failure in the log named it. A scan for uncovered code right after a `ret` found 25 such entries in XPP, all real code; they are now seeded in `seeds/p5_xpp.json`. The scan also found ~500 candidates in other sections, but those include data tables, so the ICALL feedback loop should handle them instead.
- **Audio underran about 100 ms every second.** The APU frame clock waits one EP period (5.3 ms) at a time, and at the default 15.6 ms Windows timer tick each wait overslept threefold. `main.c` now calls `timeBeginPeriod(1)`. The APU then runs at exactly 48 kHz with no underruns.
- **The Press START and menu videos stalled after one packet.** BlitzTech's heap takes 50 of the 64 MB contiguous arena, and every movie allocates two 1.2 MB surfaces and frees them at the end. Without `RECOMP_HEAP_RECLAIM` the toolkit never reuses freed contiguous memory, so by the fourth movie the decoder's allocation was refused (`[CONTIG] arena exhausted`). `main.c` now turns reclaim on by default. The old lead (the never-called thunk `0x12BA50`) was a dead end: nothing references it. Every movie plays through the blocking player `0x1394A0`, which calls XMV `GetNextFrame` (`0x27F0C2`) directly.
- **Movies ran at half speed: 15 fps for 29.97 fps content.** That also explains the audio gaps: XMV feeds audio a packet at a time, and with the video at half speed half the audio was missing. (The XMV header's "duration" field is twice the real length; ffprobe's 29.97 fps and the audio length agree.) Two causes, both in the software GPU, which ran on one thread:
  - Under `RECOMP_PB_EXEC` the toolkit printed a `[NV2A] DMA_PUT` line on every kickoff, which took 42% of the executor thread. Upstream: those lines now need `RECOMP_NV2A_TRACE`.
  - The software rasteriser was the remaining limit. `src/d3d11_backend.c` now registers a D3D11 render back end: the executor still decodes and transforms, and the host GPU fills the triangles. With it, movies hold 30 flips/s and the executor thread mostly idles. `RECOMP_D3D11=0` brings back the software rasteriser.
- **Diagnostics:** `RECOMP_SAMPLE_ALL=1` samples every thread and prints per-thread CPU time. The sampler now includes the running function (RIP). `RECOMP_D3D11_STATS=1` prints flips/s.

## P5.4 — Finish P4: full rendering

Starts once P5 is complete, because input is what gets the game past "Press START" and into the menus. It finishes the rest of P4 so that rendering is complete.

- [ ] Render the main menu correctly: textures, swizzle, blending and register combiners. The menus now draw through the D3D11 back end, which applies blend, alpha test, depth and stencil. Texture stages 1–3, specular and fog do not reach a back end yet (`nv2a_backend.h`).
  - [x] The menus were at half brightness. Every menu batch has diffuse 0x80 and a combiner that scales by 2 (`COLOR_OCW` 0x00010C00, op SHIFTLEFTBY1, which is D3D's MODULATE2X). The back end drew texel × diffuse and dropped the ×2. Now `Nv2aBatch.rc` carries the combiner program (toolkit), and the D3D11 pixel shader evaluates it with an HLSL port of `nv2a_rc_eval`. The main menu and Help & Options render at full brightness.
  - [x] ~~The software rasteriser (`RECOMP_D3D11=0`) still ignores stencil.~~ Moot: the software rasteriser is gone (P7.5). After the menu, the game clears stencil to 0x80 and draws two black quads at alpha 0.25 with stencil NOTEQUAL 0x80, which hardware rejects everywhere. The software path draws both quads and darkens the frame.
- [ ] Compare frames with xemu. Use RenderDoc to inspect the host D3D11 output; the back end's device is offscreen, so capture it with RenderDoc's process injection.
- [x] Get the Press START background video playing. Fixed in P5 by contiguous-memory reclaim.
- [x] Get movies to full speed. Fixed in P5 by the trace gate and the D3D11 back end. On the main thread, the lifted MMX YUV→RGB copy (`sub_00280085`) is now the largest cost.
- [ ] Remove the faint box around the loading icon.
- [x] ~~D3D11 back end follow-ups.~~ Superseded: the D3D11 back end is gone (P7.5).

**Exit:** every screen from boot through the main menu and its submenus renders and matches xemu visually, and movies play at full speed.

## P6 — Gameplay

- [ ] Levels load and the Sneak King missions run. The first level (the saw mill) loads, the King walks, and a challenge can be accepted: its timer and delivery counter run. A delivery and a completed mission are not tested yet.
- [x] Saves work. New Game writes a save ("Sneak King game saved"), and after a restart Continue Game loads it ("Load successful"). No signature stub was needed.
- [ ] Threading and timing are stable, with frame pacing at 60 Hz. The first level holds a steady 30 flips/s, the same cap as with the combiners off, so the title probably locks to 30. Confirm against xemu before chasing 60.
- [ ] Add golden-frame regression tests in the BLiNX 2 style: replay inputs, then compare frames.

**Exit:** one full level is playable from start to finish.

**Progress (2026-10-17):**

- **The world drew black on the D3D11 back end.** Most level batches use stage program `0x21`: r0 = t0 × v0, then r0 ×= t1, where t1 is a P8 256×256 lightmap. The back-end interface passed stage 0 only, so t1 read 0. The software rasteriser drew the level correctly. Toolkit: `Nv2aBatch` now carries every enabled stage (`stages[4]`) and the fog colour. `Nv2aVertex` carries the specular colour, the fog factor and each stage's texture coordinate (`tc[4][4]`, normalised for 2D lookups on linear textures). The D3D11 pixel shader samples all four stages by their stage mode and feeds v1, fog and t0–t3 to the combiners. The toolkit test `tests/nv2a_ffp_backend` checks the new fields.
- **11 fps in the first level, now 30.** `RECOMP_D3D11_STATS=1` now also prints where the executor thread's time goes: draws, texture lookups and decodes, readbacks, render-target uploads, vertex and constant uploads. Two causes:
  - **Vertex uploads:** a `WRITE_DISCARD` map on every draw (about 3,600 a second) took about 500 ms of every second. Vertices now go into an 8 MB ring that maps with `NO_OVERWRITE` and discards only when it wraps.
  - **The combiner shader:** evaluated from a constant buffer, every register read was an indexed temp array. The GPU took about 17 ms a frame, which showed up as stalls in the flip readback. Each combiner program now compiles its own pixel shader, with the program words as compile-time constants (`PsKey`, `ps_get`). The factor colours stay in the constant buffer, because titles change them per draw.
- **Dead end, recorded:** XDK D3D's `KickOff` spins on the PFB flush bit (`0x100410` bit 16) after every kick. The ack thread clears it only between pushbuffer commands, so the main thread waits for the executor. Clearing the bit from a dedicated thread made the main thread run ahead. D3D then saw the GPU over 9 KB behind and took `BlockOnTime`'s other path (`0x1EECB0`). That path patches a `NOP 5` into the submitted pushbuffer and waits for the latch at PGRAPH `0x400B10` (`0x1EE910`), and the game hung entering the level. Supporting that path needs a `DMA_GET` that tracks the walk and the NOP interrupt. Until then the slow ack is what keeps the CPU and GPU in step.
- **D3D11 review fixes:** a texture an earlier stage of the same draw was handed could be evicted and freed before binding (the LRU now ticks per lookup). Depth is scaled by the zeta format instead of a guess from `SET_CLIP_MAX`, and clamped at the far plane so distant geometry is no longer clipped away. One vertex that fails to transform drops only its own triangles, not the whole batch. CLIPPLANE stages discard, and bump/dependent stages sample unbumped instead of reading zero. Fixed-function vertices get a fog coordinate. Screen-space batches pass stages 1-3. Textures are hashed once a frame, and the pixel-shader cache is a 1,024-entry LRU whose file holds each program once.
- **Open:** the King is not drawn in the first level, and the user still sees NPCs stretch for about a second and parts of the map vanish. The executor, not the D3D11 back end, is the suspect: the software rasteriser shows the same missing King. Vertex-program batches are mostly not behind the eye, so the vertex programs look fine.
- **Sampler:** when RIP is outside the exe, `RECOMP_SAMPLE_*` now names its module and offset (`[ntdll.dll+161444]`). Before, stale stack words took the blame.

## P7 — Fun playable and polish

- [ ] A full playthrough completes with no crashes.
- [ ] Performance target is a locked 60 fps. Profile the hot generated functions.
- [ ] Quality of life:
  - [ ] Windowed and fullscreen.
  - [x] Resolution scaling. `RECOMP_SCALE=N` (default 2) renders every target at N times the guest surface and presents it through a swap chain on the resizable window (D3D9 since P7.5).
  - [x] Keyboard and mouse for gameplay and menus: the toolkit's raw-input layer (`mkb_input.c`), bound for this game in `mkb.ini`.
  - [x] ~~On-screen D3D11 debugger (F1-F4).~~ Removed with the D3D11 back end (P7.5).
  - [x] P8: native mouse camera, debug overlay, cheats, mouse menus, 360 HD textures (below).
  - [ ] Controller remapping.
- [ ] Optional: the Linux/macOS build through xboxrecomp's OpenGL 3.3 + SDL2 path.
- [ ] Distribution model: ship code only. The player supplies their own disc dump, and game data is never bundled.

**Exit:** the game can be completed natively and is fun to play.

## P7.5 — D3D8 on Direct3D 9 (a baseline for render bugs)

The game's own D3D8 calls now go to host Direct3D 9, one to one, instead of through the NV2A pushbuffer: `src/d3d9_hle.c`. D3D9 is the only renderer. The executor, the software rasteriser and the D3D11 back end no longer run (`docs/P1_FINDINGS.md` §3).

- [x] H0: wrap every D3D entry point the game uses (`sub_X` → `hle_*`, lifted body as `sub_X_gen`) with a call tracer (`RECOMP_D3D_TRACE`). One relift; behaviour changes after that touch only `d3d9_hle.c`. Toolkit: `--exclude-manual` can be repeated.
- [x] H1: device, Clear and present. One host target per guest surface, scaled by `RECOMP_SCALE`, presented through a swap chain on the framebuffer window, paced to 60 Hz like the Xbox flip.
- [x] H2: fixed function. Render and texture-stage state come from D3D's guest arrays (`D3D_g_RenderState`, `D3D_g_DeferredTextureState`), translated from the NV2A's GL enums. Textures upload in their own D3D9 formats (unswizzle only, DXT as is, P8 expanded). Menus and the loading screens match the D3D11 frames.
- [x] H3: 3D fixed function. Transforms, lights, materials, stream sources, `DrawVertices`/`DrawIndexed*`. The saw mill renders.
- [x] H4: vertex programs. NV2A microcode becomes `vs_2_0` HLSL, decoded as `nv2a_vsh_interp.c` does it; D3D's viewport epilogue is undone, its fog coordinate goes through the table mode in the shader. The King is skinned, lit and textured; the level frame differs from the D3D11 one by 3.3/255 on average.
- [ ] H4b: pixel shaders (register combiners → `ps_2_0`/`ps_3_0`). The title creates two; none was bound in the menus or the first level.
- [x] H5: movies. The XMV player decodes into a texture the game draws; the movie quad sits on the far plane (depth rounding, see `apply_fixed_function`). CPU writes to a render target (a movie into the back buffer) are found by hashing guest memory and uploaded.
- [x] H6: retire the rest. `d3d11_backend.c` deleted; `RECOMP_PB_EXEC` off; `xbox_Nv2aMirrorFence` keeps D3D's semaphore at its time stamp, and the toolkit's NV2A model acknowledges the pushbuffer without executing it.
- [ ] Follow-ups: GPU results are not copied back to guest memory (a title reading a render target with the CPU sees stale pixels); `ZBIAS` scale is a guess; no eviction past 32 targets or 2,048 textures; lost-device handling.

## P8 — Quality of life

- [x] Mouse camera, PC style. `InputManager::GetAnalog` (`0x5F480`) is wrapped: for the camera controls (2, 3) it returns the raw mouse counts of the current game frame scaled by the frame time, so the view turns a fixed angle per count, with no deadzone and no full-throw cap. The game's stick orbits the camera round the King, which is what felt inverted; both axes are negated so mouse right looks right and mouse up looks up. The game's Invert Camera and sensitivity options still apply. `RECOMP_MOUSE_DEG` (default 0.08) is degrees per count. Toolkit: `mkb_take_mouse_delta`.
- [x] Debug overlay (Dear ImGui 1.91.8, vendored in `third_party/imgui`). F1 opens it, F2 shows a frame-rate line. D3D9 tab: frame time graph, draws, primitives, state sets, texture and vertex-program churn, cache occupancy, HD texture count. Health tab: contiguous memory, host working set, audio underruns, per-thread CPU, world state and current level. The window thread only queues messages (toolkit `xbox_FramebufferWindowSetMsgHook`); ImGui runs on the game thread inside `present()`. While it is open the virtual pad is suspended (`mkb_set_suspended`); a real pad still reaches the game.
- [x] Cheats tab. Unlock all levels (GameState `+0xA4` bits 0–3), unlock all levels and missions (the unused dev routine `sub_00061A90`), and warp to any level (`World::RequestLevel`, `sub_00012710`, with `g_levels[]` at `0x2E2FFC`). Warp is offered only while the gameplay camera is reading the stick: a request during a level load leaves the game black.
- [x] Mouse in menus. Hovering a menu option focuses it the way the d-pad does (`0x282A0`); the click is A. The option's screen rect is the union of its 2D elements' boxes, computed as the engine's scissor code does (`0x16ABA0`). The pointer shows while a menu page is active (`mkb_set_menu_cursor`). Front end and pause menu.
- [x] 360 HD textures (spike, Urban). `tools/xen_tex.py` pairs the `.xbp` and `.xen` packages by resource name and writes the 360 DXT textures as `game/hd/<hash>.dds`, keyed by the HLE's hash of the OG bytes. The 360 surfaces are linear with padded pitch, not tiled. `RECOMP_HD_TEX=<dir>` makes `tex_get` load them: 202 converted, 207 replacements live in Urban at 30 fps. Other levels: run the tool on their packages. Models are not feasible (different mesh layouts, guest memory).
- [ ] Follow-ups: menu sliders and scroll options on the wheel; lightmaps (`lm_*`) are skipped because the 360 atlases differ; the GameState mission bits are shown by `g_levels[]` slot, unverified on a profile with progress.

## P9 — Joke modes

Cheats that borrow other games' mechanics. Nothing from those games is in the repo: each is reimplemented in `recomp_manual.c` from what REA shows of the original, and its assets come from the user's own copy at run time (`docs/MODS.md`). Neither reference recomp ([skate3recomp](https://github.com/mchughalex/skate3recomp), [mw2-recompiled](https://github.com/PaulCombal/mw2-recompiled)) has gameplay code to reuse: both lift 360 PowerPC binaries and keep all game logic in generated code.

- [x] CoD4 research. `iw3sp.exe` is wrapped by Steam DRM, so `iw3mp.exe` (same `BG_*` weapon code) was decompiled: the WeaponDef field table, view kick and its recovery, spread growth and decay, fastfile, XModel and IWI layouts (`docs/MW1_NOTES.md`).
- [x] `tools/cod4_extract.py`: a gun's third-person model from `common_mp.ff` as OBJ (LOD0, attachments hidden by the weapon's `hideTags`), its colour maps as DDS, the raw weapon file as `weapon.ini` with the muzzle (`x_flash`), its sounds as WAVs (`localized_common_mp.ff`), and the flash and tracer sprites.
- [x] `tools/cod4_anim.py`: 54 CoD4 player clips (rifle and pistol idle, ADS, walk, run, sprint, crouch, torso shoot and reload, deaths) retargeted onto the King's 85-bone skeleton (his bone defs in `L_KingMe_World.xbp`): his model space is CoD4's mirrored (faces -Z), his bind pose a T-pose against CoD4's A-pose, so each bone's rest is aligned first; the grip (`tag_weapon_right` against the wrist) rides along per frame.
- [x] Shooter mode, third person (F1 > Cheats, a primary and a sidearm). The CoD4 pose is written into the King's bones before `0x151650` builds them; locomotion blends by his real speed and direction against the aim; torso layers for the shot and the reload; spine bent to the camera's pitch. He faces the aim through his heading (`King+0x30C`/`+0x1A4`). KingCam over the right shoulder (`0x2DED0` distance, `0x2D800`/`0x2D940` shoulder offset, `0x2D5E0` without its auto-turn). Gun, muzzle flash and tracers drawn into the presented buffer against the scene's depth (`mods_mesh.c`); gun sounds through XAudio2 (`cod4_sound.c`). Fire rate, spread, view kick, ADS zoom from CoD4; hitscan through the game's raycast; a hit citizen passes out. Toolkit: `mkb_mouse_buttons`.
- [ ] Polish: play-test feel (camera distance, shoulder offset, turn rate, blend times, aim alignment of the barrel with the crosshair), the gun drawn over the game's HUD (it is drawn at present), finger grip, the rest of the King's own idle (cape, tassels) settling.
- [ ] CoD4 deaths on citizens (retarget onto the citizens' skeletons) instead of the pass-out.
- [x] Skate mode (Skate 3 style, F1 > Cheats; `src/skate.c`).
  - **Research:** Skate 3 is dumped and researched (`docs/SKATE3_NOTES.md`: XEX to PE, headless Ghidra, EB BIG/RefPack, AttribSys); flick-it gestures map to tricks as in Skate 3's state graph.
  - **Movement:** momentum through the King's physics controller (`[King+0x190]+0x30`, the game's collision and wall sliding); push, brake, carve; bails on hard hits.
  - **Look and sound:** a procedural board drawn at present (`mods_mesh.c`); the stance, ollie lift and bail fall on his bones before `0x151650`; the camera follows the board; synthesised board sounds.
  - **Toolkit:** `mkb_set_mute`.
- [ ] Skate polish: Skate 3's real tuning numbers, sounds and board model; grinds and manuals.

## W — Web port (experimental, merged into main)

Research, pros and cons, and decisions: [WEB_PORT_RESEARCH.md](WEB_PORT_RESEARCH.md). Chrome only, built and served locally from the user's own disc. Toolkit work goes in the vendored `xboxrecomp/`.

- [x] **W0 Feasibility spikes.** Done: go (see WEB_PORT_RESEARCH.md §6). Compile `src/recomp/gen` with `emcc -O2` and record wasm size, per-function body sizes against the 7.6 MB limit, and compile time. Measure the frame-time cost of a remapping `XBOX_PTR` in the native build. List every guest instruction that touches the MMIO ranges (0xFD…, 0xFE8…). Exit: go/no-go numbers written into WEB_PORT_RESEARCH.md.
- [x] **W1 Portable memory and MMIO (native).** Done: `RECOMP_COMPACT_MEM=ON` plays the camp level (WEB_PORT_RESEARCH.md §7). One linear guest buffer behind `XBOX_PTR`, with no mirrors or aliasing. MMIO through explicit checks, with no VEH or page protection. Exit: the Windows build plays the first level with the VEH disabled.
- [x] **W2 GL host (native).** Done as a GL ES 3 renderer (`src/gl_hle.c`) tested on native GL 4.3; the SDL part was dropped: the browser host is Emscripten html5 and Web Audio (WEB_PORT_RESEARCH.md §8). A GL back end for `d3d9_hle.c`: a fixed-function ubershader, VSH to GLSL ES 3.00, and an S3TC fallback. SDL for audio, input and the window. ImGui on SDL2/OpenGL3. Exit: the SDL build plays menus and the first level on Windows.
- [x] **W3 Emscripten build.** Done: boots, movies, menus and the camp level in Chrome at 30 fps (§8). An `emcmake` preset, `PROXY_TO_PTHREAD` with a pre-sized pool, OffscreenCanvas, the ISO through WORKERFS with an XDVDFS reader, audio through an AudioWorklet, pointer lock, and a web key map. Exit: boots to the title screen in Chrome.
- [x] **W4 Performance.** Exit met without tuning: the camp level holds 30 fps in Chrome (dips to ~25); profiling and the exe-size cost of compact memory remain open. Chrome profiling, splitting huge or hot functions, and batching uploads. Exit: the first level at 30 fps in Chrome.
- [x] **W4.1 Monorepo merge.** `main` merged in (P9 and the vendored toolkit). Each build target is a CMake preset (`release`, `gl`, `web`). One lift with `--mmio-range` serves every preset. The page has the unlock and warp cheats (`web_cheat`). Shooter and skate mode stay D3D9-only (`mods_stub.c`).
- [ ] Shooter and skate mode on `gl`: port `mods_mesh.c` to GL and the mod sounds to Web Audio.
- [ ] **W5 Packaging.** `tools/web_port.py`, which goes from disc to a static `web/` folder, plus `serve.py` sending COOP/COEP. Exit: a fresh clone and the user's ISO reach a playable page with one command.

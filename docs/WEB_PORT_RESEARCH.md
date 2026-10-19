# Web port research: Sneak King in the browser (WebAssembly)

Status: research only, 2026-10-17. No code changes yet. Claims tagged *[unverified]* come from one secondary source.

## 1. What other recompiled games do

### No big framework has a web build
None of these has a browser build:
- N64Recomp / Zelda64Recomp, blocked by its renderer. RT64 supports only D3D12, Vulkan and Metal.
- XenonRecomp / UnleashedRecomp.
- xboxrecomp.

### Recomps built for the browser from the start (all 2026)
| Project | Guest → toolchain | Graphics | Threads | Notable |
|---|---|---|---|---|
| [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) | EE MIPS → C++, Emscripten | WebGPU. A compute-shader tile rasteriser emulates the GS in software. | Guest in a worker, VU1 in its own worker, GPU fed through a ring buffer to the page thread. Uses shared memory (SAB). | GT4 runs at 50 fps on a laptop's built-in GPU. About 300 MB of generated code. 16 KiB code units, tail calls between them. Guest thread switches unwind the host stack, so no Asyncify. Chromium only. |
| [psp-web-recomp](https://github.com/snuri00/psp-web-recomp) | MIPS → C++, Emscripten | WebGL2. Render targets keyed by VRAM address. | GE on an OffscreenCanvas worker. Uses SAB, with a `?threads=0` fallback. | God of War: Chains of Olympus at 60 fps in **Chrome and Firefox**. The first build ran at 6 fps, and profiling, not renderer work, did most of the fixing. |
| [Pepsiman / RepsiMan](https://github.com/mstan/psxrecomp) | PS1 → C, Emscripten | ? | Threaded; COOP/COEP headers set through Cloudflare `_headers` | The user picks their own CUE/BIN. Offline PWA. Chromium only. |
| [vitaslop](https://github.com/cretz/vitaslop) | ARM → wasm (Rust) | WebGPU; shaders translated to WGSL at runtime | JSPI plus a cooperative scheduler | Imports go to OPFS. A 3.3 GB package doesn't fit in the 32-bit heap, so it is read on demand. |

### Source ports and decomps, for lessons
- **d3wasm (Doom 3).** The game was made single-threaded, so audio jitters along with the frame rate. All data was held in RAM: 0.85–2 GB.
- **diabloweb.** Its menus and modal loops had to be rewritten.
- **Celeste/Terraria wasm** (managed .NET/FNA). Takes about 600 MB of RAM.
- **BoxedWine** (emulating Win32). Slow, which is why static recompilation wins here.

### The recipe they share
1. Emscripten.
2. The guest runs in workers on SharedArrayBuffer, which needs COOP/COEP headers.
3. A renderer written for WebGL2 or WebGPU, not a ported one.
4. Audio in an AudioWorklet fed from a shared ring buffer.
5. The user supplies the disc, streamed or copied into OPFS.
6. **Lifted code is never hosted.**

## 2. Platform facts that shape the design
- **Function limits.** A function body may be at most 7,654,321 bytes and 50k locals. A module may hold 1M functions and 1 GiB. 13.7k functions is fine. Our largest lifted function is about 20k lines (`recomp_0053.c`), so measure its wasm size.
- **No on-stack replacement in V8.** A function that never returns, such as a thread entry or the main loop, stays on the baseline Liftoff code forever.
- **Memory.** wasm32 gives 4 GiB. memory64 works in Chrome 133+ and Firefox 134+, **not in Safari**, and costs 10% to over 100% in bounds checks.
- **Threads.** pthreads need cross-origin isolation. The main browser thread can't block, so use `-sPROXY_TO_PTHREAD`. Pre-size the pool with `-sPTHREAD_POOL_SIZE`. Avoid `ALLOW_MEMORY_GROWTH` with threads.
- **Control flow.**
  - Tail calls work in all browsers (Safari 18.2+).
  - Use wasm exceptions with `-sSUPPORT_LONGJMP=wasm`.
  - JSPI: Chrome 137+. Firefox and Safari *[unverified]*.
  - Asyncify: avoid. It costs about 50% and scales badly with huge functions.
- **Floating point and SIMD.** f32/f64 only, with no rounding-mode or FTZ control. Our x87 is already modelled as `double`, which is fine. SIMD128 works everywhere; relaxed SIMD is still flagged in Safari.
- **Expected speed.** 1.3–2x slower than native.
- **Graphics support.**
  - WebGL2 works everywhere, including iOS.
  - WebGPU works in Chrome, in Safari 26, and in Firefox on Windows and macOS. Linux is spotty.
  - No maintained D3D9 → WebGL/WebGPU layer exists. ToGL is desktop GL and abandoned; DXVK is Vulkan only.
  - D3DCompile doesn't exist, so shaders must be emitted as GLSL ES 3.00 (or WGSL).
- **Input.**
  - Escape always releases pointer lock.
  - Keyboard Lock is Chromium-only and works only in fullscreen.
  - F1, F5, F11 and F12 are unreliable.
- **Storage.**
  - `File.slice` / WORKERFS reads the user's ISO lazily, with no quota.
  - OPFS sync handles (in workers) work as a cache.
  - Safari can evict data after 7 days unless the page is installed as a PWA.
- **Hosting.** GitHub Pages needs `coi-serviceworker` to get COOP/COEP. Cloudflare Pages and Netlify can set the headers through `_headers`.

## 3. What in this codebase stands in the way
The generated C is already close to portable:
- No asm, SEH, `long double`, MSVC-only constructs or SSE intrinsics without guards.
- TLS registers and `__sync_*` atomics.
- `RECOMP_ICALL` uses a flat `void(*)(void)` table, which suits wasm.

The toolkit already has a POSIX/SDL2 path: `win32_compat.c`, `d3d8_gl.c` (GL 3.3), `apu_sdl2.c`, `fb_present_sdl.c`, `keyboard_sdl.c`.

| Subsystem | Where | Web difficulty | Why |
|---|---|---|---|
| Guest address space | `xbox_memory_layout.c`, `guest_vmem.c`, `XBOX_PTR` (`recomp_types.h:509`) | **High** | 64 MB of RAM appears as 28 aliased mirrors, plus a contiguous window at 0x80000000, plus apertures at 0xF0000000–0xFFFFFFFF. Wasm can't alias pages. **But** every access goes through one macro, so the layout can be changed in one place. |
| MMIO traps (APU, AC97, NV2A) | `apu_mmio_hook.c`, `nv2a_mmio_hook.c`, `mmio_decode.h`, the VEH in `main.c:225` | **High** | They use page protection and decode the faulting host x86-64 instruction. Wasm has no faults, so these become explicit range checks. |
| Generated code | `src/recomp/gen` (59 files, 62 MB, 1.5M lines) | **Med** | Huge functions and 73k gotos, so irreducible control flow adds size; long compiles. |
| Threads, sync, timers | `kernel_thread.c`, `kernel_sync.c`, `kernel_bridge.c` | **Med** | pthread shims exist. **Guest spin-waits released by host threads are load-bearing** (the NV2A ack thread, ISRs/DPCs on the timer thread), so real threads are required. |
| Rendering | `src/d3d9_hle.c` (2.2k lines) | **High** | D3D9 fixed function (texture-stage combiners, lights, fog, alpha test, BUMPENVMAP) needs an ubershader. VSH → GLSL replaces D3DCompile. DXT needs a CPU fallback where S3TC is missing. CPU writes to render targets are detected by hashing. |
| File I/O | `kernel_file.c` (has a POSIX branch) | **Med** | 1.5 GB. Movies are 919 MB. Needs lazy reads. |
| Audio out | `apu_core.c` + `apu_sdl2.c` | **Low** | SDL2 audio works under Emscripten. The APU thread needs pthreads. |
| Input | `mkb_input.c` (Raw Input), `keyboard_sdl.c`, SDL pad | **Low/Med** | Mouse via pointer lock. Rebind Escape and the F-keys. |
| Window/present, overlay, diagnostics | `fb_present.c`, `overlay.cpp`, dbghelp, VEH | **Low** | `fb_present_sdl.c` exists. ImGui has SDL2/OpenGL3 backends. Drop dbghelp. |
| Build | `CMakeLists.txt`, presets | **Low** | Add an `emcmake` preset. Drop `/bigobj`, `/LARGEADDRESSAWARE` and `/MAP`. |

## 4. Pros and cons

### Pros
- **P1. Static recomp has already been proven on the web.** PSP and PS2 recomps hit 50–60 fps in 2026 with the same Emscripten + workers + HLE-renderer recipe.
- **P2. The generated C is portable.** It is clang-ready, with no asm, SEH or MSVC-only code, and the x87 model already uses `double`.
- **P3. All memory access goes through one `XBOX_PTR` macro.** Remapping the guest address space is a change in one place, not a relift.
- **P4. The toolkit already has a POSIX/SDL path.** Threads, file I/O, audio, present and keyboard have non-Windows versions to build on.
- **P5. The hardware fits comfortably.** 64 MB of guest RAM and a 733 MHz-era CPU sit well inside wasm32, even at a 2x slowdown. Today the game runs at 30 fps on a modern machine, with a lot of headroom.
- **P6. The renderer is already HLE.** `d3d9_hle.c` reads guest D3D state and issues host draws (`DrawPrimitiveUP` only). Retargeting it to WebGL2 is a back-end swap, not GPU emulation, which is simpler than the PS2/PSP ports.
- **P7. The game's own code decodes its movies and audio.** No Media Foundation or codec work is needed.
- **P8. A cross-platform core comes for free.** An SDL/GL path is also a Linux/macOS port, and it is reusable by other xboxrecomp games, so it is worth upstreaming.
- **P9. Zero install.** A friend opens a URL, picks their ISO, and plays.

### Cons and risks
- **C1. Legal and distribution, the biggest constraint.** The `.wasm` is built from lifted game code, so hosting it publicly breaks this repo's hard rule. Every web recomp ships only the runtime and has the user generate the game part from their own disc. Options:
  - (a) Build locally and serve from localhost. Simple, but it isn't "a URL anyone opens".
  - (b) Lift in the browser. The Python lifter can't do this, so it would need a lifter rewritten in C/Rust/wasm that emits wasm directly. That is a large job.
  - (c) A local port script, the snuri00 model: the user runs a tool once and gets a static site folder.
- **C2. The address space.** The 4 GB aliased layout doesn't fit wasm32 directly. A remapping `XBOX_PTR` costs on every access: about 340k sites, maybe 5–20% *[estimate, measure in P0]*. memory64 excludes Safari and costs bounds checks.
- **C3. MMIO without page faults.** The APU, AC97 and NV2A register traps must become explicit checks. Either `XBOX_PTR` checks the range (slow, on every access), or the lifter emits MMIO accessors at the sites the game actually uses (needs analysis of which sites touch 0xFD/0xFE8 ranges).
- **C4. Threads are mandatory.** The spin-waits and the ISR/DPC model need real pthreads. That means COOP/COEP everywhere, a pre-sized worker pool of about 10–12, no main-thread blocking, and `PROXY_TO_PTHREAD`. The single-threaded/Asyncify route would mean rewriting every spin site; rule it out.
- **C5. Renderer rewrite.**
  - A fixed-function ubershader on WebGL2 (texture-stage ops, lights, fog, alpha test, bump env).
  - An NV2A → GLSL ES emitter.
  - S3TC fallback, cube maps, render-target hashing/readback, and resolution scaling.
  - About the size of P7.5 again.
- **C6. Huge functions.** The 20k-line function may produce slow Liftoff-only code (no OSR), long TurboFan tier-up, or even hit the 7.6 MB body limit. Lifter splitting may be needed.
- **C7. Performance is uncertain.** 1.3–2x slower than native, no FTZ, and SIMD only for vector code. The native build already needed profiling to reach 30 fps.
- **C8. Load and compile time.** 62 MB of C is likely 30–80 MB of wasm *[estimate]*. That means streaming compile, HTTP caching, and a first load measured in many seconds.
- **C9. Browser coverage.** Plan on Chromium plus Firefox via WebGL2. Safari is possible with no memory64 and no relaxed SIMD, but it has eviction and audio quirks.
- **C10. Input friction.** Escape (START), F1/F2/F9 and pointer lock each need a user gesture or a rebinding. `mkb.ini` defaults need a web profile.
- **C11. Debuggability drops.** No VEH stack dumps, no RenderDoc on D3D9, and DWARF debugging in Chrome is clunky. Keep the native build as the reference and land the web work as a second target, not a fork.
- **C12. The toolkit diverges.** The work touches many files on the local `sneak-king-p3-boot` branch. Keep the changes behind `__EMSCRIPTEN__` and SDL paths so they can go upstream.

## 5. Recommended shape (draft plan, for discussion)
**Decisions:**
- Emscripten, wasm32, `PROXY_TO_PTHREAD`, WebGL2 (WebGPU later if needed).
- SDL2 for audio, input and the window.
- The ISO picked by the user and read lazily.
- Distribution by **local port script** (C1c).

Go through **SDL/GL on native Windows/Linux first**: every web problem except threads and headers can be debugged with normal tools.

- **W0. Feasibility spikes (gate everything).**
  1. Compile the gen code with `emcc -O2` and record wasm size, per-function sizes and compile times.
  2. Prototype the compact address map behind `XBOX_PTR` on **native** and measure the frame-time cost.
  3. List every guest instruction that touches the MMIO ranges.

  Kill or continue on these numbers.
- **W1. Portable memory and MMIO, native.**
  1. A remapping `XBOX_PTR` with a single linear guest buffer (no mirrors or aliasing).
  2. MMIO via explicit checks.
  3. No VEH.

  The Windows build still plays.
- **W2. SDL/GL host, native.**
  1. `d3d9_hle.c` gets a GL back end: an FF ubershader, VSH → GLSL ES 3.00 (desktop GL validates it), S3TC fallback.
  2. SDL for audio, input and the window.
  3. ImGui on SDL2/OpenGL3.

  Result: a Linux/Windows SDL build that plays.
- **W3. Emscripten build.**
  1. An `emcmake` preset, the pthread pool, OffscreenCanvas or GL proxying, and WORKERFS over the picked ISO (XDVDFS reader at `0x18300000`).
  2. An AudioWorklet through SDL, pointer lock, and a web key map.
- **W4. Performance.**
  1. Profile in Chrome.
  2. Split or outline hot huge functions, and keep the main loop out of never-returning bodies.
  3. Upload batching.
- **W5. Packaging.** A `tools/web_port.py` that lifts and builds from the user's disc into a static `web/` folder, with a `serve.py` that sends COOP/COEP. Optionally a PWA plus OPFS cache.

## 6. W0 results (2026-10-17)
The verdict is **go**. Spikes were built in `build-wasm-spike/`, `build-census/` and `build-fold/`, all gitignored.

**1. Generated code under Emscripten.** Emscripten (emsdk latest) compiles all 59 gen files with `emcc -O2 -pthread` and **zero errors**.
- About 3 minutes at 4 jobs; the slowest file (`recomp_0023.c`) takes 12 s.
- 22 MB of wasm objects.
- 14,050 functions. The largest is `sub_00296B63` at **188 KB**, 40x under the 7.6 MB body limit.

The irreducible-goto worry did not materialise.

**2. Address folding.** The layout comes down to three stores:
- RAM at 0, with mirrors every 64 MB;
- the contiguous window at 0x80000000, with the tiled aperture at 0xF0000000 as its alias;
- MMIO from 0xFD000000.

`RECOMP_FOLD_ALIASES` (template `recomp_types.h`, CMake option) makes `XBOX_PTR` fold the aliases with `a >= 0xFD000000 ? a : a & 0x83FFFFFF`. The native build plays identically. Results:
- **The exe is 27.5% bigger** (31.2 → 39.8 MB): a compare and a mask on every access.
- **No measurable runtime change in the paced intro movies.** Both threads are wait-bound there, so this is not a real benchmark; measure gameplay in W4.
- **Ways to cut the cost:** have the lifter skip the fold for esp-relative and constant addresses. In wasm, place the stores so one shift-and-or (or a 64-entry slice table) maps them.

**3. MMIO census.** `RECOMP_MMIO_CENSUS` logs each lifted function that touches 0xFD000000 and up. A 90 s run through boot, movies and menus found **91 functions, all XDK library code**:
- **D3D** (0x1EE000–0x201000): NV2A registers. Plain memory today.
- **DirectSound** (0x245000–0x24E000): APU 0xFE80xxxx–0xFE82xxxx and AC97 0xFEC00xxx. **These are the only trapped pages**, about 40 functions.
- **XAPI USB** (0x2A7000–0x2AE000): OHCI at 0xFED00000.

The game's own code never touches MMIO. In W1, MMIO becomes a lifter option that emits checked accesses only inside listed functions, or simply the DSOUND/XAPI address ranges. Every other access keeps the fast path. Gameplay may reach more DSOUND functions; rerun the census in a level.

## 7. W1 results (2026-10-17): portable memory and MMIO, native
**Status:** done. `RECOMP_COMPACT_MEM=ON` (CMake) builds the WebAssembly memory model natively, and **plays the first level**: boot, movies, menus, CONTINUE GAME and the camp level, with audio and the USB pad.

**Explicit MMIO.** `tools.recomp --mmio-range LO-HI` (`tools/recomp/mmio_rewrite.py`) rewrites the listed functions' `MEMn` accesses into `MMIO_Rn`/`MMIO_Wn`. This repo passes the XDK D3D, DSOUND and XPP sections; that rewrites about 30.5k accesses. An unsupported shape raises at lift time rather than slipping through.
- The helpers (template `recomp_types.h`) are plain memory below 0xFD000000 and otherwise call `xbox_MmioRead/Write` (`xbox_memory_layout.c`).
- Those route to:
  - the APU model (0xFE800000–0xFE830000);
  - the OHCI controllers (`xbox_OhciRead/Write`);
  - the AC97 page (a plain write, then the reset sweep);
  - plain memory for everything else.
- `xbox_MmioExplicit()` is 1 under Emscripten or compact memory, or with `RECOMP_MMIO_EXPLICIT=1`. When it is on, no register page is trapped.
- Traps off: audio RMS matched the trapped run (442 vs 452 over 73 s), START skips the movies (USB pad), and the menus render.

**Compact memory** (`XBOX_COMPACT_MEM`, forced under Emscripten): one 192 MB block (RAM, the contiguous window, and 0xFC000000+).
- Mirrors and the tiled aperture fold onto their stores, so there are no views, placeholders or VEH.
- Host code translates through `xbox_GuestPtr`/`XBOX_GVA`/`xbox_GuestVa` (`xbox_memory_layout.h`). Converted: the memory layout, kernel bridge, HAL, RTL, thunks, trace, OHCI, `fb_present`, and the APU's physical resolver.
- Lifted code uses a branch-free `(a & 0x03FFFFFF) | ((a >> 5) & 0x04000000)`. It cannot reach 0xFC000000+, which only the `--mmio-range` functions touch (the W0 census).
- The exe is 41.2 MB, against 31.7 MB for the default layout (+30%). Its speed is not measured yet; measure it in W4.

**Known leftovers:**
- `nv2a_pb_scan.c` and `nv2a_pb_exec.c` still index guest memory from a raw base. They are bring-up tools (`RECOMP_PB_SCAN`, `RECOMP_PB_EXEC`) this title does not use; they are wrong in compact mode.
- The RECOMP_WATCH debug watchpoint assumes RAM addresses.
- `RECOMP_APU_TRACE=1` crashes the game in the default layout too, at guest 0xFFFFFFBE from `sub_00248906`. That is an existing bug, not part of this work.

## 8. W2/W3 results (2026-10-17): the first level in Chrome
**Status:** the game boots, plays its movies, shows the menus and **plays the camp level in Chrome at its 30 fps cap**, with dips to about 25. That is a new game, as the browser has no save yet.

**How to run it:**
1. Generate the lifted code with the `--mmio-range` sections (CLAUDE.md).
2. `tools\web_build.cmd` builds into `build-web/`.
3. `.venv\Scripts\python.exe tools\serve.py`, then open http://localhost:8080/.

Query-string switches become the environment, for example `?RECOMP_D3D9_STATS=1`. `node tools/web_drive.mjs <url> <secs> <outdir> [Key@sec|PAUSE@sec ...]` drives Chrome over the DevTools protocol: it collects console output, screenshots every 5 s, presses keys, and with `PAUSE` dumps every thread's stack.

**Renderer (W2):** `src/gl_hle.c` is a GL ES 3.0 / WebGL2 back end next to `d3d9_hle.c`, sharing the guest side (`src/d3d_hle_shared.h`). It is selected by `RECOMP_RENDERER=gl`.
- Fixed function is generated GLSL, cached per state key: FVF, D3D lighting, texture-coordinate generation and transforms, the texture-stage combiner, fog and alpha test.
- Vertex programs go through the NV2A translator (`vsh_body`) in GLSL.
- Textures decode to RGBA8; DXT goes through S3TC.
- Natively it runs on a GL 4.3 core context on the framebuffer window. Against D3D9 it differs by 2/255 on average in the menu and the level.
- Not done: register combiners (as on D3D9), bump mapping, vertex blending, the ImGui overlay.

**Browser host (W3):**
- `src/main_web.c` handles boot and mounts `game/` through the WasmFS fetch backend from a manifest. Files are read lazily with HTTP range requests.
- `src/web_host.c` maps the native `mkb.ini` layout (keys, mouse buttons, wheel) onto the toolkit keyboard pad, points at menu options with the free pointer, and locks the pointer for the mouse camera in play.
- The toolkit's `apu_web.c` plays audio: a ring in shared memory drained by a ScriptProcessorNode.
- `-sPROXY_TO_PTHREAD`, 32-worker pool, 1.5 GB fixed memory, OffscreenCanvas.
- **Frames:** an OffscreenCanvas in a worker that never yields never shows its frames, and Chrome has no `commit()`. Each present therefore sends `transferToImageBitmap()` over a BroadcastChannel to a `bitmaprenderer` canvas on the page.
- `.wasm` is 29 MB.

**Toolkit fixes found on the way** (POSIX in general, not only the web):
- **Alertable waits** (`WaitForSingleObjectEx`, `SleepEx`) now end for an APC queued *during* the wait. Before, they only checked before waiting, so Sneak King's loader thread never saw the APC that starts each package read.
- **Case-insensitive path lookup** (`posix_fold_case`).
- **Emscripten branches:** no OpenSSL, the SDL2 port, memory status.
- **Partition images** read as sparse on WasmFS, and report their table size.

**Known gaps:**
- Saves are in memory (lost on reload); use OPFS next.
- Audio has not been checked by ear.
- The fetch backend keeps every chunk it read in JS memory (movies add up).
- No overlay.
- Escape can't be START.
- Mouse camera and pointer lock untested.
- `RECOMP_D3D9_DUMP` writes to the in-memory filesystem.

**Decided (2026-10-17):**
1. Distribution is **local port-and-serve** (C1c). A hosted build is out of scope for now.
2. Target **Chrome only**. JSPI, WebGPU and Document-Isolation-Policy are all usable if they help.
3. Do the **native SDL/GL step first** (W1–W2).

Work lives on the branches `web-port-experimental` (this repo) and `sneak-king-web-port` (xboxrecomp, from `sneak-king-p8-qol`).

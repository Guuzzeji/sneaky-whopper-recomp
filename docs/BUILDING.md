# Building and running Sneak King

This project turns the original Xbox `default.xbe` of *Burger King: Sneak King* into native code. One checkout builds two kinds of game:

| Build | Preset | Output | Status |
|---|---|---|---|
| Windows (Direct3D 9) | `release` | `build\sneakking_recomp.exe` | The main build. Plays the whole game. Has the F1 overlay, the cheats and the joke modes. |
| Web (WebAssembly, Chrome) | `web` | `build-web\sneakking_recomp.{js,wasm}` | Experimental (branch `web-port-experimental`). Boots, plays the movies and menus, and runs the first level at about 30 fps. Has the unlock and warp cheats. |

Two more presets are for development. `relwithdebinfo` is the Windows build with a PDB. `gl` is the web build's renderer and memory layout as a Windows exe, so you can debug the web path in Visual Studio.

You must supply the game yourself. The repo holds no game data: no ISO, XBE, extracted files or lifted code. Everything below starts from a dump of your own disc.

---

## How it works

### 1. From the disc to C (the pipeline)

The toolkit is in `xboxrecomp/`, a vendored copy of [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) with our patches. Its Python tools run once per change to the analysis:

1. `tools.xiso` unpacks the ISO into `game/`.
2. `tools.disasm` finds every function in the XBE (about 13,650), with help from the seed lists in `seeds/`.
3. `tools.func_id` and `tools.abi_analysis` name the XDK library functions and work out calling conventions.
4. `tools.recomp` lifts each x86 function to a C function in `src/recomp/gen/`. Every guest register is a global (`g_eax`, `g_esp`, ...) and every guest memory access is a macro (`MEM32(va)`) into one block of host memory.

The `--mmio-range` sections (the XDK's D3D, DSOUND and XPP code) are lifted with explicit calls (`MMIO_R32`, `MMIO_W32`) wherever they touch hardware registers. On Windows the same calls work alongside the page traps, so **one `src/recomp/gen/` serves every preset**. The browser can't trap memory pages, so the web build depends on these calls.

Never edit `src/recomp/gen/` by hand: the next lift overwrites it. Fixes go in `src/recomp_manual.c` (function overrides), in the JSON configs passed to `tools.recomp`, or in `xboxrecomp/`.

### 2. The runtime (the toolkit's libraries)

The lifted code runs on the toolkit's model of the Xbox:

- **Kernel:** all 160 kernel imports (`xboxkrnl.exe`), mapped to the host: threads, events, files (the `D:` drive is `game/`), memory.
- **APU:** the MCPX audio processor (voice processor and DSP), mixed to the host's audio. That is XAudio2/winmm on Windows and an AudioWorklet in the browser (`apu_web.c`).
- **OHCI:** the USB controller, with a virtual Xbox pad. Keyboard and mouse drive it (`mkb.ini`).
- **NV2A:** just enough of the GPU for the game's D3D library to believe its pushbuffer ran. Nothing executes the pushbuffer.

### 3. Graphics: D3D8 HLE

The game links the XDK's Direct3D 8. We wrap its entry points (`sub_X` becomes `hle_*`) and replay each call on a host API. The setters run the lifted body and mirror the state. The draws, clears and the present are replaced.

- `src/d3d9_hle.c`: Direct3D 9 on Windows. NV2A vertex programs become `vs_2_0` HLSL.
- `src/gl_hle.c`: OpenGL ES 3 / WebGL2, for the browser and the `gl` preset. NV2A vertex programs become GLSL ES 3.00.
- `src/d3d_hle_shared.h`: the guest side both renderers share. It holds the guest state addresses, argument access, resource decoding, vertex declarations and the NV2A vertex program decoder.

The CMake option `RECOMP_RENDERER` (`d3d9` or `gl`) picks the renderer. The presets set it for you.

### 4. The host

| | Windows | Web |
|---|---|---|
| Entry point | `src/main.c` (`WinMain`) | `src/main_web.c` (`main` on a pthread, `-sPROXY_TO_PTHREAD`) |
| Window | toolkit `fb_present.c`, a Win32 window | a `<canvas>`. Frames go to the page as ImageBitmaps. |
| Input | raw input, `mkb.ini` bindings (`mkb_input.c`) | DOM key, mouse and pointer-lock events (`src/web_host.c`), with `mkb.ini`'s bindings, into the keyboard pad |
| Game files | read from `game/` | fetched from `game/` over HTTP range requests, file by file (WasmFS) |
| Memory | identity layout, with page traps for registers | compact layout (`XBOX_COMPACT_MEM`), explicit MMIO only |
| Debug UI | F1 overlay (Dear ImGui) | the log under the picture and the cheat buttons |

### 5. Cheats and joke modes

The cheats live on the game side in `src/recomp_manual.c`. They read and write guest memory, and some call lifted functions. They run on the game thread at present time, because a call into lifted code must not race the guest:

- **Unlock levels:** sets GameState `+0xA4` bits 0–3.
- **Unlock levels + missions:** calls the game's unused developer routine `sub_00061A90`.
- **Warp to a level:** calls `World::RequestLevel` (`sub_00012710`). This works only while you control the King, because a request during a load leaves the screen black.
- **Shooter mode** (CoD4 guns, third person) and **skate mode** (Skate 3 board): see `docs/MODS.md`. Each needs files extracted from your own copy of the other game.

| | Windows | Web |
|---|---|---|
| Where | F1 > Cheats | the buttons under the picture |
| Unlock, warp | yes | yes |
| Shooter, skate | yes, when their files are in `game/mods/` | no |

The joke modes draw with Direct3D 9 (`mods_mesh.c`) and play sounds through XAudio2 (`cod4_sound.c`). The `gl` and `web` presets link `src/mods_stub.c` in place of those files, so both modes report "unavailable" there. Porting them is an open item in `docs/ROADMAP.md`.

---

## Getting started

### Prerequisites (both builds)

- Windows 10 or 11.
- Visual Studio 2022 with the "Desktop development with C++" workload. It provides MSVC, CMake and Ninja. All build commands run inside `vcvars64.bat`:
  ```
  cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && <command>'
  ```
- Python 3.11 or newer, in a venv at the repo root:
  ```
  python -m venv .venv
  .venv\Scripts\python.exe -m pip install capstone pytest pefile numpy
  ```
- A dump of your own *Sneak King* disc (redump ISO). Put it in the repo root.
- A machine with about 16 GB of RAM. The generated C files are huge; keep the build at 2 jobs (the presets do this).

### Step 1: unpack the disc and lift the game (once)

Run these from `xboxrecomp\`, with `$py` set to `..\.venv\Scripts\python.exe`, `$x` to `..\game\default.xbe` and `$a` to `..\analysis`. Set `PYTHONIOENCODING=utf-8` first. `CLAUDE.md` has the same list with absolute paths.

```powershell
& $py -m tools.xiso unpack ..\<your-dump>.iso -o ..\game
& $py -m tools.xbe_parser $x --json ..\game\default_analysis.json
& $py -m tools.disasm $x -o $a\disasm --force --seed-functions ..\seeds\p1_static.json --seed-functions ..\seeds\p3_runtime.json --seed-functions ..\seeds\p5_xpp.json   # about 13 minutes
& $py tools\ghidra_naming\merge_names.py --names-json $a\names_combined.json --functions-json $a\disasm\functions.json --apply   # only if you have names_combined.json
& $py -m tools.func_id $x --functions $a\disasm\functions.json --strings $a\disasm\strings.json --xrefs $a\disasm\xrefs.json -o $a\func_id
& $py -m tools.abi_analysis $x --disasm-dir $a\disasm --func-id-dir $a\func_id --output-dir $a\abi
& $py -m tools.recomp $x --all --split 250 --disasm-dir $a\disasm --func-id-dir $a\func_id --abi-dir $a\abi --gen-dir ..\src\recomp\gen --game-name "Sneak King" --exclude-manual ..\src\recomp_manual.c --exclude-manual ..\src\d3d9_hle.c --exclude-manual ..\src\d3d_hle_shared.h --mmio-range 1EC5C0-20FB40 --mmio-range 23CB60-262FEC --mmio-range 2A65C0-2AFB10
```

Keep the three `--mmio-range` arguments, because the web build needs them. Keep all three `--exclude-manual` files too. They tell the lifter which functions our code overrides or wraps; without `d3d_hle_shared.h` the link fails with unresolved `sub_X_gen` symbols. Run only the last command again after you change `recomp_manual.c` overrides or the toolkit's lifter.

Optional: the 360 HD textures. Run `tools/xen_tex.py <pkg.xbp> <pkg.xen> -o game/hd` on the 360 packages; both builds use `game/hd` when it exists.

### Step 2a: the Windows build

```
cmake --preset release
cmake --build --preset release
build\sneakking_recomp.exe
```

The first build takes a while (the generated code is large). Later builds compile only what changed.

You can also build from an IDE:
- **Visual Studio 2022:** File > Open > Folder on the repo root, pick a preset in the toolbar, then press F5.
- **VS Code:** install the CMake Tools and C/C++ extensions. The CMake Tools status bar lists the presets. The "Sneak King" launch configuration debugs the selected build.

Controls: WASD moves, the mouse turns the camera, Shift runs, left click or Space is A, right click or Q is B, E is Y, F toggles first person, and Escape or P is START. The arrows and Enter work in menus. F1 opens the overlay (stats, health, cheats), F2 shows the frame rate and F9 frees the mouse. The bindings are in `mkb.ini`.

The exe needs no switches. `CLAUDE.md` lists the `RECOMP_*` environment switches for testing (scale, widescreen, traces, warps, the joke modes).

### Step 2b: the web build (experimental)

You must be on the `web-port-experimental` branch.

1. Install the Emscripten SDK into `C:\repos\emsdk`:
   ```
   git clone https://github.com/emscripten-core/emsdk C:\repos\emsdk
   C:\repos\emsdk\emsdk install latest
   C:\repos\emsdk\emsdk activate latest
   ```
   If you put it somewhere else, set `EMSDK` in a `CMakeUserPresets.json` that inherits the `web` preset.
2. Build:
   ```
   tools\web_build.cmd
   ```
   This is `cmake --preset web` and `cmake --build --preset web` inside vcvars (which provides Ninja). The output is `build-web\sneakking_recomp.js` and `.wasm`.
3. Serve it and open Chrome:
   ```
   .venv\Scripts\python.exe tools\serve.py
   ```
   Then open http://localhost:8080/. The server sends the COOP/COEP headers the page's threads need. It serves `game/` with range requests and generates `game/manifest.txt`. It binds to localhost only.

**Do not host the web build publicly.** The `.wasm` file contains the lifted game code, and `game/` is your disc.

Web controls are the native ones from `mkb.ini` (the bindings are copied into `src/web_host.c`, so editing `mkb.ini` does not change them): WASD moves, the mouse turns the camera, Shift runs, left click or Space is A, right click or Q is B, E is Y, F is first person, the wheel or Z zooms, Escape or P pauses, and the arrows and Enter work the menus. In menus the pointer is free and points at options. In play the mouse locks at your next key press or click; Escape frees it and a click takes it back. The cheat buttons are under the picture. Switches go in the query string, for example `http://localhost:8080/?RECOMP_SCALE=1&RECOMP_D3D9_STATS=1`. `?RECOMP_HD_TEX=0` turns off the HD textures. Widescreen (16:9, anamorphic, as natively) is on: the page sizes its canvas to match, and `?RECOMP_WIDESCREEN=0` gives 4:3. The Hide log button under the picture hides the log panel, and the browser remembers the choice.

To test without a person at the keyboard, `node tools/web_drive.mjs <url> <secs> <outdir> [Key@sec ...]` drives headless Chrome. It saves the console, screenshots and every thread's stack.

### Debugging the web path natively

```
cmake --preset gl
cmake --build --preset gl
build-gl\sneakking_recomp.exe
```

This is the Windows host with `gl_hle.c` and the compact memory layout: the web build's renderer and memory model, with a native debugger. If a bug shows here, fix it here. If it shows only in Chrome, look at `web_host.c`, `main_web.c` and the toolkit's Emscripten code (`apu_web.c`, WasmFS paths).

---

## Repo map

| Path | What |
|---|---|
| `src/main.c`, `src/main_web.c` | Host entry points: Windows and web |
| `src/d3d9_hle.c`, `src/gl_hle.c`, `src/d3d_hle_shared.h` | The two D3D8 HLE renderers and their shared guest side |
| `src/recomp_manual.c` | Overrides of lifted functions, game hooks, cheats, shooter mode |
| `src/overlay.cpp`, `src/overlay.h` | The F1 overlay (Windows) and the game-side API it uses |
| `src/web_host.c`, `web/index.html` | Browser input, the cheat bridge, the page |
| `src/skate.c`, `src/mods_mesh.c`, `src/cod4_*.c`, `src/mods_stub.c` | Joke modes (D3D9) and their stubs for `gl` |
| `src/recomp/gen/` | Lifted code (generated, gitignored) |
| `xboxrecomp/` | The toolkit: pipeline tools and runtime libraries |
| `tools/` | Our scripts: web build and server, texture and mod extractors |
| `docs/` | `ROADMAP.md` (phases), `RESEARCH.md`, `WEB_PORT_RESEARCH.md`, `MODS.md`, findings |

## Troubleshooting

- **The web page says "Not cross-origin isolated".** You opened the file directly or through another server. Use `tools/serve.py`.
- **The web build links, but the game hangs on boot in Chrome.** The lifted code was generated without `--mmio-range`. Run the `tools.recomp` step again with the three ranges, then rebuild.
- **No sound and the game quits (Windows).** `RECOMP_AC97_READY` was set to `0`. `main.c` turns it on by default.
- **The shooter or skate cheat is missing.** You need the `release` preset and the extracted files in `game/mods/` (`docs/MODS.md`).

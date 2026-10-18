# Sneak King: static recompilation

*Burger King: Sneak King* (2006, Blitz Games) for the original Xbox, recompiled into native code. No emulator: every function of the game's `default.xbe` is lifted from x86 to C, then compiled for the host with a small model of the Xbox around it.

One checkout builds two versions of the game:

- **Windows** (Direct3D 9). This is the main version. It plays the whole game with keyboard and mouse or a pad. It adds widescreen, 360 HD textures, a debug overlay with cheats, and two joke modes.
- **Web** (WebAssembly, in Chrome). This one is experimental. It boots, plays the movies and menus, and runs the levels with the same controls, widescreen and cheats.

**This repo contains no game data.** You need a dump of your own disc. The ISO, the extracted files, the analysis and the lifted code all stay on your machine (`.gitignore` covers them). For the same reason, don't host the web version publicly: its `.wasm` file holds the lifted game code.

## How it works

1. **Lift.** The [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit, vendored in `xboxrecomp/` with our patches, unpacks the disc, finds the game's roughly 13,650 functions and turns each one into C in `src/recomp/gen/`.
2. **Runtime.** The toolkit's libraries stand in for the Xbox hardware the game talks to: the kernel, the audio processor, USB pads and the GPU.
3. **Graphics.** The game's Direct3D 8 calls are intercepted and replayed on the host: Direct3D 9 on Windows (`src/d3d9_hle.c`), WebGL2 in the browser (`src/gl_hle.c`).
4. **Host.** Windows has `src/main.c` (a Win32 window, raw input and `mkb.ini`). The web build has `src/main_web.c` and `src/web_host.c` (a canvas, DOM input, and game files fetched over HTTP).

Fixes go in `src/recomp_manual.c` or the toolkit, never in the generated code. `docs/BUILDING.md` explains each part in depth.

## Running it

You'll need:

- Windows 10 or 11
- Visual Studio 2022 with the C++ workload
- Python 3.11 or newer, in `.venv`
- a dump of your own disc
- about 16 GB of RAM

### 1. Lift the game (once)

Unpack the disc and run the pipeline. The commands are in `docs/BUILDING.md`, step 1; the disassembly takes about 13 minutes. This writes `game/` and `src/recomp/gen/`, which both versions use.

### 2a. Windows

Run these in a Visual Studio x64 developer prompt (`vcvars64.bat`):

```
cmake --preset release
cmake --build --preset release
build\sneakking_recomp.exe
```

You can also open the folder in Visual Studio 2022, pick the `release` preset and press F5.

### 2b. Web

First install the [Emscripten SDK](https://emscripten.org) into `C:\repos\emsdk`. Then:

```
tools\web_build.cmd
.venv\Scripts\python.exe tools\serve.py
```

Open http://localhost:8080/ in Chrome. The server sends the headers the page needs and serves `game/` straight from disk. It only listens on localhost.

URL switches:

- `?RECOMP_WIDESCREEN=0` switches to 4:3.
- `?RECOMP_HD_TEX=0` keeps the original Xbox textures.

## Controls

Both versions use the same controls.

| Input | Action |
|---|---|
| WASD | Move |
| Mouse | Camera (points at options in menus) |
| Shift | Run |
| Left click, Space | A (surprise, accept) |
| Right click, Q | B (back) |
| E | Y (hide, action) |
| F | First person |
| Wheel, Z | Zoom |
| Escape, P | Pause (START) |
| Arrows, Enter | Menus |

On Windows, F1 opens the overlay (stats and cheats), F2 shows the frame rate and F9 frees the mouse. The bindings are in `mkb.ini`.

On the web, the cheats are buttons under the picture. Escape frees the mouse and a click takes it back. A button hides the log.

## Builds

| Preset | Output | What it's for |
|---|---|---|
| `release` | `build\sneakking_recomp.exe` | The Windows game |
| `relwithdebinfo` | `out\build\relwithdebinfo\` | The same, with a PDB for debugging |
| `web` | `build-web\sneakking_recomp.{js,wasm}` | The browser build |
| `gl` | `build-gl\sneakking_recomp.exe` | The web build's renderer and memory layout on Windows, for debugging the web path |

## Cheats and joke modes

| | Windows | Web |
|---|---|---|
| Unlock levels, unlock missions, warp to a level | yes | yes |
| Shooter mode (CoD4 guns, third person) | yes | no |
| Skate mode (Skate 3 board) | yes | no |

The joke modes need files extracted from your own copies of those games. `docs/MODS.md` covers them.

## Docs

| File | Contents |
|---|---|
| `docs/BUILDING.md` | How everything works, full setup, troubleshooting |
| `docs/ROADMAP.md` | Phases and progress |
| `docs/RESEARCH.md`, `docs/P1_FINDINGS.md` | Background and findings about the game |
| `docs/WEB_PORT_RESEARCH.md` | The web port's design and results |
| `docs/MODS.md`, `docs/MW1_NOTES.md`, `docs/SKATE3_NOTES.md` | The joke modes |
| `CLAUDE.md` | Working notes: commands, runtime switches, debugging |

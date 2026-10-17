# P1 — Analysis findings

Date: 2026-10-16. Tools: `tools/p1_survey.py` (this repo), Ghidra 12.1.4 headless via the toolkit's `ghidra_naming`, XbSymbolDatabase CLI v4.0.166. Raw outputs are in `analysis/` (gitignored, since they are derived from game code). Everything here can be regenerated with the commands in [Reproduce](#reproduce).

## 1. Function map and coverage

| Source | Functions | Agreement |
|---|---|---|
| `tools.disasm` | 13,318 | — |
| Ghidra auto-analysis (flat image) | 7,682 | 7,665 land on a disasm start |
| XbSymbolDatabase (XDK names) | 601 FUN + 43 VAR | 524 on a disasm start |
| Ghidra FidDb + symbols | 279 names (166 FidDb) | 166 on a disasm start |

**Names applied.** I applied 690 names to `analysis/disasm/functions.json`. A backup is at `functions.prenames.json`. The names carry a prefix, `xdk_` for XbSymbolDatabase and `crt_` for FidDb, because unprefixed XDK names collide with toolkit runtime symbols. The collisions include `XInputGetState`, `CreateThread`, `D3DDevice_CreateVertexShader`, `read` and `write`, 33 in total. The map is in `analysis/names_combined.json`.

**Coverage gaps.** I counted only non-padding bytes that belong to no function. Padding means `00`, `90` and `CC`.

| Section | Gaps | Bytes | Verdict |
|---|---|---|---|
| `.text` | 591 | 48 KB | **Real missed code.** For example, `0x19B490` is a switch function (`jmp [edx*4+0x19BF18]`) and `0x1CAAD4` is an SSE-aligned function. |
| D3D | 35 | 42 KB | One 41 KB block at `0x2021CE` is float/constant tables. The rest are small. |
| DSOUND | 94 | 88 KB | One 83 KB block at `0x24E6B8` is data (it contains `0xBB80` = 48000 Hz). 28 gaps hold XDK-named functions. |
| XMV | 8 | 129 KB | Two blocks, 108 KB and 20 KB, of codec tables. |
| WMADEC | 21 | 58 KB | Decoder tables. |
| XGRPH / XPP | 109 / 32 | 10 KB / 2 KB | Small. |

**Code pointers in data sections.** `.rdata`, `.data` and `.data1` hold 11,988 dwords that point into code:

- 10,945 point at a function start.
- 679 point into the middle of a function. These are merged-function candidates for `--coalesce` or a split in P3.
- 364 point into a gap. These are missed functions.

**Seeds.** `seeds/p1_static.json` lists 288 addresses to pass to `tools.disasm --seed-functions` at the start of P2:

- 182 data-pointer targets that fall in gaps
- 95 XbSymbol/FidDb names that are not function starts
- 11 Ghidra entries that fall in gaps

**DOLBY is not x86.** The section is flagged `X`, but its only references are two `data_imm` loads in DSOUND, at `0x24E38F` and `0x24E3AD`. Both sit in small getter functions that the detector missed. Each getter writes `0x336A40` or `0x336D60` and a size to out-params. This matches DirectSound's DSP image getters: the bytes are Motorola 56300 microcode for the MCPX APU's GP/EP DSP (the Dolby Digital encoder), and they are uploaded to the APU. **Do not lift DOLBY.** Treat it as data. In P5 it only matters if the APU's EP is emulated; blinx2 stubs GP/EP.

## 2. Indirect control flow

| Kind | Count |
|---|---|
| Jump tables (`jmp [reg*4+tbl]`) | 290 (`.text` 264, D3D 15, XGRPH 9, XMV 2) |
| Vtable-like runs in data (≥2 consecutive function pointers) | 419 runs, 10,151 slots |
| `call [reg+disp]` (vtable/member calls) | `.text` 1,126; DSOUND 104; XGRPH 68; WMADEC 28; XMV 20; XPP 14; D3D 8 |
| `call reg` | `.text` 247; XPP 17; D3D 13; DSOUND 10; WMADEC 1 |
| `call [abs]`, not kernel thunks | `.text` 34; DSOUND 10 |

The `call [reg+disp]` count in `.text` (1,126) matches the C++ engine: func_id found 2,345 thiscall functions and 4,046 vtable methods. All of these need `RECOMP_ICALL` dispatch, and the P3 feedback loop has to cover them. The per-site lists are in `analysis/p1/{jump_tables,vtables}.json`.

## 3. Graphics path decision: **B, the NV2A pushbuffer executor**

Evidence:

- `.text` reaches D3D through **86 distinct entry points at 663 call sites**. XbSymbolDatabase names 83 of them. The heaviest are `DrawVerticesUP` (58 sites), `SetTransform` (55), `SetRenderState_Simple` (49), `SetVertexShader` (41) and `SetStreamSource` (41). It also uses `CreatePixelShader`, so register combiners are needed, and `EnableOverlay`. The full list is in `analysis/p1/text_to_d3d.json`.
- The game **never touches the pushbuffer itself**:
  - `.text` makes no calls to `BeginPush`, `RunPushBuffer` or `MakeSpace`.
  - `.text` has no direct references to `D3D_g_pDevice` (`0x20D328`). All 199 references to it are inside the D3D section.
- D3D8 is the non-LTCG library: a separate `D3D` section with standard entry points.

Why B rather than A (D3D8 HLE):

- The API surface is clean enough that A is possible, but the toolkit has no address-keyed D3D HLE table. A would mean reimplementing 86 or more entry points plus Xbox pixel shaders.
- Upstream v0.14.0 already has B: `nv2a_pb_scan.c`, `nv2a_pb_exec.c` (a CPU rasteriser with VSH and combiners), the `nv2a_backend.h` hook, `RECOMP_PB_EXEC`, `RECOMP_VBLANK`, `RECOMP_GUEST_LOCK`, `RECOMP_TITLE_KEVENTS` and `xbox_Nv2aMirrorFence`.
- blinx2-recomp, built on the same XDK 5849 with non-LTCG D3D, chose B and ships with it.

Plan:

- Lift D3D as-is. Override only the hardware-wait entry points in `recomp_manual.c`:

  | Entry point | Address |
  |---|---|
  | `D3DDevice_BlockUntilVerticalBlank` | `0x1F0020` |
  | `D3DDevice_BlockOnFence` | `0x1EFF50` |
  | `D3D_KickOffAndWaitForIdle` | `0x1EEFC0` |
  | `CDevice_KickOff` | `0x1EEAD0` |
  | `D3DDevice_Swap` | `0x200110` |

  `CMiniport_InitHardware` at `0x1FA7F8` connects the GPU interrupt.
- A host D3D11 back end is project-side work. Upstream has no `RECOMP_PB_BACKEND`; blinx2 has one only in its fork. Use the CPU rasteriser for first pixels.
- **Fallback (hybrid):** if executor performance or fidelity blocks P4/P6, HLE the hot draw calls (`DrawVerticesUP`, `DrawVertices`, `DrawIndexedVertices`) using the address list above.

## 4. Thread map

The only `PsCreateSystemThreadEx` call is at `0x6487C`, inside `xdk_CreateThread` (`0x64843`). That function is called from these sites:

| Creator | Start routine | What it is |
|---|---|---|
| entry `0x637AA` | `0x63736` `mainXapiStartup` | The main thread. It runs CRT init and then `0x2EA20` (the game's main). |
| `0x73E90` (audio stream init, with the "AUDIO1"/"AUDIO2" slots) | `0x71260` | An audio streaming worker. It loops on an alertable `WaitForSingleObjectEx(0x2E4AC4)`, so it is driven by APCs for overlapped I/O completion. |
| `0x75300` (engine thread helper: CreateThread + SetThreadPriority) ← `0x1106A0` | `0x110630` | An engine worker thread, probably an async loader. Its object is at `+0x420`. |
| `0x128750` and `0x139B10` (the latter is device/resolution init) | `0x1286E0` | A **vblank pump**. `D3DDevice_SetVerticalBlankCallback(0x128740)` signals event `0x30F54C`. The thread waits on it, increments `0x312620` and calls the hook at `*0x312618`. Frame timing depends on it, so `RECOMP_VBLANK` is required. |
| `0x1BB536` (CRT `_beginthreadex`, mis-split; no static callers) | `0x1BB4C8` `_threadstartex` | Likely unused. |
| `0x264279` (WMA media object init, WMADEC) ← `0x26453F` | `0x2640C9` | The WMA decoder worker, with a 0x2000 stack and 4 events. |

Interrupt and DPC contexts, which need their own host threads or safe-point delivery:

- D3D `CMiniport_InitHardware` (the GPU/vblank interrupt)
- DSOUND `0x24661B` and `0x24E258` (APU interrupts and DPCs)
- XPP `0x2A71FB` (the USB/OHCI interrupt)
- Kernel timers in DSOUND and XPP

The plan is to copy blinx2: one guest CPU (`RECOMP_GUEST_LOCK=1`), with interrupts posted at safe points.

## 5. Lessons from blinx2-recomp

What to copy:

- **Config.** Keep seeds, spin-waits and overrides as project config (`seeds/`, `recomp_manual.c` with `--exclude-manual`).
- **Overrides.** Define `void sub_XXXXXXXX(void)` that reads args from `MEM32(g_esp+4..)`, sets `g_eax` and pops stdcall args itself. To wrap generated code, call `sub_X_gen` through `RECOMP_ABI_CALL`.
- **Build.** Use a separate, lower optimisation level for `gen/`.
- **File I/O and memory.**
  - Clear XBE init flag `MOUNT_UTILITY_DRIVE`; otherwise XAPI drops to the dashboard.
  - Map `D:` to `game/` and `Z:` to the cache.
  - Use the 128 MB devkit RAM layout.
- **Input.** Override the 7 XInput entry points instead of emulating the USB stack. XbSymbol gives us `XInputGetState`, `XInputPoll` and the rest in XPP.
- **Video.** Let lifted guest code decode the movies (XMV here). Don't replace a decoder that works.
- **Testing.**
  - Golden frames: scripted input, exact SHA match, with a tolerant compare as fallback.
  - Keep a `deviations` record for every departure from hardware.
- **Order.** After adding a seed, re-run disasm and then recomp. Recomp alone misses new seeds.

Caveat: blinx2 builds against a fork (`apexJCL/xboxrecomp`, branch `blinx2/portability`). Check each runtime feature against upstream before relying on it.

## Reproduce

```bash
# survey (2 s)
.venv/Scripts/python.exe tools/p1_survey.py
# XDK names
toolkit/xbsymbol/win_x64/bin/XbSymbolDatabaseCLI.exe game/default.xbe -e > analysis/xbsymbol/xbsymbol.txt
# Ghidra: Ghidra 12 dropped Jython, so run the toolkit's ExportXbeNames.py under PyGhidra
# (the venv has pyghidra installed from Ghidra's bundled wheels; change "@runtime Jython" to "PyGhidra")
.venv/Scripts/python.exe -m pyghidra.ghidra_launch --install-dir <ghidra> ghidra.app.util.headless.AnalyzeHeadless \
  <xboxrecomp>/tools/ghidra_naming/work/ghidra_project game -import <xbe_flat.bin> -loader BinaryLoader \
  -loader-baseAddr 0x10000 -processor x86:LE:32:default -cspec windows -overwrite \
  -scriptPath "<pyghidra copy dir>;<xboxrecomp>/tools/ghidra_naming/ghidra_scripts" \
  -preScript SetAnalysisOptions.java -postScript ExportXbeNamesPy.py analysis/ghidra nodecompile
```

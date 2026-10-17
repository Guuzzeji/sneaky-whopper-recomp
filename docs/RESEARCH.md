# Sneak King Recomp: Research Report

This report is the background reading for the project. It explains how original Xbox (2001) games work at the binary level and how static recompilation turns one into a native PC program. It also explains how the `sp00nznet/xboxrecomp` toolkit does this, and what we have measured about *Burger King: Sneak King* so far.

Read it top to bottom once. After that, use it as a reference.

**Confidence markers:**
- Facts measured from our own disc have no marker.
- Facts from external sources have a link in section 11.
- Anything uncertain is marked **(uncertain)**.

---

## 1. Goal and approach

### What we want

We want *Sneak King* to run on a modern PC as an ordinary native executable:
- no emulator, no original console BIOS, no interpreter in the loop;
- the original game logic, translated into C and compiled by a modern compiler;
- portable in principle to any OS and CPU the C compiler targets.

### Emulation vs static recompilation

An **emulator** (xemu, Cxbx-Reloaded) runs the original program at runtime. It decodes the guest machine code as it goes, or JIT-compiles it, and it models the hardware. Emulators are general: one emulator runs many games. The cost is runtime overhead and a layer you can't easily change.

**Static recompilation** does the translation once, ahead of time:

1. Parse the executable (the XBE).
2. Find every function in its machine code.
3. Translate each instruction into equivalent C.
4. Compile that C with a normal compiler (MSVC, clang).
5. Link it against a *runtime library* that supplies everything the original program expected from the console: kernel calls, graphics, audio, input and files.

The result is a native program. You can debug it in Visual Studio, profile it, patch it with ordinary C, and port it.

Static recompilation is well proven on other consoles:
- **N64Recomp** (N64, MIPS) produced *Zelda 64: Recompiled*.
- **XenonRecomp** (Xbox 360, PowerPC) produced *Unleashed Recompiled*.
- **ps2recomp** targets the PS2.

The original Xbox is newer ground. Its CPU is x86, and x86 is much harder to translate correctly than the fixed-width RISC CPUs of those consoles (see section 4).

### The two halves of the work

1. **The lifter (CPU side).** It turns x86 machine code into C. Most of this is automatic. The rest is fixing the cases the tool can't figure out alone: missing functions, unknown indirect-call targets, odd instructions.
2. **The runtime (system side).** It replaces the Xbox kernel, the NV2A GPU, the audio processor, controllers and the DVD drive with PC equivalents. `xboxrecomp` ships a runtime. Our job is to find and fill the gaps that *Sneak King* hits.

In practice the project is a long loop: lift, build, run, crash, find out why, fix, repeat. The roadmap (`docs/ROADMAP.md`) breaks that loop into milestones.

---

## 2. Original Xbox hardware

| Part | Detail |
|---|---|
| CPU | Intel Pentium III-class, 733 MHz. 32-bit x86 with MMX and SSE1. **No SSE2.** x87 FPU. |
| GPU | NVIDIA NV2A, roughly GeForce3/GeForce4 Ti. Fixed-function T&L, programmable vertex shaders (NV2A microcode), register-combiner pixel shading. |
| RAM | 64 MB unified, shared by CPU and GPU. Debug kits and Chihiro arcade boards have 128 MB. |
| Audio | MCPX APU: a voice processor (256 hardware voices) plus Motorola 56300-family DSPs for effects (GP) and encoding (EP), and an AC'97 output. |
| Storage | DVD drive (game disc) plus an 8–10 GB hard disk with fixed partitions. |
| Input | Controllers on USB (through a proprietary connector), driven by the XAPI library. |

### Memory map (retail)

The Xbox runs one program at a time, with no process isolation in the PC sense. Game code, the kernel and the hardware registers all share one 32-bit address space.

| Address range | What lives there |
|---|---|
| `0x00000000`–`0x03FFFFFF` | 64 MB physical RAM, also visible at low virtual addresses |
| `0x00010000` | Typical XBE base address: where the game's image is loaded |
| `0x80000000`+ | Kernel-mode view of physical memory (contiguous allocations) |
| `0x80010000` | The kernel image (`xboxkrnl.exe`) always loads here |
| `0xF0000000`+ | Another mirror of RAM (used by the GPU/driver code) |
| `0xFD000000`–`0xFDFFFFFF` | NV2A GPU registers (MMIO) |
| `0xFE800000`–`0xFE87FFFF` | APU registers |
| `0xFEC00000` | AC'97 audio codec |
| `0xFED00000`, `0xFED08000` | USB host controllers |
| `0xFEF00000` | Network controller |
| `0xFF000000`+ | Flash ROM; the MCPX boot ROM occupies the top 512 bytes |

Two facts matter most for recompilation:

1. **The game is linked to a fixed address (`0x00010000`) and has no relocations.** Every pointer baked into the code assumes that layout. The runtime must recreate it, or translate every address.
2. **RAM appears at several addresses at once (mirrors).** Writing through one alias must be visible through all the others, because the D3D and kernel code really do use different aliases for the same bytes.

### The kernel

The Xbox kernel (`xboxkrnl.exe`) is a cut-down Windows 2000-style kernel. Games call it **by ordinal number only**, never by name.

- The xboxdevwiki table runs ordinals 1–378, with a few unused. The original retail kernel exported about 366.
- `xboxrecomp` routes 371 exports: 347 functions plus 24 data exports.
- Examples: `NtCreateFile` = 189, `IoCreateFile` = 65, `DbgPrint` = 8.
- All kernel functions use the `stdcall` calling convention.

---

## 3. Disc and XBE format

### The disc: XGD and XDVDFS

Retail Xbox discs use a format Microsoft calls **XGD** (Xbox Game Disc). A full disc dump, like ours from a redump-style rip, contains two partitions:

1. A small **video partition** at the start. It is a normal DVD-video area that tells a DVD player "this is an Xbox disc".
2. The **game partition**, in Microsoft's own filesystem, **XDVDFS**.

On our image the game partition starts at **byte offset `0x18300000`**, the standard XGD1 offset. At that partition's sector 32 (offset `+0x10000`) is the XDVDFS volume descriptor. It starts with the magic string `MICROSOFT*XBOX*MEDIA`, followed by the root directory's sector and size.

XDVDFS directories are stored as **binary trees**. Each entry holds:

```
uint16 left_subtree_offset   (in 4-byte units)
uint16 right_subtree_offset
uint32 start_sector          (2048-byte sectors, relative to partition start)
uint32 file_size
uint8  attributes            (0x10 = directory)
uint8  name_length
char   name[name_length]
```

An "XISO" file is the game partition alone, without the video partition. Extraction tools:
- `extract-xiso`
- `xdvdfs` (Rust; it also has a web GUI)
- `xboxrecomp`'s own `tools.xiso`, which handled our full redump directly

### The executable: XBE

An XBE ("Xbox Executable") is Microsoft's console-specific variant of a Windows PE file. Key fields in its image header:

| Offset | Field |
|---|---|
| `0x000` | Magic `XBEH` |
| `0x104` | Base address (normally `0x00010000`) |
| `0x108` | Size of headers |
| `0x10C` | Size of image |
| `0x118` | Address of the certificate |
| `0x11C` / `0x120` | Number of sections / address of section headers |
| `0x128` | Entry point, **XOR-encoded** |
| `0x12C` | TLS directory address |
| `0x130` | Stack commit size |
| `0x14C`–`0x154` | Debug path / filename pointers |
| `0x158` | Kernel thunk table address, **XOR-encoded** |
| `0x160` / `0x164` | Number of library versions / their address |

Every "address" field is a virtual address. To find it in the file, subtract the base address.

### XOR keys

Microsoft obfuscates the entry point and the kernel thunk address by XOR-ing them with fixed keys. Retail and debug builds use different keys:

| Field | Retail | Debug | Beta |
|---|---|---|---|
| Entry point | `0xA8FC57AB` | `0x94859D4B` | `0xE682F45B` |
| Kernel thunk | `0x5B6D40B6` | `0xEFB1F152` | `0x46437DCD` |

To decode, XOR with both keys and keep whichever result is a plausible address.

For *Sneak King*:
- The raw entry `0xA8FA6001` XOR the retail key gives `0x000637AA`.
- The raw thunk `0x5B47BB96` gives `0x002AFB20`.
- So it is a retail build.

### Kernel thunk table

Games never contain kernel code. They contain a **thunk table**: an array of 32-bit slots in `.rdata`, terminated by zero. On disc, each slot holds `0x80000000 | ordinal`. At load time the real kernel overwrites each slot with the address of that kernel function.

Game code then calls the kernel indirectly:

```asm
call dword ptr [0x002AFB24]   ; slot for KeQuerySystemTime (ordinal 128)
```

For a recompiler this table is the hook point. Bind each slot to our own C implementation and every kernel call in the game lands in our code.

### Certificate

The certificate holds:
- the **title ID**: two ASCII letters for the publisher plus a 16-bit number, e.g. `0x4D5300D3` is "MS" (Microsoft) title `0x00D3`;
- the title name (UTF-16);
- allowed media, region, ratings, disc number and version;
- signature keys.

The title ID matters because save data and cache folders are named after it.

### Library versions

Xbox games are statically linked. The Microsoft XDK libraries (D3D8, DirectSound, XAPI and others) are compiled *into* the XBE. The header lists each library with its name and XDK build number. This tells us exactly which version of Microsoft's code is inside, which is how signature databases know which byte patterns to look for (section 5).

### Sections

Like PE files, XBEs are divided into sections, each with a virtual address, size and flags. The flags are: writable, preload, executable, inserted file, head-page read-only. `.text`, `.rdata` and `.data` are always present. XDK libraries often add their own named sections (`D3D`, `DSOUND`, `XPP`, ...). Each section header also stores a SHA-1 digest used by the console's signature check.

---

## 4. Why x86 → C static recompilation is hard

Recompilers for MIPS (N64) and PowerPC (360) have it comparatively easy:
- Every instruction is exactly 4 bytes, so any aligned word is a candidate instruction.
- Calls are explicit (`jal`, `bl`).
- The 360 even ships `.pdata` tables that list every function's start and length.

x86 has none of that. Below, each problem is paired with how `xboxrecomp` addresses it.

### 4.1 Variable-length instructions

x86 instructions are 1 to 15 bytes long. Start decoding at the wrong byte and you get plausible-looking garbage that slowly resynchronises. You can't just sweep linearly through `.text`, because data sits inside it.

**How xboxrecomp handles it.** `tools.disasm` uses Capstone with **recursive descent** from known entry points: the XBE entry, call targets, and addresses found in data tables. It then adds a set of heuristic passes:

- functions after a `ret` with no padding;
- prologue patterns;
- tail-jump targets;
- addresses taken as immediates;
- function pointers in data tables (698 of them in our XBE).

### 4.2 Code and data mixed together

MSVC places **switch jump tables**, literal pools and alignment padding (`int3`, `nop`) inside `.text`. The XBE keeps no relocation table, so you can't tell for certain whether a 32-bit constant is a pointer or just a number.

**How xboxrecomp handles it.**
- It detects the common jump-table pattern (`cmp reg, N; ja default; jmp [table + reg*4]`), bounds the table, and emits a C `switch`.
- It misses some unusual forms, notably **negative-index tables** such as the CRT `memcpy`/`memmove` tail tables, which every MSVC title has. These need a manual override.
- If the lifted output contains privileged registers (`cr*`, `dr*`), that usually means data was decoded as code.

### 4.3 Indirect calls and jumps

C++ virtual calls (`call [ecx+0x10]`), callbacks and function pointers stored in data all compute their target at runtime. A static tool can't always know every possible target. The README calls this "the hardest 10%".

**How xboxrecomp handles it.** Every indirect call goes through a macro, `RECOMP_ICALL`. At runtime it looks up the guest target address in a sorted table of recompiled functions and calls the matching C function. If the target was never discovered, the runtime logs it. That log feeds the **icall feedback loop** (section 6.5): seed the missing addresses, lift again, repeat until the misses stop.

### 4.4 The x87 FPU

The x87 floating-point unit is a **stack** of eight 80-bit registers with a moving top-of-stack pointer, a tag word, and precision and rounding control bits. Direct3D on the Xbox typically sets **single precision** through the control word. So `a * b + c` on real hardware can give a different result from the same expression in C `double`.

**How xboxrecomp handles it.** It models the stack explicitly: `g_fp_stack[8]`, `g_fp_top` and `g_fp_control_word`. It honours precision and rounding control, lifting x87 operations to `double` values with explicit rounding. SSE1 code is lifted to `__m128` intrinsics, and MMX helpers are being converted to intrinsics too.

### 4.5 EFLAGS

Almost every x86 arithmetic instruction updates the flags (carry, zero, sign, overflow, parity, auxiliary). A later branch may test them far from where they were set, or instructions like `adc`, `sbb` and `setcc` may consume them. Computing every flag after every instruction in C would be extremely slow.

**How xboxrecomp handles it.** It records the *comparison* (operands and kind) at the instruction that sets the flags. It then evaluates only what the consuming branch needs. Unusual flag sequences may need manual fixes, and carrying flags across control-flow joins is still being improved upstream.

### 4.6 Calling conventions

32-bit Windows code mixes several calling conventions:

| Convention | Arguments | Who cleans the stack |
|---|---|---|
| `cdecl` | Stack | Caller |
| `stdcall` | Stack | Callee (`ret N`) |
| `fastcall` | ECX, EDX, then stack | Callee |
| `thiscall` | ECX = `this`, rest on stack | Callee |

Link-time optimisation can also produce custom conventions. If the stack cleanup is wrong even once, ESP drifts and the program fails later, far from the cause.

**How xboxrecomp handles it.** It doesn't try to recover real C signatures for execution. **Every lifted function is `void sub_XXXXXXXX(void)`.** Guest registers are global (thread-local) variables, and the guest stack is a real stack in emulated guest memory. Arguments and return values travel exactly as they did on the Xbox, so a convention mismatch can't break anything. `tools.abi_analysis` still recovers conventions and parameter counts, which helps naming and the hand-written overrides.

### 4.7 Absolute pointers and the fixed address space

The game assumes it lives at `0x00010000`. Its code is full of absolute addresses of globals, vtables and strings. The XDK's D3D and kernel code also touch fixed physical and MMIO addresses.

**How xboxrecomp handles it.**
- It reserves a 64 MB block with `CreateFileMapping` and maps **28 mirror views** of it at 64 MB strides, so every alias shares the same physical pages. (`VirtualAlloc` can't create aliases.)
- It tries to place guest memory at its original addresses (offset 0). If that fails, it uses an OS-chosen base and adds `g_xbox_mem_offset` to every access.
- The guest stack top is `0x00F7FFF0` and the heap starts at `0x00F80000`.
- MMIO accesses (GPU registers and so on) are caught and routed to the NV2A/APU models.

### 4.8 Things it can't do (yet)

- **Self-modifying code:** not supported.
- **Structured exception handling (SEH):** only partly bridged, through a vectored exception handler.
- **`fs:` segment overrides:** the docs say these are dropped (this may be stale).
- **Rare instructions:** these lift to a `/* mnemonic */` comment and need attention when found.

### Two possible strategies, and which one we use

| Strategy | How | Pros | Cons |
|---|---|---|---|
| Run original x86 natively | Map the XBE at `0x10000` in a 32-bit Windows process, patch kernel thunks and HLE calls, execute directly (Cxbx-Reloaded style). | Fast, no lifting errors. | Tied to 32-bit x86 Windows. MMIO needs fault handling. Hard to modify. |
| **Lift to C** (xboxrecomp) | Translate everything to C over a simulated register file and guest memory. | 64-bit, portable (Linux/macOS/arm64), debuggable, patchable in C. | Every memory access is indirected. Flags and x87 are emulated. Lifter bugs are possible. |

We use the second. `xboxrecomp` guards its lifter with a **conformance harness**: lifted C runs side by side with the real CPU on the same instruction vectors. It reports 6,351 vectors with 0 mismatches, and we reproduced that number locally.

---

## 5. HLE vs LLE

Once the CPU code runs, the next question is what to do when the game talks to hardware. There are two broad philosophies.

- **HLE (high-level emulation):** recognise *what the game is trying to do* and do it the PC way. For example, "the game called `IDirect3DDevice8::DrawIndexedPrimitive`" becomes a D3D11 draw call.
- **LLE (low-level emulation):** let the game's own driver code run and emulate the *hardware* it talks to. For example, parse the GPU command stream the game wrote.

### 5.1 Finding library functions: XbSymbolDatabase and OOVPA

Because XDK libraries are statically linked, Microsoft's D3D8 code is inside our XBE as anonymous machine code. HLE needs to find it.

Cxbx-Reloaded solved this with **OOVPA** ("Optimized (Offset, Value)-Pair Array") signatures:
- Each library function is described by a handful of `(offset, byte)` pairs that are stable for a given XDK build.
- The library version table in the XBE header selects which signature set to try.
- These signatures now live in **XbSymbolDatabase**, which Cxbx and the Ghidra XBE loader (`ghidra-xbe`) both use.

*Sneak King* was built with **XDK 5849**, the final XDK, which these databases cover best. `xboxrecomp`'s own `tools.func_id` takes a different, heuristic approach: CRT signatures, strings, vtables, callers of XDK code. Adding XbSymbolDatabase results is a Phase 1 task.

### 5.2 Graphics: D3D8 HLE vs NV2A pushbuffer executor

On the Xbox, Direct3D 8 is a thin layer. It doesn't call a driver; it writes **pushbuffers** (GPU command streams) straight into memory and kicks the NV2A to read them. A recomp has two choices.

1. **D3D8 HLE** (`xbox_d3d8` in xboxrecomp): replace D3D8 API functions with host implementations.
   - It translates D3D8 to D3D11 (or OpenGL 3.3).
   - It handles fixed-function state, register combiners, swizzled textures, and NV2A vertex-shader microcode compiled to HLSL.
   - It's fast, but it only works if the game's D3D calls can be intercepted.
2. **NV2A pushbuffer executor** (`xbox_nv2a` plus the kernel's `nv2a_pb_exec.c`): let the game's own statically linked D3D code run unmodified, then parse the pushbuffers it writes and drive the host GPU from that PGRAPH state.
   - This is closer to what xemu does, and it's where most recent upstream work is concentrated.
   - It's slower: Burnout 3 on the software path runs at about 2–4 fps in races.

*Sneak King* links D3D8 statically (a `D3D` section, 715 functions), so the pushbuffer path is the likely one. The final choice, or a hybrid, is a Phase 1 decision.

### 5.3 Audio: DirectSound and the APU

The same choice applies to audio.

- **DirectSound HLE** (`xbox_dsound`): DirectSound calls go to a software mixer, and there is also a WMA decoder.
- **APU LLE** (`xbox_apu`): xemu's MCPX APU emulation, output through XAudio2 or SDL2. It's needed when the game's DSOUND library drives the voice processor directly.

*Sneak King* has `DSOUND` (729 functions), `WMADEC` and `DOLBY` sections, so expect to exercise both.

FMV uses Microsoft's **XMV** format (`XMV` section, 71 functions). The runtime plays these through Media Foundation (`xbox_video`), and `tools/xmv` can demux them.

### 5.4 Kernel HLE

The kernel side is pure HLE. Each of the kernel exports is reimplemented on top of Win32 or POSIX:

| Area | Examples |
|---|---|
| Memory | `MmAllocateContiguousMemory`, `NtAllocateVirtualMemory` |
| Threads | `PsCreateSystemThreadEx`, `KeWaitForSingleObject` |
| Synchronisation | `KEVENT`, `KTIMER` and DPC objects |
| I/O | `NtCreateFile`, `NtReadFile`, `IoCreateSymbolicLink` |
| HAL | Hardware abstraction calls |
| EEPROM | `ExQueryNonVolatileSetting` (language, video mode, ...) |
| Crypto | `XcSHA*`, `XcHMAC` |

Synchronisation objects live in *guest* memory, so the runtime keeps a shadow table that maps each guest object to a host handle.

### 5.5 Drive letters and paths

Xbox games open files through NT device paths and drive letters:

| Xbox path | Meaning | xboxrecomp maps it to |
|---|---|---|
| `D:\`, `\Device\CdRom0\` | Game disc | `<game_dir>/` (our extracted `game/`) |
| `E:\` | HDD Partition1 (system/data) | — |
| `C:\` | HDD Partition2 (dashboard) | — |
| `T:\` | `E:\TDATA\<TitleID>` (title persistent data) | `<save_dir>/TitleData` |
| `U:\` | `E:\UDATA\<TitleID>` (user saves) | `<save_dir>/UserData` |
| `Z:\` | Per-title cache partition (X/Y/Z = Partitions 3–5) | `<save_dir>/Cache` |

### 5.6 Save format

Each save game is a folder under `UDATA\<TitleID>\`, holding metadata files:
- `SaveMeta.xbx`
- `TitleMeta.xbx`
- `TitleImage.xbx` (UTF-16 metadata and icons)

Save data may be signed with `XCalculateSignature`, an HMAC-SHA1 keyed from the certificate's signature key and a console key. A recomp can stub the verification. **(uncertain: Sneak King's exact save layout is unexamined.)**

---

## 6. How xboxrecomp works

`xboxrecomp` (MIT licence; `src/apu/` and `src/nv2a/nv2a_regs.h` are LGPL-2.1+ code from xemu) has two parts:
- a **Python pipeline** in `tools/` that turns an XBE into C;
- a set of **C11 runtime static libraries** in `src/` that the generated C links against.

We have v0.14.0 "Other Machines" (Oct 2026), cloned at `C:\repos\xboxrecomp`.

### 6.1 Runtime libraries

| Library | Directory | Role |
|---|---|---|
| `xbox_kernel` | `src/kernel/` | Memory layout and mirrors, all kernel exports (`kernel_bridge.c`), paths, threads, sync, crypto, HAL, the NV2A pushbuffer executor |
| `xbox_d3d8` | `src/d3d/` | D3D8 to D3D11 (OpenGL 3.3 back end in `d3d8_gl.c`) |
| `xbox_nv2a` | `src/nv2a/` | NV2A registers, MMIO hooks, PGRAPH to D3D11 |
| `xbox_apu` | `src/apu/` | xemu's MCPX APU; XAudio2/SDL2 output |
| `xbox_dsound` | `src/audio/` | DirectSound to software mixer, WMA decoder |
| `xbox_input` | `src/input/` | XInput, keyboard, mouse |
| `xbox_usb` | `src/usb/` | OHCI USB host emulation, for XAPI that drives USB hardware directly |
| `xbox_video` | `src/video/` | Media Foundation FMV, framebuffer window |

A game repo pulls these in through CMake: `add_subdirectory(${XBOXRECOMP_DIR})`, then links the umbrella target `xboxrecomp`. `XBOXRECOMP_DIR` defaults to `../xboxrecomp`, which is why the toolkit sits next to this repo.

### 6.2 The pipeline, step by step

All commands run **from the toolkit directory** (`C:\repos\xboxrecomp`) with our venv's Python. The paths below are the ones this project uses.

```powershell
$py = 'C:\repos\sneak-king-recomp\.venv\Scripts\python.exe'
$G  = 'C:\repos\sneak-king-recomp'

# 1. Extract the disc's game partition (handles the full redump image)
& $py -m tools.xiso unpack $G\sneak-king-usa-dump-10-16-2026.iso -o $G\game

# 2. Parse the XBE header; the JSON must sit next to the XBE
& $py -m tools.xbe_parser $G\game\default.xbe --json $G\game\default_analysis.json

# 3. Disassemble and find functions (about 8 minutes on this XBE)
& $py -m tools.disasm $G\game\default.xbe -o $G\analysis\disasm

# 4. Classify functions (CRT, vtables, render/audio/input...)
& $py -m tools.func_id $G\game\default.xbe `
    --functions $G\analysis\disasm\functions.json `
    --strings   $G\analysis\disasm\strings.json `
    --xrefs     $G\analysis\disasm\xrefs.json -o $G\analysis\func_id

# 5. Recover calling conventions / parameter counts
& $py -m tools.abi_analysis $G\game\default.xbe `
    --disasm-dir $G\analysis\disasm --func-id-dir $G\analysis\func_id `
    --output-dir $G\analysis\abi

# 6. (optional) Ghidra name recovery: tools/ghidra_naming/

# 7. Lift to C (Phase 2)
& $py -m tools.recomp $G\game\default.xbe --all --split 250 `
    --disasm-dir $G\analysis\disasm --func-id-dir $G\analysis\func_id `
    --abi-dir $G\analysis\abi --gen-dir $G\src\recomp\gen
```

Then build the game executable from a Visual Studio developer shell:

```bat
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```

Keep build parallelism low. The generated C files are huge, and the docs report that 1,000 functions per file once ran a 15 GB machine out of memory. That is why we use `--split 250`.

At startup, `src/main.c` (copied from `templates/new-game` and filled in for Sneak King):

1. installs a vectored exception (crash) handler;
2. loads `game\default.xbe`;
3. sets up the memory layout (`xbox_MemoryLayoutInit`);
4. initialises the kernel (`xbox_kernel_init`);
5. sets up the path mapping (`xbox_path_init("game", ...)`);
6. initialises the kernel bridge, writing thunk slots into guest memory;
7. sets `g_esp = XBOX_STACK_TOP`;
8. initialises the dispatch table (`recomp_dispatch_init`);
9. calls the recompiled entry point, `0x000637AA`.

### 6.3 The register model

Every lifted function looks roughly like this:

```c
void sub_000637AA(void) {
    uint32_t ebp;                 /* ebp is a per-function local */
    g_esp -= 4; MEM32(g_esp) = g_ebx;          /* push ebx */
    g_eax = MEM32(0x002E379C);                 /* mov eax, [global] */
    ...
    RECOMP_ICALL(MEM32(g_ecx + 0x10));         /* call [ecx+10h] */
}
```

- Guest registers `g_eax`, `g_ecx`, `g_edx`, `g_ebx`, `g_esi`, `g_edi` and `g_esp` are **thread-local globals**, declared with `RECOMP_TLS`. So are the x87 stack, MXCSR and the XMM registers.
- Memory goes through `MEM8/16/32/MEMF` macros built on `XBOX_PTR(addr) = (uintptr_t)(uint32_t)addr + g_xbox_mem_offset`.

### 6.4 Dispatch: `RECOMP_ICALL`

An indirect call or jump resolves its guest target in this order:

1. `recomp_lookup_manual`: our hand-written overrides in `recomp_manual.c`;
2. `recomp_lookup`: binary search over all generated functions;
3. `recomp_lookup_kernel`: kernel thunks, which get synthetic addresses `0xFE000000` and up.

### 6.5 Per-game configuration and the icall feedback loop

There is no per-game config file (no TOML). Per-game knowledge lives in:

| Mechanism | Purpose |
|---|---|
| `--seed-functions icall_seeds.json` (to `tools.disasm`) | Extra function entry points that were *measured* at runtime |
| `--manual-functions`, `--exclude-manual src/recomp_manual.c` | Functions we write by hand; the lifter skips them |
| `--coalesce-functions` | Merge functions the disassembler wrongly split |
| `--icall-sites` | Per-call-site indirect target lists |
| `--trace-functions` | Emit entry traces for chosen functions |
| `--force-return ADDR=VALUE` | Stub a function to return a constant |
| `--seh-prolog` / `--seh-epilog` | Locations of the SEH helper functions (normally auto-detected) |
| `src/recomp_manual.c` | Hand-written C overrides |

The **icall feedback loop** is how indirect-call coverage converges:

1. Build with `RECOMP_ICALL_FEEDBACK` defined and run the game until it misbehaves.
2. Merge the recorded targets: `python -m tools.recomp.icall_feedback ... merge icall.txt`.
3. Turn them into seeds: `... seeds --out icall_seeds.json --align 16`.
4. Rerun `tools.disasm --seed-functions icall_seeds.json`, then lift and build again.

Only feed *measured* targets back as seeds. The `--help` text warns that addresses inferred by `func_id` can sit inside existing functions and wrongly truncate them.

### 6.6 Limitations and the state of other titles

As of v0.14.0 the toolkit's README says **no Xbox title is fully playable** through the pure recompiled path. Status of the related game repos:

| Title | Repo | Status |
|---|---|---|
| Burnout 3 | `sp00nznet/burnout3` | 22,097 functions. The recompiled path reaches engine/RenderWare init. The old "playable to menu" claim was withdrawn because it relied on hand-written scaffolding. |
| Wreckless | `sp00nznet/wreckless` | 3,407 functions. Boots into game main; an early-gameplay crash is under investigation. |
| Bloodwake | `sp00nznet/bloodwake` | 4,608 functions, 99.1% lifted. Scaffolded. |
| Xbox Dashboard 3944 | `sp00nznet/xboxdashboard` | Nothing renders yet. |
| Half-Life 2 | `sp00nznet/hl2-recomp` | Active. |
| BLiNX 2 | `apexJCL/blinx2-recomp` (separate project) | **Playable** on Steam Deck and macOS with sound and saves. Uses a single guest-thread lock, golden-frame tests and llvm-mingw. The best example of a finished OG Xbox recomp. |

Issues also mention Jet Set Radio Future, Black (stalls before the first Present), SSX 3 and a Halo beta. Expect per-game patches; Burnout 3 needed "10+ patch locations across 8 generated files".

*Sneak King*, at 13,318 functions, is mid-sized among these, and simpler than Burnout 3.

---

## 7. Why the OG Xbox build and not the Xbox 360 XEX

The *Sneak King* disc is a **hybrid**. It carries an OG Xbox build (`default.xbe`) *and* a separate native Xbox 360 build (`default.xex`). The 360 build is not backward-compatibility emulation; it is its own executable with better graphics. That raised a real choice between `xboxrecomp` (OG) and `hedge-dev/XenonRecomp` (360).

| | OG Xbox (`default.xbe` + xboxrecomp) | Xbox 360 (`default.xex` + XenonRecomp) |
|---|---|---|
| CPU lifting | Harder (x86: variable length, no function table, flags, x87) | Proven (PowerPC: fixed width, `.pdata` function table) |
| Runtime (GPU, audio, kernel) | Included in the toolkit, not yet proven end to end | Mostly do-it-yourself, or ReXGlue SDK (maturity unchecked) |
| Hardware to emulate | Small: 1 core, 64 MB, little-endian | Big: 3 cores / 6 threads, VMX128, Xenos eDRAM, XMA audio, big-endian |
| Binary prep | Plain XBE | XEX is encrypted and compressed; may need a title update applied |
| Result quality | Lower-resolution build | HD build |
| Track record | No title fully playable through xboxrecomp yet; BLiNX 2 proves OG recomps can finish | Unleashed Recompiled is a shipped, fully playable result |

**Decision: OG Xbox only.**
- The game is small, and the toolkit covers the whole stack, so every gap is a known, scoped job: an unbridged kernel call, a missing function, a GPU state bug.
- On the 360 side, the CPU lifter is proven, but we would have to build or borrow the entire runtime.
- The 360 XEX stays available as a **reference**. The two builds share the BlitzTech engine, so names and structures recovered from one can help map the other.

---

## 8. Sneak King: lay of the land

All facts in this section were measured on our dump.

### 8.1 Disc

- The image is `sneak-king-usa-dump-10-16-2026.iso`, 7,825,162,240 bytes, a full redump-style image.
- The game partition is at offset `0x18300000` (XGD).
- Game partition root:

| Entry | Notes |
|---|---|
| `default.xbe` | 2,998,272 bytes. **Our target.** |
| `default.xex` | 2,387,968 bytes. The Xbox 360 build (reference only). |
| `update.xbe` | 2,342,912 bytes. **(uncertain)** Likely an Xbox dashboard/system update stub. |
| `dashupdate.xbe` | 58,421,248 bytes. **(uncertain)** Likely the 360 dashboard update package for the hybrid disc. |
| `movies/` | FMV files (XMV) |
| `Packages/` | Game data packages |
| `$SystemUpdate/` | 360 system update |

- Extracted with `tools.xiso unpack`: **143 files, about 1.55 GB**, into `game/` (git-ignored).

### 8.2 `default.xbe` header

| Field | Value |
|---|---|
| SHA-1 | `0B174AD67451D29FDC39DA1A5E6DBA35FF0F0C13` |
| Title | Burger King: Sneak King |
| Title ID | `0x4D5300D3` |
| Type | Retail |
| Region | North America |
| Media | DVD_X2 |
| Build date | 2006-09-13 09:46:30 UTC (certificate 2006-09-25) |
| Debug path | `c:\Projects\SneakKing\Code\Binaries\xbox\default.exe` |
| Base address | `0x00010000` |
| Image size | `0x00330600` (3.19 MB) |
| Entry point | `0x000637AA` (raw `0xA8FA6001`) |
| Kernel thunks | `0x002AFB20` (raw `0x5B47BB96`), the first bytes of `.rdata` |
| TLS | Index at `0x002E379C`, 144 bytes zero-fill, no TLS data template |
| Stack commit | `0xFFFF` |
| Heap | reserve `0x100000`, commit `0x1000` |

### 8.3 Sections

| # | Name | Virtual address | Virtual size | Flags | Contents |
|---|---|---|---|---|---|
| 0 | `.text` | `0x00011000` | `0x001DB5BC` (1.86 MB) | preload, exec, head-RO | Game code (BlitzTech engine + game) and CRT |
| 1 | `D3D` | `0x001EC5C0` | `0x00023580` | write, preload, exec | XDK Direct3D 8 |
| 2 | `XGRPH` | `0x0020FB40` | `0x0002D010` | write, preload, exec | XDK XGraphics helpers |
| 3 | `DSOUND` | `0x0023CB60` | `0x0002648C` | write, preload, exec | XDK DirectSound |
| 4 | `WMADEC` | `0x00263000` | `0x0001B2D4` | write, preload, exec | WMA audio decoder |
| 5 | `XMV` | `0x0027E2E0` | `0x000282CC` | write, preload, exec | XMV movie player |
| 6 | `XPP` | `0x002A65C0` | `0x00009550` | write, preload, exec | XAPI peripheral (USB/controller) code |
| 7 | `.rdata` | `0x002AFB20` | `0x0001FF2C` | preload, exec | Read-only data, kernel thunk table, vtables |
| 8 | `.data` | `0x002CFA60` | `0x00066FDC` | write, preload, exec | Globals (raw size only `0x1325C`; the rest is BSS) |
| 9 | `DOLBY` | `0x00336A40` | `0x00007180` | preload, exec | Dolby audio DSP code/data (no functions found) |
| 10 | `.data1` | `0x0033DBC0` | `0x000000E0` | write, preload, exec | Small data |
| 11 | `$$XTIMAGE` | `0x0033DCA0` | `0x00002800` | inserted file | Title image for the dashboard |
| 12 | `sig` | `0x003404A0` | `0x00000150` | write, preload, inserted | Signature data |

### 8.4 Libraries

Every library is **XDK build 5849** (version 1.0.5849):

| Library | Purpose |
|---|---|
| `XAPILIB` | Xbox API (the Win32-like layer: files, threads, controllers) |
| `D3D8` | Direct3D 8 |
| `XBOXKRNL` | Kernel import library |
| `LIBCMT` | Microsoft C runtime (multithreaded) |
| `XGRAPHC` | XGraphics helpers |
| `DSOUND` | DirectSound |
| `XVOICE` | Voice communicator |
| `XMV` | Xbox Media Video player |

### 8.5 Kernel imports

- **160 kernel imports.** All of them are routed by the xboxrecomp kernel.
- 156 are functions bridged in `kernel_bridge.c`.
- The 4 data exports (`ExSemaphoreObjectType`, `ExMutantObjectType`, `ExTimerObjectType`, `IoFileObjectType`) are handled in `kernel_thunks.c`.
- No kernel import is missing at link level. Whether every bridge *behaves* correctly for this game is what Phase 3 finds out.

### 8.6 Disassembly (`tools.disasm`, all executable sections)

| Metric | Value |
|---|---|
| Instructions | 953,063 |
| Functions | 13,318 |
| ...with a classic prologue | 1,476 |
| Cross-references | 139,111 |
| ...calls | 30,763 |
| ...jumps / conditional jumps | 13,579 / 63,826 |
| ...data reads / immediates | 19,575 / 10,181 |
| ...kernel calls | 1,187 |
| Function addresses found in data tables | 698 |
| Function addresses taken as immediates | 84 |
| Strings | 1,635 |

Functions per section:

| `.text` | `D3D` | `XGRPH` | `DSOUND` | `WMADEC` | `XMV` | `XPP` | `DOLBY` |
|---|---|---|---|---|---|---|---|
| 10,554 | 715 | 786 | 729 | 196 | 71 | 267 | 0 |

How the functions were found:

| Method | Count |
|---|---|
| Calling-convention boundary | 6,342 |
| Direct call target | 4,521 |
| Prologue | 1,396 |
| Tail-jump alias | 613 |
| Gap prologue | 310 |
| Immediate reference | 84 |
| Tail-jump target | 45 |
| Indirect-call slot | 6 |
| Entry point | 1 |

The large share found only by boundary heuristics, and the 698 data-table pointers, show how much of this engine is reached through **vtables and function pointers**. Expect the icall feedback loop to matter.

### 8.7 Function classification (`tools.func_id`)

- 15,123 entries (this count includes vtable/alias entries beyond the 13,318 disassembled functions).
- **52.5% classified, 47.4% unknown.**
- **0 RenderWare.** This is BlitzTech, not RenderWare like Burnout 3, so the RenderWare-specific tooling doesn't apply.
- By category:

| Category | Count |
|---|---|
| vtable methods | 5,851 |
| render | 1,036 |
| audio | 730 |
| input | 182 |
| I/O | 37 |
| UI | 31 |
| video | 29 |
| debug | 18 |
| camera | 14 |
| CRT (by signature) | 11 |
| physics | 8 |
| vehicle | 3 |
| network | 2 |

- By method: 3,476 vtable scan, 1,939 XDK caller, 1,805 vtable thunk, 570 vtable constructor, 151 string keyword, 11 CRT signature.

### 8.8 ABI analysis

- 13,318 ABI entries.
- **2,345 functions detected as `thiscall`.** This is consistent with a C++ engine with many classes: BlitzTech.

### 8.9 What this tells us

1. **The game is a mid-sized C++ engine on top of a fully static XDK 5849.** About 2,800 of the functions are XDK library code (D3D, XGRPH, DSOUND, WMADEC, XMV, XPP), which signature databases can name.
2. **Graphics will probably go through the NV2A pushbuffer executor**, because D3D8 is linked in and writes pushbuffers itself. Phase 1 should confirm this by finding the D3D entry points (`D3DDevice_*`) and deciding whether to HLE-patch them or let them run.
3. **Indirect calls are the main CPU-side risk** (vtables everywhere).
4. **Kernel coverage is complete on paper.**
5. **Media to handle:** XMV movies, WMA audio, Dolby.

---

## 9. Environment notes

The state of this machine after Phase 0 setup:

- **Compiler:** Visual Studio 2022 Community at `C:\Program Files\Microsoft Visual Studio\2022\Community`, with the Desktop C++ workload (MSVC v143, Windows SDK, CMake and Ninja bundled). Use a developer shell (`vcvars64.bat`) so `cl` and `ninja` are on PATH.
- **Python:** The system Python 3.14.7 is managed by `uv` and refuses `pip install` (PEP 668, "externally-managed-environment"). The project uses a **venv at `.venv/`** (git-ignored) containing:
  - capstone 5.0.7
  - pytest
  - pefile
  - numpy (pefile and numpy are needed by `tools/fusion` tests)
- **Toolkit:** `C:\repos\xboxrecomp`, v0.14.0 (commit `eb0aa1d`). Libraries build in Release with Ninja + MSVC.
- **Toolkit tests:**
  - Without MSVC on PATH: 711 passed, 98 skipped, 3 failed.
  - Under `vcvars64`: `fpdiff` and the fuzz test pass, and the conformance run reports **6,351 vectors, 0 mismatches**.
  - The conformance *corpus* stage fails with `PermissionError: [WinError 5] Access is denied` when it launches the freshly built `corpus_harness.exe` from `%TEMP%`. This machine blocks running new binaries from Temp. Redirecting `TMP`/`TEMP` to `C:\repos\xboxrecomp\build\conftmp` fixes it: the full suite then gives **805 passed, 7 skipped, 0 failed**. The command is in `CLAUDE.md`.
- **Ghidra:** 12.1.4 at `C:\Users\SandGames\Desktop\ghidra_12.1.4_PUBLIC_20260921` (JDK 25 installed). The `ghidra-xbe` loader is not installed yet (Phase 1).
- **xemu:** installed through winget for reference comparison. It needs **your own** console BIOS and MCPX boot ROM dumps, plus an HDD image, to run. Nothing in this repo provides those.
- **Legal hygiene:** the ISO, extracted `game/`, `analysis/` and generated `src/recomp/gen/` are all git-ignored. Never commit or upload game data or derived disassembly.

---

## 10. Glossary

| Term | Meaning |
|---|---|
| **APU** | Audio Processing Unit inside the MCPX chip |
| **BlitzTech** | Blitz Games' in-house C++ engine, which Sneak King uses |
| **Capstone** | Open-source disassembly library used by `tools.disasm` |
| **cc_boundary** | Function found by a calling-convention/boundary heuristic, not a direct call |
| **Conformance harness** | Test that runs lifted C against the real CPU on the same inputs |
| **Cxbx-Reloaded** | HLE OG Xbox emulator; source of the OOVPA idea |
| **EFLAGS** | x86 status flags register (carry, zero, sign, overflow, ...) |
| **FMV** | Full-motion video (pre-rendered movies) |
| **HLE** | High-level emulation: replace *library calls* with host equivalents |
| **icall** | Indirect call (target computed at runtime) |
| **Kernel thunk** | Table slot holding a kernel ordinal, patched to a function address at load |
| **Lifting** | Translating machine code into a higher-level language (here C) |
| **LLE** | Low-level emulation: emulate the *hardware* the game's own code drives |
| **MCPX** | Xbox southbridge chip (audio, USB, network, boot ROM) |
| **MMIO** | Memory-mapped I/O: hardware registers accessed as memory addresses |
| **NV2A** | The Xbox GPU |
| **OOVPA** | "Optimized (Offset, Value)-Pair Array": a byte-pattern signature for an XDK function |
| **Ordinal** | Number identifying a kernel export (no names are used) |
| **PGRAPH** | The NV2A's graphics engine block and its state |
| **Pushbuffer** | GPU command stream written into memory for the NV2A to consume |
| **Recomp** | Static recompilation |
| **thiscall** | MSVC C++ method convention: `this` in ECX, callee cleans stack |
| **TLS** | Thread-local storage |
| **x87** | Legacy x86 floating-point unit with a register stack |
| **XBE** | Xbox Executable |
| **XDK** | Xbox Development Kit (Microsoft's SDK); build 5849 is the final one |
| **XDVDFS** | Xbox DVD filesystem |
| **XEX** | Xbox 360 executable |
| **XGD** | Xbox Game Disc physical layout |
| **XISO** | Image of just the XDVDFS game partition |
| **XMV** | Xbox Media Video format |
| **XbSymbolDatabase** | Library of OOVPA signatures for XDK functions |
| **xemu** | LLE OG Xbox emulator; our reference for "what should this look like" |

---

## 11. Suggested reading order and sources

### Reading order

1. **This report**, sections 1, 4 and 6.
2. xboxrecomp `README.md`, `docs/GETTING_STARTED.md`, `docs/pipeline/01-06`, `docs/technical/memory-layout.md`, all in `C:\repos\xboxrecomp\docs`.
3. The **BLiNX 2 recomp README**, to see how a finished OG Xbox recomp was structured and tested.
4. **xboxdevwiki**: the XBE, Kernel and Memory pages.
5. The **Burnout 3** repo, for a real per-game layout (seeds, `main.c`, overrides).
6. **XenonRecomp's README**, for comparison: switch tables and mid-asm hooks in a 360 recomp.
7. The Cxbx-Reloaded OOVPA wiki page, before Phase 1 library identification.

### Sources

**xboxrecomp and game repos**
- xboxrecomp: https://github.com/sp00nznet/xboxrecomp
- Burnout 3 recomp: https://github.com/sp00nznet/burnout3 (README: https://raw.githubusercontent.com/sp00nznet/burnout3/HEAD/README.md)
- Other sp00nznet titles: https://github.com/sp00nznet (`wreckless`, `bloodwake`, `xboxdashboard`, `hl2-recomp`)
- BLiNX 2 recomp: https://github.com/apexJCL/blinx2-recomp (README: https://raw.githubusercontent.com/apexJCL/blinx2-recomp/HEAD/README.md)
- Korben article on xboxrecomp: https://korben.info/en/xboxrecomp-turn-xbox-game-into-windows-exe.html

**Hardware and formats**
- xboxdevwiki, XBE: https://xboxdevwiki.net/Xbe
- xboxdevwiki, Memory: https://xboxdevwiki.net/Memory
- xboxdevwiki, Kernel: https://xboxdevwiki.net/Kernel
- xboxdevwiki, Hard Drive: https://xboxdevwiki.net/Hard_Drive
- pyxbe: https://pyxbe.readthedocs.io
- extract-xiso: https://github.com/xboxdev/extract-xiso
- xdvdfs: https://github.com/antangelo/xdvdfs
- ConsoleMods, Xbox / Xbox 360 hybrid discs: https://consolemods.org/wiki/Xbox_/_Xbox_360_hybrid_discs
- ConsoleMods, ISO extraction: https://consolemods.org/wiki/Xbox_360:ISO_Extraction

**Emulators and HLE**
- Cxbx-Reloaded: https://github.com/Cxbx-Reloaded/Cxbx-Reloaded
- OOVPA maintenance guide: https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/wiki/Maintaining-OOVPAs-for-HLE-function-detection
- XbSymbolDatabase: https://github.com/Cxbx-Reloaded/XbSymbolDatabase
- xemu: https://github.com/xemu-project/xemu (compatibility list: https://xemu.app/#compatibility)
- Cxbx-Reloaded game compatibility: https://github.com/Cxbx-Reloaded/game-compatibility
- ghidra-xbe: https://github.com/XboxDev/ghidra-xbe
- nxdk: https://github.com/XboxDev/nxdk

**Other recomp projects**
- XenonRecomp: https://github.com/hedge-dev/XenonRecomp
- N64Recomp: https://github.com/N64Recomp/N64Recomp
- RexGlue SDK: https://github.com/rexglue/rexglue-sdk
- Time Extension interview on ReXGlue: https://www.timeextension.com/news/2026/03/i-hold-my-work-to-a-strict-standard-the-driving-force-behind-xbox-360-recomp-tool-rexglue-speaks-out

**The game**
- Sneak King on Wikipedia: https://en.wikipedia.org/wiki/Sneak_King
- Giant Bomb: https://www.giantbomb.com/games/3030-20670/
- Game Developer, "Burger King and serious advergaming": https://www.gamedeveloper.com/design/burger-king-and-serious-advergaming

Released November 2006, sold only at Burger King, developed by Blitz Games on BlitzTech; over 2.7 million copies sold across the promotion.

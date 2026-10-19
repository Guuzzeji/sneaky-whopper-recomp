# Skate 3 notes

Facts gathered from the user's Skate 3 disc (Xbox 360, NTSC-U) for the skate joke mode. These are findings only; no Skate 3 code or data is committed. The dump and everything extracted from it are gitignored (`*.iso`, `skate3/`, `game/`, `analysis/`).

## Disc and executable

| Item | Value |
|---|---|
| ISO | Your own *Skate 3* (USA, 360) ISO in the repo root. Its XDVDFS partition starts at `0x0FD90000`. `tools.xiso` reads it. |
| Unpacked | `skate3/` (103 files, 6.0 GB): `.\.venv\Scripts\python.exe -m tools.xiso unpack <iso> -o skate3`, run from the toolkit. |
| `default.xex` | 6,615,040 bytes, SHA-1 `2f1e63fbdfc90a30381328c428437747ee91295b`. Title ID `0x454108E6`, media ID `0x5C087C2C`, version 3 / base version 3. Original name `sk83_na_f.self`, PE timestamp `0x4BAEE107` (2010-03-28). |
| Title update | The disc build is unpatched. skate3recomp targets TU3 (`TU_12K2276_000000C000000.00000000000O3`), which isn't on the disc. The base image is fine for research. |
| Encryption | Retail AES, normal (LZX) compression, window from the file-format header. |

### XEX to PE
`tools/xex_to_pe.py` turns the XEX into its PE image:
1. It decrypts the session key with the retail key (AES-ECB), then the basefile (AES-CBC, zero IV). Both go through Windows bcrypt.
2. It de-chunks the SHA-1-chained blocks and runs LZX through `build\lzx_cli.exe`. That is libmspack's `lzxd`, built from `tools/lzx_cli.c`; the build command is in the file header.
3. It rewrites each section's raw pointer to its VA, because the image is memory layout.

```
.venv\Scripts\python.exe tools\xex_to_pe.py skate3\default.xex -o analysis\rea\skate3_default.exe
```

Result: a 0x1130000-byte image, base `0x82000000`, entry `0x82EB4F80` (`mflr r12`). The machine is `0x1F2` (PowerPC big-endian).

| Section | VA | Size |
|---|---|---|
| `.rdata` | `0x82000400` | `0x32A204` |
| `.pdata` | `0x8232A800` | `0x41BA8` |
| `.text` | `0x82370000` | `0xC034D4` |
| `.data` | `0x82F80000` | `0x112DF8` |
| `.tls`, `.XBMOVIE`, `.edata`, `.idata`, `.XBLD`, `.reloc` | `0x83092E00` onward | |

### Analysis tooling
- **REA can't open it.** It rejects COFF machine `0x1F2`, and its raw mode has no PowerPC.
- **Ghidra 12's PE loader** has no opinion for machine 498 either.
- `tools/skate3_ghidra.py` needs PyGhidra and `GHIDRA_INSTALL_DIR` set to a Ghidra 12 install. It loads the flat image with the BinaryLoader as `PowerPC:BE:64:A2ALT-32addr`, rebased to `0x82000000`. It seeds functions from `.pdata`: 8-byte entries `{BeginAddress, packed}`, with the function length in instructions at bits 8–29. Then it runs auto-analysis.
  - Results: 33,653 seeds, 31,481 functions after analysis. The run takes about 10 minutes, and the project goes in `analysis/ghidra_skate3/`.
  - Queries take about 6 s each: `decomp <addr>...`, `xrefs <addr>...`, `str <text>...`.
- **String xrefs** are found only where Ghidra resolved the `lis`/`addi` pair. Some strings, such as the gesture debug print, show 0 refs.

## Assets

### BIG archives (`data/**/*.big`)
EA "EB" version 3, big-endian. `tools/skate3_big.py` lists them (`ls`) and extracts files (`x`).

- **Header:** `'EB'`, u16 3, u32 count, u16 flags, u8 offset shift, u8 0, u32 name-table offset, u32 name-table size, u8 name-record length, u8 directory-record length, u16 directory count, u32 0, u32 archive size.
- **Entries** at 0x30, 16 bytes each: `{u32 offset >> shift, u32 packed size (0 = stored), u32 size, u32 hash}`, sorted by hash.
- **Names:** `count × {u16 directory index, name}`, padded with spaces or NULs. Then come 12 bytes, then the directory records.
- **Packed entries** are RefPack (`10 FB`, EA's QFS LZ).
- `audio/music/overlays.big` is not EB.
- Some entries (models in `data/content/marquee.big`) are `chunkref` containers: `'chunkref'`, `u32 2`, `u32 size`, `u32 chunk size`, `u32 chunk count`, `u32 alignment`, then from 0x28 each chunk as `{u32 packed size, u32 kind}` (kind 2 is RefPack) and its data, 16-aligned. `tools/skate3_big.py` unpacks both.

There are 45,198 files across the archives. The main types:
- `.rx2`: RenderWare 4 for Xbox 360, magic `\x89RW4xb2`. Models and textures.
- `.abin`: animation.
- `.xml` and `.stategraph`: action and motion graphs.
- `.vlt` and `.bin`: AttribSys databases.
- `.abk`, `.sns`, `.snr`: audio.

### Where things are
- **Skater state machine:** `miscload.big` `/state/`, as plain XML: `ActionGraph_OnBoard`, `MotionGraph_OnBoard` and their includes.
- **Board materials:** `fedynamic.big` `e/cas/skateboard/*.rx2`, `e/cas/skatewheel/*.rx2` (about 17 KB each).
- **Board textures:** CaS board graphics are `fedynamic.big` `e/cas/skateboard/*_material.rx2`: one `0x000200E8` raster section whose Xenos texture fetch constant sits at +0x1C (dword 1: format in bits 0–5, `0x14` = DXT4/5, endian 8-in-16 in bits 6–7; dword 2: width − 1 in bits 0–12, height − 1 in bits 13–25; dword 0 bit 31: tiled), with the texels (128 × 128 DXT5, tiled) in the GPU arena. Untiling with the usual Xenos 2D tiled-address formula and swapping byte pairs gives a clean board graphic. These don't fit the pro board meshes' UV atlas (the pro boards' own textures are referenced by GUID from their `0x00EB000B` import table and haven't been located), so skate mode colours the board by part instead.
- **Board models:** `content/marquee.big` `content/marquee/model/<pro>/SkateBoard/*.rx2`, two files a board: the mesh (76 KB, 2,090 vertices, 2,508 triangles, skinned to Skateboard_Root, the two trucks and four wheels) and its textures. In metres, the board along Z (90 cm with the kicks), Y up from where the wheels touch (deck top at 0.128 m), 22.7 cm wide.
- **Board audio:** `audio/audiofiles.big` `/audio/board_scrapes.abk`, `WHEEL_SKID_BANK.abk`, `SK8_AEMS_skateboard.csi`; `audio/wheels.big`; `aud_skateboard` attribute collections, one per surface (`smoothconcrete`, `woodramp`, …).
- **Animations:** `miscload.big` `/anim/`: `OnBoard.abin` (10.8 MB, 2,672 clips, every on-board move), `OffBoard.abin` (651 clips: walking, jumping off), the pedestrian and face sets; `scene.big` (cutscene and trick-guide clips); `data/anim/cac_edit_*.abin`. See "Animations (.abin)" below.

### AttribSys tuning database (`data/big/db.big`)
`skaterschema.{vlt,bin}` holds the class definitions, and `skatercollections.{vlt,bin}` holds the values. `_summaryreport.txt` lists every collection as `/<class>.class/<collection>.xml`.

**Physics classes** (all in the collections):
- `physics`, `physics_jump`, `physics_push`, `physics_steering`, `physics_friction`, `physics_brakes`
- `physics_speed_conservation`, `physics_speed_wobble`, `physics_deck_wobble`
- `physics_trajectory`, `physics_airstates`, `physics_landingondeck`, `physics_manual`, `physics_slide`, `physics_grinds`, `physics_revert`, `physics_wipeout`
- `physicsdeck`, `physicstrucks`, `physicswheels`, `physics_skatecontroller`
- plus `recognizer` (only debug-draw colours; see below) and `camera_*`.

**Vault (`.vlt`):** chunks `Vers`, `StrN`, `DepN` (the dependency files, `.vlt` then `.bin`), `DatN`, `ExpN`, `PtrN` and `EndC`. Each chunk is `{tag, u32 size}`.
- `ExpN`: `u64 count`, then `{u64 key, u64 type, u32 size, u32 offset}`.
- Export types:
  - `Attrib::ClassLoadData` `0x2A7895AC4A876152`
  - `Attrib::CollectionLoadData` `0xAD303B8F42B3307E`
  - `Attrib::DatabaseLoadData` `0x0B38846845E9C175`
- `PtrN`: `{u32 fixup offset into the DatN payload, u16 kind, u16 dependency, u64 target}`. Kind 3 is a pointer into the dependency.

**Hashes:** Bob Jenkins' lookup8 `hash()` with level `0xABCDEF0011223344`, over the bytes without the NUL. For example, `physics_jump` hashes to `0xC2E0A8B6C13990FD` and `default` to `0xD7EDBD362D7D2152`.

**Class (schema):**
- The `u32` at +0x0C is the field count, and the pointer at +0x10 points to the field array in `skaterschema.bin`.
- Each field is `{u64 key, u64 type, u16 offset, u16 size, u16 count, u8 flags, u8 align}`.
- Flags `0x6` means the field is in the collection's fixed layout. Flags `0x4` means it is stored per collection, which isn't decoded yet.
- Types include `EA::Reflection::{Float,Bool,Int32,UInt8,Text}`, `Math::Vector2/3`, `Attrib::Types::FloatColour`, and `Sk8::PointGraphData8/16` and `Sk8::PointNegGraphData8`, which are curves (64, 128 or 80 bytes of floats).

**Collection:** the layout pointer sits right after the `{u64 collection key, u64 class key}` pair. `tools/skate3_attrib.py <raw dir> <class>` uses that to dump a class's fields and every collection's layout values.

**Field names:** only about 300 of 3,991 resolve from strings. The physics field names are not stored anywhere as text; the code reads them by hash. So the meaning of each value has to come from the code that reads it.

Example: `physics_jump/default` has floats 0.4332, 0.6270, 0.8078, 1.0, 1.0, 0.0473, 0.0135, 0.1520, an int of 63, and five normalized curves.

## Animations (.abin)
EA's Animation Toolkit (ANT) database, big-endian. The decoder skate mode uses is `abin_importer.py` from [chasmlol/skate-3-trick-toolkit](https://github.com/chasmlol/skate-3-trick-toolkit) (`skate3_anim/scripts`); its `docs/formats/` describe the codecs. That repository has no licence, so `tools/skate3_anim.py` imports it from the user's own checkout (`--toolkit`) and nothing of it is copied here.

- **Blocks:** a chain from 0x30, each `{u32 size, u32 type, u32 codec, …, name at +0x10}` (the name is packed 6-bit text). Type 2 is a clip, 3 a pose, 4 the hierarchy, 5 a physics T-pose. A block's payload starts at `(block + 0x37) & ~0xF`.
- **Hierarchy:** `u16 bone count, u16 has trajectory, u32 part count`, then `s32 parent[count]`; part records (36 bytes) follow at `+32 + 8 × count`, each with the first bone index (`sqtOffset`) its part writes.
- **Clips:** fps at payload +0x20, frame count (float) at +0x24, a table of parts at +0x38 (`u32`: part id in the top byte, offset in the low 24 bits). Each part is `{u32 header (channel count in bits 6–11), u32 data size, u32 header offset, u32 data offset}`. Codec `0x00524256` (`"RBV"`, VBR: bit-packed DCT blocks) for clips, `0x00004452` (RAW, packed quaternions) for poses.
- **Skeleton:** `OnBoard.abin` uses a compact 36-channel rig: TRAJECTORY, HIPS, SPINE…SPINE3, NECK, NECK1, HEAD, both arms (shoulder, arm, forearm, hand), both legs (up leg, leg, foot, toe), SKATEBOARD_ROOT, the two trucks with their wheels, and channels 32/33 reused as toe targets parented to the board. The names are the toolkit's canonical list; the file only has indices.
- **Poses:** `RIG_TPOSE` holds every bone's reference rotation and offset (metres; hips at 0.92 m). A clip channel is a delta on it: in Skate's row-vector maths `local = delta × reference`, so with column vectors `R = R_ref R_delta` and `t = R_ref t_delta + t_ref`.
- **Axes:** right-handed, Y up, the rider at rest facing +Z with the right hand at -X. The board runs along Z, nose (TRUCK_FRONT) at +Z, and the clips ride toward +Z: TRAJECTORY, the root motion, moves that way (about 2.9 m/s in `R_IDLE_RIDE_N_0_CYC`). The trick clips carry no height; Skate's physics lifts the rider.
- **Clip names** follow the state graphs: `R_IDLE_RIDE_N_0_CYC` (riding), `R_IDLE_HCOM_N100`/`P100` (leaning), `R_PUSH…` (pushing; `MONGO` is front-foot), `R_ANTIC_…` (crouching for a pop), `<TRICK>_LOW_G`/`_A` and `_HIGH_G`/`_A` (each trick's pop and air parts, low or high pop), `L_LAND_…`, `M_IDLE…`/`M_NOSEIDLE…` (manuals), `TKDWN_…` (being knocked down). Wipeouts are ragdoll, so there are no on-board get-up clips.

`tools/skate3_anim.py` retargets a set of these onto the King through `tools/king_retarget.py`, the step CoD4's clips use too, into `game/mods/skate3/anims.bin`; the board's transform rides along in each frame. The King's 85 bones take 22 of Skate's: hips, spine (SPINE, SPINE2, SPINE3), neck, head, arms and legs.

## Flick-it (gestures)
- The intent names are an enum in `.rdata` around `0x820DB0E0`:
  - `L_F_Kickflip`, `L_B_Kickflip`, `L_F_Heelflip`, `L_B_Heelflip`
  - `L_FS_Shuvit`, `L_BS_Shuvit`
  - `FingerFlip`, `FS_Varial`, `BS_Varial`, `Ollie`, `U_Nollie`, `DarkCatch`, `NewDarkCatch`, …
  - The `L_` names are left-stick, foot-specific; `U_` names are the motion-graph ("unified") versions.
- **Trick selection is data-driven** in `state/ActionGraphIncludes/trick.xml`:
  - The condition `HasGestureIntent group="Square|Tail|Nose"` is followed by the behaviour `CreateTrickIntentFromGesture group=…`.
  - `onboardgeneral.xml` maps gestures to tricks through `T_Trick.xml` includes (`GESTURE_NAME`, `TRICK_NAME`, `SWITCH_TRICK_NAME`). For example, the `Ollie` gesture maps to `L_F_Kickflip`, and switch stance maps it to `L_F_Heelflip`.
  - `MotionGraphIncludes/air.xml` adds the late flips and dark catches.
- **Script bindings:** `HasGestureIntent` → `0x82B99CB0`, `CreateTrickIntentFromGesture` → `0x82B9A110`. They are registered by `0x82F5BBE8` / `0x82F5BDE8` through `0x826A61B0(name, fn)`.
- `HasGestureIntent`'s factory `0x82B78878` parses `group` into 0–6: Square, Nose, Tail, 90Nose, 90Tail, N90Nose, N90Tail. The node's vtable is `0x823189E8`.
- **The stick-gesture recogniser itself isn't found yet.** Its thresholds aren't in the attribute DB: the `recognizer` class with `left_stick` and `right_stick` holds only debug colours. Anchors for the next step:
  - the debug string `(%s) Gesture Found: %s - %s, %s, %d` at `0x820DAC24`
  - `GestureDownStart` / `…Held` and `WipeoutGestureX/Y` at `0x820DACF0`
  - the `GestureSpeed` intent

## Sneak King side (how skate mode moves the King)
- **The King is a kinematic physics actor.** His controller block is `[King+0x190]`.
  - `+0x30` is a velocity in King units per second (x, z on the ground). The physics step moves him by it with the game's own collision: he stops at walls, slides along them and follows the ground. `-400` moved him about 385 units a second.
  - While he stands in KingIdle, nothing else writes it. Walking moves him through animation root motion instead (`0x1A760` → `0x185E20`), not through this field.
  - `+0x50` mirrors his position, and writing it does nothing. The rigid body `[[King+0x174]]` holds the position at `+0x190`, and `0xE5820(rb, pos, 1)` sets it.
  - Vtable slots 52 (`0x16FBE0`) and 53 (`0x16FB90`) store the vectors at `+0x30` and `+0x40`, each with a float at `+0xA8` / `+0xAC`. KingMobile's update (`0x1F940`) calls slot 53 with `(0, 1000, 0, 1)` every frame.
- **Scale:** a King unit is about 4 mm. His walk is about 175 units a second, and a 32-inch board is about 5.5 units an inch.
- **No vertical motion from code:** writing an upward `+0x34` (once, or every frame) leaves his node on the ground; the game drops him off ledges with its own, gentler gravity. Skate mode draws his height and asks the game's collision ray (`0x15FD50`, as the camera uses it) how far the ground is below to know when he has left it.
- **Level-exit gate:** in suburbia, a crested gate opens "Choose New Level" when the King reaches it. It isn't a CMissionPickup; the level script's `ShowLevelTeleportMenu` action class (vtable `0x2B7758`+0x1C) has empty slots, so the interpreter runs it elsewhere.
- **Mission pickups:** `CMissionPickup` (vtable `0x2B8F70`, factory `0x558B0`); its Update (`0x61F50`, slot 7) box-tests the King's node against the pickup and, when he enters with the "inside" byte `+0x13A` clear, sets it and has CMode_Pause (`0x11AE0`) open the offer.
- **Citizens' state machine** is the object at `[cit+0x1A0]`: current state at `+4` (its id at `+8`), a request at `+0xC`/`+0x10` (`0x1AB20` writes it). Its update (`0x1AAB0`) runs a pending request: the old state's exit (vtable slot 1), the new one's enter (slot 0, with the owner and the argument), then makes it current. AIManager's registered list (`+0xFC`, count `+0x160`, filled by `0x19140` from `0x20B20`) isn't the walking citizens: its entries never get a current state, and it keeps the first level's entries after a warp. The live citizens show up in their states' updates instead: CitizenIdle `0x237B0`, CitizenMobile `0x23D20`, CitizenAudioAlert `0x23530` (vtable slot 2 of `0x2B1A10`, `0x2B1A5C`, `0x2B19E4`), each called with the citizen as its argument. A citizen's nine states, by id: Idle, Mobile, AudioAlert, Surprised, PassedOut, Interact (`0x23900`), `0x232A0`, `0x24290`, `0x23060`.
- **Moving the King's body:** `0xE5820(obj, vec4, mode)` with obj `[[King+0x174]]` and mode 1 moved his node sideways about 130 units, not to the given point, and left it on the ground; King::Update calls it with `[King+0x170]` and the identity quaternion, so mode 1 may be an orientation. Placing him on a ledge (for grinds) needs that worked out first.
- **Collision and invisible walls.** The King's collision runs through BlitzTech's rigid-body solver, not a swept character test.
  - The controller setter `0x16FD30` chains the contact callback `0x16F650` into his body at `+0x258`.
  - That callback is cdecl `(space, self body, other body, first contact, count)`. Returning 0 drops the pair. It is only ever called indirectly, so `recomp_lookup_manual` hooks it.
  - The pair filter `0xE9090` calls it after the static-world contact generator `0xE8810`.
  - **Contacts** are 0x100-byte slots chained through `+0x8C`. The first holds the count at `+0x84`. `0xE8410` frees them by walking that chain, and the caller marks them active (`+0x80 |= 2`).
  - Contact geometry is in `+0x00..+0x7F`: position `+0x10`, normal `+0x20` (toward self when `+0x70` is self), surface type of side A at `+0x5C`, of side B at `+0x6C`.
  - **Ground test:** a contact counts as ground when its normal.y is above 0.5 (`[0x2B03A0]`). Anything steeper is a wall.
  - **Surface types** (the King's debug print `0x1B0E0`, from `ctrl+0x9C`): 0 None, 1 CameraInvisible, 2 Grass, 3 Gravel, 4 Metal, 5 Metal Platform, 6 Mud, 7 Pavement, 8 Road, 9 Sand, 10 Undergrowth, 11 Wooden Plank.
  - **Invisible walls are surface 1.** The level's static world is one body, so they arrive in the same chain as the ground and the visible walls. In the sawmill, the one at about (-110, 1400) stands at the level's edge over nothing: past it the King falls forever, because the game has no fall-out respawn.
- **States:** KingMobile's vtable is `0x2B14D0` (ctor `0x1F6C0`, update `0x1F940`). Its update sets the wanted heading `King+0x30C` from the stick and picks the run or walk blend (`King+0x320`).

## Open
- [ ] Find the recogniser (gesture windows and thresholds) through the anchors above.
- [ ] Decode the per-collection (flags 0x4) attribute values. Then name the physics fields from the code that reads them; the code uses the 64-bit hash immediates.
- [ ] Locate the deck and truck geometry in the `.rx2` files, and decide whether RW4 is worth parsing or whether a procedural board does the job.
- [x] `.abin` animation format (decoded through the trick toolkit; see above).

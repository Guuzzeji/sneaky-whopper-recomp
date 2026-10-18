# Funny cheats: remaining work

Joke-mode cheats in F1 > Cheats that borrow other games' mechanics. Every asset comes from the user's own copy of those games at run time (`docs/MODS.md`), and nothing from them is committed: `mw1/`, `skate3/` and `game/` are gitignored.

**Background:**
- `docs/MW1_NOTES.md`: CoD4 formats and decompiled weapon formulas.
- `docs/ROADMAP.md`: what has landed (P9).

**Use REA throughout** (CLAUDE.md):
- Open `analysis/rea/default_flat.exe` for Sneak King.
- Open `mw1/iw3mp.exe` for CoD4. `iw3sp.exe` is encrypted by its Steam DRM wrapper.
- For Skate 3, REA rejects PowerPC. Use `tools/skate3_ghidra.py` instead (`docs/SKATE3_NOTES.md`).

**REA contention:** REA keeps one active binary, so parallel agents switch it under each other. Give each agent its own binary or run them one after another.

---

## 1. Shooter mode (CoD4, third person)

### Already working (checked in game)
- The King plays CoD4's own multiplayer animations: 54 clips, retargeted offline by `tools/cod4_anim.py`.
- He holds a textured CoD4 world gun in his hands, following CoD4's grip frame by frame.
- The camera orbits at a fixed distance over his right shoulder (`RECOMP_TPS_DIST`, 270). Its pitch is shooter mode's own, from the mouse, between 45° up and 55° down. The game's own pitch drifts whenever the view from his head is blocked, and its distance follows the pitch. A ray from the shoulder pulls the camera in before walls and the ground. He faces where you aim.
- Shots start from the eye of the rendered view (`cam_eye`, from the view matrix), so they land on the crosshair; the trace projects each hit to the screen. `0x30F8D0` was not that eye: it lagged, and under the King's crouch it sat elsewhere, so shots missed while crouched.
- Movement is a third-person CoD's: WASD relative to the camera (S backpedals, A / D strafe), through his physics velocity as in skate mode, with the CoD4 clips' own speeds. Shift+W sprints, C toggles and Ctrl holds the crouch, all shooter mode's own. LT and RT are muted, so the game's crouch and run stay out of it.
- Reloads, the bolt and bursts follow CoD4's weapon states (`docs/MW1_NOTES.md`): the empty reload time, the rounds going in at reloadAddTime, shell-by-shell reloads that a click stops, the pump or bolt between shots, three-round bursts, auto-reload, a sprint dropping the reload, and ADS held off while reloading. Checked: the W1200 pump rate, its start-to-end reload, the M16 bursts.
- Full-auto fire at the weapon file's real rate, with CoD4's spread and recoil formulas.
- Muzzle flash and the ammo HUD (gun name, count, round bar).
- Facing, mouse look and running.

### Built but not yet confirmed in game
- [ ] R reload with CoD4's torso reload animation, and its clip-out and clip-in sounds. The shell-by-shell clip timing (start 0-0.3, a shell looping 0.3-0.7, end 0.7-1 of the one torso reload clip) is a guess.
- [ ] Weapon switch with 1 / 2: the gun dips, swaps and plays the raise sound.
- [ ] Sprint on Shift: the gun lowers and fire and ADS are blocked.
- [ ] Every gun's sounds (`cod4_sound.c`, XAudio2). Check the level against the game's own audio (`RECOMP_SHOOTER_VOL`).
- [ ] A hit citizen passes out (`CitizenPassedOut`, state 4). This was not hit-tested in third person: the cylinder size of 190 × 32 is a guess.
- [ ] Tracers. One was never caught in a screenshot; each lives 0.06 s.
- [ ] ADS: the camera closes in and the FOV zooms through the FOV hook (`sub_0012CA70`).
- [ ] Shotgun pellets (`shotCount`), and semi-auto pistols firing one shot per click.

### Polish
- [ ] **Idle stance.** CoD4's `pb_stand_alert` is a crouched, forward-leaning stance that reads as a hunch on the King's long robe. Try `pb_stand_ads` (upright, gun shouldered) as the idle, or blend in some of it.
- [ ] **Aim alignment.** The barrel points a little up and left of the crosshair. Either:
  - bend the spine and the right arm so the muzzle lines up with the camera ray, or
  - cast the shot from the muzzle toward the camera's hit point, as CoD4's third person does.
- [ ] **Aim pitch.** The spine bend (`aim_pitch`) is in, but its sign and its 60° limit are untested. Looking up and down should tilt the torso and gun.
- [ ] **Gun over the HUD.** The gun, flash and tracers are drawn at present, after the HUD.
  - Option 1: draw them at the last 3D→2D switch (`0x122C50`, mode 1 → 0) on the presented buffer.
  - Option 2: draw them right after the King's own draw call.
- [ ] **Finger grip.** The King's fingers keep the game's own pose. Curl the right-hand fingers (bones 15–29) around the grip and put the left hand on the handguard (bones 36–50). Rotate each joint about its bend axis.
- [ ] **Tuning, all by eye:**

  | What | Current value |
  |---|---|
  | Camera distance | 230 hip, 150 ADS, ×1.35 sprinting (`RECOMP_TPS_DIST`) |
  | Shoulder offset | 60 right, 22 up |
  | Turn rate | 14 /s |
  | Locomotion blend rates | |
  | Walk/run thresholds | from the clips' root speeds |
  | Playback-rate clamp | 0.6–1.6 |
- [ ] **Foot sliding.** Compare the King's real walk and run speeds (game units) with the clips' root speeds. Clip speeds are in King model units; his world scale is 0.88.
- [ ] **Recoil feel.** View kick goes onto KingCam's yaw (radians) and pitch (degrees). Check the direction and amount in game. CoD4's gun kick (`*GunKick*` fields) isn't used yet; it could add a small viewmodel-style jolt to the arms.
- [ ] **Impacts.** Add a dust puff or spark sprite where a shot hits a wall. The hit point is already returned by `guest_raycast`.
- [ ] **Shell ejection.** Optional: a small brass quad from `tag_brass`.
- [ ] **HUD.** CoD4-style hit marker, already in. Optional extras:
  - a reload progress bar
  - the weapon icon (from the iwds' `hud_icon_*` images)
- [ ] **Overlay.** Remember the loadout between runs (an ini next to `mkb.ini`).
- [ ] **Cleanup before commit:**
  - Remove the debug switches `RECOMP_TPS_NOPOSE`, `RECOMP_TPS_NOPITCH`, `RECOMP_TPS_CONJ` and `RECOMP_TPS_DUMP`, and the `[TPS]` prints. Keep `RECOMP_SHOOT_DEBUG` only if it stays useful.
  - Delete the now-unused first-person bits: the FP block in `sub_0002C670` stays, the `RECOMP_GUN_HIP` notes go.
  - Commit the toolkit change (`mkb_mouse_buttons`) on the toolkit's branch.

### CoD4 deaths on citizens (requested extra)
- [ ] **Citizen skeletons.** Find them the way the King's was found:
  - their bone defs in the level packages (`L_KingMe_* .xbp`)
  - names by CRC hash (`0x6ECC0`)
  - bind from the inverse-bind matrix at def+0x80
  - Check whether all citizens share one skeleton or each model has its own.
- [ ] **Retarget the death clips** (`pb_stand_death_*`, `pb_death_run_*`) onto that skeleton. Generalise `tools/cod4_anim.py` to take a target skeleton (bone map plus package and def address) instead of the King's.
- [ ] **Play the death on a hit citizen.**
  - Hook the bone build (`0x151650`) for the citizen's animation instance (`[[cit+0xF8]+0xB8]`, the same layout as the King's) and write the death pose.
  - Hold the last frame on the ground for a few seconds, then hand back to the game, which can still play its `CitizenPassedOut` getting-up.
  - Root motion: the deaths carry a delta part. Move the citizen's node along it, or skip it.
- [ ] **Aim at the right body part.** Choose the death from where the hit lands on the cylinder: head → `neckdeath`, legs → `legs`, otherwise random.

---

## 2. Skate mode (Skate 3 style)

The disc dump is `skate-3-usa-x360-dump-10-17-26.iso` in the repo root, unpacked into `skate3/`. Findings are in `docs/SKATE3_NOTES.md`.

**What is known so far:**
- The reference recomp (mchughalex/skate3recomp, ReXGlue) has no hand-written skate code: board physics, flick-it and tricks are all lifted PowerPC.
- So everything is learned from the XEX and reimplemented natively.
- The repo has no licence, so nothing can be copied from it anyway.
- REA can't open PowerPC. Use headless Ghidra through `tools/skate3_ghidra.py` on the PE from `tools/xex_to_pe.py`.

### Research (Ghidra on the XEX, plus the game's data)
- [x] Unpack the dump. The XEX is the unpatched disc build (version 3); skate3recomp targets TU3, which isn't on the disc.
- [ ] **Flick-it.** How the right stick's gesture becomes a trick:
  - [ ] the input buffer and sampling rate
  - [ ] gesture windows and thresholds. These aren't in the attribute DB; find the recogniser in code (anchors in the notes).
  - [x] the gesture → trick table. It's data-driven, in the state-graph XML (`trick.xml`, `onboardgeneral.xml`, `air.xml`).
- [ ] **Board physics.** Push and acceleration, rolling friction, carve/turn rate against speed, ollie impulse and gravity, landing tolerance, bails.
  - Values are in the AttribSys DB (`physics_jump`, `physics_push`, `physics_steering`, `physics_friction`, …), and `tools/skate3_attrib.py` dumps them.
  - Field names are hashes only. Name them from the code that reads them.
- [ ] **Animations and assets.** Skater animations and the board model.
  - [x] The containers are tractable: EB v3 BIG plus RefPack (`tools/skate3_big.py`).
  - [x] Animations (`.abin`) decode through the user's checkout of chasmlol/skate-3-trick-toolkit; `tools/skate3_anim.py` retargets them onto the King.
  - [x] The board: a pro's board from `marquee.big` (RenderWare 4, read by the toolkit's `rx2_skeleton.py`), written by `tools/skate3_anim.py` as `board.bin`. Its textures aren't decoded; the parts are coloured.
- [x] Write up the findings in `docs/SKATE3_NOTES.md`, facts only, like `MW1_NOTES.md`.

### Extraction (`tools/skate3_extract.py`, into gitignored `game/mods/skate3/`)
- [x] The board model (deck, trucks, wheels), coloured by part. Textures: not yet.
- [x] Sounds: rolling loop, pops, landings, flip whooshes, scrapes, a grind loop (`tools/skate3_extract.py`, through vgmstream). The banks have no names, so the picks were made by their shape: listen and swap picks in `PICKS` if one sounds wrong.
- [x] Animations: the retarget step is now shared (`tools/king_retarget.py`); `tools/cod4_anim.py` and `tools/skate3_anim.py` are the readers. CoD4's output is byte-identical after the split.

### Runtime (`src/skate.c`; the demo works: push, carve, flick-it tricks, bails)
- [x] **Cheat toggle** in F1 > Cheats. It excludes shooter mode.
- [x] **Movement with the game's collision.** The board's velocity goes into `[King+0x190]+0x30`, not slot 53 (`docs/SKATE3_NOTES.md`):
  - momentum
  - push on Shift
  - brake on S
  - carve with A/D at a rate that drops with speed
  - no strafing
  - `+0x30` is a velocity (units/s) with collision and sliding. The meaning of `+0x40` (set by slot 53) is still open.
- [x] **Flick-it on the mouse.** One quick flick, its direction (8 ways, Shift for nollies) picks the trick; Skate 3's pull-back-then-flick gestures were too hard to learn on a mouse. The camera auto-follows while skating.
- [x] **Off the board.** G steps off: he walks with the game's own controls, the board in his right hand; G again rides.
- [x] **Ollie.** A vertical offset on the King's root, on a parabola. Spike: does the game's ground snap fight it? If so, make it a visual-only offset on his bones and the board.
- [x] **The board** under his feet, flipping and spinning during tricks (roll for flips, yaw for shove-its), landing back level.
- [x] **His pose.** A skate stance on the board (sideways, knees bent), crouch on the ollie wind-up, arms out for balance: either Skate 3's animations or procedural on the same bone hook (`0x151650`).
- [x] **Tricks HUD.** Trick names and a score popup, Skate 3 style, through the overlay.
- [x] **Bails.** Hitting a wall above a speed, or landing badly: ragdoll-ish tumble (a CoD4 death clip works as a stand-in), then get back up.
- [x] **Citizens:** fly past one fast and they jump (CitizenSurprised, state 3) for a Close Call bonus. The live citizens are collected from their own state updates (AIManager's registered list isn't them). Seen in suburbia: "close call: 3 citizens" in the log (`RECOMP_SKATE_STARTLE` widened the reach); their reaction itself wasn't caught on screen.
- [x] **Air:** ollies, rolling off ledges (the game's ground ray says when he leaves the ground) and ramp lips; landing a flip early, or a big drop, is a bail. The game snaps the King's node to the ground whatever vertical velocity is written, so the height is drawn, not simulated.
- [x] **Manuals:** hold the left button (right: nose manual), balance with W / S. Seen in game: the balance bar, Skate 3's manual pose, "Manual +116".
- [x] **Skate 3's animations** on the King: riding, pushing, leaning, the crouch before a pop, every flick-it trick (pop and air parts, the board following the clip), landing, manuals. Seen in game: riding, pushing, a kickflip, a manual.
- [x] **Bails:** he goes over flat and gets back up onto the board (procedural on top of the clips: Skate 3's wipeouts are ragdoll). Seen in game.
- [x] **Slopes** slow him going up (seen in the log on the yard's ramp) and speed him going down.
- [x] **Mission pickups** don't offer missions while skating: `CMissionPickup::Update` (`0x61F50`) sees its "inside" flag held set. Seen in game.
- [ ] **The level-exit gate** (the crested gate that opens "Choose New Level", seen in suburbia) still opens its menu while skating. It isn't a CMissionPickup; it comes from a level script (`ShowLevelTeleportMenu`), whose action classes are empty shells, so the code that opens the menu still has to be found.
- [x] **Missions while skating** (`RECOMP_SKATE_MISSIONS=1`): the board and the Skate 3 pose stay through a mission start (the board is now drawn right after his bones are built, not only at the HUD switch). Seen in game.
- [x] **Invisible walls open** where there is ground 250 units ahead (surface type 1, `king_contacts` in `recomp_manual.c`). Seen: without the ground check he rolled through the sawmill's edge wall and fell forever; with it that wall holds. Not yet seen on a hill with ground behind the wall.
- [ ] **A fall-out rescue.** If he ever gets past the edge, put him back on the last ground. That needs the rigid body's position (`[[King+0x174]]+0x190`) set properly; `0xE5820` doesn't do it.
- [x] **Mouse:** looks around on the ground; in the air a flick is a trick (Space jumps).
- [ ] **Grinds:** drawn, not simulated. Coming down from an ollie beside a ledge or kerb (the collision ray finds ground 12–110 units higher 40 units to one side), he grinds: his node rolls along at the ledge's foot while he and the board are drawn on top of it with Skate 3's 50-50 clip and the grind loop, until the ledge ends, he slows, presses S or pops off. Built and running, but no ledge was reached in testing, so not seen.
- [ ] **Polish:**
  - Skate 3's own numbers once its physics attributes are named; the current values are tuned by eye (gravity is real: 9.8 m/s² at about 4 mm a unit).
  - The board model from the `.rx2` files, in place of the box model.
  - Real grinds on top of ledges need a way to place the King's node; `0xE5820` isn't it as first tried (`docs/SKATE3_NOTES.md`).
  - The board's textures (RW4 rasters, Xbox 360 tiled).

---

## 3. Shared infrastructure to tidy

- [ ] **One mesh loader** for guns, board and future props. It's in `mods_mesh.c` now; split out the OBJ/DDS loader if a second user appears.
- [ ] **A generic retargeter** in tools: source skeleton + clips → target skeleton (King, citizens). Also make his bone defs (`L_KingMe_World.xbp`, root def at file 0x8267B0, pointers relocated by 0x306100) a parameter rather than a constant.
- [ ] **A test or two:**
  - a small self-check that the King-skeleton walk finds 85 bones
  - a retarget sanity test: feet on the ground, torso lean under 30° for the idle clip
- [ ] **Docs.** Keep `docs/MODS.md` (user steps), `docs/MW1_NOTES.md` and this file current; tick `docs/ROADMAP.md` P9 as pieces land.

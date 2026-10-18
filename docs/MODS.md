# Joke modes: assets from your own games

The joke-mode cheats in F1 > Cheats use models, animations, sounds and tuning from other games you own. Nothing from them is in this repo, and the folders below are gitignored: put each install or dump there yourself.

Without a mode's extracted files the game is plain Sneak King: F1 > Cheats shows that mode as not available, and `RECOMP_SHOOTER`, `RECOMP_SKATE` and a saved `shooter.ini` can't turn it on. Shooter mode needs `game/mods/cod4/anims.bin` and at least one gun in `game/mods/guns/`. Skate mode needs `game/mods/skate3/anims.bin` and `board.bin`.

## Shooter mode: Call of Duty 4: Modern Warfare (PC)

The King, in third person, carries two CoD4 guns and moves with CoD4's own multiplayer animations, retargeted onto him.

1. Copy the game folder to `mw1/` in the repo root. It holds `iw3mp.exe`, `zone/english/` and `main/`.
2. List the guns: `.venv\Scripts\python.exe tools\cod4_extract.py`.
3. Extract the ones you want, for example:
   ```
   .venv\Scripts\python.exe tools\cod4_extract.py ak47_mp m4_mp mp5_mp deserteagle_mp beretta_mp
   ```
   Each gun lands in `game/mods/guns/<name>/`:
   - the third-person model (`gun.obj` and its `.dds` textures)
   - `weapon.ini`, the game's raw weapon settings
   - the sounds (`fire.wav`, `dry.wav`, `clipout.wav`, `clipin.wav`, `chamber.wav`, `raise.wav`; for shell-by-shell guns `start.wav`, `loop.wav`, `end.wav`, and the bolt or pump, `rechamber.wav`)

   The muzzle flash and tracer sprites go to `game/mods/cod4/fx/`.
4. Retarget the animations: `.venv\Scripts\python.exe tools\cod4_anim.py`. It takes about 4 minutes and writes `game/mods/cod4/anims.bin`.
5. In game: F1 > Cheats > Shooter mode, pick a primary and a sidearm, then close the overlay.

| Input | Action |
|---|---|
| WASD | Move relative to the camera: S backpedals, A / D strafe. The King keeps facing where you aim. |
| Mouse | Aim. The camera orbits over his right shoulder and pulls in before walls and the ground. |
| Left click | Fire: held for automatic guns, a click a shot for the others, a click a burst for the M16. |
| Right click | Aim down the sights: the camera closes in, and he walks. |
| R | Reload. An empty gun reloads itself. Shotguns and the M40A3 load shell by shell; a click stops that after the shell in hand. |
| 1 / 2 | Primary / sidearm |
| Shift (with W) | Sprint: the gun lowers, and it drops a reload. |
| C / Ctrl | Crouch (C toggles, Ctrl holds) |

The guns follow CoD4's own rules (`docs/MW1_NOTES.md`): the longer reload from empty, the rounds going in partway through, the bolt or pump between shots, the M16's three-round bursts. Movement speeds are the CoD4 clips' own, so his feet don't slide.

A citizen you hit passes out.

Switches:
- `RECOMP_SHOOTER=<primary>,<sidearm>` turns the mode on from the start.
- `RECOMP_SHOOTER_VOL=0..1` sets the gun volume (default 0.7).
- `RECOMP_TPS_DIST=f` sets the camera's distance in King units (default 270).
- `RECOMP_SHOOT_DEBUG=1` logs movement, shots and reload steps.
- `RECOMP_SHOOT_TRACE=1` logs every shot.

## Skate mode: Skate 3 (Xbox 360)

The King rides a skateboard with Skate 3's flick-it controls. He rolls with momentum through the game's own collision: walls stop him, and he slides along them; slopes speed him up or slow him down. He gets air from ollies, off ledges and off the lips of ramps. Land before a flip has come round, or drop too far, and he bails; hit something fast and he bails too, then gets back on. Citizens he flies past jump out of the way (a Close Call). The level's invisible walls don't stop him where there is ground beyond them, so he can ride up hills and over them. The walls round the level's edge, over nothing, still hold.

Turn it on in F1 > Cheats > Skate mode. It turns shooter mode off. It works with no files, but looks and sounds like Skate 3 once these are extracted from the user's own Skate 3 disc (unpacked into `skate3/`, `docs/SKATE3_NOTES.md`):

- **Animations:** `.venv\Scripts\python.exe tools\skate3_anim.py` retargets Skate 3's riding, pushing, leaning, trick, landing and manual clips onto the King into `game/mods/skate3/anims.bin`; the board follows each clip, so the flips are Skate 3's own. It also writes a pro's board (`board.bin`) for the board model. It needs the `.abin` decoder from [chasmlol/skate-3-trick-toolkit](https://github.com/chasmlol/skate-3-trick-toolkit), cloned by the user (`--toolkit <clone>\skate3_anim\scripts`; the default is `C:
epos\_research\skate-3-trick-toolkit\skate3_anim\scripts`). Without the file, the King takes a stance made in code.
- **Sounds:** `.venv\Scripts\python.exe tools\skate3_extract.py` writes the rolling loop, pops, landings, flip whooshes, scrapes and a grind loop to `game/mods/skate3/sounds/`. It needs [vgmstream](https://vgmstream.org) (`vgmstream-cli` on PATH, or `--vgmstream`). Without them the sounds are synthesised.

Both write only under `game/`, which is gitignored. Without `board.bin` the board is a model built in code. While skating, mission givers don't offer missions.

| Input | Action |
|---|---|
| W | Push (hold to keep pushing) |
| S | Brake |
| A / D | Carve left / right. The faster he goes, the wider the turn. |
| Mouse | Looks around. The camera swings back behind the board a moment after the mouse stops. |
| Space, then a quick mouse flick in the air | A trick, early in the jump (also off a ledge or ramp). The flick's direction picks the trick: up Ollie, up-left Kickflip, up-right Heelflip, left BS Pop Shove-it, right FS Pop Shove-it, down-left Varial Kickflip, down-right Varial Heelflip, down 360 Flip. |
| Shift + flick | Up, up-left and up-right become a Nollie, Nollie Kickflip and Nollie Heelflip. |
| G | Step off and walk with the board in his right hand; G again rides. The game's own controls apply on foot. |
| Space | Ollie |
| Ollie onto a ledge's edge | Grind along it (drawn on top of it) until it ends; S drops off, Space pops off. |
| Left mouse button (hold) | Manual; right button for a nose manual. Keep him balanced with W / S (watch the bar). |


- **HUD:** each trick's name and points as he lands, the total score, the speed, and a dot showing where the flick-it stick is.
- **Switches:**
  - `RECOMP_SKATE=1` turns the mode on from the start.
  - `RECOMP_SKATE_DEBUG=1` logs the board's state.
  - `RECOMP_SKATE_FLICK=n` sets how many mouse counts make a flick (default 150; lower for a low-DPI mouse).
  - `RECOMP_SKATE_BOARD=f` sets the board scale without Skate 3's clips (default 5.5).
  - `RECOMP_SKATE_POSE=hip,knee,ankle,arm,spine,drop` tunes the stance made in code, in degrees and model units.
  - `RECOMP_SKATE_ORBIT=deg/s` circles the camera round him (to look at a pose).
  - `RECOMP_SKATE_MISSIONS=1` lets mission givers offer missions while skating.
  - `RECOMP_SKATE_STARTLE=r` makes close calls reach r units (testing).
  - `RECOMP_SKATE_WALLS=1` logs the invisible-wall contacts taken out.

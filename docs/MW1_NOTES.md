# CoD4 (MW1, PC) notes for the shooter joke mode

These are facts learned from the user's own install in `mw1/` (gitignored), using REA and small scripts. No CoD4 code or data is copied into the repo. `tools/cod4_extract.py` reads the install at run time and writes into `game/mods/guns/` (also gitignored).

## Binaries
- **`iw3sp.exe`:** `.text` is encrypted by the Steam DRM wrapper. It has a `.bind` section, the entry point is in it, and the entropy is 8.0. REA can't decompile it.
- **`iw3mp.exe`:** not wrapped. The shared weapon code (`BG_*`, `PM_*`) is the same in single player and multiplayer, so REA work uses this binary. Addresses below are iw3mp VAs.
- **WeaponDef field table:** `{const char *name; int offset; int type;}`, 505 rows at `0x71faa4` (506 at `0x6d8bc8` in iw3sp).
  - Type 6 is float, 4 int, and 7 is milliseconds (the raw file gives seconds).
- **Weapon defs:** in memory they are indexed through the table at `0x72edb8`. `ps+0xe8` is the current weapon.

## Weapon stats
- All 274 raw weapon files are text in `main/iw_11.iwd`, under `weapons/{mp,sp}/<name>`.
  - Format: `WEAPONFILE\key\value\key\value...`.
  - Times are in seconds and angles in degrees.
- Useful keys and their in-memory offsets:

| Key | Offset |
|---|---|
| `fireType` | 0x140 |
| `clipSize` | 0x338 |
| `fireTime` | 0x36c |
| `reloadTime` | 0x388 |
| `adsZoomFov` | 0x424 |
| `adsTransInTime` | 0x484 |
| `hipSpreadStandMin` | 0x450 |
| `hipSpreadMax` | 0x45c |
| `hipSpreadDecayRate` | 0x468 |
| `hipSpreadFireAdd` | 0x46c |
| `adsSpread` | 0x71c |
| `hip`/`adsViewKickPitchMin`/`Max`/`YawMin`/`Max` | 0x748.. / 0x700.. |
| `hip`/`adsViewKickCenterSpeed` | 0x758 / 0x710 |
| `hip`/`adsGunKick*` | 0x728.. / 0x6e0.. |

## Formulas (decompiled from iw3mp.exe)
**View kick on fire** (`0x419410`):
- `pitch = lerp(PitchMin, PitchMax, rand)` and `yaw = lerp(YawMin, YawMax, rand)`, using the ADS values when the ADS fraction is 1 and the hip values otherwise.
- The kick angular velocity in deg/s is set to `(-pitch, yaw, -0.5*yaw)`.
- For the first `*GunKickReducedKickBullets` shots, everything is scaled by `ReducedKickPercent*0.01`.
- The gun kick (viewmodel) uses the `*GunKick*` ranges and is added to the gun's angular velocity.

**View kick recovery** (`0x44cbf0`). It runs in steps of at most 5 ms; for each axis, with `dt = step/1000`:
- While the angle is nonzero: `vel += centerSpeed * -sign(angle) * dt`, using the ADS centre speed when the ADS fraction is above 0.5.
- `d = vel*dt`. If `d` opposes the angle, then `d *= 0.06`. Then `angle += d`.
- If the angle crosses zero, both the angle and the velocity become 0.
- The angle is clamped to ±10°.

**Spread state** (`aimSpreadScale`, `ps+0x628`, 0..255):
- **Each shot** (`0x418210`): `scale += hipSpreadFireAdd * 255`, unless ADS is complete.
- **Each frame** (`0x417580`): `scale -= hipSpreadDecayRate * dt * 255`. While not in ADS, it instead *grows* from turning and moving:
  - Turning adds `hipSpreadTurnAdd * |view delta| * 0.01`.
  - Moving adds `hipSpreadMoveAdd * speed`.
  - Crouch and prone use their own decay scales.
- **Angle** (`0x415980`): the standing min/max (`hipSpreadStandMin`, `hipSpreadMax`) give `spread = min + (max-min) * scale/255`, in degrees. In ADS it lerps to `adsSpread` by the ADS fraction (inferred from the field use at `0x453138`).

**Weapon states** (`iw3mp.exe`, player move; the names are ours):
- **Functions:** PM_Weapon `0x4190D0`, timers `0x417990`, CheckForReload `0x417380`, BeginReload `0x4166A0`, ReloadAddAmmo `0x416F80`, the reloading handler `0x417210`, the reload-start handler `0x4170C0`, rechamber `0x4163A0`, StartFire `0x417EA0`, FireWeapon `0x418270`, sprint `0x419050`, ADS lerp `0x415D00`.
- **Player state:** weapon `+0xE8`, weaponstate `+0xEC`, weaponTime `+0x3C`, weaponDelay `+0x40`, shotCount `+0xF0`.
- **States:** 0 ready, 5 firing, 6 rechamber, 7 reloading, 8 reloading (interrupted), 9 reload start, 10 reload start (interrupted), 11 reload end, 22-24 sprint raise, loop and drop.
- **WeaponDef offsets:**
  - Timers: fireDelay `0x35C`, fireTime `0x36C`, rechamberTime `0x370`, rechamberBoltTime `0x374`, reloadTime `0x388`, reloadEmptyTime `0x390`, reloadAddTime `0x394`, reloadStartTime `0x398`, reloadStartAddTime `0x39C`, reloadEndTime `0x3A0`, sprintInTime `0x3C8`, adsReloadTransTime `0x794`.
  - Flags and counts: weaponType `0x12C`, fireType `0x140`, boltAction `0x4E8`, rechamberWhileAds `0x4F0`, noPartialReload `0x52C`, segmentedReload `0x530`, reloadAmmoAdd `0x534`, reloadStartAdd `0x538`, clipSize `0x338`.
- **Timers:** weaponTime and weaponDelay both count down. A state acts when both reach 0, or when weaponDelay reaches 0 (the "delayed action").
- **Magazine reload:**
  - weaponTime is reloadEmptyTime from an empty clip, otherwise reloadTime.
  - The magazine goes in when the delay `min(reloadAddTime, weaponTime)` expires. There is no separate empty add time.
  - Firing doesn't interrupt it.
  - Starting a sprint drops it. Before the add, the rounds are lost; after it, the clip is already full.
  - ADS is held off while weaponTime is over adsReloadTransTime.
- **Shell-by-shell reload** (`segmentedReload`):
  1. A start step, if reloadStartTime is set. It adds reloadStartAdd rounds at reloadStartAddTime.
  2. Then one step a shell: reloadTime (reloadEmptyTime from empty). It adds reloadAmmoAdd at `min(reloadAddTime, reloadTime)`.
  3. It loops until the clip is full, then runs an end step of reloadEndTime.
  - A newly pressed fire marks it interrupted: during the start, only once 40% of the start is through. The shell in hand still goes in, then the end plays, and you can't fire until the end is over.
- **Bolt action and pump:** after fireTime the state goes to rechamber for rechamberTime, so shots are `fireTime + rechamberTime` apart. The bolt event comes at rechamberBoltTime. A reload started then works the bolt inside it.
- **Bursts:** fireType 2, 3 or 4 is a burst of that many rounds, fireTime apart. Releasing the trigger doesn't stop it. Afterwards `player_burstFireCooldown` (dvar `0x72CB00`, default 0.2 s) must pass, and fire has to be pressed again.
- **Auto-reload:** once the clip is empty and there is reserve ammo, the reload starts as soon as the last shot's fireTime is over, held trigger or not.
- **Sprint:** starting a sprint drops a reload. A sprint doesn't start while firing or rechambering.

## Assets
- **Fastfiles:** `zone/english/*.ff` are `IWffu100`, version 5, then one zlib stream.
  - The decompressed zone starts with a header of size, external size and 9 block sizes.
  - Then come the script strings (count, -1 pointers, inline C strings) and the asset list of `{type, -1}` pairs.
  - Asset data follows in order. Pointers are either -1 (data follows inline) or `((block << 28) | offset) + 1` (a reference to data seen earlier).
- **Asset types:** 23 is a weapon (`WeaponDef`, 0x878 bytes), 5 is an xmodel, 4 is a material, 3 is an xanim.
- **Viewmodels:** `common_mp.ff` holds every multiplayer viewmodel. Single-player ones are in the level zones.
- **XModel:** 0xDC bytes; the name follows.
  - Inline arrays: boneNames (u16 script-string ids), parentList, quats, trans, partClassification, baseMat.
  - Then `XSurface[numsurfs]` (0x38 each). Each surface carries:
    - optional blend info
    - vertices: `GfxPackedVertex`, 32 bytes: xyz float3, binormal sign, colour, half2 UV, packed normal and tangent
    - rigid vertex lists: `boneOffset = bone*64`, plus a collision tree
    - u16 triangles
  - Then the material handles.
  - Multiplayer viewmodels contain every attachment (ACOG, reflex, silencer, mounts), each rigid on its own `tag_*` bone. The weapon's `hideTags` lists the ones to hide.
- **Images:** `main/iw_*.iwd` are zips of `images/*.iwi`.
  - IWI v6: `IWi`, version 6, format (0x0B DXT1, 0x0C DXT3, 0x0D DXT5), flags, u16 width, height and depth, 16 bytes of mip offsets, then the mips smallest first.

# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning rule for this project: the patch number goes up by 1 for each small item or test
round, the minor number goes up by 1 for each finished feature or phase.

## [0.3.1] - 2026-10-23

### Added

- Bullets hit entities (`src-server/server/BulletSystem.cpp`, `src/gun/BulletHit.h`): each tick
  the path up to the first block is tested against mobs and players with TACZ's fixed hitbox
  (stretched along and shifted back against the target's motion, +1/16 for standing players),
  nearest first, up to `pierce` targets.
- TACZ damage on hit: distance falloff, headshot multiplier (eye height ± 0.25), armour-ignore
  split into an armour-reduced projectile part and a part with a cause the game does not reduce
  by armour (chosen at runtime), shotgun pellets, ignite, knockback. The hurt cooldown is
  cleared before each part, as in TACZ, so full-auto fire is not absorbed.
- Damage is attributed to the shooter (`ActorDamageByChildActorSource`); hit details are
  logged and shown on the probe HUD.
- Unit tests for hit geometry (28 cases total).

### Not yet

- Player-vs-player latency rewind, configured headshot boxes, explosions, spread.

## [0.3.0] - 2026-10-23

Technical validation phase complete: all six items in `docs/VALIDATION.md` pass in single-player
(custom packets, item NBT, input interception, first-person model rendering, sound playback,
server-driven bullet entity). Next phase: gun logic implementation.

### Changed

- 0.2.6 test result: bullets are visible at full speed, disappear on the block they hit and
  leave nothing behind; `moveTo` and `teleport` sync look the same, `moveTo` stays the default.

## [0.2.6] - 2026-10-23

### Added

- Bullet entity probe: a minimal behavior pack (`packs/behavior_packs/tacz_bp`, entity
  `tacz:bullet` with no physics, damage or AI of its own) and resource pack
  (`packs/resource_packs/tacz_rp`, a small yellow cube). LeviLamina loads both from the mod
  folder automatically.
- Server bullet system (`src-server/server/BulletSystem.cpp`): firing spawns a bullet that the
  mod moves every server tick with TACZ's motion model (speed / 20 blocks per tick, then drag
  and gravity) and removes on the first block hit, on expiry or in unloaded chunks.
- `src/gun/BulletPhysics.h` with unit tests (24 cases total).
- `/tacz bdebug <moveMode> <speedScale>`: entity sync via `moveTo` (0) or `teleport` (1), and a
  speed multiplier to watch bullets in slow motion.
- `scripts/deploy-client.sh` also deploys the packs.

## [0.2.5] - 2026-10-23

### Added

- Gun pack sound playback (client, `src-client/client/sound`): `tacz_sounds/<path>.ogg` is read
  from the gun pack and played through `SoundEngine::playFromMemory`; no resource pack needed.
- Gun display `sounds` map is parsed. Probe: left click plays the held gun's `shoot` sound,
  R plays `fire_select`.

### Fixed

- Displays with an empty sound id (`"inspect": ""`, used by fn_fal, hk_mk23, spas_12) failed to
  load, so those three guns had no display. Empty ids are now skipped, as in TACZ.

## [0.2.4] - 2026-10-23

### Changed

- First-person gun quads are single-sided by default (0.2.3 test: no missing faces), halving
  the mesh size. `/tacz rdebug 6 1` turns two-sided drawing back on for diagnosis.
- Technical validation item 4 (first-person model replacement) recorded as passed in
  `docs/VALIDATION.md`.

## [0.2.3] - 2026-10-23

### Changed

- First-person gun now renders with the textured moving-block alphatest material by default
  (confirmed visible and correctly oriented in the 0.2.2 test).
- The gun transform is multiplied onto the current world matrix, which during the first-person
  pass holds the inverse view rotation. The view matrix is never replaced.
- `/tacz rdebug <material> <twoSided>`: the placement argument is gone.

### Fixed

- Crash when the first-person gun was drawn with a replaced view matrix (0.2.2, placements 0
  and 1: null read inside the renderer). That code path is removed.

## [0.2.2] - 2026-10-23

### Changed

- First-person gun render diagnostics: `/tacz rdebug <material> <placement> <twoSided>` switches
  between 7 materials and 3 placements at runtime and logs camera and matrix state once per
  change. Defaults: renderer static material, camera-basis placement, two-sided quads.
- `scripts/deploy-client.sh` builds the client and only copies it if the build succeeded and the
  manifest says `client`.

### Fixed

- 0.2.1 test result: the gun was not visible in first person (mesh, texture and material were all
  created; the cause is being narrowed down with the diagnostics above).

## [0.2.1] - 2026-10-23

### Added

- First-person gun rendering probe (client): while holding a gun (`/tacz setgun`), the vanilla
  first-person pass is replaced by the gun's TACZ model and texture read from the gun pack.
  Rest pose only: no animation, no arms, no aiming.
- Bedrock geometry loader matching TACZ's conversions (`src/model`): pivots, per-face and box UV,
  inflate, mirror, rotated cubes, `_illuminated` bones, idle_view positioning.
- Gun display files (`assets/<ns>/display/guns`) are loaded; packs stay open for asset reads.
- Unit tests for the geometry loader (19 cases total).

## [0.2.0] - 2026-10-23

### Added

- Gun pack loader for the TACZ format (`src/pack`): scans `mods/tacz/gunpacks/` for folder packs
  with `gunpack.meta.json`, parses gun index/data and ammo index with TACZ's defaults and
  validation, and collects `data/<ns>/scripts/*.lua`. Loads all 54 guns of the default pack.
- Damage and rate-of-fire calculations matching TACZ (`src/gun/GunStats.h`): distance falloff,
  headshot multiplier, armour-ignore split, shotgun pellet split, shoot interval.
- Unit tests (`tacz-tests` target, doctest).
- `/tacz packs` command; `/tacz setgun` now checks the gun id against loaded packs.

## [0.1.2] - 2026-10-23

### Fixed

- `/tacz nbt` output is also written to the log (chat output from custom commands is not shown
  in single-player).

## [0.1.1] - 2026-10-23

### Added

- Technical-validation probe: custom client/server packets, gun NBT read/write and sync,
  swallowing vanilla left-click while holding a gun, debug HUD text. Verified in single-player.

## [0.1.0] - 2026-10-23

### Added

- Project set up from the LeviLamina mod template: server and client variants, GPL-3.0 licence.

# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning rule for this project: the patch number goes up by 1 for each small item or test
round, the minor number goes up by 1 for each finished feature or phase.

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

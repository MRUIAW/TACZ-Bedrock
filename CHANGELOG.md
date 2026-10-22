# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning rule for this project: the patch number goes up by 1 for each small item or test
round, the minor number goes up by 1 for each finished feature or phase.

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

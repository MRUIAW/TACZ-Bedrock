# TACZ internal assets

The animation files and default state-machine script here are unmodified copies from
the Java TACZ reference (`src/main/resources/assets/tacz/animations` and the default gun
pack's `assets/tacz/scripts`). They are covered by this project's GPL-3.0 licence.

`textures/` contains 26 unmodified, authorized TACZ jar-resource PNGs from
`src/main/resources/assets/tacz/textures`: 22 normal crosshairs, one hit marker,
and three fire-mode HUD icons. No third-party gunpack assets are copied here.
Tests compare these files with the reference and the binary-embedded bytes.

`tacz.builtin_assets` embeds them into both the mod and test executable. Gun packs
override these resources; a build or installation never needs the reference checkout.

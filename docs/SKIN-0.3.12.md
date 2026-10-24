# 0.3.12 first-person arm skin source

Runtime follow-up: after the game was closed, the deployment script succeeded; client
0.3.12 DLL/PDB/manifest SHA-256 and both packs were verified against source. The user then
reported default Steve arms again. `2026-10-25.4.log` confirms matched native events but
expired snapshots / empty default-mesh metadata, not successful skin binding. See
`SKIN-0.3.13.md` for evidence and the blocked, diagnostic-only investigation. The earlier
build/deployment-blocked and runtime-pending notes below are the historical initial report.

## Evidence and cause

0.3.11 was approved and committed separately as **`ce8c25b`** on `feat/milestone-1`
(`TACZ Bedrock 0.3.11: first-person player arms`). No amend/push/hook skipping.
User reported correct arm animation, no crash, but both Persona and imported classic skins
showed Steve/Alex. RTX arms animate while the gun remains static; this is recorded separately
in `RTX-0.3.10.md`, with no RTX action in this patch.

Logs selected by modification time before code changes:

- `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/logs/2026-10-25.1.log`:
  line 22 loads 0.3.11; line 104 creates default arms with
  `reason=skin-owner-thread-unavailable image=64x64`.
- Preceding `2026-10-25.log`: all five arm creation events have the same reason.
- That 64x64 is the **fallback PNG's** size, not proof that the player's skin image was 64x64.
  The old code returned before reading the image. The combined condition was null owner OR
  uninitialized `mThreadIdInitialized` OR owner thread != current thread. Historical logs do
  not distinguish these cases or record IDs/format/size/empty/compressed state. We cannot
  retrospectively identify which subcondition rejected the classic skin.
- The old `auto owner = ref->mSkinImpl.get()` was a raw pointer: SDK `TypedStorage` aliases
  `std::shared_ptr` directly. Its comment about retaining shared ownership was inaccurate.
  The new code copies the actual shared wrapper for the duration of the lookup.

The demonstrated cause is the serialized-skin/thread-gated render-time access path, not an
established malformed image. Removing the guard or reading `mObject` across it would not be
a safe fix. This patch replaces that source rather than weakening the check.

## Source, ABI and lifecycle

- `ClientSkinInfoData::updateSkin(SerializedSkinRef const&, mce::Image const*, mce::Image const*)`
  is a fully typed native skin-texture update boundary. The observer forwards unchanged via
  `$updateSkin`, then records its `mSkinTexture.mBedrockTextureData`, `mDefaultMeshName` and
  weak input/stored skin identities. Both method/thunk signatures have compile-time assertions
  against the installed 26.51.1 client headers (LL package 26.51.7). No opaque-by-value hook.
- Before origin, capture only supplied image width/height/depth/format/byte count, not pixels.
  Metadata describes that native update input, not a reconstructed historical serialized image.
  A null image pointer means no image supplied to this update, not necessarily an empty skin.
- Observe from client mod load to catch pre-world/dressing-room updates. Registry is bounded
  at 128 and stores weak pointers, strings and scalars only. No retained client/player/level,
  texture resource owner or global service query/upload at the menu. Disable unhooks/clears.
- Only after GunRenderer's existing valid world/resource checks, match local `mSkin` by shared
  control-block identity (input OR stored owner); lock the texture weak pointer transiently.
  This correlation is structurally safe but still needs real-game confirmation: a deep-copied
  owner with no matching observed identity deliberately falls back rather than guesses.
- For a loaded, nonmissing, admitted texture, copy the fully defined `mce::ClientTexture`
  resource handle into the existing render texture variant. No generated resource location,
  raw image copy or custom-skin upload. The registry does not pin that handle across worlds.
- Active arm cache owns the borrowed handle only within the existing world/resource lifecycle;
  `releasePlayerArms` drops it before native teardown. Never call unload on the engine's skin.
  Only the separately owned Raw Steve/Alex fallback is unloaded via its weak TextureGroup.
- Probe at most once/second; unchanged texture uses unchanged meshes. Texture-only rebinding
  does not rebuild geometry; wide/slim changes and invalid meshes do. Unchanged fallback also
  avoids repeat disk reads. Maximum skin-selection latency remains approximately one second.

## Layout / support boundary

Installed `data/skin_packs/vanilla/skins.json` and `geometry.json` verify exactly
`geometry.humanoid.custom` / `geometry.humanoid.customSlim`, a 64x64 classic UV space and
the distinct modern left/right arm/sleeve islands. Normalized UVs also admit its 128x128
scaled equivalent. Unverified `geometry.humanoid` / `.slim` aliases are not accepted.

Admission uses the native-reported `ClientSkinInfoData::mDefaultMeshName` plus 64/128 square
GPU dimensions and a loaded/nonmissing/nonempty handle. This field is a renderer-provided
geometry/default-mesh name, **not independent inspection of a Persona compositor's UVs**.
Classic wide/slim are the implementation target; successful identity matching, actual field
values, material/alpha behavior and texture appearance still require the 0.3.12 game test.

Persona repository/appearance/compositor/geometry types are opaque or lack safe lookups in
these headers; installed `data/skin_packs/persona/skins.json` has an empty skin list. There is
no captured composed atlas or geometry for the user's Persona. Its actual dimensions/arm UV
layout therefore remain **unknown**; do not claim it is classic just because it is square.
Unrecognized reported geometry/dimensions fall back with `unverified-renderer-arm-layout`.
General Persona support (including pieces, tint, nonclassic proportions/geometry) is NOT
implemented or verified. Even a classic-looking native name is not a blanket Persona guarantee.
No atlas-animation handling, arbitrary-geometry rendering or legacy 64x32 conversion is added.

Borrowed engine textures cannot have alpha rewritten like the old private pixel copy.
Default PNG copies retain the base-opaque/sleeve-alpha policy; unusual classic alpha skins
and native material differences need visual checking. No claim of Java-exact skin materials.

## Diagnostics

`[arm-skin]` logs only when owner identity/selection revision/reason/layout/owner state changes:

- `ref`, `owner`, `ownerState`: missing ref, missing wrapper, uninitialized marker, same/other
  thread. `ownerThreadHash`/`renderThreadHash` are comparable C++ thread hashes; render/update
  numeric IDs are Windows IDs, not interchangeable with those hashes.
- `observedUpdates` and matched `revision`: no update observed versus no local identity match.
  Native image provided/dimensions/depth/format/bytes; GPU dimensions and native geometry name.
- Explicit fallback reasons: missing ref/owner, texture not observed/expired/pending/missing,
  no native handle, or unverified layout. A readiness transition may log once again; stable
  fallback does not log every frame/second. `[arm-gpu] bind ... upload=false` confirms borrowing.

These diagnostics do not dereference the guarded serialized skin object. Missing metadata
is reported as missing, not guessed. No runtime 0.3.12 evidence is available yet.

## Validation

- All **98 tests / 42,627 assertions pass**. Includes GPU-layout admission without CPU bytes,
  known wide/slim layouts, unknown/Persona geometry, unverified aliases, legacy/nonsquare/zero
  and oversized textures, plus all existing arm matrices/UVs/lifecycle policy tests.
- Pure arm matrix benchmark: approximately 0.00039 ms/frame in the final test run; excludes
  Minecraft, texture selection/upload, submission and GPU execution. This is not runtime cost.
- Server release: `xmake f -a x64 -m release -p windows --target_type=server -y`, then
  `xmake build tacz`: pass (31.953 s), generated manifest 0.3.12 / server.
- Client release: `scripts/deploy-client.sh` reconfigured/built 0.3.12 / client successfully
  (32.625 s). Only unused `_AutoHookCount` macro-variable warnings in the two render hooks.
  The script then refused deployment because `Minecraft.Windows` was running (PID 241356,
  started 2026-10-25 01:53:20). No force-close, bypass or live DLL replacement was attempted.
- Source `bin/tacz/manifest.json` is 0.3.12 / client; destination
  `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz/manifest.json`
  remains 0.3.11 / client. DLL/PDB/manifest SHA-256 all differ, as expected for blocked deployment.
  Source DLL SHA-256: `B7658374A17D8831520F850C42361A1192E18FDB1F941D00C4DD939504651D2F`.
  Close Minecraft, rerun `E:/Programs/Git/bin/bash.exe scripts/deploy-client.sh`, then compare
  all three artifacts before testing. The currently running game is NOT testing this fix.
- 0.3.12 changes remain uncommitted. No game launch/runtime acceptance performed by the agent.

## User acceptance sequence

1. Cold restart (observer must see initial skin update), confirm 0.3.12 in log, wait at menu
   >=10 seconds: no crash; observer-install log is expected, no arm allocation/world query.
2. RTX off, import a distinctive **classic wide** 64x64 skin with different left/right hands
   and sleeve colors. Enter world, equip gun, wait >1 second. Expect renderer-handle selection
   and `upload=false`; check correct skin/UVs/sleeves during all six previously tested actions.
3. Repeat with **slim** and, if available, standard-layout 128x128. Verify 3-wide arms and no
   stretching. Capture `[arm-skin]` including native geometry if fallback unexpectedly occurs.
4. Test **Persona** and capture its native geometry/GPU size/reason. Unknown layouts should
   explicitly fall back. Do not count a noncrash/default skin as successful Persona support;
   atlas/geometry evidence is needed before extending the whitelist.
5. If the game permits, change skin in-world, including wide <-> slim, wait >1 second. Ensure
   source changes, no stale old skin/pink texture, no stable-state log spam; unchanged width
   should not rebuild meshes. If UI requires leaving the world, test that supported path.
6. Quit mid-put-away, quit while holding, A -> B -> A; check release/fresh binding and no stale
   texture. Return to menu >=10 seconds: probes/draws stop and active arm GPU refs are released.
7. Optional RTX regression only: arms should still animate, gun issue remains parked. Collect
   logs after >=120 rendered frames per mode; no RTX fix is claimed by this skin patch.

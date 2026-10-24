# 0.3.9 GPU ownership fix and RTX investigation

Status: pink-texture lifetime defect fixed in code; **RTX animation is not yet fixed**.
Neither native GPU behavior nor the new trace/leave hooks have been game-tested by the agent.
0.3.7/0.3.8 were committed as `8d4f8da`; 0.3.9 is deliberately uncommitted.
Server and client release builds passed; 90 tests / 21,825 assertions passed. The client was
successfully deployed with `scripts/deploy-client.sh` while Minecraft was closed.

## Evidence: magenta after rejoin

Newest log by modification time:
`F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/logs/2026-10-24.16.log`.

- Line 78: world generation 3; lines 100-103: AK47 model/mesh/texture uploaded and drawn.
- Line 429: world generation 5; line 508: generation 7. No second texture upload/model build
  occurs anywhere in this run. The old display cache is reused despite a new world.
- At commit `8d4f8da`, `GunRenderer.cpp:124-128` makes the GPU cache process-scoped;
  `visualFor:197-200` immediately returns an existing entry, skipping texture upload;
  `disable:432-438` is the only cache destruction. Mod disable is not world exit.
- Animation snapshots contain world/pose/gun ID data, not meshes or TexturePtrs. The retained
  VM therefore does not directly hold a dead texture. All cached displays share the same
  flawed lifecycle, so quitting while simply holding a gun is also exposed by code inspection.
  That scenario and switching two different worlds have **not** been independently game-tested.
- The run contains no `[world] exit` lines, and even contains old-hand work before a subsequent
  join-loaded event. Whether the LL callback was bypassed or its global-client guard rejected
  it is not established by that log. It cannot be the sole exit boundary. The old guard queried
  the global primary client during teardown, although the callback itself already identifies
  the exiting client.

The new request-leave hook runs before `ClientInstance::_requestLeaveGameImpl` origin, covering
normal sync/async requests before resource teardown. It invalidates the world epoch and calls
GPU release. LL exit is an idempotent fallback using the recorded identity, and start-join and
join-loaded retire a previous generation too. A hook-install failure is logged rather than
mistaken for proof that release occurred.

`RenderResourceKey` includes world generation (even if all native addresses are reused), client,
TextureGroup, BufferResourceService, FrameBuilder and lighting pipeline. Before any cached display
can be used in a mismatched scope, the old cache is cleared. Materials are included in that clear.
TexturePtr and mesh references are dropped, and only our Raw texture locations are unloaded via
their original weak group. Unique world/cache-serial Raw names avoid collisions with old resource
load entries. All draw access now holds the cache lock until submission completes, so event cleanup
cannot destroy a returned GunVisual while it is being submitted.

This does not solve arbitrary device-loss/resource-pack-reload cases by guessing at GPU internals;
if a same-owner reload invalidates a texture without any scope/key change, it needs a separate test.

## Measured walk rate

See the 0.3.9 changelog table for exact values and timestamps (log lines 224,268,282,295).
In these stable ground-like windows native/Java is approximately **5/3**, i.e. native was 66.67%
higher than the corrected input (the correction is 40% of the old input). The controller rate
closely follows the distance-derived rate. This is now measurement, not the earlier assumption.

Do not apply that ratio to every window. For example line 227 reports run 5.606 blocks/s,
Java/native 0.16858/0.03842, but sampled track time is 0.00 s. Movement/state/window effects
are not identified there. The run's stable speeds are not evidence of Bedrock universally moving
faster than Java; genuine speed differences remain visible in the distance-driven animation.

## RTX: what is known, what is not

The user confirms a visible but static model in Ray Tracing, with sounds, and correct animation
in other modes. That alone does not prove that CPU pose publishing, hand submission, vertex uploads
and the RT backend all ran correctly. 0.3.8 has a one-time CPU rebuild benchmark but no per-mode
hook/build/draw/pose counters; there is no runtime RTX trace from which to select a root cause.

Reviewed LL 26.51.7 headers:

- `mce::Mesh` exposes `mCacheKey`, `mTemporary`, CPU MeshData, GPU resource pointers,
  `renderMesh` and `_renderMesh` with optional `dragon::RenderMetadata`.
- `FrameBuilder` exposes actual `isRayTracingEnabled/isDeferredEnabled` and submission of
  `RenderMeshFallbackDescription`, `RenderItemInHandDescription`, skinned mesh descriptions, etc.
- RenderMetadata has ID / item classification / CSS metadata, but the factory's generic creation
  method is not a normal exported callable API in this SDK.
- `BVHMeshKey`, `BVHMesh`, `BVHSkinnedMesh`, `RTXMinecraftFrameRenderer` and
  `RenderMeshFallbackDescription` are opaque/empty generated types, not sufficient definitions
  for safely constructing or updating a BLAS directly.
- Lamium [`src/overlay/CellMesh.cpp`](https://github.com/amatouhake/Lamium/blob/main/src/overlay/CellMesh.cpp)
  uses the same Buffered tessellation and Mesh::renderMesh approach for overlays. Its render/overlay
  sources, including GhostRenderer, contain no usable animated-RTX geometry update implementation.

These are **limits on current evidence**, not proof that RTX animation is impossible. Native Mesh
submission is callable, so no impossibility claim is made. Do not randomize IDs, force temporary
meshes, toggle the user's RTX settings or substitute another draw path without establishing the
actual backend contract. No such workaround is included in this build.

## New diagnostics / interpretation

- `[anim-frame]`: independent of the hand hook, approximately once per second while a visible
  gun session exists. World, frame-hook rate, animation frame serial, pose revision and actual
  RTX/deferred flags. A silent/missing hand hook can no longer hide behind missing render logs.
- `[gun-render]`: per-window hand-hook calls, CPU mesh builds, Mesh::renderMesh submissions,
  native `_renderMesh` calls, distinct pose revisions, animation frame/pose/mesh versions,
  invalid meshes, cache key type/value, temporary flag, CPU positions and metadata ID/item flag.
  Native-call tracing is scoped to our own synchronous submissions; other game meshes are merely
  forwarded. Installation failure produces an explicit warning. These are CPU submission counters,
  **not proof that the GPU/BLAS consumed new vertices**. Mesh revision in a log window describes
  the previous completed draw, so one-frame lag versus the just-read pose is normal.
- `[gun-gpu] bind/create/release`: includes world/cache serial and release reason. Exit should
  show release, rejoin should show a fresh bind/create, even for exactly the same AK47.

Interpret RTX vs raster while doing the same actions:

1. Animation frame advances but pose revision does not: inspect the pose/VM side first.
2. Animation/pose advances, no hand logs: different/skipped first-person hook path.
3. Hand logs but invalid meshes increase / builds stop: validity/resource transition path.
4. Builds/draw/native calls and pose/mesh versions advance but pixels stay static: investigate
   the RT submission/geometry cache below `_renderMesh`, using the recorded metadata/cache key.
   A constant key alone is not sufficient proof: a correct backend could update in place.

## User retest

1. Enter holding a gun. Quit mid-put-away, rejoin; verify real texture rather than magenta and
   fresh create/release logs. Repeat quitting normally while holding, and world A -> B -> A
   (two switches). Check no old texture/material/mesh is reused across serials.
2. RTX on: idle, walk, shoot, reload, inspect for several seconds each. Check whether animation
   actually changes, and retain the new diagnostics if it remains static. This is an investigation
   test, not a promise that RTX has been fixed. No need to paste logs: the next run can read them.
3. RTX off: repeat the same actions and all put-away/fast-scroll checks for regression.
4. Switch graphics modes while in-world if supported; expect a pipeline-scope release/recreate.
   `[anim-frame]` and `[gun-render]` should agree on the actual RTX flag.

Pure tests cover world/address reuse and resource/pipeline key changes, not actual GPU destruction,
engine event ordering or RT animation. Native hooks, early release and URI unloading need this
in-game validation. Trace hooks add small CPU overhead and should be removed/gated once RTX is
resolved.

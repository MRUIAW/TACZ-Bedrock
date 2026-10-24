# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning rule for this project: the patch number goes up by 1 for each small item or test
round, the minor number goes up by 1 for each finished feature or phase.

## [0.3.12] - 2026-10-25

### Runtime follow-up and diagnostics — skin still falls back to Steve/Alex

- User tested deployed 0.3.12: arms still show default Steve; no crash reported, no other
  acceptance results supplied. `2026-10-25.4.log` proves the update hook installed at load,
  fired at least 53 times and matched the local skin. All 15 selection records fall back:
  7 expired weak texture snapshots, 8 unverified layouts with an empty native default-mesh
  field. There are zero borrowed-handle binds. This is NOT evidence of a missed enable-time
  event, nor of failed identity matching, nor a malformed 64x64 RGBA image.
- `ClientSkinInfoData::mDefaultMeshName` was wrongly treated as actual skin layout metadata:
  it is empty in every observed selection. A one-time weak texture snapshot also does not
  establish the final vanilla renderer binding. Do not relax layout checks or pin old GPU
  textures as a speculative workaround. Exact update timestamps/unique owners were not logged.
- Added diagnostic-only update events (first 128, total count at disable), snapshot identity
  addresses/shared-owner match kind, update-time texture/handle/loading state, and one actual
  draw-return/source record per selection. World attribution reads only the atomic session
  token, with no main-menu client/level service query. Selection/rendering policy is unchanged.
- No verified draw-time/current-repository skin lookup or neutral native arm-render contract
  was found in the installed SDK. Version stays 0.3.12; diagnostic draft is not deployed and
  no 0.3.13 fix is claimed. Evidence, safe-API audit and next options: `docs/SKIN-0.3.13.md`.

### Changed (observer implementation, not a completed skin fix)

- Replace the first-person arm skin's render-thread serialized-image read with a weak observer
  of native `ClientSkinInfoData::updateSkin`. Correlate its input/stored shared-owner identities
  with `LocalPlayer::mSkin`, then borrow the game's existing typed `mce::ClientTexture` handle.
  No custom-skin pixel copy/re-upload, fabricated resource location, thread-check bypass or
  dereference of `SerializedSkinImpl::mObject`. Hook/thunk signatures are compile-time checked
  against the installed 26.51.1 client headers; arguments are references/pointers only.
- Both latest 0.3.11 logs rejected all six observed arm skin creations at the old combined
  `skin-owner-thread-unavailable` check, BEFORE image inspection. The broken access boundary,
  not an established RGBA/size rejection, explains the default skins. Those logs cannot tell
  null owner, unset debug marker and different thread apart. New `[arm-skin]` diagnostics split
  these and report native input image metadata, GPU dimensions, geometry, update/revision and
  texture availability; log only selection/owner/state changes, not every probe/frame.
- Admit only loaded/nonmissing handles with 64/128 square textures and native-reported
  `geometry.humanoid.custom` / `geometry.humanoid.customSlim`, whose classic arm UVs were checked
  in the installed vanilla geometry. Preserve Steve/Alex fallback with an explicit reason.
  Persona's composed atlas/layout is NOT established from the SDK/assets; square dimensions
  alone are not sufficient, and general Persona/custom-geometry support is not claimed.
- Observe updates from mod load (including dressing-room updates), retaining only weak
  identities/texture references and metadata outside a draw. No menu-time world service query.
  Existing world/pipeline/resource release drops borrowed handles without unloading the engine's
  texture; only our owned default texture is unloaded. Texture-only changes reuse arm meshes;
  a slim/wide change or invalid mesh rebuilds them. Unchanged fallback avoids repeated PNG I/O.

### 0.3.11 runtime results

- User tested idle/walk/shoot/reload/inspect/put-away: arms animate correctly, no crash reported.
  Persona AND imported classic skins instead showed Steve/Alex; this prompted the skin fix.
- With RTX on, cached arm geometry plus per-draw matrices animates while the CPU-baked/rebuilt
  gun stays static. Strong additional support for the reused-geometry/BLAS cache hypothesis,
  not proof of the exact backend cache key. RTX remains parked; no RTX fix/experiment here.
- Real 120-frame arm CPU means: RTX on 0.032 ms, off 0.028–0.034 ms; each sample reports four
  mesh builds / 480 draw returns. GPU execution is not timed. See `docs/RTX-0.3.10.md`.
- Approved 0.3.11 work committed separately as `ce8c25b`.

### Validation / limitations

- 98 tests / 42,627 assertions pass, including GPU-layout admission without CPU image bytes
  and rejection of unknown/Persona geometry, legacy/nonsquare/unverified dimensions/aliases.
- Server/client release builds passed. Initial deployment was blocked by the running game;
  after closing it, 0.3.12 deployed successfully with DLL/PDB/manifest and both packs verified.
  User tested: arms animate, no crash reported, skin still falls back to Steve. The additional
  diagnostic-only logging above was built/tested but not separately deployed at that point.
  Unit tests do not validate engine hook behavior or Persona composition.
- Skin detection remains 1 Hz. No animation-atlas handling, arbitrary geometry, legacy 64x32
  UV conversion or Persona piece/tint/geometry reconstruction. Borrowed pixels are not modified
  to force base alpha; unusual alpha skins need game validation. Camera/constraint remain
  deferred and are not part of this skin-only patch (supersedes 0.3.11's planned version).

## [0.3.11] - 2026-10-25

### Added

- First-person player arms at TACZ's `lefthand_pos` / `righthand_pos`. Cache each root-to-node
  path; apply the same current gun pose, then Z+180 degrees as LeftHandRender/RightHandRender
  do. Keep vanilla arm-part pivots, 4-wide/3-wide dimensions and distinct modern left-arm UVs;
  no extra left reflection, body scale or vanilla idle/swing motion. Draw after the gun on its
  shared positioning stack, including the outgoing gun's retained put-away pose.
- Standard wide/slim 64x64 and standard-layout 128x128 local skins, including sleeve layers
  inflated by 0.25 pixels. Read the local player's SerializedSkinRef only on its initialized
  owning thread; copy validated RGBA bytes, never retain Player/native skin/image pointers.
  A 1 Hz probe detects changes (up to one second latency) without per-frame image copies.
  Persona, animated/nonstandard geometry/images, or unavailable skin owners use the installed
  game's Steve/Alex PNG, not a guessed Persona atlas. Fallback reason/layout/source are logged.
- Four cached 24-vertex arm/layer meshes per skin change; per-pose work only updates matrices
  and submits at most four draws. Skin textures use unique Raw world/resource/skin serials;
  existing RenderResourceKey release clears arm meshes/materials/texture through the original
  weak TextureGroup, including world exit and pipeline changes. No new native hooks.
- `[arm-perf]` after 120 frames reports CPU mean/max, skin-probe mean, build/upload total,
  matrix/draw mean and counts; GPU execution is explicitly not timed. `[arm-gpu]` identifies
  skin creation, fallback, material readiness and release. Pure AK47 two-path matrix benchmark:
  about 0.00029-0.00040 ms/frame (no game/skin/GPU); game cost is pending the next runtime test.

### Fixed

- Do not draw vanilla hands/items over a partially completed TACZ draw when skin rendering
  fails; valid current gun sessions own the first-person pass. Menu/stale sessions still pass
  through. No additional ItemStack copy or global menu-time client/skin lookup.

### Not yet

- `camera` and `constraint` effects deferred to 0.3.12. Camera needs coordinated world/hand
  camera consumption without changing actual player rotation; a post-setup matrix edit is not
  established to reach all Bedrock render paths. Constraint needs the not-yet-implemented aim
  weight. Both channels remain excluded from ordinary geometry animation, as before.
- RTX static gun investigation parked by user decision; `docs/RTX-0.3.10.md` is retained.
  This is not an RTX fix. No cache-key/BLAS experiment or opaque-by-value detour was added.
- Native arbitrary/Persona geometry, animated skin atlases, legacy 64x32 skins, armor sleeves,
  and Java skin-layer toggle UI are not implemented. Unsupported skins fall back; if even the
  installed default image is unavailable, skip arms with a warning rather than use stale skin.
- Bedrock alpha-test/blend moving-block materials replace Java entitySolid/entityTranslucent;
  only base-arm texels are forced opaque in our private copy. Lighting, partial-alpha sleeves
  and skin/mount alignment require game validation. No full player-body/jacket is rendered.

### Validation

- 97 tests / 42,607 assertions pass: geometry/UVs/normals/slim/sleeves, ancestor transform
  order/visibility, zero/nonfinite scale, skin admission/alpha/hash and animated AK47 placement.
- Server/client release builds passed; the deploy script successfully installed 0.3.11 with
  Minecraft closed. Deployed DLL/PDB/manifest hashes match the client output. Game appearance,
  skin ownership/material behavior and `[arm-perf]` timing await runtime validation.
- Inherited 0.3.10 runtime results and investigation notes are included in this uncommitted
  round. No new commit. See `docs/ARMS-0.3.11.md` for evidence, limitations and runtime checks.

## [0.3.10] - 2026-10-24

### Fixed

- 0.3.9 crashed at the main menu before any world tests could run. The 19:46:23, 19:47:21
  and 19:49:48 traces all point to GunMeshTraceHook forwarding `origin` at the old
  `GunRenderer.cpp:505`; all three minidumps identify `std::bad_variant_access`. LL's client
  header declares Mesh::_renderMesh's by-value TextureList using `brstd::static_vector`, but
  defines that container as an empty placeholder, not the native container ABI. The detour
  therefore corrupted the texture argument even when its gun-only diagnostic branch was
  skipped. Generic menu meshes also use that globally hooked function. This was not a null
  player/resource service or a world-exit cleanup crash.
- Removed the global `_renderMesh` hook completely, including install/uninstall and native
  metadata/call counters. Keep RTX diagnostics at our own fully typed Mesh::renderMesh call
  site, which takes its TextureVariant by const reference; no guessed container layout,
  per-mode bypass or catch-and-continue workaround.
- World-session/snapshot checks now precede client, player and GPU service queries in gun
  rendering and heartbeat diagnostics. Gameplay queries likewise stop before global client
  lookup when no world session exists; recorded client identity is atomic for input readers.
  A live world with a temporarily incomplete graphics pipeline defers cache creation instead
  of caching a failure or submitting through absent services.
- Reviewed all remaining native hooks against the installed 26.51.1 client SDK headers:
  MinecraftGame::$update `void()`, ClientInstance::_requestLeaveGameImpl `void(bool,bool)`,
  ItemInHandRenderer::renderFirstPerson `void(BaseActorRenderContext&, Matrix const&,
  ItemContextFlags)`. Added compile-time signature checks; ItemContextFlags is a fully defined
  uchar enum, not an opaque by-value class. Matching a declaration alone does not make an
  empty SDK container ABI-safe. Leave cleanup requires an active matching recorded session;
  GPU release uses already-owned cache state, never a menu-time global-client query.

### Validation / still pending

- 92 tests / 21,839 assertions pass, including menu/stale-snapshot and incomplete-pipeline
  guards. Native detour ABI behavior requires Minecraft to test; unit tests do not simulate it.
- Server/client release builds passed; `scripts/deploy-client.sh` deployed the 0.3.10 client
  with Minecraft closed. Main-menu and gameplay validation are pending; no commit was made.
- The 0.3.9 texture-lifetime fix and safe `[anim-frame]` / `[gun-render]` / `[gun-gpu]`
  diagnostics remain. Native-call/metadata fields are intentionally no longer emitted;
  `[gun-render]` now says `trace=call-site-only`. RTX animation remains unresolved.
- 0.3.9's in-game feature checklist was blocked by this startup crash; no pink-texture or RTX
  result is claimed. First re-test main menu for 10 seconds, then all world/RTX checks.
  Evidence and the hook/lifecycle audit: `docs/CRASH-0.3.10.md`.

### Follow-up runtime results

- User validated 0.3.10: no main-menu, join/leave or world-switch crash; no pink texture after
  quitting mid-put-away, quitting while holding, or A -> B -> A. RTX-off actions, gun/item
  switches and fast scrolling all passed. RTX-on gun remains visible but static, with audio.
- Latest run `2026-10-24.22.log` confirms pose publishing, hand-hook calls, mesh rebuilds and
  call-site draw returns under both modes, with zero invalid meshes. In world 3, RTX-on:
  3,290 hooks / 3,275 builds / 3,289 draws over 40.03 logged seconds; RTX-off:
  778 / 777 / 777 over 6.02 seconds. Both mode changes release/recreate GPU cache resources.
- RTX backend consumption/cache invalidation is not established by those CPU counters.
  That investigation shipped no speculative RTX code or deployment; detailed findings/diagnostics:
  `docs/RTX-0.3.10.md`. Follow-up results/analysis remain uncommitted after the approved
  combined 0.3.9/0.3.10 commit `1ee753c`.

## [0.3.9] - 2026-10-24

### 0.3.8 in-game results

- Passed: world entry while holding a gun (no crash, draw plays), normal-looking walk/sprint
  rate, blocked left click/R/H during put-away, gun -> empty hand/item/gun and fast scrolling.
  Re-entering after quitting during put-away had correct animation but a magenta gun texture.
  Newly reported pre-existing issue: Ray Tracing shows a static gun despite playing sounds;
  other graphics modes animate.
- Measured in newest run `2026-10-24.16.log`, not theoretical Java speeds:

  | Time / movement | Speed blocks/s | Java delta/tick | Native delta/tick | Distance phase / controller cycles/s | Stable track sample |
  | --- | ---: | ---: | ---: | ---: | ---: |
  | 19:07:05.542 walk | 4.135 | 0.12457 | 0.20762 | 1.2405 / 1.2442 | 0.99 s |
  | 19:07:41.305 run | 5.393 | 0.16216 | 0.27026 | 1.6180 / 1.6251 | 1.00 s |
  | 19:07:42.305 run | 5.537 | 0.16627 | 0.27712 | 1.6611 / 1.6621 | 1.00 s |
  | 19:07:43.310 run | 5.584 | 0.16835 | 0.28058 | 1.6751 / 1.6843 | 1.00 s |

  Native/Java is 1.66663-1.66669 in these windows: native distance phase would be about
  66.67% higher; Java-normalized input is about 40% lower than native. This validates the
  distance-unit cause in these stable samples, not a universal ratio: other logged windows
  (including ones with zero stable-track sampling) differ substantially. These measurements
  do not establish a general Bedrock speed increase over Java; no speed compensation added.

### Fixed

- Cross-world GPU cache ownership: GunRenderer previously cached textures/meshes/materials
  until mod disable, and a cached display bypassed PNG upload on subsequent world joins.
  The log shows world generations 3 -> 5 -> 7 but only one upload in generation 3. Animation
  snapshots own no GPU handles; the stale renderer cache, not the retained Lua VM, was the
  cause-level lifetime error. It also affects the ordinary-held-gun exit path, not just
  mid-put-away exits (the latter was the in-game reproduction; the former still needs testing).
- Release meshes, TexturePtrs, Raw group textures and materials before request-leave tears
  down the world; use the original weakly held texture group, not a newly looked-up group.
  Exit callbacks compare the recorded client identity instead of depending on a still-registered
  global primary client. Start-join/loaded events and a world/client/group/buffer/pipeline cache
  key provide additional invalidation. Runtime texture names include a world/cache serial.
  `[gun-gpu]` logs bind/create/release once per event. This is code-fixed, not yet game-validated.

### RTX investigation — not fixed yet

- Added low-rate `[anim-frame]` heartbeat independent of hand rendering and `[gun-render]`
  counters for hand-hook calls, mesh builds, draw submissions, native Mesh::_renderMesh calls,
  pose/mesh revisions, validity failures, cache keys and render metadata, labelled by the actual
  FrameBuilder RTX/deferred state. The native trace hook forwards all arguments unchanged.
- Checked LL Mesh/FrameBuilder/RenderMetadata/BVH/RTX headers and Lamium CellMesh/GhostRenderer.
  Existing logs cannot distinguish skipped hand rendering, invalid meshes and stale backend
  geometry; most BVH/RTX implementation types are opaque. No RTX root cause or API impossibility
  has been established. No speculative BLAS/cache-key/temporary flag or alternate rendering
  workaround was shipped. The normal Buffered tessellation/draw behavior remains unchanged.

### Validation

- 90 tests / 21,825 assertions pass, including cross-world/address-reuse/pipeline cache keys.
- Server/client release builds passed; `scripts/deploy-client.sh` deployed the 0.3.9 client
  with Minecraft closed. No 0.3.9 commit was made; GPU/game checks are still pending.
- Runtime checks and remaining RTX investigation: `docs/RENDER-0.3.9.md`.

## [0.3.8] - 2026-10-24

### Fixed

- 0.3.7 crashed on world entry before its other features could be tested. Both
  `trace_2026-10-24_18-25-54.log` and `trace_2026-10-24_18-28-10.log` point to the new
  `GunAnimation.cpp:123` ItemStack snapshot copy from refresh -> reveal -> held -> frame.
  The copy constructor resolves the thread-local ItemRegistry after MinecraftGame::update's
  registry scope has unwound. ItemRegistryRef::_lockRegistry finds no live registry and
  deliberately writes `0xDEADC0DE` to address zero. This is a new 0.3.7 regression, not a
  walk-distance/draw-timestamp arithmetic fault or an absent LocalPlayer.
- Animation snapshots now own only parsed gun state and a deep copy of user NBT. No client
  animation ItemStack is constructed, copied or retained, and Lua NBT reads never follow the
  incoming inventory slot or require a live player. Put-away still retains its old VM/data.
- Explicit client-world lifetime: initialize selections and send draw notifications only after
  ClientJoinLevelEvent (onPlayerLoaded); invalidate work on ClientExitLevelEvent. Clear queued
  inputs/reload replies on transition, tag reload/debug/tick/render snapshots with a world
  generation, and reject stale work across leave/rejoin, including reused engine addresses.
- Stop tracked animation audio before onLevelExit tears down the world. Handles are bound to
  their original non-owning engine reference and world generation; cleanup never looks up a
  new engine to stop old numeric handles, or acquires an engine when nothing was played.
- Server selection/draw timestamps were reviewed: they retain value data only, with state
  cleared on disconnect/server stop; no corresponding new registry-lifetime defect was found.

### Validation

- Both release variants build; all 88 tests / 21,816 assertions pass, including new unloaded,
  leave/rejoin and stale-work generation tests. The native registry assertion itself needs
  Minecraft to reproduce; these tests do not claim to simulate the engine.
- The 0.3.8 client was built and deployed by `scripts/deploy-client.sh` to the LeviLauncher
  test installation with the game closed. No commit was made.
- Crash evidence and the lifecycle review: `docs/CRASH-0.3.8.md`. First re-test entry into the
  affected world, then the full 0.3.7 movement/put-away checklist. Gameplay is not yet verified.

## [0.3.7] - 2026-10-24

### 0.3.6 in-game results

- Passed: draw when switching back, standing idle sway, smooth run_end, inspect and its
  interruption, inspect blocked during reload, reload animation/sound sync, and replayed draw
  after switching away/back. Walk/sprint cycles looked faster than Java TACZ.

### Fixed

- Walk-distance input now has an explicit Java definition: capture each actual client tick's
  horizontal displacement, accumulate `distance * 0.6F`, then expose
  `current + (current - previous) * partialTicks`. 0.3.6 fed Bedrock's
  `ActorMovement::getPredictedWalkDist` directly into Java's distance-driven Lua phase without
  establishing equivalent units. Java 1.20.1 Entity.java:734 and TACZ
  GunAnimationStateContext.java:327-341 define the new computation. The native getter's scale
  and Bedrock's actual walking/sprinting speeds are still unmeasured: no claim of an exact
  5/3 excess rate, or that all of the reported difference has been isolated to this input.
- Kept the Java controller's post-seek frame advancement: each script update overwrites phase,
  so this is not another accumulating 1x cycle. Movement inputs remain tick-driven; queued
  ticks now retain individual displacement and movement classifications below 20 FPS.
- About once per second while moving, `[walk-rate]` logs physical speed, Java/native distance
  delta per tick, distance-derived phase rate, and sampled normalized walk/run controller rate.
  Flat-ground theory: 4.317 blocks/s gives 0.12951 Java units/tick and 1.2951 cycles/s;
  5.612 gives 0.16836 and 1.6836. These are examples, not Bedrock measurements.

### Added

- Retain one old gun ItemStack snapshot and Lua/controller instance during put-away, suppressing
  the incoming vanilla hand/item even if the retained mesh fails. Old ammo/NBT stays attached
  to that snapshot; movement still follows the player. Trigger put_away, exit Lua states, keep
  updating/rendering the controller, then reveal only the latest selection and start its draw.
  Rapid scrolling replaces the pending target, never queues VMs or restarts the exit timer.
- Draw/put-away display sounds; animation sounds are handle-tracked and stopped at handoff,
  without stopping gunshots or unrelated game sounds. Shoot/reload/inspect/fire-select are
  locally blocked during retention and the new gun's visible draw.
- Server draw cooldown after LivingEntityDrawGun: carry `min(elapsed draw, previous put-away)`
  into a future draw timestamp; preserve it across switches during put-away; use the Java
  5 ms cooldown window. Observe authoritative selected slots on ticks, selection notifications
  and actions; switching cancels old bursts/reloads. The client cannot submit durations/IDs.
- Computation, rapid-scroll, return-to-old-slot, clear/world-exit and cooldown carry-over tests,
  plus real AK47 Lua phase-rate tests at 30/60/144 FPS and controller playback after exit:
  86 cases / 21,799 assertions passed. Server and client release builds passed; the 0.3.7
  client was deployed with `scripts/deploy-client.sh` to the LeviLauncher test installation.

### Divergences / validation pending

- The active Java retention caller is bundled SimpleBedrockModel 2.2.2 FirstPersonRenderHandler,
  not LocalPlayerDraw's commented-out keep/doPutAway/scheduled draw calls or TACZ's commented
  equip-retention mixin. SBM reveals on the first START tick after the duration; TACZ's +50 ms
  is a separate state-machine reinitialization guard. This port deliberately retains duration
  **plus a fixed 50 ms**, then hands off on a frame, as requested (AK47: 570 + 50 = 620 ms).
  The client waits for the full visible new draw, even when Java's server formula ends sooner.
- Endpoint displacement per tick approximates Java's sum of Entity.move displacements; a
  teleport/correction can enter the distance sample. Riding, teleports and unusual movement
  need further testing. No movement-speed multiplier was added to hide Bedrock/Java differences.
- Visual/sound suppression, actual native scale, multiplayer ordering and 0.3.7 game feel still
  require in-game testing. Details, source evidence and test checklist: `docs/ANIMATION-0.3.7.md`.

## [0.3.6] - 2026-10-24

### Added

- TACZ Lua animation state machines replace the temporary C++ driver, after
  LuaStateMachineFactory / LuaAnimationState / AnimationStateMachine. Asset modules use
  `<namespace>_<path>` in package.preload; initialize, states, entry/update/exit and nested
  trigger dispatch run the unmodified default / AK47 scripts (and the other default-pack guns).
- Game-independent AnimationController / DiscreteTrackArray / ObjectAnimationRunner semantics:
  ordered track lines, overlapping additive gun kicks, loop / hold / stop, pause / seek,
  quaternion cross-fades and TACZ's actual `1 - (1 - t)^4` transition easing. Sound keyframes
  now run on those same timelines instead of the separate reload scheduler.
- GunAnimationStateContext reads ammo / barrel / fire mode / cooldown / reload state and native
  movement input, walk distance, ground, crouch and sprint state. Movement inputs are dispatched
  from ClientLevelTickEvent; state updates and animation evaluation stay per frame.
- H inspects, following LocalPlayerInspect's state-lock checks. Draw, shoot, reload,
  cancel_reload, fire_select, idle/walk/run and put_away inputs are dispatched to Lua. Fire mode
  is predicted locally until sync; draw time locks local actions. `/tacz anim <name>` now plays
  one debug override and returns control to the continuously updating state machine.
- Internal rifle/pistol fallback animations and the default state-machine script are embedded
  from TACZ's original assets; gun packs override them. Display state_machine_param and a
  read-only primitive/compound NBT accessor are available to scripts.
- A separate animation Lua VM with text/preload-only loading and an instruction budget per
  invocation, isolated per held-gun session. LuaJ compatibility shims provide unpack, math.pow,
  bit32 and integral number formatting. Script errors identify the module and fall back to
  static_idle, not a game crash.
- Controller / sandbox / real-pack sequence tests; all 54 default-pack guns' state machines
  initialize and accept action inputs (76 cases total).

### Not yet

- Delaying Bedrock's actual item switch for put_away_time: put_away and exit are dispatched,
  but selection changes immediately, so the old gun is not kept onscreen for the holster animation.
- Aiming, crawl, slide, heat, charge and attachments (including extended-magazine modifiers)
  return neutral context values; manual-action bolt and melee gameplay inputs are not wired.
- Arms, camera/constraint bone listeners, functional additional-magazine rendering and shell
  ejection rendering. Crosshair-hide flags and shell-window requests are stored/logged only.
- glTF animations and spherical interpolation on non-rotation channels remain unsupported.
- Lua module globals shared across gun-selection sessions are not retained: a fresh animation
  VM isolates a faulty pack from the next selection instead of sharing Java's asset-script VM.

## [0.3.5] - 2026-10-24

### Added

- First-person bone animation playback after TACZ's AnimationKeyframesSerializer,
  Animations, ObjectAnimationChannel and CustomInterpolator: linear / Catmull-Rom spline,
  pre/post keyframes, scalar scale, translation units and additional rotation / scale after
  the rest pose (BedrockPart and ModelTranslate/Rotate/ScaleListener).
- Temporary C++ tracks driven by the existing MinecraftGame::update frame hook: looping
  static_idle, draw on selection, tactical / empty reload, additive shoot kick. Named debug
  playback via `/tacz anim <name>`; unknown names list the held gun's clips in the client log.
- Pack animation loading and missing-name fallback via default_animation / use_default_animation
  when the referenced file is present in a gun pack. Loop / hold / stop playback boundaries,
  real AK47 parsing, rest-pose regression and interpolation tests (57 cases total).
- Mesh rebuild CPU timing logged once after 120 changing poses; static poses reuse the mesh.

### Not yet

- Lua animation state machine / transitions (0.3.6), player arms and camera / constraint
  listeners (0.3.7), aiming / sprint / fire-select animation tracks.
- Bundled rifle_default / pistol_default animation assets (use_default_animation currently
  needs those files in a gun pack), glTF animations, spherical interpolation on non-rotation
  channels, functional additional-magazine rendering. Unsupported Molang is logged and skipped
  (string vector components become zero, as in TACZ).

## [0.3.4] - 2026-10-24

### Added

- Reload (R, TACZ's default key), after LivingEntityReload / LocalPlayerReload:
  - The server runs the gun's Lua logic script every tick (AK47: `xmag_reload_logic.lua` with
    its `script_param` timings), or TACZ's default reload when a gun has no script. Tactical
    reload keeps the chambered round; empty reload chambers one from the new magazine.
  - The reload state is sent to the client on every change; no shooting while reloading,
    switching weapon stops the reload, pulling the trigger sends TACZ's interrupt.
  - The client plays the reload animation's sound keyframes (magout, magin, ...) at their times.
- Embedded Lua 5.4 + sol2 (`src/lua/GunLogic.*`) after TACZ's ScriptManager: modules in
  `package.preload` as `<ns>_<path>`, TACZ's reload-state and fire-mode constants, sandboxed
  (no io/os/file loading/bytecode, instruction limit per call). All 10 default-pack gun logic
  scripts load.
- Ammo: items carrying TACZ's `AmmoId` NBT count as ammo; survival reloads take them from the
  inventory, creative reloads are free. `/tacz ammo <count>` gives ammo for the held gun
  (placeholder look: iron nugget).
- Probe HUD shows reserve ammo and reload state; the server ping moved to Ctrl+P.
- Unit tests: reload timelines (default and AK47 script), sandbox, animation sound keyframes
  (42 cases total).

## [0.3.3] - 2026-10-23

### Fixed

- Uneven full-auto fire rate (0.3.2 test: intervals jumped between 50, 100 and 150 ms). The
  trigger was polled through LeviLamina's ClientThreadExecutor, which only runs once per 50 ms
  client tick. It now runs every frame from a `MinecraftGame::update` hook (as Lamium does), and
  burst follow-up sounds are timed per frame too. The frame hook rate is logged once per world.

### Changed

- Bullets pass through iron bars, fences, fence gates and leaves, as TACZ's
  `tacz:bullet_ignore` block tag does (matched by block name; Bedrock has no block tags).
- Bullet entity collision box is 0.0625 x 0.0625, TACZ's entity size. Hit detection does not
  use it (bullets are rays, as in TACZ). Pack change: restart the game.

## [0.3.2] - 2026-10-23

### Added

- Shooting (`src-server/server/Shooter.cpp`, `src-client/client/gameplay/ClientShooter.cpp`,
  rules in `src/gun/ShootRules.h`), following TACZ's ShootKey / LocalPlayerShoot /
  LivingEntityShoot / ModernKineticGunScriptAPI.shootOnce:
  - Holding the left button fires at the gun's rate in AUTO (and BURST with continuous_shoot);
    SEMI and BURST need a new press. G cycles the fire mode (TACZ's default key).
  - The client checks its cooldown, plays the shoot / dry-fire / fire-select sound right away
    and sends a timestamped shoot packet; the server checks the cooldown (5 ms window), the
    client clock (±300 ms), chambers a round for closed bolts, consumes ammo per bolt type and
    fires. Bursts spread their rounds over server ticks.
  - Bullet spread from TACZ's inaccuracy (stand / move / sneak, fire mode addends) with
    vanilla's triangular jitter; bullets inherit the shooter's motion.
  - Bullet speed now includes TACZ's default global modifier (x2): the AK47 flies 25 blocks/tick.
- Gun item state uses TACZ's NBT encodings (`src/nbt/GunItem.*`): `GunFireMode` as the enum
  name, `HasBulletInBarrel` as a byte. `/tacz setgun` hands the gun out as TACZ's creative tab
  does: first fire mode, full magazine, round chambered.
- Display sounds get TACZ's defaults (`dry_fire`, `fire_select`, hit and melee sounds).
- Unit tests for the shooting rules (36 cases total).

### Changed

- The probe no longer fires on click; R still pings the server, and the HUD shows fire mode
  and chamber state.

### Not yet

- Reload, bolt action, aiming, draw / sprint locks, heat, Lua `shoot` overrides.

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

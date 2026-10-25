# 0.3.16 — camera roll, held-sprint recovery, rendering investigation

Parent: `7ebd4d2` (0.3.15). **No new in-game run was performed by the agent.**
Implementation/build evidence is not a claim that the world image visibly rolled, sprint
resumed in vanilla, or GPU jitter was eliminated. Those remain the acceptance tests below.
No renderer rework, attachable, hybrid renderer, skin workaround or source-geometry edit
is included. The mesh-once/bone-matrix work stays scheduled for **0.3.19**.

## User's 0.3.15 results (not an agent-run test)

- Correct: recoil direction, ADS position, aimed sway and aimed reload.
- Incorrect: no camera roll/shake; held sprint does not resume after releasing aim.
- Freeze reduces many flickering regions to two. Depth modes 1/2 appeared unchanged and
  did not visibly clip. Gun faces show frequent small, apparently independent shifts.

## Camera: cause, reference, implementation and safety

`2026-10-25.8.log:45` contains only
`[camera-bone] setup/dependencies/crosshair hooks=true/true/true`. There are **no**
`before-dependencies`, `finalViewRetained` or failure-warning lines in the searched logs.
There is no logged consumed flag to interpret as false. Installed hooks do not prove entry.

The user's current options file has `gfx_viewbobbing:0`, `gfx_damagebobbing:0`,
`camera_shake:1`. Both 0.3.15 camera and hand consumers explicitly returned/skipped when
`getBobView()` was false. This proves a suppression path consistent with the missing sample
lines; it does **not** prove that the old dependency callback ran and its matrix was ignored.
Neither the options file nor those game settings were changed.

Studied [Do-A-Barrel-Roll-Bedrock Camera.cpp][barrel] at
`15127ea4b0b8fc741954707375ef551ce25326de`. Its README targets Bedrock **1.26.51.01**,
LL **26.51.6 client**. It is GPL-3.0 (compatible with this repository's GPL-3.0).
It uses LL **client-native hooks**, not server packets, commands, Molang or camera API calls:

| Installed 26.51.7 header signature | Role |
| --- | --- |
| `void LevelRendererPlayer::setupCamera(mce::Camera&, float)` | Inject render component orientation before native camera construction |
| `void InGamePlayScreen::$_renderLevelPrep(ScreenContext&, LevelRenderer&, Actor&)` | Hold injected orientation through native preparation, then restore |
| `void InGamePlayScreen::$render(ScreenContext&, FrameRenderObject const&)` | Hold through native rendering, then restore |

We use this render-component timing, with our existing TACZ x/y/-z camera listener,
log-space multi-track blend, uncapped sqrt(zoom) ADS scaling and Java camera-angle addition.
The component is camera-to-world, hence transpose to view rotation, apply Java angle
increments, transpose back and preserve the quaternion hemisphere. Identity animation is
an exact no-op (no Euler round-trip). Position, real look recoil, FOV and ADS/constraint
math are not repurposed or rewritten. Hand/gun camera rotation uses the same independent
`config/aim.json` **`"camera_bone_animation": true`** option, read on enable.

Safety differences from the reference:

- Every hook has an exact installed-header `static_assert`. Only native references and
  float cross these boundaries; no opaque by-value placeholder hook is used.
- Active world, matching immutable world/client/gun snapshot, live HUD eligibility and
  first-person perspective are required. No world service is queried from the main menu.
- Existing ECS storage is queried via **const registry** APIs. Non-const `get/try_get`
  instantiate storage and caused an initial unresolved native CameraComponent constructor;
  no native component is constructed, added, removed or layout-guessed by this adapter.
- entt type hashes are checked (`CameraComponent=0x4f6047c7`,
  `RenderCameraComponent=0x119e772b`). Select a unique tagged render camera, filter by
  camera-client local player when that component exists, and skip ambiguity.
- TLS RAII prep/render scopes contain all saved ownership. Before another setup (including
  a rejected one), and at scope exit/exception, restore only within the same live world,
  registry identity and valid entity, and only if orientation still equals our injection
  (including the equivalent negative quaternion). Never overwrite a later writer's value.
  Nested scopes close earlier ownership; no pending owner survives a callback/frame.
- No DirectLook/input/player-orientation component is written. No matrix fallback is used
  when the render camera or outer scope is unavailable. Custom camera/mod conflicts remain
  a runtime compatibility risk, not an excuse to force player look.
- Normalize the snapshot's ClientInstance identity to its explicit IClientInstance base.

New `[camera-bone]` lines show entry/rejection/noScope/noCamera/injection/restoration counts,
subsequent-writer conflicts, Bob setting, own setting, listener angles and interval peak roll.
`nativeViewOrientationError` compares the returned native view's 3x3 to the inverse injected
orientation; **-1 means unavailable/not injected**, not failure. Extra native camera effects
can produce a nonzero error. Even zero error is CPU camera evidence, not proof of GPU/world
image consumption. Restoration usually appears in the following sample (scope has not
ended at the moment of logging). No animation means no unnecessary component write.

## Sprint: preserve input, filter state

0.3.15 permanently cleared `SprintDown` in raw and processed input. A held key normally
does not emit another down event, so releasing ADS could not recover it.

Java `LocalPlayerSprint.getProcessedSprintStatus`, `LocalPlayerMixin.aiStep` and
`LivingEntityAim.tickSprint` filter/stop sprint while aiming or non-finishing reload;
they do not clear the held key. `model::withSprintMasked` now masks those two latches only
around native input-state processing and restores their exact prior values via RAII.
Extracted Sprinting is suppressed before/after native processing, and the existing
player-specific setSprinting filter remains. When ADS/reload blocking ends, vanilla sees
the held key again and evaluates its own forward movement, hunger, collision, sneak etc.
We do **not** call setSprinting(true), synthesize a key-down, bypass eligibility or resume
when the key has actually been released. Tests cover all held/released latch combinations,
unrelated bit changes and exceptions. Native re-engagement still needs an in-game test.

## Jitter: numerical evidence and exact limits

`GeoModel.cpp::bakeBone` starts from identity, multiplies model bone transforms and applies
`quad.pos/16`. The resulting **float32 model-space positions** enter
`Tessellator::vertex(x,y,z)`; world/view/common ADS transforms are submitted separately.
There is no world position in CPU vertex baking, nor a large world translation followed
by tiny model offsets in that bake. Native draw matrices must still be measured in-game.

`tests/RenderDiagnosticsTests.cpp` loads the actual AK47 geometry/animations/visibility:

| Offline release measurement | Result |
| --- | ---: |
| Visible vertices | 7,608 |
| Shoot sample at .0333 seconds, position FNV-1a hash | `fcac86262082fe61` |
| Maximum absolute position component | 1.5364 blocks |
| Same pose baked 240 additional times | identical hash; max component delta **0** |
| Shoot sampled at 240 Hz, 121 frames | 120/120 adjacent pairs changed |
| Maximum adjacent position component change | .0484827 blocks (authored animation, not measured jitter) |
| Maximum adjacent quad-edge length change | 3.57628e-7 blocks |

At 1–2 blocks a float32 ULP is 1.19209e-7 blocks. The edge variation is approximately three
ULPs, not evidence of frequent random face motion. This deterministic fixed-pose replay
rules out non-deterministic baking **for this tested sample/build**; it is not a live
freeze-on/off GPU capture and does not rule out every live state/transform issue.

**The reported per-face jitter's root cause is not yet established. No speculative jitter
fix is shipped.** It is not justified to blame half-float packing, TAA, depth or RTX from
these CPU measurements. Mesh-once + per-bone draw matrices should remove repeated animated
vertex upload and address the independently observed RTX stale-internal-mesh path; it is
not proven to fix this jitter, draw-matrix instability, GPU reconstruction or coplanarity.
An extra camera-relative CPU bake is not justified: the bake is already small model space.

### Safe, opt-in live diagnostics

`/tacz rdebug mesh 1` enables `[mesh-jitter]` (integrated single-player only); `mesh 0`
disables it. It does not change mesh topology, upload mode, materials, visibility or draw
transforms. Hashing/copies and a once-per-second repeat bake are intentionally opt-in.

- Per draw: hash explicit position float bits (no struct padding), previous/current hashes,
  maximum consecutive component delta and topology-change count over the interval. Non-finite
  positions/matrix entries produce an infinite delta, not a misleading zero.
- Once per second: re-bake the same immutable pose and compare with submitted CPU vertices.
- At actual upload: compare **every** Tessellator MeshData float32 position to its intended
  source, including both-winding mapping. `tessellatorMaxDelta=-1` means not measured or
  count mismatch (e.g. diagnostics enabled only after a frozen mesh was built).
- Record documented `Mesh::mRawData` byte count/hash after upload. Do not force HAL
  generation, mutate native buffers or reinterpret `VertexLayout`: the installed header is
  literally `struct VertexLayout {}` despite the native typed-storage reservation.
  Raw bytes may be absent; byte count/hash alone cannot establish stride, offsets or position
  encoding. **Downstream native/GPU position packing remains unknown** until a supported
  native layout query or external graphics capture establishes it.
- Track maximum consecutive component change in actual `Pnative*V*W*Tsubmitted` and log
  native view/world translations. A moving camera/FOV legitimately changes this matrix;
  compare with a stationary settled camera as well as with frozen pose.
- Freeze suppresses animation publication **and common transform updates**. A stable vertex
  hash with persistent image jitter would narrow the problem beyond CPU pose baking, but
  would still not identify a particular GPU cause. Matrix stability and packing evidence
  must be examined separately. Raw upload data in a frozen/reused mesh is not a fresh upload.

## Two remaining regions and Java comparison

The user's two visible regions cannot yet be uniquely assigned to cubes: freeze retains
all source geometry, and there are many positive-area coplanar pairs. Read-only
`python scripts/analyze-ak47-geometry.py` reports 32 pairs in bolt, 113 in cover2 within
.002 model pixels (not screen pixels), with other bones containing candidates too.
Zero-based examples already independently checked against Java cube/pivot formulas:

| Candidate, NOT a pixel identification | Source separation |
| --- | ---: |
| cover2 12.west/55.west and 13.west/48.west | <=1.20664e-6 model pixels = 7.5415e-8 blocks |
| bolt 4.north/5.north, also /16.north | 0 (coplanar) |

Need a marked screenshot/fixed pose plus selective face/bone isolation or an ID render to
identify the actual two. None is deleted/nudged/inflated speculatively in this version.

Java `BedrockCubePerFace.compile` traverses polygons EAST/WEST/DOWN/UP/NORTH/SOUTH,
divides by 16, transforms float32 vertices through the current PoseStack, and writes to
VertexConsumer **each draw**. Java is not inherently mesh-once either. Our within-cube
face/UV/winding order matches the source tests. `BedrockPart` draws its cubes then children;
rotated-cube helper children in our conversion can change the **between-cube/bone**
submission grouping. Equal-depth ties may depend on that order and backend rounding.
This is a plausible difference, not measured proof of either remaining region.

Java gun wrapper chooses `RenderType.entityCutout` (or entityTranslucent for transparent
displays), i.e. the usual culled cutout/depth-writing path for the AK47. Bedrock currently
uses the user-visible `moving_block_alpha`, single-sided by default. Its 0.3.15 descriptor
logged test=true, func=3, writeMask=1, bias=100, slopeBias=.1; these legacy fields are not
proof of final RenderDragon GPU state. Culling does not remove same-facing overlapping
coplanar faces, and Java contains no universal per-face epsilon/depth fix in the audited
cube path. Cannot claim Java has no such overlap or that merely copying face order fixes it.

The earlier depth experiment was confounded: changing depth while freeze=1 leaves the
captured common transform unchanged. New projection logs distinguish selected `depthMode`
from **`capturedDepthMode`**. Unfreeze, select depth, allow frames, then refreeze for each
comparison. No shared depth buffer or material is modified by this investigation.

## Attachable evaluation against fixed requirements

Sources: [official attachables/binding][attach], [definition][definition],
[render-controller arrays][controllers], [vanilla first-person player controller][player],
[Bedrock Wiki's separate player-hand animation warning][wiki]. The wiki warning is not
an official promise of Persona compatibility. This is a design evaluation, not a prototype.

| Requirement | Attachable + render controller feasibility / required work |
| --- | --- |
| One `tacz:modern_kinetic_gun` + NBT GunId | One attachable can reference arrays of loaded geometries/textures. Documented item association/queries do not expose arbitrary custom GunId NBT. Need a **C++ bridge** exporting a per-render-instance gun index to Molang/custom query context. Per-gun item IDs avoid selection but violate the fixed requirement; not recommended. |
| Runtime gunpacks, no converter | Engine needs registered resource-pack geometries, textures, controller and animation aliases. Generating/loading a pack at startup could pre-register them if done before resource loading; this timing is unproven here. It is still an **internal conversion/registration pipeline**, not direct arbitrary TACZ runtime loading. With strictly no converter it does not satisfy the requirement. Hot-added guns require reload/registration work; no automatic engine lookup under mods/tacz/gunpacks. |
| TACZ Lua multi-track blended animation | Keep Lua/controller in C++, export final per-bone transforms into generated Molang channels or inject evaluated native pose before rendering. Native controllers support layers/blends, but are not equivalent to our track/transition/recovery/log-quaternion semantics. Porting Lua is a rewrite; a per-bone bridge is substantial/version-sensitive and must be render-instance scoped. |
| Constraint and camera | Camera bones rotate geometry only by default, not the actual view. Keep native camera adapter. Constraint is listener/ADS compensation, not an ordinary animated bone; compute in C++ and bridge the common root/arm transform. No automatic TACZ semantics. |
| Keep old gun through put-away | Held attachable normally follows the current equipped item. Need a separately retained visual instance or render-only equipped-item/geometry override and old state snapshot, not a gameplay inventory delay. Not solved by an ordinary single-item controller alone. |
| Muzzle, shells, extra mag, future attachments | Geometry bones/locators, particles and child attachables can express parts. Native/C++ integration still needed for correct evaluated transforms, spawning/gameplay events, dynamic attachment registration and retained-item ownership. Not automatic from functional bone names. |
| `bullet_in_barrel`, `mag_extended_N`, attachment slots | Controller `part_visibility` can use exported per-instance booleans/indices; all candidate parts/resources must be loaded. Bridge TACZ inventory/render state and hierarchical visibility explicitly. |
| Real-skin first-person arms, including Persona | **Actual player renderer** can use its own skin/material/geometry/composite layers. Vanilla controller already exposes arm/sleeve visibility, but a held item normally hides arms. Need player animation/visibility integration and TACZ two-hand pose bridge, with conflicts against other player packs. Attachable binding inherits parent transforms; it does NOT copy the player's current skin/composite texture into the gun or drive the parent's arms. Fixed Steve/Alex cubes + `Texture.default` on the gun are not a Persona solution. Persona arbitrary geometry/layers need native validation; not guaranteed by editing player JSON. |
| RTX | Normal attachables/players use native entity rendering, making them a credible RTX route. Must validate first-person material, resource loading, live bone bridge, retained instances, skin and shadows in 1.26.51.01. It is not evidence that a new C++ bridge is consumed correctly by RTX, or that native rendering removes source coplanarity/jitter. |
| Performance | Native geometry caching/skinning avoids our repeated animated mesh upload, but keeping Lua and exporting all bone channels adds CPU/context work. Generated assets increase startup parsing/memory; retained visual instances and material/attachment splits add draws. No benchmark of this proposed route exists, so no FPS win is claimed. |

| Route | Trade-off / recommendation |
| --- | --- |
| **1. Keep renderer, static model-space meshes + bone matrices** | Recommended main route for 0.3.19. Best preserves one-item/NBT/runtime packs/Lua/listeners/retained gun. Existing arm path and whole-gun matrix are already user-observed under RTX. Still needs resource/visibility/performance testing and does not promise a jitter/overlap fix or automatically solve skin/Persona. |
| 2. Switch to attachables | Native resource/animation/RTX advantages, but adds generated resources and multiple substantial bridges, weakens strict no-converter/runtime loading, and does not solve retained gun or camera semantics. Reject as the primary route under current requirements. |
| 3. Own gun + native player arms | Worth a **separate future feasibility experiment** for native skin/Persona composition; requires safe first-person player draw, two arm pose adaptation, common ADS/camera timing and preventing duplicate arms. Vanilla controller override risks resource-pack conflicts. Do not treat it as an already solved skin fix or tie the gun rewrite to it. |

### Three-route requirement/performance comparison

| Requirement | 1. Own renderer + rework | 2. Attachable | 3. Own gun + native arms |
| --- | --- | --- | --- |
| One item + GunId NBT | Keep current mapping | C++ per-instance Molang/index bridge; not per-gun item IDs | Keep current gun mapping |
| Runtime packs, no converter | Keep native TACZ loader | Generated/registered pack required; strict no-converter fails | Gun loader unchanged; arm bridge must handle arbitrary gun bones |
| Lua/multi-track blending | Keep evaluated poses | Export final pose; do not substitute non-equivalent native blending | Keep Lua; adapt two hand paths to player pose |
| Constraint/camera | Keep listeners/common draw transform | Keep C++ listeners and camera hook, bridge common root | Same camera/common timing required across two renderers |
| Retained old gun | Keep current snapshot | Separate retained native visual or render-only equip override | Keep gun snapshot; retain matching arm pose |
| Functional bones/attachments | Keep evaluated paths/visibility | Locators/parts + C++ events and loaded attachment resources | Keep gun functions; native hands must not interfere |
| Runtime visibility | Keep rules, select cached bone draws | Export booleans/indices into part_visibility | Keep gun rules; suppress unwanted native player parts |
| Real skin / Persona | Not solved by gun rework | Player renderer integration, not attachable binding alone; Persona unverified | Best candidate to reuse native composition, but unverified pose/geometry integration |
| RTX | Existing whole-gun/arm matrix evidence; new path still needs tests | Credible native entity pipeline, bridge consumption unverified | Both pipelines and matching poses need tests |
| Performance | Avoid O(vertices) baking/upload, but per-bone draws can **increase draw calls**; batching/palette design needs profiling | Native cache/skinning vs Lua-to-pose bridge, startup/resource overhead; no measured speedup | Gun costs remain unless reworked; player pass/pose bridge adds CPU/draws; no measured speedup |

Existing offline baseline (not end-to-end GPU time): AK47 Lua/controller about .015–.020
ms/frame, evaluate+bake about .051–.060 ms/frame for 7,608 vertices, two cached arm paths about
.00026–.00040 ms/frame on this machine. Those microbenchmarks exclude native upload, draw
submission, GPU execution and skin composition; they cannot rank the proposed routes' FPS.

## Decisions for the user

1. Preserve user-confirmed recoil/ADS/constraint/aimed reload behavior; no speculative retuning.
2. Camera animation defaults on independently of walking Bob View. This intentionally
   differs from Java's Bob gate to meet the user's visible gunpack-camera expectation while
   respecting disabled walking bob; the new own option allows disabling it without editing
   game options. Keep real-look recoil separate; skip unsafe/ambiguous camera paths.
3. Preserve held sprint and let vanilla decide eligibility. Do not force sprint on release.
4. Report jitter/packing and exact two-region identity as **unresolved pending runtime
   evidence**, not invent a GPU cause, silently change source geometry, or implement 0.3.19.
5. Recommend renderer rework as route 1, evaluate native arms separately later; do not
   silently weaken one-item/no-converter/Lua/Persona requirements to justify attachables.

## Validation and deployment

Final validation (2026-10-26): server/client x64 release builds passed; **130 tests /
47,989 assertions**, no failures or skips. `git diff --check` passed. Commands:

```powershell
xmake build tacz-tests
xmake run tacz-tests
xmake f -a x64 -m release -p windows --target_type=server -y
xmake build tacz
& "E:\Programs\Git\bin\bash.exe" scripts/deploy-client.sh
```

The deploy script configured/rebuilt the client and deployed **0.3.16 / client** while
Minecraft was closed. Compared SHA-256 for DLL, PDB and manifest, and exact relative-path/
hash maps for all 4 resource-pack and 2 behavior-pack files: all match output/source.
Final client DLL SHA-256:
`f8c0ba02654660b9ec4a05ffde1e2b44b807c6e232913d51e7981700fd4f841a`.

Known compile warnings are LL hook-macro `_AutoHookCount` unused variables (also present
in existing hooks). No hook was skipped, no push/amend/reset was used. Deployment uses
`scripts/deploy-client.sh`, which refuses if Minecraft is running; do not terminate it.
Destination: `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz`.

## 0.3.16 acceptance / evidence collection

1. Start from main menu; enter world, draw AK47 with Bob View **still off**. Fire/inspect/
   reload, hip and ADS: watch a fixed world edge for actual camera roll, not just gun motion.
   Expect all setup/prep/render hooks true; active shots increase injected/restored and
   interval peakRollDeg (AK47 un-aimed .0333 s authored key is -1.025 degrees). noScope/
   noCamera must not continually grow for an ordinary active shot. Save full logs, not
   just install lines. Check nativeViewOrientationError without interpreting it as GPU proof.
2. Set own camera option false and restart to compare (recoil must remain real); restore
   true afterward. Third person, inventory/pause, focus loss, gun switch, death, world exit/
   re-entry must leave no stuck roll. Check put-away camera animation on the retained gun.
3. Hold forward+sprint -> hold aim -> release only aim: sprint should stop then resume
   without re-pressing sprint. Release sprint during ADS: no resume. Check blocked-forward,
   sneak, low hunger/sprint eligibility, finishing/non-finishing reload and toggle ADS.
4. Recheck 0.3.15 successes: recoil direction/limit, ADS size/axis, aimed sway/reload, FOV,
   sensitivity and gun/arm agreement. Repeat ordinary/RTX rendering if available.
5. In a stationary settled view, run `mesh 1`, then `freeze 0`; wait >=3 seconds each at
   idle, firing, aimed reload, and record visible jitter. Run `freeze 1`, wait >=3 seconds,
   record whether **individual faces still shift**, not only the number of flickering areas.
   Capture both `[mesh-jitter]` groups; frozen pose should have changes=0 and delta=0 after
   transition. RepeatMaxDelta should be 0, TessellatorMaxDelta 0 when measured; matrix delta
   must be evaluated with a fixed camera and settled FOV. Record RTX flag in `[gun-render]`.
6. For each depth mode 0/1/2: `freeze 0` -> `depth N` -> wait frames -> `freeze 1`.
   Confirm capturedDepthMode=N; take the same marked screenshots of the two residual spots.
   View motion/frozen transforms can differ, so avoid calling an unmatched view a controlled
   comparison. Graphics capture is still needed for final vertex packing/depth state proof.
7. Cleanup: `/tacz rdebug freeze 0`, `/tacz rdebug depth 0`, `/tacz rdebug mesh 0`.
   Diagnostics are process-local, integrated-client only; they do not operate across BDS.

[barrel]: https://github.com/theaddonn/Do-A-Barrel-Roll-Bedrock/blob/15127ea4b0b8fc741954707375ef551ce25326de/src/mod/hooks/Camera.cpp
[attach]: https://learn.microsoft.com/en-us/minecraft/creator/documents/attachables?view=minecraft-bedrock-stable
[definition]: https://learn.microsoft.com/en-us/minecraft/creator/reference/content/attachablereference/examples/attachabledefinitions/attachable?view=minecraft-bedrock-stable
[controllers]: https://learn.microsoft.com/en-us/minecraft/creator/documents/behaviorrendercontrollers?view=minecraft-bedrock-stable
[player]: https://github.com/Mojang/bedrock-samples/blob/main/resource_pack/render_controllers/player.render_controllers.json
[wiki]: https://wiki.bedrock.dev/items/attachables

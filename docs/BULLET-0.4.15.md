# 0.4.15 — production-default client-driven native bullets

2026-10-31. **Production = server every5 + always-on client drive.** The user visually
accepted smoothness with F3+B after the interpolator/age fix, including temporary-pool
water agreement. The final production-default build and rejoin fix still need live
regression acceptance (roadmap: 待实机验收). Version remains 0.4.15. This finalization
performs synthetic tests/builds only: no Minecraft/server launch, deploy or push.

`J/` = `_reference/TACZ/src/main/java/com/tacz/guns/`.

## Java mechanism and correction semantics

- `J/item/ModernKineticGunScriptAPI.java:179–193` creates each authoritative pellet.
- `J/entity/EntityKineticBullet.java:87–88,592–645`: Forge additional spawn data
  includes double velocity, owner, IDs, gravity, friction, lifetime and visual flags.
  Tracking is five chunks with update interval five; ordinary velocity updates are off.
- `EntityKineticBullet.java:243–289`: client and server move by velocity, then apply
  friction and gravity; in water use friction `0.4F`, gravity `gravity * 0.6F`.
  `(1 - friction)` is evaluated as float before promotion to the double vector scale.
- `EntityKineticBullet.java:293–357,478–513`: collision/removal decisions are server-only;
  local lifetime expiry also discards the client entity. No client collision prediction.
- `J/client/renderer/entity/EntityBulletRenderer.java:46–129`: partial-tick orientation,
  optional ammo model, tracer position and first-person offset. Production versions of
  those visuals, flight particles and bullet holes are NOT implemented in 0.4.15.
- Vanilla 1.20.1 [ServerEntity.java:110–175](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/blob/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft/server/level/ServerEntity.java#L110-L175)
  can send position updates independently of the velocity-update switch. TACZ does not
  override `lerpTo`; vanilla [ClientPacketListener.java:562–605](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/blob/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft/client/multiplayer/ClientPacketListener.java#L562-L605)
  calls `lerpTo(...,3,...)`, but inherited [Entity.java:1965–1968](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/blob/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft/world/entity/Entity.java#L1965-L1968)
  **snaps** position and rotation, not a three-tick blend. Rendering separately interpolates
  tick positions ([LevelRenderer.java:1430–1436](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/blob/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft/client/renderer/LevelRenderer.java#L1430-L1436)).

## Exact runtime controls

Enter these slash commands in a world. Client commands use LL's client registrar
(`SyncLocal`), never a command sent to the remote server. Each viewer has its own marker
setting and diagnostics. There is no sync/drive mode selection or activation command.

| Command | Meaning / default |
| --- | --- |
| `/taczbullet marker 0` / `1` | Local yellow cube off/on; **default 0**, survives world exit |
| `/tacz bullet status` / `/taczbullet status` | Confirm server/client settings and active counts |
| `/tacz bullet diag 4` | Server: arm next four pellets from issuing admin |
| `/taczbullet diag 4` | Client: arm next four received pellets (including other shooters) |
| Either diagnostic command with `0` | Stop current diagnostics and disarm future sampling |
| `/tacz bdebug 0 1` | Existing server `moveTo`, full speed; **default** |
| `/tacz bdebug 1 1` | Existing server teleport, full speed |
| `/tacz bdebug 0 0.05` | moveTo, slow newly spawned bullets to 5% for inspection |
| `/tacz bdebug 1 0.05` | teleport, same slow launch speed |

Server commands require GameDirectors permission. Speed changes only affect new launches;
change between bursts for comparable evidence. Marker changes affect active bullets.
`/tacz bullet sync` and `/taczbullet mode` were removed along with off/tick/drive-filter paths.

## Production implementation and known differences

- `src-server/server/BulletSystem.cpp` always ray-traces and steps authoritative state
  each server tick. Only native actor placement is throttled. Other server mods see a
  stale actor position between placements in `every5` (up to four ticks, roughly 100
  blocks at 25 blocks/tick), NOT the authoritative simulated position. This is the
  expressly accepted production difference; it is not Java server-entity parity.
  Every client uses this schedule, without capability negotiation or per-client tick
  fallback. Unmodded clients or clients whose drive path is unavailable can see about
  five ticks (250 ms) at the muzzle followed by jumps. The user accepted this limitation;
  those clients also lack the client gun-model renderer.
- `src/net/BulletPacket.h` / `src/gun/BulletExperiment.h`: fixed 91-byte versioned payload,
  serial, runtime ID plus unique ID, dimension, double position/velocity, float gravity/
  friction, lifetime and water-rule flag. Spawn goes only to same-dimension players
  within Euclidean 80 blocks of the launch position. Server removal goes to those original
  recipients. There is no projectile count cap and no client damage/hit decision.
  A separate 45-byte placement-age metadata packet is sent BEFORE each subsequent
  native placement (including final placement), to those same recipients. It contains
  identity, dimension, float native target and authoritative position age, not a
  client prediction or velocity correction. The spawn itself supplies age zero.
  Native MoveActor packets remain the correction source. Use matching updated server
  and client DLLs even though the mod version remains 0.4.15.
- Client state uses IDs, never retained Actor pointers. A custom packet inbox is consumed
  at the existing signature-asserted POST-`ClientLevel::$_subTick` hook in
  `src-client/client/sound/GunSound.cpp`; no additional tick or render hook is introduced.
  Driving is automatic for each tracked actor when startup hooks and its interpolator
  are available; no activation command is needed after entering or rejoining a world.
- `src-client/client/BulletExperiment.cpp`: drive calls supported `Actor::_setPos()`
  for current position/AABB, then writes `StateVectorComponent::mPosPrev` from the prior
  simulated position. It ALSO overrides the existing native interpolator;
  see the supported access path below. Native velocity is deliberately NOT set to the simulated velocity
  (which could cause double movement). `getInterpolatedPosition(.5)` is sampled for evidence.
  Existing first-person `gun-render` is unchanged; the regular native entity renderer
  renders the optional cube. The user accepted F3+B smoothness in the supplied live run;
  the final default build remains subject to regression checks below.
- Two typed, signature-asserted `LegacyClientNetworkHandler::$handle` hooks remain:
  passive `MoveActorAbsolutePacket` and `MoveActorDeltaPacket` observers. Both always call
  `origin()`; **no native packet is filtered**. They remain necessary because native
  position packets, not placement metadata, supply the tested authoritative snap targets.
  Removing them or driving directly from metadata would change the tested correction path.
  Position fields queue a snap for that client's next POST-tick checkpoint. Delta masks
  retain the last received position, avoiding lost-axis baselines. The received float
  position is matched to placement metadata for its POSITION age, never local arrival
  age, packet count or an assumed interpretation of Bedrock's packet tick field.
  Packets without position fields retain the last assembled native position's age;
  later metadata alone cannot re-age an old target.
- Motion/hint hooks and counters were removed; they neither supply position corrections
  nor change the Java simulated velocity. The existing shared POST-tick hook and existing
  early world-leave integration remain; there is no new native hook.
- `[bullet-drive-startup]` reports startup readiness. A failed END-tick or position hook
  produces one `[bullet-drive-unavailable]` warning and disables custom handling/writes,
  leaving native every5 behavior. No world/component query occurs during menu startup.
  Component availability is checked only once an actor exists; a missing interpolator
  produces the same once-per-client warning and leaves that actor on native fallback.
  Client status includes startupReady/unavailableLogged/postTicks/scopedWorld; confirm
  postTicks advances in-world. startupReady reports hooks, not a menu-time component test.
- These hooks use supported symbols, reference packet parameters, no RVA or placeholder-
  by-value hooks. They do not access native client/world services at the menu. Unknown
  actors pass through untouched. Native overrides used here are on the primary callback
  base; no secondary-base virtual override or unadjusted secondary `this` is introduced.
- Exit/disconnect invokes clear from the existing early leave path, before world token
  closure; dimension changes clear at the next post-tick before any actor writes. Inbox
  messages carry world generation and dimension. Unknown removals/corrections, duplicate
  and late spawns are ignored. Reliable ordered spawn serials use a high-water mark,
  rather than an indefinitely growing tombstone set. Custom spawn consumption can lag a
  native packet until the next post-tick; such still-unknown native packets pass through.
  Placement metadata is installed immediately for known scoped serials without native
  service access; metadata awaiting a spawn is consumed at POST tick. Unknown/late tags
  never create state. Per-active-bullet placement history and queued corrections are
  released with the bullet/scope, with no arbitrary projectile count cap.
  Module-lifetime `ClientDriver.available` and `marker` survive world exit. `clearWorld()`
  clears only bullet records, runtime mappings and spawn serial high-water state; outer
  cleanup also clears inboxes, scope and diagnostics. Only module disable clears capability.
  This fixes the live bug where exit reset drive to off while the server stayed every5,
  causing muzzle waits/jumps after rejoin. The synthetic regression clears a world, reuses
  the serial and verifies resumed stepping/interpolator override without activation.

### Supported interpolator override (live fix)

`SDK/` below is the installed LeviLamina 26.51.9 `include/` directory.

- `SDK/mc/world/actor/Actor.h:120–121` exposes `getEntityContext()`.
- `SDK/mc/deps/ecs/gamerefs_entity/EntityContext.h:30–38` provides the supported inline
  `tryGetComponent<T>()`, using the entity's registry and entity ID.
- `SDK/mc/entity/components/MovementInterpolatorComponent.h:18–25` describes the typed
  targets, step counts and completion/vehicle flags.

After the normal one-tick step and any queued position correction, the client calls
`actor->getEntityContext().tryGetComponent<MovementInterpolatorComponent>()`. If present,
the shared `overrideInterpolator()` helper writes `mPos = simulated position`,
`mRot = simulated rotation`, `mHeadYaw = simulated yaw`, and sets `mPositionSteps`,
`mRotationSteps`, `mHeadYawSteps` to zero. Completion and vehicle-ownership flags are
unchanged. The existing current/previous-position writes remain. No new hook, RVA,
component insertion/removal, or fabricated component is introduced. If absent, that actor
does not receive position/interpolator writes; native every5 remains the fallback. In
addition to the once-per-client warning, armed diagnostics report it once per bullet.

### Position-age alignment, not a blanket +1

Server placement still uses the post-step position's age (5,10,15,…). The original client
instead filtered using its PRE-step local age. In the live sparse case it was at age 4
when the server's age-5 position arrived; it either rejected that valid correction or
snapped ahead and then stepped again using the age-4 velocity. Per-tick examples also
arrived at matching local ages, so blindly adding one would break those cases.

The server now tags the native float target with its authoritative position age before
placing. Final hit endpoints are tagged by the ending segment's tick, not claimed to be
a complete free-flight step. `correctionAge(positionAge)` defines the fixed server periodic
schedule. The passive client consumes received axes, looks up the age of that assembled
native position, and queues corrections in receive order:

- Matching/older position ages snap BEFORE the upcoming physics step.
- A position ahead of the current local age snaps AFTER that one step, in the same
  client tick, before the native actor/interpolator override.
- No extra physics step, velocity replacement, clock fast-forward, blend, or extrapolated
  catch-up is introduced. Batched/late corrections preserve receive order; genuine
  multi-tick latency can still produce real snaps, just as authoritative corrections can.
- Missing position metadata is never guessed from local age: `unaged` increments;
  Java's before-step authoritative snap is retained with explicit age `-1`, rather than
  inventing an age or ignoring the server position. **Require unaged=0** for regular
  age-aligned production samples.

The extra metadata costs 45 payload bytes per placement per original viewer, plus
transport overhead. It does not replace native packets or reduce their bandwidth.

### Water sampling and known limits

`src/native/BulletWater.h` is shared by both adapters. It queries Bedrock's supported
`Actor::isInLiquid(Water, region, aabb, …)` at the **simulated tick-start position**,
using the Java 1/16-block entity box deflated by .001 and checking loaded chunks first.
It does not query the lagging server actor's cached `isInWater()` in `every5`.
`src/gun/BulletPhysics.h` is the one move/drag/gravity implementation on both sides.

Java refreshes `wasTouchingWater` during `Entity.baseTick()` before bullet motion;
`isInWater()` returns that cached flag (`Entity.java:419–440,1143–1145,1189–1206` in
the vanilla snapshot above). `updateFluidHeightAndDoFluidPushing:3017–3080` tests water-tag
fluid surfaces against the deflated box. Bedrock material/liquid intersection is the
native adapter, NOT a proven identical Java FluidState-height implementation; test
source, flowing, shallow-edge and waterlogged cases. Generic Java fluid-current pushing
and bubble particles were not present in our old ballistic system and are not added by
this friction/gravity implementation. This is not a claim of full Java environmental parity.

Native actor coordinates/corrections use floats while simulated positions/velocities
are doubles. The cast and Bedrock packet quantization differ from Java's tracker codec.
Dry-air drag's float subtraction is now Java-correct; allow that tiny rounding difference
when comparing 0.4.14 logs. Water deliberately changes authoritative trajectories/hits.

### Marker and range

The RP bullet geometry has a 1×1×1 model-unit yellow cube (1/16 block); the BP initializes
variant **1 (hidden)**, avoiding an initial flash and hiding it for unmodded clients too.
The client marker defaults **off**; `/taczbullet marker 1` shows it, `0` hides it.
The client sets variant 0/1 through supported typed `DataItem` assignment,
only when changed. The render controller hides the body for variant 1 and outside camera
distance 80. No per-frame mesh creation, production tracer/model or custom world pass.

The custom 80-block SPAWN cutoff does not reconfigure Bedrock's native actor tracking.
Native delivery also depends on its tracking/chunk visibility and the server actor's
possibly stale position. No SDK-backed claim of an exact native 80-block range is made;
`actorMissing=true` exposes an unavailable actor. There is no custom late-tracker enrollment
in this spawn-time protocol. Native actors can remain engine-tracked outside the custom
range; cube visibility is separately distance-limited. Boundary/re-entry behavior is
a known tracking limitation, not silently accepted Java tracking equivalence.

## Diagnostic interpretation and boundedness

- Arm 1–16 **pellets**, not triggers (a shotgun consumes one sample per pellet).
  Arming expires after 400 side-local ticks. Each armed bullet logs at most 400 tick lines,
  one spawn, one summary; the client additionally logs at most 64 correction lines.
  Re-arm/stop/exit flushes active summaries. Limits affect diagnostics only, never bullets.
- Server `[bullet-exp-server]`: serial, age, side tick, authoritative `sim`, native actor,
  `placed`, mode, moveMode, `water`, alive. Summary: ticks, placements, endpoint.
- Client `[bullet-exp-client]`: `sim`, `before` (read BEFORE our write), `lastWrite`
  (actual float-rounded value written last tick), `previous`, native `interpHalf`, actorMissing,
  wrote, overwritten, received/filtered since the last diagnostic tick, max snap in that
  interval, water. New fields: `interpolatorPresent`, `interpTargetBefore`,
  `interpStepsBefore` (position/rotation/head yaw), `interpTargetUsed`, `interpStepsUsed`,
  `positionAge`, `snapPhase` and `unaged`. Missing-component reporting is one extra line
  per armed bullet. Separate correction lines include authoritative positionAge,
  before-step/after-step phase, the actual snap size and target used in that tick.
- Summary: ticks, overwrites, unexplained overwrites (no accepted native packet in the
  interval), received count, absolute/delta counts, max snap and termination reason.
  Packet-associated overwrites are still counted: they are evidence, not hidden as success.
  `unexplained` increases ONLY when an overwrite is observed in an interval with no
  accepted native server packet. `filtered=0` is retained as a literal compatibility
  field, not a dormant filtering mode. Historical `[bullet-exp-*]` prefixes and
  `BulletExperiment.*` filenames remain for log/history continuity, not selectable modes.
- Compare by serial and trajectory phase, accounting for network delay/local tick ages.
  `interpHalf` should approximately equal `(previous + sim)/2` when native interpolation
  honors our states. F3+B and video/frame observation are still needed for actual smoothness.
  Expected low-latency production diagnostics: overwrites=0, unexplained=0, unaged=0,
  actorMissing=false, wrote=true, interpolatorPresent=true and interpStepsUsed=(0,0,0).
  Regular snaps should be near float error (supplied dry/water samples .000002/.000004),
  not a tick of travel. Terminal endpoints/batching/latency may produce genuine larger
  snaps; distinguish those from repeated age-offset errors, never hide them.

## Initial live evidence — 2026-10-31 10:35

One shot per combination, `/tacz bdebug 0 0.05`; this is not general acceptance.
Client evidence: `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina-hi2/logs/2026-10-31.4.log:865–1087`.
Server evidence: `F:/Games/Minecraft/BDS/LeviLamina-bedrock-server-1.26.51.1/logs/latest.log:17145–17222`.

| Serial / combination | Overwrites / unexplained | Received / filtered | Max snap |
| --- | --- | --- | --- |
| 1 tick + off | 0 / 0 | 5 / 0 | .000006 |
| 2 tick + drive | 5 / 0 | 5 / 0 | .000005 |
| 3 every5 + off | 0 / 0 | 1 / 0 | 1.310850 |
| 4 every5 + drive | 4 / 3 | 2 / 0 | 1.288880 |
| 5 tick + drive-filter | 3 / 3 | 4 / 4 | 0 |
| 6 every5 + drive-filter | 3 / 3 | 1 / 1 | 0 |

The dry free-flight calculations matched; water was NOT tested. Delta was the sole
observed movement packet. Native interpolation replayed the old target even without
accepted packets, including return toward the spawn point; filtering alone did not fix
ownership. The sparse correction was applied against age 4 although it represented age 5.

## Post-fix live evidence — 2026-10-31 11:31–11:33

Historical six-combination comparison at `/tacz bdebug 0 0.05`, after interpolator/age fix.
These mode switches no longer exist in the production build.
Client: `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina-hi2/logs/2026-10-31.5.log:867–1066`.
Paired BDS: `F:/Games/Minecraft/BDS/LeviLamina-bedrock-server-1.26.51.1/logs/latest.log:18055–18169`.

| Historical combination | Overwrites / unexplained | Max snap |
| --- | --- | --- |
| tick + off | 0 / 0 | .000006 |
| tick + drive | 0 / 0 | .000006 |
| every5 + off | 0 / 0 | .000001 |
| **every5 + drive (selected)** | **0 / 0** | **.000002** |
| tick + drive-filter | 1 / 1 | .000004 |
| every5 + drive-filter | 0 / 0 | .000002 |

Every5+off's tiny snap confirms the age offset was fixed, not merely hidden by driving.
Tick+drive-filter still allowed a filtered native packet to retarget the interpolator;
that path was rejected and removed. Small snaps there are not evidence of correctness.

Temporary-pool water evidence is in the same client log, lines 1225–1279:
every5+drive overwrites/unexplained=0/0, maxSnap=.000004, unaged=0. The user accepted
water agreement and F3+B smoothness visually. The removed tick+drive water sample had
snaps .001–.014 and unaged=6 (tick placements lacked age metadata); it is not a success
claim for all experimental paths or all Java water surfaces.

The live rejoin failure was separately identified: world exit reset drive to off while
server placement remained every5. The production capability/records split fixes this;
the final default build has not been deployed or launched by this coding agent.

## Final production live regression checklist — pending, user-run later

Do not launch while the user is editing the test world. This checklist is future work,
not an instruction to deploy/start during finalization.

1. [ ] With matching 0.4.15 DLLs and both packs installed later, verify menu safety, then
   enter a world without any mode commands. Status must show every5 / always-on and
   startupReady=1. Marker is hidden by default; enable F3+B independently.
2. [ ] Arm `/tacz bullet diag 4` and `/taczbullet diag 4` immediately before firing.
   Compare normal `/tacz bdebug 0 1` and slow `/tacz bdebug 0 0.05` with F3+B, then repeat
   moveTo/teleport using `bdebug 1`. Keep trajectories inside 80 blocks; restore `bdebug 0 1`.
   Check smoothness, interpolation midpoint, no stale-target pullback, and the diagnostic
   expectations above. Server placements must be spawn + ages 5/10/… + final endpoint;
   authoritative simulation/collision must still run every tick.
3. [ ] Leave/rejoin and shoot again **without activation**: no muzzle wait on a healthy
   modded client. Repeat dimension change and exit with bullets in flight; no stale actor
   writes/crash. Marker choice persists, diagnostics must be re-armed after scope changes.
4. [ ] Repeat the pool test; compare water flags and move-then-.4-drag / ×.6-gravity.
   Extend to flowing/source/shallow/waterlogged boundaries. Dry standing/strafe/turn hits,
   near-wall removal, expiry, shotguns and automatic fire must remain unchanged by this
   finalization; water is unchanged from the tested 0.4.15 physics, not from 0.4.14.
5. [ ] Toggle `/taczbullet marker 1` / `0` during flight; no simulation interruption.
   Confirm diagnostic output limits do not cap bullets. Test a second viewer, 79/81-block
   spawn distance and loaded-chunk/range re-entry; record actorMissing/delivery limitations.
   Unmodded/failed-drive viewers remain every5 with the accepted ~250 ms wait/jumps;
   unavailable-drive clients must emit the warning, never request per-client tick fallback.

## Synthetic validation

`tests/BulletExperimentTests.cpp` covers the codec/malformed input, exact shared air/water
steps, fixed every5 placement, passive partial-axis corrections, interpolation, Java snap
semantics, expiry/removal, unknown/late serials, lifecycle clear, no projectile cap, and
independent diagnostic limits.
The live fix adds placement-metadata codec validation, client phase-independent schedule
alignment, native-component override field tests, snap/step/override within one tick,
and accepted-packet-only attribution of unexplained overwrites. Finalization adds the
rejoin regression (capability and marker preserved, records cleared, serial reuse and
driving/override resumed), replacing obsolete selectable-mode/filter assertions.
Native packet routing, liquid surfaces, marker rendering and actual interpolation require
the live procedure; compilation/tests do not establish those outcomes.

Initial experiment validation: server/client Release and **376 tests / 938,862 assertions**
passed before the supplied live run. Updated build/test results are recorded after the
live-fix validation; this coding agent performs no deployment/launch or live acceptance.

Historical live-fix validation: server/client x64 Release and **381 tests / 939,180
assertions** passed before the supplied post-fix live run.

Finalization synthetic suite: **381 tests / 938,911 assertions passed**. The assertion
count changed because selectable-mode/filter tests were removed/replaced. Finalization
server/client x64 Release builds and test compilation passed. Client Release configuration
remains active; version remains 0.4.15. Only existing hook/UI compiler warnings were
observed. No push, deployment or game/server launch was performed.

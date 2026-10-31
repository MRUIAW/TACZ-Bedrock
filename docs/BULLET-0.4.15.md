# 0.4.15 — client-driven native bullet experiment

2026-10-31. **Not live accepted.** The main agent/user supplied the initial six-shot
live matrix below. The interpolator/age live fix still needs a new live run. This
coding agent does not launch Minecraft, deploy, push, or select a final design.

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
  those visuals, flight particles and bullet holes are NOT implemented in this experiment.
- Vanilla 1.20.1 [ServerEntity.java:110–175](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/blob/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft/server/level/ServerEntity.java#L110-L175)
  can send position updates independently of the velocity-update switch. TACZ does not
  override `lerpTo`; vanilla [ClientPacketListener.java:562–605](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/blob/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft/client/multiplayer/ClientPacketListener.java#L562-L605)
  calls `lerpTo(...,3,...)`, but inherited [Entity.java:1965–1968](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/blob/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft/world/entity/Entity.java#L1965-L1968)
  **snaps** position and rotation, not a three-tick blend. Rendering separately interpolates
  tick positions ([LevelRenderer.java:1430–1436](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/blob/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft/client/renderer/LevelRenderer.java#L1430-L1436)).

## Exact runtime controls

Enter these slash commands in a world. Client commands use LL's client registrar
(`SyncLocal`), never a command sent to the remote server. Each viewer has its own settings.

| Command | Meaning / default |
| --- | --- |
| `/tacz bullet sync tick` | Server placement every simulation tick; **default** |
| `/tacz bullet sync every5` | Server placement at spawn, age 5/10/15/…, and removal endpoint |
| `/taczbullet mode off` | No client position writes/filtering; **default**, reset on world exit |
| `/taczbullet mode drive` | Client steps/writes native actor; native packets still pass through |
| `/taczbullet mode drive-filter` | As drive; native movement accepted only for authoritative POSITION ages 5/10/15/… |
| `/taczbullet marker 0` / `1` | Local yellow cube off/on; **default 1** |
| `/tacz bullet status` / `/taczbullet status` | Confirm server/client settings and active counts |
| `/tacz bullet diag 4` | Server: arm next four pellets from issuing admin |
| `/taczbullet diag 4` | Client: arm next four received pellets (including other shooters) |
| Either diagnostic command with `0` | Stop current diagnostics and disarm future sampling |
| `/tacz bdebug 0 1` | Existing server `moveTo`, full speed; **default** |
| `/tacz bdebug 1 1` | Existing server teleport, full speed |
| `/tacz bdebug 0 0.05` | moveTo, slow newly spawned bullets to 5% for inspection |
| `/tacz bdebug 1 0.05` | teleport, same slow launch speed |

Server commands require GameDirectors permission. Mode changes affect in-flight bullets
on subsequent ticks; speed changes only affect new launches. Change between bursts for
comparable evidence. No rebuild is required for any candidate or marker setting.

## Implementation and live verification gate

- `src-server/server/BulletSystem.cpp` always ray-traces and steps authoritative state
  each server tick. Only native actor placement is throttled. Other server mods see a
  stale actor position between placements in `every5` (up to four ticks, roughly 100
  blocks at 25 blocks/tick), NOT the authoritative simulated position. This is the
  expressly requested experimental difference; it is not Java server-entity parity.
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
  All modes maintain shadow simulation so modes can change without a rebuild; `off`
  never writes position or filters native packets.
- `src-client/client/BulletExperiment.cpp`: drive calls supported `Actor::_setPos()`
  for current position/AABB, then writes `StateVectorComponent::mPosPrev` from the prior
  simulated position. In drive modes it ALSO overrides the existing native interpolator;
  see the supported access path below. Native velocity is deliberately NOT set to the simulated velocity
  (which could cause double movement). `getInterpolatedPosition(.5)` is sampled for evidence.
  Existing first-person `gun-render` is unchanged; the regular native entity renderer
  renders the cube. Whether native extraction/F3+B actually uses these states smoothly
  is exactly what the live run must settle.
- Four typed, signature-asserted `LegacyClientNetworkHandler::$handle` hooks observe
  `MoveActorAbsolutePacket`, `MoveActorDeltaPacket`, `SetActorMotionPacket`, and
  `MotionPredictionHintsPacket`. `drive-filter` applies the same age-based acceptance
  schedule to all four. Motion/hints never change our simulated velocity. Accepted native
  position fields queue a snap for that same client's next POST-tick checkpoint.
  Delta masks are decoded into the last RECEIVED server position even for filtered packets,
  avoiding lost-axis baselines. The received float position is matched to its server
  placement metadata, so the predicate uses the POSITION's age, never local arrival
  age, packet count or an assumed interpretation of Bedrock's packet tick field.
  Packets without position fields retain the last assembled native position's age;
  later metadata alone cannot re-age an old target.
- The initial six shots used **MoveActorDeltaPacket only**, with absolute/motion/hints
  counts zero. Summaries continue to count all four; teleport, water and other conditions
  have not been established by that sample.
  `[bullet-exp-hooks]` reports each installation result; `drive-filter` refuses activation
  if any receive hook failed; either drive mode requires both position hooks. Client
  status includes hooksReady/postTicks/scopedWorld;
  confirm postTicks advances in-world rather than testing a non-running tick integration.
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

### Supported interpolator override (live fix)

`SDK/` below is the installed LeviLamina 26.51.9 `include/` directory.

- `SDK/mc/world/actor/Actor.h:120–121` exposes `getEntityContext()`.
- `SDK/mc/deps/ecs/gamerefs_entity/EntityContext.h:30–38` provides the supported inline
  `tryGetComponent<T>()`, using the entity's registry and entity ID.
- `SDK/mc/entity/components/MovementInterpolatorComponent.h:18–25` describes the typed
  targets, step counts and completion/vehicle flags.

After the normal one-tick step and any queued position correction, drive modes call
`actor->getEntityContext().tryGetComponent<MovementInterpolatorComponent>()`. If present,
the shared `overrideInterpolator()` helper writes `mPos = simulated position`,
`mRot = simulated rotation`, `mHeadYaw = simulated yaw`, and sets `mPositionSteps`,
`mRotationSteps`, `mHeadYawSteps` to zero. Completion and vehicle-ownership flags are
unchanged. The existing current/previous-position writes remain. No new hook, RVA,
component insertion/removal, or change to off-mode interpolation is introduced. If absent,
one diagnostic line per armed bullet reports that fact, and position driving continues.

### Position-age alignment, not a blanket +1

Server placement still uses the post-step position's age (5,10,15,…). The original client
instead filtered using its PRE-step local age. In the live sparse case it was at age 4
when the server's age-5 position arrived; it either rejected that valid correction or
snapped ahead and then stepped again using the age-4 velocity. Per-tick examples also
arrived at matching local ages, so blindly adding one would break those cases.

The server now tags the native float target with its authoritative position age before
placing. Final hit endpoints are tagged by the ending segment's tick, not claimed to be
a complete free-flight step. Both sides use `correctionAge(positionAge)` for the periodic
schedule. The client consumes received axes even for filtered packets, looks up the age
of that assembled native position, and queues accepted corrections in receive order:

- Matching/older position ages snap BEFORE the upcoming physics step.
- A position ahead of the current local age snaps AFTER that one step, in the same
  client tick, before the native actor/interpolator override.
- No extra physics step, velocity replacement, clock fast-forward, blend, or extrapolated
  catch-up is introduced. Batched/late corrections preserve receive order; genuine
  multi-tick latency can still produce real snaps, just as authoritative corrections can.
- Missing position metadata is never guessed from local age: `unaged` increments;
  drive-filter rejects it; other modes retain Java's before-step authoritative snap
  with explicit age `-1`, rather than inventing an age or ignoring the server position.
  **Require unaged=0** when judging the age-aligned fixed experiment.

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
this friction/gravity experiment. This is not a claim of full Java environmental parity.

Native actor coordinates/corrections use floats while simulated positions/velocities
are doubles. The cast and Bedrock packet quantization differ from Java's tracker codec.
Dry-air drag's float subtraction is now Java-correct; allow that tiny rounding difference
when comparing 0.4.14 logs. Water deliberately changes authoritative trajectories/hits.

### Marker and range

The RP bullet geometry has a 1×1×1 model-unit yellow cube (1/16 block); the BP initializes
variant 0. The client sets variant 0/1 through supported typed `DataItem` assignment,
only when changed. The render controller hides the body for variant 1 and outside camera
distance 80. No per-frame mesh creation, production tracer/model or custom world pass.

The custom 80-block SPAWN cutoff does not reconfigure Bedrock's native actor tracking.
Native delivery also depends on its tracking/chunk visibility and the server actor's
possibly stale position. No SDK-backed claim of an exact native 80-block range is made;
`actorMissing=true` exposes an unavailable actor. There is no custom late-tracker enrollment
in this spawn-time experiment. Native actors can remain engine-tracked outside the custom
range; cube visibility is separately distance-limited. Boundary/re-entry behavior is an
open final-design issue, not silently accepted Java tracking equivalence.

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
  interval), received/filtered counts, packet-kind counts, max snap and termination reason.
  Packet-associated overwrites are still counted: they are evidence, not hidden as success.
  `unexplained` increases ONLY when an overwrite is observed in an interval with no
  accepted native server packet, not merely when that interval contains filtered packets.
- Compare by serial and trajectory phase, accounting for network delay/local tick ages.
  `interpHalf` should approximately equal `(previous + sim)/2` when native interpolation
  honors our states. F3+B and video/frame observation are still needed for actual smoothness.
  Small snaps and zero unexplained overwrites are useful criteria, NOT results claimed here.

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

Expected after the live fix, under the same low-latency dry conditions (NOT new results):

| Combination | Expected diagnostic behavior |
| --- | --- |
| tick + off | No override/writes; overwrite counters 0; regular snaps near float error |
| every5 + off | Still native sparse/jumpy rendering; regular age-5 snaps now near 0 |
| tick + drive | Targets used equal sim, all steps used 0; overwrites only with accepted packets |
| every5 + drive | Same override; no target pullback in packet-free ticks; regular snaps near 0 |
| tick + drive-filter | Keep position ages 5/10/… even if arriving at local age 4/9/…; no packet-free overwrites |
| every5 + drive-filter | Regular five-tick corrections retained, not all filtered; override prevents stale-target replay |

For all driven rows require `unexplained=0`, component present, and used step counts
`(0,0,0)`; in packet-free/filtered-only intervals `before` should match `lastWrite`
within the existing .0001 threshold. Require `unaged=0` for all rows. Regular every5
snaps should be close to float rounding (~.000005 in the initial tick sample), not one
tick of travel (~1.3 blocks in this slow run). Real terminal-hit endpoints, packet batching
or substantial latency are distinct from that off-by-one and must not be hidden.

## Exact live procedure — fixed build retest pending

1. [ ] Deploy matching client/server 0.4.15 DLLs and both packs; start from defaults
   `/tacz bullet sync tick`, `/taczbullet mode off`, `/taczbullet marker 1`, `/tacz bdebug 0 1`.
   Verify menu safety and command feedback. Record BDS vs integrated world, FPS and ping.
2. [ ] Enable F3+B through the existing hitbox-display setup. For each matrix row below,
   arm `/tacz bullet diag 4` and `/taczbullet diag 4` immediately before firing. Save both
   logs and a short first-/third-person recording. Use open space; keep travel inside 80.

   | Server | Client | Purpose |
   | --- | --- | --- |
   | tick | off | Original packet-driven baseline |
   | every5 | off | Sparse native placements without client driving |
   | tick | drive | Expose per-tick native interference |
   | every5 | drive | Candidate 甲: five-tick native placements plus client simulation |
   | tick | drive-filter | Candidate 乙: reject intervening received movement |
   | every5 | drive-filter | Combined control; reveal cadence/latency misalignment |

3. [ ] Repeat **all six rows** with `/tacz bdebug 1 1` (teleport), then slow-motion
   `/tacz bdebug 0 0.05` and `/tacz bdebug 1 0.05`. Restore full speed afterward.
   Every5 logs must show spawn + ages divisible by five + final endpoint placements,
   while server simulation/hit checks continue every tick. Native packets may still
   arrive between placements: that is evidence for the gate, not a failed command.
4. [ ] For every driven row, check wrote=true, actorMissing=false, F3+B/cube smoothness,
   interpolation midpoint, before vs lastWrite, unexplained count, per-kind packets and
   filtered counts and the new interpolator/positionAge fields. Filtering must use the
   authoritative position age (all regular packets at ages divisible by five,
   none at other armed ages), not one packet in five. Compare max snaps across rows;
   repeated large snaps or unexplained overwrites mean the candidate is NOT accepted.
5. [ ] Toggle marker 0/1 while bullets are active; geometry hides/reappears without
   stopping simulation/removing the native entity. Check F3+B separately with marker off.
6. [ ] Repeat with a second viewer inside 80, independently arming their client diagnostic.
   Test 79/81-block spawn distance, loaded-chunk boundary, leaving/re-entering range,
   and note native actorMissing/delivery. Do not infer Java tracking parity from one viewer.
7. [ ] Shoot into/through source and flowing water, waterlogged/shallow edges; compare
   server/client water flags and move-then-0.4-drag / ×0.6-gravity trajectory. Server
   collision remains authoritative. No claim that water hits remain identical to 0.4.14.
8. [ ] Dry standing/strafe/turn hit regressions versus accepted 0.4.14; near-wall immediate
   removal, expiry, shotgun bursts and sustained automatic fire (no visual cap). Confirm
   diagnostic limits stop output but do not stop bullets; collect summaries.
9. [ ] Change dimension, disconnect/rejoin and return to menu with bullets in flight.
   No old actor writes/state, no crash; client mode returns off on world exit. Re-arm
   after scope changes. Select the final architecture only after reviewing this evidence.

## Synthetic validation

`tests/BulletExperimentTests.cpp` covers the codec/malformed input, exact shared air/water
steps, placement/filter schedules, interpolation, Java snap semantics, expiry/removal,
unknown/late serials, lifecycle clear, no projectile cap, and independent diagnostic limits.
The live fix adds placement-metadata codec validation, client phase-independent schedule
alignment, native-component override field tests, snap/step/override within one tick,
and accepted-packet-only attribution of unexplained overwrites.
Native packet routing, liquid surfaces, marker rendering and actual interpolation require
the live procedure; compilation/tests do not establish those outcomes.

Initial experiment validation: server/client Release and **376 tests / 938,862 assertions**
passed before the supplied live run. Updated build/test results are recorded after the
live-fix validation; this coding agent performs no deployment/launch or live acceptance.

Live-fix validation: server and client x64 Release builds passed; **381 tests / 939,180
assertions** passed. Client Release configuration remains active; version is still 0.4.15.
The updated build has not been deployed/launched or live accepted by this coding agent.

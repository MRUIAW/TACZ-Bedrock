# 0.4.15 — client-driven native bullet experiment

2026-10-31. **Not live accepted.** The main agent/user will deploy and test it;
this implementation does not launch Minecraft, deploy, push, or select a final design.

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
| `/taczbullet mode drive-filter` | As drive; native movement accepted only at client bullet ages 5/10/15/… |
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
- Client state uses IDs, never retained Actor pointers. A custom packet inbox is consumed
  at the existing signature-asserted POST-`ClientLevel::$_subTick` hook in
  `src-client/client/sound/GunSound.cpp`; no additional tick or render hook is introduced.
  All modes maintain shadow simulation so modes can change without a rebuild; `off`
  never writes position or filters native packets.
- `src-client/client/BulletExperiment.cpp`: drive calls supported `Actor::_setPos()`
  for current position/AABB, then writes `StateVectorComponent::mPosPrev` from the prior
  simulated position. Native velocity is deliberately NOT set to the simulated velocity
  (which could cause double movement). `getInterpolatedPosition(.5)` is sampled for evidence.
  Existing first-person `gun-render` is unchanged; the regular native entity renderer
  renders the cube. Whether native extraction/F3+B actually uses these states smoothly
  is exactly what the live run must settle.
- Four typed, signature-asserted `LegacyClientNetworkHandler::$handle` hooks observe
  `MoveActorAbsolutePacket`, `MoveActorDeltaPacket`, `SetActorMotionPacket`, and
  `MotionPredictionHintsPacket`. `drive-filter` applies the same age-based acceptance
  schedule to all four. Motion/hints never change our simulated velocity. Accepted position
  fields snap current simulation position; velocity and prior position are not reset.
  Delta masks are decoded into the last RECEIVED server position even for filtered packets,
  avoiding lost-axis baselines. Ages are LOCAL client bullet ages, not packet counts or
  an assumed interpretation of Bedrock's packet tick field.
- Which position packet Bedrock actually uses is **pending live evidence**. Summaries
  report absolute/delta/motion/hints counts separately. Both position paths link in the
  client build; this is not proof that either particular path is used at runtime.
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
  interval, water. Separate correction lines carry each logged accepted target/snap.
- Summary: ticks, overwrites, unexplained overwrites (no accepted native packet in the
  interval), received/filtered counts, packet-kind counts, max snap and termination reason.
  Packet-associated overwrites are still counted: they are evidence, not hidden as success.
- Compare by serial and trajectory phase, accounting for network delay/local tick ages.
  `interpHalf` should approximately equal `(previous + sim)/2` when native interpolation
  honors our states. F3+B and video/frame observation are still needed for actual smoothness.
  Small snaps and zero unexplained overwrites are useful criteria, NOT results claimed here.

## Exact live procedure — all unchecked

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
   filtered counts. Filtering must be age-based (all packets at ages divisible by five,
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
Native packet routing, liquid surfaces, marker rendering and actual interpolation require
the live procedure; compilation/tests do not establish those outcomes.

Validation: server and client x64 Release builds passed; **376 tests / 938,862 assertions**
passed. Client Release configuration was left active. No live check, deployment or launch
was performed, and no candidate has been accepted as the final architecture.

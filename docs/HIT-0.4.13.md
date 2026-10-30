# 0.4.13 — bounded hit-registration diagnostic

Diagnostic only, 2026-10-31. The user reports frequent misses while strafing with
the crosshair on a mob about four blocks away, unlike Java TACZ. No hit fix is
claimed. 0.4.12 was accepted; its live results are recorded in the changelog.

## Java mechanism and source findings

`J/` below means `_reference/TACZ/src/main/java/com/tacz/guns/`.

| Step | Verified Java source |
| --- | --- |
| Trigger/aim | `client/gameplay/LocalPlayerShoot.java:285` sends timestamp/charge only. `network/message/ClientMessagePlayerShoot.java:29–46` encodes these fields and supplies server `entity::getXRot/getYRot`; `item/ModernKineticGunScriptAPI.java:179–193` samples the suppliers per round and adds each bullet to the world. Thus server-rotation latency is shared by Java, not by itself a port divergence. |
| Origin | `entity/EntityKineticBullet.java:216–220`: midpoint of `(xOld,yOld,zOld)` and current position, plus current eye height. |
| Direction | `entity/EntityKineticBullet.java:360–379`: rotate `(spreadX,spreadY,8)`, normalize, scale to speed; inherit shooter delta movement, vertical only when airborne. |
| Segment | `entity/EntityKineticBullet.java:293–358`: start at current bullet position, end at position + delta movement; COLLIDER/fluid NONE block clip first, cut the segment at the block, then entity tests. |
| Candidates | `util/EntityUtil.java:21–103`: bullet box expanded by motion, inflated by 1; fixed-box clip; nearest/ordered hits, with configured or eye-height ±0.25 headshot checks. |
| Fixed box | `util/HitboxHelper.java:83–111`: displacement = current minus old position; player-vs-player only may replace box/velocity from ping history; non-crouching player expands upward 0.0625; expand towards velocity; server player moves by velocity ×3 (×1.5 more when riding); riding/ITargetEntity moves by velocity ×−2.5; always move by velocity ×−5. Order matters. |

### First tick is phase-dependent, not an immediate spawn tick

`ModernKineticGunScriptAPI.java:193` calls `addFreshEntity`, not `bullet.tick()`.
The first bullet segment still starts at its spawn position (`EntityKineticBullet.java:307–310`).
Verified against the cached **Minecraft 1.20.1** SRG jar using `javap -c -p`:

- `ServerLevel$EntityCallbacks.onTickingStart` (`m_141987_`) adds to `EntityTickList`.
- `EntityTickList.ensureActiveIsNotIterated` (`m_156907_`) copies the active map
  when it is being iterated; `add` (`m_156908_`) then modifies that new map;
  `forEach` (`m_156910_`) finishes the original snapshot.
- `ServerLevel.tick` calls that `forEach`; `tickNonPassenger` (`m_8647_`) performs
  old-position/rotation setup (`Entity.m_146867_`) and calls entity tick.

An addition before the entity pass can tick that cycle; an addition during that
pass waits for the next one. No universal same-tick assertion is justified.
Our unchanged server bullets first advance at the existing bullet listener,
before native `ServerLevel::$_subTick`. LL's `ServerLevelTickEvent.cpp:8–10`
publishes the event **before** `origin()`. Diagnostic `age=0` marks the first
segment; `dt` measures bullet-listener checkpoints since the accepted trigger,
including any script delay/burst scheduling. It is not packet latency in ms.

### Interpolation and the −5 shift

`HitboxHelper.java:105–109` explicitly labels −5 an **experimental constant**.
Minecraft 1.20.1 `ClientPacketListener.handleMoveEntity` / `handleTeleportEntity`
bytecode calls `Entity.lerpTo` (`m_6453_:(DDDFFIZ)V`) with `iconst_3`.
That proves three interpolation steps for those Java packet paths, not a
derivation of −5 from three steps or a universal visual delay.

Bedrock LL 26.51.9 exposes `Actor::lerpTo` with a steps argument and
`mc/entity/components/QueuedMovementInterpolationComponent.h:13–15` stores
position/rotation/steps. These declarations do **not** prove the live mob delay
or its relation to Java. No guessed Bedrock multiplier is introduced.

### What Bedrock getPosDelta returns

In LL 26.51.9, `mc/world/actor/Actor.h:178–182` returns current `mPos`, stored
`mPosDelta`, and rotation. `getPosDelta()` directly returns
`mBuiltInComponents->mStateVectorComponent->mPosDelta` for **both mobs and players**;
it does not subtract positions. `mc/deps/vanilla_components/StateVectorComponent.h:12–14`
has distinct `mPos`, `mPosPrev`, `mPosDelta` fields.

LL's source `src/mc/world/actor/Actor.cpp:109–122` distinguishes its separate
`getVelocity()`: non-mob/non-ridable returns `getPosDelta`; mob/ridable uses the
vehicle or self's `PostTickPositionDeltaComponent` when present, otherwise
`getPosDelta`. `getPosDeltaPerSecLength()` scales that velocity's length by 20.
This supports a per-tick motion convention, but does not establish stored-delta
write phase/sign/units for every native mob/player path. This mod's existing
`BulletSystem.cpp` uses `getPosDelta` both for inheritance and target fixed boxes.
The diagnostic compares it with observed fixed-phase displacement rather than
silently substituting `getVelocity` or asserting equality with Java `x-xOld`.

## Unchanged port path and suspects

`src-server/server/BulletSystem.cpp::fire` still uses `getHeadPos`,
`getViewVector`, existing spread/RNG and inherited `getPosDelta` (grounded y=0).
`tick` still clips blocks then calls `findEntityHits`; the existing Mob candidates
and filters, `gun::fixedHitbox`/`gun::clip`, order, damage and pierce are unchanged.
Diagnostic results never feed this path.

Open questions: current head vs midpoint origin; target stored motion vs measured
displacement; first-segment checkpoint delay; client eye/view vs server ray; native
client interpolation/pick update phase. Aim latency is not uniquely Bedrock.
The diagnostic is not a full Java player ping-rewind/ITargetEntity implementation.

## Design, limits and lifecycle

- Admin-only `/tacz` command registration is retained. `server` arms one issuing
  player's level-scoped server session; `client` sends a distinct S2C debug packet
  only to the issuer, independently of integrated-server globals. Console rejected.
- Off by default; `1..16` selects next **accepted triggers**, not rounds. All their
  delayed/burst pellets carry the original read-only tag, timestamp and trigger tick.
  A locally accepted trigger may still be rejected on the server or produce no
  pellets through Lua; correlate `triggerTs`, not independently numbered `shot`.
- Each side has a hard **64 diagnostic-line** budget per arming; rearming replaces
  it and invalidates old tags. The cap wins over completeness: later candidates,
  segments, pellets or selected triggers can be omitted. Start with N=1 for shotguns.
  Existing bullet/shoot/sync diagnostics remain separate and are not part of this cap.
- Expires after **400 tick events** (about 20 seconds at 20 TPS). `0` stops that
  side. Client leave/disable and server stop/disable clear state; mismatched world
  identity invalidates it. No new native hooks or world accesses at the menu.
- Server sampling occurs at the start of the existing bullet PRE-subTick listener,
  without changing listener priority/order. Two maps of at most **4096 mob/player
  positions each** are retained only while armed. History requires membership in
  both consecutive samples. Arm, wait at least two ticks, then shoot.
- `previous` is the previous PRE checkpoint, not a proven native Java `xOld`.
  `measured` is current candidate position minus that observation. Spawn may occur
  in a different phase; its `javaOriginEstimate = origin + (previous-current)/2`
  preserves current head offset, not a proven Java eye-height/native-history match.
  `nativePrevious` independently reports `mPosPrev` without assuming its phase.
  Missing/new/over-cap/nonconsecutive history is **unknown**, never fabricated zero.
- Pure geometry computes minimum Euclidean distance to the **solid** AABB over a
  finite segment by minimizing piecewise quadratics. `hit*` separately uses the
  actual Java-style `gun::clip` entry semantics; distance 0 does not imply an entry
  hit when starting inside. Hypothetical hits are geometry, not accepted damage.
- Client observes the latest **native** hit result at local trigger time, not a
  fresh long-range raycast. Reach, staleness, recoil and pick phase can differ from
  the rendered crosshair; `none` is not proof there was no target four blocks away.
  Client pick IDs are runtime IDs; compare with server candidate `runtime`.

Implementation is centralized in `src/debug/HitDiagnostic.h`,
`src-server/server/HitDiagnostic.{h,cpp}`, `src-client/client/HitDiagnostic.{h,cpp}`.
Existing shot/tick/candidate sites only pass observations or lifecycle notifications.
Synthetic tests in `tests/HitDiagnosticTests.cpp` cover budgets, invalidation,
unknown history, midpoint math, grazing/inside/parallel/point/oblique distances,
symmetry, translation and sampled-distance upper bounds.

## Commands and exact diagnostic message formats

```text
/tacz hitdiag server 3
/tacz hitdiag client 3
/tacz hitdiag server 0
/tacz hitdiag client 0
```

Each server message is the following prefix followed by exactly one phase body.
`{}` is a formatted field, not literal braces; vectors have six decimals,
booleans are `true`/`false`, boxes are `(minX,minY,minZ):(maxX,maxY,maxZ)`.
Ordinary logger timestamps/metadata are outside these messages.

```text
[hit-diag-server] line={}/64 shot={} triggerTs={} tick={} dt={} phase=spawn bullet={} pellet={} shooter={} pos={} previousKnown={} previous={} nativePrevious={} origin={} javaOriginEstimate={} rotation=({:.6f},{:.6f}) direction={} posDelta={} velocity={}
[hit-diag-server] line={}/64 shot={} triggerTs={} tick={} dt={} phase=segment bullet={} age={} from={} end={} uncutEnd={} blockHit={}
[hit-diag-server] line={}/64 shot={} triggerTs={} tick={} dt={} phase=candidate bullet={} age={} target={} runtime={} type={} pos={} previousKnown={} previous={} nativePrevious={} raw={} posDelta={} measured={} fixedDelta={} fixedMeasured={} distanceRaw={:.6f} distanceDelta={:.6f} distanceMeasured={} hitRaw={} hitDelta={} hitMeasured={} player={} crouching={} riding={}
[hit-diag-client] line={}/64 shot={} triggerTs={} world={} eye=({:.6f},{:.6f},{:.6f}) direction=({:.6f},{:.6f},{:.6f}) rotation=({:.6f},{:.6f}) pickType={} pickRuntime={} pickName={} pickPos=({:.6f},{:.6f},{:.6f}) pickHit=({:.6f},{:.6f},{:.6f}) pickRayStart=({:.6f},{:.6f},{:.6f}) pickRayDirection=({:.6f},{:.6f},{:.6f})
```

`pellet` is zero-based within a round; `bullet` disambiguates repeated burst
pellet indices. `direction` is the existing pre-spread view vector; `velocity`
is the actual spread/inheritance launch vector, including current bdebug speed.
`rotation` is native `(pitch,yaw)` (`Vec2.x,z`). `end` is block-cut, `uncutEnd`
is full proposed motion; candidate distances use the cut segment.
`fixedDelta` is the current production box; `fixedMeasured` changes only the
velocity input to the same pure helper. Unknown measured values print `unknown`.
Client pick type is the native enum integer; no entity yields `pickRuntime=0`,
`pickName=none`, zero pickPos, but the native pick ray/hit fields remain visible.

## Live procedure and interpretation (not run by this task)

1. Use matching 0.4.13 BDS/client; keep normal bullet speed/move settings. Choose
   an unobstructed mob at about four blocks; record gun, mode, ADS and target motion.
2. Arm both sides for **3 triggers**, wait two ticks, shoot three stationary control
   shots. Save both logs. Rearm for three shots strafing left, then separately right,
   keeping the crosshair on the same part of the target. Finish within each expiry.
   For shotguns/burst or busy scenes use N=1 and repeat to avoid the 64-line cutoff.
3. Stop both sides with 0. Repeat on a moving target if the initial target was still.
   Also check no diagnostic lines appear when off and leave/rejoin clears arming.
4. Match `triggerTs` across sides, then `bullet`, `age=0` and target runtime ID.
   Compare client eye/view/pick ray against server origin/view/actual velocity.
   Compare `origin` with `javaOriginEstimate` and left/right sign changes; this
   identifies a possible offset, not a proof that moving the origin would hit.
5. If `hitRaw=true` but `hitDelta=false`, the existing correction removed a raw-box
   entry. If `hitMeasured` differs from `hitDelta`, inspect stored vs measured motion
   and corrected boxes. Nonzero distance quantifies the miss; zero/false can be a
   starts-inside case. Unknown history or an absent candidate cannot be treated as
   a negative measured result. Check block cutting, line cutoff and native pick reach.
6. Compare first-segment `dt` and spawn `dt` across controls/strafe/bursts. Long delay
   can include Lua scheduling, not only networking. Report matched full lines before
   selecting any fix; no one suspect is established by this diagnostic alone.

## Stage 5 dependency

Java bullet holes use `particles/BulletHoleOption` and `client/particle/BulletHoleParticle`.
Tracers/ammo models/smooth bullet presentation need client bullet simulation:
`EntityKineticBullet.java:88` disables velocity updates, and its client entity
advances from spawn velocity and supplies interpolated frame positions. This is
future presentation work, not a reason to change server hit behaviour here.

## Offline validation

Server and client Release builds passed; **347 test cases / 936,656 assertions**
passed (four new synthetic cases). Client Release configuration remains active.
Existing native-hook macro warnings and the existing flying-item signature cast
warning remain; no hooks were added. No push, deployment or game launch.

```powershell
xmake f -a x64 -m release -p windows --target_type=server -y
xmake build tacz
xmake f -a x64 -m release -p windows --target_type=client -y
xmake build tacz
xmake build tacz-tests
xmake run tacz-tests
```

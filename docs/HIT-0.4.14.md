# 0.4.14 — received-input hit registration (live acceptance pending)

2026-10-31. This implements the user's chosen fix, not a claim of live acceptance.
The user deploys/tests; this task does not push, deploy or launch Minecraft.
`J/` means `_reference/TACZ/src/main/java/com/tacz/guns/`.

## Why this change

0.4.13's **diagnostics were live-tested**, not an accepted gameplay fix. In paired
`tacz-0.4.13-hit-user2-{server,client}.log` at 01:59, the user strafed/turned at a
stationary zombie about four blocks away: client pick 16/16, server hits 7/16.
Matched yaws (client/server): #379 61.35/65.54, #383 38.15/50.29, #393 0.71/3.21.
The server origin lagged by about 0.3 blocks; miss distances were 0.06–0.66.
The target did not move, so its displacement was not the cause of those misses.

Separately, grounded zombie stored motion `(0,-0.0784,0)` versus zero displacement
incorrectly stretched/raised the Java-style fixed box by about 0.3–0.4 blocks.
Standing player native position y=-58.38 (feet -60), but `getHeadPos` y=-58.48.
LL `src/mc/world/actor/Actor.cpp:81` uses `getAttachPos(Head)`: a head attachment,
not Java eye position. That observed 0.1 offset is a divergence for bullet origin;
the new origin no longer uses it. Target headshot tests are outside this change.

## Java mechanism and Bedrock queue

| Mechanism | Reference |
| --- | --- |
| Shoot packet | `J/network/message/ClientMessagePlayerShoot.java:29–46`: timestamp/charge, server `entity::getXRot/getYRot` suppliers. Vanilla movement on the ordered connection has already supplied position/rotation. |
| Each round | `J/item/ModernKineticGunScriptAPI.java:179–193`: evaluate suppliers and construct each bullet again, including burst/later script cycles. |
| Origin | `J/entity/EntityKineticBullet.java:216–220`: midpoint of old/current feet position + current eye height. |
| Direction/inheritance | `J/entity/EntityKineticBullet.java:360–379`: rotation-based spread/direction; `getDeltaMovement()` inherited, grounded vertical contribution zero. |
| Target correction | `J/util/HitboxHelper.java:83–111`: current minus old position, expand towards velocity then ordered shifts, including experimental −5. Spawn initializes Java old position to current, hence zero without history. |
| Movement inaccuracy | `J/resource/pojo/data/gun/InaccuracyType.java:75–80`, `J/util/HitboxHelper.java:55–62`: cached player first-minus-last positions, not physics delta. |

Bedrock exposes `ServerPlayerMovementComponent::addPlayerAuthInputPacket` and
`mQueuedUpdates` (`mc/entity/components/ServerPlayerMovementComponent.h:27,44–46`).
Movement applies later, while our custom shoot handler reads immediately. The
user approved using the latest **received**, not applied/queued input per round.
It avoids the extra stale applied-state dependency without putting aim/position
in the custom shoot packet, rewinding to its diagnostic tick, or inventing a
distance/sanity fallback. There is no pitch clamp (other mods permit >±90°).

## Implementation excerpts and lifecycle review

`src/gun/HitInput.h` is pure; `src-server/server/HitInput.cpp` integrates native
capture. No equivalent LL auth-arrival event was found. The signature-checked
`ServerNetworkHandler::$handle(NetworkIdentifier const&, PlayerAuthInputPacket
const&)` hook copies before `origin`, retaining the last **two in receipt order**:

```cpp
void receive(AuthInput value) { previous = latest; latest = value; }
// mPos, mRot, mYHeadRot, client tick, native level tick of receipt
```

Records use `getOrCreateUniqueID()`, bind level/actor/dimension identities, and
are erased on disconnect/death/respawn, server stop and disable. The native
`ServerPlayer::$changeDimension(DimensionType)` hook erases before `origin`;
`observeSelection` also invokes scope binding when it observes a dimension change.
Receive/snapshot independently bind scope, covering alternate transition paths
(including credits) on their first observation. Binding preserves already-fresh
new-dimension input; it does not discard it merely because shooter observation
arrived later. There is no placeholder-by-value hook or RVA-installed hook.
World access is gated by a live server level; enable-time thread recording only
uses `std::this_thread::get_id`, safe at the main menu.

The input mutex is **separate** from `Shooter.cpp`'s mutex. Receive never takes
the shooter mutex. Native player/packet reads precede the input lock; only copied
values/map operations occur under it; it is released before diagnostic work and
`origin`/lifecycle callbacks. Per-round snapshots return owned copies. Nested
order is shooter → input, never input → shooter. The diagnostic mutex is separate;
capture releases input before taking it. No gameplay branch depends on thread
IDs/equality. Thus differing hook/shooter threads do not race the input ring or
deadlock through the shooter lock. This does not assert arbitrary native actor
APIs are generally thread-safe; the hook uses the player during its native packet
dispatch. The supplied 03:48/03:49 `phase=threads` samples show all four IDs equal;
this is observed dispatch evidence, not a general scheduling guarantee.

At every `bullet::fire`, including retained delayed/burst callbacks:

```cpp
// Single named packet-coordinate assumption (one-line correction if refuted):
authInputFeet(p) = p - vec3(0, 1.62, 0);
origin = (authInputFeet(previous.pos) + authInputFeet(latest.pos)) / 2 + eyeHeight;
direction = (-sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch));
```

One input duplicates its position for the midpoint. No input uses current native
feet + Java eye height and current native rotation. `javaEyeHeight` uses existing
native pose flags: standing 1.62, sneaking 1.27, swimming/gliding/crawling 0.4.
`mRot.x,z` are interpreted as pitch/yaw; `mYHeadRot` is head yaw, recorded but not
substituted for aim. `mPos` is **assumed** player base anchor feet +1.62, not feet
and not pose-dependent Java eye height. Native standing logs and an independent
Bedrock server's auth-input conversion corroborate it, but old logs contain no
raw auth positions. Supplied 0.4.14 `authPos`/`feetPos` samples now support this
convention for the observed poses; other pose/transition cases remain live checks.

Corroboration only, not proof of native 1.26.51 semantics:
[Dragonfly's auth-input handler](https://github.com/df-mc/dragonfly/blob/master/server/session/handler_player_auth_input.go)
subtracts 1.62; [Gophertunnel's packet definition](https://github.com/Sandertv/gophertunnel/blob/master/minecraft/protocol/packet/player_auth_input.go)
documents the pitch/yaw/position/tick payload.

### Target identity, bounds, despawn and phase

Gameplay history keys are **`getOrCreateUniqueID().rawID`**, not the possibly
unassigned `mLegacyUniqueID` sentinel (old zombie logs showed −1). Runtime IDs
remain diagnostic correlation fields only. All eligible living mob/player
positions are sampled at each existing PRE-subTick bullet checkpoint, at most
4096. The previous map stays readable through all bullet tests, then whole-map
replacement drops actors absent/despawned from the current enumeration. Unique
IDs prevent replacement actors inheriting an old actor's motion. World end,
disable and observed level replacement clear it. Dimension mismatch, new actors,
missing/over-cap history give zero, never stored physics motion. The enumeration
cap may omit targets in crowded worlds; no pack/gun/namespace exceptions exist.

```cpp
velocity = previous.contains(id) && sameDimension ? current - previous[id] : zero;
box = gun::fixedHitbox({rawBox, velocity, isPlayer, crouching, riding});
```

LL publishes `ServerLevelTickEvent` **before** native `_subTick`, so this is the
last completed native movement interval (PRE N−1 → PRE N). Java bullets tick
among entities: a target already ticked can expose that cycle's displacement;
one not yet ticked is in a different old-position phase. Our deterministic sample
can therefore be one native tick behind the already-ticked Java case; it is not
proven identical to Java `xOld`. Keep the existing checkpoint, no guessed future
motion or extra tick. First bullet scheduling is unchanged; Java entity-addition
phase analysis remains in `HIT-0.4.13.md`.

The shooter-velocity live fix below supersedes the original unchanged-inheritance
policy: inheritance is now deliberately zero pending a Java-compatible adapter.
Movement inaccuracy remains native stored motion; spread/RNG are unchanged.
Java player ping-rewind/ITargetEntity and native interpolation differences remain
outside scope. Latest received input can still be 0–1 input tick older
than the client's local shot pose (a shot can occur before the next movement
send), as with Java's asynchronous shoot/movement packets; it is not zero-latency
aim or a guarantee under network loss/stalls.

## Shooter-velocity live fix: approved interim, A/B investigation stopped

This is a live fix of **unaccepted 0.4.14**, with no version bump. The user reports
the received-input change aligned server/client fire-time direction (0.00 degrees
yaw difference over 48 shots). In the airborne 03:49 round, about 15 blocks from
a stationary zombie, 7/16 hit; inherited Bedrock motion bent trajectories about
one degree. Those results do not establish full 0.4.14 acceptance.

### Step 1: applied exactly as decided

`BulletSystem.cpp::unusedFullBedrockShooterInheritance` retains the previous
`getPosDelta()` calculation and grounded-Y exclusion, compiled with
`[[maybe_unused]]` and **never called**. Its comment states that it inherits full
real Bedrock movement and does not match Java's server velocity, normally near
zero horizontally for ordinary walking, sprinting or creative flight.
The launch path instead uses the pure `gun::pendingShooterInheritance()` = zero.
This is an explicit interim divergence, **not** Java impulse/gravity emulation:
jump, fall, knockback, fluid and explosion contributions are also absent.
No action/packet/session hook, version, target-box or spread/RNG change was made.

### A: Java-like inheritance — not implemented

`J/entity/EntityKineticBullet.java:376–379` adds stored shooter velocity, excluding
grounded Y. The vanilla mapped sources are external, not present in this repo:
[1.20.1 source snapshot](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/tree/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft).

- `server/network/ServerGamePacketListenerImpl.java:861–990` applies received
  displacement through `move` and `absMoveTo`, not assignment of packet displacement
  to stored velocity; takeoff invokes `jumpFromGround` at `:931–933`.
- Listener `:253–264` calls `ServerPlayer.doTick` and then restores position.
  `ServerPlayer.java:484–488` calls `Player.tick`; physics still evolves stored
  velocity. `ServerPlayer.setPlayerInput:1055–1067` accepts locomotion input only
  while riding, not ordinary on-foot walk/flight inputs.
- `LivingEntity.java:1966–1974` sets jump Y and adds a 0.2 horizontal sprint-jump
  vector; `:2097–2119` applies friction/gravity/drag; `:1407–1414` sets knockback.
  `Entity.rideTick:1827–1832` clears motion before ticking, not permanently afterward.
  Fluids/explosions/other impulses and collisions can also modify velocity.
- Mob knockback is not interchangeable with every damage path: vanilla
  `Player.attack` sends a struck ServerPlayer's motion and restores its previous
  stored vector (`Player.java:1203–1206`). Forge can additionally modify impulses.

Both supplied LL 26.51.6 source headers and the build's 26.51.9 headers expose
`Actor.h:180` as `StateVectorComponent::mPosDelta`; `StateVectorComponent.h:12–14`
separates position/previous position/motion. Auth input also carries `mPosDelta`
(`PlayerAuthInputPacketPayload.h:101–104`). These declarations do **not** expose
native writes, or a proven Java-equivalent impulse-only server state.
`ServerPlayerCurrentMovementComponent.h:14–18` holds applied packet/old position,
not a separate Java velocity; `OtherImpulse.h` is an empty marker, not a velocity
accumulator. `LocalMoveVelocityComponent.h` is a Vec3 wrapper with no documented
Java semantics. No field-name inference is used as a gameplay implementation.

Recalculated `tacz-0.4.14-hit-server.log`: 03:49's 16 spawn lines have horizontal
`posDelta ≈ (authPos-authPreviousPos)*0.91`; 03:48's 32 lines instead give about
0.546. At line 307, displacement is `(-0.316848,0,0.295158)`, versus motion
`(-0.288329,0,0.268595)`. Stationary-ground Y can be -0.0784 with no height change.
This supports post-drag motion, but cannot prove whether native server writes are
simulation, packet copying or reconciliation. Dividing out drag still gives real
locomotion, not Java's normally-zero horizontal stored velocity.

**Open question A:** keep the approved zero interim, or authorize a separately
reviewed Java shadow simulation? A jump/gravity-only approximation would miss
collisions, fluids, effects, riding and impulse paths; using native Bedrock motion
would restore the rejected walking/flight deflection. Neither was implemented.

### B: movement inaccuracy — not implemented, sampling parity unresolved

`J/resource/pojo/data/gun/InaccuracyType.java:75–80` tests the length of
`HitboxHelper.getPlayerVelocity`, threshold >0.05. `J/util/HitboxHelper.java:25–62`
retains two sampled positions, returns first minus last (zero without two), and
does not read physics motion. The cache is sampled at **server player-tick END**
(`J/event/HitboxHelperEvent.java:13–20`). `ServerHitboxLatencyFix` defaults **true**
(`J/config/common/OtherConfig.java:34–35`). With it off from a fresh session the
cache is not populated, so movement speed is zero. If switched off after caching,
updates stop but the last difference can remain until logout/cache removal;
there is no alternate native-motion fallback in `getPlayerVelocity`.

Forge's [1.20.1 Player patch](https://github.com/MinecraftForge/MinecraftForge/blob/d13dd327bdc75c010f3358992e80e7e92f51e030/patches/minecraft/net/minecraft/world/entity/player/Player.java.patch)
emits post-tick at the end of `Player.tick`. This is inside `ServerPlayer.doTick`,
**before** the packet listener restores pre-physics position. Thus simply naming
a Bedrock phase END is not evidence of identical samples.

The inspected LL event set has no player-tick-END event. Its
`ll/api/event/world/ServerLevelTickEvent.cpp:8–10` publishes before native
`ServerLevel::_subTick`: an existing PRE-level cache is not Java's END-player cache.
`ServerPlayer.h:155,431` exposes `normalTick/$normalTick`; sampling after its
return is a candidate, but headers give no body/order proving when queued movement
has applied, or invocation cadence relative to movement catch-up/shoot dispatch.
`ServerPlayerMovementComponent.h:25–27,44–46` exposes tick credits and queued auth
updates, not their execution phase. No new hook is installed on that assumption.

**Open question B:** authorize (1) a per-player post-`normalTick` position cache,
pending native phase/cadence evidence, or (2) latest two received auth positions?
Option 1 is a server-tick sampling candidate, not proven Java-phase parity;
option 2 samples packet receipt, which can be multiple/zero inputs per server
tick, includes unapplied movement and differs from Java's cached END samples.
A global level-END cache is also a different checkpoint. Until a choice/evidence,
`Shooter.cpp::prepareRound` keeps native `getPosDelta().length()` unchanged; it can
still classify gravity-only stationary motion as MOVE when not aimed/sneaking.

### Actual launch/input diagnostics

Every opt-in `phase=spawn` now appends:

```text
inherited={} inaccuracyMovement={} inaccuracySpeed={:.6f} inaccuracy={:.6f} inaccuracySource=native-pos-delta
```

`inherited` is the vector **actually added** after spread, now `(0,0,0)`.
`inaccuracyMovement` is the exact vector used by `prepareRound`, copied with the
selected inaccuracy through the retained round closure to the spawn diagnostic;
`inaccuracySpeed` is its 3D length and `inaccuracy` the selected final spread value.
It is not resampled at spawn. The older `posDelta` field remains a spawn-time
native observation and may differ if the round executes later. Movement input
is observed, not added to the projectile. The existing opt-in budgets and protocol
are unchanged; no normal-play diagnostic logging is added.

## Exact new diagnostic fields and interpretation

Off by default, issuer/level scoped. Commands remain `/tacz hitdiag server N` and
`/tacz hitdiag client N`, N=1..16; 0 stops. Each side remains 64 lines/400 ticks;
thread evidence consumes **one of** the server's 64 lines, not an extra allowance.
Client chat acknowledges arming/stopping on the next diagnostic tick. Diagnostic
shoot tick is the last auth tick observed **after** the existing native send hook;
it is serialized only for correlation and never selects gameplay input. Matching
0.4.14 client/server are required (shoot wire format extended by unsigned tick).

The existing server prefix and segment format are unchanged. Exactly once at the
first logged spawn per arming (not per pellet/round), the following body is logged:

```text
phase=threads enableThread={} authThread={} levelTickThread={} shootHandlerThread={} authEqualsLevel={} shootEqualsLevel={} enableEqualsLevel={}
```

IDs are `std::thread::id` text (opaque; equality within the process matters).
`enableThread` records the thread at module enable, not an asserted main-thread
identity. `authThread`/`levelTickThread`/`shootHandlerThread` are observations of
the armed shooter's hook, level event, and `ShootHandler::handlePacket` since
arming. All equal and equality fields `true` confirm same-thread observations;
different IDs/`false` refute it. Enable may differ on integrated-server startup;
level tick is the gameplay reference. `unknown` means no observation yet, not a
mismatch: rearm, wait two ticks, shoot. This samples thread affinity, not a proof
about all future invocations or native scheduling.

Spawn additions relative to 0.4.13 (all names literal):

```text
packetClientTick={} authKnown={} authClientTick={} authReceiptTick={} authPos={} authRot={} authHeadYaw={} authPreviousPos={} feetPos={} javaEyeHeight={} authFeet={} authPreviousClientTick={} authPreviousReceiptTick={}
```

- `authPos`, `authPreviousPos`: unmodified latest two raw `mPos` vectors;
  `authRot`: latest raw `(pitch,yaw)`; `authHeadYaw`: latest raw head yaw.
- `pos` (existing field): native `player.getPosition()`; `feetPos`: native
  `getFeetPos()`; `authFeet`: `authInputFeet(authPos)`; `javaEyeHeight`: pose value;
  `origin` (existing): actual launch origin. `javaOriginEstimate` now equals
  `origin`, not the old head-offset-preserving hypothetical estimate.
- Client/receipt ticks accompany both records. Receipt ticks are native level
  ticks, not the diagnostic `tick` checkpoint counter. Missing values print
  `unknown`; `authKnown=false` identifies fallback. One input has unknown previous.
- **Standing motionless control:** raw positions should agree with `pos`, and
  `authPos.y - feetPos.y ≈ 1.62`; `authFeet ≈ feetPos`, `origin.y ≈ feetPos.y+1.62`.
  E.g. feet −60 → auth/pos/origin −58.38. Raw auth y≈−60 instead would refute the
  anchor assumption (current conversion would put origin at feet). A stable
  different offset likewise refutes it. Use six-decimal tolerance, not exact
  equality; moving samples/applied state need not coincide.
- Client `inputTick` must match server `packetClientTick` at the same `triggerTs`.
  On immediate shots `authClientTick >= packetClientTick` normally confirms the
  referenced auth input has arrived; equal is the simplest case. Lower indicates
  ordering/timing or send-observer issues to investigate, not gameplay fallback.
  For delayed/burst rounds the latest auth tick is expected to advance while the
  trigger's packet tick stays fixed. Tick zero means unknown on the client.

Candidate additions:

```text
gameplayVelocity={} fixedGameplay={} distanceGameplay={:.6f} hitGameplay={}
```

These are the real `fixedHitbox` inputs/box and segment test. Old `posDelta`,
`fixedDelta`, `distanceDelta`, `hitDelta` remain stored-motion comparisons only;
old `measured`/`fixedMeasured` fields still use opt-in diagnostic history and print
unknown when missing. Gameplay missing history is zero by design. Stationary
zombie: `gameplayVelocity=(0,0,0)` and unshifted `fixedGameplay=raw`, even if
`posDelta=(0,-0.0784,0)`. Client line appends `inputTick={}`; other fields retain
their 0.4.13 meanings, including native pick timing/reach limitations.

## User-run live checklist

1. Matching 0.4.14 server/client; confirm `[hit-input] receive/dimension hooks=true/true`.
   Arm each side for 3 triggers, wait two ticks; one standing, motionless shot at
   the stationary zombie provides thread and coordinate evidence above. Save full
   spawn/thread/client lines, not only a filtered screenshot.
2. Rearm and strafe left/right **while turning**, crosshair on the stationary zombie
   about four blocks away; expect hits whenever on target (allow configured
   spread and block obstruction). Compare matched `triggerTs`, ticks and gameplay
   boxes; do not infer damage from pick alone. Use N=1 for shotgun/burst to avoid
   the hard line cutoff. Repeat the stationary control.
3. Repeat with the target walking; inspect displacement direction/magnitude and
   measured gameplay box. Check burst/delayed rounds use newer auth records.
4. Death/respawn, dimension transfer (including alternate/credits path), leave/rejoin:
   no old-input aim/history leakage; fallback only until fresh input arrives.
   Stop with 0; no further diagnostic lines. Client chat acknowledges both commands.
5. Report equality flags, standing coordinate differences, packet/auth tick ordering
   and hit/miss comparison. Initial live logs support the sampled thread/coordinate
   facts above; full 0.4.14 acceptance and this velocity fix remain pending.

For this shooter-velocity live fix, repeat the airborne sideways flight/turning
round at about 15 blocks, plus standing/walking/sprinting controls. Check each
spawn has `inherited=(0.000000,0.000000,0.000000)` despite nonzero `posDelta` and
that velocity follows the spread direction without a movement-correlated bend.
Allow the configured RNG spread; zero inheritance does not mean zero inaccuracy.
Check `inaccuracySpeed=length(inaccuracyMovement)` and source `native-pos-delta`;
that input has **not** been fixed to Java's cached displacement. Repeat a burst or
delayed shot and confirm the logged input belongs to its prepared round, not an
unrelated spawn-time sample. Jump/fall/knockback likewise inherit zero for now.
Retest death/respawn, dimension transfer and leave/rejoin; no new hook or cache
was added. Stop diagnostics with 0 and send full paired logs/hit counts. A/B
decisions and overall 0.4.14 live acceptance remain open.

## Offline validation

Server and client Release builds passed; **354 test cases / 936,867 assertions**
passed. Client Release configuration remains active. Expected native-hook macro
warnings remain; no build errors. Edited files validated as UTF-8 without BOM;
whitespace check passed. No push, deployment or game launch.

Synthetic `tests/HitInputTests.cpp` covers receipt order, lifecycle/scope resets,
fallback/midpoint/pose eye heights, >±90° pitch, gravity-only stored motion versus
stationary measured box, unique-id replacement/missing history, bounds, and
per-round delayed-provider refresh. The live-fix tests additionally exercise the
production zero-inheritance policy with walk/sprint/flight/jump/fall/impulse
fixtures, non-axis-aligned aim, seeded spread/RNG preservation and unchanged
bullet gravity/drag. They do not establish native sampling parity for A/B.
Build commands:

```powershell
xmake f -a x64 -m release -p windows --target_type=server -y
xmake build tacz
xmake f -a x64 -m release -p windows --target_type=client -y
xmake build tacz
xmake build tacz-tests
xmake run tacz-tests
```

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
Server teleports now additionally clear the shared ring and gate capture until
`HandledTeleport` or a 100-server-tick timeout (see the teleport section below).

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
Movement inaccuracy now uses the user-chosen received-position difference below;
spread/RNG are unchanged.
Java player ping-rewind/ITargetEntity and native interpolation differences remain
outside scope. Latest received input can still be 0–1 input tick older
than the client's local shot pose (a shot can occur before the next movement
send), as with Java's asynchronous shoot/movement packets; it is not zero-latency
aim or a guarantee under network loss/stalls.

## Shooter-velocity live fixes: zero inheritance, B receipt-input decision

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
That Step 1 added no action/packet/session hook, version, target-box or spread/RNG
change; the later teleport fix below adds its explicitly approved native hook.

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

**A remains the approved zero interim from `751b261`.** A separately reviewed
Java shadow simulation was not authorized. A jump/gravity-only approximation would miss
collisions, fluids, effects, riding and impulse paths; using native Bedrock motion
would restore the rejected walking/flight deflection. Neither was implemented.

### B: movement inaccuracy — option 2 chosen and implemented

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

**User decision:** use option 2, the latest received raw auth position minus the
previous received position. The pure `gun::authInputInaccuracy` in
`src/gun/ShooterInaccuracy.h` uses zero with fewer than two samples, the existing
strict >0.05 threshold, and unchanged aim / sneak / move / stand precedence.
`Shooter.cpp::prepareRound` obtains one owned `hitinput::snapshot` at its existing
preparation point. No native-motion fallback, threshold adjustment, tick division,
client-tick filtering, pose/platform special case or new cache/hook is added.
The existing ring lifecycle clears cover missing records on fresh join, dimension
change, death/respawn and world end. Diagnostic count is 0, 1 or 2 for that snapshot.

**Accepted sampling difference:** Java's cache is updated at server player-tick
END; our ring is updated at packet receipt, before native movement application.
Zero/several inputs can arrive during one server tick, so this is not claimed as
identical Java sampling. Raw receipt order remains authoritative, even for repeated
or descending client ticks; no time normalization is applied. An input gap does
not manufacture a tick sample. Post-`normalTick` or global level-END sampling was
not selected. Java's config-off cache behavior above is reference context; no new
latency-fix config toggle is introduced for this user-chosen path.

**Evaluation relative to firing:** `J/item/ModernKineticGunScriptAPI.java:128–130`
selects `InaccuracyType`/spread inside `shootOnce`, **before** registering the burst
cycle (`:151`). The final value is captured and reused at `doBulletSpread:191–192`
for all bullets/pellets of that cycle; a later script `shootOnce` selects again.
Our existing `GunLogic.cpp::shootOnce:349–357` invokes `prepareRound` before cycle
registration. That relation is preserved: the owned movement, count, type and
spread are captured together, not resampled inside later burst callbacks.
Per-bullet aim still resolves the latest auth input independently, as before.

**Uncalled native alternative:** `Shooter.cpp::unusedNativePosDeltaInaccuracyMovement`
is compiled with `[[maybe_unused]]` and never called. Its comment states that
native motion is not Java's per-tick position difference, ground gravity residue
`(0,-0.0784,0)` exceeds 0.05 and falsely selects MOVE at rest, and raising the
threshold to 0.0785 would only mask flat-ground standing. A player carried by a
future moving platform, vehicle or mob could then be judged STAND despite moving:
native motion is **expected** not to include the carrier's motion, **not verified
live**. The alternative is retained as an option for possible future moving-platform
mods; neither it nor a 0.0785 threshold is enabled. Full-motion projectile
inheritance remains separately uncalled and the actually inherited vector is zero.

### Actual launch/input diagnostics

Every opt-in `phase=spawn` now appends:

```text
inherited={} inaccuracyMovement={} inaccuracySpeed={:.6f} inaccuracy={:.6f} inaccuracySource=auth-input-difference inaccuracyType={} inaccuracySamples={} nativeInaccuracySpeed={:.6f}
```

`inherited` is the vector **actually added** after spread, now `(0,0,0)`.
`inaccuracyMovement` is the exact vector used by `prepareRound`, copied with the
selected inaccuracy through the retained round closure to the spawn diagnostic;
`inaccuracySpeed` is its 3D length and `inaccuracy` the selected final spread value.
It is not resampled at spawn. `inaccuracyType` explicitly prints STAND/MOVE/SNEAK/AIM
(the label function also supports LIE); `inaccuracySamples` is the captured count
0/1/2. The older `posDelta` field remains a spawn-time native observation, with
its length now logged as `nativeInaccuracySpeed`; these can differ from the
preparation-time input if the round executes later. Movement input is observed,
not added to the projectile. The existing opt-in budgets and protocol
are unchanged; no normal-play diagnostic logging is added.

## Server teleport live fix — user-approved implementation, coverage pending live

06:21 user evidence after server `/tp`: #722 mixed places about 70 blocks apart;
#723 retained the pre-teleport position 30 blocks behind; #725 used the exact
between-places midpoint. Subsequent shots and a shot delayed 200 ms were correct.
The user authorized implementation now and will verify `/tp`, ender pearl and
chorus fruit coverage; no native-path live success is claimed by this build.

### Detection, confirmation and lifecycle

`src-server/server/HitInput.cpp::AuthInputTeleport` hooks **`Player::$teleportTo`**
with an asserted `void (Player::*)(Vec3 const&, bool, int, int, bool)` signature.
LL 26.51.9 headers: `mc/world/actor/player/Player.h:698–704,1379–1380`.
There was no equivalent LL teleport event in the inspected event set. The hook
owns a copy of the native target argument, calls `origin` **first**, then marks
the record. It requires a current server level, a non-client actor in that level,
and identity equality with the network handler's typed `ServerPlayer` lookup.
The user-identity component is checked before reading its network ID/sub-client,
so construction/teardown without that component cannot dereference an empty lookup.
Client-side Players are never marked, including in an integrated-server process.
This override is on the primary Player/Mob/Actor chain; it has no secondary-base
`this` adjustment. The existing auth-receive secondary override still uses
`thisFor<NetEventCallback>()`. There is no RVA or placeholder-by-value hook.

`PlayerAuthInputPacketPayload.h:64` defines **`HandledTeleport=37`**;
`:111` stores the flags in `mInputData`. The existing receive hook checks
`contains(HandledTeleport)`. The state lives in the pure `gun::AuthInputRing`:

1. Mark clears **both aim and movement-inaccuracy** samples, records the native
   server start tick/target, resets ignored count and sets `awaitingTeleportAck`.
2. While pending, unflagged receipts increment ignored count but do not enter
   our ring. Native `origin` still receives every packet; we do not suppress or
   change Minecraft's own movement/ack processing.
3. Before expiry, the first flagged packet ends the wait and is the **first**
   fresh sample (`previous` absent). Its first origin is that received location
   after the existing anchor/eye conversion, not a midpoint with pre-teleport data.
4. At **100 server ticks** (about five seconds at 20 TPS), recording is released
   without requiring an ack. This bounds a lost-flag stall while allowing several
   seconds for ordinary client/network delay. Expiry is checked by the existing
   LL level-tick event and also by receive/snapshot, so shots/packets at the boundary
   do not depend on listener order. Timeout is a single transition; it wins if a
   flagged packet arrives exactly at expiry, then that packet records normally.
   With no new packets the empty record continues ordinary server fallback.
5. Disconnect, death/respawn, dimension change, server stop/disable and observed
   level/actor/dimension rebinding clear pending state together with the ring.
   A fresh native teleport during a transition can create a new scoped mark.

The native level's `getCurrentTick().tickID`, **not client tick or wall time**, is
the deadline clock. No input timestamp sorting, teleport-distance heuristic,
separate inaccuracy history or aim sanity filter is introduced. Native reads and
`origin` occur outside the input mutex; copied state alone is changed under it.
All teleport diagnostic calls occur after releasing it. The hook never acquires
the shooter mutex. The level-tick expiry collects copied events before logging.

### Fallback and deliberate Java differences

Pending snapshots have no usable samples. `gun::roundAim` explicitly selects the
existing no-record path: **current native feet + pose eye height, current native
rotation**, not a frozen saved target and not a pre/post-teleport midpoint.
Inaccuracy preparation also sees an empty shared ring, then one sample after ack:
zero movement gives STAND unless AIM/SNEAK takes precedence. An already prepared
burst retains its existing captured inaccuracy, as documented above; later
`shootOnce` preparation uses the cleared/new ring. Projectile inheritance stays
zero and both uncalled motion alternatives remain untouched.

Java reference sources are the external
[1.20.1 snapshot](https://github.com/MCFireworkDev/MinecraftDeobfuscated-Mojang/tree/39cf080c749058613d9456c53ccdbe74111345ef/minecraft/src/net/minecraft):

- `ServerGamePacketListenerImpl.java:878–885` ignores movement while
  `awaitingPositionFromClient`; `:479–498` accepts the matching teleport ID and
  clears the wait. `:1011–1024` sets the position via `absMoveTo` and sends the ID.
- **The old-position distinction matters:** `Entity.absMoveTo:1328–1335` resets
  `xo/yo/zo`, not `xOld/yOld/zOld`. `ServerPlayer.teleportTo:1223–1248` routes
  same-level player teleports through that connection path. Non-player
  `Entity.teleportTo:2630–2661` instead uses `moveTo:1349–1368`, which resets both.
  `ServerLevel.tickNonPassenger:715–721` later calls `setOldPosAndRot`.
  `J/entity/EntityKineticBullet.java:216–218` uses **xOld**, so Java's immediate
  post-player-teleport midpoint can remain stale until that tick update. The user
  explicitly rejects reproducing that corner case: server-current fallback here
  is a deliberate improvement/difference, not an assertion of exact Java parity.
- Non-riding ender pearls call `teleportTo` (`ThrownEnderpearl.java:62–66`);
  chorus fruit calls `randomTeleport` (`ChorusFruitItem.java:41`), which routes
  through `teleportTo` (`LivingEntity.java:3046–3078`). These Java paths do not prove
  Bedrock hook coverage; that remains part of the user's live check.
- `J/util/HitboxHelper.java:25–62` does **not** clear on teleport; the only event
  updates/removal are server player-tick END/logout (`HitboxHelperEvent.java:13–25`).
  Before its next sample a shot sees the prior cached difference; after a new END
  sample, pre/post-teleport displacement can give MOVE, then return to zero when
  stationary samples converge. Clearing our shared ring gives STAND for a newly
  prepared first hip-fire shot instead. This is accepted; no separate history.

Known remaining differences, explicitly accepted for this scope:

- **Rapid consecutive teleports:** the first HandledTeleport flag ends the latest
  wait without a transaction-ID match. An earlier teleport's ack may therefore
  release a later mark. No guessed ID/position matching is added.
- **Rubber-band corrections and mount/dismount jumps** are not separately detected
  or fixed. Only paths invoking this native Player teleport override are covered.
- Plugin/pearl/chorus native-path coverage is awaiting live results. A path not
  producing a mark must be reported, not silently treated as proven covered.
- Timeout resumes ordinary receipt recording even without confirmed application;
  this is the requested bounded-loss policy, not Java's matched-ID handshake.

### Exact teleport diagnostic schema and user live check

At enable, require `[hit-input] teleport hook=true ackTimeoutTicks=100` in addition
to the existing receive/dimension hook line. Opt-in spawn lines append:

```text
teleportPending={} teleportWaitTicks={} originSource={} authRecordSamples={}
```

`originSource` is exactly `server-fallback` or `auth-input`; pending implies
`server-fallback`, `authKnown=false`, `authRecordSamples=0`. Wait ticks are elapsed
native server ticks for a pending snapshot, otherwise 0. The record count belongs
to the per-bullet aim snapshot; `inaccuracySamples` still belongs to its earlier
prepared cycle and need not match after a teleport during that cycle.

Mark/ack/timeout evidence, including **before the first shot**, is one line each:

```text
phase=teleport event=mark|ack|timeout serverTick={} target={} teleportWaitTicks={} ignoredPackets={}
```

`target` is the copied native `teleportTo(pos)` argument, without an assumed feet
conversion. `serverTick` is the native deadline clock; prefix `tick` is still the
separate diagnostic checkpoint counter. Prefix `shot=0 triggerTs=0` identifies an
event, not a shoot trigger. These lines share the **same 64-line/400-tick budget**,
consume no shoot triggers and are issuer/level scoped. Arming with 0, exhaustion
or expiry disables them; gameplay timeout/ack handling does not depend on arming.

1. As the target player, arm `/tacz hitdiag server 4` (and client diagnostics for
   correlation), then server `/tp` that player a large distance and fire immediately.
   Expect `event=mark`. If still pending, the shot must show `teleportPending=true`,
   `originSource=server-fallback`, count 0, and `origin = feetPos + (0,eyeHeight,0)`
   at the new server location. Native rotation supplies direction; configured
   spread still applies. No old-location or between-places origin should appear.
2. Expect `event=ack` with elapsed ticks and ignored-packet count. The next shot
   shows pending false and `auth-input`, initially count 1 (or 2 if another input
   has arrived). If ack already preceded the immediate shot, this is also valid:
   use event order and samples, not a requirement to catch the short pending window.
   Later shots must use only new-location samples; newly prepared first hip fire
   with 0/1 records has zero movement / STAND, unless aimed/sneaking.
3. Rearm separately for **ender pearl** and **chorus fruit**. Fire immediately
   after landing/consumption; check the same mark → fallback-if-pending → ack →
   fresh-sample sequence. Save full lines. Missing `event=mark` while armed (before
   budget expiry/exhaustion) identifies a native path needing further investigation.
4. Normally ack arrives well before 100 ticks. If it does not, expect one
   `event=timeout`, wait >=100, then pending false and ordinary recording on the
   next input. Test cleanup via dimension/life/world transitions; no old wait or
   samples should leak. Stop with 0 and report paired logs/hit counts. Do not use
   rapid consecutive teleports to assert matched-ack behavior: that remains known.

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
Check `inaccuracySpeed=length(inaccuracyMovement)` and source `auth-input-difference`.
With two identical standing samples, expect zero / STAND, versus native speed
about 0.0784 on flat ground. The user's AK47 hip-fire case should select STAND
inaccuracy 4.5 instead of MOVE 5.0 (data/mode dependent, not a gun special case).
A horizontal 0.2 step or Y-only 0.2 jump should give speed 0.2 / MOVE; aim and sneak
still take precedence. With 0/1 samples, expect zero / STAND unless aimed/sneaking.
Repeat a burst and confirm movement/count/type/spread remain those prepared by
that `shootOnce`, even when newer per-bullet aim records arrive; a later
`shootOnce` prepares afresh. Jump/fall/knockback still inherit zero for now.
Retest death/respawn, dimension transfer and leave/rejoin; the teleport hook/state
above now extend the existing ring's cleanup. Stop diagnostics with 0 and send
full paired logs/hit counts. Further A
inheritance work and overall 0.4.14 live acceptance remain open; B's phase
difference is explicitly user-approved.

## Offline validation

Server and client Release builds passed; **368 test cases / 937,151 assertions**
passed. Client Release configuration remains active. Expected native-hook macro
warnings remain; no build errors. Edited files validated as UTF-8 without BOM;
whitespace check passed. No push, deployment or game launch.

Synthetic `tests/HitInputTests.cpp` covers receipt order, lifecycle/scope resets,
fallback/midpoint/pose eye heights, >±90° pitch, gravity-only stored motion versus
stationary measured box, unique-id replacement/missing history, bounds, and
per-round delayed-provider refresh. The live-fix tests additionally exercise the
production zero-inheritance policy with walk/sprint/flight/jump/fall/impulse
fixtures, non-axis-aligned aim, seeded spread/RNG preservation and unchanged
bullet gravity/drag. `tests/ShooterInaccuracyTests.cpp` additionally covers stationary
and horizontal/Y-only auth displacement, 0/1/2 samples and clears, threshold and
aim/sneak precedence, receipt order/gaps, captured cycle values and explicit type
labels. `tests/TeleportInputTests.cpp` covers shared-ring clear, ignored packets,
ack as first sample, fallback/new-location origins, 100-server-tick boundary,
timeout without a shot/input, scope/lifecycle cleanup, the accepted unmatched rapid
ack behavior and pre-shot diagnostic events sharing the hard line budget.
Synthetic tests do not establish native sampling parity or teleport path coverage.
Build commands:

```powershell
xmake f -a x64 -m release -p windows --target_type=server -y
xmake build tacz
xmake f -a x64 -m release -p windows --target_type=client -y
xmake build tacz
xmake build tacz-tests
xmake run tacz-tests
```

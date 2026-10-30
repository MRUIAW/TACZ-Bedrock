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
dispatch. **Actual dispatch thread: pending live evidence** in `phase=threads`.

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
raw auth positions. **Position convention: pending live evidence** in
`authPos`, `authPreviousPos`, `pos`, `feetPos`, `origin` (see below).

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

Inherited shooter `getPosDelta()` (grounded y=0), spread/RNG and movement
inaccuracy inputs are intentionally unchanged. Java inheritance uses physical
`getDeltaMovement`, but Bedrock stored physics delta need not equal it numerically.
Inaccuracy is a clearer remaining difference: Java cached displacement vs our
stored delta (which can include gravity). No permission to fix these in 0.4.14.
Java player ping-rewind/ITargetEntity and native interpolation differences also
remain outside scope. Latest received input can still be 0–1 input tick older
than the client's local shot pose (a shot can occur before the next movement
send), as with Java's asynchronous shoot/movement packets; it is not zero-latency
aim or a guarantee under network loss/stalls.

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
   and hit/miss comparison. Both native facts and full 0.4.14 acceptance remain
   pending until this run; no invented native-thread or coordinate verification.

## Offline validation

Server and client Release builds passed; **352 test cases / 936,722 assertions**
passed. Client Release configuration remains active. Expected native-hook macro
warnings remain; no build errors. Edited files validated as UTF-8 without BOM;
whitespace check passed. No push, deployment or game launch.

Synthetic `tests/HitInputTests.cpp` covers receipt order, lifecycle/scope resets,
fallback/midpoint/pose eye heights, >±90° pitch, gravity-only stored motion versus
stationary measured box, unique-id replacement/missing history, bounds, and
per-round delayed-provider refresh. Build commands:

```powershell
xmake f -a x64 -m release -p windows --target_type=server -y
xmake build tacz
xmake f -a x64 -m release -p windows --target_type=client -y
xmake build tacz
xmake build tacz-tests
xmake run tacz-tests
```

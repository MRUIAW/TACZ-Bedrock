# 0.4.7 — synced cooldowns and Java client state lock

## Java vs. this port

Java paths below are relative to `_reference/TACZ/src/main/java/com/tacz/guns/`.
The entity transport/store from `SYNC-0.4.6.md` is retained; revision remains 1.

| Rule | Java file/line | This release |
| --- | --- | --- |
| Lock acquisition, pending predicate, 250 ms and completion | `client/gameplay/LocalPlayerDataHolder.java:81–118` | Pure `src/gun/ClientStateLock.h`, wall-clock milliseconds like Java; timeout only stops waiting for an unseen condition, never overrides authority |
| Shoot acquires, but does not generally test the lock | `LocalPlayerShoot.java:42,136,162` | Every successful shot packet replaces the timestamp/predicate; local rate timing remains independent |
| Draw/reload acquire | `LocalPlayerDraw.java:104–121`; `LocalPlayerReload.java:70–89` | Actual selection/DrawPacket and reload request; local presentation is no longer permission |
| Shoot prechecks | `LocalPlayerShoot.java:189–204` | Reload active, draw != 0, melee != 0; bolt shoot precheck deferred to 0.4.8 as assigned |
| Fire-select/inspect read lock only | `LocalPlayerFireSelect.java:29`; `LocalPlayerInspect.java:35` | Check at execution, no acquisition; mode prediction unchanged |
| Client tick ordering | `mixin/client/LocalPlayerMixin.java:105–115` | `aim::tick(); tickStateLock();` in ClientLevelTickEvent; auto bolt not implemented |
| Respawn reset | `LocalPlayerDataHolder.java:124–137`; `LocalPlayerMixin.java:125–128` | Reset lock on observed dead→alive or player replacement, plus world reset; ordinary dimension/spectator changes do not reset it |
| Server getter publication | `mixin/common/LivingEntityMixin.java:218–241` | Seven values in the same end-of-shooter-tick snapshot, owner plus trackers |
| Shoot getter / reload uses same value | `entity/shooter/LivingEntityShoot.java:228–262`; `LivingEntityReload.java:48–50` | Translated timestamp basis, 5 ms window, normal interval / burst minimum, no gun 0, unknown -1, no accepted shot 0 |
| Draw getter | `LivingEntityDrawGun.java:59–77` | Existing DrawCooldown including future put-away timestamp, no gun 0, unknown -1 |
| Unset melee getter | `LivingEntityMelee.java:105–145` | No melee timestamp/producer yet: known gun 0, unknown -1, no gun 0; not the missing schema default |
| Sensitivity and sprint | `mixin/client/MouseHandlerMixin.java:57–66`; `LocalPlayerSprint.java:27–29` | Synced aiming progress / is_aiming; FOV/model/recoil/crosshair stay on predicted aim |

## Key table

All keys retain `SyncMode.ALL`, fixed IDs and Java defaults. Full join/respawn/
dimension snapshots contain exactly the seven produced keys, independent of dirty
baselines. Only is_bolting is unproduced. Remote data still has no visual consumers.

| ID | Key | Default | Producer |
| --- | --- | --- | --- |
| 0 | shoot_cool_down | int64 -1 | 0.4.7 |
| 1 | melee_cool_down | int64 -1 | 0.4.7 getter, timestamp unset; actual melee later in stage 4 |
| 2 | reload_state | NOT_RELOADING / -1 | 0.4.6 |
| 3 | aiming_progress | float +0 | 0.4.7 |
| 4 | draw_cool_down | int64 -1 | 0.4.7 |
| 5 | is_aiming | false | 0.4.7 |
| 6 | sprint_time | float +0 | 0.4.6 |
| 7 | is_bolting | false | From 0.4.8, not produced here |

## Lock and guards as built

`src/gun/ClientStateLock.h` stores clientStateLock, lockedCondition
(None/Shoot/Draw/Reload/Bolt/Melee) and lockTimestamp. Bolt/Melee predicates are
supported without acquiring them in this release. The tick's decisive part is:

```cpp
if (elapsed < 250 && pending && !met) return false;
bool timedOut = pending && !met && elapsed >= 250;
lockedCondition = LockCondition::None;
if (isReloading(state.reload().type) || state.shootCoolDown() > 0 || state.drawCoolDown() > 0
    || state.isBolting() || state.meleeCoolDown() > 0) return timedOut;
clientStateLock = false; // never acquire from tick
```

Timeout is diagnostic only when an unmet predicate reaches 250 ms. A predicate
seen at that boundary is not counted as timeout. No local decay of synced values.
`-1` is not idle: draw/melee use != 0 in shoot precheck, but > 0 for lock release.

In `src-client/client/gameplay/ClientShooter.cpp`:

```cpp
// Shoot: locally timed cadence first, not synced shoot cooldown.
if (gun::clientCooldownMs(data, mode, now, lastShot) > 0) return false;
if (pendingNonShootLock() || !gun::syncedShootPrecheck(localSyncedState())) return false;
// After ammo/sprint checks, before each shot packet:
acquireStateLock(gun::LockCondition::Shoot);

// Reload: keep 100 ms since shot and ammo checks; no presentation cooldown.
if (stateLocked()) return;
// Fire-select / inspect, respectively, read the same lock at execution:
if (wantsFireSelect && !stateLocked()) { /* existing mode prediction */ }
if (wantsInspect && !stateLocked()) { /* existing inspect */ }

// Existing real client tick listener, not the rendered frame hook:
aim::tick();
tickStateLock();
```

Draw acquires at the existing actual selection notification and retains shot/aim/
burst/prediction resets. Synced entity values are never deleted on selection.
Burst follow-up presentation rounds neither acquire nor recheck the lock: Java's
scheduled check is before the first round only (`LocalPlayerShoot.java:275–285`).

### Tick/thread finding and respawn signal

LL 26.51.9 `src-client/ll/api/event/world/ClientLevelTickEvent.cpp` publishes
synchronously before ClientLevel::$_subTick origin. Our frame callback runs after
MinecraftGame::update origin. That source and SDK declarations do **not prove**
that these native entry points always run on one thread. No live thread trace was
taken and no game was launched. Therefore the implementation does not rely on
thread equality: a dedicated mutex protects the pure lock, counters and a copy of
the last frame-applied local synced view. Tick reads that copy, not the mutable
frame-owned entity map. World tokens reject retired projections. No lock is held
across engine, network, sound or Lua calls. Sensitivity and sprint read atomic
projections, not the frame-owned store.

Respawn observation reuses SprintIntentScope's player identity and separately
remembers actual alive/dead status, avoiding spectator eligibility as a false
respawn. A dead→alive edge or player replacement resets the lock; initial join is
covered by world reset. This observation needs live respawn acceptance, as does
the already-queued server respawn full sync (not exercised in 0.4.6 acceptance).

## Server getters and aim switches

`src/gun/ShooterCooldowns.h` is pure/testable. Server publication and handleReload
share the shoot getter; receipt-time-only lastShotServerMs was removed:

```cpp
if (!hasGun) return 0;
if (!data) return -1;
return serverCooldownMs(*data, mode, serverNow - clockOffset.value_or(0), lastShot);
```

Burst minimum conversion now casts the stored double to Java float before
`* 1000.f` and truncation, rather than allowing a double multiplication to shorten
e.g. 0.57F to 569 ms. The same trigger interval helper remains the local cadence
helper; it is not replaced with authoritative cooldown gating.

`src-client/client/gameplay/ClientAim.cpp` changes only the chosen aim consumers:

```cpp
auto scale = model::aimingSensitivity(base, state.ironZoom, shooter::syncedAimingProgress(), screenDistance, sensitivityMultiplier);
bool blockSprint = gun::blocksSprint(stopSprint, shooter::syncedIsAiming(), reload);
```

Existing exact hook signature assertions and recoil/world/player guards remain.
Sprint's existing input/raw/wire/native-latch machinery is retained, including
while local put-away disallows ADS. FOV, gun pose, camera recoil and crosshair
still read `aim::snapshot()`; no wholesale prediction replacement.

## Removed client paths and Java justification

| Removed path | Why / Java evidence |
| --- | --- |
| Reload-entry unconditional unlock/ack | Java data application only sets values (`ServerMessageUpdateEntityData.java:50–61`); `LocalPlayerDataHolder.java:90–118` alone releases the lock on tick |
| Idle-after-active automatic cancel_reload, including server interruption | Java's same sync handler does not trigger animations. `GunAnimationStateContext.java:242–248` exposes synced state to pack scripts; our existing context already supplies it, so no replacement synthetic event |
| Idle/refused-request cancellation (`wasRequested`) | Server refuses without a reply (`ClientMessagePlayerReloadGun.java:21–30`); no client reply callback in Java |
| Frame's >250 ms reload timeout cancelling animation | Java lock timeout only clears the predicate, then tests authority (`LocalPlayerDataHolder.java:95–118`); it never emits cancel_reload |
| reloadReplyType and selection-reset cancellation history | Only needed by the removed idle/refusal callbacks. Java draw resets predicted fields/locks (`LocalPlayerDraw.java:104–121`), not a reload reply history |
| reloadCancelRequested and one-cancel-per-press synthetic success | `LocalPlayerMixin.java:34–36` invokes cancelReload on each shoot attempt; `LocalPlayerReload.java:33–50,112–116` sends cancel/INPUT_CANCEL_RELOAD, and `ShootKey.java:67–69` marks success only after actual shoot success |
| Local anim::actionsBlocked / client cooldown / isReloading as reload guard | Java reload checks the state lock, 100 ms since shot and ammo (`LocalPlayerReload.java:70–80`) |
| Local draw/reload lock guards in shoot; composite fire-select/inspect guard | Java shoot pending-predicate and precheck (`LocalPlayerShoot.java:136,189–204`); fire-select/inspect read clientStateLock only (`LocalPlayerFireSelect.java:29`; `LocalPlayerInspect.java:35`) |
| 0.4.6 SyncDiagnostics, first/full/absent/rejection/summary logs | Acceptance complete except respawn; remove as assigned. Decode validation, token checks, absent drops/pruning and lifecycle remain |

Reload-cancel packets and animation triggers are limited to one per real client tick via an atomic ClientLevelTickEvent counter and a session-reset gate (Java `ShootKey.java:40–43`); the accepted frame-driven shooting divergence remains, without a per-press latch or shooting gate.
Explicit input cancellation remains. The default state machine plays reload
once and has no universal synced-idle cancellation (`resources/tacz/default_state_machine.lua:77–87,215–269`).
For example Java's shell animation reacts to INPUT_CANCEL_RELOAD
(`assets/tacz/scripts/m870_state_machine.lua:83–88` in its default pack); scripts
can also query the synced state via the already-implemented context. No gun/pack
special case or new automatic animation transition is introduced.

## Visible behavior and retained differences

- Auto cadence remains locally timed; a Shoot lock does not stop the next
  rate-eligible shot. Semi hold consumes a press on success only. Burst
  presentation stays scheduled separately.
- Fire→reload/inspect/fire-select may wait for server cooldown observation/zero
  plus up to one client tick, especially under latency. Repeated shots replace the
  condition timestamp. An operation entirely missed between publications waits
  up to 250 ms, but positive authority can hold longer.
- Reload completion must reach the synced view before shooting; explicit cancel
  presentation can happen immediately while the server magazine reload continues.
  Refused reload no longer receives an immediate recovery/cancel event; a locally
  predicted reload animation may finish naturally while the lock times out.
- Gameplay draw follows the synced cooldown plus pending Draw lock, not the
  local HandSwitch::blocked/50 ms reveal guard. Local put-away/draw presentation
  and ADS put-away restriction remain, so permission and visual readiness may differ.
- Sensitivity and sprint suppression intentionally follow delayed synced aim;
  local FOV/model/recoil/crosshair remain responsive prediction. Sprint recovery
  zero remains authoritative; there is no local sprint-time decay.

Retained approved differences: translated steady-clock client timestamps plus
our existing offset estimator rather than Java's base-timestamp handshake;
server `remaining > 0` rejection for shoot/reload even if burst minimum elapsed;
publication after our shooter simulation but before native server subTick rather
than Java tick END; StartTracking full sync deferred to stage 8 without a fallback;
player-only scope; fixed key IDs/protocol revision instead of registration
handshake. No bolt refusal ack/token/producer or auto bolt in this release.

## Accepted 0.4.6 and live acceptance checklist

User (2026-10-29): reload animations/ammo, magazine reload not interrupted by
firing, full/no-reserve refusal leaving firing available, sprint→fire, rejoin and
dimension change passed. Logged self IDs were R=L=1, with no rejected/absent/stale
packets. Respawn full sync was not tested, not failed. Single-player Esc really
pauses the world; animation can finish without refilling ammo, matching Java
without Open to LAN. This accepted pause behavior was not investigated or changed.

Test 0.4.7:

1. Hold auto fire: cadence unchanged, including sustained fire; held semi fires once.
2. Burst and continuous burst: correct presentation rounds, no extra trigger packets.
3. Fire→R/H/G: wait for authoritative cooldown, then work; holding fire still works.
4. Reload→fire: magazine reload continues as Java; shell scripts accept explicit
   interruption, and shooting waits for authoritative completion.
5. Switch/draw→fire, including rapid repeated switches: no permanent lock;
   gameplay follows server cooldown, presentation/ADS put-away restriction remains.
6. Locally valid but server-refused reload: no reply or synthetic cancellation on
   timeout, animation may finish naturally, then operations recover. Full/no-reserve
   client ammo checks still reject locally.
7. ADS: sensitivity follows synced progress; FOV/model/recoil/crosshair remain local.
   ADS release and sprint/fire/reload overlaps must not flicker or permanently block.
8. Die/respawn (same gun and a changed gun), rejoin and dimension changes: test
   lock reset and the still-unverified respawn full-self synchronization.
9. Inspect/fire-select do not acquire. Return to menu safely; no retired-world state.

At world end, one temporary low-volume line, never a per-tick log:

```text
[lock] session acquired=<n> byShoot=<n> byDraw=<n> byReload=<n> timeouts=<n> maxHeldMs=<n>
```

acquired is the sum of those three acquisition counters. timeouts counts each
unmet condition cleared at >=250 ms (not lock release, and not each held tick).
maxHeldMs is the longest continuous locked span, including repeated acquisitions
and an unfinished span at exit. Remove counters/log after acceptance. Old `[sync]`
lines must be absent.

## Validation — 2026-10-29

- Server and client x64 Release: requested xmake configurations and `xmake build
  tacz` both report **build ok**. Client configuration remains active.
- `xmake build tacz-tests`: **build ok**; `xmake run tacz-tests`:
  **267 cases / 924,137 assertions, Status: SUCCESS** (18 additional cases,
  net 1,639 additional assertions). Synthetic fixtures cover all lock predicates,
  250 ms boundaries, stale/missing/positive values, no implicit acquisition,
  getter clock/burst/draw boundaries and seven-key idle publication. No live
  network/thread/respawn equivalence is claimed by these tests.
- Existing hook-macro, GunHud native-cast and test u8path deprecation warnings
  remain. No new native hooks/RVAs, game launch, deployment or push.
- All changed/new files verified UTF-8 without BOM; whitespace checks clean.

## Changed files

- Pure logic: `src/gun/ClientStateLock.h`, `src/gun/ShooterCooldowns.h`,
  `src/gun/ShootRules.h`, `src/gun/SyncedShooterData.h`.
- Adapters: `src-server/server/Shooter.cpp`, `src/net/GunPackets.h`,
  `src-client/client/gameplay/ClientShooter.cpp`,
  `src-client/client/gameplay/ClientShooter.h`, `src-client/client/gameplay/ClientAim.cpp`.
- Tests: `tests/ClientStateLockTests.cpp`, `tests/ShooterCooldownsTests.cpp`,
  `tests/SyncedShooterDataTests.cpp`, `tests/ItemTooltipTests.cpp`.
- Release/docs: `xmake.lua`, `tooth.json`, both `packs/*/tacz_*/manifest.json`,
  `CHANGELOG.md`, `docs/ROADMAP.md`, this document.

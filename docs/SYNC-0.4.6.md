# 0.4.6 — Java-style synced shooter data

## Java vs. this port

Java paths below are relative to `_reference/TACZ/src/main/java/com/tacz/guns/`.

| Rule | Java file/line | This release |
| --- | --- | --- |
| Typed keys, defaults and ALL | `entity/sync/ModSyncedEntityData.java:14–60` | Fixed shared IDs in declaration order; ALL means owner plus trackers, excluding duplicate owner delivery |
| Dirty only when equals changes | `entity/sync/core/DataHolder.java:16–24` | Per-player publication baseline; full-value comparison, including Java Float.equals signed zeros/canonical NaNs |
| Reload equality includes countdown | `api/entity/ReloadState.java:52–55` | Countdown-only changes publish; no discarded countdown |
| Produce full reload result and sprint time | `mixin/common/LivingEntityMixin.java:218–241` | After aim/sprint/burst/reload simulation, publish the last full reload result and authoritative sprint time; idle is NOT_RELOADING / -1 (`entity/shooter/LivingEntityReload.java:102–120`) |
| Join full self set | `event/SyncedEntityDataEvent.java:54–65` | PlayerJoinEvent queues produced keys for next shooter tick; respawn and observed dimension changes also queue full self sync |
| Clone/persistence | `event/SyncedEntityDataEvent.java:68–86` | Publication baseline is separate from simulation; respawn retains existing shooter values and resends after native respawn, runtime-id replacement resets the publication baseline |
| Dirty owner/tracker publication at tick END | `event/SyncedEntityDataEvent.java:89–126` | Owner and native entity recipients; after our shooter tick, before native subTick (approved phase difference) |
| Entity ID and partial entries, main-thread apply, absent entity drop | `network/message/ServerMessageUpdateEntityData.java:26–61` | 64-bit runtime ID; packet thread validates/enqueues, frame resolves local ID directly or remote entity through Level, then merges; absent entities are dropped and stored absent actors pruned |

## Schema and wire format

`src/gun/SyncedShooterData.h` is header-only and independent of LL/SDK. No
throwable key is registered. Declaration alone does not enable a producer/consumer.

| ID | Key | Value / Java default | Producer/consumer schedule |
| --- | --- | --- | --- |
| 0 | shoot_cool_down | int64 ms / -1 | 0.4.7 |
| 1 | melee_cool_down | int64 ms / -1 | Reserved; actual melee in stage 4 item 5, no producer/check now |
| 2 | reload_state | ordinal + int64 ms / NOT_RELOADING, -1 | 0.4.6 |
| 3 | aiming_progress | float / +0 | 0.4.7 |
| 4 | draw_cool_down | int64 ms / -1 | 0.4.7 |
| 5 | is_aiming | bool / false | 0.4.7 |
| 6 | sprint_time | float seconds / +0 | 0.4.6 |
| 7 | is_bolting | bool / false | From 0.4.8 |

All eight keys are ALL. No reserved value is produced or consumed in 0.4.6;
even full lifecycle snapshots contain only keys 2 and 6. Melee's producer version
is not assigned yet; reserving -1 must not introduce a firing check now.

The LL wrapper `src/net/GunPackets.h::SyncedShooterDataPacket` carries raw pure-codec
bytes: little-endian u32 revision (=1), u64 actor runtime ID, u8 entry count,
then u8 key and its typed value. Cooldowns/countdowns are signed 64-bit bit patterns;
reload ordinals/bools are u8; floats are IEEE float32 bits. The key determines the
type, not an untrusted type tag. Both variants compile the same revision constant.
This version detects a bad payload, not incompatible packet-name registration.

```cpp
auto decoded = gun::sync::decode(packet.payload);
// decode returns either a fully validated Message or an error with no partial data.
// Wrong revision, count > 8, unknown/duplicate key, bad ordinal, truncation,
// non-finite float, invalid bool, trailing bytes and oversize payload all reject.
```

The bounded LL reader passes malformed payloads to the handler as rejections,
without treating them as valid entity state. Revision mismatch logs once per
session; other rejection logs are limited to one per second, with every rejection
counted. There is no partial store apply, actor lookup, sound or gameplay in the
network handler. Ordered inbox entries retain the session token captured there.

## Server publication

`src-server/server/Shooter.cpp` stores the full reload value, a baseline and
runtime/dimension observations per player. Reload start/stop and tickSprint no
longer send directly. At the end of the existing shooter tick:

```cpp
gun::sync::publish(runtimeId, snapshot, s.syncBaseline,
    [&](auto const& dirty) { net::SyncedShooterDataPacket{dirty}.sendTo(player); },
    [&](auto const& dirty) {
        net::SyncedShooterDataPacket{dirty}.sendTo(static_cast<Actor const&>(player), player);
    }); // baseline commits after both sends; idle emits nothing
```

Full self sends use `full(runtimeId, snapshot)` without consuming dirty state, so
trackers still receive changes in the same tick. Join/respawn callbacks queue only;
they never look up or send a snapshot before the native operation. LL 26.51.9 has
no typed player dimension-change event; the existing selection observation queues
dimension changes. Disconnect/server stop/disable drop pending full sends and
baselines. No native hook was added or changed.

Temporary until 0.4.7: refused reload sends the current reload_state immediately
to self only, bypassing the baseline. Java `ClientMessagePlayerReloadGun.java:21–30`
sends nothing. This is deliberately retained with today's client recovery, not
silently removed before the generic state-lock port.

## Client store and consumers

`src-client/client/gameplay/ClientShooter.cpp` drains after aim in `frame()`, at
the old mailbox application boundary. State is scoped by (world token, runtime ID).
Partial packets merge; gun switches do not delete entity data. Local consumers
read the local-player typed view; the atomic reload type used by animation is a
published projection, not another source of authority. No remote consumer yet.

```cpp
bool self = reply.message.runtimeId == localRuntimeId;
if (!exists(reply.message.runtimeId)) continue; // no deferred replay
if (!syncedEntities.apply(reply.world, reply.message)) continue;
// Only a self reload entry executes today's reply/cancel handling.
```

Each self reload entry, even unchanged, plays the old reply role: clear
reloadLockedAt, retain the same refusal/interruption animation dispatch and cancel
flags. The >250 ms timeout and cancel animation remain unchanged; an unrelated key
or remote reload never acknowledges a local request. Sprint has no local decay.
Cancellation-dispatch history still resets on selection, so a previous gun's idle
entry does not cancel the new gun's animation; this history never gates gameplay.
Local fire cadence, ADS/FOV/sensitivity/sprint-suppression prediction and draw
presentation are untouched. Fresh-session packets arriving before its first frame
survive reset; world end clears queues/store, and old tokens cannot populate a new
session even when the same runtime ID is reused.

## Approved staged differences and next release

- Fixed key IDs and payload revision instead of Java's registration handshake.
- Existing ServerLevelTickEvent is before native ServerLevel::$_subTick origin,
  not Java server tick END. We publish after our own complete shooter simulation.
- Java StartTracking full initialization (`SyncedEntityDataEvent.java:39–51`) is
  deferred to stage 8. No new hook, polling or periodic full fallback. Trackers may
  retain defaults until the next change; no third-person consumers currently exist.
- Only player shooters, not all Java LivingEntity instances. Native
  Dimension::sendPacketForEntity routing is used; exact Forge tracking equivalence
  is not established by headers or synthetic recipient tests.
- Temporary refused-reload reply until 0.4.7, as described above.

0.4.7 will produce shoot/draw cooldowns and aim values, port
clientStateLock/lockedCondition/tickStateLock, switch sensitivity and client sprint
suppression to synced aim values, and remove refused-reload replies together with
the old recovery policy. This release implements none of those changes. Bolt
production starts from 0.4.8. No melee rule is introduced by reserving its key.

## Runtime ID and clients without the mod

ActorRuntimeID is uint64 (`mc/legacy/ActorRuntimeID.h`); server publication reads
Player::getRuntimeID and client self resolution compares LocalPlayer::getRuntimeID,
not ActorUniqueID or a truncated int. Static SDK declarations do **not** establish
that integrated-server self IDs are equal. Acceptance must compare the logged
server runtimeId with localRuntimeId/self=true below. The first-absent diagnostic
also exposes mismatched IDs even if nothing can be applied.

LL v26.51.9 source inspection: `ll/api/network/packet/Packet.cpp` wraps packets in
RuntimePacket; `src/mc/network/Packet.cpp` routes owner by net ID/subclient and
actor recipients through Dimension::sendPacketForEntity. No TACZ capability check.
`RuntimePacket.cpp::_read` consumes/drops an unknown custom runtime hash when LL
is installed without TACZ; PacketRegistrar's dispatcher ignores missing handlers.
The envelope uses MinecraftPacketIds::LeviLaminaRuntimePacket (=201), created by
an LL hook. A client without LL has no such hook; vanilla's precise unknown-packet
handling/disconnect behavior is **not verified**. Do not claim vanilla compatibility.
Capability negotiation/modless handling remains stage 8; none was added here.

## Acceptance logs and checks

Temporary formats (runtime IDs are decimal uint64; remove after acceptance):

```text
[sync] full <name> reason=join entries=2 runtimeId=<R>
[sync] full <name> reason=respawn entries=2 runtimeId=<R>
[sync] full <name> reason=dimension entries=2 runtimeId=<R>
[sync] first applied runtimeId=<R> localRuntimeId=<L> self=true entries=<N>
[sync] first absent runtimeId=<R> localRuntimeId=<L> entries=<N>
[sync] rejected reason=<reason> revision=1 generation=<G>
[sync] session packets=<P> selfEntries=<S> remoteEntries=<T> droppedAbsent=<A> droppedStale=<D> rejected=<E>
```

For integrated self sync expect R=L, self=true and selfEntries>0. No rejection in
normal play. First absent is once per session, not a per-tick log. With a second
modded viewer, change the observed player's reload/sprint state and expect
remoteEntries>0 at exit; entering view alone intentionally does not trigger full
sync. Check normal/empty reload, interruption/refusal, sprint-stop firing and exact
zero recovery, semi-auto hold, draw/switch, respawn, dimensions, exit/rejoin and menu.
Remote data still has no visible presentation. Publication/countdown packets are
not logged per tick. Build/test results are recorded below; live checks remain pending.

## Validation — 2026-10-29

- Server and client x64 Release: `xmake f -a x64 -m release -p windows
  --target_type=<server|client> -y`, `xmake build tacz`: **build ok**.
- `xmake build tacz-tests`: **build ok**. `xmake run tacz-tests`:
  **249 cases / 922,498 assertions, Status: SUCCESS** (14 cases / 683 assertions added).
- Tests use synthetic IDs, values, clocks and recipient adapters; they do not prove
  native tracking or integrated-server runtime-ID equality. Existing macro hook
  warnings and the existing GunHud native function-pointer cast warning remain.
- Client configuration left active. All 13 changed/new files verified UTF-8 without
  BOM; `git diff --check` clean. No deployment, game launch or push.

## Changed files

- Schema/codec/store: `src/gun/SyncedShooterData.h`.
- Transport/adapters: `src/net/GunPackets.h`, `src-server/server/Shooter.cpp`,
  `src-client/client/gameplay/ClientShooter.cpp`.
- Tests: `tests/SyncedShooterDataTests.cpp`, `tests/ItemTooltipTests.cpp`.
- Release/docs: `xmake.lua`, `tooth.json`,
  `packs/behavior_packs/tacz_bp/manifest.json`, `packs/resource_packs/tacz_rp/manifest.json`,
  `CHANGELOG.md`, `docs/ROADMAP.md`, this document.

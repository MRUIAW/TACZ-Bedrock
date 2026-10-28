# 0.4.2 — sprint/fire parity with Java TACZ

## Java vs. this port

Java paths below are relative to `_reference/TACZ/src/main/java/com/tacz/guns/`.

| Rule | Java file/line | Ours before | Ours after |
| --- | --- | --- | --- |
| Held shoot stops sprint; main-hand gun, gameplay, non-spectator | `client/input/ShootKey.java:40–77` | `ClientShooter.cpp` trigger did not affect sprint | `src-client/client/gameplay/ClientShooter.cpp:346–349` passes scoped trigger intent to ClientAim, even on refused/dry/held semi-auto attempts and during draw; unknown gun data does not prevent suppression |
| Filter client sprint state and native attempts | `client/gameplay/LocalPlayerSprint.java:24–33`; `mixin/client/LocalPlayerMixin.java:108–122` | `ClientAim.cpp` ADS/reload gates only | `src-client/client/gameplay/ClientAim.cpp:245–256` extends the same input/raw/state/packet gates, without new hooks or a second sprint mechanism |
| Server sprint policy has no shoot-key override | `entity/shooter/LivingEntitySprint.java:16–24` | `src-server/server/Shooter.cpp` ADS/reload policy | Unchanged; vanilla sprint state carries the client's stop to the server |
| Accumulate/decay real elapsed seconds; cap growth at gun sprint_time | `entity/shooter/LivingEntityAim.java:92–123` | `src/pack/GunData.h` parsed value unused | `src/gun/Sprint.h`; `src-server/server/Shooter.cpp:435–447`: per-player timer grows while sprinting and not sneaking, otherwise decays to exact zero; no known gun means zero |
| Reset on selection | `entity/shooter/LivingEntityDrawGun.java:28–30`; `entity/shooter/ShooterDataHolder.java:110–120` | No sprint state | `src-server/server/Shooter.cpp:172–174` resets timer and timestamp along with existing draw state |
| Server tick and synchronization | `mixin/common/LivingEntityMixin.java:220–240` (SPRINT_TIME_KEY) | Reload-state packets, no sprint-time sync | `src/net/GunPackets.h:102–117`: initial/change-only float packet to the owner, including final zero; client mailbox uses existing reload-state mutex/world-generation pattern and preserves a fresh initial sync arriving before the first frame |
| Client rejection after preCheck | `client/gameplay/LocalPlayerShoot.java:147–156` | No sprint rejection | `src-client/client/gameplay/ClientShooter.cpp:223–224`: reject positive server-synced time before packet/recoil/animation; no local timer prediction |
| Server rejection after reload/draw/bolt-state checks | `entity/shooter/LivingEntityShoot.java:88–103` | No sprint rejection | `src-server/server/Shooter.cpp:232`: IS_SPRINTING before ammo preparation, shot timestamp or burst scheduling |
| Held rejected attempts retry, including semi-auto | `client/input/ShootKey.java:60–75` | Existing success-only trigger latch | Same success-only latch; sprint rejection returns false without consuming the press |
| Movement animation reads native sprint, not recovery time | `client/event/TickAnimationEvent.java:30–41`; `client/animation/statemachine/GunAnimationConstant.java:20` | `src-client/client/animation/GunAnimation.cpp:161,411`; `src/lua/AnimationStateMachine.cpp:296,330` use native sprint state | Unchanged: stop sprint, emit walk/idle, leave run through the pack's run_end transition; no forced animation or timer-derived run state |

`ShootKey` also stops sprint when chargeShoot returns true. Charging remains a
pre-existing unimplemented phase-4 mechanic; this change implements held-trigger
behaviour without inventing a charge path.

## Key code

### Existing hooks, extended policy only

No hook bodies, target functions or signatures changed. Existing exact-signature
assertions and world-token guards remain in place; no RVA or new placeholder-type
hooks. The shared policy in `ClientAim.cpp` now includes the scoped shoot intent:

```cpp
bool blockSprint = stopSprint || (usable && (target ||
    (gun::isReloading(reload) && !gun::isReloadFinishing(reload))));
sprintPlayer = blockSprint ? reinterpret_cast<uintptr_t>(&player) : 0;
sprintInput = (usable || stopSprint) && input ? reinterpret_cast<uintptr_t>(&*input) : 0;
sprintBlocked = blockSprint;
// Existing sender/subclient identity gates also use blockSprint.
if (blockSprint) player.setSprinting(false);
```

The existing `AimSprintInput`, `AimSprintRaw`, `AimSprintPacket` and
`AimSprintState` consume these identities/flags, retaining native sprint intent
without allowing prediction/network sprint requests while blocked. All are inert
without `shooter::worldToken()`; frame policy is reached only in an active session.

The existing state hook and its exact-signature check remain unchanged:

```cpp
static_assert(std::is_same_v<decltype(&LocalPlayer::$setSprinting), void (LocalPlayer::*)(bool)>);
LL_TYPE_INSTANCE_HOOK(AimSprintState, ll::memory::HookPriority::Normal, LocalPlayer,
    &LocalPlayer::$setSprinting, void, bool value) {
    if (shooter::worldToken() && sprintPlayer.load() == reinterpret_cast<uintptr_t>(this)) value = false;
    origin(value);
}
```

### Server tick and sync

```cpp
float elapsed = s.sprintTimestamp ? std::chrono::duration<float>(now - *s.sprintTimestamp).count() : 0;
s.sprintTimestamp = now;
s.sprintTimeS = gun::advanceSprintTime(s.sprintTimeS, elapsed,
    entry ? std::optional{entry->data.sprintTime} : std::nullopt,
    player.isSprinting(), player.isSneaking());
if (!s.syncedSprintTime || *s.syncedSprintTime != s.sprintTimeS) {
    net::SprintTimePacket{s.sprintTimeS}.sendTo(player);
    s.syncedSprintTime = s.sprintTimeS;
}
```

Tick runs after the existing ADS/reload sprint correction. As elsewhere in this
port, elapsed time uses steady_clock rather than wall-clock adjustments. The
transport is an owner-only packet rather than Forge entity data; native sprint
state still drives movement animation. No deliberate gameplay deviation or client
decay/prediction was introduced.

### Rejection

```cpp
// Client, after preCheck and before shot timestamps/effects:
if (gun::sprintBlocksShot(syncedSprintTime)) return false;
// Server, before ammo preparation and shot timestamps:
if (gun::sprintBlocksShot(s.sprintTimeS)) return "IS_SPRINTING";
// Shared predicate: sprintTime > 0, without an epsilon.
```

## Validation — 2026-10-29

- Server x64 Release: `xmake f -a x64 -m release -p windows --target_type=server -y`,
  `xmake build tacz`: **build ok**.
- Client x64 Release: corresponding client configuration and build: **build ok**;
  client configuration left active.
- `xmake build tacz-tests`: **build ok**; `xmake run tacz-tests`:
  **217 cases / 920,728 assertions, Status: SUCCESS** (7 cases / 87 assertions added).
- Pure tests use synthetic times, limits, input flags and trigger success. Animation
  regression uses the embedded default state machine with synthetic clips/context,
  verifying run → run_end → walk and shooting without a retained run loop.
- Version 0.4.2 synchronized across xmake, tooth, both pack manifests and manifest tests.
  Changelog records 0.4.1 acceptance; roadmap updates the remaining turning-sway issue.
- No game launched, deployment or push. Sprint firing/pose transitions still await
  in-game acceptance; offline tests do not establish live input/network timing.

## Changed files

- Logic/transport: `src/gun/Sprint.h`, `src/net/GunPackets.h`,
  `src-server/server/Shooter.cpp`, `src-client/client/gameplay/ClientShooter.cpp`,
  `src-client/client/gameplay/ClientAim.cpp`, `src-client/client/gameplay/ClientAim.h`.
- Tests: `tests/SprintTests.cpp`, `tests/AnimationStateMachineTests.cpp`,
  `tests/ItemTooltipTests.cpp`.
- Release/docs: `xmake.lua`, `tooth.json`, both `packs/*/tacz_*/manifest.json`,
  `CHANGELOG.md`, `docs/ROADMAP.md`, this report.

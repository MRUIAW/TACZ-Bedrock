# 0.4.9 — Lua bolt dispatch

## Scope and Java contract

Java citations are relative to `_reference/TACZ/src/main/java/com/tacz/guns/`.

- `item/ModernKineticGunItem.java:66–97,551–558`: independently dispatch
  `start_bolt` / `tick_bolt`. Nil means default (true / defaultTickBolt), a function
  is called and its first return must be boolean, any other declaration is an error.
- `entity/shooter/LivingEntityBolt.java:76–77`: set the timestamp before start;
  assign isBolting from its result. Inside start, isBolting is still false.
- `item/ModernKineticGunScriptAPI.java:312–317,372–374`: getBoltTime is zero when
  inactive, otherwise current wall-clock milliseconds minus boltTimestamp;
  adjustBoltTime subtracts alpha from that timestamp, even while inactive.
- Java API ammo/chamber writes act on the item immediately. A start hook returning
  false is not a rollback. This adapter commits its ScriptApi copy regardless of
  the result, including mutations before a protected script error.

## Dispatch and error policy

`GunLogic::startBolt` / `tickBolt` use a protected dispatcher for module lookup and
execution, including inherited/dynamic hooks and functional __index:

```lua
local fn = module[name]
if fn == nil then return nil end
if type(fn) ~= 'function' then error(name .. ': function or nil expected') end
local value = fn(api)
if type(value) ~= 'boolean' then error(name .. ': boolean result expected') end
return value
```

A valid nil dispatcher result means **absent**, not a hook returning nil:

```cpp
auto result = call(a, boltDispatch, name, *table, name);
if (!result.valid()) return false;
if (result.get_type() == sol::type::nil) return start || defaultTickBolt(a);
return result.get<bool>();
```

An absent script/module uses the same per-hook defaults. Existing missing/failed-module
loader behavior is retained. Hook call errors, bad return types, invalid hook
values and lookup errors warn once per gun ID + hook and return false. Start
does not activate; tick stops. This is the **explicitly approved reload-style
divergence**: Java throws LuaError out of the server tick; this port does not crash.
The instruction limit also applies to module lookup and hook bodies. The VM-local
budget is retained because it avoids global state and remains cleaner for
independent VMs; there is no longer a client query VM.

## Server order, time and item write-back

`src-server/server/Shooter.cpp` keeps the existing authoritative Java checks:

```cpp
s.boltTimestamp = boltNowMs();
api.boltTimestamp = s.boltTimestamp; // api.isBolting remains false during start
s.isBolting = gunLogic().startBolt(api);
s.boltTimestamp = api.boltTimestamp;
commitBoltAmmo(player, *item, api); // unconditional with respect to hook result
```

The shared commit helper compares ammo/chamber, then uses nbt::writeSelectedGun,
the existing balanced NBT path. Tick calls gunLogic().tickBolt(api), writes the
timestamp back and uses the same helper even if the result is false. Changed
timestamps are also read back after reload hook calls, since those receive the
same usertype. Inventory extraction still uses the existing authoritative callback.

ScriptApi exposes getBoltTime/adjustBoltTime. Production supplies boltClock as
the wall clock, sampled on **each getter**, and boltTimestamp as a writable copy:

```cpp
int64_t getBoltTime() const {
    return isBolting ? subtractTime(boltClock ? boltClock() : boltNowMs, boltTimestamp) : 0;
}
void adjustBoltTime(int64_t alpha) { boltTimestamp = subtractTime(boltTimestamp, alpha); }
```

subtractTime preserves Java long wraparound without C++ signed overflow. Pure
tests inject a deterministic clock. Adjusting an inactive start still changes the
timestamp for the first active tick, but getBoltTime stays zero inside start.

Existing start/chambered/finished/refusal logs remain; a non-start emits
`[bolt] <name> start_bolt returned false` once for that request, never each tick.
Hook errors additionally have their warn-once diagnostic.

## Client and retained adapters

Removed BoltCheck.scriptedHooks, SCRIPT_BOLT_DEFERRED, hasBoltHooks, and the client
query VM and its lifecycle code. Java's client does not inspect gun scripts;
script-hook guns now acquire the same lock/latch and send the same empty request.
The generic resolver no longer labels guns staged. No installed-gun identity is
used in code or new synthetic tests.

The 1423ce3 per-frame main-hand latch observation remains before any shot. Auto
bolt still attempts once per completed tick, after applyServerData and before
inputs; completion is published after tickStateLock. The unchanged latch only
clears on chambered/non-gun observation or existing draw/world/respawn reset.
No refusal ack, extra death cancellation, server fire-select bolt guard, protocol
change or new native hook was added. Retained approved adapters are the translated
shoot clock/offset, pending-burst restrictions, pre-native publication phase,
player-only fixed schema and stage-8 StartTracking sync deferral.

## Scratch installed-pack scan — 2026-10-30

Scratch report (not committed):
`C:\Users\MRUIAW\AppData\Local\Temp\opencode\tacz-0.4.9-bolt-scan.json`.
The production registry supplies resolved modules/parameters and GunLogic runs
start/tick against synthetic ammo, at 10 ms steps, for each declared fire mode and
magazine counts 1 and 3. The previous scan selects the 15 formerly staged guns;
module bodies were inspected for remaining attachment/shoot/inventory dependencies.
Scratch instrumentation was removed from the resolver source after the scan.
This is offline evidence, not live acceptance or a claim of complete gun support.

**Ten now have complete bolt cycles without a known related mechanism gap:**

- `ccrp:lastwar`, `ccrp:m1887_long`, `ccrp:marlin_1895`, `ccrp:springfield1873_tube_mag`.
- `cyber_armorer:carnage`, `cyber_armorer:carnage_guts`, `cyber_armorer:m2038`,
  `cyber_armorer:m2038_bloody_maria`, `cyber_armorer:m2038_the_headsman`.
- `tacz:m870`.

These read script_param bolt_time/bolt_feed_time and chamber one magazine round;
their shoot hooks are simple shootOnce passthroughs, still not dispatched until
0.4.11. No special-case implementation is used to emulate those hooks.

**Five retain known gaps:**

| Gun | Bolt result and remaining dependency |
| --- | --- |
| `classicr:kar98` | Chambers/finishes at 1250 ms using only bolt_time; reload calls getAttachment, deferred to 0.4.10 |
| `tacz:kar98` | Feeds 680 ms, finishes 870 ms; reload also calls getAttachment, deferred to 0.4.10 |
| `tacz:spas_12` | Bolt feeds 300 ms, finishes 530 ms; SEMI can use normal cycling, BURST's shoot hook rechambers after shootOnce and awaits 0.4.11 |
| `sfms:super90m2` | tick_bolt only cycles when magazine == 1; with 3 it returns false without chambering. Its shoot hook normally rechambers and has an extra-shot branch; remains dependent on 0.4.11. Do not replace this with default bolt |
| `sfms:tb23` | start_bolt calls missing getAttachment; expected protected failure. Also needs inventory-direct shooting and attachment-dependent shoot/async behavior; remains later work |

All 13 other than Super90/TB23 chambered and finished for both tested magazine
counts, without bolt-hook warnings. Super90's one-round probe feeds at 450 ms and
stops at the next 10 ms step because its magazine is no longer 1, exactly its script.
TB23 generated the expected getAttachment warning; no unsupported API was stubbed.

## Accepted 0.4.8 and live checklist

User-tested acceptance: AWP/M700 auto bolt with trigger released/held and no extra
held shot; fire/reload refused during bolt; zero-action M107/M109 did not wedge;
switch mid-bolt preserved ammo; staged M870/Kar98 had no fake bolt. AK47, respawn,
dimension and rejoin passed. Immediate post-finish firing wedge fixed by 1423ce3
passed retest. Empty magazine was not separately reported; logs show empty reload
after bolting without repeated bolts, not a stronger acceptance claim.

0.4.9 live acceptance remains pending:

1. **Pump/lever + script total/feed params:** `tacz:m870`, `ccrp:m1887_long`,
   `ccrp:marlin_1895`: fire then release/hold trigger; one round feeds at the script
   time, no firing/reload before completion, no double transfer or extra held shot.
2. **M870 shell-by-shell reload:** after bolting, reload partially empty magazine,
   confirm one shell at a time; fire to interrupt and confirm normal cycling resumes
   after the script's finishing phase. Test empty reload as well as partial reload.
3. **Rifle with total time only:** `classicr:kar98` chambers at the total time; use
   existing magazine rounds. Its reload is intentionally not accepted until 0.4.10;
   `tacz:kar98` has the same attachment-related reload gap.
4. **Shoot-hook gun in SEMI:** `tacz:spas_12` cycles using its bolt hook. Do not
   interpret BURST or Super90's missing shoot behavior as a bolt regression; those
   remain 0.4.11. `cyber_armorer:carnage` is another SEMI bolt/feed example.
5. **Regressions:** AWP/M700, immediate shot after bolt completion, zero-action
   M107/M109, switching before/after feed, no ammo, AK47 reload/fire, respawn,
   dimensions and rejoin. Confirm low-volume edge logs and no staging refusal.
6. **Expected unsupported:** TB23 may warn once and retain Java's refusal latch
   until draw/non-gun; it is not ready for acceptance. Synthetic tests cover a
   start hook that chambers then returns false and timestamp adjustment.

## Offline tests

Synthetic tests cover independent defaults, both hooks, inherited/dynamic lookup,
strict function/boolean validation, warn-once identity, exceptions/runaway scripts,
false-start mutation preservation, active/inactive clock reads, per-getter clock
sampling, adjusted timestamp write-back, script params and one-time feed/finish.
The existing latch regression, timing/precondition matrices, snapshot and reload
tests remain.

- Server then client x64 Release: **build ok**; tests build: **build ok**.
- Full suite: **293 cases / 925,080 assertions passed** (8 new cases; net 222
  additional assertions). Existing hook/cast/u8path warnings remain.
- Client configuration left active; whitespace and UTF-8-without-BOM checks pass.
  No push, deployment or game launch; live acceptance remains pending.

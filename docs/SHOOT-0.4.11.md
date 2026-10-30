# Lua shooting and scheduled tasks — 0.4.11

2026-10-30. Implementation complete; **live acceptance pending**. No installed
pack/gun/namespace exceptions. Tests use synthetic modules, not installed packs.
The 0.4.10 BDS/client acceptance results are recorded in CHANGELOG and ROADMAP.

## Java mechanism and the port

`J` below means `_reference/TACZ/src/main/java/com/tacz/guns/`.

| Mechanism | Java source | 0.4.11 |
| --- | --- | --- |
| Validate pull, pre-chamber closed bolt, previous/current timestamps | `entity/shooter/LivingEntityShoot.java:51–159` | Keep existing validation order and balanced selected-slot NBT writes; update previous shot and heat wall timestamp before dispatch |
| Function-or-nil shoot lookup, ignored hook result, default `shootOnce(consumesAmmo)` | `item/ModernKineticGunItem.java:100–118` | Protected lookup/call; nil alone defaults; a present non-function/errors stop the call, not fallback firing |
| Independently capture mode, bullet/speed data, inaccuracy, burst count/period | `item/ModernKineticGunScriptAPI.java:105–203` | Each `shootOnce` creates a separate task; immutable registry data is shared by const reference, scalar mode/inaccuracy/count/period captured at call time |
| Per-cycle life/item check, optional ammo reduction, bullet emission | same file `:151–202`, `:233–292` | Same per-cycle checks through approved hold identity; existing `fireRound`/`bullet::fire` path, no fork |
| Safe async function and strict boolean result | same file `:686–689` | Owned Lua API userdata and callback; invalid function/result or Lua error warns once and stops call/task |
| Float seconds, compensation, count and delayed-branch quirks | `util/CycleTaskHelper.java:18–99` | Pure injectable-clock `src/gun/CycleTasks.h`; see phase difference below |
| Timing getters/adjustment | `item/ModernKineticGunScriptAPI.java:324–356` | Data interval minus 5 ms, clamped to zero; previous raw timestamp + wall base; wrap-safe addition to current timestamp |
| Heat dispatch even without heat data | `entity/shooter/LivingEntityHeat.java:16–21`; `item/ModernKineticGunItem.java:208–226` | Selected gun's `tick_heat(api, heatTimestamp)` after bolt, before publication; nil default is no-op |
| Draw reset then heat wall time | `entity/shooter/ShooterDataHolder.java:110–127`; `LivingEntityDrawGun.java:28–37` | Unconditional selection cache reset; heat `-1` then current wall time; accepted pulls also set wall time |

### Key implementation excerpts

Scheduler (`src/gun/CycleTasks.h`):

```cpp
float duration = static_cast<float>(now() - t.timestamp) / 1000.f + t.compensation;
if (t.delay > 0) {
    if (t.delay > duration) { t.delay -= duration; return true; }
    t.delay = 0;
    duration = duration - t.delay + t.period;
}
// Later: while (t.compensation > t.period), not >=.
```

The pre-delay branch deliberately does **not** update timestamp; the elapsed
time since creation is subtracted repeatedly. The later subtraction deliberately
uses the already-zero delay. `cycles > 0` alone enables a count limit: `-1`, zero
and other negative counts are unlimited until false/lifecycle cancellation.
Absent `burst_data.count` defaults to **3**, not 1. Count-zero/until-dry is tested.
Zero/nonpositive periods retain Java behavior; packs must not schedule an
unlimited nonpositive-period callback that always returns true.

Dispatch (`src/lua/GunLogic.cpp`, protected Lua trampoline):

```lua
local fn = module[name]
if fn == nil then return false end
if type(fn) ~= 'function' then error(name .. ': function or nil expected') end
if name == 'tick_heat' then fn(api, timestamp) else fn(api) end
return true
```

`shootOnce` captures before scheduling; `add` executes a no-delay task immediately:

```cpp
auto mode = a.fireMode;
auto cycles = mode == pack::FireMode::Burst ? a.data->burst.count : 1;
auto period = mode == pack::FireMode::Burst ? gun::burstShootIntervalMs(*a.data) : 1;
auto body = a.prepareRound(a, consume);
schedule(a, std::move(body), 0, period, cycles, gun::CycleTasks::Kind::Shot);
a.refreshView();
```

`Shooter.cpp` keeps validation/pre-chambering, then replaces the single burst:

```cpp
s.previousShot = s.lastShot.value_or(-1);
s.heatTimestamp = boltNowMs();
s.lastShot = timestamp;
auto api = scriptApi(player, *item, *entry, s);
gunLogic().shoot(api);
api.commitView();
```

Thus immediate round reduction precedes script re-chambering, and the next
`shootOnce` or later cycle sees those mutations. Errors do not roll them back.
Magazine-fed manual/closed/open reduction matches Java. Inventory-fed reduction
and server pre-chamber validation remain the existing unsupported path; TB23 is
not silently advertised as implemented.

## Owned API, identity and lifecycle

Lua receives a `shared_ptr<ScriptApi>` rather than a pointer to a stack snapshot.
Every retained server closure owns its original-item context and resolves the
player by key; no borrowed player/state/NBT pointer escapes. Context identity is
**selection generation + slot + gun ID**, with generation bumped on every
observed selection change. No native stack network IDs are used.

- While identity matches, item methods refresh from/write to the live selection.
- After switching, ammo/chamber/attachment methods use detached owned original
  state and **never write to any inventory item**. Actual player-ammo extraction
  still addresses the shared player inventory with the original ammo ID, as in
  Java; it is separate from writing a replacement gun.
- Timing and script cache remain shared player state. Async callbacks survive a
  weapon switch. Shot tasks are canceled; a delayed callback can run but cannot
  fire from an old hold, including after switching away and back.
- Death/respawn observation, disconnect, world end and disable clear tasks.
  Queued life resets avoid reentrant locking when a shot causes death. Disconnect
  also removes the player's scheduler/cache; world end destroys the VM.
- Maximum **256 retained tasks per player**, including reentrant immediate calls.
  Excess tasks are rejected with one warning per gun; existing tasks continue.
- Pending shot tasks block shoot/reload, retaining the port restriction formerly
  expressed as `s.remaining > 0`. Async tasks alone do not block either action.

Java checks life/item equality inside the immediate first cycle. The WIP's early
check was removed (review item 8); preparation now runs before that check, as in
Java. Preparation only reads player/data and builds a closure: no ammo, cache or
inventory mutation. The stale-hold test verifies preparation followed by no
shot/mutation/pending task.

### Data lifetime (review item 7)

Per-shot whole-`GunData` copies were removed. Closures share the same immutable
registry data through `const GunData*`. `reloadGunPacks()` has exactly one caller:
mod `load()`, before shooter enable; registry entries are not replaced during a
world session. Tasks are destroyed before mod shutdown. This proves the current
reference lifetime without per-shot allocation/copy. **Runtime pack reload is
deferred to stage 7**; it must add snapshot ownership/invalidation before enabling
replacement while these references or any other existing registry views live.

## Clock mapping and resets

`s.lastShot` is Java's current `shootTimestamp`, on the client-steady timeline.
`s.previousShot` is its previous raw (possibly script-adjusted) value. Our existing
`clockOffset` converts client-steady to server-steady, not directly to wall time:

```text
base = clockOffset + (serverWallNow - serverSteadyNow)
getLastShootTimestamp = previousRaw + base
```

Before an offset exists, use the holder's construction-time wall base. Draw resets
the current raw timestamp, not `lastShootTimestamp`; the first accepted shot then
copies `-1` into previous, so its hook sees **base - 1**, not literal `-1`. The
second sees the first shot's timestamp plus base. This follows executable Java,
not its misleading Javadoc. Both synchronized/no-offset cases are tested.
`adjustShootInterval(alpha)` wrap-adds to the current timestamp, so positive alpha
delays the next permitted pull. `getCurrentTimestamp()` samples wall time.

`safeAsyncTask` arguments match FiguraMC LuaJ `CoerceLuaToJava.NumericCoercion`:
long is Java `(long)value.todouble()`; int is `value.toint()` (double→long, then
32-bit narrowing). Numeric strings work, other types default to zero, NaN→0,
long overflow saturates. `shootOnce` boolean uses existing `luaJBoolean`: only
nil/false are false, including numeric zero being true. Callback/hook boolean
return checks remain strict. `src/lua/NumericCoercion.h` and synthetic tests cover
these rules; ordinary existing numeric API bindings are not rewritten.

### `initialData()` audit / retained differences

Explicit Java callers: draw; server dimension travel (`event/TravelToDimensionEvent.java:20`);
server `restoreFrom` (`mixin/common/ServerPlayerMixin.java:12–15`); client clone
after 10 ticks (`client/event/RefreshClonePlayerDataEvent.java:20–25`). No separate
direct death/login caller was found. Only cache/heat reset changes were authorized.

| Java initializer field | Selection reset here |
| --- | --- |
| shootTimestamp | Reset `lastShot`; previous timestamp deliberately retained until next accepted pull |
| meleeTimestamp, meleePrepTickCount | Not implemented; unchanged |
| isAiming, aimingProgress | Reset false/zero |
| reloadTimestamp, reloadStateType | Stop active reload and clear reload result/time |
| sprintTimestamp, sprintTimeS | Reset recovery clock/time |
| boltTimestamp, isBolting | Reset -1/false |
| shootCount, chargeProgress | Not implemented; unchanged |
| scriptData | Now cleared unconditionally, including outside an active reload |
| heatTimestamp | Reset -1, then draw wall time; wall time on each accepted pull |

Other accepted differences remain: dimension travel/respawn do not reproduce the
full Java holder initialization, client clone has no equivalent delayed full
reset, and our selection reset additionally refreshes `aimingTimestamp`. Java's
initializer leaves that field alone. No fixes to these were folded into 0.4.11.
Java reload cancellation only calls `interruptReload`; a missing gun index returns
idle reload state without clearing cache (`LivingEntityReload.java:tickReloadState`).
Our `stopReload` no longer clears cache, including the missing-data path.

## Accepted scheduler-phase divergence (review item 4)

`J/event/ServerTickEvent.java:11–14` does not filter phase.
[Forge 1.20.1 ForgeEventFactory:968–975](https://github.com/MinecraftForge/MinecraftForge/blob/1.20.1/src/main/java/net/minecraftforge/event/ForgeEventFactory.java#L968-L975)
posts START and END; its
[MinecraftServer patch](https://github.com/MinecraftForge/MinecraftForge/blob/1.20.1/patches/minecraft/net/minecraft/server/MinecraftServer.java.patch)
places these around server tick processing. Consequently Java pumps twice:
**two unevenly spaced checkpoints**, not regular 40 Hz or guaranteed 25 ms gaps.

By explicit user decision, Bedrock keeps **one pump per player/server tick**,
after aim/sprint and before reload. No new native hook. Existing LL
`ServerLevelTickEvent` runs before native subTick; publication remains in its
already-approved pre-native phase. `tick_heat` remains after bolt/before publication.

- An already-queued burst round can advance at Java END but must wait for our
  next pump. At steady **20 TPS**, allow roughly **up to one tick (~50 ms)** extra
  checkpoint latency. Compensation preserves catch-up; this does not halve RPM.
- A one-cycle delayed callback is different: Java subtracts elapsed time since
  creation on **both** checkpoints. Its delay therefore often expires earlier
  than ours, not just at a finer sampling boundary. There is **no delay-independent
  one-tick bound**. With requested delay `D` ms, cumulative elapsed is roughly
  `25*n²` once vs `50*n²` twice after `n` ticks; skew grows roughly with sqrt(D).
- For a requested **1-second**, one-cycle task at 20 TPS, a rough conservative
  phase/quantization allowance is **~150 ms later than Java**, absent stalls.
  Example pinned by test: once-per-tick fires at **300 ms**, Java START + END
  (END 10 ms after START) at **210 ms**, a **90 ms** difference. The burst's
  second round at period 100 ms is **150 vs 110 ms**, a **40 ms** difference.
  A fixed-phase sweep of integer creation/END offsets found ~133 ms maximum skew
  for that 1-second example; 150 ms is a rough estimate, not an engine guarantee.
  Arbitrary server stalls/wall-clock changes have no such bound.

The synthetic **20 TPS phase contract** test deliberately pins both streams so a
future pump-rate change must revisit this accepted behavior, especially Colt
Python and the multi-stage delayed melee scripts.

## Heat dispatch, transition audit and cost (review item 6)

No-script guns now skip API/NBT construction: default cooling is deferred, so
there is no observable default work to perform. Guns with a script still build
an owned API even when `tick_heat` is nil: one original `CompoundTag` clone per
tick if NBT exists (**20 clones/sec/held player at 20 TPS**), context/closure
allocation and a protected lookup. A warmed 5,000-call synthetic benchmark gives
**~0.60–0.66 µs/call for the missing-hook protected dispatch alone**. Native API
construction/NBT clone cost is explicitly excluded and unmeasured offline; no
game was launched to claim an end-to-end number. Clone cost scales with item NBT.

Read-only installed-source audit (scratch instrumentation removed from resolver):
30 physical cache-writing shoot/reload/bolt modules × 5 physical failing heat
variants = **150 switch-and-return pairs, zero failures**. The writer hooks were
exercised; 27 produced cache in the fixture. Remaining unreachable/unreferenced
branches were conservatively seeded with a cache marker. After each simulated
selection clear, the real heat hook saw nil; ammo/chamber/cache/shot-task state
was unchanged before its first missing API call, and cache was nil on return.
The generic regression test covers writer A → heat B → A without pack names.

| Heat script | First missing call after switch | Pre-failure mutations | Warm µs/call* |
| --- | --- | --- | ---: |
| `tacz:sp_heat` default | `hasHeatData` | None | 4.98 |
| `tacz:sp_heat` TWX (identical bytes) | `hasHeatData` | None | 6.17 |
| `sfms:alternative_heat_logic` | `getHeatAmount` with nil cache | None | 8.46 |
| `sfms:changed_c8msw_logic` | `getHeatAmount` | None | 6.27 |
| `sfms:changed_cms901_logic` | `getHeatAmount` | None | 6.31 |

\* 5,000 warmed protected calls, engine/API/NBT construction excluded. Errors still
execute every tick; only warnings are suppressed after the first per gun/hook.
This is a small nonzero new dispatch cost, not implemented heat simulation.

With an **existing same-gun cache**, `alternative_heat_logic` caches data and
`ammoTimestampFix` replaces magazine count with a timestamp before failing at
`hasHeatData`. That is the script's intentional Java design, not corruption to
roll back. The regression was **previous-gun cache leaking through a switch**,
which skipped its nil-cache branch; unconditional selection clearing fixes it.
The earlier seeded-cache failure probe cost ~6.4 µs/call; no chamber/task changes.
The effective `sp_heat` module has no directly referencing installed gun.

Mantis Blade and Maxtac are not excluded: `shoot` writes cached attack/input
state; a later `tick_heat` refills its one-round magazine and calls
`shootOnce(false)` repeatedly according to that cache. The synthetic regression
mirrors shoot→cache→later-tick refill/fire. It does not fake heat APIs or melee.

## Installed `M.shoot` callers: before vs 0.4.11

**D** = 0.4.10 bypassed the hook and ran one consuming data-defined burst per pull.
Rows are all **27 physical definitions**, including the Devotion/M870 duplicate
providers. `unused` means no effective installed gun references that module (D
would apply if assigned). Sources are audited, not patched or used as test fixtures.

| # | Script / physical provider when duplicated | Today (0.4.10) | 0.4.11 |
| ---: | --- | --- | --- |
| 1 | `cyber_armorer:carnage_logic` | D | Same shot; existing staged bolt/reload retained |
| 2 | `cyber_armorer:m2038_logic` | D | Same shot; existing staged bolt/reload retained |
| 3 | `cyber_armorer:mantis_blade_logic` | D; no heat-tick attack flow | Cache-driven later `tick_heat` refills/fires; default spread, not real melee |
| 4 | `tacz:db_short_gun_logic` | D | Two independent shootOnce bursts in BURST |
| 5 | `tacz:devotion_lmg_logic` — default | unused | Supported previous-shot/interval acceleration if assigned |
| 6 | `tacz:kar98_gun_logic` | D | Same shot, Lua bolt/reload retained |
| 7 | `tacz:m870_gun_logic` — default | D (shadowed provider) | Same shot; last-wins module resolution unchanged |
| 8 | `tacz:spas_12_gun_logic` | D; BURST lacked scripted re-chamber | Immediate shot then magazine→chamber before later cycles |
| 9 | `tacz:devotion_lmg_logic` — TWX | unused | Same acceleration API support; last-wins provider unchanged |
| 10 | `tacz:m870_gun_logic` — TWX | D | Same shot; effective module serves M870 and other referencing guns |
| 11 | `classicr:kar98_gun_logic` | D | Same shot, accepted reload/bolt paths retained |
| 12 | `classicr:mgl_gun_logic` | unused | SEMI shot / BURST shot then re-chamber if assigned |
| 13 | `classicr:python_logic` | D; immediate | `safeAsyncTask` requested 1 s delayed shot (phase quirk above) |
| 14 | `sfms:alternative_heat_logic` | D; consuming | `shootOnce(false)`; heat APIs still unavailable, nil-cache heat failure is non-mutating |
| 15 | `sfms:bamboo_mk1_logic` | D | Cached light combo, interval changes, delayed/multiple non-consuming shots |
| 16 | `sfms:c8msw_logic` | unused | AUTO shot / SEMI shot and extra magazine removal if assigned |
| 17 | `sfms:changed_c8msw_logic` | D | AUTO consuming shot; SEMI consuming shot plus parameter-defined non-consuming shots; heat deferred |
| 18 | `sfms:changed_cms901_logic` | D | AUTO shot; SEMI parameter-defined consuming shot calls; heat deferred |
| 19 | `sfms:changed_xm1014_logic` | D | Re-chamber logic and last-magazine `shootOnce(false)` extra shot |
| 20 | `sfms:har50_logic` | D | AUTO shot; SEMI shot then remove five magazine rounds |
| 21 | `sfms:inf_x_logic` | D | SEMI shot; conditional extra independent BURST task |
| 22 | `sfms:jak_wardens_akimbo_gun_logic` | D | Shot plus shared-cache left/right alternating interval |
| 23 | `sfms:jumonji_yari_mk1_logic` | D | Timed light/heavy nested callbacks and interval changes; default spread, not real melee |
| 24 | `sfms:kastham_knife_mk1_logic` | D | Cached light/heavy combos, delayed multi-shots and interval changes; default spread |
| 25 | `sfms:katana_logic` | D | Parameter-defined delayed non-consuming shot; default spread |
| 26 | `sfms:noammo` | unused | Would fail at nonexistent autoFire/burstFire/singleFire if assigned; not a current regression |
| 27 | `sfms:tactical_bow_logic` | Inventory-direct feed already deferred | Script dispatch available; TB23 inventory feed remains deferred, not claimed working |

The `sfms:m4a5` reference to `trisdyna:noammo` is commented out; it is not an
active missing-method regression. Resolved MGL guns use other modules, and the
current effective registry does not reference Devotion's installed module.
Scratch call audit covered **42 referenced physical script/gun pairs** across
their declared modes and aim 0/1, with 2,400 simulated 50 ms task pumps per
scenario: **zero shoot/async warnings**. The five unreferenced provider rows were
not treated as live gun scenarios. This is offline API reachability evidence,
not live ballistics/client/animation acceptance or full heat implementation.

Installed async callbacks use shootOnce/isShootingNeedConsumeAmmo/adjustShootInterval;
TB23 additionally extracts player ammo and sets chamber. This is why detached
item handling is tested even though inventory-direct shooting remains deferred.

## Other known differences / deferred work

- Error policy: warn once and stop call/task rather than Java propagation. The
  existing sandbox/instruction budget remains; task bound/lifecycle cleanup are
  deliberate safety/lifetime differences from Java's global unbounded list.
- Identity: switching away and back cancels firing here, unlike returning to the
  same retained Java stack object. Detached state is not written back to the
  original inventory stack after switching. Async timing/cache remain shared.
- Wall-base mapping, once-per-tick phase, pending-shot shoot/reload restriction,
  and incomplete initializer coverage are described above; no hidden adapter
  exclusions. Mutable firearm property/attachment modifier caches are not newly
  implemented; immutable registry data keeps existing bullet defaults.
- Heat APIs, heat amount/overheat handling and Java `defaultTickHeat` cooling
  (`ModernKineticGunItem.java:228–241`) remain **stage 4 item 2**. Missing
  `tick_heat` is no-op even for heat-data guns. `handle_shoot_heat` is not dispatched.
- `calcSpread` stays default, no worse than 0.4.10. Definitions found:
  `cyber_armorer:mantis_blade_logic`; `tacz:sp_spread_logic` default + TWX;
  `sfms:bamboo_mk1_logic`, `changed_c8msw_logic`, `changed_cms901_logic`,
  `jumonji_yari_mk1_logic`, `kastham_knife_mk1_logic`, `katana_logic`.
- Real melee, TB23 inventory-direct feed, charge and shoot-count integration are
  not implemented. No Forge fire-cancellation events or new third-person events.

### Client prediction is unchanged, and matches Java's limitation

`J/client/gameplay/LocalPlayerShoot.java:240–305` predicts one ammo-limited,
data-defined burst per pull; it does not dispatch gun Lua shooting. Extra server
rounds do not gain local sound/recoil/animation/`[recoil-shot]` predictions.
`ModernKineticGunScriptAPI.java:163` sends fire via
`NetworkHandler.sendToTrackingEntity` (`network/NetworkHandler.java:159–160`,
Forge `TRACKING_ENTITY`), which excludes the shooter. Java therefore has the
same script-added-round prediction limit. Server item writes correct ammo/HUD;
we made no client-prediction changes. Missing other-player fire events/sound
remain stage 8, not disguised as a local prediction fix.

## Validation and live checklist

Automated coverage includes timing/strict boundaries/compensation/counts,
once-versus-twice phase contract, independent tasks, consume true/false for all
bolt types, immediate re-chamber ordering, delayed callback lifetime/errors,
switch and switch-back rejection, detached writes, lifecycle cleanup, shared
cache reset, previous-shot wall mapping and the Mantis-style later-tick flow.
Build server Release, then client Release, then tests; finish with client config.
No push/deploy/game launch is part of this change.

Final validation: **server Release OK → client Release OK → tests OK**, client
configuration left active. **328 cases / 925,769 assertions** (baseline 305 /
925,258). All three PowerShell helpers parse successfully without executing live
input/console/pipe operations. Modified files validate as UTF-8 without BOM or
replacement characters; scratch resolver instrumentation was removed and its
normal executable rebuilt.

Pending live checks (manual helpers: `tools/e2e/README.md`):

- [ ] SPAS-12 BURST immediate shot→re-chamber→later cycle; ammo and bolt recovery.
- [ ] Super90 last-magazine extra `shootOnce(false)` and subsequent reload/bolt.
- [ ] db_short BURST double independent tasks, no accidental duplicate consumption.
- [ ] Colt Python requested 1 s delay; expect scheduler quirk timings above, no
      immediate shot; switch/switch-back/death cancel firing from the old hold.
- [ ] Devotion acceleration if testing a pack that actually references the module.
- [ ] M1887s alternating left/right interval and ammo/bolt state.
- [ ] Mantis/Maxtac shoot→later tick refill/fire, repeated input, switch/cache reset.
- [ ] Default fallback regression: M16A4 BURST, AK47 AUTO; Kar98/M870 reload/bolt.
- [ ] No old-gun ammo writes after delayed callbacks; death/respawn/rejoin/world-end
      cleanup; heat guns warn once without leaked-cache timestamp mutations.

0.4.10 live results remain accepted; none of these 0.4.11 live checks are claimed
complete from offline tests.

# 0.4.8 — Java default manual bolt

## Java mechanism and scope

Java paths below are relative to `_reference/TACZ/src/main/java/com/tacz/guns/`.

| Rule | Java file/line | This release |
| --- | --- | --- |
| Server start checks | `entity/shooter/LivingEntityBolt.java:27–78` | Server-selected item/data; shoot cooldown != 0, reload, draw != 0, already bolting, manual-action, empty chamber, feed ammo; no extra sprint/death/burst guard |
| Default start/tick dispatch | `item/ModernKineticGunItem.java:66–97` | Default start=true and default tick only; any declared Lua bolt hook is temporarily staged out |
| Default feed/action | `ModernKineticGunItem.java:340–359` | Float multiplication then long narrowing, exact -1 feed fallback, feed one round once; feed may be before or after action |
| Bolt elapsed getter | `ModernKineticGunScriptAPI.java:312–317` | ScriptApi::getBoltTime returns wall-clock elapsed while active, otherwise zero; not exposed to Lua until 0.4.9 |
| Tick and publish | `mixin/common/LivingEntityMixin.java:223–236` | Bolt ticks after reload/aim/sprint, before existing end-of-shooter-tick publication; eight keys now produced |
| Shoot/reload exclusion | `LivingEntityShoot.java:97–98`; `LivingEntityReload.java:57–59` | Both reject active bolt; a chamber filled early is not permission to fire |
| Client operation | `client/gameplay/LocalPlayerBolt.java:26–78` | Lock + separate local latch, current item/data/display/ammo, BoltPacket, then sound/input if a state machine exists |
| Auto cycle/latch clear | `LocalPlayerBolt.java:81–94` | Once per completed client tick, regardless of trigger; clear local latch for non-gun or a chambered item, not on refusal/timeout |
| NEED_BOLT fallback | `LocalPlayerShoot.java:233–235` | Unchambered manual shot calls the same bolt() path |
| Lock and resets | `LocalPlayerDataHolder.java:90–118,124–137`; `LocalPlayerDraw.java:104–121` | Existing Bolt predicate/release, latch reset on selection/world/observed respawn; no extra death cancellation |
| Presentation | `GunAnimationConstant.java:8`; `SoundPlayManager.java:159–160` | Input literal **"blot"**, sound key **"bolt"**, volume/pitch 1; no fabricated clip or HUD meter |
| Server fire select | `LivingEntityFireSelect.java:23–39` | No new bolt guard; client operation lock still blocks fire-select/inspect |

The accepted 0.4.7 lock, predicted ADS and local shot cadence remain. Lua
start_bolt/tick_bolt/getBoltTime exposure is 0.4.9; data-side getAttachment is
0.4.10; Lua shoot/shootOnce is 0.4.11; inventory-direct shooting remains later.

## Default progression and server adapter

`src/gun/BoltRules.h` contains game-independent precondition/latch rules and
Java float-to-millisecond conversion (no negative-time clamp). The default path
in `src/lua/ScriptApi.h` mirrors Java directly:

```cpp
auto action = gun::boltMillis(api.data->boltActionTime);
auto feed = api.data->boltFeedTime == -1.f ? action : gun::boltMillis(api.data->boltFeedTime);
if (api.getBoltTime() < feed) return true;
if (!api.hasAmmoInBarrel()) {
    if (api.useInventoryAmmo()) {
        if (api.consumeAmmoFromPlayer(1) == 1) api.setAmmoInBarrel(true);
    } else if (api.removeAmmoFromMagazine(1) != 0) api.setAmmoInBarrel(true);
}
return api.getBoltTime() < action;
```

Thus early feed remains active until action; feed > action remains active until
feed. Zero action/default feed can chamber and finish on its first server tick.
An existing chamber prevents repeated transfer. Inventory-fed feed consumes one
inventory round, or a creative virtual round using the existing ScriptApi rule;
magazine-fed creative still needs a magazine round. Failed feed does not invent
ammo. These mechanics do not depend on a clip or its length.

`src-server/server/Shooter.cpp` stores isBolting and a wall-clock boltTimestamp.
handleBolt observes selection, builds BoltCheck from authoritative item/data,
translated shoot cooldown, draw getter and stock/creative state, then:

```cpp
check.scriptedHooks = gunLogic().hasBoltHooks(entry->data);
if (auto reason = gun::serverBoltRefusal(check)) return reason;
s.boltTimestamp = boltNowMs();
s.isBolting = true; // Java default startBolt
```

tickBolt runs after tickReload. Missing gun/data ends bolting; otherwise its
ScriptApi copy is ticked and changed magazine/chamber NBT is committed through
nbt::writeSelectedGun, the same balanced path as reload. Selection resets the
bolt flag/timestamp, while a previously fed round remains in its old item. State
map removal handles disconnect/server stop/disable. No new native hook.

BoltPacket is empty client→server intent. Only the existing is_bolting sync key
reports state: self plus trackers, full snapshots contain all eight keys,
protocol revision 1 unchanged. No bolt reply/ack/correlation token.

Low-volume server lines (not per-tick diagnostics):

```text
[bolt] <name> started <gun>
[bolt] <name> chambered
[bolt] <name> finished
[bolt] <name> refused: <reason>
```

## Client ordering, latch and presentation

The approved adapter for Java's single-thread order is:

```cpp
// Existing ClientLevelTickEvent listener, no new bolt engine/network work here:
aim::tick();
tickStateLock();
if (worldSession.current()) clientTickCount.fetch_add(1, std::memory_order_release);

// frame(), immediately after applyServerData, before keys/trigger handling:
auto completedTick = clientTickCount.load(std::memory_order_acquire);
if (completedTick && autoBoltTickGate.tryAcquire(completedTick)) tickAutoBolt(mainHand);
```

This is an ordering adapter, **not a divergence**. ClientLevelTickEvent vs update
thread equality remains unverified. The atomic marker is published after the
mutex-protected lock evaluation. Java bolt()/tickAutoBolt read no synced values:
only clientStateLock, the local latch and current main-hand item/data/display.
Our bolt() reads that current protected lock, including any earlier same-frame
acquisition (e.g. selection's Draw lock). Frame applies incoming data first, then
auto-bolt runs before fire-select/reload/inspect/ShootKey handling, matching player
tick before Java ClientTickEvent END. No captured synced snapshot is needed.
The reload-cancel gate keeps the same counter and remains at most once per real
client tick; shooting is not tick-gated. Skipped ticks are not replayed as bursts
of auto-bolt calls, and tick zero is not treated as a completed tick.

Client bolt checks the general lock and local latch, not the individual synced
shoot/draw/reload values. After item/data/display/manual/chamber/ammo checks and
the staging query, it acquires LockCondition::Bolt, sets the latch, sends the empty
request, and uses sound "bolt" plus anim::bolt() → "blot" if a state machine
exists. No new clip-presence check blocks gameplay. Existing local handoff/
put-away presentation machinery is retained.

tickAutoBolt calls bolt before clearing a chambered latch, as Java does. The latch
is not a second authoritative bolt flag and is not cleared by the 250 ms timeout:
a refused empty-chamber operation can stay latched until chamber sync, draw, or
non-gun. This Java limitation is explicitly accepted. World/observed respawn reset
also clears it. Early chamber sync can clear the latch while authoritative
is_bolting still keeps the operation lock and shoot precheck blocked.

Live-test fix (2026-10-30): frame() also observes the main-hand item every frame before any shot, so a newly synced chamber clears the latch before being consumed; bolt attempts remain once per completed tick and latch-clear conditions are unchanged.

## Temporary script staging rule and query VMs

For 0.4.8 only, a script declaring start_bolt **or** tick_bolt is not bolted. Both
server and client use `GunLogic::hasBoltHooks(GunData const&)`. Client returns
before locking/latching/sending/animating; server returns without starting. This
is the approved temporary difference from Java and is removed in 0.4.9.

```cpp
auto table = mImpl->script(*data.script); // production require/module cache
sol::object start = (*table)["start_bolt"];
sol::object tick = (*table)["tick_bolt"];
return start.get_type() != sol::type::nil || tick.get_type() != sol::type::nil;
```

It uses executed module tables, including require/metatable inheritance and
dynamically assigned functions, never source regexes or gun names. Non-nil malformed
declarations also defer rather than accidentally enabling a default; Java's
checkFunction rejects such declarations (`ModernKineticGunItem.java:551–558`).
No hook body is invoked. Missing/failed modules retain the existing loader policy
(no table, no function), with the existing once-only load warning.

The client owns a frame-thread query GunLogic VM, cached per world, rather than
sharing the integrated server's VM. The server retains its existing shooter VM.
Both load the same resolved dataScripts from the production registry. The existing
instruction budget is now VM-local through Lua extra space, so concurrent
client-query/server script loading cannot race a global counter; each query also
resets its budget for functional __index lookups. ScriptApi bolt
elapsed fields exist only in C++; getBoltTime is deliberately absent from Lua's
registered API in 0.4.8. No attachment/shoot scripting was added.

## Installed-pack scan — 2026-10-29

Read-only production resolver/query scan:

```text
xmake run tacz-pack-resolve <installed gunpacks> <scratch report.json>
```

The generic `manual_bolt` report uses the same registry precedence, runtime module
query and resolved animation files as production. Scratch output:
`C:\Users\MRUIAW\AppData\Local\Temp\opencode\tacz-0.4.8-bolt-scan.json`.
Five packs, 35 manual-action guns: **20 eligible, 15 staged**, no script-load
warnings. Eligibility means default bolt mechanics, not complete support for
other unimplemented mechanics. Names below are scan results, never code/tests.

**Eligible default bolt (20):**

- `ccrp:aics_m700`, `ccrp:mcx_spear_tombstone`, `ccrp:mk13_mod5`.
- `classicr:m24_renewed`, `classicr:mrad`, `classicr:mrad_elr`, `classicr:msr`.
- `cyber_armorer:grad`, `cyber_armorer:grad_05`, `cyber_armorer:grad_borzaya`,
  `cyber_armorer:grad_overwatch`, `cyber_armorer:grad_sparky` (reload script only).
- `sfms:m109`, `sfms:m1887`, `sfms:m200`.
- `tacz:ai_awp`, `tacz:m107`, `tacz:m700`, `tacz:m95`.
- `twx:twx_kcb`.

**Staged script bolt (15), still unbolted in 0.4.8:**

- `ccrp:lastwar`, `ccrp:m1887_long`, `ccrp:marlin_1895`, `ccrp:springfield1873_tube_mag`.
- `classicr:kar98`.
- `cyber_armorer:carnage`, `cyber_armorer:carnage_guts`, `cyber_armorer:m2038`,
  `cyber_armorer:m2038_bloody_maria`, `cyber_armorer:m2038_the_headsman`.
- `sfms:super90m2`, `sfms:tb23`.
- `tacz:kar98`, `tacz:m870`, `tacz:spas_12`.

18 eligible guns have a bolt clip. The two without a clip also have zero action:
`tacz:m107`, `sfms:m109`. All 35 use top-level bolt_feed_time=-1. The only installed
inventory-fed manual gun is `sfms:tb23`, which is staged and also awaits later
inventory-direct shooting. Thus default inventory-fed behavior is synthetic-tested,
not claimed live-tested against an installed eligible gun.

## Accepted 0.4.7, retained differences and live checklist

User acceptance: auto cadence, held semi, burst, reload→fire, draw→fire, refused
reload recovery, ADS sensitivity/sprint, death/respawn all passed. This also closes
0.4.6's respawn full-sync test. Immediate fire→R waiting matches Java; magazine
reload remains uninterrupted by fire. M870 was untestable without scripted bolt
(still staged here). Accepted [lock] diagnostics/counters are removed; lock logic
and the tick-limited explicit reload-cancel side effects remain.

Retained approved differences: translated shoot timestamp/offset estimator,
pending-burst shoot/reload restriction, pre-native ServerLevelTickEvent publication
phase, StartTracking full sync deferred to stage 8, player-only fixed key IDs and
revision instead of Java registration handshake. New temporary staging difference
is explicit above. No refusal ack, token, extra death cancel or server fire-select
bolt guard; zero/short operations can remain unseen and use Java's 250 ms wait.

Acceptance (use properties, examples from the scan):

1. Manual-action, default logic, nonzero time and clip (e.g. `tacz:ai_awp`,
   `tacz:m700`, `classicr:msr`): fire, release trigger, automatic bolt still runs;
   magazine loses exactly one on chambering. Repeat; held semi does not fire twice.
2. Early feed / feed > action where supplied by a test pack: no early shot before
   action; late feed remains active until feed. No repeated transfer each tick.
3. Zero action/default feed without a clip (`tacz:m107`, `sfms:m109`): chamber and
   finish without invented animation or permanent lock; a brief 250 ms unseen-state
   wait is permitted by Java. No-ammo state must not loop requests.
4. Switch before/after feed, including same-ID stacks in different slots: old bolt
   stops; no ammo transferred into the new gun; a fed old round stays in its item.
5. Active bolt → reload/inspect/fire-select/shoot: client lock prevents presentation
   overlap, server shoot/reload refuse until completion. ADS/sprint stay as Java;
   no new bolt gate for those actions. Test death/respawn, dimensions, rejoin/menu.
6. Inventory-fed default gun if supplied: survival consumes one inventory round,
   creative chambers virtually without stock. None eligible is installed today.
7. Script-hook gun (e.g. `tacz:m870`, `tacz:kar98`, `cyber_armorer:carnage`): no fake
   bolt sound/animation/request/lock; intentionally still cannot cycle in 0.4.8.
8. Verify [bolt] start/chambered/finished edges; normal play has no repeated refusal
   flood and no [lock] session line. Default reload/firing/ADS/sprint remain normal.

## Offline validation and changed files

- Requested server and client x64 Release configurations: **build ok**; client
  configuration left active. `xmake build tacz-tests`: **build ok**;
  `xmake run tacz-tests`: **284 cases / 924,841 assertions, Status: SUCCESS**
  (14 new cases, net 629 additional assertions). Existing hook/cast/u8path warnings
  remain; no new native hooks or runtime thread-equivalence claim.
- Synthetic tests cover default feed/action arithmetic, exact -1/negative/zero/
  late feed, stock/creative consumption, server/client precondition matrices,
  Bolt lock/latch semantics, early-fed action exclusion, runtime hook inheritance/functional lookup
  and independent concurrent query/server VM budgets. Existing tests check the
  literal INPUT_BOLT='blot'; snapshot tests now require all eight keys.
- Read-only production resolver scan: five installed packs, 20 eligible / 15 staged,
  no script-load warnings. No game launch, deployment or push. Live acceptance is
  pending. UTF-8 without BOM and whitespace checks passed.

Changed files:

- Rules/API/VM: `src/gun/BoltRules.h`, `src/gun/ClientStateLock.h`,
  `src/gun/SyncedShooterData.h`, `src/lua/ScriptApi.h`, `src/lua/GunLogic.h`,
  `src/lua/GunLogic.cpp`.
- Adapters/transport: `src-server/server/Shooter.cpp`, `src/net/GunPackets.h`,
  `src-client/client/gameplay/ClientShooter.cpp`,
  `src-client/client/animation/GunAnimation.h`, `src-client/client/animation/GunAnimation.cpp`.
- Tests/tool: `tests/BoltTests.cpp`, `tests/SyncedShooterDataTests.cpp`,
  `tests/ShooterCooldownsTests.cpp`, `tests/ItemTooltipTests.cpp`, `scripts/pack-resolve.cpp`.
- Release/docs: `xmake.lua`, `tooth.json`, `packs/behavior_packs/tacz_bp/manifest.json`,
  `packs/resource_packs/tacz_rp/manifest.json`, `CHANGELOG.md`, `docs/ROADMAP.md`, this document.

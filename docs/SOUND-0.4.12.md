# Local sound concurrency — 0.4.12

**Status:** production alias path implemented; control/A/B live gate accepted by
the user. Full 0.4.12 regression acceptance remains pending. No push, deployment
or game launch by this finishing task.

`J/` below means `_reference/TACZ/src/main/java/com/tacz/guns/`.

## Java mechanism and scope

- `J/client/sound/SoundPlayManager.java:58–75`: check resource, enforce positive
  concurrency limit **before** play, then track the new instance.
- `:243–281`: key `(entityId, logical soundId)`, prune inactive instances, compute
  `active - limit + 1`, stop that many **oldest active** voices in play order.
  Category is not part of the key.
- `J/config/client/SoundConfig.java:15–21`: defaults Hit **1**, Default **2**,
  HighFrequency **4**; configurable zero means unlimited in Java.
- `SoundPlayManager.java:222–236,288–329`: every five END client ticks prune
  inactive instances and stop missing/removed/dead/different-UUID owners. No
  level means stop and clear all tracked voices.

This release covers the local player's existing sound sites only, without gun,
pack or namespace exceptions. Remote/3P audio and sound configuration UI remain
outside scope. Playback keeps its existing non-positional native arguments.

## Live control / A / B evidence

User-run hi2 client logs, 2026-10-30, retained under
`C:\Users\MRUIAW\AppData\Local\Temp\opencode\`:

| Route / log | Observation | Decision |
| --- | --- | --- |
| Control, `tacz-0.4.12-sound-overlap-live.log` | New handles; previous same-name voice still alive at about +6–18 ms, dead by about +100 ms; no TACZ stop | Same-name memory route replaces tails; limiter alone cannot fix it |
| A, `tacz-0.4.12-sound-A-live.log` | Register by memory then play by name always returned inactive handles (`name-inactive-fallback-memory`) | Rejected; do not promote fallback audio as successful named playback |
| B, `tacz-0.4.12-sound-B-live.log` | Stable aliases `#0`–`#3`; previous voice alive at +100 ms for Colt Python (~600 ms), AK47 (~100 ms), M16A4 (~70–80 ms); oldest stopped at four active voices | User accepted audible overlap and no pan/fade while moving/turning; production path |

Distinct handles alone were not accepted as proof of audible overlap. B had both
handle evidence and the user's listening result. No latency/wait was inserted
between stopping an old voice and reusing its name.

All experiment switches, variant A and `[sound-overlap]` logging are **removed**.
Normal playback logs no per-shot or first-success messages; missing resources
still log once per cache load. The old diagnostic tests were adapted to pure
playback-state tests, rather than retaining dead experimental code.

## Design as built

`src/sound/SoundConcurrency.h` is pure C++ (no Minecraft/LL). It contains the
named defaults/category table, `SoundLimiter`, `SoundAliasPool`, five-tick cadence
and the composed `SoundPlaybackState`. `src/sound/TemporarySound.h` holds only
Java's one temporary base instance.

Limiter excerpt:

```cpp
std::erase_if(queue, [&](auto handle) { return !playing(handle); });
auto toStop = static_cast<int64_t>(queue.size()) - limit + 1;
// Iterate from the front, stop active handles and erase them until toStop == 0.
```

Playback ordering (simplified excerpt):

```cpp
auto& pool = pools[id]; // logical id only, shared across categories
limiter.beforePlay(key, limit, playing, [&](auto handle) {
    stop(handle);
    pool.release(handle);
});
auto slot = pool.acquire(limit, playing);
auto handle = playAlias(slot);
pool.track(slot, handle);
limiter.track(key, handle);
```

The explicit release makes the FIFO-evicted name immediately reusable even if
native `stop(handle)` has not yet become observable through `isPlayingSound`.
Other inactive slots are found first. `acquire` grows capacity to the highest
positive category limit used for that id, but allocates another slot only when
all existing slots are occupied. A non-overlapping sound uses only `#0`; a Default
only sound uses at most two names, Hit only one, HighFrequency at most four. A
later lower-limit play still evicts according to its lower Java limit, without
changing the historical maximum capacity. Failed zero handles do not occupy slots.

Native wiring in `src-client/client/sound/GunSound.cpp`:

```cpp
engine->playFromMemory(Core::Path(name + "#" + std::to_string(slot)),
    data->data(), data->size(), false,
    1.0f, 16.0f, volume, pitch, false, false);
```

`name` remains `tacz/<namespace>/<path>`. All aliases receive the same cached byte
pointer, volume and pitch. `is3D=false` and every other memory-play argument are
unchanged. Stable names never include a per-shot or world counter.

### Zero / unlimited

Zero cannot arise from the three production constants (1/2/4); there is no config
UI. The pure limiter accepts zero and does not enforce a cap. The bounded native
adapter **explicitly rejects** zero with `invalid_argument` before changing state:
unlimited overlap would need another validated engine route, not silent capping
at four or unlimited distinct native names. This is not a configurable-unlimited
implementation; adding such configuration requires resolving that adapter first.

### Call-site → category table

`playGunSound` resolves its key through `gunSoundCategory`; direct-id sites pass
their category explicitly. Every existing local call uses the current local
player as owner.

| Local call site | Category / limit | Java citation |
| --- | --- | --- |
| `ClientShooter.cpp` shoot and predicted burst follow-ups | HighFrequency / 4 | SoundPlayManager `:121–127` |
| `ClientShooter.cpp` dry_fire, bolt, reload_empty/tactical, fire_select, inspect/inspect_empty | Default / 2 | `:53–55,129–172` |
| `GunAnimation.cpp` animation keyframes | HighFrequency / 4 | `:83–89`; ObjectAnimationSoundChannel `:46` |
| `GunAnimation.cpp` draw, put_away | Default / 2 | `:163–168` |
| `client/ui/GunHud.cpp` head_hit, flesh_hit | Hit / 1 | `:187–195` |
| `client/ui/GunHud.cpp` kill | Default / 2 | `:197–198` |

The key table also maps silence to HighFrequency and other keys (including melee
and refit) to Default, ready for their future call sites. Head/flesh limits are
per **sound id**, not a shared total across two different sound ids, as Java.

## Cleanup and owner mapping

`SoundCleanupTickHook` hooks `ClientLevel::$_subTick`, calls `origin()`, then the
sound cadence. LL 26.51.9's `ClientLevelTickEvent.cpp:7–9` publishes its event
**before** origin, so that event cannot serve as Java's END phase. Cleanup runs
every five real client-level ticks, never every five rendered frames, including
when no gun is held. It filters to the joined local client's level.

Pure cleanup injects `isPlaying`, `ownerValid` and `stop`. Runtime ownership uses
the local player's runtime ID plus a captured UUID. Valid means the same local
player identity is present and `isAlive() && !isDead()`. Local-player absence
stands in for Java's missing/removed entity; changed runtime ID/UUID prevents
respawn/replacement from inheriting old voices. This scope has no remote owners
to resolve from an entity map. Playback also retires the previous scope on owner,
world or engine replacement. A weak/non-owner engine reference ensures old
numeric handles are never sent to a newly looked-up engine.

`ClientShooter::endWorld` calls `stopAllTracked()` **before**
`worldSession.close()`, using the existing request-leave hook plus event fallback;
join replacement also retires the old session. Disable and sound-cache clear
stop/clear tracking as well. Synchronous leave replaces waiting for Java's next
no-level tick. Alias pools reset with playback scope; stable engine names are reused.

## Java temporary-sound stop-site mapping

`SoundPlayManager.java:50,92–104,143–168` holds only the last base
reload/inspect/bolt/draw/put-away instance. Replacing it does not implicitly stop
its predecessor. Missing base sounds clear the slot. Keyframes/shots never occupy
it; limiter/lifecycle tracking includes them independently.

| Bedrock site | Action | Java counterpart |
| --- | --- | --- |
| `ClientShooter::reload` | Stop one temporary, then replace with base reload | LocalPlayerReload `:106–107` |
| Inspect action in `ClientShooter::frame` | Stop one temporary, then base inspect | LocalPlayerInspect `:51–52` |
| Shoot and predicted burst follow-ups | Stop temporary only when logical id equals display's **inspect**, not inspect_empty | LocalPlayerShoot `:302` |
| `GunAnimation::reveal` | Stop temporary, then draw | AnimateGeoItemRenderer `:372–373` |
| `GunAnimation::held` put-away | Stop temporary, then put_away | Renderer `:382–383`; LocalPlayerDraw `:84–85` |
| Bolt | Replace temporary; no extra stop | SoundPlayManager `:159–160` |
| `GunAnimation::clear` | No extra action stop on session/cache disposal | Explicit draw/put-away own their action stops |
| `ClientShooter::endWorld`, disable; `GunSound::clear` | Stop all tracked voices and clear | SoundPlayManager no-level cleanup `:315–322` |

LocalPlayerDraw's delayed draw block `:73–74` is commented out; renderer draw is
the executable reference. Logical-id filtering preserves Java name equality if
inspect and inspect_empty intentionally share one id. Reload/inspect start does
not cancel unrelated keyframes; FIFO eviction can still stop a keyframe sharing
that same logical id, exactly as Java's common key.

## Memory and LL header findings

Our cache holds one shared immutable Ogg buffer per logical sound id, not one
copy per alias. As before, buffers survive for the process lifetime conservatively;
cache reload retires old buffers rather than freeing potentially streamed bytes.
Native aliases may carry extra compressed or decoded engine data; neither its
representation nor size is established. There are no invented memory estimates.

SDK `SoundEngine.h:205–216` declares memory playback; `:114,167–171` declares
per-handle stop/query. `PlaySoundOptions.h:13–20` has no visible overlap-limit
field or memory-data argument. `SoundEngine.h:227` declares global
`releaseSoundFileHandles()`, but no per-name cache eviction/unregister-memory
API is exposed. Stopping a handle or clearing our pool does **not** prove native
decoder/cache eviction. Opaque SoundSystemBase/FMOD declarations do not establish
cache policy. Broad native release calls are deliberately not added.

## Differences and remaining gaps

- Bounded aliases are the user-approved Bedrock adaptation for Java's distinct
  sound instances. FIFO/category semantics remain Java's; no gun special cases.
- Local-only identity/lifecycle mapping and synchronous leave are described above.
  Unlimited is supported by the pure limiter, not this bounded native adapter.
- Existing gaps remain unchanged: Java's 0.8² shoot volume (ours 0.8), other
  players' `shoot_3p` audio, and silencer selection. No spatial audio change here.
- Live B accepted the native route; full production categories, temporary-stop
  regression, lifecycle and native memory behavior still require the checklist.

## Validation and live checklist

Synthetic coverage: limits 1/2/4/0; prune-before-evict arithmetic; oldest-first;
per-id/per-owner independence; shared categories; lazy aliases/free holes/reused
evicted slots; deferred stops/failed plays; owner cleanup; world/engine/no-level
teardown; five-tick cadence; temporary/name-filter isolation; category table.
The adapted probe test checks supplied handle states, not native audibility.

Final validation: **server Release OK → client Release OK → tests OK**;
**343 test cases / 926,238 assertions passed**. Client Release configuration
remains active. Compiler warnings include LL hook macro unused counters and
the existing GunHud flying-item cast; no build errors remain. Release version
is synchronized across xmake, tooth, both pack manifests and the manifest test.
Modified/new release files validate as UTF-8 without BOM; `git diff --check`
passes. No push, deployment or game launch performed.

- [ ] Colt Python rapid fire overlaps (maximum four same-id voices).
- [ ] AK47 AUTO and M16A4 BURST sound remains correct.
- [ ] Repeated same-id hit sound is limited to one.
- [ ] Reload/inspect/bolt are not cut off by themselves.
- [ ] Keyframes survive reload/inspect start unless Java's same-id limit applies.
- [ ] World leave stops all tracked sounds.
- [ ] Reload log has no false `+1` for open-bolt guns (log text only changed).
- [ ] Compare client memory before/after firing many guns; native alias cost unknown.

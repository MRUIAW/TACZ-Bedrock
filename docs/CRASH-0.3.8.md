# 0.3.7 world-entry crash / 0.3.8 fix

## Evidence captured before rebuilding

Installation: `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina`.

- Newest run log by modification time: `logs/2026-10-24.9.log`. Lines 24-29 confirm
  TACZ 0.3.7 and 54 loaded guns. Line 59 already reports a vanilla-slot reveal during entry;
  line 60 reports player spawn. The crash truncates buffered logs, so absent final animation
  log lines were **not** used to rule out a code path.
- Compared `logs/crash/trace_2026-10-24_18-25-54.log` with
  `logs/crash/trace_2026-10-24_18-28-10.log`. Both have the same exception instruction and
  frames 0-8. This is the same reproducible failure, not two different crashes.
- The original `bin/tacz/tacz.pdb` and deployed PDB matched SHA-256:
  `42307434900032D86FE09EA35AFF2BCF6A4A56897AFBBA7818F6DDBE6E212120`.

Both traces, lines 37,45-48:

```text
Minecraft.Windows.exe: mov dword ptr [0], 0xDEADC0DE
refresh +0x200       GunAnimation.cpp:123
reveal +0x3F7        GunAnimation.cpp:214
held +0x60B          GunAnimation.cpp:248
ShooterFrameHook     ClientShooter.cpp:353
```

CrashLogger prints TACZ function starts plus offsets. `llvm-symbolizer --relative-address`
on the original DLL confirms RVA `0x1aa800` (`refresh` start `0x1aa600` + `0x200`) maps to
`std::make_unique` / `GunAnimation.cpp:123`:

```cpp
session->item = std::make_unique<ItemStack>(player->getSelectedItem());
```

`llvm-objdump` confirms the call at DLL RVA `0x1aa7fa`, returning to `0x1aa800`.
Engine frame RVAs, using the module base recorded in the trace:

```text
0x348eea6  intentional assertion crash
0x2486d33  return from ItemRegistryRef lookup
0x24867fa  ItemStackBase item initialization
0x2481fd5  ItemStackBase copy constructor
0x24823f1  ItemStack copy constructor
```

At engine RVA `0x2486cd1` the code reads the thread-local registry weak reference; at
`0x348ee20` it tries to lock it and asserts when no live registry is obtained. The embedded
assertion strings selected by that exact branch are:

```text
std::shared_ptr<ItemRegistry> ItemRegistryRef::_lockRegistry() const
ItemRegistryRef cannot access ItemRegistry - attempt to lock weak_ptr returned null!
.../world/item/registry/ItemRegistryRef.cpp
```

Thus the null object is the **registry obtained from the current thread-local scope**, not
the LocalPlayer or selected ItemStack. Frame code runs after `MinecraftGame::update` origin
returns, outside its registry scope. On entry the saved/server inventory exposes a gun, so
normal selection observation calls reveal and refresh without any user action. The new 0.3.7
copy constructor immediately needs that missing registry. Even waiting until player loading
alone would not make the outer per-frame registry context valid; removing that dependency
from animation snapshots is the cause-level fix.

The trace already identifies the source line, and the assertion branch is confirmed directly
in the engine image; minidump analysis was not needed to guess what was null. No game file or
crash artifact was modified by the investigation.

## Fix and adjacent lifecycle review

| Path | Change / finding |
| --- | --- |
| `src-client/client/animation/GunAnimation.cpp` | Session owns `GunItemState` values + `CompoundTag::clone()` user data, not an ItemStack or registry reference. Refresh updates these only while the visible selection is active; put-away freezes them. Lua reads the owned NBT, independent of current player/slot. VM destruction before cache/context destruction remains intact. |
| `src-client/client/gameplay/ClientShooter.cpp` | Non-null LocalPlayer was previously treated as a loaded world. Now ClientJoinLevelEvent opens a generation; ClientExitLevelEvent closes it. Frame initialization, action packets and movement capture cannot run pre-join/post-exit. Pending action edges and reload replies reset at transitions. |
| `src/gun/ClientWorldSession.h` | Atomic readiness/generation shared by input/packet/frame adapters. Rejoining invalidates work even if native object addresses are reused. |
| Animation packets / ticks / renderer | Queued debug and movement work and immutable render snapshots carry a generation; old ones are rejected. A renderer cannot show a retained old gun during the interval before the next frame destroys its VM. |
| `src-client/client/sound/GunSound.cpp` | Recorded engine is a validity-aware NonOwnerPointer, with world generation. End-of-world stops animation handles before native exit; later cleanup only drops already-invalid work. No new-client engine lookup for old handles. Sample bytes still live long enough for asynchronous playback. |
| Server draw selection / timestamps | Only optional IDs, slots and integer times are retained; no ItemStack/registry/Player pointer is cached. Existing disconnect/server-stop cleanup remains. Server writes stay in server gameplay paths, not the outer client frame. Java cooldown formula is unchanged. |

The join event is emitted from ClientInstance::onPlayerLoaded and exit is emitted before
ClientInstance::onLevelExit's origin. For the primary client, these establish the lifetime
boundaries rather than relying on menu names, pointer presence or arbitrary delays.

No broad exception handling or null-check workaround was added around ItemStack copying.
Native constructors are absent from the fixed animation path entirely. Existing frame error
handling was not used as a substitute for the ownership fix.

## Validation and retest

- Server and client release builds passed; 88 cases / 21,816 assertions passed.
- New unit tests cover pre-load, exit, repeated exit, leave/rejoin and replacement-world token
  rejection. Native ItemRegistry asserts, engine weak pointers and event ordering still need
  game testing; unit tests do not provide a Minecraft process.
- Version is 0.3.8 in `xmake.lua` and `tooth.json`. 0.3.7's changelog entry is unchanged;
  no commit was made.
- `scripts/deploy-client.sh` successfully built and deployed 0.3.8 to the installation above
  with Minecraft closed. Startup/gameplay have not been tested by the agent.

First enter the affected world with the saved gun selected and wait at least 10 seconds;
repeat with an empty slot selected. Confirm log startup reports **0.3.8**, and that drawing
the gun works, not merely that the crash disappeared because animation is inactive.
The new `[world] local player loaded: generation ...` line should precede `[hand]` / `[anim]`
selection work. Exit should report `[world] exit: queued work invalidated`.

Then run all prior checks: walk and sprint 5 seconds each (save `[walk-rate]`); gun -> empty
hand, gun -> item, gun -> gun; fast scrolling and back to the old gun; fire/R/H during the
handoff. Finally quit **mid put-away**, enter again, and repeat entry/exit several times.
There must be no stale gun, action lock, queued sound or unexpectedly continuing reload.
Also re-test with a remote updated server when available. The walk-distance measurement
limitations and fixed +50 ms handoff divergence documented for 0.3.7 remain unchanged.

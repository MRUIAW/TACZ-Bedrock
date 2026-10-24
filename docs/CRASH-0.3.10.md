# 0.3.9 main-menu crash / 0.3.10 fix

The crash is a new diagnostic-hook ABI regression, not a failed pink-texture reproduction.
0.3.9 never reached its in-world test checklist. Its changelog entry is kept unchanged as a
historical record; the native trace instructions in `RENDER-0.3.9.md` are superseded here.

## Trace, minidump and binary evidence

Picked artifacts by modification time in
`F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/logs`:

- Latest run `2026-10-24.20.log`: TACZ 0.3.9 loads/enables; the user reports the main-menu
  crash. Buffered log truncation was not used to infer that a late code path never ran.
- Compared all three new traces and matching minidumps:
  `2026-10-24_19-46-23`, `2026-10-24_19-47-21`, `2026-10-24_19-49-48`.
  Each has C++ exception code `0xE06D7363` and the same relevant frame chain:

```text
Minecraft.Windows.exe                 RVA 0x01202F8 (throw)
Minecraft.Windows.exe                 RVA 0x4B04B74
GunMeshTraceHook::detour +0xFB         old GunRenderer.cpp:505, origin(...)
Minecraft.Windows.exe                 RVA 0x4B05000
... native render frame ...
ShooterFrameHook::detour +0x3E         old ClientShooter.cpp:372, origin()
```

The shooter frame is still in the game's original update/render, before our post-origin
world checks. It does not mean our gun animation or GPU-ownership code ran at the menu.
Other mods also wrap the frame in these stacks; none is substituted for the proven failing
TACZ forwarding frame. The same failure occurred in runs with different other mods loaded.

The original local/deployed 0.3.9 PDBs matched SHA-256:
`C7B4A8287A18DEC5208B6CC030DC9A1442B72896853E79D214A0E60D40AD14DE`.
`llvm-symbolizer --relative-address` on the original DLL maps RVA `0x1B994B` to
GunMeshTraceHook::origin/detour at old line 505. (CrashLogger prints the function start
`0x1B9850` plus `0xFB`, rather than the full return address.)

Decoded each minidump's MSVC ThrowInfo -> CatchableTypeArray -> TypeDescriptor using the
recorded exception/image addresses. All three identify:

```text
.?AVbad_variant_access@std@@
.?AVexception@std@@
```

The SDK at `levilamina/26.51.7/.../include` for the client target says:

- `mc/deps/minecraft_renderer/renderer/Mesh.h:98-109`: `_renderMesh` takes a
  **by-value** `brstd::static_vector<TextureVariant, 8>`.
- `mc/deps/core/debug/brstd/static_vector.h:7-8`: `class static_vector {};`.
  This is an empty one-byte placeholder with no native storage/copy/move ABI implementation.

Native image disassembly confirms the difference independently of the declaration:

- Engine `renderMesh` wrapper at RVA `0x4B04F70` constructs the real container in a large
  stack frame; at `0x4B04FEE` it places the container address in R9, then calls `_renderMesh`
  at `0x4B04470`, returning at `0x4B05000` (the trace frame above).
- TACZ's old detour at `0x1B9850` never preserves that incoming texture-container argument.
  Before calling origin, `0x1B9923` uses R9 for a local optional-metadata address instead;
  the empty placeholder produces no real container forwarding. The native callee interprets
  wrong data as its container/variants and throws bad_variant_access.

Thus the invalid object is the **ABI-corrupted forwarded texture container/variant data**,
not an absent LocalPlayer. `_renderMesh` is generic rendering and also runs for menu meshes.
Installing a global hook at mod enable intercepts those even with no gun/world. A conditional
around diagnostic counter updates never bypassed the unsafe forwarding. Adding a world check
inside that hook, or enabling it only after join, would leave the by-value ABI wrong.

## Cause-level fix

Remove GunMeshTraceHook and all of its hook lifecycle. Do not guess a native container size,
reinterpret its layout, swallow the engine exception, or change graphics modes. Only observe
the fully typed `Mesh::renderMesh(..., TextureVariant const&, ...)` invocation in our own gun
submission. Menu meshes no longer traverse any TACZ low-level mesh detour.

`[gun-render]` still reports hand-hook calls, CPU builds, draw-call returns, animation/pose/mesh
versions, validity failures, cache keys and RTX/deferred state. It now explicitly reports
`trace=call-site-only`; native `_renderMesh` counts and metadata IDs are **not** available.
These counters do not prove GPU/BLAS consumption. `[anim-frame]` is still independent of the
hand hook; `pipelinePresent` distinguishes no FrameBuilder from a real non-RTX pipeline.

## Review of every remaining client hook / 0.3.9 menu path

Checked the actual installed client headers (not a server header or a remembered signature):

| Hook | Header / exact signature | Review |
| --- | --- | --- |
| ShooterFrameHook | `mc/client/game/MinecraftGame.h:1803`: `void $update()` | Always forwards origin, including menus. Extensions stop on a world-session token before global client queries; menu cleanup only destroys owned data. |
| LeaveWorldHook | `mc/client/game/ClientInstance.h:1363`: `void _requestLeaveGameImpl(bool const, bool const)` | Two complete scalar values, no opaque class by value. `endWorld` requires a live matching recorded session, otherwise it just forwards native origin. Top-level const on by-value bool is not part of the function type. |
| FirstPersonGunHook | `mc/client/renderer/game/ItemInHandRenderer.h:280-281`: `void renderFirstPerson(BaseActorRenderContext&, Matrix const&, ItemContextFlags)` | Context/matrix by reference; `ItemContextFlags.h:5-16` fully defines the uchar enum. No world/valid current gun snapshot means immediate pass-through before accessing client/player/resource services. |

Compile-time member-function signature checks now cover all three. This checks symbol/type
drift, but **cannot** establish the ABI of an opaque placeholder like the removed container.
No remaining custom client hook forwards an opaque container by value.

Additional lifetime review/fixes:

- The heartbeat now exits on session zero before any renderer-global lookup. It rejects old
  snapshots by generation, not by whether a global client pointer happens to be non-null.
- `frame`, `heldGun`, and gameplay checks consult the session first, then verify recorded
  client identity. That identity is atomic because input callbacks can also read it.
- In-world pipeline transitions may temporarily lack TextureGroup/buffer service/FrameBuilder.
  `RenderResourceKey::ready()` defers GPU work rather than building a poisoned cache entry.
  This runs only after a valid session/snapshot check; it is not a null-check workaround for
  the diagnostic ABI fault. A one-time deferred-resource line identifies that state.
- Exit closes the session before releasing cached GPU owners. Release must therefore use its
  already-owned nonzero-generation cache/original weak TextureGroup, not insist on a currently
  open world or query a new global client. No-cache menu cleanup invokes no TextureGroup API.
- The prior pink-texture fix (release/recreate across world/pipeline ownership) is retained;
  no GUI-wide or RTX-specific alternate rendering path was added.

## Validation / user checks

All 92 pure tests / 21,839 assertions pass, including menu/stale-snapshot and partial-pipeline
gates. They do not emulate native detours or GPU resources. Native exception decoding and
disassembly establish this root cause; runtime main-menu and graphics-mode tests still matter.
Version is 0.3.10; no commit is requested or made.
Both release variants built successfully; `scripts/deploy-client.sh` built and deployed the
client with Minecraft closed. The agent has not launched the game or verified startup/gameplay.

1. Start the game, confirm **0.3.10**, wait at the main menu for at least **10 seconds**.
   No in-world `[gun-gpu]` allocation or gun-render diagnostics should run there.
2. Enter with a saved gun. Quit mid-put-away and rejoin; repeat quitting while simply holding.
   Expect `[gun-gpu] release` at exit and a fresh bind/create on rejoin, without magenta.
3. Switch A -> B -> A (two world switches), checking texture recreation and no stale lock/sound.
4. RTX on: idle, walk, shoot, reload, inspect for a few seconds each. Read `[anim-frame]` and
   `[gun-render] ... trace=call-site-only`; check increasing pose/build/draw versions. RTX
   animation is still an investigation, not a claimed fix.
5. RTX off: repeat all actions plus gun -> hand/item/gun and fast scroll regression tests.
6. Return to the menu and wait another 10 seconds; diagnostics/resources from the old world
   must stop. Pipeline changes in-world may log `GPU work deferred` until services are ready.

## User validation after deployment

The user reports all crash and pink-texture checks above passed, including ordinary holding
exit, mid-put-away exit and A -> B -> A. RTX-off actions/switching/fast scrolling also passed.
RTX-on still renders a static gun with working audio. The subsequent `.22.log` investigation
is in `RTX-0.3.10.md`; this is not an RTX fix claim.

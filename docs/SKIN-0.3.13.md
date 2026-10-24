# 0.3.13 investigation — blocked, not released

Historical skin investigation filename: the overnight plan assigns 0.3.13 to ADS and defers
the pixel-copy skin fix to 0.3.17. The 0.3.12 observer plus diagnostic-only logging is approved
for the baseline commit; it does not fix Steve/Alex fallback. The user has been given issue
text for an LL mcapi-requests request for `ClientSkin` / `ClientSkinSystem` members. This is
not evidence that the issue has been filed or that new API definitions are available.

Status: no safe, complete skin fix established. Following the user's explicit stop condition,
do not deploy a speculative change or label diagnostics as a working 0.3.13. `xmake.lua` and
`tooth.json` remain 0.3.12. All changes remain uncommitted; HEAD is still `ce8c25b`.

## 0.3.12 runtime evidence

User reported default Steve arms again; no crash reported, other test results not supplied.
Newest nonempty run is
`F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/logs/2026-10-25.4.log`.
It initially appeared empty while the process was writing; subsequent reads contain the run.
Preceding `.3.log` loaded 0.3.11 (lines 21–22), so it is not a test of the 0.3.12 observer.
Newest crash artifacts remain `logs/crash/trace_2026-10-24_19-53-08.log` and matching dump;
no new 0.3.12 crash artifact found. That does not certify all lifecycle tests passed.

Concrete `.4.log` evidence (line numbers from the file-content search):

| Lines | Observation |
| --- | --- |
| 22–23 | 02:01:31.364: loads 0.3.12 and installs observer during **load**, not enable. |
| 41 | 02:02:20.211: mod enable, roughly 49 seconds after installation. |
| 71 | 02:02:42.956: local player loaded, world generation 3. |
| 104 | First arm selection 02:02:43.814: revision=3, observedUpdates=3, ref/owner=true, default / renderer-texture-expired; input 64x64, format=4, bytes=16384. |
| 165, 169 | Revision=12 matched; GPU 64x64, **geometry empty**; default / unverified-renderer-arm-layout. |
| 210 | Revision=12 texture weak ref later expired while total updates=15. |
| 215 | Revision=23 matched; input changed to 256x256, format=4, bytes=262144; expired snapshot. This alone does not identify Persona. |
| 221, 246, 290, 317 | Exits/joins; generations 3 -> 5 -> 7. These establish transitions, not user acceptance of every lifecycle case. |
| 513 | Revision=37 matched, input/GPU 64x64, empty geometry; still default. |
| 714, 740 | Matched revisions=45 / 53, total updates=45 / 53, 64x64 GPU, empty geometry, default. |
| 757 | 02:06:25.970: mod disable begins. |

Across the run: **15** arm-skin selection records, all `source=default`; **7**
`renderer-texture-expired`, **8** `unverified-renderer-arm-layout`; **6** default texture
creation records, **0** `[arm-gpu] bind ... source=renderer-handle` records. Native update
count is **at least 53** by the last selection (not an exact final total: no disable counter
existed). Matched revisions are 3, 12, 23, 25, 28, 37, 45, 53; these are event numbers,
NOT eight proven unique skin identities. Every recorded updateThread equals render Windows
thread ID 267136. Owner thread hashes are diagnostic thread markers, NOT skin identities.

Code proof: `nextRevision` increments only after the hook's origin returns. The nonzero
matched revision is assigned only after shared control-block comparison of local
`LocalPlayer::mSkin->mSkinImpl` with the observed input OR stored `mSkinImpl` succeeds.
Therefore the hook fired and identity matching succeeded on all recorded selections. The
logger ran before submission, but the selection code chose/uploaded/reused its default
texture and never entered the borrowed-handle branch. No successful custom-skin bind is
hidden behind a misleading log label.

The hook did not log individual calls or identity addresses. We cannot recover exact
menu/join/change call counts, timestamps, how many distinct owners existed, input-vs-stored
match, or whether each expired weak ref was empty already at update versus expired later.
The first three updates happened between installation and first draw; their relation to
menu/join cannot be narrowed further. Later counters rise while world 3/7 is active, but
without per-event logs it is not possible to assign all increments to a specific UI action.

## Cause and rejected shortcuts

1. Treating `mDefaultMeshName` as the actual skin geometry was incorrect. It is empty for
   both 64x64 and 256x256 update snapshots in this run; valid GPU 64x64 snapshots therefore
   fail the whitelist. Blank default-mesh metadata cannot establish classic or slim UVs.
2. One update-time weak texture pointer is not a robust source of the vanilla renderer's
   current binding. `updateSkin` does not prove that it is the final upload/composition
   boundary; native data may be empty, replaced or destroyed. Exact replacement path is not
   yet established. Merely changing weak_ptr to shared_ptr could pin obsolete GPU resources
   and still render the wrong texture; it is not a justified fix.
3. This run is **not** a hook-never-fired, no-identity-match or simply-enable-too-late case.
   Processing before mod load is still a general limitation of event-only observation, but
   does not explain the demonstrated empty-layout rejection after a successful match.
4. SDK `mce::ImageFormat` value 4 means RGBA8Unorm. 64x64 x4 equals the observed 16384 bytes;
   256x256 x4 equals 262144. The logged data is native update input metadata, not a pixel
   dump; neither fallback reason is a CPU image format/empty-buffer rejection. Do not
   reinterpret the old debug thread markers as permission to dereference guarded mObject.

## Safe API investigation, in requested preference order

### 1. Current renderer texture at draw time

- `ActorRenderData.h:27–38` exposes actor/animation variables and metadata, no skin handle.
  `BaseActorRenderContext.h` provides context/dispatchers, but no current player texture.
- `ActorRenderDispatcher::_getRenderer(Actor&) const` safely gets a renderer, not the local
  player's skin. `DataDrivenRenderer::mDefaultSkin` is a shared default fallback, NOT proof
  of a per-player dynamic texture. Substituting it repeats the default-skin bug.
- `ClientSkin.h` and `ClientSkinSystem.h` are empty SDK placeholders. Reading hypothetical
  ECS skin fields via guessed offsets is not a safe API.
- `RenderParams` / `ClientAnimationComponent` expose model/render-controller references,
  but `ActorResourceDefinition` is empty and `RenderControllerToProcess` is untyped storage.
  No typed current per-player texture lookup is available through those declarations.
- V2 `SimpleTextureData` / `RenderControllerResources` expose typed baked texture resources,
  but the per-actor selection/remapping path is incomplete (`PoolSlotMap` empty, plus dynamic
  skin component missing). Shared baked material textures cannot be assumed to be this
  player's currently bound dynamic skin. No pool-index/offset guessing added.

### 2. Query current skin/repository on join, including pre-hook processing

- `IClientInstance::getSkinRepository() const` returns a shared repository, but
  `SkinRepository` / `ISkinRepository` / `Skin` lack the required typed query/data methods.
  Having the repository pointer does not reveal its native map layout.
- Paper-doll APIs return serialized skin info or `SkinImages` and depend on UI contexts;
  they are not a verified world-player GPU texture getter. Do not create fake UI contexts
  or call opaque-by-value types. No safe fallback query was established.

### 3. Native first-person arm drawing under TACZ matrices

- `ItemInHandRenderer::_renderMiniMapHand(BaseActorRenderContext&, Player&, bool)` has a
  non-placeholder argument signature, but is explicitly the **map hand** path. Headers do
  not establish its extra transforms, visibility/side effects, or whether it is a neutral
  one-arm renderer. Calling it under TACZ's attachment matrix without verifying that contract
  would be another speculative visual/pose change.
- `renderFirstPerson` is the whole held-item/hand pass, not an isolated neutral arm. Calling
  it again risks duplicate items/vanilla motion; suppressing/capturing global mesh draws to
  force this would expand scope and cannot use the opaque `_renderMesh` TextureList ABI.
- There is no typed `PlayerRenderer::renderLeftHand/renderRightHand` equivalent exposed in
  these client headers. No new unverified native function declaration or offset is added.

These findings establish a limitation of the **available verified declarations**, not proof
that the game has no such internal path. The existing update-hook signatures remain checked
at compile time. No extra native hook or guessed ABI was added in this investigation.

## Diagnostic-only draft (not a skin fix; not deployed)

- `[arm-skin-update]`: first 128 actual native events, event number/timestamp, atomic
  worldToken (zero = no authorized world, not necessarily main menu), OS thread, info/input/
  stored identity snapshot addresses, shared-owner relation, native validity, texture data
  and resource block presence, load/missing state, GPU size and default-mesh string. Total
  event count is logged at disable even if per-event logging reaches its cap.
- `[arm-skin-match]`: local wrapper identity versus recorded input/stored wrappers, actual
  shared-control-block match side, update-time data/block versus current weak-lock result.
  Addresses are snapshots for correlation only, never retained pointers to dereference later;
  event numbers and control-block comparison avoid treating address reuse as proof of identity.
- `[arm-skin-draw]`: once after a successful arm draw return per selection, actual default/
  borrowed source, texture variant, owned-default flag/fallback serial and borrowed block.
  A draw return is submission evidence, not proof of backend GPU output.
- No selection/layout/lifetime policy or pure model logic changed. No new pure unit-test case
  is needed for log formatting; existing tests still run. No menu client/world service lookup:
  `shooter::worldToken()` reads only `ClientWorldSession`'s atomic state.
- Diagnostic draft: client release builds pass (initial 13.750 s, final 29.187 s after the
  server build); server release build passes (27.063 s). Existing hook macro unused-variable
  warnings only. All **98 tests / 42,627 assertions pass**, no skipped tests. No pure logic
  changed in this investigation, so no additional pure-policy test was added.
- No deployment attempted: this is not a verified fix. Installed artifacts remain the prior
  0.3.12; local `bin/tacz` is a client diagnostic build, not the previously deployed binary.
  No 0.3.13 version bump or commit.

## Options before implementing a real 0.3.13

1. Read-only symbol/debugger/GPU inspection of the actual local-player draw to establish
   `ClientSkin` fields or its typed texture-getter boundary, current resource ownership,
   selected geometry/arm model, and replacement behavior. Add/use a verified SDK definition
   rather than guessed offsets. Prefer a draw-time getter over the current weak event snapshot.
2. Alternatively verify `_renderMiniMapHand`'s implementation/callees and isolate an actual
   neutral native arm draw contract, including stack restoration and avoidance of map/held
   item side effects. A safe signature alone does not establish correct TACZ pose semantics.
3. If further runtime evidence is needed first, explicitly test the diagnostic-only draft;
   it will still show defaults. Do not advertise it as a skin fix. Logs can then answer
   when each update happened and whether data was absent at update or expired afterward.

Acceptance for a future verified fix: classic wide 64/128, slim 3-wide, Persona only with
actual geometry/UV evidence, in-world change if supported (>1 s), A -> B -> A, quitting while
holding/mid-put-away, and >=10 seconds at main menu. Check actual bind/draw source, stale/pink
textures and release before teardown; no crash alone is not a skin-support result. RTX remains
parked. These are pending steps, not claimed 0.3.12 user test results.

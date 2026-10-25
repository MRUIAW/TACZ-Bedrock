# 0.3.18 — immutable gun meshes + draw-time bone matrices

Parent `88c21f3` (0.3.17). User-approved route 1. No new agent-run Minecraft/GPU test:
the implementation/equivalence evidence below is not a claim that jitter disappeared,
RTX animated or the new native draw-call cost has already been measured in game.

## Accepted baseline and scope

User confirmed 0.3.17 Camera Shake option gating and sprint suppression/resume in all cases.
Keep those paths unchanged. User previously confirmed matrix-only arms animate under RTX
without the per-face jitter, and whole-gun ADS matrices reach RTX; the dynamically rebuilt
gun stays internally static. Moving gun = jitter/flicker; freeze = neither, including the
two formerly suspected z-fighting regions. See `RTX-0.3.10.md` and the 0.3.17 addendum in
`CAMERA-RENDER-0.3.16.md`. No source cubes are removed/nudged or GPU vertex format guessed.

This changes the gun render route, shared bone evaluation and visibility-state plumbing.
It does not implement native player-arm rendering, skin/Persona fixes, HUD, shell/muzzle
renderers, attachments, a new camera/recoil path or the View Bobbing aimed-walk sway fix.

## Granularity and exact transform

`model/RigidGunModel` compiles immutable vertex groups and a parent-before-child traversal.
Every **named bone remains a separate transform/visibility boundary**, even if none of the
current clips animates it. Its unnamed, static rotated-cube helpers are folded into that
bone's local frame. There is no sampling-based claim that a named bone will never animate,
nor a duplicate animation-file load/scan in the renderer. This conservative rule works for
all existing and future **named-bone** pose channels, blends and transition poses.

Do not fold across `attachment_adapter` child visibility or different illuminated flags.
Groups are **contiguous DFS draw runs**: if another group's faces intervene, start a new
run rather than reorder coplanar faces. UVs, quad order and winding are unchanged. An
explicit empty-name pose channel is incompatible with static anonymous helpers: log once
and use retained path 0 for that pose, rather than silently freeze that channel.

For a group anchor a and static helper relative transform H, source vertex q:

```
old: v_model = (B_a(pose) * H) * vec4(q/16, 1)
new cached local: v_local = H * vec4(q/16, 1)       // once
new draw: P_native * V * W * T_common * B_a(pose) * v_local
```

`B_a` uses exactly `partTransform`: pivot/16 and animated translation signs, rest Rz Ry Rx,
then additional Rz Ry Rx, then scale, with the existing camera/constraint exceptions.
Cache rest-local matrices; on pose/visibility revision change evaluate **one BoneFrame**.
The same frame is used by the gun and both arms. Arms append the existing Rz(180) to their
functional node's world matrix and use ancestor visibility (not the functional node's
placeholder-cube visibility). No separate arm pose evaluation, hand offset or independent
sway was introduced. Common ADS/constraint/camera/zoom_model_fov and frozen common matrix
remain outside the bone draw loop, unchanged. Put-away continues using the retained gun's
snapshot, not the selected inventory item's resources or state.

Floating-point reassociation has bounded error, not bitwise-identical positions. All-zero
bone basis (authored scale-zero hide) skips its zero-area draw. Negative/nonuniform and
planar scales remain supported. Non-finite bone matrices are not submitted. Normal inverse
transpose is well-defined for nonsingular transforms; shader handling of singular planar
transforms is a runtime lighting risk, not an asserted exact normal contract.

## Counts and equivalence evidence

Real default AK47, release test, all 35 merged gun/default clips, 33 time samples each,
each also composed onto static_idle. Visibility samples vary chamber/ammo, all four mag
levels and additional magazine; grouping vertices are checked immutable after all poses.

| Offline measurement | Result |
| --- | ---: |
| Named source bones / parsed bones | 94 / 605 |
| Parsed bones owning faces before folding | 549 |
| Immutable groups (including currently hidden branches) | **43** |
| Local vertices in all resident groups | 11,736 |
| Maximum absolute local position | **1.29703 blocks** |
| Rest/default-visible groups | **29** |
| Drawable gun groups over tested poses/visibility states | **25–29** |
| Poses / vertices compared with legacy bake | **2,310 / 16,574,320** |
| Maximum position-component error | **4.76837e-7 blocks**, required <2e-6 |
| Maximum nonsingular normal-component error | **2.38419e-7**, required <2e-5 |
| UVs and traversal order | exact |
| Both arm attachment matrices | exact (max component difference 0) |

Thus typical rest is **29 gun submissions**, plus up to four cached arm/layer submissions,
not 549 or 605 gun calls. Counts above come from the exact pure draw-selection code, **not
an agent-run in-game native/GPU counter**. Actual `[gun-render] lastGunCalls`/per-frame
means must confirm it. No speculative cross-named-bone grouping is needed initially;
profile native cost before adding more complex rigid-group analysis.

There were 63,256 singular-normal vertex comparisons in scale-zero animation hides.
**All their positions and UVs were still checked**. They are excluded only from the
inverse-transpose normal comparison: legacy bake's fallback original normal depends on
the old helper frame when the transform is singular. The initial test correctly exposed
that distinction, rather than a position/composition failure. Synthetic tests separately
cover nonuniform/negative/zero/planar scales, named bone animation, hidden ancestry,
attachment-adapter helpers, additional-magazine visibility and immutable local hashes.

## GPU submission and A/B route

- Default **`/tacz rdebug path 1`**: upload each group once per visual/resource generation,
  draw by `W*T_common*B_bone`, no pose-baked vertex stream or per-pose native Mesh emplace.
- **`/tacz rdebug path 0`**: retained CPU animated bake and buffered reconstruction; same
  common transform, pose, visibility and shared arms, one gun draw. This is the A/B control,
  not an automatic RTX fix. New path became default only after equivalence tests passed.
- Commands are process-local/integrated-client diagnostics, like existing rdebug; a remote
  BDS command does not mutate a separate client DLL's atomic setting.
- Every GPU group is heap-owned at a **stable address**, with one native Mesh construction
  per winding variant. Arrays of unique owners may move; Mesh objects do not. Single- and
  double-sided debug variants are cached separately and built lazily, not recreated on a
  toggle back. Both variants are destroyed with their original resource scope.
- Reuse the user-validated arm-style world stack push/dirty flag/renderMesh/pop sequence.
  Both inner and common stacks pop on exceptions. No new native hook was added. Native
  Mesh::renderMesh overload, Tessellator::vertex and MatrixStack::push signatures are
  compile-time checked against installed client headers. Tessellator's established begin/
  Buffered end call is reused; no global opaque TextureList/skinning metadata hook.
- Mesh validity is checked before submission. Invalid handles are skipped/warned, never
  drawn through stale services. No unsupported buffer/BLAS invalidation or cache-ID trick.

This should address the suspected per-pose upload quantization: local vertex bits no longer
change as the gun animates, only rigid draw matrices do. It should address the observed RTX
stale internal geometry path for the same reason the cached arms work. **Neither outcome
is yet a verified 0.3.18 image/RTX result.** Actual packing remains unknown, native backend
classification/caching and shader lighting still require testing. No universal cure for
source coplanarity, reconstruction/TAA or unrelated camera jitter is claimed.

## Visibility, cache and lifetime

Publish actual barrel/ammo/mag-extension and keyed additional-magazine state in the same
immutable animation snapshot/revision. Freeze retains both pose and visibility. Old and
new routes use the **same** GunRenderState predicate. Previously the gun adapter always
passed its default state and always hid additional_magazine; wiring the captured state and
the channel-presence visibility is needed for the requested skip-draw behavior. It follows
Java ModelAdditionalMagazineListener's keyed visibility without adding a gameplay feature.

Parent hiding propagates. `bullet_in_barrel`, bullet/mag chains, standard/extended mags,
sight/carry, handguard and attachment-slot rules retain named boundaries, so visibility
selects existing draws rather than rebuilding. `_illuminated` flags are not merged with a
different flag; the existing full-bright gun material/shader setup is retained. Actual
attachments and extension item selection remain unimplemented (`magExtent=0` in the live
context); alternate levels are covered by pure tests, not falsely claimed as new gameplay.
Functional hand/shell/muzzle placeholder geometry remains hidden/replaced as before.

Visual cache key is **display ID + model ID + texture ID**, under existing RenderResourceKey
(world, client, texture group, buffer service, FrameBuilder and lighting/pipeline mode).
No native work occurs before the world/snapshot/resource-service guards. World exit and
pipeline/owner replacement release arms and all gun meshes **before** dropping textures;
unload only via the original weak texture-group owner. Each runtime texture name now also
has a unique texture serial: different visuals sharing an asset cannot unload/replace each
other's Raw texture name. Cache does not promise hot-reload of file contents under unchanged
asset IDs; the existing pack-enable/resource lifecycle remains the reload boundary.

## CPU measurements: do not mix scopes

Existing **user-run 0.3.17** log `2026-10-26.1.log` identifies version at lines 21–22:

| Path/evidence | Mean ms | Max ms | Scope |
| --- | ---: | ---: | --- |
| Legacy, RTX off, line 145 | **3.788** | **4.241** | CPU bake+tessellation/upload submission, 120 changing poses |
| Legacy, RTX on, line 1290 | **3.862** | **5.009** | same scope |
| Legacy, RTX off again, line 1455 | **3.792** | **4.584** | same scope |
| Offline old pure bake, latest 1,000 iterations | **.0524581** | not collected | no native upload/draw/GPU |
| Offline new pure BoneFrame, latest 1,000 iterations | **.0138968** | not collected | no native upload/draw/GPU |
| New 0.3.18 in-game native submission | **pending** | **pending** | must collect with A/B below |

Other local runs varied (.035–.052 ms bake, .009–.015 ms matrices). These are small CPU
microbenchmarks, **not** a replacement for the roughly 4 ms native rebuild baseline or an
FPS/GPU benchmark. Actual native per-bone submission overhead can offset some savings.

Diagnostics:

- Once per second `[gun-render]`: path, RTX/deferred flags, completed frames, hook count,
  native mesh build returns, gun/arm call-site draw returns, mean/last gun calls per frame,
  visual cache hits/misses, CPU total mean/max, prepare mean, gun+arm draw mean, revisions,
  invalid mesh count, group count and meshDiagnostics flag. No GPU timings are implied.
- `[gun-perf] path=... N=120`: first 120 steady completed frames after a path/pipeline/cache
  scope change. Cold visual misses excluded; path 1's initial static upload frames excluded.
  Path 0's normal per-pose rebuilds are intentionally **included**. Reports total mean/max,
  geometry prepare mean and gun+arm draw mean. New path includes shared hierarchy/validity
  checks; old path also evaluates the shared frame for its arms. Original `[anim] mesh
  rebuild CPU` remains for the exact old changing-pose bake/upload metric.
- `[gun-rigid-build]`: cold one-time group upload CPU time/count/vertices and local hash;
  do not hide this startup cost inside a warm performance claim. After warm-up, path 1
  should show builds=0 and cache misses=0 unless a new visual/winding/resource scope appears.
- `mesh 1` is opt-in and distorts CPU timings. Path 0 keeps its old CPU vertex/upload
  comparisons. Path 1 logs immutable local hash and changing/frozen bone-matrix delta;
  it does **not** flatten/animate/upload vertices for diagnostics or mislabel unchanged
  local vertices as a GPU no-jitter proof. Disable mesh diagnostics for performance samples.

## Decisions for the user

1. Conservative helper-only rigid grouping: 43 resident / 29 default-visible groups, keeps
   arbitrary named-bone animations/visibility and exact DFS order. Do not merge across a
   possible future animated or visibility boundary merely to save another few calls.
2. Default path 1, retain path 0 and original bake/upload benchmark as a transparent A/B
   fallback. No new opaque native hook, skin experiment or GPU cache mutation.
3. Use one evaluated frame for both gun and arms; preserve all common ADS/camera/constraint
   math and put-away ownership. Wire captured visibility to both paths so A/B stays fair.
4. Report native new-path timing, visible jitter and RTX acceptance as pending until an
   actual game run supplies evidence. Unit tests do not make those measurements possible.

## Validation / deployment

- Both **client and server x64 release builds passed**. Only existing LL hook-macro/test
  u8path deprecation warnings remain; no new native hook or ABI workaround was added.
- Final full test run: **138 tests / 73,430 assertions passed**, zero failures/skips.
  The two printed sol2 exceptions are intentional negative-test cases, not test failures.
- `scripts/deploy-client.sh` succeeded with manifest **0.3.18 / client**, destination
  `F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz`.
- Deployed DLL, PDB and manifest SHA-256 matched `bin/tacz` byte-for-byte. Deployed
  resource-pack (4 files) and behavior-pack (2 files) maps/hashes exactly matched `packs`.
- Final client DLL SHA-256:
  `6D34EF32A03DE43676B762E422ED0B4ABEACAC95B8A6C5D2FE56A3876310DCC9`.
- No new agent-run Minecraft test; native new-path CPU, jitter and RTX acceptance remain
  pending. The assistant did not stop the game to deploy.

## In-game A/B acceptance plan

1. Start at main menu, join world and draw AK47 (path 1 default). Save `[gun-rigid-build]`,
   `[gun-render]` and `[gun-perf]`. After warm-up, verify builds=0, cacheHits grow, usually
   ~29 gun calls (visibility/zero-scale keys can reduce that), no unbounded mesh growth.
2. With `mesh 0`, `freeze 0`, `depth 0`, run idle/walk/shoot/tactical+empty reload/inspect,
   hip and ADS, then put-away/rapid scroll. Watch individual faces, not only overall motion.
   Check both hands, sights, moving bolt/trigger/magazine, additional magazine, empty chamber
   visibility and common FOV/constraint alignment. Camera Shake toggling and sprint cases
   accepted in 0.3.17 must remain correct; View Bobbing aimed-walk sway is still known.
3. `/tacz rdebug path 0`, repeat matched actions for >=120 changing poses and >=120 frames;
   then `path 1` for >=120 frames. Capture original old rebuild and both new `[gun-perf]`
   records. Compare like-for-like RTX/movement/material/settings, not a cold load to a warm
   cache or frozen idle to active reload. Path switching resets the 120-frame summary.
4. `freeze 1` at a settled pose, switch 0/1: geometry/UV/position/arms should align; zero-scale
   hidden pieces must stay absent. Unfreeze and compare per-face motion flicker. `mesh 1`
   can show fixed local hash vs advancing matrices; restore `mesh 0` before timing.
5. Turn RTX ON, repeat all actions in path 1. Internal bolt/mag/trigger etc. must animate,
   not merely whole-gun ADS placement or arms. Compare path 0 as the historical control.
   Save renderer logs with rtx=true and explicit path. Check lighting, culling and shadows,
   not only motion. Toggle RTX off/on again for fresh resource generations.
6. Leave during held gun and mid put-away; world A -> B -> A. Ensure release/rebind serials,
   no stale/pink texture, meshes upload only on fresh scope and no stuck arm/gun state.
   Main menu must pass vanilla through with no world GPU lookup or draw.
7. Cleanup: `path 1`, `freeze 0`, `depth 0`, `mesh 0`, default material/winding `rdebug 6 0`.

If native matrices fail to move parts or measured draw cost is unusable, capture the exact
path/RTX/call/timing evidence and stop rollout; path 0 remains available while investigating.
Do not claim vertex-quantization proof or blindly delete source faces as an alternative.

# RTX static gun: 0.3.10 runtime analysis

Status (2026-10-25): parked by user decision. The historical analysis below predates the
separate 0.3.11 arms work (`ARMS-0.3.11.md`); that release does not claim an RTX fix.

## 0.3.11 runtime follow-up: arms animate, gun remains static

User tested RTX off/on after the implementation below: first-person arms correctly follow
idle, walking, shooting, reload, inspect and put-away; no crash reported. Under RTX the **arms
animate while the gun remains static**. This is the user's game observation, not an agent run.
Both Persona and imported classic arm skins fell back to defaults (tracked separately in
`SKIN-0.3.12.md`). Approved 0.3.11 code/docs were committed as `ce8c25b`.

The meaningful difference is unchanged cached arm geometry + per-draw bone/world matrices,
versus gun poses baked into vertices and uploaded via mesh reconstruction on each pose.
The gun uses `optional<Mesh>::emplace` in reused cache storage: this is not merely editing one
existing vertex array, and a new C++ construction does not establish a fresh backend identity.
The arm result strongly supports hypothesis 1 below: per-draw transforms reach the RT path,
while geometry/BLAS associated with a reused mesh identity may remain stale. It does **not**
prove the exact backend cache key, a specific buffer/BLAS lifetime, or exclude classification
and metadata differences. No new RTX hook, cache-key experiment or render-path fix is made
in 0.3.12. RTX work remains parked for the planner's separate task.

Latest evidence (`logs/2026-10-25.log` and `2026-10-25.1.log`):

- RTX on, prior log line 122: arm CPU totalMean **0.032 ms**, max 0.571 ms,
  probeMean 0.007 ms, buildUploadTotal 0.111 ms, matrixDrawMean 0.023 ms.
- RTX off: totalMean **0.028–0.034 ms** across the recorded 120-frame samples; newest log
  line 125: 0.034 ms, max 0.448 ms, probeMean 0.006 ms, buildUploadTotal 0.117 ms,
  matrixDrawMean 0.026 ms.
- Each sample: **4 builds / 480 draw returns in 120 frames**, consistent with four cached
  arm/layer meshes and matrix-only animation. These are CPU submission timings, NOT GPU/DXR
  execution timings; counters alone do not prove visible animation or BLAS refresh.

The remaining sections are the earlier 0.3.10 investigation, not a new 0.3.12 implementation.

## Outcome / scope

Approved combined 0.3.9/0.3.10 commit: `1ee753c` on `feat/milestone-1` (no amend/push).
User-confirmed results: no crashes at menu/join/leave/world switches; pink texture gone for
mid-put-away exit, held-gun exit and A -> B -> A; RTX-off actions/switches/fast scrolling pass.
RTX-on is still visibly static with sound. These observations are the user's game tests,
not a claim that the agent launched Minecraft.

No cause-level RTX fix is yet established. No 0.3.11 version bump, rendering modification,
native hook, build or deployment was made in this investigation. Only follow-up documentation
was changed after committing the approved code. Re-ran all 92 tests / 21,839 assertions: pass.
The prior server/client release builds and deployment remain the tested 0.3.10 artifacts.

## Latest log and quantitative comparison

Selected by modification time: `logs/2026-10-24.22.log`, 258,578 bytes, last modified
2026-10-24 20:19:54, in the LeviLauncher `1.26.51.01-LeviLamina` installation.
The preceding `.21.log` is only 4,059 bytes and predates this successful run.
`.22.log:21-22` identifies TACZ 0.3.10. It includes RTX on -> off -> on, not just one toggle.

Each row below sums the logged `[gun-render]` windows for a single world/cache serial;
duration is the sum of their rounded `seconds` fields, NOT elapsed wall time across menus.
Trailing partial windows are not logged. Calls are CPU call-site observations, not GPU draws.

| World / serial / mode | Windows / logged seconds | Hooks | Builds | Draw returns | Builds/hook | Hooks/s / builds/s / draws/s | First -> last poseRev |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 3 / 1 / RTX on | 39 / 40.03 | 3,290 | 3,275 | 3,289 | 0.9954 | 82.19 / 81.81 / 82.16 | 61 -> 3,297 |
| 3 / 2 / RTX off | 6 / 6.02 | 778 | 777 | 777 | 0.9987 | 129.24 / 129.07 / 129.07 | 3,423 -> 4,080 |
| 5 / 3 / RTX off | 4 / 4.02 | 378 | 372 | 377 | 0.9841 | 94.03 / 92.54 / 93.78 | 4,184 -> 4,492 |
| 7 / 4 / RTX off | 36 / 36.21 | 7,973 | 1,359 | 7,972 | 0.1705 | 220.19 / 37.53 / 220.16 | 4,584 -> 5,871 |
| 7 / 5 / RTX on | 1 / 1.00 | 101 | 28 | 100 | 0.2772 | 101 / 28 / 100 | 5,898 (one sample) |
| 9 / 6 / RTX on | 312 / 316.40 | 9,002 | 8,993 | 9,001 | 0.9990 | 28.45 / 28.42 / 28.45 | 6,075 -> 14,990 |

All rows have `invalidMeshes=0`, `firstPerson=true`, `deferred=false`, `cacheType=0`,
`cacheId=0`, `temporary=false`, `cpuPositions=0` in every window.
`cacheType=0` means **monostate/no explicit cache key**, not a real integer cache ID of zero.
Do not infer that every such mesh maps to one BLAS without tracing the downstream key.

- Independent `[anim-frame]`: world 3 RTX-on poseRev 5 -> 3,272, animFrame 4 -> 3,392;
  same-world RTX-off poseRev 3,369 -> 4,016, animFrame 3,489 -> 4,137. World 9 RTX-on
  poseRev 6,028 -> 14,893. The independent heartbeat confirms the producer is advancing.
- RTX inspect/walk window 20:11:54-20:12:10: 1,419 hooks, 1,414 builds, 1,419 draw returns.
  RTX tactical-reload window 20:14:03-20:14:08: 481 / 481 / 481; poseRev 6,364 -> 6,740.
  RTX empty reload also appears at 20:16:18.820-20:16:21.445 while builds continue.
- RTX-off active window 20:13:14-20:13:25: 1,270 hooks, 1,170 builds, 1,270 draws.
  Later unchanged idle poses legitimately reuse their mesh: e.g. 20:13:48 logs 389 hooks,
  0 builds, 389 draws at poseRev=meshRev=5,871. Do not interpret this as stopped animation.
- Mesh versions also advance: world 3 RTX-on meshRev 60 -> 3,296; RTX-off 3,422 -> 4,079;
  world 9 RTX-on 6,074 -> 14,989. Logging occurs BEFORE the current draw's rebuild: poseRev
  is current, meshRev is previous completed build. A difference of one (occasionally two)
  is not evidence that the current draw uses an old pose. Code rebuilds first, then submits
  that mesh. There is no per-draw version record or GPU acknowledgement in this log.
- Hook totals include the current not-yet-drawn invocation, whereas build/draw counters
  describe completed work. Reset/initialization windows therefore need not match by one;
  the first display also builds a rest mesh before building its animation pose.
- `cpuPositions=0` is measured on the buffered mesh, not the TACZ vertex vector nor the
  tessellator before upload. Both modes have it; initial tessellation has 7,608 positions,
  UVs/normals/colors. It does NOT establish missing vertices or an RTX-only CPU-data bug.
- Log rates vary with workload and long idle/background periods; this is not a controlled
  RTX/raster FPS benchmark or proof of a mode-specific performance regression.

Resource events match the intended lifecycle, despite the TextureGroup address being reused:

| Time | Event |
| --- | --- |
| 20:11:51.093-.110 | world 3, serial 1, RTX-on bind/create |
| 20:12:31.304-.317 | release serial 1, RTX-off serial 2 bind/create |
| 20:12:37.564 | release serial 2, world exit |
| 20:12:57.971-.988 | world 5, serial 3, bind/create |
| 20:13:02.112 | release serial 3, world exit |
| 20:13:11.810-.825 | world 7, serial 4, bind/create |
| 20:13:49.023-.041 | release serial 4, RTX-on serial 5 bind/create |
| 20:13:50.950 | release serial 5, world exit |
| 20:13:59.377-.391 | world 9, serial 6, RTX-on bind/create |

## Current CPU cost (not a new fix's cost)

Existing first-120-changing-pose instrumentation includes CPU bake + tessellation/upload
submission, not GPU execution or full rendering cost:

| World / mode | Mean ms/rebuild | Max ms/rebuild |
| --- | ---: | ---: |
| 3 RTX on | 4.175 | 5.236 |
| 3 RTX off | 3.849 | 4.583 |
| 5 RTX off | 4.146 | 5.177 |
| 7 RTX off | 4.121 | 4.793 |
| 9 RTX on | 4.039 | 4.936 |

These small initial samples are not whole-session per-frame timings. This investigation adds
zero render work. Any eventual diagnostic/fix build must separately time producer/bake/upload/
submission in each mode, including unchanged-pose frames and diagnostic overhead.

## Header and native-path findings

Authoritative SDK: installed LL 26.51.7 client headers for Minecraft 26.51.1, under `include/mc`.
Searched RayTracing/RTX/Deferred/BLAS/Dynamic and mesh/cache/first-person/actor paths.

1. `GunRenderer.cpp:206-234` constructs a fresh Tessellator and calls Buffered `end` on each
   changing pose, then `optional<mce::Mesh>::emplace`. This **already destroys/reconstructs**
   the mesh, rather than merely editing an existing vertex array. However, its object storage
   address is reused. The log does not record buffer resource identities. Switching to a heap
   allocation does not guarantee distinct backend keys or safe asynchronous lifetime either.
2. `Mesh.h:44-70` exposes cache key, temporary flag, CPU MeshData, raw bytes and resource
   pointers; it provides no documented `setDynamic`/BLAS-dirty/update contract. The three
   Tessellator UploadModes (Buffered/Manual/Never) are not a documented RTX-dynamic selector.
3. Our call is `Mesh::renderMesh` without explicit metadata. Read-only disassembly of the
   installed executable at `_renderMesh` RVA `0x4B04470` shows absent-metadata branch
   `0x4B0484C-0x4B0488D` initializes ID=-1, CSS=0, isItem=false. At `0x4B049EE-0x4B04A14`
   it selects description variant index 2 and calls FrameBuilder's insertion slot 107
   (vtable offset `0x358`). The
   `FrameBuilder.h:433-446` variant index 2 is `RenderMeshFallbackDescription`, distinct from
   `RenderItemInHandDescription` (index 6) and `RenderSkinnedMeshDescription` (index 7).
   This identifies fallback submission/default classification, **not proof it causes staleness**;
   the description also carries MeshContext flags, including first-person state.
   No hook was installed, and no native function or metadata factory was invoked for analysis.
4. The initial log and all later material-state lines say material **6, moving-block alphatest**;
   this is the current source default (`src/debug/RenderDebug.h:10-11`), not an inferred user
   override. `firstPerson=true` alone is not equivalent to explicit item/skinning metadata.
   `ItemInHandRenderer.h:301-325` provides `renderObject` with metadata and an engine
   `_createRenderMetadata` factory; the correct identity/classification/invalidation semantics
   for a CPU-deformed custom mesh still need verification before using them as a fix.
5. Native actor geometry is not limited to the generic fallback: `ModelPart.h:122-152`
   copies bone matrices to actor/skinned meshes; `SkinnedMeshGroup.h:53-76` updates part
   matrices/visibility and draws with RenderMetadata. `SkinnedMesh.h:26-35,48-59` keeps current
   and previous bone matrices and draws with material variants/PBR data/previous transforms.
   `FrameBuilder` has skinned/batch-actor descriptions. `bgfx.h:150-159` exposes scheduled
   pre-BLAS skinning; D3D12's `BLASGeometryInfo` tracks creation/update frames and buffer handles.
   These establish an animated-geometry route worth pursuing, not that one call can replace
   our CPU-baked gun. Mapping 605 gun bones, visibility, materials and first-person transforms
   into that model pipeline is a larger integration, not a contained safe fix established here.
6. `RenderMeshFallbackDescription`, `RenderItemInHandDescription`, `RenderSkinnedMeshDescription`,
   BVHMesh/BVHMeshKey/BVHSkinnedMesh and RT frame-renderer headers are empty placeholders.
   `DynamicVertexBufferHandle` is also an empty placeholder despite being passed by value by
   `schedulePreBlasSkinningDispatch`. Calling it through this declaration would repeat the
   class of ABI error removed in 0.3.10. Do not construct these or guess their layouts.
7. Lamium main's `src/overlay/CellMesh.cpp` and `src/features/schematic/Ghost{Mesh,Renderer}.cpp`
   also use Buffered meshes and fallback renderMesh, including moving-block materials. They
   do not establish the animated-RTX invalidation contract. They are examples of overlays,
   not proof that rebuilding a first-person animated mesh works under RTX.

## Ranked hypotheses / what is still missing

1. **RT fallback geometry/BLAS cache retains the first geometry for a reused identity.**
   Best fit to continuously advancing builds/draw returns but static pixels. Object storage
   address reuse is real; actual backend key, buffer changes and BLAS cache hits are NOT
   measured. Neither monostate nor a constant pointer alone proves this hypothesis.
2. **Fallback/block-material classification bypasses the animated item/skinning update path,
   or a cached RT first-person draw list consumes a different/stale geometry resource.**
   The separate description variants, absent explicit metadata and block material support
   investigation. Current logs cannot distinguish this from (1); both may be parts of one
   cause. No evidence establishes that setting isItem or changing material alone is sufficient.
3. **Pose changes fail to reach the baked/uploaded vertex data.** Lower priority because the
   same code animates in raster and rebuilds track changing poses under RTX. But pose revision
   is not a vertex checksum; relevant geometry might still remain unchanged. Not ruled out.

There is no evidence here that RTX animation is impossible. Lamium/playback are loaded;
interaction has not been isolated, so do not attribute a native backend defect to them.

## Exact safe diagnostic plan (not implemented/deployed)

1. At TACZ's existing owned build/draw call sites only, record a bounded sample/ring buffer:
   world/cache serial, producer frame + pose revision, built revision, submitted revision,
   mesh construction serial/address, vertex count, position/normal/UV hash and bounds **before
   `end`**, raw-byte size/hash when available, and typed buffer resource identities before/after
   draw. Record selected material and existing MeshContext flags too. Log AFTER successful
   draw, not the current pre-build window. Never dereference expired owners or inspect opaque
   description memory. This separates (3) and repeated buffer identity from downstream staleness.
2. Pair adjacent D3D12 GPU captures (e.g. PIX with DXR capture support) during an obvious reload
   or inspect, RTX on then off. Match the `tacz gun` resource and construction/draw serials.
   Compare uploaded vertex bytes, VB/IB GPU addresses, AS build/update commands and BLAS/TLAS
   references. New vertex data but old BLAS input/reference supports (1); no corresponding
   submission or a different consumed mesh supports (2). CPU API return counts cannot replace
   this observation. Capture may require launch/injection setup and can significantly slow or
   disturb timing; if unavailable, report that limitation, not a fabricated cache result.
3. Compare a vanilla animated actor/held animated item in the same capture: identify its bone
   update/pre-BLAS skinning path and frame/resource ownership. Read-only debugger inspection at
   known, symbol-verified functions can compare metadata IDs/item flags; no new native detour
   is necessary. Do not call the opaque-handle skinning function from the generated headers.
4. If classification remains implicated, first reverse/verify the exported engine metadata
   factory and its consumption, including how geometry identity and update are coupled; only
   then consider a targeted experiment using its fully defined return type and the const-ref
   metadata overload. A material-only command probe can use the existing `/tacz rdebug`, but
   disappearance under an entity material is not proof of cache invalidation or a working fix.

No random cache IDs, temporary flips, per-pose allocation workaround, global cache flush,
unbounded retained mesh pool or opaque-by-value hook is justified by current evidence.

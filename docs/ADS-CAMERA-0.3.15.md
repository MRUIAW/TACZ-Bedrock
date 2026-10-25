# 0.3.15 — ADS/recoil fixes, functional bones, z-fighting investigation

Parent `3a56a2a`. This supersedes the queued camera/constraint-only task. The agent did not
launch Minecraft or run new in-game tests. A pre-deploy process check briefly found Minecraft
running; a subsequent check found it exited, and the normal deploy script succeeded. No game
process was stopped by the agent. Final acceptance risks below
are intentional, not replaced with a claim that a typed hook or unit test proves rendering.

## User-confirmed 0.3.13 / 0.3.14 baseline

Right-click interception, hold/release aim, smooth movement to iron_view, world FOV zoom,
slightly lower aim sensitivity, shoot animation, native pitch limit, switch/focus aim reset
and world switching all passed. Under RTX the **whole gun moves to the iron-sight position**,
but baked internal animation remains static. `RTX-0.3.10.md` records that distinction.

## 1. Recoil: native input is not an actor-angle setter

Evidence: installed 26.51.1 `_applyTurnDelta(Vec2 const&)` signature is unchanged and checked.
User log `2026-10-25.5.log:191–194`:

| frame | native request pitch/yaw | actor before pitch/yaw |
| --- | --- | --- |
| 1 | -.531189 / +.160341 | -6.547424 / 48.878433 |
| 2 | -.153759 / +.054462 | -6.016251 / 49.038773 |
| 3 | -.122496 / +.052588 | -5.862488 / 49.093231 |

The next-frame pitch change is **minus** the prior native input; yaw change is **plus** it.
Immediate before==after is normal: the native camera is turned first, actor orientation is
copied next frame. The old warning is removed, not used as evidence of a failed turn.

TACZ subtracts curve deltas from both actor angles. New pure `bedrockRecoilTurn` maps the
resulting Java look delta to native input as **(-lookPitch, lookYaw)**, i.e. **(+curvePitch,
-curveYaw)**. Only pitch needed reversal; reversing yaw as well would introduce a new bug.
Java `Entity.calculateViewVector` is `(-sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))`:
yaw +90 faces -X, right when facing +Z; negative yaw is left. Lamium's documented Bedrock
view-ray convention is identical. Thus an AK47 negative yaw key (-.23) turns right; a positive
key (+.18) turns left, exactly as TACZ. Per-axis tests pin both endpoints/signs.

Natural splines, +30 ms knots, randomization, per-shot ADS modifier, replacement overlap,
no extra recovery spring and the player-specific MDV bypass are retained. `[recoil-burst]`
prints one summary after a >=750 ms quiet/settled gap or cancellation: shot count, summed
intended actor deltas, submitted native deltas and observed actor deltas. A continuous spray
or rapid semi pulls form one diagnostic burst; it is not the gun's fire-mode burst count.
Observed deltas wrap yaw and include mouse input, clamping/corrections; they are not a clean
measurement of recoil alone. Totals near pitch limits will legitimately differ.

## 2. ADS size, sight projection and crosshair

AK47 source display is **iron_zoom=1.33, zoom_model_fov=45** (not iron_zoom=1.2).
`2026-10-25.5.log:310` proves 45 reached the old aim snapshot. Parsing/direction was not the bug:
`withModelFov` reduces FOV by increasing P00/P11. However the old code only pushed a temporary
Camera projection stack around renderMesh; there was no evidence RenderDragon consumed that
late edit. The existing logs print the native 70-degree hand projection, before the edit.

The new model-only path uses **C = inverse(P_native) * P_model**, then submits `C * modelView`
through the already user-verified per-draw transform. Therefore `P_native*C == P_model`,
without modifying the world projection or relying on a late Camera projection-stack edit.
At 70 ->45 degrees its X/Y scale is ~1.690; Z and depth are unchanged in default mode.
Use the same TACZ model-FOV dynamics and progress interpolation; do not stack a second FOV
edit on top, physically move the gun forward, or invent a hand-offset/centering constant.
This is an algebraically equivalent model projection carried by a draw-local transform,
not a newly verified dedicated RenderDragon first-person projection API.
It guarantees the same clip-space mapping for affine model transforms under the assumed
native projection, not identical lighting/RT scene semantics: shaders may also use the scaled
model basis for normals and RTX treats it as geometry scaling. Native per-pass model projection
integration remains preferable in the render rework; compare lighting as well as size at runtime.

Compared `getPositioningNodeInverse` / wrapper / +/-1.5 translations / Rz180 / /16 units:
the existing rest inverse is correct. A real-AK47 test places iron_view's origin exactly at
the eye and a point one block down its optical axis at NDC (0,0). The origin itself has
homogeneous **w=0 at full ADS**, so reporting its x/w,y/w as a screen point would be invalid.
`[ads-projection]` logs origin **clip x/y/z/w**, and `sightAxisNDC` using the current animated
path, view/world matrices and submitted transform. NDC (0,0) is screen centre; right/down
drift at runtime can now be separated from static inverse errors. Constraint coefficients
deliberately permit some residual motion, so animated sight coordinates need not be zero.

Vanilla crosshair is conditionally skipped at the typed `HudCursorRenderer::$render` boundary,
as Lamium skips HUD custom renderers. Only the matching local world/client/gun is affected.
Follow `RenderCrosshairEvent`: hide for progress >.9 unless display `show_crosshair` forces
it, for state-machine `setShouldHideCrossHair`, and while reloading. No whole-HUD hide or
resource-pack UI replacement; custom crosshair/hit-marker drawing is still a later task.

## 3. Constraint listener and common matrix

`ConstraintTranslateListener`: source /16 animation values become coefficients via *16.
`ConstraintRotateListener`: radians become positive degrees. Blending takes **component max**,
including a zero clean/reset baseline, not sums/quaternion blending. Transition recovery now
accepts model-dependent listener `initialValue`; constraint translation recovers the converted
child pivot /16, or (rawX,-rawY,rawZ)/16 at a root, rather than universal zero.

`FirstPersonRenderGunEvent:306–396` is implemented separately from mesh transforms:

1. Root-to-constraint paths: animated ancestor translation -> local rest pivot (root Y-1.5)
   -> animated quaternion -> rest Rz/Ry/Rx. No scale. Constraint's own translation/rotation
   channels are coefficients, excluded from the path's animation and mesh transforms.
2. Extract rest/animated positions and ZYX Euler-angle differences. Transform rest-animated
   displacement by the **post-ADS matrix direction basis**, then multiply (tx-1,ty-1,1-tz).
3. Multiply rotation difference by (rx-1,ry-1,rz-1), apply weighted **Rx,Ry,Rz** about animated
   constraint position +Y1.5; directly adjust matrix translation (-inverseX,-inverseY,+inverseZ).
4. Weight is the same smoothed aiming progress as ADS placement. No refit UI exists, hence
   refit multiplier is neutral 1. Camera hand quaternion is outside this ADS/constraint matrix.

GunRenderer appends this entire common matrix before both gun and PlayerArms draws. Arms
remain attached. Special channels are published for consumers but never baked as fake
constraint/camera geometry rotations. Existing ordinary part rest/additional order is kept;
the constraint reference deliberately uses its different animation-before-rest order.

### Actual AK47 channels (do not infer them from names)

- `camera`: draw, put_away, switch_auto, switch_semi, shoot; reload_tactical/reload_empty and
  both xmag variants; inspect/inspect_empty and both xmag variants.
- `constraint`: **static_idle**, shoot, the four reload variants. Static idle coefficients:
  translation .1/.1/.1, rotation .2/.2/.4. Reload uses .02 translation and 0 rotation for most
  of the clip, then returns toward idle coefficients. Shoot supplies its own animated values.
- **idle/walk_forward/walk_backward/walk_sideway do not directly key constraint** in the loaded
  reference clips. They animate ancestors; the static constraint layer still compensates
  their motion. This explains why missing constraint affects aimed idle and walking too.
- Rifle-default fallback adds camera to run_end and other unused slide/melee prototypes;
  AK47's own draw/put_away win merge precedence. This task does not add slide/melee gameplay.

## 4. Camera-only pitch/yaw/roll

CameraRotateListener uses quaternion from (x,y,-z), MathUtil's log-space quaternion blend,
and axis-angle scale **1-aim+aim/sqrt(iron_zoom)** (NO recoil sqrt cap). Extract its Euler angles
and add them to the rendered camera angles, including roll; hand stack gets the scaled
quaternion. Bob-view option gates both, matching TACZ. AK47 shoot's +1.025-degree Z key at
.0333 s becomes -1.025 degrees roll before aim scaling. No player/actor rotation setter,
turn delta, input packet or persistent camera-ECS mutation is used for camera bones.

Studied Lamium CameraSessions/DetachedCameraRig: safe setup signature is
`void LevelRendererPlayer::setupCamera(mce::Camera&,float)`, but Lamium explicitly found
**post-return edits too late** for detached rendering. This adapter instead scopes setup with
an exact camera pointer on TLS, and changes its view **before native
`mce::Camera::updateViewMatrixDependencies()`**. Native code then updates inverse view/axes/
frustum for that view inside setup, before callers can snapshot it. Matrix math is Java/Forge
Rz(roll)*Rx(pitch)*Ry(yaw+180) with angle additions, retaining the eye position. All hook member
signatures are compile-time checked against installed client headers; no opaque by-value type.

Entry guards check active world then the immutable animation snapshot's captured live-HUD
eligibility and comparison-only client identity, first-person and Bob View options. The
retained put-away item can still shake the view with aim weight zero, like KeepingItemRenderer;
camera ownership does not incorrectly depend on the ADS-allowed gun snapshot. No primary-client/world lookup on the
main menu. TLS restores on exceptions; no engine owner survives a callback. Recoil still acts
on real look in the frame adapter; FOV remains independent. Nothing can feed visual bone
rotation back to the player through a camera component that updates actor look.

**Consumption remains a runtime acceptance item.** The scoped dependency callback must
actually be reached on this pipeline. `[camera-bone] before-dependencies=true` and
`finalViewRetained=true` observe that path, not GPU output. If not reached, warn and do not
fall back to moving player look. First 8 samples per world/debug revision are logged; use
`/tacz rdebug 6 0` after draw settles to reset samples, then fire or inspect. Detached
Lamium cameras, custom camera presets, RTX/Deferred consumption and hook ordering must be
tested. Safe signatures alone are not proof the world image rolled. No game was launched here.

## 5. Aimed reload: keep aim, no invented variant

Read LocalPlayerAim/Reload and LivingEntityAim/Reload: reload does not clear aiming; only
put-away blocks client aiming. AK47 `runReloadAnimation` chooses tactical/empty/xmag by ammo
and magazine, **not by aim**. ADS positioning runs on every first-person render, including
reload. Thus preserve right-button aim/progress/FOV and the same reload clip; the new constraint
path, especially reload's near-zero coefficients, counteracts the large hip-reload motion.
The adapter already kept aiming, so no new aim-cancel or animation branch was added. A real
Lua state-machine test verifies aimed tactical reload retains aim and emits constraint/camera.

## 6–7. Sprint input and aim.json

Frame-end setSprinting(false) alone allowed vanilla input to request sprint again between
frames. The frame publishes comparison-only identities for this player's MoveInputComponent
and actor. A typed `ClientInputUpdateSystem::inputHandlerUpdatePlayerState` hook clears
SprintDown on input/raw input and the extracted Sprinting flag before and after native
processing; `LocalPlayer::$setSprinting(bool)` also rejects re-entry for that exact owner.
No movement directions/jump/sneak are swallowed. Both paths require a live world token.
Block during aim and non-finishing reload, like LivingEntityAim.tickSprint. On release,
native sprint input resumes; a held sprint key may naturally sprint again after aiming ends.

Old config is migrated without discarding unrelated keys:

```json
{
  "hold_to_aim": true,
  "screen_distance_coefficient": 1.33,
  "zoom_sensitivity_base_multiplier": 1.0
}
```

Sensitivity = atan(c*tan(fovAim/2))/atan(c*tan(fovBase/2)) *
(1+(mult-1)*progress). Require finite c>0 (else 1.33), clamp finite mult to [0,2] (else 1).
Config loads on enable; restart/re-enable after editing. Recoil bypass remains independent.

## 8. Z-fighting: geometry first, depth second, material third

Read TACZ BedrockModel/BedrockCubePerFace/Box/Polygon/FaceUVsItem. Independent tests cover
25 candidate cover2/bolt cubes, including multi-axis rotations, small faces, inflate +.001,
all generated vertex positions AND polygon/UV ordering. Parent/helper pivot conversion,
degrees, rest Rz/Ry/Rx, /16 scale, source face remapping agree with the Java formulas.
Existing geometry tests cover mirrored Box UVs/winding and PerFace's intentional lack of mirror.
**No source cube is deleted, inflated or nudged.** The candidate identification is anatomical
(top rear cover/chamber); no screenshot pixel was claimed uniquely mapped to a cube.

`python scripts/analyze-ak47-geometry.py` is a read-only source polygon-plane/SAT audit.
Within each selected bone it requires positive-area overlap (not shared edges). Source itself
contains coplanar/near-coplanar faces. Examples, **zero-based cube indices**, separation in pixels:

| candidate | face pair | max plane separation | blocks |
| --- | --- | ---: | ---: |
| top sloped cover | cover2 12.west /55.west; 13.west /48.west | 1.20664e-6 | 7.5415e-8 |
| top sloped cover | cover2 22.east /25.east; 24.east /27.east | 4.84720e-6 | 3.0295e-7 |
| chamber/bolt | bolt 4.north /5.north (also /16.north) | 0 | 0 |

Their face coverage/textures can contend despite matching model conversion. Recomputing tiny
animated vertex differences with CPU float matrices versus another renderer's GPU path can
alter winners. This is a plausible mechanism, not proof of the exact Bedrock GPU failure.
Why the same source is stable in the user's Java renderer is still not established.

Depth: native first-person logs give P22≈-1 and P32≈-.05 with w=-z, consistent with D3D
zero-to-one depth and near≈.05; rounded logs cannot determine finite versus infinite far.
Now log exact Camera near/far and matrix, and the material's descriptor on each debug change.
Java/Forge builds a distinct held-item projection after clearing depth (Forge 1.20.1
GameRenderer patch shows clear(256) before hand); Java's .05 near, renderDistance*4 far are
not automatically a magic fix for coplanar faces. We do **not** clear Bedrock's shared depth.

Material: default here is moving-block alphatest (`rdebug 6 0`), not Java entityCutout.
Java cutout uses LEQUAL, color/depth writes and default no polygon offset. The installed
RenderMaterial headers expose legacy depth-test/function/write mask/bias/slope bias;
`[gun-depth]` reads/logs them without mutating a shared material. RenderDragon may map or
override those descriptors, so final GPU state is **not known without a capture**. Existing
`/tacz rdebug 0 0` can compare entity_alphatest, but earlier versions did not reliably draw
that material. Do not globally change the material's depth test or guess a bias.

### Debug toggles (integrated/single-player only, same style as existing rdebug)

- `/tacz rdebug freeze 1`: retain current published animated pose AND gun/arm common render
  transform. Controller/gameplay/sounds still advance; this is not a pause or ammo cheat.
  Stand still and don't move the mouse: compare flicker without animated vertex updates.
  Camera bone sample is frozen too; native camera movement is not disabled. `freeze 0` resumes
  the current live pose (no catch-up sound replay). Switch/world change cannot reuse an old pose.
- `/tacz rdebug depth 0`: native depth mapping (default).
- `/tacz rdebug depth 1`: model-only zero-to-one near=.05/far=16 diagnostic.
- `/tacz rdebug depth 2`: near=.10/far=16 diagnostic. May clip nearby sights or change world
  occlusion; do not keep it as a purported fix. Uses P_native^-1*P_debug in the model draw,
  no shared camera/projection/depth-buffer state edits. Projective matrix handling itself
  needs runtime comparison on each backend; this is an experiment, not a proven GPU API.
- Restore `/tacz rdebug depth 0`, `/tacz rdebug freeze 0`, `/tacz rdebug 6 0` after testing.

These atomics are shared by the integrated server/client DLL like the old material toggle;
remote server commands do not transmit render settings to a separate client. For remotely
hosted worlds use an integrated test world for this diagnostic first.

**Outcome:** source overlaps and matching conversion are established; precise GPU depth/cache/
material cause is not. No cause-level z-fighting fix is claimed. Defer dedicated depth-pass,
material/GPU capture and stable per-bone draw work to the planned **0.3.19 mesh-once + bone
matrices rework**, rather than bake a speculative source-asset workaround into this release.

## Decisions for the user

1. Fix only the inverted native pitch axis; retain yaw's proven sign. Summed submitted and
   observed angles are separately labelled, not conflated near clamps or mouse motion.
2. Keep Java ADS inverse/no hand-offset guess; move model-FOV effect to mathematically
   equivalent draw-local correction instead of an unverified late projection-stack edit.
3. Implement full translation+rotation constraint, including special max blending/recovery,
   after ADS and before attached arms. No extra walk-sway multiplier or fabricated reload clip.
4. Full camera roll is implemented at a typed pre-dependency view boundary. No player-look
   fallback; if runtime says the consumer isn't reached, report that pipeline as unsupported.
5. Keep aiming during reload; same AK47 clip plus ADS/constraint, as Java does. Block sprint
   requests while aiming and non-finishing reload, not all movement or indefinitely after release.
6. Hide only vanilla crosshair when TACZ rules ask; preserve force-show option. Custom HUD remains
   deferred. Respect Bob View for world/hand camera bone rotation.
7. Preserve model source overlaps, default material and native depth. Depth/freeze toggles are
   diagnostics, not defaults or a claimed z-fighting cure. Final render-path work stays 0.3.19.

## Validation / next acceptance

- Server release build passed (26.953 s), packed manifest `0.3.15 / server` verified after
  the preceding successful server build. Final client release/deploy build passed (29.468 s),
  installed manifest **0.3.15 / client**. Only known LL macro unused-variable warnings;
  test compilation also reports existing std::filesystem::u8path deprecation warnings.
- **126 tests / 46,949 assertions passed, zero failures/skips**. Includes per-axis native
  sign mapping, MDV options, log-space camera blending and roll/angle conversion, camera-eye
  invariance, max coefficient blending, model-dependent recovery, constraint path/order/basis/
  pivot/weights, projection equivalence/depth, real AK47 sight centre/reload/channel tests,
  and 25 suspect source cubes' independently calculated vertices/face/UV order.
- Pure timings from the final run: AK47 Lua/controller .0141 ms/frame; evaluate+bake .0485
  ms/frame; both cached arm paths .000291 ms/frame. Not native rendering/upload/GPU timings.
- `git diff --check` passed; deployment succeeded. DLL/PDB/manifest SHA-256 all match build
  output; all 4 resource-pack and 2 behavior-pack files match source. Final DLL SHA-256:
  `F9550965F2D20CD07FE95ED6EEAA6EDD8F368FB42DF8EC52895B6266973A77EC`.
- No push/amend/hook skipping. No 0.3.15 in-game acceptance result, GPU capture or verified
  z-fighting fix. The next eight-part runtime plan below is still required.

External source cross-checks (no code from them is bundled): Forge's
`https://raw.githubusercontent.com/MinecraftForge/MinecraftForge/1.20.1/patches/minecraft/net/minecraft/client/renderer/GameRenderer.java.patch`
for camera event/order and pre-hand depth clear; mapped Minecraft source at
`https://github.com/mahtomedi/minecraft/tree/main/src/main/java/net/minecraft`
for Entity view-vector convention and GameRenderer/RenderType/RenderStateShard formulas;
Lamium `src/features/camera/CameraSessions.cpp`, `DetachedCameraRig.cpp` and
`src/features/information/DebugVanillaHud.cpp` on `https://github.com/amatouhake/Lamium`.
Installed headers, not current upstream declarations, remain the native ABI authority.

Next game test, in order:

1. Main menu >=10 s; enter world; confirm all new hook startup flags true. No world callback,
   crash or menu camera motion; repeat quit, held/put-away quit and A -> B -> A.
2. Hip single-shot then automatic burst with no mouse input, let >.75 s settle. Positive pitch
   kicks UP, yaw varies left/right; inspect `[recoil-burst]` requested/observed totals. Repeat
   ADS, manual compensation and pitch limits. No double MDV scaling or snap-back.
3. ADS AK47: visibly larger than the old 70-FOV gun, unchanged world zoom; compare modelFov
   approaching45 and optical-axis NDC. Vanilla crosshair disappears past .9 aim and during
   inspect/reload, returns on release/item switch. `show_crosshair=true` forces ADS visibility.
4. Aim stationary and walk forward/back/side: much smaller sight drift with arms attached;
   some remaining sway is authored by nonzero coefficients. Reload tactical and empty while
   holding right click: keep zoom/aim, no hip-reload displacement; release mid-reload still works.
5. Bob View on: after draw settles use `/tacz rdebug 6 0`, shoot, switch fire mode, reload and
   inspect; world AND gun/arms should show roll, without changing the actual post-animation
   look/aim ray. Check before-dependencies/finalViewRetained and the visible image, not logs
   alone. Bob View off: no camera-bone shake; actual recoil still operates.
6. Hold sprint/run then aim: no sprint FOV/animation flicker. Try held sprint, toggle sprint,
   double-tap forward, release aim, reload, UI/focus switch; normal sprint resumes afterwards.
7. Restart with sensitivity multiplier0,1,2 and a different positive c; ADS mouse response
   should follow the formula, hip unchanged and recoil unchanged. Restore defaults.
8. Stop moving/aiming and inspect the top rear cover/chamber: live vs freeze1; depth0/1/2;
   record if frozen still flickers, if far/near changes alter it, or merely clip the sight.
   Capture `[gun-depth]` + full projection; optionally compare material0 then restore6.
   Repeat RTX off/Deferred/RTX and Lamium zoom/freecam separately. Reset toggles afterwards.

Risks: new view-consumption hook/pipeline order, model-FOV equivalence under RenderDragon/RTX,
depth diagnostic projective matrix, native sprint extraction on alternate input devices,
and unchanged backend baked-geometry/skin fallback. Unit tests do not establish GPU output,
absence of all runtime crashes, or a z-fighting cure.

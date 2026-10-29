# 0.4.4 — view-turn gun inertia and native hand-sway isolation

## Java vs. this port

Java paths are relative to `_reference/TACZ/src/main/java/com/tacz/guns/`.
Angles below are degrees unless marked otherwise. No game launch or deployment
was performed for this implementation; the checklist below remains pending.

| Rule | Java evidence | 0.4.4 |
| --- | --- | --- |
| Bob ticks independently of held item, only for the controlled camera | `LocalPlayer.serverAiStep`, source 653–664, bytecode below | One fixed step beside `aim::tick()` in `ClientShooter.cpp:570–572`; no gun gate |
| Copy previous before updating; factor .5 per tick | Same method, offsets 44–101 | `model::ViewTurnBob::tick`; no frame-rate-dependent smoothing |
| View minus interpolated bob, not interpolated view | `LocalPlayer.getViewXRot/getViewYRot`, source 202,207–210; vanilla `ItemInHandRenderer.renderHandsWithItems`, source 391–394 | Current actor pitch/yaw, with typed interpolated head yaw when riding |
| Animation first, inertia second | `client/renderer/item/GunItemRendererWrapper.java:176–199` | Draw-local effective copy of published pose; named `root` only |
| Root offset and quaternion multiplication | Same file 194–199 | Animation offset `(sy*.1/48, sx*.1/48, 0)`; `Qanim · Qx(sx*.05°) · Qy(sy*.05°)` |
| Placement and constraints follow inertia | Same file 201–206; `client/event/FirstPersonRenderGunEvent.java:119–129,306–335` | Effective pose feeds rigid evaluation, CPU bake, constraints, sight diagnostics and arms |
| Hand camera quaternion H precedes lag rotations | `mixin/client/ItemInHandRendererMixin.java:34–36`; wrapper 144–156 | H before residual C; existing projective adapter remains left of H |

Vanilla hand lag is **not gated by View Bobbing**:

```text
bx = lerp(alpha, xBobO, xBob); by = lerp(alpha, yBobO, yBob)
dx = getViewXRot(alpha) - bx;  dy = getViewYRot(alpha) - by
a = .1*dx; b = .1*dy
vanilla hand: Rx(a) · Ry(b)
TACZ appends: Rx(-a) · Ry(-b)
C = Rx(a) · Ry(b) · Rx(-a) · Ry(-b)
sx = float(tanh(dx/25))*25; sy = float(tanh(dy/25))*25
```

C is identity for a single-axis turn, not generally for a diagonal turn. At
dx=dy=25°, its angle is approximately **0.109°**, mostly roll. C uses **raw dx/dy**,
not the tanh-limited sx/sy. Maximum root translation is 25*.1/48 blocks per axis;
maximum additional rotation is 1.25° per axis. Root rotations are quaternion
postmultiplication, converted back by `model::quaternionAngles`, never Euler addition.

## Approach A, capture safety and exact order

**NOT the original TACZ implementation (Bedrock adaptation):** Java TACZ keeps
vanilla's xBob/yBob hand lag and appends inverse -0.1 rotations. We render from
the pre-turn-sway hand matrix instead, then re-apply Java's net residual C.
Never apply the inverse rotations alone to this already-unswayed matrix.

The existing typed `LevelRendererPlayer::bobView(Matrix&,float)` hook still skips
native hand bob while the published visible item is a gun, including put-away.
It now copies the matrix only for the owned client/current world and the exact
world-stack top of the active screen callback. It neither edits the captured
native matrix nor affects world/hurt bob or vanilla items.

`HandTurnScope` is thread-local, owned by the existing prep/render `RenderScope`
in `CameraBones.cpp`. Each callback gets a generation and starts empty; nested
callbacks retire their parent's capture. Scope destruction discards it, including
exception exits. The value copy carries comparison-only scope/world/client/player/
camera/stack/top/depth identities. At first-person-hook entry it is consumed once
and validated against the **PRE-push** top. Reused addresses alone cannot pass a
different scope or world. Only our pushed child is assigned; push/pop and native
parent remain intact. No new hooks, placeholder-by-value hook parameters or RVA
installed hooks were added.

Native 1.26.51.01 disassembly from the read-only research establishes:

- World stack push/top at RVAs 0x4c4a0f–0x4c4a5f; `bobView` at 0x4c4be5–0x4c4be8.
- Native X/Y sway at 0x4c4ef1 and 0x4c4fdf; same screen context reaches synchronous
  `renderFirstPerson` at 0x4c5338. No intervening translation/scale was found.
- View Bobbing OFF bypasses both bobView and native turn sway, so no capture is
  expected. Fallback uses the current top but **still applies C and root inertia**.
- These are evidence addresses only, never runtime hook installation addresses.

The corrected draw order is:

```cpp
auto selectedBase = handBase.captured.value_or(currentTop);
auto transform = model::modelProjectionTransform(nativeProjection, fov, depthMode)
    * glm::mat4_cast(cameraRotation) * residual * placement;
// Assigned only after push(false):
world.mat->_m = selectedBase * visual->transform;
```

Thus **base · projection correction · H · C · placement**. The initial task's
base · C · gunTransform order was corrected after finding the Java HEAD injection:
`ItemInHandRendererMixin.java:34–36` posts the camera event before vanilla lag;
`GunItemRendererWrapper.java:144–156` appends H; 185–206 appends inverse lag and
then placement. H and C do not commute. The synthetic nonzero-H diagonal test
checks H·C·placement and rejects C·H·placement. Existing model projection correction
stays left of H; its FOV/depth relationship is unchanged. Debug freeze retains C
inside the frozen common transform too.

### Axis/sign mapping and placement evidence

The basis mapping for C is **identity**, with no sign reversal or conjugation:

1. Java `Axis.XP/YP.rotationDegrees` multiplies degrees by .017453292 and builds
   JOML positive-axis `rotationX/rotationY`. Local jar javap confirms this (below).
   Both use right-handed active rotations and postmultiplication of column vectors.
2. Installed GLM 1.0.1 `glm/ext/matrix_transform.inl:17–45` constructs the same
   positive-axis rotation and returns m·R. In particular Rx(θ) sends +Y toward +Z,
   and Ry(θ) sends +Z toward +X. D3D's depth convention does not reverse these axes.
3. `GunRenderer.cpp:753–756` records the established hand-pass world top as the
   inverse view rotation/camera basis. Multiplication by the native view therefore
   returns the captured local axes to view axes (+X right, +Y up, -Z forward).
   SDK `mc/deps/renderer/Camera.h:15–21` separates view/world/projection stacks and
   right/up/forward; research follows that same world stack from capture to draw.
4. Our `src/model/Aiming.cpp:81–86` is Java's wrapper `T(0,1.5,0)·Rz(180°)` followed
   by positioning `T(0,1.5,0)·blend(idle,iron)·T(0,-1.5,0)` from wrapper 201–206 and
   `FirstPersonRenderGunEvent.java:196–210`. Rest positioning inverse at
   `src/model/GeoModel.cpp:276–289` matches Java's reverse path, X/Y/Z inverse
   rotations and root -1.5 correction (`FirstPersonRenderGunEvent.java:217–235`).
   This places the model conversion **after H and C**, not around C.
5. `src/model/GeoModel.cpp:258–273` uses the same positive-axis rest Rz·Ry·Rx,
   animation quaternion and scale order as Java `BedrockPart.java:82–95`.
   Its `(1,-1,1)` applies to **animation translation only**: hence root +sx Y in
   BonePose becomes Java's -sx offsetY. It is not a hand-base reflection.
6. Actor pitch/yaw signs are used directly, as in `ClientRecoil.cpp:126–127`.
   `docs/ADS-CAMERA-0.3.15.md:27–37` records the matching Java/Bedrock view-ray
   signs. The recoil adapter's native-input pitch inversion is not applicable to C.

The existing constraint path receives the effective root before applying its
weighted transform to placement (`GunRenderer.cpp:761–762`). Keyframed shoot/reload
animation is in the immutable source pose; inertia follows it. Recoil still changes
the actor look via the existing recoil adapter, not another placement rotation.
This release does not add Java's separate procedural jump/shot-root sway or change
the existing camera, recoil, ADS, FOV, hurt-bob or attachment implementations.

## Java bytecode and typed lifecycle inputs

Read-only local check, not a guess from Java method names:

```text
javap -c -p -classpath <Gradle mcp 1.20.1 joined/rename/output.jar>
  net.minecraft.world.entity.LivingEntity net.minecraft.world.entity.player.Player
  com.mojang.math.Axis
```

Jar: `%USERPROFILE%/.gradle/caches/forge_gradle/mcp_repo/de/oceanlabs/mcp/
mcp_config/1.20.1-20230612.114412/joined/rename/output.jar`. Official 1.20.1
`client_mappings.txt` and the cached renamed jar identify the source methods.
Scratch dump: `%LOCALAPPDATA%/Temp/opencode/tacz-analysis/lifecycle-axis.javap.txt`.

| Method / source | Bytecode evidence |
| --- | --- |
| `LivingEntity.aiStep` (`m_8107_`, source 2621–2754) | At 372 calls `isImmobile` (`m_6107_`); true branch 378–393 skips to 434. `serverAiStep` (`m_6140_`) is invoked at 419 only in the non-immobile branch. Dump lines 7008–7030. |
| `LivingEntity.isImmobile` (source 2017) | Offsets 0–4 return `isDeadOrDying` (`m_21224_`). Dump 5053–5057. |
| `Player.isImmobile` (`m_6107_`) | Calls superclass at 1; if false, calls `isSleeping` (`m_5803_`) at 8; returns their OR at 19. Dump 12175–12186. **Death/sleep skip confirmed.** |
| `LocalPlayer.serverAiStep`, source 653–664 | Controlled-camera check 5–8; previous copies 44–57; current pitch half-step 60–79 and body-yaw half-step 82–101. `LocalPlayer.javap.txt:2045–2110`. |
| `LocalPlayer.isControlledCamera`, source 667 | Compares Minecraft's camera entity to this player, offsets 1–8. Same dump 2115–2125. |
| `LivingEntity.getViewYRot`, source 2925–2928 | Current head yaw at alpha=1, otherwise ordinary lerp of previous/current head yaw. `Entity.javap.txt:24337–24355`. LocalPlayer uses this only while riding. |
| `Axis.rotationDegrees` and positive X/Y implementations | Degrees conversion at 2–5; positive `rotationY` and `rotationX` (no fneg) at 8. Lifecycle dump 15663–15670,15701–15727. |

Typed Bedrock inputs (installed LeviLamina 26.51.9 headers, relative to `include/`):

- Actor pitch `.x`, yaw `.z` from `Actor::getRotation()`, never camera matrices.
- Alive/dead and `Player::isSleeping()` gates freeze bob samples; yaw observation/
  common-rebase housekeeping can continue without advancing or copying bob.
  Dead→alive on the same player reinitializes; sleep→awake does not reset.
- Passenger is `Actor::getVehicle()`. Const `EntityContext` lookup of
  `mc/entity/components/ActorHeadRotationComponent.h:9–10` supplies `mYHeadRotO`
  and `mYHeadRot` for ordinary alpha lerp. Bob ticks still use body yaw, like Java.
- `mc/client/game/IClientInstance.h:304` supplies `getCameraEntity()`;
  `WeakEntityRef::lock()` yields an optional EntityContext. Registry identity plus
  entity ID is compared to the local player's context. No guessed camera tag/offset
  or unconditional local-player ticking fallback is used.
- Mutex-protected value publication separates fixed ticks from render sampling.
  World token/client/player identities reject stale snapshots. Exit/disable clear
  state; world/player change or dead→alive initializes it. No pointers are retained
  for later native dereferencing.

## Deliberate differences and remaining uncertainty

- **2a:** Java's bob fields start at zero. We initialize all four to current view
  on world entry, player replacement and respawn: no opening sway. Documented in code.
- Yaw is continuous through shortest signed observation deltas, doubles internally,
  with common 360° rebase of view and both bob samples. Never wrap view-minus-bob.
  Java directly subtracts floats; Bedrock angle normalization is not guaranteed.
  More than 180° between observations is inherently ambiguous. No teleport or
  distance heuristic resets the transient; ordinary teleport rotations retain it.
- Passenger head source and controlled-camera check are implemented, not missing
  SDK features. Live availability/meaning of a riding player's head component and
  custom camera-entity ownership still require acceptance. If a head component is
  absent during a native transition, sampling safely uses body yaw; no fabricated data.
- ClientLevelTickEvent phase vs. Java serverAiStep is not proven. Actor look can
  lag camera input by a frame (existing recoil evidence). Tick/frame diagnostics
  expose this; synthetic tests do not establish timing parity or GPU consumption.
- Capture excludes other mods' transforms appended between bobView and our hand
  hook. Native hurt shake already in the base is retained, not repaired. Default,
  RTX, Deferred, custom-camera and F1 paths need runtime checks. Capture failure
  deliberately falls back to the current top, with rejection counted, rather than
  reusing an unrelated native matrix. No model/path/namespace special cases.

## Validation and acceptance

- Server x64 Release (`xmake f -a x64 -m release -p windows --target_type=server -y`,
  `xmake build tacz`): **build ok**, 25.672 s.
- Client x64 Release (corresponding client configuration/build): **build ok**,
  27.719 s; client configuration left active. Existing SDK/LL hook warnings remain.
- `xmake build tacz-tests`: **build ok**; `xmake run tacz-tests`:
  **235 cases / 921,801 assertions, Status: SUCCESS**, zero failures/skips.
  Added **12 cases / 677 assertions** over 0.4.3. The missing-root fixture updates
  both the bone name and name lookup; the named-root-only assertion is retained.
- Version 0.4.4 is synchronized across xmake, tooth, both manifests and manifest
  checks. Changelog records all-passed 0.4.3 acceptance and removal of its diagnostic;
  roadmap records turn inertia as implemented but awaiting live acceptance.
- `git diff --check` passed; all 19 changed files verified UTF-8 without BOM.
- No game launch, deployment or push. Native capture consumption and timing are
  not established by these offline results.

Synthetic fixtures cover half-step/copy order, view-minus-interpolated-bob, signs,
tanh, quaternion/rest/scale order, immutable repeated composition, named-root
absence, H/C order, single-axis/diagonal C, 2a, frozen bob, riding head input,
yaw boundaries/large finite values/rebasing/nonfinite retirement, capture-key
rejection and effective-pose agreement across CPU/rigid/constraint/sight/arm paths.
They do not hard-code expected in-game output.

Temporary diagnostic (six decimals for angles, cumulative counts per world,
at most one line/second; quiet after settling unless capture state changes):

```text
[turn-inertia] world=<id> tickPitch=<p> tickYaw=<continuous-y> dx=<dx> dy=<dy> sx=<sx> sy=<sy> residualDeg=<C-angle> capture=hit hits=<n> fallbacks=<n> rejections=<n>
[turn-inertia] world=<id> tickPitch=<p> tickYaw=<continuous-y> dx=<dx> dy=<dy> sx=<sx> sy=<sy> residualDeg=<C-angle> capture=fallback hits=<n> fallbacks=<n> rejections=<n>
[turn-inertia] world=<id> tickPitch=<p> tickYaw=<continuous-y> dx=<dx> dy=<dy> sx=<sx> sy=<sy> residualDeg=<C-angle> capture=rejected hits=<n> fallbacks=<n> rejections=<n>
```

These are exact field formats, **not claimed runtime samples**. No log from a
0.4.4 game run exists in this implementation session.

1. Main menu ≥10 s, enter, exit with held/put-away gun, re-enter A→B→A. No crash,
   stale matrix or spawn/respawn opening sway; toggle F1 and third person.
2. With View Bobbing ON, settle the gun and make horizontal, vertical and diagonal
   quick turns. Expect `capture=hit` with rising hits, no ordinary-path rejections.
   Compare muzzle/body-side pivot motion with Java; stop input and observe smooth
   settling. Single-axis C is near zero; diagonal C may be nonzero by design.
3. Repeat with View Bobbing OFF: `capture=fallback` is expected, root inertia and
   C remain present. Single-axis inertia should not disappear with this option.
   World walk bob, vanilla items and hurt bob retain their previous behaviour.
4. Repeat hip/ADS, walking/sprint, firing/inspect/reload and gun/put-away switches.
   Camera animation must combine H before C; arms remain attached and constraints
   see the moving root. Compare rigid path 1 and CPU path 0 via existing
   `/tacz rdebug path 1` / `path 0`, then restore path 1; no stale revision-only sway.
5. Cross ±180° yaw repeatedly, make several full turns, teleport while turning,
   die/respawn, sleep/wake, and ride while turning the head. No boundary spike,
   teleport reset, dead/sleep bob decay or respawn transient. Test a non-player
   camera entity separately: bob must not tick while detached.
6. Repeat default/RTX/Deferred where available, ADS with model FOV and camera
   shake ON/OFF. Capture counters are call-site evidence only; inspect actual image.
   Check 0.4.3 sprint-fire/semi-auto/release regression and absence of `[sprint-server]`.

## Changed files

- Pure logic: `src/model/ViewTurnInertia.h`, `src/model/ViewTurnInertia.cpp`.
- Client adapter/capture: `src-client/client/animation/ViewTurnInertia.h`,
  `src-client/client/animation/ViewTurnInertia.cpp`,
  `src-client/client/render/HandTurnCapture.h`,
  `src-client/client/render/HandTurnCapture.cpp`.
- Integration: `src-client/client/gameplay/ClientShooter.cpp`,
  `src-client/client/render/CameraBones.cpp`, `src-client/client/render/GunRenderer.cpp`;
  diagnostic removal: `src-server/server/Shooter.cpp`.
- Tests: `tests/ViewTurnInertiaTests.cpp`, `tests/ItemTooltipTests.cpp`.
- Release/docs: `xmake.lua`, `tooth.json`, `packs/behavior_packs/tacz_bp/manifest.json`,
  `packs/resource_packs/tacz_rp/manifest.json`, `CHANGELOG.md`, `docs/ROADMAP.md`, this document.

## 0.4.5 acceptance

- User-tested 0.4.4: fast horizontal, vertical and diagonal turns basically match
  Java with View Bobbing ON; OFF behaves the same. ADS, firing, reload and put-away
  while turning keep arms aligned with the gun. No opening sway on world entry or
  respawn; riding, third person, main menu and world re-entry are normal.
- `F:\Games\Minecraft\LeviLauncher\versions\1.26.51.01-LeviLamina\logs\2026-10-29.4.log`:
  1,203 `[turn-inertia]` lines, 1,130 `capture=hit` (ON), 73 `capture=fallback`
  (OFF), `rejections=0` throughout; no `[tacz]` errors or
  "First-person gun render failed". This acceptance supersedes the pending status
  above for these user-tested paths; unreported backend/custom-camera checks are
  not claimed as tested.
- 0.4.5 removes the temporary diagnostic, its counters, sample/capture-only fields
  and residual-angle helper. Capture validation, fallback and inertia are unchanged;
  residual matrix finiteness and composition behaviour remain tested.
- 0.4.5 offline validation: server/client x64 Release **build ok** (27.390 s /
  31.500 s); tests **235 cases / 921,815 assertions, Status: SUCCESS**. Client
  configuration remains active; no push, deployment or game launch.

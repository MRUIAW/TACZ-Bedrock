# 0.3.7 animation evidence and game checks

## Walk/sprint findings

Source paths below are relative to `_reference/TACZ/src/main/java/com/tacz/guns/`.

| Question | Evidence / conclusion |
| --- | --- |
| Distance or real-time loop? | `resources/tacz/default_state_machine.lua:412,489` seeks normalized phase to `(getWalkDist() % 2) / 2` each update; anchors at 396,470,477,484. One cycle per two Java walk units. |
| Java distance? | Vanilla 1.20.1 `net/minecraft/world/entity/Entity.java:457` saves walkDistO; line 734 adds `(float) vec3.horizontalDistance() * 0.6F`. Verified inside the local Forge mapped sources JAR under `_reference/TACZ/build/fg_cache/net/minecraftforge/forge/1.20.1-47.3.19_mapped_parchment_2023.08.20-1.20.1/`. |
| Java partial ticks? | `client/animation/statemachine/GunAnimationStateContext.java:327-341`: anchor and value both use **current + delta * partialTicks**, not previous-to-current interpolation. |
| Old Bedrock input? | HEAD `191aac9`, `src-client/client/animation/GunAnimation.cpp:113` directly used `ActorMovement::getPredictedWalkDist(entity, partialTicks)`, with no Java-unit conversion or validation of equivalence. It was a getter, not a per-frame distance accumulator. Its SDK declaration does not establish its scale. |
| Tick/frame doubling? | HEAD `191aac9`, `ClientShooter.cpp:280,422-424` dispatched pending client ticks, while GunAnimation.cpp:219 updated per frame. No extra per-frame movement-trigger loop was found. |
| Controller's post-seek advancement? | Java `api/client/animation/ObjectAnimationRunner.java:227-267` advances progress when running, even after a seek; `api/client/animation/statemachine/AnimationStateMachine.java:52-56` updates the controller after states. The next script update overwrites progress. Kept this behavior, tested with real Lua at three frame rates. |

**Confirmed defect:** a Java-distance API was being supplied by an unvalidated native
animation-distance API. The new path explicitly implements the verified Java definition
in `src/model/WalkDistance.h`, sampled by `captureMovementTick()` and consumed by
`movementTick()` in `src-client/client/animation/GunAnimation.cpp`.

**Still unknown:** the exact cause/proportion of the visually faster cycles has not been
measured in Bedrock. In particular, do not describe the native getter as raw blocks, assume
an exact 5/3 ratio, or attribute a speed difference to Bedrock without logs. The old getter
is retained **only for diagnostics**. Actual position sampling preserves genuine speed
differences rather than adding a compensating speed multiplier.

For steady 20 Hz movement, speed `v` blocks/s gives `0.03*v` Java units/tick and `0.3*v`
cycles/s. Examples (not game measurements):

| Speed | Java delta/tick | Cycles/s |
| --- | --- | --- |
| 4.317 | 0.12951 | 1.2951 |
| 5.612 | 0.16836 | 1.6836 |

`[walk-rate]` logs speed, actual sampled tick count, Java/native deltas, `distancePhase`,
and `controllerRate` (normalized target-track progress after controller update, with wraps
unwrapped using displacement). `sampled` reports seconds of stable walk/run track samples;
zero is normal during draw, run_start or cross-fades. Log comparisons should use steady
flat-ground movement for several seconds, no shooting/jumping, and a high `sampled` value.
No existing movement diagnostic flag exists, so this low-rate line uses info logging.

## Actual TACZ 1.1.8 put-away chain

The authoritative library is locally bundled:
`_reference/TACZ/libs/simplebedrockmodel-2.2.2-forge+mc1.20.1-sources.jar`.

- Library `v1/client/handler/FirstPersonRenderHandler.java:94-120`: keep the previous instance,
  get old renderer's put-away duration, call `triggerPutAway()`. During retention, change only
  `pendingTarget`; rapid scrolling does not start another old-item instance.
- Lines 150-172: reveal/create the pending instance at a client START tick after the duration,
  and trigger draw from rendering. Lines 175-210/222-245 choose/render the retained instance
  and cancel vanilla hand rendering. Library `v1/mixin/client/ItemInHandRendererMixin.java:21-27`
  freezes vanilla equip ticking while retaining.
- TACZ `client/renderer/item/AnimateGeoItemRenderer.java:325-384` creates the instance:
  draw calls tryInit and the draw sound, put-away calls tryExit and the put-away sound.
- `client/renderer/item/GunItemRendererWrapper.java:80-94` triggers put_away, exits states and
  sets the reinitialization guard to duration +50 ms.
- `api/client/animation/statemachine/AnimationStateMachine.java:52-56,123-128`: exit clears
  states, not the controller. Controller update/render continues after exit.
- `client/gameplay/LocalPlayerDraw.java:51-54,71-76`: keep/doPutAway/scheduled draw calls are
  **commented out**. Active client timestamp math is not the actual retained-item renderer.
  TACZ's own equip-retention code in `mixin/client/ItemInHandRendererMixin.java` is also
  commented out; it is not the active library mixin above.
- `entity/shooter/LivingEntityDrawGun.java:33-48,59-76`: if elapsed time since drawTimestamp
  is nonnegative, new timestamp is `now + min(elapsed, previousPutAway)`; otherwise preserve
  the future timestamp. Cooldown is `max(newDraw - (now - timestamp) - 5, 0)`.

The port retains one VM and old ItemStack until `HandSwitch::Reveal`. Renderer snapshots
carry the **visible**, not actual selected, gun ID. Incoming vanilla rendering is skipped
even if mesh creation/rendering fails during retention. A fresh VM is created only for the
latest target at reveal; selecting the old slot again also starts one fresh draw. Client
actions use visual retention/draw locks; server actions use the authoritative Java formula.

**Explicit differences:** fixed duration +50 ms retention, frame-boundary handoff, instead of
SBM's tick-boundary duration-only retention plus a separate re-init guard. Client action lock
waits for a full new visible draw even if the Java server timestamp allows an earlier shot
(e.g. interrupting an unfinished draw). Neither inventory selection nor third-person equipped
items are delayed; only first-person visuals are retained. Native vanilla equip ticking is
not frozen, but its entire first-person rendering call is suppressed while retaining.

## Game checklist (0.3.7 not yet game-validated)

Build validation: both release variants built; all 86 tests / 21,799 assertions passed.
`scripts/deploy-client.sh` built and deployed the client DLL/PDB/manifest and packs to
`F:/Games/Minecraft/LeviLauncher/versions/1.26.51.01-LeviLamina/mods/tacz`.
The game was closed; startup/gameplay have not been tested. HEAD remains `191aac9`;
0.3.7 is intentionally uncommitted.

1. On flat ground, walk straight for 5 seconds, then sprint for 5 seconds. Compare visual rate
   to Java; save `[walk-rate]` lines. Check `javaDelta = speed*0.6/20` when ticks are about 20/s,
   and steady `controllerRate` approximately `distancePhase = speed*0.3`. Compare native delta
   against Java delta to quantify the old-input difference rather than assuming a factor.
2. Repeat at low/high FPS; stop/start, strafe, crouch, jump/land. Stationary idle and run_end
   should remain unchanged. Flying/riding/corrections are not established equivalent to Java.
3. AK47 -> empty hand and AK47 -> ordinary item: old gun must finish put-away, no incoming
   hand/item flash, then vanilla appears. Expect `[hand] put-away ... 570 ms + 50 ms guard`
   followed by one reveal about 620 ms later (plus frame quantization). Check put-away sound.
4. AK47 -> another gun: old put-away, then new draw and draw sound, no overlapping VMs.
   Hold fire / press R or H during the handoff; no old-gun shot/reload, new gun waits for draw.
5. Scroll rapidly through guns/items, including back to AK47 before its exit finishes. Expect
   one put-away and one final reveal, unchanged old timer, no intermediate draws/sound queues.
   Repeat while reloading and during an unfinished draw; verify old reload/burst stops.
6. Exit/re-enter the world during retention. No retained gun, stale sounds, stale distance
   samples or inherited action lock should survive. Also test a remote updated server:
   inventory-selection/notification ordering may differ from the integrated server.

Risks: tick-endpoint position sampling is a chord rather than the sum of all Java Entity.move
calls; teleports/server corrections can enter the distance sample. Missing pack sounds remain
silent as before. Old-state NBT is a snapshot, not a reference to the actual inventory slot.
Rendering hooks and sound handles require game testing; unit tests cannot prove visual
suppression or engine audio lifetime. Existing deferred arms/camera/attachments are unchanged.

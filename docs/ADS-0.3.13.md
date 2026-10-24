# 0.3.13 — aiming (ADS)

Baseline: **`a940642`**, approved 0.3.12 observer + diagnostics. Skin still falls back to
Steve/Alex; this patch is ADS only. The historically named `SKIN-0.3.13.md` remains a skin
investigation, not this release's objective. No camera recoil/constraint/HUD/skin/RTX fix here.

## Java TACZ evidence / mapping

- `client/gameplay/LocalPlayerAim.java:24–94` accepts aim state, sends a bool packet, progresses
  on client ticks with `(elapsedMs+1)/(max(adsTime,0)*1000)`, interpolates old/current values.
  Only put-away's future draw timestamp forces aim off; not reload or ordinary draw duration.
- `client/input/AimKey.java` and `config/client/KeyConfig.java` default to hold, support toggle,
  hold pre-input on switching, and cancel in UI/spectator. Our right-button event adapter uses
  the same interception path as the tested left-button gun input and atomics for input edges.
- `entity/shooter/LivingEntityAim.java` maintains independent authoritative progress and
  cancels sprint while aiming (also during non-finishing reload). `ClientMessagePlayerAim`
  sends only bool state. `AdsModifier` initializes cache from gun aim_time; attachment evaluation
  is not present in this port, so both sides explicitly use that unmodified base.
- `FirstPersonRenderGunEvent.java:119–235`: second-order AIMING_DYNAMICS (1.2,1.2,.5), inverse
  idle/iron sight paths and rigid translation/relative-rotation blend, bracketed by +/-1.5 Y.
  Our outer translate / Z180 coordinate convention is retained. View-node inverses are cached;
  the per-draw placement does not cause extra geometry rebuilds.
- `CameraSetupEvent.java:94–164`: actual tangent magnification FOV and hand FOV interpolation,
  separate dynamics (.5,1.2,.5). Our world hook composes with native/origin FOV; false variable
  (hand/non-variable) queries pass through, and our gun/arm pass modifies its own perspective
  projection. Depth/aspect/off-centre terms and pre-existing stack values are preserved.
- `MouseHandlerMixin.java` and `ZoomConfig.java`: MDV133 screen-distance sensitivity, base
  multiplier 1.0. Applied at the fully typed turn-delta hook, using predicted client progress
  and the last native world FOV (70 before first valid sample), not a persistent option edit.
- `ModernKineticGunScriptAPI` / `resource/pojo/data/gun/InaccuracyType.java`: only FULL progress
  gets AIM accuracy. Server ticks compute progress; the request cannot send progress or
  accuracy. Shooting and every later burst round use the server's current state.
- `LivingEntitySpeedModifier`: reload > aiming > base signed movement modifier, MULTIPLY_TOTAL.
  Our native modifier is transient and isolated by UUID. Negative raw gun fields (e.g. AK47
  aim=-.2/reload=-.15) are applied as signed total multipliers; no fake weight implementation.
  Invalid amounts become zero and values below -1 are bounded to prevent negative speed.
- `GunAnimationStateContext` bindings are real now. The default Lua machine chooses optional
  aim_start/aim/aim_end clips and its actual walk_aiming prototype; no new hardcoded animation
  selection or invented default-pack aim clips. AK47 does not supply those three optional
  clips, so the integration test supplies tiny fixtures solely to exercise the script branches.

## Native safety / lifetime

- Lamium main `src/features/camera/CameraSessions.cpp` was read. It uses the same getFov and
  _applyTurnDelta boundaries. We neither call into Lamium private state nor copy its opaque
  camera detours. Existing hook-chain origin is respected; combined zoom behavior still
  needs in-game validation.
- Compile-time signatures: `getFov` float(float,bool) const, `_applyTurnDelta` void(Vec2 const&),
  focus-loss void(), sprint setter void(bool). No placeholder type crosses these detours.
- Attribute APIs use fully typed AttributeModifier/UUID/MutableAttributeWithContext/
  AttributeModificationContext. Signature and native value-size checks accompany add/get
  and sprint paths. No raw offsets or replacement of the player's base speed.
- FOV/turn callbacks consume immutable mutex-protected snapshots and comparison-only client/
  player identities. They reject zero/stale world tokens before world queries; the FOV hook
  does not call the global client/level service. Clear/leave invalidates snapshots immediately
  through the existing world token. Focus callback only clears atomics, not native gameplay.
- ADS ticks run on the actual client tick; the frame hook updates targets and interpolated
  presentation, and the renderer only consumes snapshots. Native ItemStack is never copied
  outside the native registry scope. No retained player/client/level reference is introduced.
- Projection scope restores even if later stack allocation/draw fails; arm and gun use the
  same model FOV/placement. Missing/non-perspective projection is left unchanged.
- Movement modifier is removed on non-gun/death/spectator; hot-disable removes it from live
  players while a server state exists. World stop destroys the nonserialized modifier.

## Decisions for the user

1. Hold-to-aim by default. Optional toggle via `mods/tacz/config/aim.json`:
   `{"hold_to_aim": false}`; restart required. No UI/rebind/controller/touch in this milestone.
2. Follow Java: reload does not cancel the aim target; Lua may retreat its optional ADS clip
   while a main reload animation runs. Ordinary drawing permits aim; put-away does not.
   Hold pre-input can re-aim after the switch; toggle is reset on selection/put-away.
3. Focus/UI loss cancels safely and requires a new press instead of restoring a potentially
   stuck button. World exit/death/spectator never retains zoom or movement modifications.
4. Port Java's same discrete second-order coefficients/t=.05, ~6 ms cadence and velocity
   prediction, but integrate deterministically on the game/render frames, not 15 worker
   threads. Catch-up is bounded to .25 s, view interpolation weights to [0,1], FOV to valid
   degrees. Progress itself uses actual tick elapsed time and Java's +1 ms, independently
   on client/server; zero/negative duration snaps rather than producing NaN.
5. No attachment cache system exists yet. Use base aim_time (initial ads cache value), iron
   view/zoom/model FOV only; cachedAds override is tested in pure policy for future use.
6. Signed movement_speed values are interpreted by LivingEntitySpeedModifier's actual
   MULTIPLY_TOTAL use, including the default pack's negative aim/reload slowdown. Do not
   import MoveSpeed.of's surprising nonnegative clamp into raw unmodified gun data. No
   weight modifier; client movement follows authoritative attribute replication.

## Validation / deployment

- All **105 tests / 42,822 assertions pass**, no skipped tests. Includes default/AK47 Lua ADS
  optional clip transitions, real aiming walk, numeric progress, ads override, tangent FOV,
  MDV sensitivity, deterministic smoother/stall/reset, quaternion blend and projection
  invariants, full-progress accuracy and movement state priority.
- Final server release build passed (26.171 s), manifest 0.3.13 / server. Final client release
  build passed via deploy script (28.187 s), manifest 0.3.13 / client. Only known LL hook-macro
  unused-variable / existing test u8path deprecation warnings; no build failure.
- `git diff --check` passed. With Minecraft closed, deploy script succeeded; source/deployed
  DLL/PDB/manifest SHA-256 all match, as do all 4 resource-pack and 2 behavior-pack files.
  Deployed client DLL SHA-256: `6E3EA4D1189354743670956A987424F5F17A71D57BF58605DC18339CB0A2EE7F`.
- Agent did not launch Minecraft or claim visual acceptance. Only task 1 is implemented.

## Morning acceptance

1. Confirm 0.3.13 and `[ads] ... Hook=true` startup results; wait at main menu >=10 s. No
   zoom, skin/world query crash or gameplay packets should originate from menu actions.
2. RTX off, AK47: right hold/release while idle and walking. Check smooth idle_view ->
   iron_view placement, world zoom, model FOV, reduced sensitivity and aiming walk; right
   click must not place/use/activate the held gun's underlying vanilla item/block interaction.
3. Fire hip vs partially vs fully aimed: server AIM spread begins only at full progress.
   Test semi/auto/burst, crouch and motion; ammunition/reload/fire selection remain functional.
4. Hold ADS during reload and draw; start sprint then aim (sprint stops), try sprinting while
   aimed. Check reload-speed priority and normal speed restoration after switching to a
   nongun, dying/respawning or spectator. Preserve external speed effects.
5. Hold right through gun -> other gun / item -> gun and fast scroll: outgoing put-away cannot
   aim, hold can pre-input the next gun. Toggle mode (restart config) must reset on switch.
6. Open inventory/chat/pause and alt-tab while aiming, release there; close/refocus. No stuck
   aim/sensitivity. Quit holding/mid-put-away, A -> B -> A and menu >=10 s: no stale zoom,
   speed, skin/GPU resource or sound; no crash/pink texture.
7. Repeat relevant ADS checks in Deferred/RTX and with Lamium zoom disabled/enabled. Internal
   gun animation is still RTX-static pending 0.3.18; ADS placement is a per-draw matrix, not
   proof that dynamic gun geometry now refreshes. Record FOV/aspect/sight alignment problems.

Main risks: native runtime hook/attribute behavior is not covered by unit tests; FOV hook
chaining, Bedrock speed replication and sight alignment must be visually confirmed. Imported
skin/Persona support remains unchanged (default fallback); no full-body/armor/attachment aim.

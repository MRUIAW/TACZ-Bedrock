# 0.3.14 camera recoil

Parent ADS commit: `e7475b3`. Only overnight task 2 is implemented; no camera/constraint bone,
HUD, skin or RTX-geometry work. No game launch or new user runtime result is claimed.

## TACZ source evidence

- `resource/pojo/data/gun/GunRecoil.java:58–73`: Commons Math 3 **SplineInterpolator** produces
  a natural cubic PolynomialSplineFunction, not the animation system's Catmull-Rom. Append
  the initial `(0,0)`, move each key's time to `float(time*1000+30)` ms, sample its value as
  `(min + Math.random()*(max-min))*modifier` per key/per shot.
- `GunRecoilKeyFrame` stores float time and [min,max]. `CommonGunIndex.checkRecoil:77–99`
  validates ranges/nonnegative times and sorts both arrays before interpolation. This port
  sorts a local copy, preserving loaded pack data and matching the sampling order.
- Commons Math 3.6.1 SplineInterpolator was read: at least 3 knots, strictly increasing times,
  natural boundary conditions (zero second derivatives), piecewise cubic evaluation over
  the inclusive knot domain. The C++ implementation solves the equivalent tridiagonal
  second-derivative system in double precision. No library or Java code is bundled.
- `client/gameplay/LocalPlayerShoot.java:240–312`: first successful client pull sends one
  packet; each scheduled burst round posts a client GunFireEvent before animation/audio.
  This is **client prediction/acceptance**, not a server acknowledgement. The new recoil event
  follows our same accepted-shot and due burst-round paths; dry fire/blocked requests do not
  start curves. Each curve starts at actual event time, not a rate-compensated packet clock.
- `CameraSetupEvent.java:167–205`: every GunFireEvent replaces both spline functions, sets
  the shot timestamp and zeros xRotO/yRotO. No concurrent recoil-curve pool or additive tails.
  Modifier is `1-aim+aim/min(sqrt(zoom),1.5)` times crawl and evaluated attachment multipliers.
- `CameraSetupEvent.java:209–234`: while a curve's isValidPoint accepts elapsed ms, subtract
  `value-previousValue` from the player's pitch/yaw and retain that previous sample. No separate
  recovery/spring/return-to-origin exists. A new shot does not undo an old shot's applied turn.
  A missed final knot is not sampled forcibly after the domain expires.

### AK47 reference

`data/tacz/data/guns/ak47_data.json:266–278`:

- crawl_recoil_multiplier=.5 (stored, but inactive because crawl is not implemented).
- pitch source: `(0,.66),(.1,.66),(.45,-.175),(.6,0)`, deterministic ranges.
  Interpolation knots: **0/30/130/480/630 ms**, values 0/.66/.66/-.175/0.
- yaw source: `(0,[-.23,.18]),(.35,0)`; knots **0/30/380 ms**, new random first value per shot.
- Example hip-fire pitch values from independent rational arithmetic using Commons Math's
  mu/z/b/c/d formulas: 15 ms=.35940649046504, 80 ms=.96221794728820,
  200 ms=.23258411657559, 330 ms=-.15695868979910, 555 ms=-.09816305620609.
  Natural spline overshoot above .66 is intentional; clamping it would change TACZ behavior.
- +30 ms moves the first key away from the origin to smooth the initial rise; it does **not**
  mean do nothing for 30 ms. The first frame may already have a nonzero sample.

## Bedrock adapter / sensitivity / networking reasoning

1. The existing MinecraftGame::update hook calls native origin, client shooting logic,
   `updateCameraRecoil()`, then animation publication. Recoil is applied once per game frame,
   not once per tick or mesh draw. Server and client builds share only the pure spline code;
   only the client invokes native look turns.
2. Before any global lookup, `updateCameraRecoil` checks the active world token, then the
   recorded joined-client identity. The adapter validates the exact world/client/player,
   selected-gun snapshot, HUD/alive/non-spectator state again before turning. No ItemStack
   copy, raw entity-component write, retained native owner or main-menu service query is added.
3. Call the installed native `LocalPlayer::_applyTurnDelta(Vec2 const&)`, which is also the
   turn boundary used by Lamium CameraSessions. Compile-time exact member-function signature
   checks cover this call and both MinecraftGame focus thunks (void()). Arguments are a
   fully defined Vec2 by const reference, not a placeholder-by-value ABI.
4. Actual look delta is `(-deltaPitchCurve, -deltaYawCurve)` in degrees; positive curve pitch
   means kick upward. The native look path handles camera/body rotation and pitch limits.
   Unlike a render matrix edit, it is intended to persist in the local player's look state.
5. Scoped TLS contains the exact player only around that native call, restoring on exceptions.
   0.3.13's MDV sensitivity hook sees that marker and forwards origin(delta) unchanged. Mouse
   turns still follow the usual MDV133 path. ADS recoil reduction has already been applied
   when sampling the curve and is not multiplied by MDV a second time.
6. Bedrock's `PlayerAuthInputPacketPayload.h:101–109` exposes mRot and head/interaction/camera
   angles. The ordinary native look/input pipeline is the intended source of those fields on
   subsequent input ticks. No hand-render-only offset is substituted, no server rotation
   setter fights the client's turn, and no rotation/Input packet is fabricated or overwritten.
7. This is code-path/protocol reasoning, **not a measured guarantee against correction**.
   Native turn units/camera ownership, outgoing input propagation and server persistence must
   be confirmed with the runtime traces. `getRotation()` is read before/after turns and again
   on later applied frames; the server independently logs actual rotation at accepted shots.
   User input, pitch clamps, teleport/corrections and Lamium freecam can also change these
   values. A normal next-frame input tick is expected to carry the changed look; comparing
   the logs and sustained-fire direction will detect snapping rather than assert it away.

## Lifecycle / diagnostics

- Clear curves on selection/put-away, UI/alive/spectator mismatch and world invalidation.
  App focus callbacks touch atomics only; the game frame consumes cancellation. No old tail
  resumes after refocus or rejoin. Pending scheduled burst recoil follows existing selection
  cancellation; if several rounds become due in one frame, the final event replaces earlier
  ones before the camera sample, like queued Java GunFireEvents.
- Clearing curves does NOT reverse previously applied real rotation. Per-world counters and
  comparison-only identity scalars retain no client/player/world resource owner.
- Missing pitch/yaw is normal. Too few knots (one source key), duplicate/nonfinite times,
  invalid ranges or nonfinite coefficients disable only that axis with a once-per-gun/reason
  warning; no invented linear curve. Limit 4096 keys/axis and refuse nonfinite native float
  turn values. This is safer than allowing a Commons Math exception into the native frame.
- `[recoil-shot]`: first 8 shots per world, actual event sequence, gun, triggering packet
  timestamp (shared by one burst group), ADS/zoom/modifier, shifted endpoints, sampled first
  values and whether it replaced an active curve.
- `[recoil-look]`: first 12 applied frames per world, requested pitch/yaw delta, actual local
  rotation before/after, previous observed result and MDV bypass marker. If the first
  meaningful turn leaves the actor unchanged away from a pitch limit, warn for investigation.
- `[recoil-server-look]`: first 8 accepted pulls per player/server session, matching triggerTs
  and actual authoritative player rotation. This is read-only evidence, not server recoil.
- Disable logs counts. Traces are bounded; to capture a fresh hip/ADS comparison, rejoin a
  world and perform those early shots before a long automatic spray. No runtime trace yet.

## Decisions for the user

1. Match TACZ overlap literally: replace the old curves, reset samples, retain already applied
   look. Do not sum full recovery tails, add a damped spring or return to a saved crosshair.
2. Preserve Java's inclusive-domain-only evaluation. Low FPS or a stalled frame that skips
   the endpoint may leave a small residual return; no non-TACZ forced-final-sample fix.
3. Use the predicted client-accepted shot, including burst rounds, not wait for server ACK
   or apply server-side recoil twice. Rejected local/dry/blocked attempts have no recoil.
4. Use current interpolated ADS progress and iron_zoom at each event. Attachment/crawl
   multipliers are neutral **1**, even though AK47's future crawl setting is .5. Scope and
   attachment recoil cache evaluation remain deferred, not guessed from unrelated data.
5. Use ordinary native local turn, with only our own ADS mouse-scaling bypass. Do not bypass
   another mod's hook chain or assume detached-camera ownership. Test Lamium zoom/freecam;
   a verified dedicated camera/body integration can be scheduled if those paths conflict.
6. Cancel pending curves on UI/focus/lifecycle changes for safety; Java's static camera curve
   may otherwise continue after an item switch. No off-screen jump or stale-world recoil.
7. Seeded mt19937 in tests and per-process runtime entropy give repeatable tests/uniform range
   sampling, not bit-for-bit Java Math.random sequences. The distribution/formulas match.

## Validation

- **113 tests / 44,106 assertions pass** (none skipped). New tests: independent natural-cubic
  values and real AK47 table, seeded per-shot range/reproducibility, sorting/+30ms, domain/
  malformed axis behavior, ADS/crawl/attachment modifier formula, frame-delta telescoping,
  skipped endpoint/no synthetic recovery, three overlapping shots and missing-curve replacement.
- Final server release build passed (26.391 s), generated manifest 0.3.14 / server. Final
  client release build passed through the deploy script (27.594 s), manifest 0.3.14 / client.
  Only the known LL hook-macro unused-variable warnings; no build failure.
- `git diff --check` passed. Minecraft was closed; deploy script succeeded. Source/deployed
  DLL/PDB/manifest SHA-256 all match; all 4 resource-pack and 2 behavior-pack files match.
  Deployed DLL SHA-256: `8BE3AA1C61084CEAB62C7FA433549453C8068206BC9BB97B96BB0A078CE6BE2A`.
- No in-game recoil acceptance run. Native behavior and network persistence remain explicit
  risks; do not interpret build success or diagnostic labels as a measured runtime result.

## Morning test sequence

1. Main menu >=10 s, no world: no recoil rotation/query/crash. Confirm 0.3.14 and focus-hook
   startup status. Enter world with RTX off; do not move the mouse for the initial samples.
2. AK47 semi hip-fire, one shot: actual view rises, small randomized yaw; the data's own return
   follows over ~630 ms pitch / ~380 ms yaw. Observe possible spline overshoot and compare
   requested/before/after `[recoil-look]`; not just gun-model animation.
3. Repeat early in ADS (zoom 1.2): modifier should be ~0.912871, without another MDV reduction.
   Manual mouse compensation should work concurrently; ordinary aim sensitivity remains slowed.
4. Automatic sustained fire: repeated replacement, climbing view; release and let only the
   final curve finish. Test a burst gun for one recoil event per due round. Dry fire/reload/
   draw-blocked trigger should not create accepted recoil events.
5. Compare `[recoil-shot] triggerTs` and `[recoil-server-look]` on successive shots; bullets
   should follow the changed aim and view should not snap back at the next input/server tick.
   Test integrated and remote 0.3.14 server if available; do not infer replication from visuals alone.
6. Test near pitch limits, long/stalled frames, inventory/pause/chat, alt-tab and refocus;
   switch gun/item mid-tail, quit holding/mid-put-away, A -> B -> A, menu >=10 s. No NaN,
   replayed kick, menu motion or stale-world callback. Rejoining should reset trace budgets.
7. Repeat the critical view/ADS/exit checks in Deferred/RTX; then Lamium zoom/freecam separately.
   Internal gun animation under RTX remains the existing issue, not a recoil acceptance claim.

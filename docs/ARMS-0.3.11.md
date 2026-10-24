# 0.3.11 first-person arms

Follow-up (2026-10-25): user verified arm animation for idle/walk/shoot/reload/inspect/put-away,
including RTX-on arms while the gun remains static; no crash reported. Both Persona and
imported classic skins incorrectly showed defaults. Approved 0.3.11 was committed as
`ce8c25b`. The implementation/validation-pending notes below describe that historical release;
the replacement skin path and current acceptance checks are in `SKIN-0.3.12.md`, and the new
RTX evidence is in `RTX-0.3.10.md`. Camera/constraint remain deferred beyond this skin patch.

## Reference behavior

Studied the local TACZ source, not just gun-pack placeholder cubes:

- `client/model/BedrockGunModel.java:76-79` attaches functional LeftHandRender/RightHandRender
  to `lefthand_pos` / `righthand_pos`. Animated `lefthand` / `righthand` are ordinary ancestors,
  so all rest and animation matrices on the path matter, including root and attachment node.
- `FunctionalBedrockPart.java:54-75` first transforms the functional node, then invokes its
  renderer instead of its placeholder cubes/children. That renderer takes precedence over
  the node's own visible flag; a hidden ancestor still prevents traversal.
- Both `functional/{Left,Right}HandRender.java:23-38` check first-person + renderHand, append
  **Z+180 degrees**, capture the matrices, and delegate until after gun rendering. There is
  no extra negative X scale for the left hand and no hand-specific compensating translation.
- `util/RenderHelper.java:73-89` chooses Java PlayerRenderer.renderLeftHand/renderRightHand
  using the actual local player, hence the player's skin/model and matching sleeve part.
- `mixin/client/PlayerModelMixin.java:33-57` clears both arms' XYZ rotations for the gun's
  first-person setupAnim(age=0) and copies them to sleeves. It does not zero their pivots.
- Checked TACZ's cached decompiled **1.20.1** source in
  `build/fg_cache/mcp/.../decompile/output.jar`: PlayerRenderer's SRG `m_117775_` calls neutral
  setupAnim, then arm entitySolid and sleeve entityTranslucent. HumanoidModel's non-crouching
  setupAnim sets both arm pivots to x=+/-5,y=2,z=0 (including slim, whose mesh creation initially
  has y=2.5). PlayerModel replaces the mirrored old left arm with a separate unmirrored island.
  Wide right local box (-3,-2,-2), left (-1,-2,-2); slim right (-2,-2,-2), left (-1,-2,-2).
  Dimensions 4/3 x 12 x 4, sleeve inflate 0.25; UV right (40,16)/(40,32), left (32,48)/(48,48).
  PlayerRenderer's 0.9375 whole-body scale is NOT called by renderHand.
- `GunItemRendererWrapper.java:201-227` shares positioning with the gun, disables hand rendering
  for the refit view, and resets animation afterwards. No refit view exists in this port yet.
  `FirstPersonRenderGunEvent` adds positioning/aim/constraint to that same stack; it is not
  another independent arm motion. The existing Bedrock replacement hook suppresses vanilla's
  whole first-person pass while a valid gun snapshot owns it, including put-away.

These functional arm renderers live in TACZ, not SimpleBedrockModel; vanilla player geometry
is reached through RenderHelper. No extra library arm transform is needed.

## Bedrock implementation and safe skin acquisition

`src/model/PlayerArms.{h,cpp}` generates vanilla neutral boxes using the existing box unwrap.
The part pivots are baked once into those boxes; each draw applies:

```text
existing first-person world * gun positioning
    * root rest/animation * ... * hand-parent rest/animation * hand_pos rest/animation
    * Rz(pi) * cached vanilla arm/sleeve vertices
```

Paths are cached in GunVisual. No per-frame model traversal of all 605 AK47 bones or arm mesh
rebake. Invalid/missing paths, hidden ancestors and collapsed/nonfinite transforms skip the
corresponding arm. A pure integration test matches slim arm bounds against AK47's original
placeholder geometry, before and after root/hand animation. This tests placement mathematics,
not rendered pixels.

Searched LL 26.51.7's 26.51.1 client headers for arm/hand/player/skin routes. ItemInHandRenderer
exposes the whole first-person pass and item draws, not an isolated pair of Java-like hand
draw functions. The actor route is data-driven: ActorRenderData needs live animation/Molang
references, skin model materials and actor render state; no verified arbitrary-matrix,
independent-arm invocation was found. It is therefore not used as a speculative shortcut.

`src-client/client/render/PlayerArms.cpp` instead reads:

```text
LocalPlayer::mSkin -> SerializedSkinRef::mSkinImpl (shared ThreadOwner wrapper)
    -> [check initialized thread id == current thread, BEFORE reading mObject]
    -> SerializedSkinImpl::mSkinImage / mArmSizeType / mDefaultGeometryName / persona/animation flags
```

The SDK fully defines ThreadOwner, SerializedSkinImpl, SkinImage (mce::Image), image format and
Blob. Only validated RGBA8, standard humanoid geometry, square 64/128 images of exactly w*h*4
bytes are copied. The shared owner exists only during that read; no skin/player/registry-bound
native object is retained. A different/uninitialized owner thread is **not bypassed**: it uses
the default fallback. Native thread/layout behavior still requires runtime verification.

Every second while a gun with arm nodes is being drawn, probe the skin and compare a private
pixel hash + layout/dimensions/fallback state. This also detects in-place image changes, not
just pointer/ID changes. Skin updates may take up to one second; there is no player-update hook.
No skin IDs, image bytes or personal texture contents are logged.

Persona has separate pieces/tints/geometry and may have an atlas that is not a vanilla arm
layout. Persona, animated atlas skins, nonstandard geometry, 64x32 and inaccessible skin owners
fall back to the installed game's `data/skin_packs/vanilla/{steve,alex}.png`. Select Alex when
a safely read arm-size identifies slim, otherwise Steve. Path is resolved relative to the
running executable, not this development install or a guessed launcher profile. No Mojang
image is copied into the repository or distribution. Missing default image logs once and
defers arms instead of displaying an old user's skin.

Skin upload follows the existing gun texture path: owned RGBA -> mce::Image -> cg::ImageBuffer
-> original TextureGroup::uploadTexture -> TexturePtr, under unique Raw world/resource/skin
serial names. The private copy forces only base-arm islands opaque (Java's entitySolid
treatment), preserving sleeve alpha. Four meshes each have 24 vertices; base uses the known
moving-block alpha-test material, sleeves its blend layer. This is not Java's exact material/
lighting pipeline; fractional-alpha ordering and material readiness are explicit runtime risks.
Only sleeves, not chest jacket/body/cape/armor, are drawn.

All work is inside GunRenderer's existing lock after nonzero world + matching pose generation
and RenderResourceKey readiness checks. World/pipeline/owner changes and request-leave clear
arms alongside guns. Release drops meshes/texture and unloads the Raw name using its original
weak TextureGroup; it never queries a new global client. The process-exit leak pattern matches
the existing gun cache, avoiding destructors after the renderer has gone. If an arm draw
throws after a gun was submitted, the native first-person pass is not drawn over it.

**No new hooks.** Existing compile-time checks remain for MinecraftGame::$update,
ClientInstance::_requestLeaveGameImpl and ItemInHandRenderer::renderFirstPerson. No opaque
container/handle is guessed, copied or forwarded by a new detour.

## Camera / constraint: deferred to 0.3.12

Studied TACZ CameraSetupEvent, CameraRotateListener, ConstraintObject/RotateListener and
FirstPersonRenderGunEvent's constraint application. Camera rotation must affect the view and
the hand-stack consistently, without becoming actual player aim/rotation; constraint's
counter-transform is multiplied by aim weight, which is currently absent.

Reviewed Lamium's `features/camera/CameraSessions.cpp` and `DetachedCameraRig.cpp`. Its
LevelRendererPlayer::setupCamera hook has safe reference/scalar arguments
`void(mce::Camera&,float)` (confirmed in the installed header at line 405). But its notes say
post-setup edits did not reach detached render consumption, and it uses camera ECS state to
coordinate entity/culling/render ownership. A reference-safe hook alone does not establish a
correct consumed-camera contract. No camera hook/ECS mutation/player rotation write is added
just to squeeze this into arms. Existing camera/constraint channel exclusions remain unchanged.
RTX static-gun investigation is parked; `RTX-0.3.10.md` remains its historical record.

## Performance / verification

- Cached arm animation: two short matrix chains and up to four draws/frame. Skin hashing/copy
  is 1 Hz; four meshes (96 vertices total) build only on skin/layout/resource changes or an
  invalid mesh, never once per gun pose. Gun CPU baking is unchanged.
- Pure release test: AK47 two cached paths about **0.00029-0.00040 ms/frame** over 10,000 updates.
  This excludes Minecraft, skin read/hash, upload, material setup, draw submission and GPU.
- `[arm-perf]` after 120 frames per skin/resource scope reports CPU totalMean/max, probeMean,
  buildUploadTotal, matrixDrawMean, builds and draw returns, with RTX state. The build/upload
  total is a subset of probe work, not an additive timing. First-frame setup is included.
- No game launch/test was performed by the agent; **in-game timing and appearance are pending**.
  Do not substitute the pure matrix benchmark or the old ~4ms gun bake for actual arm timing.
- Pure tests: 97 cases / 42,607 assertions pass, including 1.20.1 neutral dimensions/pivots,
  UVs/winding/normals, scale/order/ancestor visibility and classic-skin admission/alpha/hash.
- Server and client release builds succeeded. `scripts/deploy-client.sh` deployed **0.3.11**
  with Minecraft closed; DLL/PDB/manifest SHA-256 match the final client output. No commit:
  HEAD remains `1ee753c`; the earlier RTX/crash follow-up notes are retained in this round.

## User regression sequence (RTX off)

1. Confirm 0.3.11, wait in the main menu >=10 seconds: no crash or arm allocations.
2. Use default Steve/wide: inspect both hands and sleeve edges, verify no duplicate vanilla arm.
   Idle, walk, shoot, reload and inspect; outgoing arms must follow put-away until handoff.
3. Test a custom classic skin with distinctive left/right hand and sleeve colors; wait at least
   one second after changing skin. Repeat with Alex/slim (3-wide) if available. Check skin,
   orientation, left/right UVs and no shoulder-offset/stretch error. Persona should explicitly
   log source=default/reason=persona, not pretend to support its composed geometry.
4. Collect `[arm-gpu]` and `[arm-perf]` after at least 120 gun-rendered frames. Stable skin
   should show four builds, not one rebuild per animation frame; at most four arm draw returns
   per frame. Check baseValid/sleeveValid and that skin source is local-skin when appropriate.
5. Gun -> hand/item/gun and rapid scroll. Quit mid-put-away, quit while holding, A -> B -> A;
   require fresh arm resources/skin on rejoin, no pink texture, stale sound or lock.
6. Return to menu >=10 seconds. Arm allocations/probes/draws must stop; verify release logs.

RTX is deliberately not an acceptance target for this round. Existing static-pose behavior
is unresolved, and matrix-based arms do not imply that the RTX gun backend is fixed.

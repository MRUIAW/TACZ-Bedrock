# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versioning rule for this project: the patch number goes up by 1 for each small item or test
round, the minor number goes up by 1 for each finished feature or phase.

## [0.4.1] - 2026-10-29

### Fixed — gun sway with View Bobbing ON

- Suppress native hand-pass `LevelRendererPlayer::bobView` while the published
  displayed item is a gun in the active world, including put-away animation,
  matching Java TACZ. This call bobs the world matrix our gun is drawn on;
  level/world bob remains native. No camera-mode or successful-draw gate.
- Remove the temporary log-only bob research hooks and draw diagnostics.
  Hurt bob (`bobHurt`), camera setup, Camera Shake/camera-bone gating and
  GunHurtBobTweak remain untouched.
- Awaiting in-game acceptance for the sway fix and native bob regression checks.

## [0.4.0] - 2026-10-28

Phases 1–3 complete. No code changes; version metadata and roadmap only.

### Confirmed — 0.3.34 user acceptance

- All 7 in-game items pass: pack override sounds, 5.7x28 icon, coloured ammo
  titles, TWX reload script and the new stack limits match Java.
- Item names follow the game language after switching (several guns checked).
- B93R second magazine during reload: the user sees no difference from Java;
  the known issue is closed.
- The 7.62x39 "copper shell" name seen in Java belongs to a different ammo,
  `tacz:762x39copper` (ClassicRCCRP, used by classicr mk47, white title). The
  default `tacz:762x39` is `§e7.62x39mm 步枪弹` (yellow) in its own lang file; both
  resolve correctly.

### Changed — roadmap phase ownership

- Phase 1 (AK47 single-player) done. Other players hearing gunfire (`shoot_3p`
  broadcast) moves to phase 8 (server).
- Phase 2 (multi-pack) done. Guns that depend on unimplemented mechanics are
  handed to phase 4.
- Phase 3 (items) done. Held/dropped item appearance merges into phase 7
  third-person rendering (same world-space renderer).
- Fire volume stays 0.8 (Java local 0.64); the user hears no practical difference.

## [0.3.34] - 2026-10-28

### Confirmed — 0.3.33 user acceptance

- Container shift quick-move now shows the native flying icon for both guns and
  ammo. Rejoining the world keeps per-gun ammo counts (17+1 and empty0, two guns).
  Gun NBT immediate update and gun drop pass regression after the 0.3.32 balanced write.

### Changed — gun-pack same-path override order (Java alignment)

- Keep the native directory enumeration order of installed packs; only the
  pack-list sort was removed (in-pack file lists stay sorted). The user's Java
  instance confirms the same order C→D→T→R→M. Server and client share one loader.
- Add `ResourceLookup` {Direct, Listed, ListedStack, ResourceStack} and route each
  consumer by its Java category: sounds, gun/ammo display, data index and Lua use
  the last pack (Listed); geo models, Bedrock animations, PNG textures and
  gunpack_info use the first pack (Direct). A selected file that fails to open or
  parse never silently falls back to another pack. No pack/path/namespace special cases.
- Language files merge per key within each namespace through the Forge
  ResourceStack order. Cross-namespace duplicate keys follow a documented port
  policy (Java leaves this to HashMap iteration): the namespace named by the key
  prefix owns the key; otherwise the lexicographically last namespace wins.
  Reproduces the user's four Java names, e.g. `§e5.45x39mm 步枪弹`, `§b8mm 毛瑟 步枪弹`.
- Ammo tooltip titles keep the translation's own § colour (5.7x28 is orange,
  matching Java); gun titles and custom names are unchanged. The pack footer still
  resolves from the AmmoId namespace.
- Intentional behaviour changes from the newly selected files: 5.7x28 stack
  limit60→50 and 7.92x57 36→30 (5.45x39 stays60); existing over-limit stacks are
  not truncated, migrated or rewritten, only new merges/splits/give use the new limit.
  `tacz:xmag_reload_logic` now resolves to TWX's script (18 default-pack guns,
  102 registered guns in total). Audible source changes: ak47_shoot, hk416d_shoot,
  hk416d_reload_end and two scar_h inspect sounds now come from ClassicRCCRP;
  the 5.7x28 slot icon follows ClassicRCCRP's display to the classicr PNG.

### Added — pack resolution audit tool

- `xmake run tacz-pack-resolve <gunpacks> <out.json> [--reload-script id]` loads
  the production registry and reports every duplicate path with all four lookups,
  the consumer's choice, source pack, byte size and FNV-1a-64, plus derived ammo
  limits/icons/tooltips and language sources. Installed packs: 429 duplicate paths,
  100 with different content; all 7 Java evidence items pass
  (`docs/PACK-OVERRIDE-0.3.34.md` §14, CSV alongside). A Lua VM probe ran the TWX
  script for 102 guns × 6 reload/cancel scenarios with no warnings (not in-game acceptance).
- Rule tests use synthetic fixture packs only. In-game listening/icon/reload
  regression for the new DLL is pending.

## [0.3.33] - 2026-10-28

### Upgraded — LeviLamina 26.51.9

- Pin the SDK and both tooth LL dependencies to26.51.9; upstream selects
  bedrockdata v26.51.1-server.12/client.12. All existing hook signature/layout
  assertions pass unchanged. The upgrade was built/tested and committed separately.
- Note ClientSkin/ClientSkinSystem/ActorResourceDefinition availability for the
  later skin stage; do not use new skin APIs yet. Keep the descriptor bridge:
  ItemDescriptor copy/move constructor thunks and Item::buildDescriptor are still absent.

### Fixed — container quick-move flying icon ABI

- Native caller0x55689c7→callee0x5568a30 in1.26.51.01 passes **7** arguments:
  context/client/FlyingItem/x/y/scale/int zOrder. Both SDK versions incorrectly
  declare an extra transparency before scale. The old hook read zOrder23 as a
  subnormal float (~3.2e-44), drew16*scale≈0, and suppressed origin.
- Use a proven7-argument detour AND trampoline, taking only the SDK symbol's
  address through a corrected function-pointer type. Preserve the SDK declaration
  assertion and separately assert the native detour type. No RVA-installed hooks.
- Reuse native quintic-eased x/y/scale unchanged;16*scale slot size and constant
  alpha1 match the native flat-item call. Invalid/nonfinite coordinates or scale
  outside[0.1,16], missing identity/texture or failed draw fall back to native.
  No PNG/blend experiments or changes to normal/cursor/tooltip icon routes.
- Add regression tests for scaled slot size and rejected parameters, including
  the exact integer23-as-float failure. Player inventory without native flying
  animation remains expected. New in-game animation/layering checks are pending.

### Confirmed — 0.3.32 user acceptance

- Both scenarios pass: full inventory + ground-ammo reload, and no-ground
  fire30 then reload with immediate backpack decrement. Keep the accepted gun
  balanced write, real-time gun-slot packets and native ammo pickup unchanged.

## [0.3.32] - 2026-10-28

### 0.3.31 user results / corrected root cause

- No-ground reload now works: fire30, reload, backpack60→30 immediately. Full
  inventory + ground ammo still alternates between a stale reduced count and60;
  ground consumption is always correct. Log `2026-10-28.3.log`,02:03:54–02:03:59.
- The missing refill is NOT an mLastSlots baseline problem. First extraction
  sends60→35 and the client applies it. The subsequent gun NBT rewrite (slot2,
  count1→1,forceBalanced=false) leaves a pending transaction; pickup35→59→60
  remains pending too. Own-menu slotChanged is suppressed and cannot rescue it.
- Native disassembly: transaction groups include item/aux/NBT, so old-gun−1 and
  new-gun+1 do not cancel. Pickup's matching ammo +N/−N cancels only its own group;
  remaining gun groups block the normal transaction send, then server tick cleanup
  discards it. The next reload starts from server60/client35, leading to native
  mismatch recovery/full content60. See `docs/ITEMS-0.3.32.md` for RVAs/log lines.

### Fixed — balance the server gun rewrite, preserve native pickup

- writeSelectedGun now uses native Inventory::setItemWithForceBalance(...,true),
  rather than setSelectedItem's ordinary setItem(false). Keep the existing explicit
  single-slot gun packet and matching full-stack baseline commit for immediate NBT
  display/drop. No hand-written net IDs, transaction clearing or gameplay changes.
- Retain0.3.31's accepted extraction path. Do NOT add an after-take absolute-slot
  correction: it would mask the display symptom while leaving the poisoned native
  transaction. Native pickup/add/canAdd/survival remainder/creative discard remain
  authoritative; the existing take hook is diagnostic-only.
- Add pending nonzero/gun/ammo group counts to capped action diagnostics; split gun
  samples into their own budget. Determine server/client action labels from the
  actual ServerPlayer component, not isClientSide alone in an integrated session.
- Tests cover the shared native balanced-write call and a clearly labelled balance
  model reproducing NBT1→1 blocking ammo pickup, including consecutive refills.
  Both builds/pure tests do not constitute new in-game acceptance.

## [0.3.31] - 2026-10-28

### Fixed / awaiting in-game acceptance — inventory synchronization boundary

- Preserve the user-verified gun NBT single-slot InventorySlotPacket (immediate
  firing count and gun drop work). Pair explicit sends with a full ItemStack copy
  into only that InventoryMenu::mLastSlots entry, including the native net ID.
  Never initialize an absent baseline by copying unrelated, unsent slots.
- Ammo extraction now uses native setItemWithForceBalance(slot, remaining, true)
  and sends/commits the changed slot immediately. A normal unbalanced mutation
  must not coalesce with the subsequent pickup; sending nothing was user-tested
  and failed even without the ground refill. No full resend or tick correction.
- Give-ammo uses the same balanced mutation and tracked per-slot sends, retaining
  splitting/overflow drop. Give-gun keeps native Inventory::add but removes
  addAndRefresh's full resend; sends/commits only newly occupied slots. No separate
  inventory refund writer exists in the current script API; reload extraction and
  gun writes all pass through these boundaries. No changes to native pickup,
  creative overflow, chest/hopper rules or bridge enforcement.
- Static evidence (1.26.51.01): broadcastChanges compares counts/stacks and copies
  base stack + net ID after slotChanged; ServerPlayer::slotChanged explicitly skips
  its own InventoryMenu. Thus "removeItem notifies listeners, so it must send" and
  "just broadcast own menu" are invalid. Native inventoryChanged/addAction also
  distinguishes forceBalanced. A stale-net-ID rejection or packet-order race has
  NOT been established; final consume/refill packet traces still require gameplay.
- Add opt-in `/tacz rdebug ammosync 0|1` (default off, 256 samples per category,
  repeat 1 to rearm): action/balance, outgoing slot/content/transaction, native
  pickup, vanilla ranged control and client received/applied counts. Diagnostic
  hooks are read-only, session-guarded and signature-checked against 26.51.7.

### Reverted — unnecessary icon alpha experiments

- User confirmed affected cyber_armorer PNG backgrounds are genuinely opaque:
  the alleged black transparency bug was a misread, not an established blend bug.
- Remove icon debug command/state/preRender bypass; remove PackSlotMaterial,
  sprite flush selection/SlotIconMaterial, alpha sampling/helper/test and the
  AlphaUsage ordering change. Restore ui_textured_and_glcolor for all UI images.
- Keep verified continuous dirty redraw, owner/clone re-resolution, cursor/flying
  icons, one flat pass, Default batch and native fallback. No PNG or gunpack edits.

### 0.3.30 user results and correction of earlier conclusions

- User accepted survival partial pickup, creative full-inventory pickup,
  chest/hopper/drag/split and hotbar/inventory/chest/hopper icons after reopening,
  including drag preview (latest supplied log: `2026-10-27.9.log`).
- **Retract the earlier 0.3.29 reload-on-ground "fixed" claim.** Full inventory +
  60 rounds: consume 4 to 56, server picks up 4 to 60, client stays 56. The stack
  limit bridge fixed container capacity, not this consume/sync boundary. Vanilla
  bow/arrows work in the equivalent control experiment. No-send extraction later
  also failed: 60 minus 30 still displayed 60 until interaction/rejoin.
- This release's engine behavior and transparency revert still await in-game
  regression; pure slot-commit/budget tests do not prove native network acceptance.

## [0.3.30] - 2026-10-27

### Part 3 — lean enforcing bridge

- Keep descriptor identity/clone/constructor invalidation, Item/aux/thread/session/
  TTL checks and AmmoId enforcement unchanged. Remove runtime caller maps, histograms,
  return-address/PE-image collection, per-second summary/bridge and exit total dumps.
  Retain one install line and one world-exit error-count line (`stats=off`).
- Resolve the enforcement limit under one cache lock; steady-state world tick
  returns immediately after Item registration. Historical statistics pure fixtures
  move to tests only, not production code. Add explicit validated-limit/native-fallback
  regression tests. No stack policy, creative behaviour or bridge lifetime changes.

### Part 2 — continuous sprite icons and cursor/flying previews

- Keep pack slots dirty on update rather than caching an empty immediate-draw mesh;
  draw on each render using cached renderer/owner/world/base/aux identity. Refresh
  on render if the owner/cache changed. Native clone copies only value sidecar data
  and must resolve its own owner before painting. Unknown identity/failed paint
  falls back native; no blanket placeholder hiding.
- Use one flat sprite pass/material for known slot PNGs, not native item/glint
  passes. The installed vanilla sprite material uses source-over destination alpha
  (OneMinusSrcAlpha rather than DestColor). Preserve RGBA8 and set alpha usage after
  raw-image replacement; log transparent/partial/opaque pixel counts. HUD and tooltip
  keep their existing material selection. Real alpha/persistent screen pixels need testing.
- Cursor-specific inventory_selected_icon controls resolve the controller's active
  cursor SlotData, not the hovered slot. FlyingItemRenderer::_renderGuiIcon uses
  its full FlyingItem ItemStack and native coordinates/scale; missing identity stays
  native. Held/dropped3D appearance remains out of scope.

### Part 1 — vanilla inventory and pickup, bridge limits

- User-tested0.3.29: chest shift-click both ways (including53+60), hoppers accepted.
  The planner's pickup-during-reload acceptance was a misread (corrected in0.3.31).
  Creative full-inventory pickup was
  blocked by the obsolete canAdd adapter (native=true, adapter=false).
- Remove Inventory add/canAdd adapters and the Player::take observation hook;
  vanilla owns capacity prediction, mutation, remainder/sync, creative discard
  and sound. No A2 or replacement per-path patch. Keep /tacz give splitting and
  overflow drops; its command-only insertion helper no longer intercepts native operations.
- Remove redundant outer ItemStackBase max hook: native0x247df40 calls
  getDescriptor (0x247df6f), then the bridge-covered Item virtual getter (+0x320).
- Retain only marked legacy TaczAutoName userdata compatibility; unmarked ammo
  delegates native, real custom names/lore/other NBT remain significant. Otherwise
  old generated-name stacks would not merge with new stacks. Registered tacz:ammo
  limits now come only from the descriptor bridge; legacy vanilla-base identity
  reads remain supported but are outside that bridge's item filter.
- Native-only pickup/creative regression still needs new in-game acceptance.

## [0.3.29] - 2026-10-27

### Changed — A1 descriptor bridge enforces the AmmoId stack limit

- 0.3.28 measurement (log `2026-10-27.6.log`): every `Item::getMaxStackSize(descriptor)` /
  `ComponentItem` read for `tacz:ammo` resolved through the bridge — 20 caller/thread groups
  incl. `ItemStackBase::set`, `isStackable`, `isFullStack`, `FillingContainer` slot search and
  hopper/container/UI/network sites, 0 unresolved, 0 errors; native always 64, AmmoId limit 60.
- The inner getters now return the bridge's AmmoId limit when a validated candidate (same Item,
  aux and thread, within 250 ms) exists; any unresolved read keeps the native value. One switch
  (`EnforceLimit` in `src/nbt/StackLimitProbe.cpp`). Logs print `enforce=true`.
- Expected to fix with the same cause: chest shift-click loss (60+49 / 4 left behind), shift-click
  back to the inventory, hopper jamming once a target stack reaches the pack limit, client/server
  desync when reloading while standing on ground ammo with a full inventory.
  **0.3.31 correction:** the last scenario was never fixed by this bridge; earlier
  reports calling it fixed were incorrect. See the consume/sync investigation above.
- Not changed yet: the older per-path ammo hooks (`Inventory::add/canAdd`, `hasSameUserData`,
  `ItemStackBase::getMaxStackSize`, observe-only `Player::take`) stay until the user test confirms
  the bridge; the probe logging/overhead remains for that test. Icon drawing (empty after
  opening screens, black transparency, drag preview placeholder) is unchanged.

### 0.3.28 user results

- Icons: identity resolution works (`findCollectionIndex` gives `hotbar_items[0..8]`, rendered
  logs on hud/inventory/chest/hopper), but icons vanish after opening a screen, sometimes in the
  hotbar too; transparent pixels show black; the drag preview still shows the placeholder;
  held/dropped ammo still uses the placeholder (3D ammo models are later work).
- Pickup/chest bugs unchanged (expected: observe only). New: hopper stops filling a new slot
  once the target stack is at 60.

## [0.3.28] - 2026-10-27

### Part 2 — descriptor stack-limit probe (observe only)

- Observe ItemStackBase::getDescriptor and Item/ComponentItem max-stack getters.
  Bridge mImpl BaseDescriptor allocation + thread to validated AmmoId/pack limit;
  keep moves on the same allocation, propagate observed InternalItemDescriptor
  clones/descriptor assignments, invalidate observed Item/name constructors.
- This is a bounded250ms, per-world candidate cache, NOT a lifetime proof. Unseen
  constructors/destruction/address reuse and inlined/cross-thread copies remain
  limits. Never dereference cached pointers or change descriptors/Item fields.
- Every inner getter returns its native result unchanged. Once-per-second caller
  RVA/thread summaries record native/would histograms, candidate resolution and
  misses/expiry; world exit prints totals and clears state. Existing AmmoId stack
  getter and custom inventory behaviour remain unchanged; the three ammo bugs are
  not fixed by measurement. Current26.51.7 SDK signatures/sizes checked; older
  26.20.7 source used only to identify candidates, not to copy layouts/signatures.

### 0.3.27 user results / Part 1 — scalar slot index

- Creative pickup matches vanilla; survival scripted pickup accepted. Icons remain
  placeholders: `.5.log` finds correct ancestor collection names, but every callback
  index is1188008220 (0x46CF9F9C), not a slot. Treat the function_ref int as unusable;
  collect names only and use signature-checked UIControl::findCollectionIndex.
  Validate names and reject pointer-like/out-of-range indices; preserve ambiguity
  rejection and world cleanup. Log each distinct scalar index with its source.
- New ammo failures remain unfixed in this measurement release: pickup during
  shooting/reload can leave client counts stale until Q/rejoin; chest→inventory
  shift-click can fail with matching ammo already present; chest53 + inventory60+60
  shift transfer yields chest60+49, inventory60+4, consistent with mixed64/60 limits.
  Do not claim full A1 coverage or change gameplay to hide these failures.

## [0.3.27] - 2026-10-27

### Part 2 — item icon ancestor slot lookup

- Finish the staged ancestor lookup as a separate0.3.27 commit. Walk the renderer
  owner's parent chain (bounded/cycle checked) and resolve the nearest valid slot
  collection through the container controller or local HUD inventory. Preserve
  base/aux validation, ambiguity rejection, owner/world checks and cleanup; never
  borrow a sibling/hovered slot or an outer grid after the nearest scope fails.
- Log ancestor control names, collection/index, selected depth/identity and the
  existing resolve/source/rendered evidence for a few samples per screen. Mojang
  templates bind collections on grid/item scopes; the exact live ancestor names
  and successful icon pixels still need the new runtime logs, not just compilation.

### Part 1 — keep vanilla creative pickup

- User-tested0.3.26 `9966c25`: survival partial pickup and persisted ground remainder
  are correct. Partial pickup has no sound in vanilla Bedrock either: leave it unchanged.
- Per user decision, remove Actor::isCreative hook, its take scope and suppression
  helper. Creative intentionally discards overflow like vanilla; do not preserve it
  artificially. Keep survival's false-with-remainder add contract and native actor
  transaction, with no A2 splitting or post-take correction.
- Keep raw/effective post-take counts, removal/pending-removal state and conservation
  diagnostics. Log actual adds without the canAdd sampling cap. Creative discard
  is separately reported (conserved may be false as intended; accounted includes
  intentional discard). No game-mode, sound or installed-gunpack changes.

## [0.3.26] - 2026-10-27

### Part 1 — partial pickup data loss

- User-tested0.3.25 `f7bd259`: tooltip ammo correct, no observed stacks above60;
  icons remain placeholders. Partial pickup fills a stack but deletes ground overflow,
  including after rejoin. The bool-only fix was insufficient.
- `.1.log`04:45:01/04:45:07 explicitly records add returning false with15/7 left,
  followed by take=true. Missing later add logs were caused by the40-sample cap, not
  evidence of a different overload. Native take's false branch writes back remainder,
  then its creative-mode cleanup discards the local remainder and returns full success.
- Keep the original actor and native partial transaction. A synchronous, player-matched
  ammo-take scope suppresses the creative predicate only after an add leaves a remainder;
  no global game-mode change, no A2 actor split, no corrective packets or spawned copies.
  Log every add inside take, raw post-take count, removed/pending-removal state,
  effective ground count and creative-discard suppression. Full-pickup stale actor
  counts no longer masquerade as conserved ground ammo. Other A1 paths remain pending.
- Native persistence/client-sync acceptance still requires the new in-game test.
  Evidence and remaining risks: `docs/ITEMS-0.3.26.md`.

## [0.3.25] - 2026-10-27

### Interim testing release — A1 pending

- User-approved interim release: ship slot identity, numeric-tag tooltip counts,
  partial-pickup bool correction and give-overflow drops now. A1 max-stack
  unification is NOT complete and continues in the next version. Some operations
  may still stack ammo to64 instead of the pack limit (e.g.7.62x39 above60).
  This known limit mismatch is independent of the fixed partial-add true/false
  duplication root cause; native screen/pickup acceptance is still pending.

### User-tested 0.3.24 / fixes

- `d05b631`:120 ammo gives60+60 and full-stack pickup with free slots accepted.
  Hotbar/inventory icons still placeholders: `.27.log` calls both hooks but every
  render-data copy lacks identity; successful tooltip PNG draws rule out a blanket
  upload/drawImage failure. New slot-update hook resolves complete collection stacks
  through ContainerScreenController (HUD hotbar through local inventory), validates
  base/aux, and associates identity with renderer+owner+world. No category-ID reverse
  guessing or native render-copy mutation; conflicting candidates fail closed.
- Tooltip0/30 had a different cause: SDK integral tags cannot convert to double,
  and the Variant visitor silently returned0. Read concrete scalar payloads instead,
  with native conversion assertions and a pure regression. Test Java's current
  effective chamber for closed/open/manual bolt and effective capacity40→41/41;
  no imaginary chamber added to an empty chamber's max. Attachment gameplay remains
  unimplemented. Add tag-presence diagnostics; keep names live and common-white.
- Keep native line spacing: changing only Font draw padding would disagree with
  the already measured background/clipping. Tooltip ammo picture remains a separate
  drawImage after the font hook locates the ammo line, not an image glyph.
- Native disassembly disproves the provisional all-or-nothing add assumption:
  FillingContainer::add can insert partially and return false with a remainder.
  Player::take uses that false branch to commit the consumed-count transaction and
  reduce the original ground actor. Correct the old adapter's partial-success true
  return. Remove provisional A2 remainder-actor splitting; take now only observes
  actual ground-before/after and inventory gain, preserving native parameters/bool.
- A1 investigation proves descriptor-only max reads bypass the existing stack getter
  in ItemStackBase::set/isFullStack/isStackable and
  FillingContainer::_getSlotWithRemainingSpace/_addResource. The
  shared Item max getter reads mMaxStackSize (+0xa8); descriptor construction drops
  NBT. These 64/60 differences are real, but not the proven direct duplication cause.
  Hopper/container/UI/network/client-prediction paths remain incompletely classified.
  Current custom add/canAdd remain; this interim release does not claim unified limits.
- Give ammo now drops each stack's overflow through native Player::drop, continues
  all split stacks, and reports inserted+dropped; single gun overflow also drops.
  Keep base/aux/meaningful-NBT merge rules, existing UV/origin precision and installed
  gunpacks untouched. Native signatures checked; full SDK function_ref (16 bytes),
  no opaque by-value placeholder or fabricated batch. Sidecar released at world exit.
- New native GUI/partial pickup behaviour still needs in-game acceptance. Evidence,
  implementation tradeoffs and test checklist: `docs/ITEMS-0.3.25.md`.

## [0.3.24] - 2026-10-26

### User-tested 0.3.23 / corrections

- `9fc3404`: HK416D side text no longer z-fights: vertex-position precision/origin
  rebasing accepted as the fix. Preserve it and the accepted0.3.22 UV fixes without
  changing installed gunpacks. Old setgun guns and survival reserve consumption OK.
- Hotbar/inventory still showed placeholders and English names; tooltip missing.
  120 rounds gave only60, and ground ammo would not pick up despite free space.
  `.15.log` has hook-installation lines but no successful icon draw lines: previous
  implementation must not be treated as screen coverage or working stack splitting.

### Item UI and ammo

- Route pack-slot items from SharedMesh/atlas batching to native Default batching,
  allowing the typed common slot-render hook to replace the native icon. Add actual
  batch/entry/identity/upload/paint/fallback diagnostics. Hotbar/inventory/container
  are the intended shared-renderer coverage, **not newly game-verified screens**.
- Resolve full current I18n language through getFullLanguageCode(), per-key locale
  then en_us fallback; cover normal/redacted names and formatted hover text. New
  items no longer store English display.Name. Preserve real custom names, render
  common titles white, strip pack colour codes. Local pack says “步枪弹”, not the
  screenshot's different-version “铜壳弹”; no hardcoded translation replacement.
- Typed, fully-defined RedactableString hover-text hook builds dynamic Java-derived
  tooltip order, colours/HideFlags/current chamber counts and mode-adjusted stats.
  AK47 fixture checks31/31,0(MAX),9,25%,150%,-5.2%. Draw ammo slot image after native
  tooltip text when its marker matches. Cache pack display-name keys by namespace.
- Embed ten unchanged generic strings/locale from authorized TACZ source resources.
  No release jar found locally, so jar-exact provenance is not asserted. Refit Z hint
  retained as Java text only (feature not implemented); Bedrock spacing/fonts and
  #555555 instead of Java #777777 gray remain divergent. Attachments/ammo boxes,
  custom Java sync multipliers and pixel-identical layout are not implemented.
- Give explicitly splits all requested rounds; shared pure merge-first/empty-slot
  insertion plan drives native inventory add/canAdd and giving. Prediction is read-only;
  remaining source rounds preserved. Native NBT comparison ignores only marked old
  autogenerated names, retaining genuine names/lore/other tags. Base/aux/AmmoId must
  match; keep per-ammo1..64 and over-limit fallback64, no forced legacy-base conversion.
  Add native-vs-adapter pickup diagnostics. The old native rejection's exact subcause
  is **not proven by the old log**; new pickup/partial-merge acceptance is still needed.
- All new native hook signatures are compile-time checked. No fabricated batch or
  by-value opaque placeholder; world guards and world-exit image/text cache release.
  Details, evidence boundaries and acceptance checklist: `docs/ITEMS-0.3.24.md`.

### Validation / deployment

- Server and client x64 Release builds passed; **178 tests / 654,221 assertions**
  passed. Only existing SDK hook-macro/u8path warnings; expected Lua negative-test
  exceptions do not represent failures. `git diff --check` passed.
- Read-only installed-pack audit:325 loaded guns/80 ammo, all slot mappings and
  colour-free localized titles valid; give120 sums preserved at every ammo limit;
  installed AK47 Java-default tooltip values match the fixture. Existing UV/origin/
  HK precision regression audit passed. These are offline checks, not native UI proof.
- Deployment-time process checks found the game had closed. Deployed client0.3.24
  without starting/terminating it; DLL/PDB/manifest hashes match, RP11/BP5 exact
  paths and hashes match. Installed gunpacks unchanged. New screen/pickup/font/
  lifecycle acceptance remains pending; BDS work remains last in ROADMAP.

## [0.3.23] - 2026-10-26

### User-tested 0.3.22

- `0d1be46`: Timeless_50 and block_17 slide decals now correct; cz75/glock_17
  magazines, AK47 and other gun regression checks OK. Gun packs remain untouched;
  preserve the fix in this mod's upload compatibility layer. HK416D side text still z-fights.
- Read the specified `2026-10-26.12.log`: its13 mesh-enabled entries are frame
  statistics with builds=0, not packed-position diagnostics. There are no
  `[gun-packed]`/position-format values. Do not report half as runtime-confirmed.

### HK416D precision hardening

- Add per-group binary-grid origin rebasing on far-pivot/narrow-span axes, only
  if it halves worst simulated half-rounding error. Upload p-origin once, restore
  origin in per-draw bone matrix. No authored geometry/inflate changes, global
  depth bias, unsafe float-layout fabrication, extra per-frame mesh rebuild,
  named-bone folding or draw-order changes; arms retain their separate path.
- HK label origin(-.375,0,0): simulated half previously collapsed cube0/2
  .00006249547-block outward separation to0. Now it preserves .00006103515625;
  cube3 preserves -.00030517578125 vs source-.00031125546. Example .25..2-block
  first-person projections retain distinct front depth, including simulatedD24.
  **This is offline proof of precision improvement, not a captured GPU format or
  an in-game HK fix claim.** Actual backend format/HK result still needs acceptance.
- mesh1 now also inspects existing cached meshes when the setting changes;
  log layout slot8/12, origin, and actual decoded positions if raw CPU bytes remain.
  Empty raw data is reported, never forced re-encoded. No need to rebuild just
  to get the format metadata that was missing from the user's run.

### Real category items / names / icons

- BP registers `tacz:modern_kinetic_gun`(stack1), `tacz:ammo`(base64),
  `tacz:attachment`(stack1); no durability/use/charge/vanilla weapon components.
  RP has original schematic fallback PNGs and item atlas. BP/RP versions0.3.23,
  same UUIDs and explicit BP→RP dependency; attachment NBT accessors only.
- `/tacz give gun <GunId>` gives a real item with creative-tab initial NBT.
  `setgun` is now a give-gun alias, no longer overwrites the selected stack.
  `/tacz give ammo <AmmoId> [count=1]` and `/tacz ammo <count>` give real ammo,
  split by the ammo's effective stack limit; counts1..2304. Missing BP/unknown
  identity fails instead of substituting a vanilla item. Legacy GunId/AmmoId
  stacks still read/fire/reload; do not migrate or delete old items.
- Typed dual-side ItemStackBase getMaxStackSize hook returns per-AmmoId index
  limit within Bedrock1..64. twx_ldp1/twx_nl1 declare25565: explicit64 fallback
  plus warning, not support for arbitrary Java stack sizes. Native drag/merge
  paths and over-limit legacy stacks still need in-game checks.
- Client common `InventoryItemRenderer::$render` hook replaces pass0 icon using
  gun/ammo display `slot` PNG via the existing guarded UI texture cache. Full
  reference/int signature assertions; no by-value empty UIItemRenderInfo,
  RenderMetadata or function_ref hooks/shared atlas mutation. Targets hotbar,
  inventory/container slots using this renderer; per-screen rendered logs added.
  **No screen has been newly game-tested by the agent**; other UI/atlas/drag/glint
  paths may retain fallback icons. Failure/menu/outside session calls native origin.
- Load pack lang JSON (including tr-TR locale filenames), current language per-key
  →en_us→key fallback. New items store native display.Name in English so names
  survive client-hook failure. Client getHoverName hook resolves current locale
  without rewriting inventory NBT; keep real player/anvil names. Only the client
  SDK exports this name method; server writes the English fallback.
- **Not yet:** third-person/dropped real gun/item appearance (category placeholder
  expected), attachment mechanics, ammo box, per-gun creative entries, special
  UI coverage and >64 ammo stacks. Milestone1 single-player item code is landed,
  but native item/icon/name/survival/RTX acceptance is still pending, not declared done.

### Validation / scope

- Production installed325/325 gun models and reference54/54 default audited;
  all source positions roundtrip `(p-origin)+origin` exactly (max0), selected
  Timeless/block_17/AK47/cz75/glock_17 UV regression0 failures. Installed325/325
  gun and80/80 ammo slot assets exist. Offline regression script passes.
- 168 tests /203,996 assertions passed,0 failures/skips. Pure tests cover item
  NBT schema/initial state, stack caps, language fallback, slot mapping, BP behavior
  constraints, half preservation/depth examples and prior UV/animation regressions.
  No installed gunpack changed or game process started/terminated.
- Windows x64 release client/server builds and BP/RP JSON/PNG validation passed;
  `git diff --check` passed. Game closed; client0.3.23 deployed. Final DLL/PDB/
  manifest hashes match; manifest platform/version and exact RP/BP paths+hashes
  checked (11/5 files). No new in-game acceptance claimed.
  Client DLL SHA256: `4ae1147c53104f8c621968961b376038f12fd7d4a1eb2a82fcd0e05875219650`;
  server: `fd9c6bdecf8af279ad8283f32a066f42024fb447a3c72380ede5f27dea79dbf2`.
- Detailed limits, actual-log findings and tests: `docs/HK-DEPTH-0.3.23.md`,
  `docs/ITEMS-0.3.23.md`. BDS testing/data sync/server-only warning remain **last**.

## [0.3.22] - 2026-10-26

### User-tested 0.3.21

- HUD now closely matches Java; fresh closed-bolt AK47 was always correctly031,
  empty/low ammo red works; RTX toggle/menu/world-switch stability passed.
- Timeless/HK416D unchanged by depth1/2 or sampler0/1. Timeless looks like much
  of the gun texture compressed into its decal. Do not keep treating alpha/mips
  or a missing chamber as established causes.

### Fixed / proved

- Trace float32 Vertex UV → Tessellator mNextUV0/vertex → MeshData UV vector →
  default native layout field4/type4 → Mesh raw buffer. Version-pinned binary
  evidence proves **2×16-bit normalized UV**: multiply65535, ceilf, store low16,
  **no clamp**. This is not float32/half UV; default UV branch selection is proved,
  not inferred from finding an unused packer. Read-only reproducible script added.
- Timeless V `.7890625→1.005859375` packs to words `51712→383`, decoded
  `.78907454→.00584421`, interpolating backwards across most of the texture.
  This explains the user's observation without assuming material shader frac.
- Add tested gun-upload compatibility transform: split in original linear UV
  coordinates at integer repeat boundaries, then integer-rebase each piece into
  [0,1]. Timeless becomes two quads at36/37 of its original V span. Preserve
  reversal, negative/zero/multi-tile UVs, positions at original corners, normals,
  winding, original face order; use post-split native draw counts for both variants.
  In-range faces copied unchanged. Java source-model UVs and rigid groups remain
  immutable; default mesh-once/bone path, RTX selection, arms and cache safety retained.
- No unsafe float-layout/sampler fabrication or shared native state edits. Java
  default gun textures repeat, not clamp (no installed PNG mcmeta found). Native
  normal16-bit quantization remains; actual GPU sampler is not claimed captured.
- Extend production pack audit + `audit-gunpacks.py --uv-only`: installed325/325
  guns/1,180,714 source faces plus independent reference54/54 default guns.
  ClassicRCCRP4 guns/28 outside faces, default2/1223, other3 packs0; all1265
  resulting installed pieces pass production split/pack/interpolation checks.
  Worst examples: kar98 locator V3, block_17 V1.0126953125, Timeless V1.005859375;
  cz75/glock_17 have many zero-UV-area faces. Counts include hidden/locator faces,
  not1265 proven visible defects or325 in-game tests. Separate per-pack reports added.
- Offline images now show Java / broken packed endpoints / fixed split controls.
  Timeless fixed nearest crop matches Java0/7672 differing pixels (all3 HK crops
  likewise match); third-party images stay outside repository/gunpacks.

### HK416D evidence / unresolved

- Refute the proposed helper-fold divergence for the actual3 white-text faces:
  both decal `bone` and body `gun_body2` retain named bone boundaries; labels
  have no cube helper. Production prepack clip z/w:3072 comparisons against
  same-source Java-style path,0 bit mismatches. Keep Java DFS draw order.
- Java entityCutout inherits **LEQUAL**; however these authored decals have
  inflate, so they are not strictly coplanar with the body. Do not claim arbitrary
  coplanar polygons or differing named-bone chains guarantee bit-identical GPU depth.
- Stronger next candidate: native **position half** encoding can erase the
  0.001pixel outward separation on cube0/2 (localX~-.34 becomes a plane exactly
  coincident with the body). Numeric production audit + regression fixture added.
  Actual position branch is backend-capability-dependent and **not read back in
  this run**; no speculative geometry/transform rewrite. **HK white-text flicker
  remains unresolved**, and its UVs do not overflow.
- Opt-in mesh diagnostics log bounded read-only native layout/raw metadata,
  position slot size and packed UV words where CPU bytes remain available.
  No forced re-encoding/GPU readback; empty raw bytes are reported honestly.

### Validation / scope

- Windows x64 release client/server builds passed;162 tests /81,789 assertions
  passed,0 failed/skipped. Installed and reference UV audits have0 failures;
  no new in-game/GPU/RTX acceptance claimed. Existing LL hook macro warnings remain.
- Game closed; client0.3.22 deployed without starting/terminating the game.
  DLL/PDB/manifest SHA256 match final build; manifest version/platform and exact
  RP/BP file sets + hashes verified (4/2 files). `git diff --check` passed.
  Client DLL SHA256: `83083cda06e6f0b876565131fcb5addd5dd4335fa50781954d74394bc4ff944a`;
  server: `9e4d6b0f35dd17b3cc66d01f729ce234a2d85455f5c2eb69d9ef5b5f2180737c`.
- Version0.3.22; details/reproduction/risks: `docs/UV-PACKING-0.3.22.md`,
  `docs/UV-AUDIT-INSTALLED-0.3.22.md`, `docs/UV-AUDIT-DEFAULT-0.3.22.md`.
  ROADMAP retains BDS testing/data sync/server-only warning **last**.

## [0.3.21] - 2026-10-26

### User-tested 0.3.20

- `7fce9c2` crosshair and body/head/kill markers + corresponding sounds passed.
  Mixed numeric/percent ammo is correct display-driven TACZ behavior, not a HUD bug.
  Current production audit confirms 18 percent displays/guns (13 twx, 2 cyber_armorer,
  1 ccrp, 2 sfms); the planner's “14 twx” subtotal was off by one. No style override added.
- User found HUD font/proportions/version line unlike Java. Timeless_50 pattern and HK416D
  white text remain wrong; blend0/1 had no visible effect. Alpha threshold did not fix them.
- User decision: BDS testing, gun-pack data sync and server-only warning go **last**;
  ROADMAP updated. This release builds both variants but does not start BDS integration work.

### Changed / investigated

- Extract tested TACZ GUI layout math, matching current/reserve/version positions and
  independent1.5/.8/.5 font scales, 1×18 separator, 39×13 gun HUD and 10×10 fire-mode icon;
  preserve Java integer truncation for fire-mode/version placement. Bedrock scaled UI units
  map1:1 to Java GUI units, without multiplying physical GUI scale a second time.
- Use the game's retained original/default bitmap font rather than the alias's modern UI
  override; normalize digit advance to6 and use the same font metrics for measurement/draw.
  Restore grey shadowed version line: native SharedConstants Major.Minor.Patch + mod version,
  e.g. `1.26.51-0.3.21`, without launcher/build-revision suffix. No copied/custom font texture.
- Keep correct strict low-ammo/percent/bolt rules; add inventory/dummy color tests and
  read-only DummyAmmo HUD support. No heat simulation/bar, ammo-box or attachment-capacity
  gameplay added. Preserve50ms cache and creative9999, infinite∞, hud_empty/red fallback.
- 030/031: no dropped chamber found in the HUD formula. Production AK47 has closed bolt,
  initial magazine30/rawChamber=true; initialAmmo→HUD tests confirm031 (open bolt030).
  The screenshot lacks frame-specific raw NBT, so do not invent a chamber or claim its state.
  `/tacz rdebug hud` logs magazine/raw chamber/bolt/final text and font/GUI metrics for diagnosis.
- Exact installed decal UV audit: Java/native divide by **geometry description**, with zero
  inset; negative size retains reversal and JSON up/down have the same face routing.
  Timeless normalized `(0,.7890625)→(.533203125,1.005859375)`, PNG texels
  `(0,202)→(136.5,257.5)`, identical both sides. HK white text localized to bone/cubes0,2,3;
  all four-corner UVs match production-model tests. Detailed rectangles in the evidence doc.
- Add read-only `scripts/analyze-decal-sampling.py`: raw crops, Java/native UV point-repeat
  controls, clamp/linear/mip hypotheses and contact sheet. Controls match pixel-for-pixel;
  simulated filtering blurs tiny text, **not proof of the actual native sampler**.
  Third-party output images stay in approved temporary storage, not the repository/gunpacks.
- `/tacz rdebug sampler 0|1`: default0 retains established ImageBuffer upload; experimental1
  uploads a typed single-level TextureContainer, explicitly one mip/no mip dropping.
  Log requested/native metadata mip counts and texture/UV dimensions; rebuild only at resource
  boundaries, keeping immutable group meshes, bone animation, arm handling and native RTX path.

### Evidence / limits

- Java entity texture state disables blur/mips. Native actual point/linear/wrap/clamp and
  automatic mip behavior remain **unverified**: installed SDK SamplerStateDescription is an
  empty placeholder, not safe to mutate/index as a real sampler array. No unsafe shared-state
  edit, speculative UV rewrite or geometry/depth workaround. Sampler1 is not a point-filter switch.
- **Decal root cause/fix is not established.** This release excludes the UV-rectangle mismatch
  and supplies a bounded mip A/B, not a claim that Timeless/HK416D are fixed or RTX-tested.
  Exact HUD pixel appearance, bitmap fallback and the experimental upload still need in-game
  screenshot/RTX/world-switch acceptance. Prior gameplay/skin/kill-count gaps remain.
- Evidence, all rectangles, sampling-image legend and regression steps:
  `docs/HUD-SAMPLING-0.3.21.md`. No installed gunpack changed or encrypted content accessed.

### Validation / deployment

- Client/server Windows x64 release builds passed; **158 tests / 78,054 assertions** passed,
  zero failures/skips. Production offline audit325/325 loaded; four decal UV controls agree.
  `git diff --check` passed.
- Game closed; client0.3.21 deployed. DLL/PDB/manifest SHA-256 values match final build,
  manifest platform/version checked; RP/BP exact path sets + hashes match (4/2 files).
  No game process terminated; no new in-game run by the agent.
- Client DLL SHA-256: `186c19a634fa1eb40d8fba3877049bdf47dc3c87991c775fc84ffbeab6c69588`.
  Server DLL: `bbfffbec058d876f7f8d8f848345898230879968d063d91bf53e9aa0afc58df2`.

## [0.3.20] - 2026-10-26

### User-tested 0.3.19 / continuation review

- `91e4d2b` was subsequently deployed. User passed m4a1/glock_17/deagle reload magazines,
  glock/deagle put-away, b93r's removed face-level bullet, open-bolt single-number ammo,
  and ccrp:sr_3m / sfms:mgl416 / cyber_armorer:g58_dian / classicr:ak12 / twx:twx_b3.
  **No acceptance report yet** for HK416D decal, P320 inspect, `/tacz guns`, `/tacz packs`.
- Reviewed and retained the interrupted task's uncommitted alpha/HUD/crosshair/hit-feedback
  implementation. Fixed text-mode PNG embedding (all 26 images were transcoded), completed
  HUD pipeline-owner invalidation/client identity/disable guards, and matched Java's float
  percent rounding (`1/40 -> 003%`). Added byte-for-byte embedded-image regressions.

### Added / changed

- Respect Java's actual display transparency switch: `GunItemRendererWrapper.java:217–220`
  uses `entityTranslucent` only for `enable_transparency=true`, otherwise `entityCutout`.
  The installed Timeless_50 **and** HK416D omit that flag. Do not automatically blend their
  decals or claim the planner's proposed shared root cause was established.
- Transparent displays select the native moving-block blend layer, with fully opaque groups
  before other groups and stable DFS order within each phase; keep immutable rigid meshes,
  per-bone matrices, full-bright behavior and independent arm materials. Cache a cutout-mask
  texture (`alpha <26 ->0`, otherwise255, RGB unchanged) to approximate Java's .1 discard
  on the existing native alpha-test route. `/tacz rdebug blend 0|1` selects legacy/raw versus
  display-routing/cutout compatibility (default1); explicit material7 is a separate blend experiment.
- Read-only PNG/UV audit confirms Timeless pattern's tiny west face and 128 UV / 256 PNG:
  alpha0=3,899, alpha255=2,187, fractional=1,586; only10 texels at104–106. Its UV V exceeds1.
  HK416D: 4,224 per-face entries, **zero fractional-alpha footprints**. Keep ignoring
  uv_rotation (Timeless20/1,358): reference FaceItem/BedrockCubePerFace ignore it too.
- TACZ-style bottom-right ammo/reserve HUD, strict low-ammo red rule, normal/percent style,
  effective closed/manual chamber sum versus open-bolt no+1, creative9999, infinite∞,
  pack hud/hud_empty icons and embedded fire-mode icons. Inventory-fed display hides reserve;
  this does not implement inventory-fed shooting. No unimplemented heat bar.
- Replace vanilla crosshair with TACZ textures, default dot_1, client `config/hud.json`
  `crosshair_type`. ADS>.9 hides unless display forces; reload/animation-hide still win.
  Gate old yellow probe UI behind `/tacz rdebug probe 0|1` (default0).
- Server accepted-damage hit/kill packet to shooter, retaining launch GunId and server headshot
  result. Client-thread flesh_hit/head_hit/kill sounds from display; kill replaces hurt sound.
  Four-quadrant hit marker fades300ms; recent headshots red; only kills expand gap4→8.
- World/menu/client gates, bounded marker queue, cross-world sound-task cancellation and
  explicit texture release to original owners; HUD also invalidates on buffer/frame-builder/
  lighting changes. Native UI and cursor signatures checked at compile time.

### Limits / decisions

- **No new in-game acceptance run.** Native blend depth/write/sorting equivalence and RTX
  transparency are not established. Reuse the current pipeline's native layer, log selected
  alpha/depth/blend descriptors, warn on unavailable blend and explicitly fall back to cutout.
  No shared native-state mutation, speculative RTX material or geometry depth offset.
- Cutout masking before filtering only approximates Java's sampled-alpha discard; wrap/clamp,
  native cutoff and filtering remain risks. Timeless/HK416D fixes are **not claimed**.
  Stable group ordering is not per-triangle translucent sorting; legacy path0 stays diagnostic.
- Kill-count overlay: **Not yet**. Dummy ammo/ammo boxes/attachment capacity, Java version
  small print and ShoulderSurfing/refit UI are not implemented; missing HUD PNG skips rather
  than drawing a checkerboard. Prior B93R authored reload/P320 inspect/skin/gameplay gaps remain.
- Evidence, implementation divergences, user choices and regression steps:
  `docs/HUD-ALPHA-0.3.20.md`; current queue: `docs/ROADMAP.md`. No gunpack files changed/decrypted.

### Validation / deployment

- Client and server Windows x64 release builds passed. **153 tests / 77,964 assertions**
  passed, zero failures/skips (10 added cases). `git diff --check` passed.
- Game closed; deployed client0.3.20 DLL/PDB/manifest and both packs. Manifest version/platform
  checked; all three artifact SHA-256 values match; RP/BP exact path sets and hashes match
  source (4/2 files). No game process terminated; gunpacks outside deployment targets.
- Client DLL SHA-256: `a34dc01e22ade4e66b9f5506b01876875901368c58358deb8ee39683d06a3018`.
  Server DLL: `fc57901fae170d76c1eac20fa734d31a6f0b890d2819fa83691e7c4d05ce56b7`.

## [0.3.19] - 2026-10-26

### User-tested 0.3.18 / real in-game cost

- User confirmed `5aac463`: mesh-once rendering almost perfectly eliminated motion
  jitter/flicker, and internal gun animation now works under RTX; no new-path problems noticed.
- Real `[gun-perf]` N=120 CPU totals (not GPU): `2026-10-26.2.log:276/289`, raster old
  path 0 **4.116 mean / 5.038 max ms** versus path 1 **.141 / .486 ms**, 1 versus 29 gun
  submissions (both +4 arm draws). RTX path 1 `.2.log:427`: .158/.284 ms, 29+4 draws.
  Latest `.7.log:180`: RTX path 1 **.117/.363 ms**, 21+4 draws; no path 0 in that run.
  Different guns/runs are not a matched A/B. Source/scope details: `docs/MULTIGUN-0.3.19.md`.

### Fixed / changed

- Implement TACZ's functional `additional_magazine` rendering: instance the real magazine
  subtree at the keyed extra-magazine matrix, excluding its original root transform/parent
  chain (which can be zero-scaled). Keep immutable groups/shared arm frame; path 0 also
  implements this functional reference. User-reported reload cases covered with real assets:
  m4a1, glock_17, hk_mp5a5, deagle_golden, cz75, deagle, fn_fal, g36k, hk_mk23, m9a4.
- Open-bolt HUD/probe/command text no longer misreads raw HasBulletInBarrel as +1.
  Preserve TACZ creative-builder NBT (raw true even for open bolt); existing consume checks
  already ignore it. m249/aa12/minigun must show zero usable rounds when the magazine is empty.
- Glock/Deagle put-away: their shipped scripts look up undefined SWITCH_MODE_TRACK;
  sol2 previously threw on nil and forced static_idle (confirmed in user log). A missing
  optional track lookup now returns -1/no-op, allowing the original put_away clip/seek/HOLD.
- Remove the visible placeholder yellow bullet cubes, keeping the behavior entity/physics/
  hit logic. Like TACZ, normal ammo without an entity model should be invisible. Dedicated
  ammo entity models and tracers remain future work, not a generic particle substitute.
- Gson-compatible lenient JSON syntax and typed primitive coercion: array trailing commas
  become null entries; leading + numbers are tree strings coerced by numeric fields;
  string booleans follow TypeAdapters. Unquoted/single-quoted literals supported; object
  trailing commas and invalid typed values remain errors. Verified with actual Gson 2.10.1,
  including JsonElement→POJO versus streaming differences; `scripts/GsonProbe.java` retained.
- `/tacz guns [namespace]` lists sorted IDs in readable chunks to chat and log. `/tacz packs`
  also reports each pack's namespaces/gun count/missing-asset gun count. Startup aggregates
  each gun's missing model/texture/animation/script/sound resources into one `[gun-assets]` line.
- Read-only production-loader audit of installed packs: **325 indexes / 325 guns loaded**,
  80 ammo / 36 logic scripts. Previous 322 was the successful-load count, with three data
  failures; nine display failures overlapped some affected guns. All core display assets
  now parse/exist in this snapshot; missing optional sounds are separately listed.
  `classicr:tec_9` still warns about two unsupported animation values (`-` / Molang path).
  Re-runnable per-gun table/unknown fields/APIs: `docs/GUNS-0.3.19.md`; queue: `docs/ROADMAP.md`.

### Not yet / decisions

- B93R is a distinct reload case: original clips do not key additional_magazine; the normal
  magazine follows mag_and_lefthand offscreen. Needs Java frame comparison, not invented keys.
- HK416D static white-text z-fighting and P320 inspect tilt investigated but not safely fixed:
  illuminated in Java changes light only, not polygon offset; thin source faces and authored
  inspect/camera rotations are not sufficient evidence to nudge geometry or change matrices.
- Confirmed causes: minigun's default 30 = unimplemented inventory feed; AWP needs manual bolt;
  AUG needs builtin scope attachments. Heat/charging/explosion/melee/extensions are not newly
  implemented. HUD/crosshair/hit feedback, items, effects, skin and View Bobbing aim sway remain.
- Third-party loadability does not imply Arcana/gameplay compatibility. Unknown fields/APIs
  are listed, not silently simulated. Removed encrypted Vanguard pack is not accessed/decrypted.
- Decisions for the user and evidence: `docs/MULTIGUN-0.3.19.md`.

### Validation

- Both client/server Windows x64 release builds passed; **143 tests / 77,719 assertions**
  passed, zero failures/skips. Production offline audit: 325/325 data/display/geometry loaded.
  **Deployment pending: the user is playing; no deploy script run, no game process stopped,
  no loaded mod/resources overwritten.** Close the game before deploying DLL and RP/BP together.

## [0.3.18] - 2026-10-26

### User-tested 0.3.17 baseline

- Camera-bone animation correctly follows the game Camera Shake option, including live
  changes. Sprint while aiming and recovery on release passed in all user-tested cases.
- Preserve these fixes. Earlier moving-face jitter/flicker stops entirely under freeze;
  matrix-driven arms animate under RTX while the rebuilt gun does not. These are user tests.

### Changed

- Default first-person render path is now immutable bone-local meshes plus per-draw bone
  matrices. Keep named-bone animation/visibility boundaries; fold only static anonymous
  cube helpers into ordered rigid draw runs, preserving DFS quad/UV/winding order. AK47:
  605 parsed bones / 549 face-owning bones -> **43 groups, 29 default-visible gun draws**.
  No pose-baked vertices/upload on path 1; winding debug variants are cached independently.
- One cached BoneFrame drives gun parts and both arm attachments, outside the unchanged
  ADS/constraint/camera/model-FOV/put-away transform. Publish actual retained-item visibility
  state; skip existing groups for chamber/ammo/mag/sight/handguard/functional rules. Keyed
  additional-magazine visibility now reaches both paths; attachment gameplay stays deferred.
- `/tacz rdebug path 0|1` selects retained animated bake (0) versus new default (1) in an
  integrated client. Empty-name pose channels safely use path 0 with a warning. Keep old
  bake/upload benchmark; add per-second path/draw/cache/CPU diagnostics, one-time upload cost
  and 120-frame steady mean/max/prepare/draw measurements for in-game A/B comparison.
- Resource-scoped cache is keyed by display+model+texture; stable per-group native mesh
  addresses, unique runtime texture names and existing RenderResourceKey release/owner checks
  prevent per-pose reconstruction and preserve the world/pipeline/pink-texture lifecycle.
  No new native hook or opaque by-value ABI path.
- Equivalence: 35 AK47 clips plus static-idle blends, 2,310 poses / 16,574,320 vertices,
  max position error 4.76837e-7 blocks (<2e-6); nonsingular normal error 2.38419e-7; exact
  UV/order and both arm matrices. All singular/zero-scale positions remain checked; only
  undefined singular-normal comparisons excluded. Details: `docs/RENDER-0.3.18.md`.

### Evidence / Not yet

- User-run 0.3.17 native rebuild baseline: 3.788 ms mean raster / 3.862 ms RTX (120 poses).
  Latest offline pure bake .0525 ms vs new matrices .0139 ms, excluding native upload/draw/GPU.
  **New in-game CPU cost, jitter disappearance and RTX internal animation remain acceptance
  items**, not results inferred from tests. New path removes the suspected repeated upload
  mechanism and follows the user-validated arm matrix principle; GPU packing stays unknown.
- Not yet: HUD, shells/muzzle flash, real skin/Persona/native-arm experiment, attachments,
  and View Bobbing aimed-walk left/right sway. No source-geometry or depth/material workaround.

### Validation

- Client and server x64 release builds passed; all **138 tests / 73,430 assertions** passed
  (zero failures/skips). Client 0.3.18 deployed; DLL/PDB/manifest SHA-256 matched final
  artifacts, and resource/behavior packs matched exact source file maps and hashes (4/2 files).
  No new in-game acceptance run. Final client DLL SHA-256 is recorded in `docs/RENDER-0.3.18.md`.

## [0.3.17] - 2026-10-26

### User-tested 0.3.16 baseline

- Camera-bone firing shake now works. Held sprint resumes after ADS, but sprint attempts/
  flicker while ADS is held regressed. This is user testing, not an agent-run game test.
- All reported mesh jitter/flicker occurs while moving and stops under freeze, including
  the last two regions. The earlier static-z-fighting attribution is not established.
- User selected route 1 (own renderer, static model-space meshes + bone draw matrices),
  moved to **0.3.18**, plus a separate later small native-arm/real-skin experiment.

### Fixed / Changed

- Gunpack camera animation now follows the live game **Camera Shake** switch, not View
  Bobbing. Installed SDK OptionID has no named constants; current 1.26.51.01 video and
  accessibility bindings both use ID **570 / 0x23A** (read-only binary audit). Use native
  `IOptionRegistry::getIfValid` and `Option::save(vector&)` in memory; require save tag
  `camera_shake`, fail closed if unavailable. No BoolOption cast, opaque iterator, file
  polling or restart needed. Remove/migrate `camera_bone_animation` from aim.json.
- Sprint intent and consumption are now separate end-to-end: retain raw/processed native
  latches, suppress state processing plus independent RawMoveInput extraction, filter
  SprintDown/Sprinting/StartSprinting at the owned sender/subclient's mutable packet egress,
  and drop only standalone StartSprinting actions. Preserve StopSprinting/unrelated bits.
  Server rejects native sprint=true while aim/non-finishing reload policy is active instead
  of relying only on its later tick correction. Native toggle callback is confirmed to XOR
  raw SprintDown; no custom toggle/W timer or forced setSprinting(true) is introduced.
  Tests cover repeated blocked ticks, held release/resume, toggle cancellation, double-tap
  request suppression/no queued key, vanilla ineligibility and the old boundary regressions.
- Record upload-position quantization (possibly half/packed positions) as the **working
  hypothesis**, not a verified GPU format or implemented fix. SDK MeshData is float32 vec3;
  VertexLayout and VertexField/VertexFieldType are opaque/empty. No renderer rework here;
  GunRenderer's only change is the requested camera-option gate.
- Detailed current findings and test plan: `docs/CAMERA-RENDER-0.3.16.md` (0.3.17 addendum).

### Not yet / Known issues

- With View Bobbing ON, aimed walking sways the gun left/right. Recorded, **not fixed**.
- Moving gun mesh jitter remains; upload encoding needs capture/native-layout evidence.
  Route 1 rework is 0.3.18; native arm rendering/Persona feasibility is a separate experiment.

### Validation

- Server/client x64 release builds passed; **135 tests / 50,123 assertions** passed with
  no failures/skips. `git diff --check` passed. Known LL hook-macro unused-variable warnings
  remain. Deployed `0.3.17 / client`; SHA-256 for DLL/PDB/manifest and exact relative-path/
  hashes for all 4 resource-pack / 2 behavior-pack files match output/source.
- No agent-run new in-game option/sprint acceptance test; native hook paths must be tested.

## [0.3.16] - 2026-10-26

### User-tested 0.3.15 baseline

- Passed: recoil direction, ADS placement, aimed sway and aimed reload.
- Failed: camera roll/shake absent; held sprint does not resume after aim release.
- Freeze reduces flickering to two regions; depth 1/2 appeared unchanged without clipping.
  User also reports frequent small per-face gun-mesh shifts. These are user observations,
  not a new agent-run game test or proof of a GPU cause.

### Fixed / Added

- Camera: current Bob View is disabled, and 0.3.15 explicitly gated both consumers on it.
  Existing log contains only hook installation, no consumed flags. Add independent
  `aim.json` `camera_bone_animation=true`, leaving game settings untouched. Study GPL-3.0
  Do-A-Barrel-Roll-Bedrock's client-native render-camera-component timing; inject orientation
  before typed setupCamera, restore at callback-scoped prep/render exit with same-world,
  registry/entity and subsequent-writer checks. No persistent pending owner, player-look
  mutation, packets/Molang or unsafe matrix fallback. Exact signatures/entt hashes checked;
  use existing const ECS storage to avoid native component construction. Add entry/rejection/
  injection/restoration, peak-roll and native view-orientation diagnostics (not GPU proof).
- Sprint: mask raw/processed held latches only while native input state is processed, then
  restore via RAII, including exceptions. Retain sprint-state suppression during ADS/reload;
  afterward vanilla sees held input and decides eligibility. Never force sprint true.
- Opt-in `/tacz rdebug mesh <0|1>`: consecutive CPU position hashes/max deltas, same-pose
  repeat bake, Tessellator position comparison, documented native raw-data size/hash and
  actual draw clip-matrix stability/translation diagnostics. No opaque packing inference.
  Log selected versus captured depth mode to expose frozen-transform depth comparisons.
- Real AK47: 7,608 vertices, maximum absolute coordinate 1.5364 blocks, fixed pose repeated
  240 times with identical hash and zero delta; authored shoot adjacent max .0484827 blocks,
  quad-edge delta 3.57628e-7 blocks. Bake already uses small model space, not world space.
  **Live jitter root cause, downstream packing and the exact two visible cubes remain
  unconfirmed**; no speculative mesh/geometry fix or render rework is included.
- Attachable/runtime pack/Lua/functional bone/retained gun/real-skin/Persona/RTX comparison,
  Java draw-order/depth limits, decisions and regression/evidence plan:
  `docs/CAMERA-RENDER-0.3.16.md`. Recommend own mesh-once/bone-matrix route for 0.3.19;
  native player-arm hybrid is a separate future feasibility task, not an implemented fix.

### Validation

- Server and client x64 release builds passed; **130 tests / 47,989 assertions** passed,
  zero failures/skips. `git diff --check` passed. Only known LL hook-macro unused-variable
  warnings remain. Deployed `0.3.16 / client` to the configured LeviLauncher version;
  DLL/PDB/manifest SHA-256 and all 4 resource-pack / 2 behavior-pack files match output/source.
- No agent-run in-game camera/sprint/jitter/Persona/RTX acceptance test. Do not equate
  build/unit-test success with visible rendering or measured GPU behavior.

## [0.3.15] - 2026-10-25

### User-tested baseline

- 0.3.13/0.3.14: right-click interception, hold/release ADS, smooth iron-sight placement,
  world FOV zoom, lower aiming sensitivity, shooting animation, pitch clamp, switch/focus aim
  reset and world switching passed. User confirmed RTX honors whole-gun ADS placement while
  baked internal gun animation remains static (`docs/RTX-0.3.10.md`). Not an agent-run test.

### Fixed / Added

- Inverted recoil: runtime traces prove native turn negates pitch but preserves yaw, with
  next-frame camera-to-actor copy. Map TACZ actor-angle deltas to `(-pitch,yaw)` native input;
  preserve Java's yaw left/right convention. Remove misleading immediate-no-change warning;
  add per-axis tests and one diagnostic summary per settled/cancelled burst, separating
  intended/submitted/observed angle totals. Native clamps and ADS MDV bypass remain intact.
- Model-only ADS FOV: 45 was already parsed correctly for AK47, but late Camera projection
  stack edits had no established RenderDragon consumption. Use `inverse(Pnative)*Pmodel`
  in the shared draw transform instead, mathematically the same clip-space projection,
  increasing model size at 70 ->45 without changing world zoom. Keep Java's existing inverse
  sight transform; add origin clip-coordinate and optical-axis NDC diagnostics, no guessed
  hand offset. Native per-pass projection and lighting/RTX equivalence still need validation.
- `constraint`: reference translation *16, positive-degree rotation coefficients, component
  max blending and model-dependent transition recovery. Ancestor translation/rotation path
  excludes the constraint's own coefficient channels/scales; apply weighted Rx/Ry/Rz and
  direction-basis translation compensation AFTER ADS on the common gun/arm matrix.
- `camera`: quaternion (x,y,-z), log-space blend and uncapped sqrt(zoom) ADS multiplier.
  Apply visual-only pitch/yaw/roll before native Camera view-dependency calculation scoped to
  typed setupCamera; native inverse-view/axes/frustum follow. Apply matching hand quaternion;
  Bob View gates both. No player rotation/turn/input packet or persistent camera-ECS write.
  Signature checks and runtime consumer/retained-view diagnostics do not claim a GPU result.
- Aimed reload follows Java: keep aim/FOV and the ordinary tactical/empty/xmag clip; no separate
  aim-reload variant exists in AK47. The new ADS constraint compensates the hip-pose movement.
- Sprint re-entry: clear local sprint input/flags before/after native client input processing
  and block native setSprinting(true) for that exact live owner during aim/non-finishing reload.
  This fixes the request source rather than repeatedly clearing sprint only after each frame.
- Migrate aim.json preserving unrelated keys: `screen_distance_coefficient=1.33` and
  `zoom_sensitivity_base_multiplier=1.0` (clamped 0–2) in the requested MDV formula.
- Safely skip only vanilla HudCursorRenderer while ADS progress>.9 (unless show_crosshair),
  state-machine hideCrossHair or reload; custom HUD/crosshair/hit marker remains deferred.
- Z-fighting investigation: independent TACZ-formula tests for 25 suspect cover/chamber cubes
  agree on vertices/face/UV order, including rotated helper pivots and inflate. Read-only
  `scripts/analyze-ak47-geometry.py` finds source coplanar/near-coplanar overlapping faces;
  cover2 slope gaps can be ~7.54e-8 blocks. Do not edit source cubes speculatively.
- Integrated-world `/tacz rdebug freeze <0|1>` freezes the published pose/common transform;
  `/tacz rdebug depth <0|1|2>` compares native vs model-only .05/16 and .10/16 depth mappings.
  Default 0, no shared depth clear/material mutation. Log native projection/near/far and
  material descriptor depth func/write/bias for runtime comparison; GPU state remains unknown.

### Validation

- Server and client release builds passed; **126 tests / 46,949 assertions** passed with no
  skips, `git diff --check` passed. Deployed `0.3.15 / client`; DLL/PDB/manifest hashes and
  all 4 resource-pack / 2 behavior-pack files match the final output/source.
- Final build/test/deploy records, source evidence, decisions and eight-part regression plan:
  `docs/ADS-CAMERA-0.3.15.md`. No 0.3.15 game launch/acceptance result claimed.

### Not yet

- Z-fighting's exact GPU/material/depth cause or a cause-level cure. Source overlaps are proven,
  but Java stability vs Bedrock flicker is not explained conclusively. Dedicated render-pass,
  GPU capture and mesh-once/bone-matrix work are deferred to planned **0.3.19**.
- RTX baked internal animation, pixel-copy skin fix, custom HUD/hit marker, attachments/crawl,
  scopes and verified touch/controller/VR/custom camera support. Camera hook consumption,
  Lamium detached camera coexistence and model projection semantics need in-game acceptance.

## [0.3.14] - 2026-10-25

### Added

- Camera recoil following TACZ GunRecoil + CameraSetupEvent: natural cubic splines (Commons
  Math 3 SplineInterpolator's zero endpoint second derivatives), sorted keyframes, per-shot
  uniform range samples, initial (0,0), and time*1000+30 ms knots. This is neither Catmull-Rom
  nor a 30 ms dead zone. Preserve natural spline overshoot rather than clamp curve values.
- Each client-accepted shot, including scheduled burst rounds, replaces the current pitch/yaw
  curves and resets their previous samples to zero. Per frame apply negative curve-value
  differences to the actual local look through `LocalPlayer::_applyTurnDelta(Vec2 const&)`,
  after MinecraftGame::update. Already-applied rotation stays; older recovery tails are not
  summed. No added spring, automatic centering, server-forced kick or fabricated input packet.
- Snapshot ADS progress and iron_zoom per shot for `1-aim+aim/min(sqrt(zoom),1.5)`; crawl and
  attachment multipliers remain neutral 1 until those systems exist. A scoped player-specific
  thread-local marker bypasses only 0.3.13's MDV mouse sensitivity during programmatic turns;
  normal mouse motion keeps MDV scaling. Native turn/focus signatures are compile-time checked
  against the installed 26.51.1 client headers; no opaque-by-value detour.
- Guard against missing/stale world/client/player/gun, UI, death/spectator, put-away, focus loss
  and leave/disable; discard pending curves without undoing real rotation or replaying a stale
  tail. Malformed axes warn once and are disabled independently instead of throwing into the
  native frame; absent axes are normal. Bounded shot/look and server-angle traces support the
  morning check of actual view rotation, client-input propagation and any snap-back.

### Validation

- 113 tests / 44,106 assertions pass. Includes an independent AK47 Commons Math formula table,
  seeded randomization bounds/reproducibility, +30 ms/sorting, natural cubic/domain behavior,
  modifiers, timestep deltas, invalid axes, replacement overlap and no synthetic recovery.
- Final builds/deployment verification: `docs/RECOIL-0.3.14.md`. No in-game run by the agent;
  actual rotation magnitude, native clamp/turn behavior and server persistence remain runtime
  acceptance items, not facts established by compiling or reading PlayerAuthInput's fields.

### Not yet

- Attachment RecoilModifier evaluation, crawling pose detection, scopes, third-person recoil,
  touch/controller and detached-camera compatibility. The ordinary native turn path is used;
  another mod's zoom/freecam may modify or redirect it and needs compatibility testing.
- Intentional Java behavior: out-of-domain frames do not force a terminal sample; a stall
  skipping the endpoint may leave a small unapplied return. New shots discard old tails,
  so sustained fire can leave accumulated climb. No non-TACZ recovery workaround was added.
- Camera/constraint bones, HUD/crosshair/hit marker, pixel-copy skin fix and once-built gun
  rendering remain later tasks. Skin fallback and RTX static internal geometry are unchanged.

## [0.3.13] - 2026-10-25

### Added

- Desktop right-button ADS, default hold-to-aim as Java TACZ. Both mouse edges consume
  vanilla use/place while holding a gun; a consumed release stays consumed after a switch.
  `mods/tacz/config/aim.json` supports `hold_to_aim: false` for toggle (restart to apply).
  Hold pre-input resumes after switching; selection/put-away, death/spectator, UI/focus loss
  and world exit reset/cancel as appropriate. Reload is not an ADS prohibition; aim cancels
  sprint locally and on the server. Drawing itself does not block aim, matching LocalPlayerAim.
- Client/server tick-time aiming progress with `aim_time` (base `ads` cache value), +1 ms per
  real tick, clamp/reversal/zero-duration behavior and interpolated client progress. A state-only
  aim packet resolves the authoritative player's held gun; clients never choose progress,
  duration or accuracy. Full server progress alone enables AIM inaccuracy, including burst
  rounds. Expose real progress to ModernKineticGunScriptAPI's Lua view.
- Wire AnimationContext `isAiming` / `getAimingProgress`, allowing pack state machines to select
  aim_start/aim/aim_end and walk_aiming. Optional aim clips are not fabricated: bundled AK47
  lacks aim_start/aim/aim_end, but has walk_aiming; physical ADS uses idle_view -> iron_view.
- Rigid inverse-view translation/quaternion blend on the shared gun/arm draw matrix. Separate
  TACZ second-order dynamics for view placement, world FOV and hand/model FOV, evaluated without
  background worker threads. World FOV uses `2*atan(tan(base/2)/(1+(iron_zoom-1)*progress))`.
  Gun/arm projection lerps toward `zoom_model_fov`, preserving aspect/depth and restoring stacks
  on exceptions. Missing iron_view explicitly retains idle placement.
- World FOV hook at Lamium's `LevelRendererPlayer::getFov(float,bool) const` point; mouse
  sensitivity uses `_applyTurnDelta(Vec2 const&)` and TACZ default MDV133. Signature assertions
  against the installed 26.51.1 headers, primitive/reference arguments only, no opaque-by-value
  detour. Menu/stale worlds pass through before any client/player/level lookup.
- Server-side transient movement-speed modifier for movement_speed.base/aim/reload (reload
  takes precedence); no base attribute overwrite. Remove on leaving guns/death and hot-disable,
  never serialize it. Use typed native attribute/context/UUID APIs and checked signatures.

### Validation

- 105 tests / 42,822 assertions pass: progress/timing/cache override, FOV/sensitivity,
  deterministic smoother, view blend, projection invariants, full-aim accuracy and speed
  priority; real default/AK47 Lua ADS transitions with optional clip fixtures.
- Release builds/deployment and final artifact checks are recorded in `docs/ADS-0.3.13.md`.
  No in-game ADS test was run by the agent; morning runtime acceptance remains required.

### Not yet

- Scope/attachment ADS modifiers and view selection, crawl/touch/controller, configuration UI,
  weight-speed modifier and third-person synchronized aim animation. Unmodified gun aim_time /
  iron_zoom / zoom_model_fov are used, not guessed attachment behavior.
- Camera recoil (0.3.14), camera/constraint bones (0.3.15), HUD/crosshair/hit marker (0.3.16),
  pixel-copy skin fix (0.3.17) and once-built gun geometry (0.3.18) remain separate tasks.
  Skin still falls back to Steve/Alex. RTX static internal gun animation is still unresolved;
  per-draw ADS matrices are not an RTX geometry-cache fix.
- Bedrock FOV/movement replication, hook composition with Lamium and visual sight alignment
  across graphics modes need game testing. FOV smoothing uses bounded deterministic catch-up,
  clamped view weight and immediate no-world/UI reset rather than Java's unsynchronized workers.

## [0.3.12] - 2026-10-25

### Runtime follow-up and diagnostics — skin still falls back to Steve/Alex

- User tested deployed 0.3.12: arms still show default Steve; no crash reported, no other
  acceptance results supplied. `2026-10-25.4.log` proves the update hook installed at load,
  fired at least 53 times and matched the local skin. All 15 selection records fall back:
  7 expired weak texture snapshots, 8 unverified layouts with an empty native default-mesh
  field. There are zero borrowed-handle binds. This is NOT evidence of a missed enable-time
  event, nor of failed identity matching, nor a malformed 64x64 RGBA image.
- `ClientSkinInfoData::mDefaultMeshName` was wrongly treated as actual skin layout metadata:
  it is empty in every observed selection. A one-time weak texture snapshot also does not
  establish the final vanilla renderer binding. Do not relax layout checks or pin old GPU
  textures as a speculative workaround. Exact update timestamps/unique owners were not logged.
- Added diagnostic-only update events (first 128, total count at disable), snapshot identity
  addresses/shared-owner match kind, update-time texture/handle/loading state, and one actual
  draw-return/source record per selection. World attribution reads only the atomic session
  token, with no main-menu client/level service query. Selection/rendering policy is unchanged.
- No verified draw-time/current-repository skin lookup or neutral native arm-render contract
  was found in the installed SDK. Version stays 0.3.12; diagnostic draft is not deployed and
  no 0.3.13 fix is claimed. Evidence, safe-API audit and next options: `docs/SKIN-0.3.13.md`.

### Changed (observer implementation, not a completed skin fix)

- Replace the first-person arm skin's render-thread serialized-image read with a weak observer
  of native `ClientSkinInfoData::updateSkin`. Correlate its input/stored shared-owner identities
  with `LocalPlayer::mSkin`, then borrow the game's existing typed `mce::ClientTexture` handle.
  No custom-skin pixel copy/re-upload, fabricated resource location, thread-check bypass or
  dereference of `SerializedSkinImpl::mObject`. Hook/thunk signatures are compile-time checked
  against the installed 26.51.1 client headers; arguments are references/pointers only.
- Both latest 0.3.11 logs rejected all six observed arm skin creations at the old combined
  `skin-owner-thread-unavailable` check, BEFORE image inspection. The broken access boundary,
  not an established RGBA/size rejection, explains the default skins. Those logs cannot tell
  null owner, unset debug marker and different thread apart. New `[arm-skin]` diagnostics split
  these and report native input image metadata, GPU dimensions, geometry, update/revision and
  texture availability; log only selection/owner/state changes, not every probe/frame.
- Admit only loaded/nonmissing handles with 64/128 square textures and native-reported
  `geometry.humanoid.custom` / `geometry.humanoid.customSlim`, whose classic arm UVs were checked
  in the installed vanilla geometry. Preserve Steve/Alex fallback with an explicit reason.
  Persona's composed atlas/layout is NOT established from the SDK/assets; square dimensions
  alone are not sufficient, and general Persona/custom-geometry support is not claimed.
- Observe updates from mod load (including dressing-room updates), retaining only weak
  identities/texture references and metadata outside a draw. No menu-time world service query.
  Existing world/pipeline/resource release drops borrowed handles without unloading the engine's
  texture; only our owned default texture is unloaded. Texture-only changes reuse arm meshes;
  a slim/wide change or invalid mesh rebuilds them. Unchanged fallback avoids repeated PNG I/O.

### 0.3.11 runtime results

- User tested idle/walk/shoot/reload/inspect/put-away: arms animate correctly, no crash reported.
  Persona AND imported classic skins instead showed Steve/Alex; this prompted the skin fix.
- With RTX on, cached arm geometry plus per-draw matrices animates while the CPU-baked/rebuilt
  gun stays static. Strong additional support for the reused-geometry/BLAS cache hypothesis,
  not proof of the exact backend cache key. RTX remains parked; no RTX fix/experiment here.
- Real 120-frame arm CPU means: RTX on 0.032 ms, off 0.028–0.034 ms; each sample reports four
  mesh builds / 480 draw returns. GPU execution is not timed. See `docs/RTX-0.3.10.md`.
- Approved 0.3.11 work committed separately as `ce8c25b`.

### Validation / limitations

- 98 tests / 42,627 assertions pass, including GPU-layout admission without CPU image bytes
  and rejection of unknown/Persona geometry, legacy/nonsquare/unverified dimensions/aliases.
- Server/client release builds passed. Initial deployment was blocked by the running game;
  after closing it, 0.3.12 deployed successfully with DLL/PDB/manifest and both packs verified.
  User tested: arms animate, no crash reported, skin still falls back to Steve. The additional
  diagnostic-only logging above was built/tested but not separately deployed at that point.
  Unit tests do not validate engine hook behavior or Persona composition.
- Skin detection remains 1 Hz. No animation-atlas handling, arbitrary geometry, legacy 64x32
  UV conversion or Persona piece/tint/geometry reconstruction. Borrowed pixels are not modified
  to force base alpha; unusual alpha skins need game validation. Camera/constraint remain
  deferred and are not part of this skin-only patch (supersedes 0.3.11's planned version).

## [0.3.11] - 2026-10-25

### Added

- First-person player arms at TACZ's `lefthand_pos` / `righthand_pos`. Cache each root-to-node
  path; apply the same current gun pose, then Z+180 degrees as LeftHandRender/RightHandRender
  do. Keep vanilla arm-part pivots, 4-wide/3-wide dimensions and distinct modern left-arm UVs;
  no extra left reflection, body scale or vanilla idle/swing motion. Draw after the gun on its
  shared positioning stack, including the outgoing gun's retained put-away pose.
- Standard wide/slim 64x64 and standard-layout 128x128 local skins, including sleeve layers
  inflated by 0.25 pixels. Read the local player's SerializedSkinRef only on its initialized
  owning thread; copy validated RGBA bytes, never retain Player/native skin/image pointers.
  A 1 Hz probe detects changes (up to one second latency) without per-frame image copies.
  Persona, animated/nonstandard geometry/images, or unavailable skin owners use the installed
  game's Steve/Alex PNG, not a guessed Persona atlas. Fallback reason/layout/source are logged.
- Four cached 24-vertex arm/layer meshes per skin change; per-pose work only updates matrices
  and submits at most four draws. Skin textures use unique Raw world/resource/skin serials;
  existing RenderResourceKey release clears arm meshes/materials/texture through the original
  weak TextureGroup, including world exit and pipeline changes. No new native hooks.
- `[arm-perf]` after 120 frames reports CPU mean/max, skin-probe mean, build/upload total,
  matrix/draw mean and counts; GPU execution is explicitly not timed. `[arm-gpu]` identifies
  skin creation, fallback, material readiness and release. Pure AK47 two-path matrix benchmark:
  about 0.00029-0.00040 ms/frame (no game/skin/GPU); game cost is pending the next runtime test.

### Fixed

- Do not draw vanilla hands/items over a partially completed TACZ draw when skin rendering
  fails; valid current gun sessions own the first-person pass. Menu/stale sessions still pass
  through. No additional ItemStack copy or global menu-time client/skin lookup.

### Not yet

- `camera` and `constraint` effects deferred to 0.3.12. Camera needs coordinated world/hand
  camera consumption without changing actual player rotation; a post-setup matrix edit is not
  established to reach all Bedrock render paths. Constraint needs the not-yet-implemented aim
  weight. Both channels remain excluded from ordinary geometry animation, as before.
- RTX static gun investigation parked by user decision; `docs/RTX-0.3.10.md` is retained.
  This is not an RTX fix. No cache-key/BLAS experiment or opaque-by-value detour was added.
- Native arbitrary/Persona geometry, animated skin atlases, legacy 64x32 skins, armor sleeves,
  and Java skin-layer toggle UI are not implemented. Unsupported skins fall back; if even the
  installed default image is unavailable, skip arms with a warning rather than use stale skin.
- Bedrock alpha-test/blend moving-block materials replace Java entitySolid/entityTranslucent;
  only base-arm texels are forced opaque in our private copy. Lighting, partial-alpha sleeves
  and skin/mount alignment require game validation. No full player-body/jacket is rendered.

### Validation

- 97 tests / 42,607 assertions pass: geometry/UVs/normals/slim/sleeves, ancestor transform
  order/visibility, zero/nonfinite scale, skin admission/alpha/hash and animated AK47 placement.
- Server/client release builds passed; the deploy script successfully installed 0.3.11 with
  Minecraft closed. Deployed DLL/PDB/manifest hashes match the client output. Game appearance,
  skin ownership/material behavior and `[arm-perf]` timing await runtime validation.
- Inherited 0.3.10 runtime results and investigation notes are included in this uncommitted
  round. No new commit. See `docs/ARMS-0.3.11.md` for evidence, limitations and runtime checks.

## [0.3.10] - 2026-10-24

### Fixed

- 0.3.9 crashed at the main menu before any world tests could run. The 19:46:23, 19:47:21
  and 19:49:48 traces all point to GunMeshTraceHook forwarding `origin` at the old
  `GunRenderer.cpp:505`; all three minidumps identify `std::bad_variant_access`. LL's client
  header declares Mesh::_renderMesh's by-value TextureList using `brstd::static_vector`, but
  defines that container as an empty placeholder, not the native container ABI. The detour
  therefore corrupted the texture argument even when its gun-only diagnostic branch was
  skipped. Generic menu meshes also use that globally hooked function. This was not a null
  player/resource service or a world-exit cleanup crash.
- Removed the global `_renderMesh` hook completely, including install/uninstall and native
  metadata/call counters. Keep RTX diagnostics at our own fully typed Mesh::renderMesh call
  site, which takes its TextureVariant by const reference; no guessed container layout,
  per-mode bypass or catch-and-continue workaround.
- World-session/snapshot checks now precede client, player and GPU service queries in gun
  rendering and heartbeat diagnostics. Gameplay queries likewise stop before global client
  lookup when no world session exists; recorded client identity is atomic for input readers.
  A live world with a temporarily incomplete graphics pipeline defers cache creation instead
  of caching a failure or submitting through absent services.
- Reviewed all remaining native hooks against the installed 26.51.1 client SDK headers:
  MinecraftGame::$update `void()`, ClientInstance::_requestLeaveGameImpl `void(bool,bool)`,
  ItemInHandRenderer::renderFirstPerson `void(BaseActorRenderContext&, Matrix const&,
  ItemContextFlags)`. Added compile-time signature checks; ItemContextFlags is a fully defined
  uchar enum, not an opaque by-value class. Matching a declaration alone does not make an
  empty SDK container ABI-safe. Leave cleanup requires an active matching recorded session;
  GPU release uses already-owned cache state, never a menu-time global-client query.

### Validation / still pending

- 92 tests / 21,839 assertions pass, including menu/stale-snapshot and incomplete-pipeline
  guards. Native detour ABI behavior requires Minecraft to test; unit tests do not simulate it.
- Server/client release builds passed; `scripts/deploy-client.sh` deployed the 0.3.10 client
  with Minecraft closed. Main-menu and gameplay validation are pending; no commit was made.
- The 0.3.9 texture-lifetime fix and safe `[anim-frame]` / `[gun-render]` / `[gun-gpu]`
  diagnostics remain. Native-call/metadata fields are intentionally no longer emitted;
  `[gun-render]` now says `trace=call-site-only`. RTX animation remains unresolved.
- 0.3.9's in-game feature checklist was blocked by this startup crash; no pink-texture or RTX
  result is claimed. First re-test main menu for 10 seconds, then all world/RTX checks.
  Evidence and the hook/lifecycle audit: `docs/CRASH-0.3.10.md`.

### Follow-up runtime results

- User validated 0.3.10: no main-menu, join/leave or world-switch crash; no pink texture after
  quitting mid-put-away, quitting while holding, or A -> B -> A. RTX-off actions, gun/item
  switches and fast scrolling all passed. RTX-on gun remains visible but static, with audio.
- Latest run `2026-10-24.22.log` confirms pose publishing, hand-hook calls, mesh rebuilds and
  call-site draw returns under both modes, with zero invalid meshes. In world 3, RTX-on:
  3,290 hooks / 3,275 builds / 3,289 draws over 40.03 logged seconds; RTX-off:
  778 / 777 / 777 over 6.02 seconds. Both mode changes release/recreate GPU cache resources.
- RTX backend consumption/cache invalidation is not established by those CPU counters.
  That investigation shipped no speculative RTX code or deployment; detailed findings/diagnostics:
  `docs/RTX-0.3.10.md`. Follow-up results/analysis remain uncommitted after the approved
  combined 0.3.9/0.3.10 commit `1ee753c`.

## [0.3.9] - 2026-10-24

### 0.3.8 in-game results

- Passed: world entry while holding a gun (no crash, draw plays), normal-looking walk/sprint
  rate, blocked left click/R/H during put-away, gun -> empty hand/item/gun and fast scrolling.
  Re-entering after quitting during put-away had correct animation but a magenta gun texture.
  Newly reported pre-existing issue: Ray Tracing shows a static gun despite playing sounds;
  other graphics modes animate.
- Measured in newest run `2026-10-24.16.log`, not theoretical Java speeds:

  | Time / movement | Speed blocks/s | Java delta/tick | Native delta/tick | Distance phase / controller cycles/s | Stable track sample |
  | --- | ---: | ---: | ---: | ---: | ---: |
  | 19:07:05.542 walk | 4.135 | 0.12457 | 0.20762 | 1.2405 / 1.2442 | 0.99 s |
  | 19:07:41.305 run | 5.393 | 0.16216 | 0.27026 | 1.6180 / 1.6251 | 1.00 s |
  | 19:07:42.305 run | 5.537 | 0.16627 | 0.27712 | 1.6611 / 1.6621 | 1.00 s |
  | 19:07:43.310 run | 5.584 | 0.16835 | 0.28058 | 1.6751 / 1.6843 | 1.00 s |

  Native/Java is 1.66663-1.66669 in these windows: native distance phase would be about
  66.67% higher; Java-normalized input is about 40% lower than native. This validates the
  distance-unit cause in these stable samples, not a universal ratio: other logged windows
  (including ones with zero stable-track sampling) differ substantially. These measurements
  do not establish a general Bedrock speed increase over Java; no speed compensation added.

### Fixed

- Cross-world GPU cache ownership: GunRenderer previously cached textures/meshes/materials
  until mod disable, and a cached display bypassed PNG upload on subsequent world joins.
  The log shows world generations 3 -> 5 -> 7 but only one upload in generation 3. Animation
  snapshots own no GPU handles; the stale renderer cache, not the retained Lua VM, was the
  cause-level lifetime error. It also affects the ordinary-held-gun exit path, not just
  mid-put-away exits (the latter was the in-game reproduction; the former still needs testing).
- Release meshes, TexturePtrs, Raw group textures and materials before request-leave tears
  down the world; use the original weakly held texture group, not a newly looked-up group.
  Exit callbacks compare the recorded client identity instead of depending on a still-registered
  global primary client. Start-join/loaded events and a world/client/group/buffer/pipeline cache
  key provide additional invalidation. Runtime texture names include a world/cache serial.
  `[gun-gpu]` logs bind/create/release once per event. This is code-fixed, not yet game-validated.

### RTX investigation — not fixed yet

- Added low-rate `[anim-frame]` heartbeat independent of hand rendering and `[gun-render]`
  counters for hand-hook calls, mesh builds, draw submissions, native Mesh::_renderMesh calls,
  pose/mesh revisions, validity failures, cache keys and render metadata, labelled by the actual
  FrameBuilder RTX/deferred state. The native trace hook forwards all arguments unchanged.
- Checked LL Mesh/FrameBuilder/RenderMetadata/BVH/RTX headers and Lamium CellMesh/GhostRenderer.
  Existing logs cannot distinguish skipped hand rendering, invalid meshes and stale backend
  geometry; most BVH/RTX implementation types are opaque. No RTX root cause or API impossibility
  has been established. No speculative BLAS/cache-key/temporary flag or alternate rendering
  workaround was shipped. The normal Buffered tessellation/draw behavior remains unchanged.

### Validation

- 90 tests / 21,825 assertions pass, including cross-world/address-reuse/pipeline cache keys.
- Server/client release builds passed; `scripts/deploy-client.sh` deployed the 0.3.9 client
  with Minecraft closed. No 0.3.9 commit was made; GPU/game checks are still pending.
- Runtime checks and remaining RTX investigation: `docs/RENDER-0.3.9.md`.

## [0.3.8] - 2026-10-24

### Fixed

- 0.3.7 crashed on world entry before its other features could be tested. Both
  `trace_2026-10-24_18-25-54.log` and `trace_2026-10-24_18-28-10.log` point to the new
  `GunAnimation.cpp:123` ItemStack snapshot copy from refresh -> reveal -> held -> frame.
  The copy constructor resolves the thread-local ItemRegistry after MinecraftGame::update's
  registry scope has unwound. ItemRegistryRef::_lockRegistry finds no live registry and
  deliberately writes `0xDEADC0DE` to address zero. This is a new 0.3.7 regression, not a
  walk-distance/draw-timestamp arithmetic fault or an absent LocalPlayer.
- Animation snapshots now own only parsed gun state and a deep copy of user NBT. No client
  animation ItemStack is constructed, copied or retained, and Lua NBT reads never follow the
  incoming inventory slot or require a live player. Put-away still retains its old VM/data.
- Explicit client-world lifetime: initialize selections and send draw notifications only after
  ClientJoinLevelEvent (onPlayerLoaded); invalidate work on ClientExitLevelEvent. Clear queued
  inputs/reload replies on transition, tag reload/debug/tick/render snapshots with a world
  generation, and reject stale work across leave/rejoin, including reused engine addresses.
- Stop tracked animation audio before onLevelExit tears down the world. Handles are bound to
  their original non-owning engine reference and world generation; cleanup never looks up a
  new engine to stop old numeric handles, or acquires an engine when nothing was played.
- Server selection/draw timestamps were reviewed: they retain value data only, with state
  cleared on disconnect/server stop; no corresponding new registry-lifetime defect was found.

### Validation

- Both release variants build; all 88 tests / 21,816 assertions pass, including new unloaded,
  leave/rejoin and stale-work generation tests. The native registry assertion itself needs
  Minecraft to reproduce; these tests do not claim to simulate the engine.
- The 0.3.8 client was built and deployed by `scripts/deploy-client.sh` to the LeviLauncher
  test installation with the game closed. No commit was made.
- Crash evidence and the lifecycle review: `docs/CRASH-0.3.8.md`. First re-test entry into the
  affected world, then the full 0.3.7 movement/put-away checklist. Gameplay is not yet verified.

## [0.3.7] - 2026-10-24

### 0.3.6 in-game results

- Passed: draw when switching back, standing idle sway, smooth run_end, inspect and its
  interruption, inspect blocked during reload, reload animation/sound sync, and replayed draw
  after switching away/back. Walk/sprint cycles looked faster than Java TACZ.

### Fixed

- Walk-distance input now has an explicit Java definition: capture each actual client tick's
  horizontal displacement, accumulate `distance * 0.6F`, then expose
  `current + (current - previous) * partialTicks`. 0.3.6 fed Bedrock's
  `ActorMovement::getPredictedWalkDist` directly into Java's distance-driven Lua phase without
  establishing equivalent units. Java 1.20.1 Entity.java:734 and TACZ
  GunAnimationStateContext.java:327-341 define the new computation. The native getter's scale
  and Bedrock's actual walking/sprinting speeds are still unmeasured: no claim of an exact
  5/3 excess rate, or that all of the reported difference has been isolated to this input.
- Kept the Java controller's post-seek frame advancement: each script update overwrites phase,
  so this is not another accumulating 1x cycle. Movement inputs remain tick-driven; queued
  ticks now retain individual displacement and movement classifications below 20 FPS.
- About once per second while moving, `[walk-rate]` logs physical speed, Java/native distance
  delta per tick, distance-derived phase rate, and sampled normalized walk/run controller rate.
  Flat-ground theory: 4.317 blocks/s gives 0.12951 Java units/tick and 1.2951 cycles/s;
  5.612 gives 0.16836 and 1.6836. These are examples, not Bedrock measurements.

### Added

- Retain one old gun ItemStack snapshot and Lua/controller instance during put-away, suppressing
  the incoming vanilla hand/item even if the retained mesh fails. Old ammo/NBT stays attached
  to that snapshot; movement still follows the player. Trigger put_away, exit Lua states, keep
  updating/rendering the controller, then reveal only the latest selection and start its draw.
  Rapid scrolling replaces the pending target, never queues VMs or restarts the exit timer.
- Draw/put-away display sounds; animation sounds are handle-tracked and stopped at handoff,
  without stopping gunshots or unrelated game sounds. Shoot/reload/inspect/fire-select are
  locally blocked during retention and the new gun's visible draw.
- Server draw cooldown after LivingEntityDrawGun: carry `min(elapsed draw, previous put-away)`
  into a future draw timestamp; preserve it across switches during put-away; use the Java
  5 ms cooldown window. Observe authoritative selected slots on ticks, selection notifications
  and actions; switching cancels old bursts/reloads. The client cannot submit durations/IDs.
- Computation, rapid-scroll, return-to-old-slot, clear/world-exit and cooldown carry-over tests,
  plus real AK47 Lua phase-rate tests at 30/60/144 FPS and controller playback after exit:
  86 cases / 21,799 assertions passed. Server and client release builds passed; the 0.3.7
  client was deployed with `scripts/deploy-client.sh` to the LeviLauncher test installation.

### Divergences / validation pending

- The active Java retention caller is bundled SimpleBedrockModel 2.2.2 FirstPersonRenderHandler,
  not LocalPlayerDraw's commented-out keep/doPutAway/scheduled draw calls or TACZ's commented
  equip-retention mixin. SBM reveals on the first START tick after the duration; TACZ's +50 ms
  is a separate state-machine reinitialization guard. This port deliberately retains duration
  **plus a fixed 50 ms**, then hands off on a frame, as requested (AK47: 570 + 50 = 620 ms).
  The client waits for the full visible new draw, even when Java's server formula ends sooner.
- Endpoint displacement per tick approximates Java's sum of Entity.move displacements; a
  teleport/correction can enter the distance sample. Riding, teleports and unusual movement
  need further testing. No movement-speed multiplier was added to hide Bedrock/Java differences.
- Visual/sound suppression, actual native scale, multiplayer ordering and 0.3.7 game feel still
  require in-game testing. Details, source evidence and test checklist: `docs/ANIMATION-0.3.7.md`.

## [0.3.6] - 2026-10-24

### Added

- TACZ Lua animation state machines replace the temporary C++ driver, after
  LuaStateMachineFactory / LuaAnimationState / AnimationStateMachine. Asset modules use
  `<namespace>_<path>` in package.preload; initialize, states, entry/update/exit and nested
  trigger dispatch run the unmodified default / AK47 scripts (and the other default-pack guns).
- Game-independent AnimationController / DiscreteTrackArray / ObjectAnimationRunner semantics:
  ordered track lines, overlapping additive gun kicks, loop / hold / stop, pause / seek,
  quaternion cross-fades and TACZ's actual `1 - (1 - t)^4` transition easing. Sound keyframes
  now run on those same timelines instead of the separate reload scheduler.
- GunAnimationStateContext reads ammo / barrel / fire mode / cooldown / reload state and native
  movement input, walk distance, ground, crouch and sprint state. Movement inputs are dispatched
  from ClientLevelTickEvent; state updates and animation evaluation stay per frame.
- H inspects, following LocalPlayerInspect's state-lock checks. Draw, shoot, reload,
  cancel_reload, fire_select, idle/walk/run and put_away inputs are dispatched to Lua. Fire mode
  is predicted locally until sync; draw time locks local actions. `/tacz anim <name>` now plays
  one debug override and returns control to the continuously updating state machine.
- Internal rifle/pistol fallback animations and the default state-machine script are embedded
  from TACZ's original assets; gun packs override them. Display state_machine_param and a
  read-only primitive/compound NBT accessor are available to scripts.
- A separate animation Lua VM with text/preload-only loading and an instruction budget per
  invocation, isolated per held-gun session. LuaJ compatibility shims provide unpack, math.pow,
  bit32 and integral number formatting. Script errors identify the module and fall back to
  static_idle, not a game crash.
- Controller / sandbox / real-pack sequence tests; all 54 default-pack guns' state machines
  initialize and accept action inputs (76 cases total).

### Not yet

- Delaying Bedrock's actual item switch for put_away_time: put_away and exit are dispatched,
  but selection changes immediately, so the old gun is not kept onscreen for the holster animation.
- Aiming, crawl, slide, heat, charge and attachments (including extended-magazine modifiers)
  return neutral context values; manual-action bolt and melee gameplay inputs are not wired.
- Arms, camera/constraint bone listeners, functional additional-magazine rendering and shell
  ejection rendering. Crosshair-hide flags and shell-window requests are stored/logged only.
- glTF animations and spherical interpolation on non-rotation channels remain unsupported.
- Lua module globals shared across gun-selection sessions are not retained: a fresh animation
  VM isolates a faulty pack from the next selection instead of sharing Java's asset-script VM.

## [0.3.5] - 2026-10-24

### Added

- First-person bone animation playback after TACZ's AnimationKeyframesSerializer,
  Animations, ObjectAnimationChannel and CustomInterpolator: linear / Catmull-Rom spline,
  pre/post keyframes, scalar scale, translation units and additional rotation / scale after
  the rest pose (BedrockPart and ModelTranslate/Rotate/ScaleListener).
- Temporary C++ tracks driven by the existing MinecraftGame::update frame hook: looping
  static_idle, draw on selection, tactical / empty reload, additive shoot kick. Named debug
  playback via `/tacz anim <name>`; unknown names list the held gun's clips in the client log.
- Pack animation loading and missing-name fallback via default_animation / use_default_animation
  when the referenced file is present in a gun pack. Loop / hold / stop playback boundaries,
  real AK47 parsing, rest-pose regression and interpolation tests (57 cases total).
- Mesh rebuild CPU timing logged once after 120 changing poses; static poses reuse the mesh.

### Not yet

- Lua animation state machine / transitions (0.3.6), player arms and camera / constraint
  listeners (0.3.7), aiming / sprint / fire-select animation tracks.
- Bundled rifle_default / pistol_default animation assets (use_default_animation currently
  needs those files in a gun pack), glTF animations, spherical interpolation on non-rotation
  channels, functional additional-magazine rendering. Unsupported Molang is logged and skipped
  (string vector components become zero, as in TACZ).

## [0.3.4] - 2026-10-24

### Added

- Reload (R, TACZ's default key), after LivingEntityReload / LocalPlayerReload:
  - The server runs the gun's Lua logic script every tick (AK47: `xmag_reload_logic.lua` with
    its `script_param` timings), or TACZ's default reload when a gun has no script. Tactical
    reload keeps the chambered round; empty reload chambers one from the new magazine.
  - The reload state is sent to the client on every change; no shooting while reloading,
    switching weapon stops the reload, pulling the trigger sends TACZ's interrupt.
  - The client plays the reload animation's sound keyframes (magout, magin, ...) at their times.
- Embedded Lua 5.4 + sol2 (`src/lua/GunLogic.*`) after TACZ's ScriptManager: modules in
  `package.preload` as `<ns>_<path>`, TACZ's reload-state and fire-mode constants, sandboxed
  (no io/os/file loading/bytecode, instruction limit per call). All 10 default-pack gun logic
  scripts load.
- Ammo: items carrying TACZ's `AmmoId` NBT count as ammo; survival reloads take them from the
  inventory, creative reloads are free. `/tacz ammo <count>` gives ammo for the held gun
  (placeholder look: iron nugget).
- Probe HUD shows reserve ammo and reload state; the server ping moved to Ctrl+P.
- Unit tests: reload timelines (default and AK47 script), sandbox, animation sound keyframes
  (42 cases total).

## [0.3.3] - 2026-10-23

### Fixed

- Uneven full-auto fire rate (0.3.2 test: intervals jumped between 50, 100 and 150 ms). The
  trigger was polled through LeviLamina's ClientThreadExecutor, which only runs once per 50 ms
  client tick. It now runs every frame from a `MinecraftGame::update` hook (as Lamium does), and
  burst follow-up sounds are timed per frame too. The frame hook rate is logged once per world.

### Changed

- Bullets pass through iron bars, fences, fence gates and leaves, as TACZ's
  `tacz:bullet_ignore` block tag does (matched by block name; Bedrock has no block tags).
- Bullet entity collision box is 0.0625 x 0.0625, TACZ's entity size. Hit detection does not
  use it (bullets are rays, as in TACZ). Pack change: restart the game.

## [0.3.2] - 2026-10-23

### Added

- Shooting (`src-server/server/Shooter.cpp`, `src-client/client/gameplay/ClientShooter.cpp`,
  rules in `src/gun/ShootRules.h`), following TACZ's ShootKey / LocalPlayerShoot /
  LivingEntityShoot / ModernKineticGunScriptAPI.shootOnce:
  - Holding the left button fires at the gun's rate in AUTO (and BURST with continuous_shoot);
    SEMI and BURST need a new press. G cycles the fire mode (TACZ's default key).
  - The client checks its cooldown, plays the shoot / dry-fire / fire-select sound right away
    and sends a timestamped shoot packet; the server checks the cooldown (5 ms window), the
    client clock (±300 ms), chambers a round for closed bolts, consumes ammo per bolt type and
    fires. Bursts spread their rounds over server ticks.
  - Bullet spread from TACZ's inaccuracy (stand / move / sneak, fire mode addends) with
    vanilla's triangular jitter; bullets inherit the shooter's motion.
  - Bullet speed now includes TACZ's default global modifier (x2): the AK47 flies 25 blocks/tick.
- Gun item state uses TACZ's NBT encodings (`src/nbt/GunItem.*`): `GunFireMode` as the enum
  name, `HasBulletInBarrel` as a byte. `/tacz setgun` hands the gun out as TACZ's creative tab
  does: first fire mode, full magazine, round chambered.
- Display sounds get TACZ's defaults (`dry_fire`, `fire_select`, hit and melee sounds).
- Unit tests for the shooting rules (36 cases total).

### Changed

- The probe no longer fires on click; R still pings the server, and the HUD shows fire mode
  and chamber state.

### Not yet

- Reload, bolt action, aiming, draw / sprint locks, heat, Lua `shoot` overrides.

## [0.3.1] - 2026-10-23

### Added

- Bullets hit entities (`src-server/server/BulletSystem.cpp`, `src/gun/BulletHit.h`): each tick
  the path up to the first block is tested against mobs and players with TACZ's fixed hitbox
  (stretched along and shifted back against the target's motion, +1/16 for standing players),
  nearest first, up to `pierce` targets.
- TACZ damage on hit: distance falloff, headshot multiplier (eye height ± 0.25), armour-ignore
  split into an armour-reduced projectile part and a part with a cause the game does not reduce
  by armour (chosen at runtime), shotgun pellets, ignite, knockback. The hurt cooldown is
  cleared before each part, as in TACZ, so full-auto fire is not absorbed.
- Damage is attributed to the shooter (`ActorDamageByChildActorSource`); hit details are
  logged and shown on the probe HUD.
- Unit tests for hit geometry (28 cases total).

### Not yet

- Player-vs-player latency rewind, configured headshot boxes, explosions, spread.

## [0.3.0] - 2026-10-23

Technical validation phase complete: all six items in `docs/VALIDATION.md` pass in single-player
(custom packets, item NBT, input interception, first-person model rendering, sound playback,
server-driven bullet entity). Next phase: gun logic implementation.

### Changed

- 0.2.6 test result: bullets are visible at full speed, disappear on the block they hit and
  leave nothing behind; `moveTo` and `teleport` sync look the same, `moveTo` stays the default.

## [0.2.6] - 2026-10-23

### Added

- Bullet entity probe: a minimal behavior pack (`packs/behavior_packs/tacz_bp`, entity
  `tacz:bullet` with no physics, damage or AI of its own) and resource pack
  (`packs/resource_packs/tacz_rp`, a small yellow cube). LeviLamina loads both from the mod
  folder automatically.
- Server bullet system (`src-server/server/BulletSystem.cpp`): firing spawns a bullet that the
  mod moves every server tick with TACZ's motion model (speed / 20 blocks per tick, then drag
  and gravity) and removes on the first block hit, on expiry or in unloaded chunks.
- `src/gun/BulletPhysics.h` with unit tests (24 cases total).
- `/tacz bdebug <moveMode> <speedScale>`: entity sync via `moveTo` (0) or `teleport` (1), and a
  speed multiplier to watch bullets in slow motion.
- `scripts/deploy-client.sh` also deploys the packs.

## [0.2.5] - 2026-10-23

### Added

- Gun pack sound playback (client, `src-client/client/sound`): `tacz_sounds/<path>.ogg` is read
  from the gun pack and played through `SoundEngine::playFromMemory`; no resource pack needed.
- Gun display `sounds` map is parsed. Probe: left click plays the held gun's `shoot` sound,
  R plays `fire_select`.

### Fixed

- Displays with an empty sound id (`"inspect": ""`, used by fn_fal, hk_mk23, spas_12) failed to
  load, so those three guns had no display. Empty ids are now skipped, as in TACZ.

## [0.2.4] - 2026-10-23

### Changed

- First-person gun quads are single-sided by default (0.2.3 test: no missing faces), halving
  the mesh size. `/tacz rdebug 6 1` turns two-sided drawing back on for diagnosis.
- Technical validation item 4 (first-person model replacement) recorded as passed in
  `docs/VALIDATION.md`.

## [0.2.3] - 2026-10-23

### Changed

- First-person gun now renders with the textured moving-block alphatest material by default
  (confirmed visible and correctly oriented in the 0.2.2 test).
- The gun transform is multiplied onto the current world matrix, which during the first-person
  pass holds the inverse view rotation. The view matrix is never replaced.
- `/tacz rdebug <material> <twoSided>`: the placement argument is gone.

### Fixed

- Crash when the first-person gun was drawn with a replaced view matrix (0.2.2, placements 0
  and 1: null read inside the renderer). That code path is removed.

## [0.2.2] - 2026-10-23

### Changed

- First-person gun render diagnostics: `/tacz rdebug <material> <placement> <twoSided>` switches
  between 7 materials and 3 placements at runtime and logs camera and matrix state once per
  change. Defaults: renderer static material, camera-basis placement, two-sided quads.
- `scripts/deploy-client.sh` builds the client and only copies it if the build succeeded and the
  manifest says `client`.

### Fixed

- 0.2.1 test result: the gun was not visible in first person (mesh, texture and material were all
  created; the cause is being narrowed down with the diagnostics above).

## [0.2.1] - 2026-10-23

### Added

- First-person gun rendering probe (client): while holding a gun (`/tacz setgun`), the vanilla
  first-person pass is replaced by the gun's TACZ model and texture read from the gun pack.
  Rest pose only: no animation, no arms, no aiming.
- Bedrock geometry loader matching TACZ's conversions (`src/model`): pivots, per-face and box UV,
  inflate, mirror, rotated cubes, `_illuminated` bones, idle_view positioning.
- Gun display files (`assets/<ns>/display/guns`) are loaded; packs stay open for asset reads.
- Unit tests for the geometry loader (19 cases total).

## [0.2.0] - 2026-10-23

### Added

- Gun pack loader for the TACZ format (`src/pack`): scans `mods/tacz/gunpacks/` for folder packs
  with `gunpack.meta.json`, parses gun index/data and ammo index with TACZ's defaults and
  validation, and collects `data/<ns>/scripts/*.lua`. Loads all 54 guns of the default pack.
- Damage and rate-of-fire calculations matching TACZ (`src/gun/GunStats.h`): distance falloff,
  headshot multiplier, armour-ignore split, shotgun pellet split, shoot interval.
- Unit tests (`tacz-tests` target, doctest).
- `/tacz packs` command; `/tacz setgun` now checks the gun id against loaded packs.

## [0.1.2] - 2026-10-23

### Fixed

- `/tacz nbt` output is also written to the log (chat output from custom commands is not shown
  in single-player).

## [0.1.1] - 2026-10-23

### Added

- Technical-validation probe: custom client/server packets, gun NBT read/write and sync,
  swallowing vanilla left-click while holding a gun, debug HUD text. Verified in single-player.

## [0.1.0] - 2026-10-23

### Added

- Project set up from the LeviLamina mod template: server and client variants, GPL-3.0 licence.

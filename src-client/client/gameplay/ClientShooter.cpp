#include "client/gameplay/ClientShooter.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <type_traits>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/client/ClientStartJoinLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/input/HIDController.h"
#include "mc/deps/input/MouseAction.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/world/level/Level.h"

#include "client/sound/GunSound.h"
#include "client/animation/GunAnimation.h"
#include "client/animation/ViewTurnInertia.h"
#include "client/gameplay/ClientAim.h"
#include "client/gameplay/ClientRecoil.h"
#include "client/render/GunRenderer.h"
#include "client/ui/GunHud.h"
#include "gun/ShootRules.h"
#include "gun/Sprint.h"
#include "gun/DrawRules.h"
#include "gun/ClientWorldSession.h"
#include "gun/ClientStateLock.h"
#include "gun/ClientTickGate.h"
#include "gun/BoltRules.h"
#include "mod/TaczMod.h"
#include "nbt/AmmoItem.h"
#include "nbt/StackLimitProbe.h"
#include "nbt/GunItem.h"
#include "net/GunPackets.h"

namespace tacz::client::shooter {
namespace {

using Clock = std::chrono::steady_clock;

int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
int64_t systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

// Written from the window procedure / packet threads, consumed in frame().
std::atomic<bool> triggerDown{false};
std::atomic<bool> resetPending{false};
std::atomic<bool> reloadKeyPressed{false};
std::atomic<bool> fireSelectPressed{false};
std::atomic<bool> inspectPressed{false};
std::array<std::atomic<bool>, 3> actionKeyDown{}; // rising edges, not OS key repeat
gun::ClientWorldSession worldSession;
std::atomic<IClientInstance const*> joinedClient{nullptr}; // identity only; input callbacks also read it

using FrameSignature = void (MinecraftGame::*)();
using LeaveSignature = void (ClientInstance::*)(bool, bool);
static_assert(std::is_same_v<decltype(&MinecraftGame::$update), FrameSignature>);
static_assert(std::is_same_v<decltype(&ClientInstance::_requestLeaveGameImpl), LeaveSignature>);

// Packet threads only decode/enqueue. Entity resolution, store and consumers are
// frame-owned, as with ServerMessageUpdateEntityData.java:42-61's main-thread work.
std::mutex serverStateMutex;
struct SyncedReply {
    gun::sync::Message message;
    uint64_t world;
};
std::vector<SyncedReply> serverDataInbox;
gun::sync::EntityStore syncedEntities;
uint64_t localRuntimeId = 0; // frame-owned local-player view, not a cached actor pointer
gun::sync::Snapshot const& localSyncedState() { return syncedEntities.local(worldSession.current(), localRuntimeId); }
gun::ReloadStateType reloadType() { return localSyncedState().reload().type; }
std::optional<gun::SprintIntentScope> sprintLife;
std::optional<gun::HandSelection> sprintSelection;
std::optional<bool> previousAlive;
std::atomic<uint64_t> clientTickCount{0}; // tick listener may run on another thread
gun::ClientTickGate reloadCancelTickGate; // frame-owned; never gates shooting itself
gun::ClientTickGate autoBoltTickGate;
gun::ClientBoltLatch boltLatch;

// ClientLevelTickEvent is synchronous in ClientLevel::$_subTick, whereas frame()
// is after MinecraftGame::update. Do not assume an unproven native call/thread
// relationship: share only pure lock state and a copy of the last applied local
// view. Never hold this mutex across engine/network/sound/Lua calls.
std::mutex stateLockMutex;
gun::ClientStateLock stateLock;
gun::sync::Snapshot lockSyncedState;
uint64_t lockWorld = 0;
void acquireStateLock(gun::LockCondition condition) {
    auto world = worldSession.current();
    auto now = systemNowMs(); // LocalPlayerDataHolder uses System.currentTimeMillis
    std::lock_guard lock(stateLockMutex);
    if (!world || lockWorld != world) return;
    stateLock.lockState(condition, now);
}
bool stateLocked() {
    std::lock_guard lock(stateLockMutex);
    return lockWorld == worldSession.current() && stateLock.clientStateLock;
}
bool pendingNonShootLock() {
    std::lock_guard lock(stateLockMutex);
    return lockWorld == worldSession.current() && stateLock.pendingNonShoot();
}
void resetStateLock(bool newSession = false) {
    auto world = worldSession.current();
    boltLatch.reset(); // existing world/respawn reset path; no extra death cancellation
    std::lock_guard lock(stateLockMutex);
    stateLock.reset();
    if (newSession) lockSyncedState = {};
    lockWorld = world;
}
void tickStateLock() {
    auto world = worldSession.current();
    if (!world) return;
    auto now = systemNowMs();
    std::lock_guard lock(stateLockMutex);
    if (lockWorld != world) return;
    stateLock.tickStateLock(now, lockSyncedState);
}

// LocalPlayerDataHolder, reduced to what is implemented. Main thread only (frame()).
std::optional<int64_t> lastShot;                 // clientShootTimestamp
std::optional<int64_t> shotTimestamp;            // system time, for GunAnimationStateContext
std::optional<int64_t> previousShotTimestamp;    // clientLastShootTimestamp (previous trigger pull)
bool                   lastShotSucceeded = false; // ShootKey.lastTimeShootSuccess
bool                   dryFireArmed      = true;  // SoundPlayManager.DRY_SOUND_TRACK
std::atomic<int>       reloadTypePublished{0};
std::atomic<float> aimingProgressPublished{0}; // sensitivity hook may run outside frame()
std::atomic<bool> isAimingPublished{false};

// Burst follow-up shots. Animation sound keys now belong to ObjectAnimationRunner, not a
// second hard-coded reload timeline that would desynchronise Lua-selected animations.
struct PendingShot {
    Clock::time_point due;
    std::optional<pack::ResourceId> sound;
    float             pitch;
    int64_t triggerTimestamp;
};
std::vector<PendingShot> pendingShots;

struct PredictedMode {
    pack::ResourceId gun;
    int slot;
    std::optional<pack::FireMode> mode;
    Clock::time_point at;
};
std::optional<PredictedMode> predictedMode; // frame thread only, as LocalPlayerFireSelect

std::mt19937& rng() {
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}

float soundPitch() { return 0.9f + std::uniform_real_distribution<float>(0.0f, 0.125f)(rng()); }

struct HeldGun {
    nbt::GunItemState     state;
    pack::GunEntry const* entry;
};

std::optional<HeldGun> heldGun(bool predict = false) {
    if (!worldSession.current()) return std::nullopt;
    auto client = ll::service::getClientInstance();
    if (!client || &*client != joinedClient.load()) return std::nullopt;
    auto* player = client->getLocalPlayer();
    if (!player) return std::nullopt;
    auto state = nbt::readGun(player->getSelectedItem());
    if (!state) return std::nullopt;
    auto const* entry = TaczMod::getInstance().gunPacks().gun(state->gunId);
    if (!entry) return std::nullopt;
    if (predict && predictedMode) {
        auto const& p = *predictedMode;
        if (p.gun != state->gunId || p.slot != player->getSelectedItemSlot() || p.mode == state->fireMode
            || Clock::now() - p.at > std::chrono::milliseconds(250)) predictedMode.reset();
        else state->fireMode = p.mode;
    }
    return HeldGun{*state, entry};
}

bool inGameplay() {
    if (!worldSession.current()) return false;
    auto client = ll::service::getClientInstance();
    return client && &*client == joinedClient.load() && client->getLocalPlayer() && client->getScreenName().starts_with("hud_screen");
}

// ShootKey tests IGun, not whether its data/display is known or ready to fire.
bool hasMainHandGun() {
    if (!worldSession.current()) return false;
    auto client = ll::service::getClientInstance();
    if (!client || &*client != joinedClient.load()) return false;
    auto* player = client->getLocalPlayer();
    return player && nbt::readGun(player->getSelectedItem()).has_value();
}

std::optional<pack::ResourceId> displaySound(HeldGun const& gun, std::string const& key) {
    auto const& packs = TaczMod::getInstance().gunPacks();
    if (!gun.entry->index.display) return std::nullopt;
    auto const* display = packs.gunDisplay(*gun.entry->index.display);
    if (!display) return std::nullopt;
    auto it = display->sounds.find(key);
    return it == display->sounds.end() ? std::nullopt : std::optional{it->second};
}

void bolt() {
    if (stateLocked() || boltLatch.isBolting) return;
    auto gun = heldGun();
    if (!gun) return;
    auto client = ll::service::getClientInstance();
    auto player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    auto const& data = gun->entry->data;
    gun::BoltCheck check;
    check.hasDisplay = gun->entry->index.display && TaczMod::getInstance().gunPacks().gunDisplay(*gun->entry->index.display);
    check.type = data.bolt;
    check.ammo = {gun->state.ammo, gun->state.barrel};
    check.inventoryFed = data.reload.type == pack::FeedType::Inventory;
    check.needCheckAmmo = !player->isCreative();
    check.inventoryHasAmmo = data.ammoId && nbt::countAmmo(*player, *data.ammoId) > 0;
    if (gun::clientBoltRefusal(check)) return;
    acquireStateLock(gun::LockCondition::Bolt);
    boltLatch.isBolting = true;
    net::BoltPacket{}.sendToServer();
    if (anim::hasStateMachine()) {
        if (auto id = displaySound(*gun, "bolt")) sound::play(*id, 1.f, 1.f, true);
        anim::bolt();
    }
}

void tickAutoBolt(std::optional<nbt::GunItemState> const& mainHand) {
    if (!mainHand) { boltLatch.observeItem(false, false); return; }
    bolt(); // independent of trigger hold, before chamber-based latch clearing (Java)
    boltLatch.observeItem(true, mainHand->barrel);
}

// LocalPlayerReload.reload.
void reload(HeldGun const& gun) {
    auto const& data = gun.entry->data;
    if (data.reload.type == pack::FeedType::Inventory) return;
    if (stateLocked()) return; // LocalPlayerReload.java:70-80, not local cooldown/presentation
    if (lastShot && nowMs() - *lastShot < 100) return;
    // canReload: not full and, outside creative, carrying the ammo.
    auto client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    if (gun.state.ammo >= data.ammoAmount) return;
    bool creative = player->isCreative();
    if (!creative && !data.reload.infinite && !(data.ammoId && nbt::countAmmo(*player, *data.ammoId) > 0)) return;

    acquireStateLock(gun::LockCondition::Reload);
    net::ReloadPacket{}.sendToServer();
    bool noAmmo = data.bolt == pack::Bolt::OpenBolt ? gun.state.ammo <= 0 : !gun.state.barrel;
    if (auto id = displaySound(gun, noAmmo ? "reload_empty" : "reload_tactical")) sound::play(*id, 1.f, 1.f, true);
    anim::reload();
}

// LocalPlayerShoot.shoot + doShoot. In continuous fire the shot is stamped one interval after
// the previous one instead of "now", so frame timing jitter does not change the fire rate: a
// frame that comes late is made up by the next one (TACZ gets the same effect by scheduling
// the shot `coolDown` ms after its tick on a timer thread).
bool shoot(HeldGun const& gun, int64_t now, bool chained) {
    auto const& data = gun.entry->data;
    auto        mode = gun.state.fireMode;
    if (gun::clientCooldownMs(data, mode, now, lastShot) > 0) return false;
    // LocalPlayerShoot.java:136,189-204: no general state-lock check, and never
    // use authoritative shoot cooldown as the cadence clock.
    if (pendingNonShootLock() || !gun::syncedShootPrecheck(localSyncedState())) return false;

    gun::GunAmmo ammo{gun.state.ammo, gun.state.barrel};
    int          rounds = gun::availableRounds(*data.bolt, ammo);
    if (rounds < 1) {
        if (dryFireArmed) {
            if (auto id = displaySound(gun, "dry_fire")) sound::play(*id, 1.0f, soundPitch());
            dryFireArmed = false;
        }
        return false;
    }
    if (*data.bolt == pack::Bolt::ManualAction && !ammo.barrel) { bolt(); return false; }
    // Java checks the server-synced value after preCheck, before shot effects.
    if (gun::sprintBlocksShot(localSyncedState().sprintTime())) return false;

    int64_t interval = gun::triggerIntervalMs(data, mode);
    // A stall longer than an interval (loading, alt-tab) restarts the rhythm instead of
    // firing a catch-up double shot.
    bool    keepRhythm = chained && lastShot && now - *lastShot < interval * 2;
    int64_t shotAt     = keepRhythm ? *lastShot + interval : now;
    previousShotTimestamp = shotTimestamp;
    shotTimestamp      = systemNowMs();
    lastShot           = shotAt;
    acquireStateLock(gun::LockCondition::Shoot); // once per packet, not burst presentation round
    net::ShootPacket{shotAt}.sendToServer();
    recoil::shot(data.recoil, gun.state.gunId, shotAt);
    anim::shoot();

    // Burst: one packet, `count` authoritative rounds; client follow-ups predict only
    // presentation (animation, sound and camera recoil), not damage/ammo consumption.
    auto shootSound = displaySound(gun, "shoot");
    if (shootSound) sound::play(*shootSound, 0.8f, soundPitch());
    int  burst  = mode == pack::FireMode::Burst ? std::min(std::max(data.burst.count, 1), rounds) : 1;
    auto period = std::chrono::milliseconds(gun::burstShootIntervalMs(data));
    for (int i = 1; i < burst; ++i) pendingShots.push_back({Clock::now() + period * i, shootSound, soundPitch(), shotAt});
    return true;
}

// Logs once per world how often the frame hook runs, to confirm it is per frame and not per tick.
void measureFrameRate() {
    static std::optional<Clock::time_point> start;
    static int                              frames = 0;
    static bool                             logged = false;
    if (logged) return;
    if (!start) start = Clock::now();
    ++frames;
    auto elapsed = std::chrono::duration<double>(Clock::now() - *start).count();
    if (elapsed < 3.0) return;
    logger().info("[shoot] frame hook rate: {:.0f} Hz", frames / elapsed);
    logged = true;
}

void applyServerData(LocalPlayer& player) {
    auto world = worldSession.current();
    syncedEntities.begin(world);
    localRuntimeId = player.getRuntimeID().rawID;
    auto exists = [&](uint64_t id) { return id == localRuntimeId || player.getLevel().getRuntimeEntity(ActorRuntimeID{id}, false) != nullptr; };
    syncedEntities.prune(exists);
    std::vector<SyncedReply> replies;
    { std::lock_guard lock(serverStateMutex); replies.swap(serverDataInbox); }
    for (auto const& reply : replies) {
        if (!worldSession.accepts(reply.world) || !exists(reply.message.runtimeId)) continue;
        syncedEntities.apply(reply.world, reply.message);
    }
    // No synthetic cancel_reload on synced idle/refusal: Java's data update only
    // merges. Packs can read getReloadStateType (GunAnimationStateContext:242-248),
    // already supplied by GunAnimation::context; explicit cancel input stays below.
    reloadTypePublished = static_cast<int>(reloadType());
    aimingProgressPublished = localSyncedState().aimingProgress();
    isAimingPublished = localSyncedState().isAiming();
    {
        std::lock_guard lock(stateLockMutex);
        if (lockWorld == world) lockSyncedState = localSyncedState();
    }
}

// ShootKey.autoShoot and the reload / fire-select keys. Runs every rendered frame on the main thread.
void frame() {
    if (resetPending.exchange(false)) {
        triggerDown = false;
        reloadKeyPressed = fireSelectPressed = inspectPressed = false;
        for (auto& down : actionKeyDown) down = false;
        {
            std::lock_guard lock(serverStateMutex);
            // A fresh session's initial sync may arrive before its first frame.
            // Keep ALL its entries; discard only retired-world queued work.
            std::erase_if(serverDataInbox, [](auto const& reply) {
                return !worldSession.accepts(reply.world);
            });
        }
        syncedEntities.begin(worldSession.current());
        localRuntimeId = 0;
        sprintLife.reset();
        sprintSelection.reset();
        previousAlive.reset();
        clientTickCount.store(0, std::memory_order_relaxed);
        reloadCancelTickGate.reset();
        autoBoltTickGate.reset();
        lastShot.reset();
        shotTimestamp.reset();
        previousShotTimestamp.reset();
        predictedMode.reset();
        lastShotSucceeded = false;
        dryFireArmed      = true;
        reloadTypePublished = 0;
        aimingProgressPublished = 0;
        isAimingPublished = false;
        resetStateLock(true);
        pendingShots.clear();
        aim::clear();
        recoil::clear();
        anim::clear();
    }
    // LocalPlayer may already exist while its inventory/network is still loading. Draw
    // notifications and session initialization start only after onPlayerLoaded, not merely
    // on the first non-null player observed from MinecraftGame::update.
    if (!worldSession.current()) return;
    auto client = ll::service::getClientInstance();
    if (!client || &*client != joinedClient.load()) return;
    auto* player = client->getLocalPlayer();
    if (!player) return;
    gun::SprintIntentScope life{reinterpret_cast<uintptr_t>(player), static_cast<int>(player->getDimensionId()),
        player->isAlive() && !player->isDead() && !player->isSpectator()};
    bool alive = player->isAlive() && !player->isDead();
    // LocalPlayerMixin.java:125-128: reset after respawn, not on death or an
    // ordinary dimension/spectator change. Observe actual life, not usability.
    if (sprintLife && (sprintLife->actor != life.actor || (previousAlive && !*previousAlive && alive))) resetStateLock();
    previousAlive = alive;
    if (sprintLife && *sprintLife != life) triggerDown = false; // no stale press across death/respawn or dimension/player replacement
    sprintLife = life;
    auto mainHand = nbt::readGun(player->getSelectedItem());
    gun::HandSelection selection{mainHand ? std::optional{mainHand->gunId} : std::nullopt, player->getSelectedItemSlot()};
    bool sprintSelectionChanged = !sprintSelection || *sprintSelection != selection;
    sprintSelection = selection; // actual gun identity even if pack data/display is unavailable
    auto gun = heldGun(true);
    bool selectionChanged = anim::held(gun ? std::optional{gun->state.gunId} : std::nullopt, player->getSelectedItemSlot());
    if (selectionChanged) {
        recoil::clear();
        boltLatch.reset(); // LocalPlayerDraw.java:115-116
        acquireStateLock(gun::LockCondition::Draw); // LocalPlayerDraw.java:104-116
        net::DrawPacket{}.sendToServer();
        pendingShots.clear(); // do not kick the newly selected gun with an old burst
        lastShotSucceeded = false;
        lastShot.reset();
        shotTimestamp.reset();
        previousShotTimestamp.reset();
        predictedMode.reset();
    }
    auto const* display = gun && gun->entry->index.display
        ? TaczMod::getInstance().gunPacks().gunDisplay(*gun->entry->index.display) : nullptr;
    aim::frame(*client, *player, gun ? std::optional{gun->state.gunId} : std::nullopt,
        gun ? &gun->entry->data : nullptr, display, selectionChanged || sprintSelectionChanged, anim::aimAllowed(),
        gun::shootStopsSprint(inGameplay(), mainHand.has_value(),
            player->isSpectator(), triggerDown.load()));
    applyServerData(*player);
    // Observe every frame before shooting can consume a newly synced chambered round.
    boltLatch.observeItem(mainHand.has_value(), mainHand && mainHand->barrel);
    // Completed-tick marker is published AFTER tickStateLock. No engine/item/
    // sound/network work runs in that listener. Java LocalPlayerBolt reads only
    // its post-evaluation lock, local latch and main-hand item, not synced values.
    auto completedTick = clientTickCount.load(std::memory_order_acquire);
    if (completedTick && autoBoltTickGate.tryAcquire(completedTick)) tickAutoBolt(mainHand);
    if (inGameplay()) measureFrameRate();
    anim::movementTick();

    auto clockNow = Clock::now();
    for (size_t i = 0; i < pendingShots.size();) {
        if (pendingShots[i].due > clockNow) {
            ++i;
            continue;
        }
        if (pendingShots[i].sound) sound::play(*pendingShots[i].sound, 0.8f, pendingShots[i].pitch);
        if (gun) recoil::shot(gun->entry->data.recoil, gun->state.gunId, pendingShots[i].triggerTimestamp, true);
        anim::shoot();
        pendingShots.erase(pendingShots.begin() + static_cast<std::ptrdiff_t>(i));
    }

    bool wantsReload     = reloadKeyPressed.exchange(false);
    bool wantsFireSelect = fireSelectPressed.exchange(false);
    bool wantsInspect = inspectPressed.exchange(false);
    if (!inGameplay() || player->isSpectator()) return;
    if (!gun) {
        lastShotSucceeded = false;
        return;
    }
    if (wantsFireSelect && !stateLocked()) {
        if (auto id = displaySound(*gun, "fire_select")) sound::play(*id, 1.0f, 1.0f);
        net::FireSelectPacket{}.sendToServer();
        gun->state.fireMode = gun::nextFireMode(gun->entry->data.fireModes, gun->state.fireMode);
        predictedMode = PredictedMode{gun->state.gunId, player->getSelectedItemSlot(), gun->state.fireMode, Clock::now()};
        anim::fireSelect();
    }
    if (wantsReload) reload(*gun);
    // Inspect checks at execution (reload above can acquire), but never acquires.
    if (wantsInspect && !stateLocked()) {
        bool empty = gun->entry->data.bolt == pack::Bolt::OpenBolt ? gun->state.ammo <= 0 : !gun->state.barrel;
        if (auto id = displaySound(*gun, empty ? "inspect_empty" : "inspect")) sound::play(*id, 1.f, 1.f, true);
        anim::inspect();
    }

    if (!triggerDown) {
        lastShotSucceeded = false;
        dryFireArmed      = true;
        return;
    }
    bool continuous = gun::firesContinuously(gun->entry->data, gun->state.fireMode);
    if (!continuous && lastShotSucceeded) return;
    // Java's shoot attempts/cancellation run once per client tick (ShootKey.java:
    // 40-43, LocalPlayerMixin.java:34-36). Only cancel side effects are tick-gated;
    // retain frame-driven shooting and success-only semi-auto press consumption.
    if (gun::isReloading(reloadType()) && reloadCancelTickGate.tryAcquire(clientTickCount.load(std::memory_order_relaxed))) {
        net::CancelReloadPacket{}.sendToServer();
        anim::cancelReload();
    }
    if (shoot(*gun, nowMs(), continuous && lastShotSucceeded)) lastShotSucceeded = true;
}

class SyncedShooterDataHandler final : public ll::network::PacketHandlerBase<SyncedShooterDataHandler, net::SyncedShooterDataPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, net::SyncedShooterDataPacket const& packet) const {
        auto world = worldSession.current();
        if (!world) return;
        auto decoded = packet.transportError == gun::sync::DecodeError::None
            ? gun::sync::decode(packet.payload) : gun::sync::DecodeResult{packet.transportError, {}};
        if (!decoded) return; // preserve whole-packet rejection; acceptance sync logs retired
        std::lock_guard lock(serverStateMutex);
        if (worldSession.accepts(world)) serverDataInbox.push_back({std::move(decoded.message), world});
    }
};

void updateCameraRecoil() {
    if (!worldSession.current()) { recoil::clear(); return; }
    auto client = ll::service::getClientInstance();
    if (!client || &*client != joinedClient.load()) { recoil::clear(); return; }
    auto* player = client->getLocalPlayer();
    if (player) recoil::update(*client, *player);
    else recoil::clear();
}

// Main-thread, frame-accurate adapter. This is OUTSIDE MinecraftGame::update's native
// registry scopes; animations must not construct engine inventory objects here.
LL_TYPE_INSTANCE_HOOK(ShooterFrameHook, ll::memory::HookPriority::Normal, MinecraftGame, &MinecraftGame::$update, void) {
    origin();
    try {
        frame();
        updateCameraRecoil();
        anim::update();
        render::diagnoseFrame();
    } catch (std::exception const& e) {
        logger().error("Shooter frame failed: {}", e.what());
    } catch (...) {
        logger().error("Shooter frame failed");
    }
}

void clearSyncedData() {
    {
        std::lock_guard lock(serverStateMutex);
        serverDataInbox.clear();
    }
    syncedEntities.clear();
    localRuntimeId = 0;
    clientTickCount.store(0, std::memory_order_relaxed);
    reloadCancelTickGate.reset();
    autoBoltTickGate.reset();
    boltLatch.reset();
    reloadTypePublished = 0;
    aimingProgressPublished = 0;
    isAimingPublished = false;
    {
        std::lock_guard lock(stateLockMutex);
        stateLock.reset();
        lockSyncedState = {};
        lockWorld = 0;
    }
}

void endWorld(IClientInstance const* client) {
    if (!worldSession.current() || !client || joinedClient.load() != client) return;
    // Do not look up the primary client here: it can already have been detached from the
    // global service when exit callbacks arrive. Request-leave runs before that teardown.
    sound::stopAnimation();
    worldSession.close();
    clearSyncedData();
    turn::clear();
    nbt::releaseStackLimitProbe();
    hud::releaseWorld(client);
    recoil::requestClear();
    render::releaseWorld(client);
    joinedClient = nullptr;
    triggerDown = false;
    resetPending = true;
    for (auto& down : actionKeyDown) down = false;
    logger().info("[world] exit: queued work invalidated; GPU cache released");
}

// Covers both synchronous and async leave requests before native world/resource teardown;
// ClientExitLevelEvent remains an idempotent fallback (not all leave paths logged it in 0.3.8).
LL_TYPE_INSTANCE_HOOK(LeaveWorldHook, ll::memory::HookPriority::Normal, ClientInstance,
    &ClientInstance::_requestLeaveGameImpl, void, bool switchScreen, bool sync) {
    endWorld(this);
    origin(switchScreen, sync);
}

bool                                  hooked = false;
bool leaveHooked = false;
std::array<ll::event::ListenerPtr, 6> listeners;

} // namespace

gun::ReloadStateType reloadState() { return static_cast<gun::ReloadStateType>(reloadTypePublished.load()); }
float syncedAimingProgress() { return worldSession.current() ? aimingProgressPublished.load() : 0; }
bool syncedIsAiming() { return worldSession.current() && isAimingPublished.load(); }
uint64_t worldToken() { return worldSession.current(); }
bool ownsClient(IClientInstance const* client) { return worldSession.current() && client && joinedClient.load()==client; }

AnimationTiming animationTiming() {
    AnimationTiming result;
    if (auto gun = heldGun(true)) {
        result.interval = gun::triggerIntervalMs(gun->entry->data, gun->state.fireMode);
        result.cooldown = gun::clientCooldownMs(gun->entry->data, gun->state.fireMode, nowMs(), lastShot);
        result.fireMode = gun->state.fireMode ? static_cast<int>(*gun->state.fireMode) : 3;
    }
    result.lastShootTimestamp = previousShotTimestamp.value_or(-1);
    return result;
}
void adjustShootInterval(int64_t delta) {
    for (auto t : {lastShot, shotTimestamp}) {
        if (t && ((delta > 0 && *t > std::numeric_limits<int64_t>::max() - delta)
            || (delta < 0 && *t < std::numeric_limits<int64_t>::min() - delta))) {
            throw std::overflow_error("animation shoot timestamp overflow");
        }
    }
    if (lastShot) *lastShot += delta;
    if (shotTimestamp) *shotTimestamp += delta;
}

void enable() {
    auto& bus = ll::event::EventBus::getInstance();

    listeners[0] = bus.emplaceListener<ll::event::input::MouseInputEvent>([](ll::event::input::MouseInputEvent& event) {
        if (event.actionButtonId() == MouseAction::ActionRight) {
            bool gameplay = inGameplay();
            if (aim::mouse(event.buttonData() == MouseAction::DataDown, gameplay, gameplay && heldGun().has_value())) event.cancel();
            return;
        }
        if (event.actionButtonId() != MouseAction::ActionLeft) return;
        bool down = event.buttonData() == MouseAction::DataDown;
        if (!down) triggerDown = false; // always release, even if the gun was put away meanwhile
        if (!inGameplay() || !hasMainHandGun()) return;
        // Both edges are swallowed while a gun is held (verified in 0.1.x: no mining, no attack).
        event.cancel();
        if (down) triggerDown = true;
    });

    // TACZ's default keys: G fire select, R reload, H inspect.
    listeners[1] = bus.emplaceListener<ll::event::input::KeyInputEvent>([](ll::event::input::KeyInputEvent& event) {
        int key = event.keyCode();
        if (key != 'G' && key != 'R' && key != 'H') return;
        bool wasDown = actionKeyDown[key == 'G' ? 0 : key == 'R' ? 1 : 2].exchange(event.isDown());
        if (!event.isDown() || event.controller().mTextboxIsFocused) return;
        if (!inGameplay() || !heldGun()) return;
        event.cancel();
        if (wasDown) return;
        (key == 'G' ? fireSelectPressed : key == 'H' ? inspectPressed : reloadKeyPressed) = true;
    });

    listeners[2] = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](ll::event::ClientExitLevelEvent& event) {
        endWorld(&event.self());
    });
    listeners[3] = bus.emplaceListener<ll::event::ClientLevelTickEvent>([](ll::event::ClientLevelTickEvent&) {
        aim::tick();
        tickStateLock(); // LocalPlayerMixin.java:109-113; real client tick, never frame cadence
        if (worldSession.current()) clientTickCount.fetch_add(1, std::memory_order_release);
        turn::tick(); // one fixed bob step per real client tick, even without a gun
        anim::captureMovementTick();
    });
    listeners[4] = bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](ll::event::ClientJoinLevelEvent& event) {
        auto client = ll::service::getClientInstance();
        if (!client || &*client != &event.self()) return;
        // Also retire a previous generation if a native transfer bypassed the leave event.
        endWorld(joinedClient.load());
        joinedClient = &event.self();
        uint64_t world;
        {
            std::lock_guard lock(serverStateMutex);
            serverDataInbox.clear();
            world = worldSession.open();
        }
        resetPending = true;
        logger().info("[world] local player loaded: generation {}", world);
    });
    listeners[5] = bus.emplaceListener<ll::event::ClientStartJoinLevelEvent>([](ll::event::ClientStartJoinLevelEvent& event) {
        endWorld(&event.self());
    });

    hooked = ShooterFrameHook::hook() == 0;
    aim::enable();
    recoil::enable();
    leaveHooked = LeaveWorldHook::hook() == 0;
    if (!leaveHooked) logger().warn("Cannot hook request-leave; event and GPU-generation fallbacks remain active");
    if (!hooked) logger().warn("Cannot hook MinecraftGame::update; guns cannot fire");
}

void disable() {
    aim::disable();
    if (hooked) ShooterFrameHook::unhook();
    if (leaveHooked) LeaveWorldHook::unhook();
    recoil::disable();
    hooked = false;
    leaveHooked = false;
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) {
        if (listener) bus.removeListener(listener);
        listener = nullptr;
    }
    triggerDown  = false;
    sound::stopAnimation();
    worldSession.close();
    clearSyncedData();
    turn::clear();
    joinedClient = nullptr;
    resetPending = true;
    anim::clear();
}

} // namespace tacz::client::shooter

#include "server/Shooter.h"
#include "server/HitInput.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <cmath>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerRespawnEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/Bedrock.h"

#include "mc/legacy/ActorUniqueID.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeModifier.h"
#include "mc/world/attribute/MutableAttributeWithContext.h"
#include "mc/world/attribute/SharedAttributes.h"

#include "gun/Reload.h"
#include "gun/DrawRules.h"
#include "gun/ShootRules.h"
#include "gun/ShooterCooldowns.h"
#include "gun/Sprint.h"
#include "model/Aiming.h"
#include "lua/GunLogic.h"
#include "mod/TaczMod.h"
#include "nbt/AmmoItem.h"
#include "nbt/GunItem.h"
#include "net/GunPackets.h"
#include "server/BulletSystem.h"
#include "server/HitDiagnostic.h"

namespace tacz::server::shooter {
namespace {

using Clock = std::chrono::steady_clock;

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

int64_t serverNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

int64_t boltNowMs() {
    // getBoltTime uses Java's wall-clock elapsed milliseconds, not shot timestamps.
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ShooterDataHolder, reduced to what is implemented.
struct ShooterState {
    unsigned recoilLookSamples = 0;
    bool isAiming = false;
    float aimingProgress = 0;
    int64_t aimingTimestamp = -1;
    float sprintTimeS = 0;
    std::optional<Clock::time_point> sprintTimestamp;
    gun::ShootSprintIntent shootSprintIntent;
    Mob const* sprintOwner = nullptr; // comparison only, including actor replacement/removal
    gun::HandSelection selection;
    gun::DrawCooldown draw;
    std::optional<int64_t> clockOffset; // server ms - client ms, set on the first trigger pull
    std::optional<int64_t> lastShot;    // client timestamp of the last accepted trigger pull
    int64_t previousShot = -1;
    int64_t initialWallBase = boltNowMs();
    int64_t heatTimestamp = -1;
    uint64_t selectionGeneration = 0;

    // Reload (LivingEntityReload / ShooterDataHolder.reloadTimestamp / reloadStateType).
    gun::ReloadStateType reloadState      = gun::ReloadStateType::NotReloading;
    int64_t              reloadStartedMs  = -1;
    int                  reloadSlot       = -1;
    pack::ResourceId     reloadGunId;
    bool                 isBolting = false;
    int64_t              boltTimestamp = -1;
    gun::ReloadState     reloadValue; // full tickReload result, including countdown
    gun::sync::Baseline  syncBaseline;
    std::optional<uint64_t> publishedRuntimeId;
    std::optional<int> observedDimension;
};

std::mutex                                 mutex;
std::unordered_map<int64_t, ShooterState> states; // by ActorUniqueID
std::unique_ptr<lua::GunLogic>             logic;
// Comparison-only SERVER actor identities. Separate lock: native callbacks may
// run from inside shooter code already holding mutex. Never dereference an owner.
std::mutex sprintMutex;
std::unordered_set<Mob const*> sprintBlockedOwners;
std::unordered_set<int64_t> sprintLifeResets;
bool sprintHooked=false;
// Event callbacks may run inside shooter actions. Queue only, without taking mutex.
std::mutex syncQueueMutex;
std::unordered_map<int64_t, char const*> pendingFullSync;
void queueFullSync(Player const& player, char const* reason) {
    std::lock_guard lock(syncQueueMutex);
    pendingFullSync[player.getOrCreateUniqueID().rawID] = reason;
}
void sprintPolicy(Player& player,bool blocked) {
    std::lock_guard lock(sprintMutex);
    if (blocked) sprintBlockedOwners.insert(static_cast<Mob*>(&player));
    else sprintBlockedOwners.erase(static_cast<Mob*>(&player));
}
LL_TYPE_INSTANCE_HOOK(ServerAimSprint,ll::memory::HookPriority::Normal,Mob,&Mob::$setSprinting,void,bool value) {
    if (value) {
        std::lock_guard lock(sprintMutex);
        if (sprintBlockedOwners.contains(this)) value=false;
    }
    origin(value); // reject native server re-entry, not just the later tick correction
}

std::array<ll::event::ListenerPtr, 6> listeners;

int64_t       keyOf(Player const& player) { return player.getOrCreateUniqueID().rawID; }
ShooterState& stateOf(Player const& player) { return states[keyOf(player)]; }

Player* senderOf(NetworkIdentifier const& netId, SubClientId subId) {
    auto handler = ll::service::getServerNetworkHandler();
    return handler ? handler->_getServerPlayer(netId, subId) : nullptr;
}

pack::GunEntry const* gunEntry(pack::ResourceId const& id) { return TaczMod::getInstance().gunPacks().gun(id); }

lua::GunLogic& gunLogic() {
    if (!logic) {
        logic = std::make_unique<lua::GunLogic>(TaczMod::getInstance().gunPacks(), [](std::string const& msg) {
            logger().warn("{}", msg);
        });
    }
    return *logic;
}

Player* taskPlayer(int64_t key) {
    auto level = ll::service::getLevel();
    auto actor = level ? level->fetchEntity(ActorUniqueID{key}, false) : nullptr;
    return actor && actor->isPlayer() ? static_cast<Player*>(actor) : nullptr;
}

struct HeldContext {
    nbt::GunItemState item;
    std::shared_ptr<CompoundTag> tag;
    gun::HoldIdentity identity;
    debug::hit::Tag diagnostic;
};

bool sameHold(int64_t key, HeldContext const& context) {
    auto found = states.find(key);
    auto player = taskPlayer(key);
    if (!player || found == states.end()) return false;
    auto item = nbt::readGun(player->getSelectedItem());
    return item && context.identity == gun::HoldIdentity{found->second.selectionGeneration, player->getSelectedItemSlot(), item->gunId};
}

bool fireRound(int64_t key, std::shared_ptr<HeldContext> const& context,
    pack::GunData const& data, std::optional<pack::FireMode> mode, float inaccuracy, glm::dvec3 inaccuracyMovement, bool consume);

// Every retained closure owns its original item state and resolves the player by
// key. No player reference, state reference or selected-item NBT pointer escapes.
lua::ScriptApi scriptApi(Player& player, nbt::GunItemState const& item, pack::GunEntry const& entry, ShooterState const& s, debug::hit::Tag diagnostic = {}) {
    lua::ScriptApi api;
    auto key = keyOf(player);
    auto context = std::make_shared<HeldContext>(HeldContext{item, {}, {s.selectionGeneration, player.getSelectedItemSlot(), item.gunId}, diagnostic});
    if (auto tag = player.getSelectedItem().mUserData.get()) context->tag = tag->clone();
    api.gunId          = item.gunId;
    api.data           = &entry.data;
    api.ammo           = {item.ammo, item.barrel};
    api.maxAmmo        = entry.data.ammoAmount;
    api.magExtentLevel = 0; // extended magazines come with attachments
    api.fireMode       = item.fireMode;
    api.needCheckAmmo  = !player.isCreative();
    api.consumesAmmo   = true; // TACZ GunConfig.CREATIVE_PLAYER_CONSUME_AMMO default
    api.reloadState    = s.reloadState;
    api.nowMs          = serverNowMs();
    api.reloadTimeMs   = s.reloadStartedMs < 0 ? 0 : api.nowMs - s.reloadStartedMs;
    api.shooterKey     = keyOf(player);
    api.aimingProgress = s.aimingProgress;
    api.isBolting     = s.isBolting;
    api.boltTimestamp = s.boltTimestamp;
    api.boltClock     = boltNowMs;
    api.readNbt = [context](auto const& path, auto const& key, int mode) { return nbt::readNbt(context->tag.get(), path, key, mode); };
    auto ammoId        = entry.data.ammoId;
    api.extractAmmo = [key, ammoId](int n) { auto p = taskPlayer(key); return p && ammoId ? nbt::extractAmmo(*p, *ammoId, n) : 0; };
    api.inventoryHasAmmo = [key, ammoId] { auto p = taskPlayer(key); return p && ammoId && nbt::countAmmo(*p, *ammoId) > 0; };
    api.alive = [key] { auto p = taskPlayer(key); return p && p->isAlive() && !p->isDead(); };
    api.sameHold = [key, context] { return sameHold(key, *context); };
    api.wallClock = boltNowMs;
    api.previousShot = [key] {
        auto found = states.find(key);
        if (found == states.end()) return int64_t{-1};
        auto const& state = found->second;
        return gun::addTime(state.previousShot, gun::shotWallBase(state.clockOffset, boltNowMs(), serverNowMs(), state.initialWallBase));
    };
    api.adjustShot = [key](int64_t alpha) {
        if (auto found = states.find(key); found != states.end())
            found->second.lastShot = gun::addTime(found->second.lastShot.value_or(-1), alpha);
    };
    api.refresh = [key, context](lua::ScriptApi& a) {
        auto p = taskPlayer(key);
        if (p && sameHold(key, *context)) {
            context->item = *nbt::readGun(p->getSelectedItem());
            auto tag = p->getSelectedItem().mUserData.get();
            context->tag = tag ? std::shared_ptr<CompoundTag>(tag->clone()) : nullptr;
        }
        a.ammo = {context->item.ammo, context->item.barrel};
        a.fireMode = context->item.fireMode;
        if (p) a.needCheckAmmo = !p->isCreative();
        if (auto found = states.find(key); found != states.end()) {
            auto const& state = found->second;
            a.reloadState = state.reloadState;
            a.reloadTimeMs = state.reloadStartedMs < 0 ? 0 : serverNowMs() - state.reloadStartedMs;
            a.aimingProgress = state.aimingProgress;
            a.isBolting = state.isBolting;
            a.boltTimestamp = state.boltTimestamp;
        }
    };
    api.commit = [key, context](lua::ScriptApi const& a) {
        bool changed = a.ammo != gun::GunAmmo{context->item.ammo, context->item.barrel};
        context->item.ammo = a.ammo.magazine;
        context->item.barrel = a.ammo.barrel;
        if (changed && sameHold(key, *context)) if (auto p = taskPlayer(key)) nbt::writeSelectedGun(*p, context->item);
        if (auto found = states.find(key); found != states.end()) found->second.boltTimestamp = a.boltTimestamp;
    };
    api.prepareRound = [key, context](lua::ScriptApi& a, bool consume) -> std::function<bool()> {
        auto p = taskPlayer(key);
        if (!p) return {};
        auto delta = p->getPosDelta();
        // Step 2B remains undecided: preserve the native-motion input, but carry
        // this exact sample through the round closure for truthful diagnostics.
        glm::dvec3 inaccuracyMovement{delta.x, delta.y, delta.z};
        auto type = gun::inaccuracyType(model::fullyAimed(a.aimingProgress), p->isSneaking(), glm::length(inaccuracyMovement));
        float inaccuracy = gun::inaccuracy(*a.data, a.fireMode, type);
        // Shared immutable registry object, as in Java (not a per-shot copy).
        // reloadGunPacks is called only by mod load, before shooter enable;
        // all tasks are destroyed on world end/disable. Runtime pack reload is
        // deferred and must introduce snapshot ownership before changing this.
        auto const* data = a.data;
        return [key, context, data, mode = a.fireMode, inaccuracy, inaccuracyMovement, consume] {
            return fireRound(key, context, *data, mode, inaccuracy, inaccuracyMovement, consume);
        };
    };
    return api;
}

void stopReload(Player& player, ShooterState& s, char const* why);

void applySprintPolicy(Player& player, ShooterState& s) {
    bool valid = player.isAlive() && !player.isDead() && !player.isSpectator()
        && nbt::readGun(player.getSelectedItem()).has_value();
    if (!valid) s.shootSprintIntent.update(false);
    bool blocked = valid && gun::blocksSprint(s.shootSprintIntent.held, s.isAiming, s.reloadState);
    sprintPolicy(player, blocked);
    if (blocked) player.setSprinting(false);
}

void queueShootSprintReset(Player& player) {
    // Death can occur inside a shot while mutex is held. Consume at the next
    // tick/action observation, without re-entering shooter state from an event.
    std::lock_guard lock(sprintMutex);
    sprintLifeResets.insert(keyOf(player));
}

// Called from tick and every action, before using a previous gun's state. Slot is part of
// identity so switching between two stacks of the same GunId still starts a fresh draw.
void observeSelection(Player& player, ShooterState& s) {
    auto dimension = static_cast<int>(player.getDimensionId());
    // LL 26.51.9 has no typed player dimension-change event. Observation is also
    // used by actions, but publication always waits for the next shooter tick.
    if (s.observedDimension && *s.observedDimension != dimension) {
        // Scope bind discards old-dimension input, but preserves any fresh input
        // already captured in the new dimension before this observation.
        (void)hitinput::snapshot(player);
        queueFullSync(player, "dimension");
    }
    s.observedDimension = dimension;
    auto item = nbt::readGun(player.getSelectedItem());
    gun::HandSelection next{item ? std::optional{item->gunId} : std::nullopt, player.getSelectedItemSlot()};
    {
        std::lock_guard lock(sprintMutex);
        if (sprintLifeResets.erase(keyOf(player))) {
            s.shootSprintIntent = {};
            if (logic) logic->clearTasks(keyOf(player));
        }
        if (s.sprintOwner != static_cast<Mob*>(&player)) {
            sprintBlockedOwners.erase(s.sprintOwner);
            s.sprintOwner = static_cast<Mob*>(&player);
        }
    }
    s.shootSprintIntent.observe({reinterpret_cast<uintptr_t>(&player), static_cast<int>(player.getDimensionId()),
        player.isAlive() && !player.isDead() && !player.isSpectator() && item.has_value()}, next != s.selection);
    if (next == s.selection) return;
    ++s.selectionGeneration;
    gunLogic().clearScriptData(keyOf(player));
    gunLogic().cancelShots(keyOf(player));
    s.heatTimestamp = -1; // initialData, then LivingEntityDrawGun.draw wall time
    s.heatTimestamp = boltNowMs();
    stopReload(player, s, "weapon switched");
    s.isBolting = false; // ShooterDataHolder.initialData, not an extra death cancel
    s.boltTimestamp = -1;
    s.lastShot.reset();
    s.isAiming = false;
    s.aimingProgress = 0;
    s.aimingTimestamp = serverNowMs();
    // ShooterDataHolder.initialData also resets sprint recovery on selection.
    s.sprintTimeS = 0;
    s.sprintTimestamp.reset();
    auto entry = item ? gunEntry(item->gunId) : nullptr;
    s.draw.select(serverNowMs(), entry ? gun::animationMillis(entry->data.putAwayTime) : 0);
    s.selection = next;
    applySprintPolicy(player, s);
}

bool drawing(ShooterState const& s, pack::GunEntry const& entry) {
    return s.draw.remaining(serverNowMs(), gun::animationMillis(entry.data.drawTime)) > 0;
}

// One round of the trigger pull: ammo, then the bullets. False ends the pull (TACZ's cycle
// task returns false when the shooter died, switched weapon or ran dry).
bool fireRound(int64_t key, std::shared_ptr<HeldContext> const& context,
    pack::GunData const& data, std::optional<pack::FireMode> mode, float inaccuracy, glm::dvec3 inaccuracyMovement, bool consume) {
    auto p = taskPlayer(key);
    if (!p || !p->isAlive() || p->isDead() || !sameHold(key, *context)) return false;
    auto& player = *p;
    auto item = nbt::readGun(player.getSelectedItem());
    if (!item || !gunEntry(item->gunId)) return false;

    gun::GunAmmo ammo{item->ammo, item->barrel};
    if (consume && !gun::reduceAmmoOnce(*data.bolt, ammo)) return false;
    item->ammo   = ammo.magazine;
    item->barrel = ammo.barrel;
    if (consume) nbt::writeSelectedGun(player, *item);
    context->item = *item;
    bullet::fire(player,item->gunId, data, mode, inaccuracy, inaccuracyMovement, context->diagnostic);
    return true;
}

// LivingEntityShoot.shoot. Returns a reason when the trigger pull is refused.
char const* handleShoot(Player& player, int64_t timestamp, uint64_t packetClientTick) {
    auto item = nbt::readGun(player.getSelectedItem());
    if (!item) return "NOT_GUN";
    auto const* entry = gunEntry(item->gunId);
    if (!entry) return "ID_NOT_EXIST";
    auto const& data = entry->data;

    std::lock_guard lock(mutex);
    auto&           s = stateOf(player);
    observeSelection(player, s);
    if (drawing(s, *entry)) return "IS_DRAWING";
    if (gunLogic().pendingShots(keyOf(player))) return "COOL_DOWN";
    if (gun::serverCooldownMs(data, item->fireMode, timestamp, s.lastShot) > 0) return "COOL_DOWN";

    // TACZ SERVER_SHOOT_NETWORK_V: the client clock must stay within -300 / +300 ms + 2 ticks of
    // the server's. A jump re-syncs the offset and drops this pull.
    int64_t now = serverNowMs();
    if (!s.clockOffset) s.clockOffset = now - timestamp;
    int64_t alpha = now - *s.clockOffset - timestamp;
    if (alpha < -300 || alpha > 300 + 100) {
        s.clockOffset = now - timestamp;
        return "NETWORK_FAIL";
    }
    if (gun::isReloading(s.reloadState)) return "IS_RELOADING";
    if (gun::boltBlocksAction(s.isBolting)) return "IS_BOLTING";
    if (gun::sprintBlocksShot(s.sprintTimeS)) return "IS_SPRINTING";

    gun::GunAmmo ammo{item->ammo, item->barrel};
    switch (gun::prepareShot(*data.bolt, ammo)) {
    case gun::ShotCheck::NoAmmo:
        return "NO_AMMO";
    case gun::ShotCheck::NeedBolt:
        return "NEED_BOLT";
    case gun::ShotCheck::Ok:
        break;
    }
    if (ammo.magazine != item->ammo || ammo.barrel != item->barrel) {
        item->ammo   = ammo.magazine;
        item->barrel = ammo.barrel;
        nbt::writeSelectedGun(player, *item);
    }

    s.previousShot = s.lastShot.value_or(-1);
    s.heatTimestamp = boltNowMs();
    s.lastShot = timestamp;
    auto api = scriptApi(player, *item, *entry, s, hitdiag::trigger(player, timestamp, packetClientTick));
    gunLogic().shoot(api);
    api.commitView();
    // Bounded trigger observation, not a client rotation override.
    if (s.recoilLookSamples++ < 8) {
        auto const& rotation = player.getRotation();
        logger().info("[recoil-server-look] {} triggerTs={} rotation=({:.6f},{:.6f}) aim={:.3f} rotationSource=server-player (observe; compare client traces, no forced server recoil)",
            player.getRealName(), timestamp, rotation.x, rotation.z, s.aimingProgress);
    }
    return nullptr;
}

// AbstractGunItem.canReload: not full, and (outside creative) ammo in the inventory.
bool canReload(Player& player, nbt::GunItemState const& item, pack::GunEntry const& entry) {
    auto const& data = entry.data;
    if (item.ammo >= data.ammoAmount) return false;
    if (data.reload.type == pack::FeedType::Inventory) return false;
    if (player.isCreative() || data.reload.infinite) return true;
    return data.ammoId && nbt::countAmmo(player, *data.ammoId) > 0;
}

// LivingEntityReload.reload. Returns a reason when the request is refused.
char const* handleReload(Player& player) {
    auto item = nbt::readGun(player.getSelectedItem());
    if (!item) return "NOT_GUN";
    auto const* entry = gunEntry(item->gunId);
    if (!entry) return "ID_NOT_EXIST";
    auto const& data = entry->data;
    if (data.reload.type == pack::FeedType::Inventory) return "INVENTORY_FED";

    std::lock_guard lock(mutex);
    auto&           s = stateOf(player);
    observeSelection(player, s);
    if (drawing(s, *entry)) return "IS_DRAWING";
    if (gun::isReloading(s.reloadState)) return "ALREADY_RELOADING";
    // LivingEntityReload uses the same getter as synced publication, not receipt
    // time. Pending burst rounds remain an explicitly retained port restriction.
    if (gun::syncedShootCooldownMs(true, &data, item->fireMode, serverNowMs(), s.clockOffset, s.lastShot) != 0) {
        return "COOL_DOWN";
    }
    if (gunLogic().pendingShots(keyOf(player))) return "COOL_DOWN";
    if (gun::boltBlocksAction(s.isBolting)) return "IS_BOLTING";
    if (!canReload(player, *item, *entry)) return "CANNOT_RELOAD";

    gun::GunAmmo ammo{item->ammo, item->barrel};
    bool         empty = gun::availableRounds(*data.bolt, ammo) <= 0;
    s.reloadState      = empty ? gun::ReloadStateType::EmptyFeeding : gun::ReloadStateType::TacticalFeeding;
    s.reloadStartedMs  = serverNowMs();
    s.reloadSlot       = player.getSelectedItemSlot();
    s.reloadGunId      = item->gunId;
    s.reloadValue      = {s.reloadState, 0};

    auto api = scriptApi(player, *item, *entry, s);
    auto started = gunLogic().startReload(api);
    // Item-method mutations and cache writes commit during dispatch, even on
    // errors. Copy the adjusted timestamp back on every outcome as before.
    s.boltTimestamp = api.boltTimestamp;
    if (started == lua::ReloadStartResult::Refused) {
        s.reloadState     = gun::ReloadStateType::NotReloading;
        s.reloadStartedMs = -1;
        s.reloadValue     = {};
        return "SCRIPT_REFUSED";
    }
    // Error is not success, but Java's exception skips the reset above: keep
    // the initialized state/timestamp so tick_reload can run on following ticks.
    logger().info(
        "[reload] {} {} reload of {} ({}{} rounds)",
        player.getRealName(),
        empty ? "empty" : "tactical",
        item->gunId.toString(),
        item->ammo,
        gun::hasChamberedRound(data.bolt,item->barrel) ? "+1" : ""
    );
    return nullptr;
}

// ShooterDataHolder.initialData for the reload part, after a weapon switch or cancel.
void stopReload(Player& player, ShooterState& s, char const* why) {
    if (!gun::isReloading(s.reloadState)) return;
    s.reloadState     = gun::ReloadStateType::NotReloading;
    s.reloadStartedMs = -1;
    s.reloadValue     = {};
    logger().info("[reload] {} reload stopped: {}", player.getRealName(), why);
}

// LivingEntityReload.cancelReload -> ModernKineticGunItem.interruptReload. Scripts decide what
// interrupting means (shell-by-shell guns stop after the current shell); TACZ's default does
// nothing, so a magazine reload keeps running.
void handleCancelReload(Player& player) {
    auto item = nbt::readGun(player.getSelectedItem());
    if (!item) return;
    auto const* entry = gunEntry(item->gunId);
    if (!entry) return;
    std::lock_guard lock(mutex);
    auto&           s = stateOf(player);
    observeSelection(player, s);
    if (!gun::isReloading(s.reloadState)) return;
    auto api = scriptApi(player, *item, *entry, s);
    gunLogic().interruptReload(api);
    s.boltTimestamp = api.boltTimestamp;
}

// LivingEntityReload.tickReloadState, once per server tick.
void tickReload(Player& player, ShooterState& s) {
    if (!gun::isReloading(s.reloadState)) { s.reloadValue = {}; return; }
    auto item = nbt::readGun(player.getSelectedItem());
    // Switching away from the gun resets the shooter (LivingEntityDrawGun -> initialData).
    if (!item || item->gunId != s.reloadGunId || player.getSelectedItemSlot() != s.reloadSlot) {
        stopReload(player, s, "weapon switched");
        return;
    }
    auto const* entry = gunEntry(item->gunId);
    if (!entry) {
        stopReload(player, s, "gun data gone");
        return;
    }
    auto api  = scriptApi(player, *item, *entry, s);
    auto next = gunLogic().tickReload(api);
    s.boltTimestamp = api.boltTimestamp;
    if (api.ammo != gun::GunAmmo{item->ammo, item->barrel}) {
        item->ammo   = api.ammo.magazine;
        item->barrel = api.ammo.barrel;
        nbt::writeSelectedGun(player, *item);
        logger().info("[reload] {} loaded: {}{} rounds", player.getRealName(), item->ammo, gun::hasChamberedRound(entry->data.bolt, item->barrel) ? "+1" : "");
    }
    // LivingEntityMixin.java:223,237 publishes tickReloadState's WHOLE result.
    s.reloadValue = gun::isReloading(next.type) ? next : gun::ReloadState{};
    s.reloadState = next.type;
    if (!gun::isReloading(next.type)) {
        s.reloadStartedMs = -1;
        logger().info("[reload] {} reload finished", player.getRealName());
    }
}

// Both hook paths commit mutations even when a hook returns false or errors.
void commitBoltAmmo(Player& player, nbt::GunItemState& item, lua::ScriptApi const& api) {
    if (api.ammo == gun::GunAmmo{item.ammo, item.barrel}) return;
    bool chambered = !item.barrel && api.ammo.barrel;
    item.ammo = api.ammo.magazine;
    item.barrel = api.ammo.barrel;
    nbt::writeSelectedGun(player, item); // same balanced NBT path as tickReload
    if (chambered) logger().info("[bolt] {} chambered", player.getRealName());
}

// LivingEntityBolt.java:27-78. Empty request; all state comes from server item/data.
char const* handleBolt(Player& player) {
    auto item = nbt::readGun(player.getSelectedItem());
    if (!item) return "NOT_GUN";
    auto entry = gunEntry(item->gunId);
    if (!entry) return "ID_NOT_EXIST";
    std::lock_guard lock(mutex);
    auto& s = stateOf(player);
    observeSelection(player, s);
    auto api = scriptApi(player, *item, *entry, s);
    gun::BoltCheck check;
    check.shootCooldown = gun::syncedShootCooldownMs(true, &entry->data, item->fireMode, serverNowMs(), s.clockOffset, s.lastShot);
    check.drawCooldown = gun::syncedDrawCooldownMs(true, &entry->data, s.draw, serverNowMs());
    check.reload = s.reloadState;
    check.serverBolting = s.isBolting;
    check.type = entry->data.bolt;
    check.ammo = api.ammo;
    check.inventoryFed = api.useInventoryAmmo();
    check.inventoryHasAmmo = api.inventoryHasAmmo && api.inventoryHasAmmo();
    check.needCheckAmmo = api.needCheckAmmo;
    if (auto reason = gun::serverBoltRefusal(check)) return reason;
    s.boltTimestamp = boltNowMs();
    api.boltTimestamp = s.boltTimestamp; // start_bolt sees the timestamp, but isBolting is still false
    s.isBolting = gunLogic().startBolt(api);
    s.boltTimestamp = api.boltTimestamp;
    commitBoltAmmo(player, *item, api); // start mutations persist regardless of its result
    if (!s.isBolting) {
        logger().info("[bolt] {} start_bolt returned false", player.getRealName());
        return nullptr;
    }
    logger().info("[bolt] {} started {}", player.getRealName(), item->gunId.toString());
    return nullptr;
}

void tickBolt(Player& player, ShooterState& s) {
    if (!s.isBolting) return;
    auto item = nbt::readGun(player.getSelectedItem());
    auto entry = item ? gunEntry(item->gunId) : nullptr;
    if (!entry) s.isBolting = false;
    else {
        auto api = scriptApi(player, *item, *entry, s);
        s.isBolting = gunLogic().tickBolt(api);
        s.boltTimestamp = api.boltTimestamp;
        commitBoltAmmo(player, *item, api);
    }
    if (!s.isBolting) logger().info("[bolt] {} finished", player.getRealName());
}

void tickHeat(Player& player, ShooterState& s) {
    auto item = nbt::readGun(player.getSelectedItem());
    auto entry = item ? gunEntry(item->gunId) : nullptr;
    if (!entry || !entry->data.script) return; // no hook and default cooling is deferred
    auto api = scriptApi(player, *item, *entry, s);
    gunLogic().tickHeat(api, s.heatTimestamp);
    api.commitView();
}

// All native value arguments here are fully defined SDK types, never an opaque container.
static_assert(sizeof(AttributeModificationContext) == 8);
static_assert(sizeof(MutableAttributeWithContext) == 16);
static_assert(sizeof(mce::UUID) == 16);
static_assert(std::is_same_v<decltype(&Actor::getMutableAttribute), MutableAttributeWithContext (Actor::*)(Attribute const&)>);
static_assert(std::is_same_v<decltype(&Mob::$setSprinting), void (Mob::*)(bool)>);
using AddModifier = void (AttributeInstance::*)(AttributeModifier const&, AttributeModificationContext);
static_assert(std::is_same_v<decltype(static_cast<AddModifier>(&AttributeInstance::addModifier)), AddModifier>);

mce::UUID const& speedModifierId() {
    static auto id = mce::UUID::fromString("4d5696ae-a7c5-c59c-80e9-2a2dc8373c46");
    return id; // TACZ EXTRA_SPEED_MODIFIER_UUID; transient, not a base-value overwrite
}
void applyMovement(Player& player, std::optional<float> amount) {
    auto attribute = player.getMutableAttribute(SharedAttributes::MOVEMENT_SPEED());
    auto* instance = attribute.mInstance->mPtr;
    if (!instance || !attribute.mContext->mAttributeMap) return;
    auto const& modifiers = instance->mModifierList.get();
    auto existing = std::find_if(modifiers.begin(), modifiers.end(), [](auto const& m) { return m.mId.get() == speedModifierId(); });
    if (!amount || *amount == 0) {
        if (existing != modifiers.end()) instance->removeModifier(speedModifierId(), attribute.mContext.get());
        return;
    }
    if (existing != modifiers.end() && existing->mAmount == *amount) return;
    if (existing != modifiers.end()) instance->removeModifier(speedModifierId(), attribute.mContext.get());
    AttributeModifier modifier{speedModifierId(), "TACZ extra gun speed", *amount,
        AttributeModifierOperation::OperationMultiplyTotal, AttributeOperands::OperandCurrent, false};
    instance->addModifier(modifier, attribute.mContext.get());
}
void tickAim(Player& player, ShooterState& s) {
    auto item = nbt::readGun(player.getSelectedItem());
    auto entry = item ? gunEntry(item->gunId) : nullptr;
    auto now = serverNowMs();
    if (!entry || !player.isAlive() || player.isSpectator()) {
        s.isAiming = false;
        s.aimingProgress = 0;
        applyMovement(player, {});
    } else {
        s.aimingProgress = model::advanceAimProgress(s.aimingProgress, s.isAiming,
            s.aimingTimestamp < 0 ? 50 : std::max<int64_t>(0, now - s.aimingTimestamp),
            model::effectiveAdsTime(entry->data.aimTime)); // ads cache base until attachments are implemented
        applyMovement(player, model::movementModifier(entry->data.movementSpeed, s.isAiming, gun::isReloading(s.reloadState)));
    }
    applySprintPolicy(player, s); // includes valid held-trigger intent even without known gun data
    s.aimingTimestamp = now;
}

void tickSprint(Player& player, ShooterState& s) {
    auto item = nbt::readGun(player.getSelectedItem());
    auto entry = item ? gunEntry(item->gunId) : nullptr;
    auto now = Clock::now();
    float elapsed = s.sprintTimestamp ? std::chrono::duration<float>(now - *s.sprintTimestamp).count() : 0;
    s.sprintTimestamp = now;
    s.sprintTimeS = gun::advanceSprintTime(s.sprintTimeS, elapsed,
        entry ? std::optional{entry->data.sprintTime} : std::nullopt, player.isSprinting(), player.isSneaking());
}

void publishShooterData(Player& player, ShooterState& s) {
    auto runtimeId = player.getRuntimeID().rawID;
    if (s.publishedRuntimeId != runtimeId) {
        s.syncBaseline = {};
        s.publishedRuntimeId = runtimeId;
    }
    gun::sync::Snapshot snapshot;
    auto item = nbt::readGun(player.getSelectedItem());
    auto entry = item ? gunEntry(item->gunId) : nullptr;
    auto data = entry ? &entry->data : nullptr;
    auto now = serverNowMs();
    snapshot.set({gun::sync::Key::ShootCoolDown, gun::syncedShootCooldownMs(item.has_value(), data,
        item ? item->fireMode : std::nullopt, now, s.clockOffset, s.lastShot)});
    snapshot.set({gun::sync::Key::MeleeCoolDown, gun::syncedMeleeCooldownMs(item.has_value(), data)});
    snapshot.set({gun::sync::Key::ReloadState, s.reloadValue});
    snapshot.set({gun::sync::Key::AimingProgress, s.aimingProgress});
    snapshot.set({gun::sync::Key::DrawCoolDown, gun::syncedDrawCooldownMs(item.has_value(), data, s.draw, now)});
    snapshot.set({gun::sync::Key::IsAiming, s.isAiming});
    snapshot.set({gun::sync::Key::SprintTime, s.sprintTimeS});
    snapshot.set({gun::sync::Key::IsBolting, s.isBolting});
    char const* fullReason = nullptr;
    {
        std::lock_guard lock(syncQueueMutex);
        if (auto found = pendingFullSync.find(keyOf(player)); found != pendingFullSync.end()) {
            fullReason = found->second;
            pendingFullSync.erase(found);
        }
    }
    // SyncedEntityDataEvent.java:54-65: full self sync is independent of dirty
    // publication. StartTracking initialization is explicitly deferred to stage 8.
    if (fullReason) {
        auto full = gun::sync::full(runtimeId, snapshot);
        net::SyncedShooterDataPacket{full}.sendTo(player);
    }
    // Java tick END (SyncedEntityDataEvent.java:89-126). Deliberate phase
    // difference: our existing ServerLevelTickEvent runs BEFORE native subTick.
    gun::sync::publish(runtimeId, snapshot, s.syncBaseline,
        [&](auto const& dirty) { net::SyncedShooterDataPacket{dirty}.sendTo(player); },
        [&](auto const& dirty) { net::SyncedShooterDataPacket{dirty}.sendTo(static_cast<Actor const&>(player), player); });
}

void tick(Level& level) {
    std::lock_guard lock(mutex);
    // Establish timing even for players who have not shot/reloaded yet.
    level.forEachPlayer([&](Player& player) {
        observeSelection(player, stateOf(player));
        return true;
    });
    for (auto& [id, s] : states) {
        Actor* actor = level.fetchEntity(ActorUniqueID{id}, false);
        if (!actor || !actor->isPlayer()) {
            if (logic) logic->clearTasks(id);
            s.shootSprintIntent = {};
            { std::lock_guard sprintLock(sprintMutex); sprintBlockedOwners.erase(s.sprintOwner); }
            s.sprintOwner = nullptr;
            continue;
        }
        auto& player = *static_cast<Player*>(actor);
        tickAim(player, s);
        tickSprint(player, s); // after ADS/reload/held-trigger have corrected native sprint state
        if (!player.isAlive() || player.isDead()) gunLogic().clearTasks(id);
        // Accepted 0.4.11 divergence: once here, NOT Forge's START + END.
        // See SHOOT-0.4.11.md and the 20 TPS phase-contract regression test.
        else gunLogic().tickTasks(id);
        tickReload(player, s);
        tickBolt(player, s); // LivingEntityMixin:223-236, after reload/aim/sprint, before publication
        tickHeat(player, s); // dispatch even without heat data; default cooling deferred
    }
    // After every player's aim/sprint/burst/reload simulation, not from producers.
    level.forEachPlayer([&](Player& player) {
        publishShooterData(player, stateOf(player));
        return true;
    });
}

class AimHandler final : public ll::network::PacketHandlerBase<AimHandler, net::AimPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::AimPacket const& packet) const {
        auto* player = senderOf(netId, packet.mSenderSubId);
        if (!player) return;
        std::lock_guard lock(mutex);
        auto& s = stateOf(*player);
        observeSelection(*player, s);
        auto item = nbt::readGun(player->getSelectedItem());
        s.isAiming = packet.aiming && player->isAlive() && !player->isSpectator() && item && gunEntry(item->gunId);
        applySprintPolicy(*player, s); // an ADS release must not remove held-fire suppression
        logger().info("[ads-server] {} aiming={} progress={:.3f}", player->getRealName(), s.isAiming, s.aimingProgress);
    }
};

class ShootSprintHandler final : public ll::network::PacketHandlerBase<ShootSprintHandler, net::ShootSprintPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::ShootSprintPacket const& packet) const {
        auto* player = senderOf(netId, packet.mSenderSubId);
        if (!player) return;
        std::lock_guard lock(mutex);
        auto& s = stateOf(*player);
        observeSelection(*player, s); // authoritative actor/life/dimension/selected gun eligibility
        s.shootSprintIntent.update(packet.held);
        // Only stops the authenticated sender's sprint. No timer or shot state comes from the client.
        applySprintPolicy(*player, s);
    }
};

class ShootHandler final : public ll::network::PacketHandlerBase<ShootHandler, net::ShootPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::ShootPacket const& packet) const {
        auto* player = senderOf(netId, packet.mSenderSubId);
        if (!player) return;
        hitdiag::shootHandler(*player);
        if (char const* refused = handleShoot(*player, packet.timestamp, packet.inputTick)) {
            // Cooldown refusals are routine at high fire rates; everything else is worth a line.
            if (std::string_view(refused) != "COOL_DOWN") logger().info("[shoot] {} refused: {}", player->getRealName(), refused);
        }
    }
};

class DrawHandler final : public ll::network::PacketHandlerBase<DrawHandler, net::DrawPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::DrawPacket const& packet) const {
        auto player = senderOf(netId, packet.mSenderSubId);
        if (!player) return;
        std::lock_guard lock(mutex);
        observeSelection(*player, stateOf(*player));
    }
};

class FireSelectHandler final : public ll::network::PacketHandlerBase<FireSelectHandler, net::FireSelectPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::FireSelectPacket const& packet) const {
        auto* player = senderOf(netId, packet.mSenderSubId);
        if (!player) return;
        auto item = nbt::readGun(player->getSelectedItem());
        if (!item) return;
        auto const* entry = gunEntry(item->gunId);
        if (!entry) return;
        std::lock_guard lock(mutex);
        auto& s = stateOf(*player);
        observeSelection(*player, s);
        if (drawing(s, *entry) || gun::isReloading(s.reloadState)) return;
        item->fireMode = gun::nextFireMode(entry->data.fireModes, item->fireMode);
        nbt::writeSelectedGun(*player, *item);
        logger().info("[shoot] {} fire mode -> {}", player->getRealName(), nbt::fireModeName(item->fireMode));
    }
};

class ReloadHandler final : public ll::network::PacketHandlerBase<ReloadHandler, net::ReloadPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::ReloadPacket const& packet) const {
        auto* player = senderOf(netId, packet.mSenderSubId);
        if (!player) return;
        if (char const* refused = handleReload(*player)) {
            logger().info("[reload] {} refused: {}", player->getRealName(), refused);
            // ClientMessagePlayerReloadGun.java:21-30 sends no refusal reply.
        }
    }
};

class BoltHandler final : public ll::network::PacketHandlerBase<BoltHandler, net::BoltPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::BoltPacket const& packet) const {
        auto player = senderOf(netId, packet.mSenderSubId);
        if (!player) return;
        if (auto refused = handleBolt(*player)) logger().info("[bolt] {} refused: {}", player->getRealName(), refused);
    }
};

class CancelReloadHandler final : public ll::network::PacketHandlerBase<CancelReloadHandler, net::CancelReloadPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::CancelReloadPacket const& packet) const {
        if (auto* player = senderOf(netId, packet.mSenderSubId)) handleCancelReload(*player);
    }
};

} // namespace

void enable() {
    sprintHooked=ServerAimSprint::hook()==0;
    logger().info("[ads-sprint-server] native sprint-state hook={}",sprintHooked);
    auto& bus    = ll::event::EventBus::getInstance();
    listeners[0] = bus.emplaceListener<ll::event::ServerLevelTickEvent>([](ll::event::ServerLevelTickEvent& event) {
        tick(event.level());
    });
    listeners[1] = bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](ll::event::PlayerDisconnectEvent& event) {
        std::lock_guard lock(mutex);
        auto            key = keyOf(event.self());
        sprintPolicy(event.self(),false);
        {
            std::lock_guard sprintLock(sprintMutex);
            sprintLifeResets.erase(key);
            if (auto found = states.find(key); found != states.end()) sprintBlockedOwners.erase(found->second.sprintOwner);
        }
        states.erase(key);
        { std::lock_guard syncLock(syncQueueMutex); pendingFullSync.erase(key); }
        if (logic) logic->removeShooter(key);
    });
    listeners[2] = bus.emplaceListener<ll::event::ServerStoppingEvent>([](ll::event::ServerStoppingEvent&) {
        std::lock_guard lock(mutex);
        states.clear();
        { std::lock_guard syncLock(syncQueueMutex); pendingFullSync.clear(); }
        { std::lock_guard sprintLock(sprintMutex); sprintBlockedOwners.clear(); sprintLifeResets.clear(); }
        logic.reset(); // a fresh Lua state per world, as TACZ reloads scripts with the data packs
    });
    listeners[3] = bus.emplaceListener<ll::event::PlayerDieEvent>([](ll::event::PlayerDieEvent& event) {
        queueShootSprintReset(event.self());
    });
    listeners[5] = bus.emplaceListener<ll::event::PlayerJoinEvent>([](ll::event::PlayerJoinEvent& event) {
        queueFullSync(event.self(), "join");
    });
    listeners[4] = bus.emplaceListener<ll::event::PlayerRespawnEvent>([](ll::event::PlayerRespawnEvent& event) {
        queueShootSprintReset(event.self());
        queueFullSync(event.self(), "respawn");
    });
}

void disable() {
    if (sprintHooked) ServerAimSprint::unhook();
    sprintHooked=false;
    { std::lock_guard sprintLock(sprintMutex); sprintBlockedOwners.clear(); sprintLifeResets.clear(); }
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) {
        if (listener) bus.removeListener(listener);
        listener = nullptr;
    }
    std::lock_guard lock(mutex);
    // A hot-disable must not leave our transient modifier on living players. Empty/menu
    // state never asks for a level; a stopping world destroys its nonserialized modifiers.
    if (!states.empty()) {
        if (auto level = ll::service::getLevel()) level->forEachPlayer([](Player& player) {
            applyMovement(player, {});
            return true;
        });
    }
    states.clear();
    { std::lock_guard syncLock(syncQueueMutex); pendingFullSync.clear(); }
    logic.reset();
}

} // namespace tacz::server::shooter

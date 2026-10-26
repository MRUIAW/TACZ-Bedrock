#include "server/Shooter.h"

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
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/Bedrock.h"

#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeModifier.h"
#include "mc/world/attribute/MutableAttributeWithContext.h"
#include "mc/world/attribute/SharedAttributes.h"

#include "gun/Reload.h"
#include "gun/DrawRules.h"
#include "gun/ShootRules.h"
#include "model/Aiming.h"
#include "lua/GunLogic.h"
#include "mod/TaczMod.h"
#include "nbt/AmmoItem.h"
#include "nbt/GunItem.h"
#include "net/GunPackets.h"
#include "server/BulletSystem.h"

namespace tacz::server::shooter {
namespace {

using Clock = std::chrono::steady_clock;

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

int64_t serverNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// ShooterDataHolder, reduced to what is implemented.
struct ShooterState {
    unsigned recoilLookSamples = 0;
    bool isAiming = false;
    float aimingProgress = 0;
    int64_t aimingTimestamp = -1;
    gun::HandSelection selection;
    gun::DrawCooldown draw;
    std::optional<int64_t> clockOffset; // server ms - client ms, set on the first trigger pull
    std::optional<int64_t> lastShot;    // client timestamp of the last accepted trigger pull
    int64_t                lastShotServerMs = -1;

    // Rounds of the current trigger pull still to fire (bursts).
    int                           remaining = 0;
    Clock::time_point             nextRound;
    std::chrono::milliseconds     period{1};
    pack::ResourceId              gunId;
    std::optional<pack::FireMode> mode;
    int                           slot = -1;

    // Reload (LivingEntityReload / ShooterDataHolder.reloadTimestamp / reloadStateType).
    gun::ReloadStateType reloadState      = gun::ReloadStateType::NotReloading;
    int64_t              reloadStartedMs  = -1;
    int                  reloadSlot       = -1;
    pack::ResourceId     reloadGunId;
};

std::mutex                                 mutex;
std::unordered_map<int64_t, ShooterState> states; // by ActorUniqueID
std::unique_ptr<lua::GunLogic>             logic;
// Comparison-only SERVER actor identities. Separate lock: native callbacks may
// run from inside shooter code already holding mutex. Never dereference an owner.
std::mutex sprintMutex;
std::unordered_set<Mob const*> sprintBlockedOwners;
bool sprintHooked=false;
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

std::array<ll::event::ListenerPtr, 3> listeners;

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

// The script API view of the player's held gun.
lua::ScriptApi scriptApi(Player& player, nbt::GunItemState const& item, pack::GunEntry const& entry, ShooterState const& s) {
    lua::ScriptApi api;
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
    auto ammoId        = entry.data.ammoId;
    api.extractAmmo    = [&player, ammoId](int n) { return ammoId ? nbt::extractAmmo(player, *ammoId, n) : 0; };
    api.inventoryHasAmmo = [&player, ammoId] { return ammoId && nbt::countAmmo(player, *ammoId) > 0; };
    return api;
}

void sendReloadState(Player& player, gun::ReloadState state) {
    net::ReloadStatePacket{static_cast<int>(state.type), state.countDown}.sendTo(player);
}

void stopReload(Player& player, ShooterState& s, char const* why);

// Called from tick and every action, before using a previous gun's state. Slot is part of
// identity so switching between two stacks of the same GunId still starts a fresh draw.
void observeSelection(Player& player, ShooterState& s) {
    auto item = nbt::readGun(player.getSelectedItem());
    gun::HandSelection next{item ? std::optional{item->gunId} : std::nullopt, player.getSelectedItemSlot()};
    if (next == s.selection) return;
    stopReload(player, s, "weapon switched");
    s.remaining = 0;
    s.lastShot.reset();
    s.lastShotServerMs = -1;
    s.isAiming = false;
    s.aimingProgress = 0;
    s.aimingTimestamp = serverNowMs();
    auto entry = item ? gunEntry(item->gunId) : nullptr;
    s.draw.select(serverNowMs(), entry ? gun::animationMillis(entry->data.putAwayTime) : 0);
    s.selection = next;
}

bool drawing(ShooterState const& s, pack::GunEntry const& entry) {
    return s.draw.remaining(serverNowMs(), gun::animationMillis(entry.data.drawTime)) > 0;
}

// One round of the trigger pull: ammo, then the bullets. False ends the pull (TACZ's cycle
// task returns false when the shooter died, switched weapon or ran dry).
bool fireRound(Player& player, ShooterState& s) {
    if (!player.isAlive() || player.isDead() || player.getSelectedItemSlot() != s.slot) return false;
    auto item = nbt::readGun(player.getSelectedItem());
    if (!item || item->gunId != s.gunId) return false;
    auto const* entry = gunEntry(item->gunId);
    if (!entry) return false;
    auto const& data = entry->data;

    gun::GunAmmo ammo{item->ammo, item->barrel};
    if (!gun::reduceAmmoOnce(*data.bolt, ammo)) return false;
    item->ammo   = ammo.magazine;
    item->barrel = ammo.barrel;
    nbt::writeSelectedGun(player, *item);

    auto  delta      = player.getPosDelta();
    auto  type       = gun::inaccuracyType(model::fullyAimed(s.aimingProgress), player.isSneaking(), glm::length(glm::dvec3{delta.x, delta.y, delta.z}));
    float inaccuracy = gun::inaccuracy(data, s.mode, type);
    bullet::fire(player,item->gunId, data, s.mode, inaccuracy);
    return true;
}

// LivingEntityShoot.shoot. Returns a reason when the trigger pull is refused.
char const* handleShoot(Player& player, int64_t timestamp) {
    auto item = nbt::readGun(player.getSelectedItem());
    if (!item) return "NOT_GUN";
    auto const* entry = gunEntry(item->gunId);
    if (!entry) return "ID_NOT_EXIST";
    auto const& data = entry->data;

    std::lock_guard lock(mutex);
    auto&           s = stateOf(player);
    observeSelection(player, s);
    if (drawing(s, *entry)) return "IS_DRAWING";
    if (s.remaining > 0) return "COOL_DOWN";
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

    s.lastShot         = timestamp;
    s.lastShotServerMs = now;
    s.gunId            = item->gunId;
    s.mode             = item->fireMode;
    s.slot             = player.getSelectedItemSlot();
    int cycles         = s.mode == pack::FireMode::Burst ? std::max(data.burst.count, 1) : 1;
    s.period           = std::chrono::milliseconds(s.mode == pack::FireMode::Burst ? gun::burstShootIntervalMs(data) : 1);
    s.remaining        = cycles;
    // CycleTaskHelper runs the first round right away; the rest follow on server ticks.
    if (fireRound(player, s)) {
        // Bounded observation only: actual authoritative look used for this shot, not a
        // client recoil/rotation override. Compare triggerTs with [recoil-shot] in tests.
        if (s.recoilLookSamples++ < 8) {
            auto const& rotation = player.getRotation();
            logger().info("[recoil-server-look] {} triggerTs={} rotation=({:.6f},{:.6f}) aim={:.3f} rotationSource=server-player (observe; compare client traces, no forced server recoil)",
                player.getRealName(), timestamp, rotation.x, rotation.z, s.aimingProgress);
        }
        --s.remaining;
        s.nextRound = Clock::now() + s.period;
    } else {
        s.remaining = 0;
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
    // getShootCoolDown() != 0 at the server's own clock.
    if (s.lastShotServerMs >= 0 && gun::triggerIntervalMs(data, item->fireMode) - (serverNowMs() - s.lastShotServerMs) - 5 > 0) {
        return "COOL_DOWN";
    }
    if (s.remaining > 0) return "COOL_DOWN";
    if (!canReload(player, *item, *entry)) return "CANNOT_RELOAD";

    gun::GunAmmo ammo{item->ammo, item->barrel};
    bool         empty = gun::availableRounds(*data.bolt, ammo) <= 0;
    s.reloadState      = empty ? gun::ReloadStateType::EmptyFeeding : gun::ReloadStateType::TacticalFeeding;
    s.reloadStartedMs  = serverNowMs();
    s.reloadSlot       = player.getSelectedItemSlot();
    s.reloadGunId      = item->gunId;

    auto api = scriptApi(player, *item, *entry, s);
    if (!gunLogic().startReload(api)) {
        s.reloadState     = gun::ReloadStateType::NotReloading;
        s.reloadStartedMs = -1;
        return "SCRIPT_REFUSED";
    }
    logger().info(
        "[reload] {} {} reload of {} ({}{} rounds)",
        player.getRealName(),
        empty ? "empty" : "tactical",
        item->gunId.toString(),
        item->ammo,
        gun::hasChamberedRound(data.bolt,item->barrel) ? "+1" : ""
    );
    sendReloadState(player, {s.reloadState, 0});
    return nullptr;
}

// ShooterDataHolder.initialData for the reload part, after a weapon switch or cancel.
void stopReload(Player& player, ShooterState& s, char const* why) {
    if (!gun::isReloading(s.reloadState)) return;
    s.reloadState     = gun::ReloadStateType::NotReloading;
    s.reloadStartedMs = -1;
    gunLogic().clearScriptData(keyOf(player));
    logger().info("[reload] {} reload stopped: {}", player.getRealName(), why);
    sendReloadState(player, {});
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
}

// LivingEntityReload.tickReloadState, once per server tick.
void tickReload(Player& player, ShooterState& s) {
    if (!gun::isReloading(s.reloadState)) return;
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
    if (api.ammo != gun::GunAmmo{item->ammo, item->barrel}) {
        item->ammo   = api.ammo.magazine;
        item->barrel = api.ammo.barrel;
        nbt::writeSelectedGun(player, *item);
        logger().info("[reload] {} loaded: {}{} rounds", player.getRealName(), item->ammo, item->barrel ? "+1" : "");
    }
    if (next.type != s.reloadState) sendReloadState(player, next);
    s.reloadState = next.type;
    if (!gun::isReloading(next.type)) {
        s.reloadStartedMs = -1;
        logger().info("[reload] {} reload finished", player.getRealName());
    }
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
        sprintPolicy(player,false);
        s.isAiming = false;
        s.aimingProgress = 0;
        applyMovement(player, {});
    } else {
        s.aimingProgress = model::advanceAimProgress(s.aimingProgress, s.isAiming,
            s.aimingTimestamp < 0 ? 50 : std::max<int64_t>(0, now - s.aimingTimestamp),
            model::effectiveAdsTime(entry->data.aimTime)); // ads cache base until attachments are implemented
        bool blocked=s.isAiming || (gun::isReloading(s.reloadState) && !gun::isReloadFinishing(s.reloadState));
        sprintPolicy(player,blocked);
        if (blocked) player.setSprinting(false);
        applyMovement(player, model::movementModifier(entry->data.movementSpeed, s.isAiming, gun::isReloading(s.reloadState)));
    }
    s.aimingTimestamp = now;
}

void tick(Level& level) {
    std::lock_guard lock(mutex);
    // Establish timing even for players who have not shot/reloaded yet.
    level.forEachPlayer([&](Player& player) {
        observeSelection(player, stateOf(player));
        return true;
    });
    auto            now = Clock::now();
    for (auto& [id, s] : states) {
        Actor* actor = level.fetchEntity(ActorUniqueID{id}, false);
        if (!actor || !actor->isPlayer()) {
            s.remaining = 0;
            continue;
        }
        auto& player = *static_cast<Player*>(actor);
        tickAim(player, s);
        while (s.remaining > 0 && now >= s.nextRound) {
            if (!fireRound(player, s)) {
                s.remaining = 0;
                break;
            }
            --s.remaining;
            s.nextRound += s.period;
        }
        tickReload(player, s);
    }
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
        sprintPolicy(*player,s.isAiming || (gun::isReloading(s.reloadState) && !gun::isReloadFinishing(s.reloadState)));
        if (s.isAiming) player->setSprinting(false);
        logger().info("[ads-server] {} aiming={} progress={:.3f}", player->getRealName(), s.isAiming, s.aimingProgress);
    }
};

class ShootHandler final : public ll::network::PacketHandlerBase<ShootHandler, net::ShootPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::ShootPacket const& packet) const {
        auto* player = senderOf(netId, packet.mSenderSubId);
        if (!player) return;
        if (char const* refused = handleShoot(*player, packet.timestamp)) {
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
            // The client already played its reload sound; tell it the reload is not happening.
            sendReloadState(*player, {});
        }
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
        states.erase(key);
        if (logic) logic->clearScriptData(key);
    });
    listeners[2] = bus.emplaceListener<ll::event::ServerStoppingEvent>([](ll::event::ServerStoppingEvent&) {
        std::lock_guard lock(mutex);
        states.clear();
        { std::lock_guard sprintLock(sprintMutex); sprintBlockedOwners.clear(); }
        logic.reset(); // a fresh Lua state per world, as TACZ reloads scripts with the data packs
    });
}

void disable() {
    if (sprintHooked) ServerAimSprint::unhook();
    sprintHooked=false;
    { std::lock_guard sprintLock(sprintMutex); sprintBlockedOwners.clear(); }
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
    logic.reset();
}

} // namespace tacz::server::shooter

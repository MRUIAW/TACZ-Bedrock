#include "server/Shooter.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/Bedrock.h"

#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "gun/ShootRules.h"
#include "mod/TaczMod.h"
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
    std::optional<int64_t> clockOffset; // server ms - client ms, set on the first trigger pull
    std::optional<int64_t> lastShot;    // client timestamp of the last accepted trigger pull

    // Rounds of the current trigger pull still to fire (bursts).
    int                           remaining = 0;
    Clock::time_point             nextRound;
    std::chrono::milliseconds     period{1};
    pack::ResourceId              gunId;
    std::optional<pack::FireMode> mode;
    int                           slot = -1;
};

std::mutex                                 mutex;
std::unordered_map<int64_t, ShooterState> states; // by ActorUniqueID

std::array<ll::event::ListenerPtr, 3> listeners;

ShooterState& stateOf(Player const& player) { return states[player.getOrCreateUniqueID().rawID]; }

Player* senderOf(NetworkIdentifier const& netId, SubClientId subId) {
    auto handler = ll::service::getServerNetworkHandler();
    return handler ? handler->_getServerPlayer(netId, subId) : nullptr;
}

pack::GunEntry const* gunEntry(pack::ResourceId const& id) { return TaczMod::getInstance().gunPacks().gun(id); }

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

    // Aiming is not implemented yet, so the aim inaccuracy never applies.
    auto  type        = gun::inaccuracyType(false, player.isSneaking(), glm::length(glm::dvec3{
                                                                   player.getPosDelta().x,
                                                                   player.getPosDelta().y,
                                                                   player.getPosDelta().z,
                                                               }));
    float inaccuracy  = gun::inaccuracy(data, s.mode, type);
    bullet::fire(player, data, s.mode, inaccuracy);
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

    s.lastShot  = timestamp;
    s.gunId     = item->gunId;
    s.mode      = item->fireMode;
    s.slot      = player.getSelectedItemSlot();
    int cycles  = s.mode == pack::FireMode::Burst ? std::max(data.burst.count, 1) : 1;
    s.period    = std::chrono::milliseconds(s.mode == pack::FireMode::Burst ? gun::burstShootIntervalMs(data) : 1);
    s.remaining = cycles;
    // CycleTaskHelper runs the first round right away; the rest follow on server ticks.
    if (fireRound(player, s)) {
        --s.remaining;
        s.nextRound = Clock::now() + s.period;
    } else {
        s.remaining = 0;
    }
    return nullptr;
}

void tickBursts(Level& level) {
    std::lock_guard lock(mutex);
    auto            now = Clock::now();
    for (auto& [id, s] : states) {
        if (s.remaining <= 0) continue;
        Actor* actor = level.fetchEntity(ActorUniqueID{id}, false);
        if (!actor || !actor->isPlayer()) {
            s.remaining = 0;
            continue;
        }
        auto& player = *static_cast<Player*>(actor);
        while (s.remaining > 0 && now >= s.nextRound) {
            if (!fireRound(player, s)) {
                s.remaining = 0;
                break;
            }
            --s.remaining;
            s.nextRound += s.period;
        }
    }
}

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

class FireSelectHandler final : public ll::network::PacketHandlerBase<FireSelectHandler, net::FireSelectPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::FireSelectPacket const& packet) const {
        auto* player = senderOf(netId, packet.mSenderSubId);
        if (!player) return;
        auto item = nbt::readGun(player->getSelectedItem());
        if (!item) return;
        auto const* entry = gunEntry(item->gunId);
        if (!entry) return;
        item->fireMode = gun::nextFireMode(entry->data.fireModes, item->fireMode);
        nbt::writeSelectedGun(*player, *item);
        logger().info("[shoot] {} fire mode -> {}", player->getRealName(), nbt::fireModeName(item->fireMode));
    }
};

} // namespace

void enable() {
    auto& bus    = ll::event::EventBus::getInstance();
    listeners[0] = bus.emplaceListener<ll::event::ServerLevelTickEvent>([](ll::event::ServerLevelTickEvent& event) {
        tickBursts(event.level());
    });
    listeners[1] = bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](ll::event::PlayerDisconnectEvent& event) {
        std::lock_guard lock(mutex);
        states.erase(event.self().getOrCreateUniqueID().rawID);
    });
    listeners[2] = bus.emplaceListener<ll::event::ServerStoppingEvent>([](ll::event::ServerStoppingEvent&) {
        std::lock_guard lock(mutex);
        states.clear();
    });
}

void disable() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) {
        if (listener) bus.removeListener(listener);
        listener = nullptr;
    }
    std::lock_guard lock(mutex);
    states.clear();
}

} // namespace tacz::server::shooter

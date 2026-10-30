#include "server/HitInput.h"
#include "server/HitDiagnostic.h"

#include <array>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerRespawnEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/io/Logger.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/entity/components/UserEntityIdentifierComponent.h"
#include "mc/network/packet/PlayerAuthInputPacket.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mod/TaczMod.h"

namespace tacz::server::hitinput {
namespace {
// Independent of Shooter.cpp's mutex. Never call native functions/origin or
// shooter code while holding this lock. Shooter -> input is the only nested order.
std::mutex mutex;
std::unordered_map<int64_t, gun::AuthInputRing> records;
std::array<ll::event::ListenerPtr, 5> listeners;
bool receiving = false, dimensionHooked = false, teleportHooked = false;

void erase(Player const& player) {
    auto key = player.getOrCreateUniqueID().rawID;
    std::lock_guard lock(mutex);
    records.erase(key);
}
void clear() { std::lock_guard lock(mutex); records.clear(); }

using ReceiveSignature = void (ServerNetworkHandler::*)(NetworkIdentifier const&, PlayerAuthInputPacket const&);
static_assert(std::is_same_v<decltype(static_cast<ReceiveSignature>(&ServerNetworkHandler::$handle)), ReceiveSignature>);
LL_TYPE_INSTANCE_HOOK(ReceivedAuthInput, ll::memory::HookPriority::Normal, ServerNetworkHandler,
    static_cast<ReceiveSignature>(&ServerNetworkHandler::$handle), void, NetworkIdentifier const& source, PlayerAuthInputPacket const& packet) {
    // Packet dispatch, before native handle queues movement. Do not take the
    // shooter lock here; origin can publish lifecycle callbacks synchronously.
    // This override receives the secondary NetEventCallback subobject as this;
    // adjust to ServerNetworkHandler before using its vtable (as LL does).
    auto player = thisFor<NetEventCallback>()->_getServerPlayer(source, packet.mSenderSubId);
    if (player && ll::service::getLevel() && player->isAlive() && !player->isDead()) {
        auto const& pos = *packet.mPos;
        auto const& rot = *packet.mRot;
        gun::AuthInput value{{pos.x, pos.y, pos.z}, {rot.x, rot.z}, packet.mYHeadRot,
            packet.mClientTick->mValue, player->getLevel().getCurrentTick().tickID};
        auto level = &player->getLevel();
        int dimension = player->getDimensionId();
        auto key = player->getOrCreateUniqueID().rawID;
        bool handledTeleport = packet.mInputData->contains(PlayerAuthInputPacketPayload::InputData::HandledTeleport);
        std::optional<gun::TeleportEvent> transition;
        {
            std::lock_guard lock(mutex);
            auto& record = records[key];
            record.bind(reinterpret_cast<uintptr_t>(level), reinterpret_cast<uintptr_t>(player), dimension);
            transition = record.receive(value, handledTeleport); // receipt order, not clientTick sorting
        }
        if (transition) hitdiag::teleportEvent(level, key, *transition);
        hitdiag::receivedInput(*player); // no input/shooter lock held
    }
    origin(source, packet);
}
using DimensionSignature = void (ServerPlayer::*)(DimensionType);
static_assert(std::is_same_v<decltype(&ServerPlayer::$changeDimension), DimensionSignature>);
LL_TYPE_INSTANCE_HOOK(AuthInputDimension, ll::memory::HookPriority::Normal, ServerPlayer,
    &ServerPlayer::$changeDimension, void, DimensionType dimension) {
    erase(*this);
    origin(dimension);
}

using TeleportSignature = void (Player::*)(Vec3 const&, bool, int, int, bool);
static_assert(std::is_same_v<decltype(&Player::$teleportTo), TeleportSignature>);
LL_TYPE_INSTANCE_HOOK(AuthInputTeleport, ll::memory::HookPriority::Normal, Player,
    &Player::$teleportTo, void, Vec3 const& pos, bool stopRiding, int cause, int sourceType, bool keepVelocity) {
    glm::dvec3 target{pos.x, pos.y, pos.z}; // own the argument before origin can alter it
    origin(pos, stopRiding, cause, sourceType, keepVelocity);
    auto levelRef = ll::service::getLevel();
    if (!levelRef) return;
    auto level = &*levelRef;
    if (isClientSide() || &getLevel() != level) return;
    // Only an actual server-network ServerPlayer counts, never a client Player.
    // teleportTo overrides the primary Actor base: no secondary-this adjustment.
    auto handler = ll::service::getServerNetworkHandler();
    auto identity = getEntityContext().tryGetComponent<UserEntityIdentifierComponent>();
    if (!handler || !identity || static_cast<Player*>(handler->_getServerPlayer(identity->mNetworkId, identity->mClientSubId)) != this) return;
    auto key = getOrCreateUniqueID().rawID;
    auto tick = level->getCurrentTick().tickID;
    int dimension = getDimensionId();
    gun::TeleportEvent transition;
    {
        std::lock_guard lock(mutex);
        auto& record = records[key];
        record.bind(reinterpret_cast<uintptr_t>(level), reinterpret_cast<uintptr_t>(this), dimension);
        transition = record.markTeleport(tick, target);
    }
    hitdiag::teleportEvent(level, key, transition); // input lock released; no shooter mutex acquired
}

void expireTeleports(Level& level) {
    auto tick = level.getCurrentTick().tickID;
    std::vector<std::pair<int64_t, gun::TeleportEvent>> expired;
    {
        std::lock_guard lock(mutex);
        for (auto& [key, record] : records) if (record.boundToWorld(reinterpret_cast<uintptr_t>(&level))) {
            if (auto event = record.expireTeleport(tick)) expired.emplace_back(key, *event);
        }
    }
    for (auto const& [key, event] : expired) hitdiag::teleportEvent(&level, key, event);
}
}

gun::AuthInputRing snapshot(Player& player) {
    if (!ll::service::getLevel() || player.isClientSide()) return {};
    auto level = &player.getLevel();
    int dimension = player.getDimensionId();
    auto key = player.getOrCreateUniqueID().rawID;
    auto tick = level->getCurrentTick().tickID;
    gun::AuthInputRing result;
    std::optional<gun::TeleportEvent> transition;
    {
        std::lock_guard lock(mutex);
        auto found = records.find(key);
        if (found == records.end()) return {};
        found->second.bind(reinterpret_cast<uintptr_t>(level), reinterpret_cast<uintptr_t>(&player), dimension);
        transition = found->second.expireTeleport(tick);
        result = found->second; // owned copy; pending records have no usable samples
    }
    if (transition) hitdiag::teleportEvent(level, key, *transition);
    return result;
}

void enable() {
    hitdiag::enableThreadEvidence();
    receiving = ReceivedAuthInput::hook() == 0;
    dimensionHooked = AuthInputDimension::hook() == 0;
    teleportHooked = AuthInputTeleport::hook() == 0;
    TaczMod::getInstance().getSelf().getLogger().info("[hit-input] receive/dimension hooks={}/{}", receiving, dimensionHooked);
    TaczMod::getInstance().getSelf().getLogger().info("[hit-input] teleport hook={} ackTimeoutTicks={}", teleportHooked, gun::TeleportAckTimeoutTicks);
    auto& bus = ll::event::EventBus::getInstance();
    listeners[0] = bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](auto& event) { erase(event.self()); });
    listeners[1] = bus.emplaceListener<ll::event::PlayerDieEvent>([](auto& event) { erase(event.self()); });
    listeners[2] = bus.emplaceListener<ll::event::PlayerRespawnEvent>([](auto& event) { erase(event.self()); });
    listeners[3] = bus.emplaceListener<ll::event::ServerStoppingEvent>([](auto&) { clear(); });
    listeners[4] = bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto& event) { expireTeleports(event.level()); });
}
void disable() {
    if (receiving) ReceivedAuthInput::unhook();
    if (dimensionHooked) AuthInputDimension::unhook();
    if (teleportHooked) AuthInputTeleport::unhook();
    receiving = dimensionHooked = teleportHooked = false;
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) { if (listener) bus.removeListener(listener); listener = nullptr; }
    clear();
}
} // namespace tacz::server::hitinput

#include "server/HitInput.h"
#include "server/HitDiagnostic.h"

#include <array>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerRespawnEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/io/Logger.h"
#include "mc/network/ServerNetworkHandler.h"
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
std::array<ll::event::ListenerPtr, 4> listeners;
bool receiving = false, dimensionHooked = false;

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
    auto player = _getServerPlayer(source, packet.mSenderSubId);
    if (player && ll::service::getLevel() && player->isAlive() && !player->isDead()) {
        auto const& pos = *packet.mPos;
        auto const& rot = *packet.mRot;
        gun::AuthInput value{{pos.x, pos.y, pos.z}, {rot.x, rot.z}, packet.mYHeadRot,
            packet.mClientTick->mValue, player->getLevel().getCurrentTick().tickID};
        auto level = &player->getLevel();
        int dimension = player->getDimensionId();
        auto key = player->getOrCreateUniqueID().rawID;
        {
            std::lock_guard lock(mutex);
            auto& record = records[key];
            record.bind(reinterpret_cast<uintptr_t>(level), reinterpret_cast<uintptr_t>(player), dimension);
            record.receive(value); // receipt order, not clientTick sorting
        }
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
}

gun::AuthInputRing snapshot(Player& player) {
    auto level = &player.getLevel();
    int dimension = player.getDimensionId();
    auto key = player.getOrCreateUniqueID().rawID;
    std::lock_guard lock(mutex);
    auto found = records.find(key);
    if (found == records.end()) return {};
    found->second.bind(reinterpret_cast<uintptr_t>(level), reinterpret_cast<uintptr_t>(&player), dimension);
    return found->second; // owned copy, no native packet/player reference escapes
}

void enable() {
    hitdiag::enableThreadEvidence();
    receiving = ReceivedAuthInput::hook() == 0;
    dimensionHooked = AuthInputDimension::hook() == 0;
    TaczMod::getInstance().getSelf().getLogger().info("[hit-input] receive/dimension hooks={}/{}", receiving, dimensionHooked);
    auto& bus = ll::event::EventBus::getInstance();
    listeners[0] = bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](auto& event) { erase(event.self()); });
    listeners[1] = bus.emplaceListener<ll::event::PlayerDieEvent>([](auto& event) { erase(event.self()); });
    listeners[2] = bus.emplaceListener<ll::event::PlayerRespawnEvent>([](auto& event) { erase(event.self()); });
    listeners[3] = bus.emplaceListener<ll::event::ServerStoppingEvent>([](auto&) { clear(); });
}
void disable() {
    if (receiving) ReceivedAuthInput::unhook();
    if (dimensionHooked) AuthInputDimension::unhook();
    receiving = dimensionHooked = false;
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) { if (listener) bus.removeListener(listener); listener = nullptr; }
    clear();
}
} // namespace tacz::server::hitinput

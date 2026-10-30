#include "client/HitInput.h"

#include <mutex>
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/network/LoopbackPacketSender.h"
#include "mc/network/packet/PlayerAuthInputPacket.h"
#include "client/gameplay/ClientShooter.h"

namespace tacz::client::hitinput {
namespace {
std::mutex mutex;
uint64_t world = 0, tick = 0;
}
void sent(LoopbackPacketSender& sender, Packet const& packet) {
    auto token = shooter::worldToken();
    if (!token || packet.getId() != MinecraftPacketIds::PlayerAuthInputPacket) return;
    auto client = ll::service::getClientInstance();
    if (!client || !shooter::ownsClient(&*client) || &client->getPacketSender() != static_cast<PacketSender*>(&sender)
        || client->getClientSubId() != packet.mSenderSubId) return;
    auto const& auth = static_cast<PlayerAuthInputPacket const&>(packet);
    std::lock_guard lock(mutex);
    world = token; tick = auth.mClientTick->mValue;
}
uint64_t lastSentTick() {
    std::lock_guard lock(mutex);
    return world && world == shooter::worldToken() ? tick : 0;
}
void clear() { std::lock_guard lock(mutex); world = tick = 0; }
}

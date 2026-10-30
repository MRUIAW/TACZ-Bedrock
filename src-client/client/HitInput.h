#pragma once
#include <cstdint>
class LoopbackPacketSender;
class Packet;
namespace tacz::client::hitinput {
void sent(LoopbackPacketSender& sender, Packet const& packet);
uint64_t lastSentTick();
void clear();
}

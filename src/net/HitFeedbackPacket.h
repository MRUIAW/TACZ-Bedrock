#pragma once
#include <string>
#include "ll/api/network/packet/Packet.h"
#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"

namespace tacz::net {
// Server -> shooter only. Shot identity is retained on the bullet, never inferred
// from whichever gun the client happens to hold when an in-flight bullet hits.
class HitFeedbackPacket final : public ll::network::PacketBase<HitFeedbackPacket> {
public:
    std::string gunId;
    bool headshot=false,kill=false;
    HitFeedbackPacket()=default;
    HitFeedbackPacket(std::string gun,bool head,bool killed):gunId(std::move(gun)),headshot(head),kill(killed) {}
    void write(BinaryStream& s) const override {
        s.writeString(gunId,nullptr,nullptr); s.writeBool(headshot,nullptr,nullptr); s.writeBool(kill,nullptr,nullptr);
    }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& s) override {
        auto gun=s.getString(256); if (!gun) return nonstd::make_unexpected(gun.error());
        auto head=s.getBool(); if (!head) return nonstd::make_unexpected(head.error());
        auto killed=s.getBool(); if (!killed) return nonstd::make_unexpected(killed.error());
        gunId=std::move(*gun); headshot=*head; kill=*killed; return {};
    }
};
}

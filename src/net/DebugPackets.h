#pragma once

#include <string>

#include "ll/api/network/packet/Packet.h"

#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"

// Technical-validation packets. C2S and S2C use distinct types because in a
// client-hosted world both sides live in one process and share one handler table.
namespace tacz::net {

// Server -> issuing player: /tacz anim. Separate from the technical probe acknowledgement.
class AnimationDebugPacket final : public ll::network::PacketBase<AnimationDebugPacket> {
public:
    std::string gunId;
    std::string name;

    AnimationDebugPacket() = default;
    AnimationDebugPacket(std::string gun, std::string animation) : gunId(std::move(gun)), name(std::move(animation)) {}

    void write(BinaryStream& stream) const override {
        stream.writeString(gunId, nullptr, nullptr);
        stream.writeString(name, nullptr, nullptr);
    }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        auto gun = stream.getString(256);
        if (!gun) return nonstd::make_unexpected(gun.error());
        auto animation = stream.getString(256);
        if (!animation) return nonstd::make_unexpected(animation.error());
        gunId = std::move(*gun);
        name = std::move(*animation);
        return {};
    }
};

// Client -> server: a player action ("ping", "fire", "aim", "reload").
class DebugActionPacket final : public ll::network::PacketBase<DebugActionPacket> {
public:
    unsigned int seq{};
    std::string  action;

    DebugActionPacket() = default;
    DebugActionPacket(unsigned int s, std::string a) : seq(s), action(std::move(a)) {}

    void write(BinaryStream& stream) const override {
        stream.writeUnsignedVarInt(seq, nullptr, nullptr);
        stream.writeString(action, nullptr, nullptr);
    }

    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        auto s = stream.getUnsignedVarInt();
        if (!s) return nonstd::make_unexpected(s.error());
        auto a = stream.getString(256);
        if (!a) return nonstd::make_unexpected(a.error());
        seq    = *s;
        action = std::move(*a);
        return {};
    }
};

// Server -> client: acknowledgement with whatever the server saw.
class DebugAckPacket final : public ll::network::PacketBase<DebugAckPacket> {
public:
    unsigned int seq{};
    std::string  info;

    DebugAckPacket() = default;
    DebugAckPacket(unsigned int s, std::string i) : seq(s), info(std::move(i)) {}

    void write(BinaryStream& stream) const override {
        stream.writeUnsignedVarInt(seq, nullptr, nullptr);
        stream.writeString(info, nullptr, nullptr);
    }

    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        auto s = stream.getUnsignedVarInt();
        if (!s) return nonstd::make_unexpected(s.error());
        auto i = stream.getString(1024);
        if (!i) return nonstd::make_unexpected(i.error());
        seq  = *s;
        info = std::move(*i);
        return {};
    }
};

} // namespace tacz::net

#pragma once
#include "gun/BulletExperiment.h"
#include "ll/api/network/packet/Packet.h"
#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"

namespace tacz::net {
class BulletPacket final : public ll::network::PacketBase<BulletPacket> {
public:
    std::optional<gun::bullet::Message> message;
    BulletPacket() = default;
    explicit BulletPacket(gun::bullet::Message const& value) : message(value) {}
    void write(BinaryStream& stream) const override {
        if (message) { auto bytes = gun::bullet::encode(*message); stream.write(bytes.data(), bytes.size()); }
    }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        message.reset();
        if (!stream.mHasOverflowed && stream.mReadPointer <= stream.mView.size())
            message = gun::bullet::decode(stream.mView.substr(static_cast<size_t>(stream.mReadPointer)));
        stream.mReadPointer = stream.mView.size(); return {};
    }
};
class BulletPlacementPacket final : public ll::network::PacketBase<BulletPlacementPacket> {
public:
    std::optional<gun::bullet::Placement> placement;
    BulletPlacementPacket() = default;
    explicit BulletPlacementPacket(gun::bullet::Placement const& value) : placement(value) {}
    void write(BinaryStream& stream) const override {
        if (placement) { auto bytes = gun::bullet::encodePlacement(*placement); stream.write(bytes.data(),bytes.size()); }
    }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        placement.reset();
        if (!stream.mHasOverflowed && stream.mReadPointer <= stream.mView.size())
            placement = gun::bullet::decodePlacement(stream.mView.substr(static_cast<size_t>(stream.mReadPointer)));
        stream.mReadPointer = stream.mView.size(); return {};
    }
};
} // namespace tacz::net
